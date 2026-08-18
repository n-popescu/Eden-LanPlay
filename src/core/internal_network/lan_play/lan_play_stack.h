// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "common/common_types.h"
#include "common/socket_types.h"
#include "core/internal_network/lan_play/lan_play_client.h"
#include "core/internal_network/lan_play/lan_play_config.h"
#include "core/internal_network/lan_play/lan_play_network_interface.h"

namespace Network::LanPlay {

/**
 * The LAN Play networking stack of one emulation session: the relay client and the virtual network
 * interface built on top of it.
 */
class Stack {
public:
    ~Stack();

    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;

    /// Connects to the relay and picks the virtual address of the emulated console.
    static std::shared_ptr<Stack> Create(const Configuration& configuration);

    [[nodiscard]] Client& GetClient() const {
        return *client;
    }

    [[nodiscard]] NetworkInterface& GetNetworkInterface() const {
        return *network_interface;
    }

    [[nodiscard]] bool IsConnected() const {
        return client->IsRunning();
    }

    [[nodiscard]] u32 GetAddress() const {
        return network_interface->GetAddress();
    }

    /**
     * True once the emulated console has actually used the LAN Play network: it joined or hosted an
     * LDN session, or one of its sockets exchanged traffic on it (which is what entering a game's own
     * LAN mode does).
     *
     * Until then LAN Play stays completely out of the way — in particular the console keeps reporting
     * its host address — so that having LAN Play selected does not change anything for online play.
     */
    [[nodiscard]] bool IsGuestActive() const {
        return guest_active.load(std::memory_order_relaxed);
    }

    /// Called the first time the guest uses the LAN Play network.
    void MarkGuestActive(std::string_view reason);

    /// True for the socket kinds the virtual interface can carry; everything else uses the host stack.
    [[nodiscard]] static bool Supports(Domain domain, Type type, Protocol protocol);

private:
    Stack(std::unique_ptr<Client> client_, std::unique_ptr<NetworkInterface> network_interface_);

    std::unique_ptr<Client> client;
    std::unique_ptr<NetworkInterface> network_interface;

    std::atomic<bool> guest_active{false};
};

/**
 * Applies the LAN Play configuration for the running session.
 *
 * Calling this with LAN Play enabled joins the relay in the background; calling it disabled leaves
 * the relay and puts the console straight back on the host network. It is safe to call while a game
 * is running, and re-applying the same settings does nothing.
 */
void ApplySettings();

/// The stack of the running session, or nullptr when LAN Play is not active.
std::shared_ptr<Stack> GetStack();

/// Leaves the relay and disposes the stack.
void Shutdown();

} // namespace Network::LanPlay
