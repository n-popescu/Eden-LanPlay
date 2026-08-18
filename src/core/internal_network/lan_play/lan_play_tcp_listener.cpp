// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>

#include "common/logging.h"
#include "core/internal_network/lan_play/lan_play_network_interface.h"
#include "core/internal_network/lan_play/lan_play_tcp_connection.h"
#include "core/internal_network/lan_play/lan_play_tcp_listener.h"

namespace Network::LanPlay {

namespace {

constexpr u8 FlagSyn = 0x02;
constexpr u8 FlagAck = 0x10;

} // Anonymous namespace

TcpListener::TcpListener(NetworkInterface& network_interface_, u16 local_port_)
    : network_interface{network_interface_}, local_port{local_port_} {}

TcpListener::~TcpListener() = default;

void TcpListener::SetConnectionHandler(ConnectionHandler handler) {
    std::scoped_lock lock{handler_mutex};

    connection_handler = std::move(handler);
}

bool TcpListener::IsReadable() const {
    std::scoped_lock lock{mutex};

    return std::any_of(pending.begin(), pending.end(),
                       [](const auto& connection) { return connection->IsConnected(); });
}

void TcpListener::HandleSegment(u32 remote_address, u16 remote_port, std::span<const u8> segment) {
    const u8 flags = segment[13];

    // Only a bare SYN opens a connection; anything else belongs to a connection we do not have.
    if (closed || (flags & FlagSyn) == 0 || (flags & FlagAck) != 0) {
        return;
    }

    const u32 sequence = ReadBE32(segment.subspan(4));

    auto connection =
        TcpConnection::Create(network_interface, local_port, remote_address, remote_port);

    {
        std::scoped_lock lock{mutex};

        std::erase_if(pending, [](const auto& entry) { return entry->IsAborted(); });

        if (pending.size() >= MaxPendingConnections) {
            LOG_WARNING(Network_LanPlay, "backlog full, refusing a connection from {}.",
                        connection->Describe());

            connection->Abort();

            return;
        }

        pending.push_back(connection);
    }

    const TcpConnection* raw = connection.get();

    connection->SetClosedHandler([this, raw] { Remove(raw); });
    connection->SetEstablishedHandler([this, raw] {
        std::shared_ptr<TcpConnection> established;

        {
            std::scoped_lock lock{mutex};

            const auto it = std::find_if(pending.begin(), pending.end(), [raw](const auto& entry) {
                return entry.get() == raw;
            });

            if (it == pending.end()) {
                return;
            }

            established = *it;
        }

        Publish(established);
    });

    connection->AcceptSyn(sequence);
}

void TcpListener::Publish(const std::shared_ptr<TcpConnection>& connection) {
    ConnectionHandler handler;

    {
        std::scoped_lock lock{handler_mutex};

        handler = connection_handler;
    }

    if (!handler) {
        connection_pending.notify_all();

        return;
    }

    Remove(connection.get());

    handler(connection);
}

void TcpListener::Remove(const TcpConnection* connection) {
    std::scoped_lock lock{mutex};

    std::erase_if(pending, [connection](const auto& entry) { return entry.get() == connection; });
}

std::shared_ptr<TcpConnection> TcpListener::Accept(int timeout_milliseconds) {
    const u64 deadline = timeout_milliseconds < 0
                             ? std::numeric_limits<u64>::max()
                             : GetTickCountMs() + static_cast<u64>(timeout_milliseconds);

    while (!closed) {
        {
            std::unique_lock lock{mutex};

            std::erase_if(pending, [](const auto& entry) { return entry->IsAborted(); });

            const auto it = std::find_if(pending.begin(), pending.end(), [](const auto& entry) {
                return entry->IsConnected();
            });

            if (it != pending.end()) {
                auto connection = *it;

                pending.erase(it);

                return connection;
            }

            connection_pending.wait_for(lock, std::chrono::milliseconds{20});
        }

        if (GetTickCountMs() >= deadline) {
            return nullptr;
        }
    }

    return nullptr;
}

void TcpListener::Close() {
    if (closed) {
        return;
    }

    closed = true;

    network_interface.RemoveListener(local_port, this);

    std::vector<std::shared_ptr<TcpConnection>> outstanding;

    {
        std::scoped_lock lock{mutex};

        outstanding.swap(pending);
    }

    for (const auto& connection : outstanding) {
        connection->Abort();
    }

    {
        std::scoped_lock lock{handler_mutex};

        connection_handler = nullptr;
    }

    connection_pending.notify_all();
}

} // namespace Network::LanPlay
