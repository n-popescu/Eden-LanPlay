// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "core/internal_network/lan_play/lan_play_config.h"
#include "core/internal_network/lan_play/lan_play_diagnostics.h"
#include "core/internal_network/lan_play/lan_play_protocol.h"

namespace Network::LanPlay {

/**
 * Embedded switch-lan-play client: carries raw IPv4 packets between the emulated console's virtual
 * network interface and a LAN Play relay server over a single host UDP socket.
 *
 * Nothing is captured or injected on a host interface, so no libpcap, no virtual adapter and no
 * elevated privileges are involved on any platform.
 */
class Client {
public:
    /// Callback invoked for every complete IPv4 packet received from the relay.
    using Ipv4Handler = std::function<void(std::span<const u8>)>;

    explicit Client(Configuration configuration_);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Opens the host UDP socket. Returns false if it could not be created or bound.
    bool Open();

    /// Starts the receive loop and the keepalive timer.
    void Start();

    void Stop();

    /**
     * Registers a handler for received IPv4 packets and returns a token to unregister it with.
     * Handlers run on the client's receive thread, so they must not block for long.
     */
    std::size_t AddIpv4Handler(Ipv4Handler handler);
    void RemoveIpv4Handler(std::size_t token);

    /**
     * Sends a complete IPv4 packet (starting at its header) to the relay, fragmenting it at the
     * configured path MTU if needed. The relay routes it using the packet's destination address.
     */
    void SendIpv4(std::span<const u8> packet);

    [[nodiscard]] bool IsRunning() const {
        return is_running.load(std::memory_order_relaxed);
    }

    /// Tick count of the last datagram received from the relay, or 0 if nothing was ever received.
    [[nodiscard]] u64 GetLastReceiveTime() const {
        return last_receive_time.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const Configuration& GetConfiguration() const {
        return configuration;
    }

    [[nodiscard]] Diagnostics& GetDiagnostics() {
        return diagnostics;
    }

private:
    struct Impl;

    /// One in-flight relay level fragment reassembly.
    struct Fragment {
        bool in_use{};
        u16 id{};
        u32 source{};
        u32 received_parts{};
        std::size_t total_length{};
        u64 timestamp{};
        std::vector<u8> buffer;
    };

    void ReceiveLoop();
    void KeepAliveLoop();
    void HandleDatagram(std::span<const u8> datagram);
    void DeliverIpv4(std::span<const u8> packet);
    void HandleFragment(std::span<const u8> payload);
    void HandleAuthMe(std::span<const u8> payload);
    void SendFragmented(std::span<const u8> packet, std::size_t path_mtu);
    void Send(Relay::PacketType type, std::span<const u8> payload);

    const Configuration configuration;

    Diagnostics diagnostics;

    std::unique_ptr<Impl> impl;

    std::thread receive_thread;
    std::thread keep_alive_thread;

    std::atomic<bool> is_running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<u64> last_receive_time{0};

    std::mutex handler_mutex;
    std::vector<std::pair<std::size_t, Ipv4Handler>> handlers;
    std::size_t next_handler_token{1};

    std::mutex fragment_mutex;
    std::array<Fragment, Relay::MaxFragmentParts> fragments;
    u16 fragment_id{};

    std::mutex send_mutex;
};

} // namespace Network::LanPlay
