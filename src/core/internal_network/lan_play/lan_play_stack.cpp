// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "common/settings.h"
#include "core/internal_network/lan_play/lan_play_stack.h"
#include "core/internal_network/lan_play/virtual_address_allocator.h"

namespace Network::LanPlay {

namespace {

std::mutex stack_mutex;
std::shared_ptr<Stack> active_stack;

/// The settings the active stack was created from, so re-saving unchanged settings does not rejoin.
std::string active_server;
std::string active_virtual_ip;

} // Anonymous namespace

Stack::Stack(std::unique_ptr<Client> client_, std::unique_ptr<NetworkInterface> network_interface_)
    : client{std::move(client_)}, network_interface{std::move(network_interface_)} {}

Stack::~Stack() {
    // The interface first: it stops its timer thread and tears down the sockets that sit on the
    // client, so nothing is still using the relay socket when the client goes away.
    network_interface.reset();
    client.reset();
}

std::shared_ptr<Stack> Stack::Create(const Configuration& configuration) {
    auto client = std::make_unique<Client>(configuration);

    if (!client->Open()) {
        return nullptr;
    }

    client->Start();

    const u32 address = VirtualAddress::Allocate(*client, configuration.virtual_address);

    auto network_interface = std::make_unique<NetworkInterface>(*client, address);

    network_interface->Announce();

    LOG_INFO(Network_LanPlay,
             "the emulated console is {} on the {} network. The host's own network configuration is "
             "untouched.",
             FormatAddress(address), configuration.Describe());

    LOG_INFO(Network_LanPlay,
             "online features are unaffected. Everything that is not addressed to the LAN Play "
             "network keeps using the host network, and the console only presents its LAN Play "
             "address once the game enters a local or LAN mode.");

    return std::shared_ptr<Stack>{new Stack{std::move(client), std::move(network_interface)}};
}

void Stack::MarkGuestActive(std::string_view reason) {
    if (guest_active.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    LOG_INFO(Network_LanPlay,
             "the game started using the LAN Play network ({}). The console now presents {} to it; "
             "its other traffic keeps using the host network.",
             reason, FormatAddress(GetAddress()));
}

bool Stack::Supports(Domain domain, Type type, Protocol protocol) {
    if (domain != Domain::INET) {
        return false;
    }

    // The transport is decided by the socket type: a guest often passes Protocol::Unspecified and
    // expects the default protocol for the type it asked for.
    switch (type) {
    case Type::DGRAM:
        return protocol == Protocol::UDP || protocol == Protocol::Unspecified ||
               protocol == Protocol::IP;
    case Type::STREAM:
        return protocol == Protocol::TCP || protocol == Protocol::Unspecified ||
               protocol == Protocol::IP;
    default:
        return false;
    }
}

void ApplySettings() {
    const bool enabled = Settings::values.lan_play_enabled.GetValue();
    const std::string server = Settings::values.lan_play_server.GetValue();
    const std::string virtual_ip = Settings::values.lan_play_virtual_ip.GetValue();

    std::scoped_lock lock{stack_mutex};

    if (!enabled) {
        if (active_stack) {
            LOG_INFO(Network_LanPlay,
                     "LAN Play was switched off; the console is back on the host network.");

            active_stack.reset();
            active_server.clear();
            active_virtual_ip.clear();
        }

        return;
    }

    // Re-saving the same settings must not tear down a live session under the running game.
    if (active_stack && server == active_server && virtual_ip == active_virtual_ip) {
        return;
    }

    if (active_stack) {
        LOG_INFO(Network_LanPlay, "the LAN Play settings changed; rejoining the relay.");

        active_stack.reset();
    }

    Configuration configuration;

    if (!Configuration::TryParse(server, virtual_ip, configuration)) {
        LOG_ERROR(Network_LanPlay,
                  "could not join the LAN Play relay: \"{}\" is not a usable server address.",
                  server);

        return;
    }

    active_stack = Stack::Create(configuration);

    if (!active_stack) {
        LOG_ERROR(Network_LanPlay, "could not join the LAN Play relay {}.",
                  configuration.Describe());

        return;
    }

    active_server = server;
    active_virtual_ip = virtual_ip;
}

std::shared_ptr<Stack> GetStack() {
    std::scoped_lock lock{stack_mutex};

    return active_stack;
}

void Shutdown() {
    std::scoped_lock lock{stack_mutex};

    active_stack.reset();
    active_server.clear();
    active_virtual_ip.clear();
}

} // namespace Network::LanPlay
