// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/ldn/lan_play/ldn_mitm_protocol.h"
#include "core/internal_network/lan_play/ipv4_packet.h"
#include "core/internal_network/lan_play/lan_play_config.h"
#include "core/internal_network/lan_play/lan_play_connection_test.h"
#include "core/internal_network/lan_play/lan_play_network_interface.h"
#include "core/internal_network/lan_play/lan_play_protocol.h"
#include "core/internal_network/lan_play/lan_play_stack.h"
#include "core/internal_network/lan_play/lan_play_udp_endpoint.h"
#include "core/internal_network/network.h"

namespace {

namespace Ldn = Service::LDN::LanPlay::Ldn;

using namespace Network::LanPlay;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
#endif

/**
 * In-process stand-in for a switch-lan-play relay, implementing the routing the real one does: it
 * learns which client owns which 10.13.x.x address from the source address of the IPv4 packets it
 * receives, forwards unicast packets to that owner, and floods broadcasts to everybody else.
 *
 * Ported from Ryubing-LanPlay's TestLanPlayRelay so both implementations are checked against the same
 * behaviour. Note the ObjectDisposedException equivalent is not a concern here: send failures on a
 * closed socket are ordinary errno results, and the loop simply stops.
 */
class TestRelay {
public:
    TestRelay() {
        socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(socket_handle != InvalidSocket);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;

        REQUIRE(::bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

        sockaddr_in bound{};
        socklen_t bound_length = sizeof(bound);

        REQUIRE(::getsockname(socket_handle, reinterpret_cast<sockaddr*>(&bound), &bound_length) ==
                0);

        port = ntohs(bound.sin_port);
        thread = std::thread{[this] { Run(); }};
    }

    ~TestRelay() {
        running.store(false, std::memory_order_relaxed);

#ifdef _WIN32
        ::closesocket(socket_handle);
#else
        ::shutdown(socket_handle, SHUT_RDWR);
        ::close(socket_handle);
#endif

        if (thread.joinable()) {
            thread.join();
        }
    }

    TestRelay(const TestRelay&) = delete;
    TestRelay& operator=(const TestRelay&) = delete;

    [[nodiscard]] std::string Endpoint() const {
        return "127.0.0.1:" + std::to_string(port);
    }

private:
    struct EndpointKey {
        u32 address{};
        u16 port{};

        bool operator<(const EndpointKey& other) const {
            return std::tie(address, port) < std::tie(other.address, other.port);
        }
    };

    void Run() {
        std::vector<u8> buffer(4096);

        while (running.load(std::memory_order_relaxed)) {
            sockaddr_in from{};
            socklen_t from_length = sizeof(from);

            const auto received =
                ::recvfrom(socket_handle, reinterpret_cast<char*>(buffer.data()),
                           static_cast<int>(buffer.size()), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_length);

            if (received <= 0) {
                if (!running.load(std::memory_order_relaxed)) {
                    return;
                }

                continue;
            }

            const EndpointKey sender{from.sin_addr.s_addr, from.sin_port};

            clients.insert(sender);

            const auto type =
                static_cast<Relay::PacketType>(buffer[0] & 0x7F);
            const std::span<const u8> payload{buffer.data() + 1,
                                              static_cast<std::size_t>(received - 1)};

            u32 source{};
            u32 destination{};

            if (type == Relay::PacketType::Ipv4 && payload.size() >= Ipv4::MinHeaderSize) {
                source = ReadBE32(payload.subspan(12));
                destination = ReadBE32(payload.subspan(16));
            } else if (type == Relay::PacketType::Ipv4Fragment && payload.size() >= 16) {
                source = ReadBE32(payload);
                destination = ReadBE32(payload.subspan(4));
            } else {
                continue;
            }

            owners[source] = sender;

            std::vector<EndpointKey> targets;

            const auto owner = owners.find(destination);

            if (NetworkInterface::IsBroadcast(destination) || owner == owners.end()) {
                for (const EndpointKey& client : clients) {
                    if (!(client < sender) && !(sender < client)) {
                        continue;
                    }

                    targets.push_back(client);
                }
            } else {
                targets.push_back(owner->second);
            }

            for (const EndpointKey& target : targets) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = target.address;
                to.sin_port = target.port;

                ::sendto(socket_handle, reinterpret_cast<const char*>(buffer.data()),
                         static_cast<int>(received), 0, reinterpret_cast<sockaddr*>(&to),
                         sizeof(to));
            }
        }
    }

    SocketHandle socket_handle{InvalidSocket};
    u16 port{};
    std::thread thread;
    std::atomic<bool> running{true};
    std::set<EndpointKey> clients;
    std::map<u32, EndpointKey> owners;
};

std::shared_ptr<Stack> CreateStack(const TestRelay& relay, const std::string& address) {
    Configuration configuration;

    REQUIRE(Configuration::TryParse(relay.Endpoint(), address, configuration));

    auto stack = Stack::Create(configuration);

    REQUIRE(stack != nullptr);

    return stack;
}

/// Polls a condition rather than sleeping a fixed time, so a slow machine does not make this flaky.
bool WaitFor(const std::function<bool()>& condition, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};

    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    return condition();
}

} // Anonymous namespace

TEST_CASE("LanPlay: the relay carries broadcast and unicast datagrams", "[core][lan_play]") {
    Network::NetworkInstance network_instance;

    TestRelay relay;

    const auto host = CreateStack(relay, "10.13.90.2");
    const auto station = CreateStack(relay, "10.13.90.3");

    const auto host_endpoint = host->GetNetworkInterface().BindUdp(Ldn::DefaultPort);
    const auto station_endpoint = station->GetNetworkInterface().BindUdp(Ldn::DefaultPort);

    const std::string broadcast_text = "scan";
    const std::span<const u8> broadcast{reinterpret_cast<const u8*>(broadcast_text.data()),
                                        broadcast_text.size()};

    station_endpoint->SendTo(Relay::SubnetBroadcast, Ldn::DefaultPort, broadcast);

    UdpEndpoint::Datagram received;

    REQUIRE(WaitFor([&] { return host_endpoint->TryDequeue(received); }));
    CHECK(received.source_address == station->GetAddress());
    CHECK(received.was_broadcast);
    CHECK(std::string{received.data.begin(), received.data.end()} == broadcast_text);

    // The relay learned where the station is from the broadcast, so a unicast reply reaches it.
    const std::string reply_text = "here";
    const std::span<const u8> reply{reinterpret_cast<const u8*>(reply_text.data()),
                                    reply_text.size()};

    host_endpoint->SendTo(station->GetAddress(), Ldn::DefaultPort, reply);

    REQUIRE(WaitFor([&] { return station_endpoint->TryDequeue(received); }));
    CHECK(received.source_address == host->GetAddress());
    CHECK_FALSE(received.was_broadcast);
    CHECK(std::string{received.data.begin(), received.data.end()} == reply_text);
}

TEST_CASE("LanPlay: a datagram larger than the MTU is fragmented and reassembled",
          "[core][lan_play]") {
    Network::NetworkInstance network_instance;

    TestRelay relay;

    const auto sender = CreateStack(relay, "10.13.91.2");
    const auto receiver = CreateStack(relay, "10.13.91.3");

    const auto sender_endpoint = sender->GetNetworkInterface().BindUdp(Ldn::DefaultPort);
    const auto receiver_endpoint = receiver->GetNetworkInterface().BindUdp(Ldn::DefaultPort);

    // Comfortably past one Ethernet frame, so the IPv4 fragmentation path has to run.
    std::vector<u8> payload(3000);

    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<u8>(i * 7);
    }

    sender_endpoint->SendTo(receiver->GetAddress(), Ldn::DefaultPort, payload);

    UdpEndpoint::Datagram received;

    REQUIRE(WaitFor([&] { return receiver_endpoint->TryDequeue(received); }));
    REQUIRE(received.data.size() == payload.size());
    CHECK(received.data == payload);
}

TEST_CASE("LanPlay: an ldn_mitm scan request reaches a host over the relay", "[core][lan_play]") {
    Network::NetworkInstance network_instance;

    TestRelay relay;

    const auto host = CreateStack(relay, "10.13.92.2");
    const auto station = CreateStack(relay, "10.13.92.3");

    // This is the path that makes a game with no LAN mode of its own work over a relay: the scan a
    // console broadcasts on the LDN port, in the ldn_mitm framing, carried by the relay rather than
    // by the host acting as an access point.
    const auto host_endpoint = host->GetNetworkInterface().BindUdp(Ldn::DefaultPort);

    std::atomic<bool> saw_scan{false};

    host_endpoint->SetPacketHandler([&](u32, u16, std::span<const u8> data) {
        Ldn::PacketHeader header{};

        if (Ldn::TryReadHeader(data, header) && header.type == Ldn::PacketType::Scan) {
            saw_scan.store(true, std::memory_order_relaxed);
        }
    });

    const auto station_endpoint = station->GetNetworkInterface().BindUdp(Ldn::DefaultPort);
    const auto request = Ldn::BuildPacket(Ldn::PacketType::Scan, {});

    station_endpoint->SendTo(Relay::SubnetBroadcast, Ldn::DefaultPort, request);

    CHECK(WaitFor([&] { return saw_scan.load(std::memory_order_relaxed); }));
}

TEST_CASE("LanPlay: the connection test reports a reachable relay", "[core][lan_play]") {
    Network::NetworkInstance network_instance;

    TestRelay relay;

    // A second participant, so the test has something to see besides itself.
    const auto peer = CreateStack(relay, "10.13.93.3");
    const auto peer_endpoint = peer->GetNetworkInterface().BindUdp(Ldn::DefaultPort);

    std::atomic<bool> answered{false};

    peer_endpoint->SetPacketHandler([&](u32 source_address, u16 source_port,
                                        std::span<const u8> data) {
        Ldn::PacketHeader header{};

        if (!Ldn::TryReadHeader(data, header) || header.type != Ldn::PacketType::Scan) {
            return;
        }

        // Answer the way a host does, so the test can report a session as well as a participant.
        const auto response = Ldn::BuildPacket(Ldn::PacketType::ScanResponse, {});

        peer_endpoint->SendTo(source_address, source_port, response);

        answered.store(true, std::memory_order_relaxed);
    });

    const auto result = RunConnectionTest(relay.Endpoint(), "10.13.93.2");

    CHECK(result.success);
    CHECK(result.message.find("Joined the relay") != std::string::npos);
    CHECK(answered.load(std::memory_order_relaxed));
    CHECK(result.message.find("10.13.93.3") != std::string::npos);
}

TEST_CASE("LanPlay: an unusable relay server is rejected", "[core][lan_play]") {
    const auto result = RunConnectionTest("this is not a server", "");

    CHECK_FALSE(result.success);
    CHECK(result.message.find("Could not use this server") != std::string::npos);
}
