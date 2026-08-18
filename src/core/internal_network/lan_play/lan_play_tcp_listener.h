// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Network::LanPlay {

class NetworkInterface;
class TcpConnection;

/**
 * A listening TCP port on the virtual network interface: turns incoming SYNs from the LAN Play
 * network into connections waiting to be accepted.
 */
class TcpListener {
public:
    using ConnectionHandler = std::function<void(std::shared_ptr<TcpConnection>)>;

    TcpListener(NetworkInterface& network_interface_, u16 local_port_);
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    [[nodiscard]] u16 GetLocalPort() const {
        return local_port;
    }

    /**
     * Installs a handler called once a connection is established. While one is set, connections are
     * handed to it instead of being queued for Accept(), which is what the event driven LDN
     * discovery server needs.
     */
    void SetConnectionHandler(ConnectionHandler handler);

    [[nodiscard]] bool IsReadable() const;

    /// Called by the virtual interface for every segment addressed to this port.
    void HandleSegment(u32 remote_address, u16 remote_port, std::span<const u8> segment);

    /// Waits for an established incoming connection. A negative timeout waits forever.
    std::shared_ptr<TcpConnection> Accept(int timeout_milliseconds);

    void Close();

private:
    static constexpr std::size_t MaxPendingConnections = 16;

    void Publish(const std::shared_ptr<TcpConnection>& connection);
    void Remove(const TcpConnection* connection);

    NetworkInterface& network_interface;
    const u16 local_port;

    mutable std::mutex mutex;
    std::condition_variable connection_pending;
    std::vector<std::shared_ptr<TcpConnection>> pending;

    std::mutex handler_mutex;
    ConnectionHandler connection_handler;

    bool closed{false};
};

} // namespace Network::LanPlay
