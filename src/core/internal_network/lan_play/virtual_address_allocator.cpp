// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <condition_variable>
#include <random>
#include <vector>

#include "common/logging.h"
#include "core/internal_network/lan_play/ipv4_packet.h"
#include "core/internal_network/lan_play/lan_play_client.h"
#include "core/internal_network/lan_play/virtual_address_allocator.h"

namespace Network::LanPlay::VirtualAddress {

namespace {

constexpr u64 ProbeTimeoutMs = 500;
constexpr int ProbeAttempts = 4;

constexpr u8 EchoReply = 0;
constexpr u8 EchoRequest = 8;

/// Identifier put in the probe so that an unrelated ping cannot be mistaken for an answer.
constexpr u16 ProbeIdentifier = 0x4564; // "Ed"

u32 RandomAddress() {
    static thread_local std::mt19937 engine{std::random_device{}()};

    std::uniform_int_distribution<u32> third{0, 255};
    std::uniform_int_distribution<u32> fourth{1, 254};

    while (true) {
        const u32 candidate = Relay::SubnetNetwork | (third(engine) << 8) | fourth(engine);

        if (IsUsableAddress(candidate)) {
            return candidate;
        }
    }
}

std::vector<u8> BuildEchoRequest(u32 source, u32 candidate) {
    std::vector<u8> packet(Ipv4::MinHeaderSize + 16);
    const auto message = std::span{packet}.subspan(Ipv4::MinHeaderSize);

    message[0] = EchoRequest;
    WriteBE16(message.subspan(4), ProbeIdentifier);
    WriteBE16(message.subspan(6), 1);
    WriteBE16(message.subspan(2), Ipv4::Checksum(message));

    Ipv4::WriteHeader(packet, source, candidate, Ipv4::ProtocolIcmp, message.size(), 1);

    return packet;
}

} // Anonymous namespace

bool IsUsableAddress(u32 address) {
    if ((address & Relay::SubnetMask) != Relay::SubnetNetwork) {
        return false;
    }

    if (address == Relay::GatewayAddress) {
        return false;
    }

    const u8 third = static_cast<u8>(address >> 8);
    const u8 fourth = static_cast<u8>(address);

    if (fourth == 0 || fourth == 255) {
        return false;
    }

    return !(third == 0 && fourth == 1);
}

bool IsAddressTaken(Client& client, u32 candidate) {
    std::mutex mutex;
    std::condition_variable condition;
    bool answered = false;

    // The probe is sent from a throwaway address rather than from the candidate itself: a relay
    // routes packets to whichever client last used an address, so probing "as" the candidate would
    // make the relay send the probe straight back to us instead of to the console we are looking for.
    u32 probe_source = RandomAddress();

    while (probe_source == candidate) {
        probe_source = RandomAddress();
    }

    const std::size_t token = client.AddIpv4Handler([&](std::span<const u8> packet) {
        Ipv4::Header header{};

        if (!Ipv4::TryParse(packet, header) || header.source != candidate ||
            header.protocol != Ipv4::ProtocolIcmp) {
            return;
        }

        const auto message = packet.subspan(header.header_length);

        // Only an echo *reply* means somebody answered. Relays route on the addresses they have
        // seen, so they can reflect the probe itself back at us, which proves nothing.
        if (message.size() >= 8 && message[0] == EchoReply &&
            ReadBE16(message.subspan(4)) == ProbeIdentifier) {
            std::scoped_lock lock{mutex};

            answered = true;
            condition.notify_all();
        }
    });

    client.SendIpv4(BuildEchoRequest(probe_source, candidate));

    {
        std::unique_lock lock{mutex};

        condition.wait_for(lock, std::chrono::milliseconds{ProbeTimeoutMs},
                           [&] { return answered; });
    }

    client.RemoveIpv4Handler(token);

    std::scoped_lock lock{mutex};

    return answered;
}

u32 Allocate(Client& client, std::optional<u32> preferred) {
    if (preferred && IsUsableAddress(*preferred)) {
        LOG_INFO(Network_LanPlay, "using the configured virtual address {}.",
                 FormatAddress(*preferred));

        return *preferred;
    }

    for (int attempt = 0; attempt < ProbeAttempts; attempt++) {
        const u32 candidate = RandomAddress();

        if (!IsAddressTaken(client, candidate)) {
            LOG_INFO(Network_LanPlay, "using the virtual address {}.", FormatAddress(candidate));

            return candidate;
        }

        LOG_INFO(Network_LanPlay, "virtual address {} is already in use, trying another one.",
                 FormatAddress(candidate));
    }

    const u32 fallback = RandomAddress();

    LOG_WARNING(Network_LanPlay, "could not confirm a free virtual address, using {}.",
                FormatAddress(fallback));

    return fallback;
}

} // namespace Network::LanPlay::VirtualAddress
