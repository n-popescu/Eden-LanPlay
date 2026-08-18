// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "core/internal_network/lan_play/ipv4_packet.h"
#include "core/internal_network/lan_play/lan_play_client.h"
#include "core/internal_network/lan_play/lan_play_diagnostics.h"

namespace Network::LanPlay {

class TcpConnection;
class TcpListener;
class UdpEndpoint;

/**
 * Virtual network interface of the emulated console on the LAN Play network.
 *
 * It owns a 10.13.x.x address that only exists inside the emulator, turns the traffic of the
 * emulated console into IPv4 packets for the relay client, and dispatches the IPv4 packets coming
 * back from the relay to the UDP endpoints, TCP connections and ICMP handler registered on it.
 * No host network interface is touched and no ARP is needed, because the relay boundary is IPv4.
 */
class NetworkInterface {
public:
    NetworkInterface(Client& client_, u32 address_);
    ~NetworkInterface();

    NetworkInterface(const NetworkInterface&) = delete;
    NetworkInterface& operator=(const NetworkInterface&) = delete;

    [[nodiscard]] u32 GetAddress() const {
        return address;
    }

    [[nodiscard]] u32 GetSubnetMask() const {
        return Relay::SubnetMask;
    }

    [[nodiscard]] u32 GetBroadcastAddress() const {
        return Relay::SubnetBroadcast;
    }

    [[nodiscard]] Client& GetClient() const {
        return client;
    }

    [[nodiscard]] Diagnostics& GetDiagnostics() const {
        return client.GetDiagnostics();
    }

    [[nodiscard]] static bool IsInSubnet(u32 candidate) {
        return (candidate & Relay::SubnetMask) == Relay::SubnetNetwork;
    }

    /**
     * True for anything the LAN Play relay floods to every client: the subnet broadcast address and
     * the all-ones broadcast address a guest may use for discovery.
     */
    [[nodiscard]] static bool IsBroadcast(u32 candidate) {
        return candidate == Relay::SubnetBroadcast || candidate == 0xFFFFFFFF;
    }

    /**
     * True for the broadcast address of one of the host's own IPv4 networks, such as 192.168.0.255
     * for a host on 192.168.0.91/24.
     *
     * A game that implements its own LAN mode asks nifm for the console's address and broadcasts to
     * that network's broadcast address, so it uses the host network's broadcast address for as long
     * as the console still presents its host address. Those broadcasts are LAN discovery and belong
     * on the LAN Play network.
     */
    [[nodiscard]] static bool IsHostBroadcast(u32 candidate);

    /// True when traffic for this address belongs on the LAN Play network, not on the host network.
    [[nodiscard]] static bool IsHandledAddress(u32 candidate) {
        return IsInSubnet(candidate) || IsBroadcast(candidate) || IsHostBroadcast(candidate);
    }

    /// Binds a UDP port. Passing 0 allocates an ephemeral one. Returns nullptr if it is taken.
    std::shared_ptr<UdpEndpoint> BindUdp(u16 port);
    void UnbindUdp(u16 port, const UdpEndpoint* endpoint);

    void SendUdp(u16 source_port, u32 destination_address, u16 destination_port,
                 std::span<const u8> data);

    /// Starts listening on a TCP port. Passing 0 allocates an ephemeral one.
    std::shared_ptr<TcpListener> ListenTcp(u16 port);
    void RemoveListener(u16 port, const TcpListener* listener);

    u16 AllocateTcpPort();

    void RegisterConnection(std::shared_ptr<TcpConnection> connection);
    void UnregisterConnection(const TcpConnection* connection);

    /**
     * Wraps a transport payload in an IPv4 header and hands it to the relay, fragmenting it the way
     * a real interface would when it does not fit in a single Ethernet frame.
     */
    void SendIpv4(u32 destination, u8 protocol, std::span<const u8> payload);

    /**
     * Announces our virtual address to the relay, which routes on the addresses it has seen its
     * clients use. A real console does this implicitly with its ARP and discovery traffic; without
     * it a relay would not know where to send packets addressed to us until we spoke first.
     */
    void Announce();

private:
    static constexpr std::size_t ReassemblySlots = 16;
    static constexpr u64 ReassemblyTimeoutMs = 5000;
    static constexpr u64 TimerIntervalMs = 100;

    /// One in-flight IPv4 reassembly.
    struct Reassembly {
        bool in_use{};
        u16 identification{};
        u32 source{};
        u32 destination{};
        u8 protocol{};
        std::size_t length{};
        bool has_last{};
        std::size_t received_bytes{};
        u64 timestamp{};
        std::vector<u8> buffer;
    };

    /// Ephemeral port allocator for the virtual interface's own port space.
    class PortPool {
    public:
        u16 Get();
        void Return(u16 port);

    private:
        static constexpr u16 FirstPort = 49152;
        static constexpr u16 LastPort = 65535;

        std::mutex mutex;
        u16 next{FirstPort};
    };

    void HandleIpv4(std::span<const u8> packet);
    void Dispatch(const Ipv4::Header& header, std::span<const u8> payload);
    void HandleUdp(const Ipv4::Header& header, std::span<const u8> segment);
    void HandleTcp(const Ipv4::Header& header, std::span<const u8> segment);
    void HandleIcmp(const Ipv4::Header& header, std::span<const u8> message);
    bool TryReassemble(const Ipv4::Header& header, std::span<const u8> packet,
                       std::vector<u8>& out_payload);
    void TimerLoop();

    Client& client;
    const u32 address;

    std::size_t ipv4_handler_token{};

    PortPool udp_ports;
    PortPool tcp_ports;

    mutable std::mutex udp_mutex;
    std::unordered_map<u16, std::shared_ptr<UdpEndpoint>> udp_endpoints;

    mutable std::mutex tcp_mutex;
    std::unordered_map<u16, std::shared_ptr<TcpListener>> tcp_listeners;

    /// Connections keyed by the tuple that identifies them on the wire.
    struct ConnectionKey {
        u16 local_port;
        u32 remote_address;
        u16 remote_port;

        bool operator==(const ConnectionKey&) const = default;
    };

    struct ConnectionKeyHash {
        std::size_t operator()(const ConnectionKey& key) const {
            return (static_cast<std::size_t>(key.remote_address) << 32) ^
                   (static_cast<std::size_t>(key.local_port) << 16) ^ key.remote_port;
        }
    };

    std::unordered_map<ConnectionKey, std::shared_ptr<TcpConnection>, ConnectionKeyHash>
        tcp_connections;

    std::mutex reassembly_mutex;
    std::array<Reassembly, ReassemblySlots> reassemblies;

    std::atomic<u16> identification{0};

    std::atomic<bool> stop_requested{false};
    std::thread timer_thread;
};

} // namespace Network::LanPlay
