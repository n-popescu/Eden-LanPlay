// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/hle/service/ldn/lan_play/ldn_mitm_protocol.h"
#include "core/internal_network/lan_play/ipv4_packet.h"
#include "core/internal_network/lan_play/lan_play_client.h"
#include "core/internal_network/lan_play/lan_play_config.h"
#include "core/internal_network/lan_play/lan_play_connection_test.h"
#include "core/internal_network/lan_play/lan_play_network_interface.h"
#include "core/internal_network/lan_play/lan_play_protocol.h"
#include "core/internal_network/lan_play/lan_play_udp_endpoint.h"
#include "core/internal_network/lan_play/virtual_address_allocator.h"

namespace Network::LanPlay {

namespace {

namespace Ldn = Service::LDN::LanPlay::Ldn;

constexpr int ListenTimeMs = 4000;

/// Source port, destination port, length, checksum.
constexpr std::size_t UdpHeaderSize = 8;

/// The ldn_mitm header of an empty scan request, which is what a console sends when it looks for a
/// local session. Anything hosting one on this relay answers it.
std::vector<u8> BuildScanRequest() {
    return Ldn::BuildPacket(Ldn::PacketType::Scan, {});
}

/// True for a UDP datagram carrying an ldn_mitm scan response, which is what a host sends back.
bool IsScanResponse(const Ipv4::Header& header, std::span<const u8> packet) {
    if (header.protocol != Ipv4::ProtocolUdp || packet.size() <= header.header_length) {
        return false;
    }

    const std::span<const u8> datagram = packet.subspan(header.header_length);

    // UDP header, then enough of the ldn_mitm header to see the magic and the type.
    if (datagram.size() < UdpHeaderSize + 5 ||
        ReadBE16(datagram.subspan(2)) != Ldn::DefaultPort) {
        return false;
    }

    const std::span<const u8> ldn = datagram.subspan(UdpHeaderSize);

    Ldn::PacketHeader ldn_header{};

    return Ldn::TryReadHeader(ldn, ldn_header) &&
           ldn_header.type == Ldn::PacketType::ScanResponse;
}

} // Anonymous namespace

ConnectionTestResult RunConnectionTest(const std::string& server,
                                       const std::string& virtual_address_setting) {
    Configuration configuration;

    if (!Configuration::TryParse(server, virtual_address_setting, configuration)) {
        return {false,
                "Could not use this server. Expected host:port, for example "
                "switch.example.com:11451. See the log for details."};
    }

    try {
        Client client{configuration};

        if (!client.Open()) {
            return {false, "Could not open a UDP socket for the relay. See the log for details."};
        }

        client.Start();

        std::mutex seen_mutex;
        std::set<u32> peers;
        std::atomic<int> scan_responses{0};
        std::string relay_message;

        client.SetInfoHandler([&](std::string_view message) {
            std::scoped_lock lock{seen_mutex};
            relay_message = std::string{message};
        });

        const std::size_t token = client.AddIpv4Handler([&](std::span<const u8> packet) {
            Ipv4::Header header{};

            if (!Ipv4::TryParse(packet, header)) {
                return;
            }

            {
                std::scoped_lock lock{seen_mutex};
                peers.insert(header.source);
            }

            if (IsScanResponse(header, packet)) {
                scan_responses.fetch_add(1, std::memory_order_relaxed);
            }
        });

        const u32 address = VirtualAddress::Allocate(client, configuration.virtual_address);

        NetworkInterface network_interface{client, address};
        network_interface.Announce();

        const auto endpoint = network_interface.BindUdp(Ldn::DefaultPort);
        const auto request = BuildScanRequest();

        endpoint->SendTo(network_interface.GetBroadcastAddress(), Ldn::DefaultPort, request);

        std::this_thread::sleep_for(std::chrono::milliseconds{ListenTimeMs});

        client.RemoveIpv4Handler(token);
        client.SetInfoHandler(nullptr);

        std::string message =
            fmt::format("Joined the relay {}:{} as {}. ", configuration.relay_host,
                        configuration.relay_port, FormatAddress(address));

        {
            std::scoped_lock lock{seen_mutex};

            // Our own announcement comes back from some relays, and it says nothing about anyone else.
            peers.erase(address);

            if (peers.empty()) {
                message +=
                    "No traffic came back, which is normal when nobody else is connected. If other "
                    "players are on this relay, check that they use the same address and port.";
            } else {
                std::string list;

                for (const u32 peer : peers) {
                    if (!list.empty()) {
                        list += ", ";
                    }

                    list += FormatAddress(peer);
                }

                message += fmt::format("Saw {} other participant(s): {}. ", peers.size(), list);

                const int responses = scan_responses.load(std::memory_order_relaxed);

                message += responses > 0
                               ? fmt::format("{} of them answered the local session scan.", responses)
                               : "None of them is hosting a local session right now.";
            }

            if (!relay_message.empty()) {
                message += fmt::format(" Relay message: {}", relay_message);
            }
        }

        LOG_INFO(Network_LanPlay, "connection test: {}", client.GetDiagnostics().GetReport());

        return {true, message};
    } catch (const std::exception& e) {
        LOG_ERROR(Network_LanPlay, "connection test failed: {}", e.what());

        return {false, fmt::format("Could not join the relay: {}", e.what())};
    }
}

} // namespace Network::LanPlay
