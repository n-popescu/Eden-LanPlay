// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include <fmt/format.h>

#include "core/internal_network/lan_play/lan_play_network_interface.h"
#include "core/internal_network/lan_play/lan_play_udp_endpoint.h"

namespace Network::LanPlay {

UdpEndpoint::UdpEndpoint(NetworkInterface& network_interface_, u16 local_port_)
    : network_interface{network_interface_}, local_port{local_port_} {}

UdpEndpoint::~UdpEndpoint() = default;

void UdpEndpoint::SetPacketHandler(PacketHandler handler) {
    std::scoped_lock lock{handler_mutex};

    packet_handler = std::move(handler);
}

void UdpEndpoint::HandleDatagram(u32 source_address, u16 source_port, u32 destination_address,
                                 std::span<const u8> data) {
    if (closed) {
        return;
    }

    {
        std::scoped_lock lock{handler_mutex};

        if (packet_handler) {
            packet_handler(source_address, source_port, data);

            return;
        }
    }

    {
        std::scoped_lock lock{queue_mutex};

        if (queue.size() >= MaxQueuedDatagrams) {
            // The guest is not reading fast enough; drop the oldest datagram, like a real interface.
            queue.pop_front();

            network_interface.GetDiagnostics().Dropped(
                DropReason::QueueFull, fmt::format("udp port {}", local_port));
        }

        Datagram datagram;
        datagram.source_address = source_address;
        datagram.source_port = source_port;
        datagram.data.assign(data.begin(), data.end());
        datagram.was_broadcast = network_interface.IsBroadcast(destination_address);

        queue.push_back(std::move(datagram));
    }

    data_available.notify_all();
}

bool UdpEndpoint::TryDequeue(Datagram& out_datagram) {
    std::scoped_lock lock{queue_mutex};

    if (queue.empty()) {
        return false;
    }

    out_datagram = std::move(queue.front());
    queue.pop_front();

    return true;
}

bool UdpEndpoint::TryPeek(Datagram& out_datagram) {
    std::scoped_lock lock{queue_mutex};

    if (queue.empty()) {
        return false;
    }

    out_datagram = queue.front();

    return true;
}

std::size_t UdpEndpoint::GetAvailable() const {
    std::scoped_lock lock{queue_mutex};

    std::size_t total = 0;

    for (const auto& datagram : queue) {
        total += datagram.data.size();
    }

    return total;
}

bool UdpEndpoint::IsReadable() const {
    std::scoped_lock lock{queue_mutex};

    return !queue.empty();
}

bool UdpEndpoint::WaitForData(int timeout_milliseconds) {
    std::unique_lock lock{queue_mutex};

    if (!queue.empty()) {
        return true;
    }

    if (closed) {
        return false;
    }

    if (timeout_milliseconds < 0) {
        data_available.wait(lock, [this] { return !queue.empty() || closed; });

        return !queue.empty();
    }

    data_available.wait_for(lock, std::chrono::milliseconds{timeout_milliseconds},
                            [this] { return !queue.empty() || closed; });

    return !queue.empty();
}

std::size_t UdpEndpoint::SendTo(u32 destination_address, u16 destination_port,
                                std::span<const u8> data) {
    if (closed) {
        return 0;
    }

    network_interface.SendUdp(local_port, destination_address, destination_port, data);

    return data.size();
}

void UdpEndpoint::Close() {
    if (closed) {
        return;
    }

    closed = true;

    network_interface.UnbindUdp(local_port, this);

    {
        std::scoped_lock lock{handler_mutex};

        packet_handler = nullptr;
    }

    {
        std::scoped_lock lock{queue_mutex};

        queue.clear();
    }

    // Wake up any blocked reader so a close from another thread does not leave it stuck.
    data_available.notify_all();
}

} // namespace Network::LanPlay
