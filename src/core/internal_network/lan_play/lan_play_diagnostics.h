// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <string>

#include "common/common_types.h"
#include "core/internal_network/lan_play/ipv4_packet.h"
#include "core/internal_network/lan_play/lan_play_protocol.h"

namespace Network::LanPlay {

/// Why an incoming packet was not delivered to the emulated console.
enum class DropReason {
    Malformed,
    NotForUs,
    LoopedBack,
    BadHeaderChecksum,
    BadTransportChecksum,
    NoUdpEndpoint,
    NoTcpConnection,
    Reassembly,
    RelayFragment,
    UnsupportedProtocol,
    QueueFull,
    Count,
};

std::string_view GetDropReasonName(DropReason reason);

/**
 * Counters, per packet tracing and periodic summaries for one LAN Play session.
 *
 * Everything here is available in a normal build: the summary and the warnings go to the info log,
 * the per packet lines to the network log (Network_LanPlay), so a user can diagnose a session
 * without a debugger.
 */
class Diagnostics {
public:
    explicit Diagnostics(std::string relay_description);
    ~Diagnostics();

    void RelayDatagramSent(Relay::PacketType type, std::size_t size);
    void RelayDatagramReceived(Relay::PacketType type, std::span<const u8> datagram);

    void Ipv4Sent(u32 destination, u8 protocol, std::size_t size, std::size_t fragments);
    void Ipv4Received(const Ipv4::Header& header, std::span<const u8> payload);

    void Dropped(DropReason reason, std::string_view detail = {});

    void TcpRetransmit(int attempt);
    void TcpReset();

    /// Called periodically by the virtual interface: emits the summary and the relay silence warning.
    void Tick(u64 last_receive_time, bool relay_running);

    void LogFinalReport();

    [[nodiscard]] std::string GetReport() const;

private:
    std::string BuildReport() const;

    const std::string relay_description;

    mutable std::mutex mutex;

    u64 packets_sent{};
    u64 bytes_sent{};
    u64 keep_alives_sent{};
    u64 packets_received{};
    u64 bytes_received{};

    u64 udp_sent{};
    u64 udp_received{};
    u64 tcp_sent{};
    u64 tcp_received{};
    u64 icmp_sent{};
    u64 icmp_received{};

    u64 tcp_retransmits{};
    u64 tcp_resets{};

    std::array<u64, static_cast<std::size_t>(DropReason::Count)> drops{};

    /// The first few relay datagrams are dumped as hex under trace logging, for framing problems.
    unsigned int traced_datagrams{};

    u64 next_summary_time{};
    bool relay_silent{};
    bool ever_received{};
};

} // namespace Network::LanPlay
