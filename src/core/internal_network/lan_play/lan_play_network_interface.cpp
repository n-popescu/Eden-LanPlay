// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/internal_network/lan_play/lan_play_network_interface.h"
#include "core/internal_network/lan_play/lan_play_tcp_connection.h"
#include "core/internal_network/lan_play/lan_play_tcp_listener.h"
#include "core/internal_network/lan_play/lan_play_udp_endpoint.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"

namespace Network::LanPlay {

namespace {

constexpr u8 IcmpEchoReply = 0;
constexpr u8 IcmpEchoRequest = 8;

/**
 * Broadcast addresses of the host's own networks, collected once: the stack only exists while a game
 * is using the network, and a guest socket asks about them for every datagram it sends.
 */
const std::unordered_set<u32>& GetHostBroadcastAddresses() {
    static const std::unordered_set<u32> addresses = [] {
        std::unordered_set<u32> result;

        for (const auto& adapter : GetAvailableNetworkInterfaces()) {
            const u32 ip = IPv4AddressToInteger(TranslateIPv4(adapter.ip_address));
            const u32 mask = IPv4AddressToInteger(TranslateIPv4(adapter.subnet_mask));

            // A /32 has no broadcast address of its own, and an unusable mask is not one either.
            if (ip == 0 || mask == 0 || mask == 0xFFFFFFFF) {
                continue;
            }

            if ((ip & 0xFF000000) == 0x7F000000) {
                continue;
            }

            result.insert(ip | ~mask);
        }

        return result;
    }();

    return addresses;
}

} // Anonymous namespace

u16 NetworkInterface::PortPool::Get() {
    std::scoped_lock lock{mutex};

    const u16 port = next;

    next = next == LastPort ? FirstPort : static_cast<u16>(next + 1);

    return port;
}

void NetworkInterface::PortPool::Return(u16) {
    // Ports are handed out round robin over a 16k range, which is plenty for one emulated console
    // and avoids reusing a port that a peer may still be sending to.
}

bool NetworkInterface::IsHostBroadcast(u32 candidate) {
    return GetHostBroadcastAddresses().contains(candidate);
}

NetworkInterface::NetworkInterface(Client& client_, u32 address_)
    : client{client_}, address{address_} {
    for (auto& reassembly : reassemblies) {
        reassembly.buffer.resize(0x10000);
    }

    ipv4_handler_token =
        client.AddIpv4Handler([this](std::span<const u8> packet) { HandleIpv4(packet); });

    timer_thread = std::thread{[this] { TimerLoop(); }};
}

NetworkInterface::~NetworkInterface() {
    stop_requested.store(true, std::memory_order_relaxed);

    if (timer_thread.joinable()) {
        timer_thread.join();
    }

    client.RemoveIpv4Handler(ipv4_handler_token);

    std::vector<std::shared_ptr<UdpEndpoint>> endpoints;
    std::vector<std::shared_ptr<TcpConnection>> connections;
    std::vector<std::shared_ptr<TcpListener>> listeners;

    {
        std::scoped_lock lock{udp_mutex};

        for (auto& [port, endpoint] : udp_endpoints) {
            endpoints.push_back(endpoint);
        }

        udp_endpoints.clear();
    }

    {
        std::scoped_lock lock{tcp_mutex};

        for (auto& [key, connection] : tcp_connections) {
            connections.push_back(connection);
        }

        for (auto& [port, listener] : tcp_listeners) {
            listeners.push_back(listener);
        }

        tcp_connections.clear();
        tcp_listeners.clear();
    }

    for (const auto& endpoint : endpoints) {
        endpoint->Close();
    }

    for (const auto& connection : connections) {
        connection->Abort();
    }

    for (const auto& listener : listeners) {
        listener->Close();
    }
}

std::shared_ptr<UdpEndpoint> NetworkInterface::BindUdp(u16 port) {
    std::scoped_lock lock{udp_mutex};

    if (port == 0) {
        port = udp_ports.Get();
    } else if (udp_endpoints.contains(port)) {
        return nullptr;
    }

    auto endpoint = std::make_shared<UdpEndpoint>(*this, port);

    udp_endpoints.emplace(port, endpoint);

    return endpoint;
}

void NetworkInterface::UnbindUdp(u16 port, const UdpEndpoint* endpoint) {
    std::scoped_lock lock{udp_mutex};

    const auto it = udp_endpoints.find(port);

    if (it != udp_endpoints.end() && it->second.get() == endpoint) {
        udp_endpoints.erase(it);
        udp_ports.Return(port);
    }
}

void NetworkInterface::SendUdp(u16 source_port, u32 destination_address, u16 destination_port,
                               std::span<const u8> data) {
    // The relay floods the LAN Play broadcast address, and a peer drops anything addressed to
    // neither itself nor a broadcast address, so a broadcast aimed at the host's network has to be
    // re-addressed to the LAN Play network to be seen by anybody.
    if (IsHostBroadcast(destination_address)) {
        destination_address = Relay::SubnetBroadcast;
    }

    const std::size_t length = 8 + data.size();

    std::vector<u8> segment(length);

    WriteBE16(segment, source_port);
    WriteBE16(std::span{segment}.subspan(2), destination_port);
    WriteBE16(std::span{segment}.subspan(4), static_cast<u16>(length));

    if (!data.empty()) {
        std::memcpy(segment.data() + 8, data.data(), data.size());
    }

    const u16 checksum =
        Ipv4::TransportChecksum(address, destination_address, Ipv4::ProtocolUdp, segment);

    // A zero checksum means "not computed" in UDP, so the all-ones form has to be used instead.
    WriteBE16(std::span{segment}.subspan(6), checksum == 0 ? 0xFFFF : checksum);

    SendIpv4(destination_address, Ipv4::ProtocolUdp, segment);
}

void NetworkInterface::HandleUdp(const Ipv4::Header& header, std::span<const u8> segment) {
    if (segment.size() < 8) {
        return;
    }

    const u16 source_port = ReadBE16(segment);
    const u16 destination_port = ReadBE16(segment.subspan(2));

    std::size_t length = ReadBE16(segment.subspan(4));

    if (length < 8 || length > segment.size()) {
        length = segment.size();
    }

    // A zero checksum means the sender did not compute one, which UDP allows.
    if (ReadBE16(segment.subspan(6)) != 0 &&
        Ipv4::TransportChecksum(header.source, header.destination, Ipv4::ProtocolUdp,
                                segment.first(length)) != 0) {
        GetDiagnostics().Dropped(DropReason::BadTransportChecksum,
                                 fmt::format("udp {} -> {}", source_port, destination_port));

        return;
    }

    std::shared_ptr<UdpEndpoint> endpoint;

    {
        std::scoped_lock lock{udp_mutex};

        const auto it = udp_endpoints.find(destination_port);

        if (it != udp_endpoints.end()) {
            endpoint = it->second;
        }
    }

    if (!endpoint) {
        GetDiagnostics().Dropped(DropReason::NoUdpEndpoint,
                                 fmt::format("udp port {}", destination_port));

        return;
    }

    endpoint->HandleDatagram(header.source, source_port, header.destination,
                             segment.subspan(8, length - 8));
}

std::shared_ptr<TcpListener> NetworkInterface::ListenTcp(u16 port) {
    std::scoped_lock lock{tcp_mutex};

    if (port == 0) {
        port = tcp_ports.Get();
    } else if (tcp_listeners.contains(port)) {
        return nullptr;
    }

    auto listener = std::make_shared<TcpListener>(*this, port);

    tcp_listeners.emplace(port, listener);

    return listener;
}

void NetworkInterface::RemoveListener(u16 port, const TcpListener* listener) {
    std::scoped_lock lock{tcp_mutex};

    const auto it = tcp_listeners.find(port);

    if (it != tcp_listeners.end() && it->second.get() == listener) {
        tcp_listeners.erase(it);
    }
}

u16 NetworkInterface::AllocateTcpPort() {
    return tcp_ports.Get();
}

void NetworkInterface::RegisterConnection(std::shared_ptr<TcpConnection> connection) {
    std::scoped_lock lock{tcp_mutex};

    const ConnectionKey key{connection->GetLocalPort(), connection->GetRemoteAddress(),
                            connection->GetRemotePort()};

    tcp_connections[key] = std::move(connection);
}

void NetworkInterface::UnregisterConnection(const TcpConnection* connection) {
    std::scoped_lock lock{tcp_mutex};

    const ConnectionKey key{connection->GetLocalPort(), connection->GetRemoteAddress(),
                            connection->GetRemotePort()};

    const auto it = tcp_connections.find(key);

    if (it != tcp_connections.end() && it->second.get() == connection) {
        tcp_connections.erase(it);
    }
}

void NetworkInterface::HandleTcp(const Ipv4::Header& header, std::span<const u8> segment) {
    if (segment.size() < TcpConnection::HeaderSize || IsBroadcast(header.destination)) {
        GetDiagnostics().Dropped(DropReason::Malformed, "tcp segment");

        return;
    }

    const u16 source_port = ReadBE16(segment);
    const u16 destination_port = ReadBE16(segment.subspan(2));

    if (Ipv4::TransportChecksum(header.source, header.destination, Ipv4::ProtocolTcp, segment) != 0) {
        GetDiagnostics().Dropped(DropReason::BadTransportChecksum,
                                 fmt::format("tcp {} -> {}", source_port, destination_port));

        return;
    }

    std::shared_ptr<TcpConnection> connection;
    std::shared_ptr<TcpListener> listener;

    {
        std::scoped_lock lock{tcp_mutex};

        const auto it =
            tcp_connections.find(ConnectionKey{destination_port, header.source, source_port});

        if (it != tcp_connections.end()) {
            connection = it->second;
        } else {
            const auto listener_it = tcp_listeners.find(destination_port);

            if (listener_it != tcp_listeners.end()) {
                listener = listener_it->second;
            }
        }
    }

    if (connection) {
        connection->HandleSegment(segment);
    } else if (listener) {
        listener->HandleSegment(header.source, source_port, segment);
    } else {
        GetDiagnostics().Dropped(DropReason::NoTcpConnection,
                                 fmt::format("tcp port {}", destination_port));

        TcpConnection::SendReset(*this, destination_port, header.source, source_port, segment);
    }
}

void NetworkInterface::HandleIcmp(const Ipv4::Header& header, std::span<const u8> message) {
    if (message.size() < 8) {
        return;
    }

    // Answering pings makes the emulated console behave like a real one on the LAN Play network, and
    // lets other clients detect that our virtual address is taken.
    if (message[0] == IcmpEchoRequest && !IsBroadcast(header.destination)) {
        std::vector<u8> reply(message.begin(), message.end());

        reply[0] = IcmpEchoReply;
        WriteBE16(std::span{reply}.subspan(2), 0);
        WriteBE16(std::span{reply}.subspan(2), Ipv4::Checksum(reply));

        SendIpv4(header.source, Ipv4::ProtocolIcmp, reply);
    }
}

void NetworkInterface::Announce() {
    std::vector<u8> message(16);

    message[0] = IcmpEchoRequest;
    WriteBE16(std::span{message}.subspan(4), static_cast<u16>(address));
    WriteBE16(std::span{message}.subspan(2), Ipv4::Checksum(message));

    SendIpv4(Relay::SubnetBroadcast, Ipv4::ProtocolIcmp, message);
}

void NetworkInterface::SendIpv4(u32 destination, u8 protocol, std::span<const u8> payload) {
    const u16 packet_id = identification.fetch_add(1, std::memory_order_relaxed);

    if (payload.size() <= Ipv4::MaxPayloadSize) {
        std::vector<u8> packet(Ipv4::MinHeaderSize + payload.size());

        Ipv4::WriteHeader(packet, address, destination, protocol, payload.size(), packet_id);

        if (!payload.empty()) {
            std::memcpy(packet.data() + Ipv4::MinHeaderSize, payload.data(), payload.size());
        }

        client.SendIpv4(packet);

        GetDiagnostics().Ipv4Sent(destination, protocol, packet.size(), 1);

        return;
    }

    // IPv4 fragment payloads must be a multiple of 8 bytes, except for the last one.
    const std::size_t fragment_payload = Ipv4::MaxPayloadSize & ~std::size_t{7};
    const std::size_t fragment_count =
        (payload.size() + fragment_payload - 1) / fragment_payload;

    GetDiagnostics().Ipv4Sent(destination, protocol, payload.size(), fragment_count);

    for (std::size_t offset = 0; offset < payload.size(); offset += fragment_payload) {
        const std::size_t length = std::min(fragment_payload, payload.size() - offset);
        const bool last = offset + length >= payload.size();

        const u16 flags = static_cast<u16>((last ? 0 : Ipv4::FlagMoreFragments) |
                                          ((offset / 8) & Ipv4::FragmentOffsetMask));

        std::vector<u8> packet(Ipv4::MinHeaderSize + length);

        Ipv4::WriteHeader(packet, address, destination, protocol, length, packet_id, flags);
        std::memcpy(packet.data() + Ipv4::MinHeaderSize, payload.data() + offset, length);

        client.SendIpv4(packet);
    }
}

void NetworkInterface::HandleIpv4(std::span<const u8> packet) {
    Ipv4::Header header{};

    if (!Ipv4::TryParse(packet, header)) {
        GetDiagnostics().Dropped(DropReason::Malformed,
                                 fmt::format("{} byte ipv4 packet", packet.size()));

        return;
    }

    if (Ipv4::Checksum(packet.first(header.header_length)) != 0) {
        GetDiagnostics().Dropped(DropReason::BadHeaderChecksum);

        return;
    }

    // The relay hands us everything it routes our way, plus anything it floods. Drop what a real
    // interface would drop: packets addressed to somebody else, and our own echoed traffic.
    if (header.destination != address && !IsBroadcast(header.destination)) {
        GetDiagnostics().Dropped(DropReason::NotForUs);

        return;
    }

    // Traffic that appears to come from us is either our own broadcast coming back or a relay
    // reflecting our own packets.
    if (header.source == address) {
        GetDiagnostics().Dropped(DropReason::LoopedBack);

        return;
    }

    if (header.IsFragment()) {
        std::vector<u8> reassembled;

        if (!TryReassemble(header, packet, reassembled)) {
            return;
        }

        GetDiagnostics().Ipv4Received(header, reassembled);

        Dispatch(header, reassembled);

        return;
    }

    const auto transport_payload =
        packet.subspan(header.header_length, header.total_length - header.header_length);

    GetDiagnostics().Ipv4Received(header, transport_payload);

    Dispatch(header, transport_payload);
}

void NetworkInterface::Dispatch(const Ipv4::Header& header, std::span<const u8> payload) {
    switch (header.protocol) {
    case Ipv4::ProtocolUdp:
        HandleUdp(header, payload);
        break;

    case Ipv4::ProtocolTcp:
        HandleTcp(header, payload);
        break;

    case Ipv4::ProtocolIcmp:
        HandleIcmp(header, payload);
        break;

    default:
        GetDiagnostics().Dropped(DropReason::UnsupportedProtocol,
                                 fmt::format("ip protocol {}", header.protocol));
        break;
    }
}

bool NetworkInterface::TryReassemble(const Ipv4::Header& header, std::span<const u8> packet,
                                     std::vector<u8>& out_payload) {
    out_payload.clear();

    const std::size_t fragment_length = header.total_length - header.header_length;

    if (fragment_length == 0 || header.fragment_offset + fragment_length > 0xFFFF) {
        GetDiagnostics().Dropped(
            DropReason::Reassembly,
            fmt::format("fragment of {} bytes at offset {}", fragment_length,
                        header.fragment_offset));

        return false;
    }

    std::scoped_lock lock{reassembly_mutex};

    const u64 now = GetTickCountMs();

    Reassembly* entry = nullptr;
    Reassembly* free_slot = nullptr;

    for (auto& candidate : reassemblies) {
        if (!candidate.in_use) {
            if (free_slot == nullptr) {
                free_slot = &candidate;
            }

            continue;
        }

        if (now - candidate.timestamp > ReassemblyTimeoutMs) {
            candidate.in_use = false;

            if (free_slot == nullptr) {
                free_slot = &candidate;
            }

            continue;
        }

        if (candidate.identification == header.identification && candidate.source == header.source &&
            candidate.destination == header.destination && candidate.protocol == header.protocol) {
            entry = &candidate;

            break;
        }
    }

    if (entry == nullptr) {
        if (free_slot == nullptr) {
            GetDiagnostics().Dropped(DropReason::Reassembly, "reassembly buffer is full");

            return false;
        }

        entry = free_slot;
        entry->in_use = true;
        entry->identification = header.identification;
        entry->source = header.source;
        entry->destination = header.destination;
        entry->protocol = header.protocol;
        entry->length = 0;
        entry->has_last = false;
        entry->received_bytes = 0;
    }

    entry->timestamp = now;
    entry->received_bytes += fragment_length;

    std::memcpy(entry->buffer.data() + header.fragment_offset,
                packet.data() + header.header_length, fragment_length);

    if (!header.MoreFragments()) {
        entry->has_last = true;
        entry->length = header.fragment_offset + fragment_length;
    }

    if (!entry->has_last || entry->received_bytes < entry->length) {
        return false;
    }

    out_payload.assign(entry->buffer.begin(), entry->buffer.begin() + entry->length);

    entry->in_use = false;

    return true;
}

void NetworkInterface::TimerLoop() {
    while (!stop_requested.load(std::memory_order_relaxed)) {
        GetDiagnostics().Tick(client.GetLastReceiveTime(), client.IsRunning());

        std::vector<std::shared_ptr<TcpConnection>> connections;

        {
            std::scoped_lock lock{tcp_mutex};

            connections.reserve(tcp_connections.size());

            for (auto& [key, connection] : tcp_connections) {
                connections.push_back(connection);
            }
        }

        for (const auto& connection : connections) {
            connection->Tick();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{TimerIntervalMs});
    }
}

} // namespace Network::LanPlay
