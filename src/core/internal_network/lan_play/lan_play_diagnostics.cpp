// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/logging.h"
#include "core/internal_network/lan_play/lan_play_diagnostics.h"

namespace Network::LanPlay {

namespace {

constexpr u64 SummaryIntervalMs = 30 * 1000;

/// How long the relay may stay quiet before we say so; the reference client keepalive is 10 seconds.
constexpr u64 RelaySilenceMs = 30 * 1000;

constexpr std::array<std::string_view, static_cast<std::size_t>(DropReason::Count)> DropNames{
    "Malformed",   "NotForUs",         "LoopedBack",          "BadHeaderChecksum",
    "BadTransportChecksum", "NoUdpEndpoint", "NoTcpConnection", "Reassembly",
    "RelayFragment", "UnsupportedProtocol", "QueueFull",
};

std::string_view GetProtocolName(u8 protocol) {
    switch (protocol) {
    case Ipv4::ProtocolUdp:
        return "UDP";
    case Ipv4::ProtocolTcp:
        return "TCP";
    case Ipv4::ProtocolIcmp:
        return "ICMP";
    default:
        return "IP";
    }
}

} // Anonymous namespace

std::string_view GetDropReasonName(DropReason reason) {
    return DropNames[static_cast<std::size_t>(reason)];
}

Diagnostics::Diagnostics(std::string relay_description_)
    : relay_description{std::move(relay_description_)} {
    next_summary_time = GetTickCountMs() + SummaryIntervalMs;
}

Diagnostics::~Diagnostics() = default;

void Diagnostics::RelayDatagramSent(Relay::PacketType type, std::size_t size) {
    std::scoped_lock lock{mutex};

    packets_sent++;
    bytes_sent += size;

    if (type == Relay::PacketType::KeepAlive) {
        keep_alives_sent++;
    }
}

void Diagnostics::RelayDatagramReceived(Relay::PacketType type, std::span<const u8> datagram) {
    std::scoped_lock lock{mutex};

    packets_received++;
    bytes_received += datagram.size();
    ever_received = true;

    if (relay_silent) {
        relay_silent = false;

        LOG_INFO(Network_LanPlay, "the relay {} is answering again.", relay_description);
    }

    // The first few datagrams are what to attach when the framing itself is suspect.
    if (type != Relay::PacketType::KeepAlive && traced_datagrams < 8) {
        traced_datagrams++;

        LOG_TRACE(Network_LanPlay, "relay datagram #{} ({} bytes): {:02x}", traced_datagrams,
                  datagram.size(), fmt::join(datagram.first(std::min<std::size_t>(datagram.size(), 64)), " "));
    }
}

void Diagnostics::Ipv4Sent(u32 destination, u8 protocol, std::size_t size, std::size_t fragments) {
    {
        std::scoped_lock lock{mutex};

        switch (protocol) {
        case Ipv4::ProtocolUdp:
            udp_sent++;
            break;
        case Ipv4::ProtocolTcp:
            tcp_sent++;
            break;
        case Ipv4::ProtocolIcmp:
            icmp_sent++;
            break;
        default:
            break;
        }
    }

    if (fragments > 1) {
        LOG_DEBUG(Network_LanPlay, "out: {} -> {} {} bytes in {} fragments",
                  GetProtocolName(protocol), FormatAddress(destination), size, fragments);
    } else {
        LOG_DEBUG(Network_LanPlay, "out: {} -> {} {} bytes", GetProtocolName(protocol),
                  FormatAddress(destination), size);
    }
}

void Diagnostics::Ipv4Received(const Ipv4::Header& header, std::span<const u8> payload) {
    {
        std::scoped_lock lock{mutex};

        switch (header.protocol) {
        case Ipv4::ProtocolUdp:
            udp_received++;
            break;
        case Ipv4::ProtocolTcp:
            tcp_received++;
            break;
        case Ipv4::ProtocolIcmp:
            icmp_received++;
            break;
        default:
            break;
        }
    }

    const bool has_ports = (header.protocol == Ipv4::ProtocolUdp ||
                            header.protocol == Ipv4::ProtocolTcp) &&
                           payload.size() >= 4;

    if (has_ports) {
        LOG_DEBUG(Network_LanPlay, "in: {} {} -> {} ports {} -> {} {} bytes",
                  GetProtocolName(header.protocol), FormatAddress(header.source),
                  FormatAddress(header.destination), ReadBE16(payload), ReadBE16(payload.subspan(2)),
                  payload.size());
    } else {
        LOG_DEBUG(Network_LanPlay, "in: {} {} -> {} {} bytes", GetProtocolName(header.protocol),
                  FormatAddress(header.source), FormatAddress(header.destination), payload.size());
    }
}

void Diagnostics::Dropped(DropReason reason, std::string_view detail) {
    {
        std::scoped_lock lock{mutex};

        drops[static_cast<std::size_t>(reason)]++;
    }

    // A relay that floods the subnet sends us everybody's traffic, so these two are the normal case
    // and would bury everything else if they were logged per packet.
    if (reason == DropReason::NotForUs || reason == DropReason::LoopedBack) {
        return;
    }

    if (detail.empty()) {
        LOG_DEBUG(Network_LanPlay, "dropped a packet ({})", GetDropReasonName(reason));
    } else {
        LOG_DEBUG(Network_LanPlay, "dropped a packet ({}): {}", GetDropReasonName(reason), detail);
    }
}

void Diagnostics::TcpRetransmit(int attempt) {
    std::scoped_lock lock{mutex};

    tcp_retransmits++;

    LOG_DEBUG(Network_LanPlay, "retransmitting a TCP segment (attempt {})", attempt);
}

void Diagnostics::TcpReset() {
    std::scoped_lock lock{mutex};

    tcp_resets++;
}

void Diagnostics::Tick(u64 last_receive_time, bool relay_running) {
    const u64 now = GetTickCountMs();

    bool report_silence = false;
    bool report_summary = false;

    {
        std::scoped_lock lock{mutex};

        if (now >= next_summary_time) {
            next_summary_time = now + SummaryIntervalMs;
            report_summary = packets_sent != 0 || packets_received != 0;
        }

        if (relay_running && !relay_silent) {
            const u64 reference = last_receive_time != 0 ? last_receive_time : 0;

            if (reference == 0 ? !ever_received && now >= RelaySilenceMs
                               : now - reference >= RelaySilenceMs) {
                relay_silent = true;
                report_silence = true;
            }
        }
    }

    if (report_summary) {
        LOG_INFO(Network_LanPlay, "{}", GetReport());
    }

    if (report_silence) {
        LOG_WARNING(Network_LanPlay,
                    "nothing received from the relay {} in {} seconds. Check the address and port, "
                    "that outbound UDP is not blocked, and that the relay is up.",
                    relay_description, RelaySilenceMs / 1000);
    }
}

void Diagnostics::LogFinalReport() {
    LOG_INFO(Network_LanPlay, "session finished. {}", GetReport());
}

std::string Diagnostics::GetReport() const {
    std::scoped_lock lock{mutex};

    return BuildReport();
}

std::string Diagnostics::BuildReport() const {
    std::string drop_summary;

    for (std::size_t i = 0; i < drops.size(); i++) {
        if (drops[i] == 0) {
            continue;
        }

        drop_summary +=
            fmt::format("{}{}={}", drop_summary.empty() ? "" : " ",
                        GetDropReasonName(static_cast<DropReason>(i)), drops[i]);
    }

    return fmt::format(
        "status: relay {}, sent {} packets ({} bytes, {} keepalives), received {} packets "
        "({} bytes), udp {}/{}, tcp {}/{}, icmp {}/{}, tcp retransmits {}, resets {}{}{}",
        relay_description, packets_sent, bytes_sent, keep_alives_sent, packets_received,
        bytes_received, udp_sent, udp_received, tcp_sent, tcp_received, icmp_sent, icmp_received,
        tcp_retransmits, tcp_resets, drop_summary.empty() ? "" : ", dropped ", drop_summary);
}

} // namespace Network::LanPlay
