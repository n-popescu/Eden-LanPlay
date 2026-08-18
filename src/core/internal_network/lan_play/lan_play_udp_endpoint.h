// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Network::LanPlay {

class NetworkInterface;

/**
 * A UDP port bound on the virtual network interface.
 *
 * Datagrams are queued for pull based readers (the emulated console's BSD sockets) and can instead
 * be pushed to a handler for event driven readers (LDN discovery). An endpoint is one or the other,
 * never both.
 */
class UdpEndpoint {
public:
    struct Datagram {
        u32 source_address{};
        u16 source_port{};
        std::vector<u8> data;
        bool was_broadcast{};
    };

    using PacketHandler =
        std::function<void(u32 source_address, u16 source_port, std::span<const u8> data)>;

    UdpEndpoint(NetworkInterface& network_interface_, u16 local_port_);
    ~UdpEndpoint();

    UdpEndpoint(const UdpEndpoint&) = delete;
    UdpEndpoint& operator=(const UdpEndpoint&) = delete;

    [[nodiscard]] u16 GetLocalPort() const {
        return local_port;
    }

    /**
     * Installs a handler that receives datagrams on the relay receive thread. While one is set,
     * datagrams are not queued.
     */
    void SetPacketHandler(PacketHandler handler);

    /// Called by the virtual interface for every datagram addressed to this port.
    void HandleDatagram(u32 source_address, u16 source_port, u32 destination_address,
                        std::span<const u8> data);

    [[nodiscard]] bool TryDequeue(Datagram& out_datagram);
    [[nodiscard]] bool TryPeek(Datagram& out_datagram);

    /// Total number of bytes queued, which is what a guest FIONREAD expects.
    [[nodiscard]] std::size_t GetAvailable() const;

    [[nodiscard]] bool IsReadable() const;

    /// Waits until a datagram is queued or the timeout expires. A negative timeout waits forever.
    bool WaitForData(int timeout_milliseconds);

    /// Sends a datagram from this port. Returns the number of bytes handed to the interface.
    std::size_t SendTo(u32 destination_address, u16 destination_port, std::span<const u8> data);

    void Close();

    [[nodiscard]] bool IsClosed() const {
        return closed;
    }

private:
    static constexpr std::size_t MaxQueuedDatagrams = 256;

    NetworkInterface& network_interface;
    const u16 local_port;

    mutable std::mutex queue_mutex;
    std::condition_variable data_available;
    std::deque<Datagram> queue;

    std::mutex handler_mutex;
    PacketHandler packet_handler;

    bool closed{false};
};

} // namespace Network::LanPlay
