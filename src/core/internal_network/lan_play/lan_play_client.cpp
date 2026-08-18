// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "common/logging.h"
#include "core/internal_network/lan_play/ipv4_packet.h"
#include "core/internal_network/lan_play/lan_play_client.h"

namespace Network::LanPlay {

namespace {

constexpr std::size_t ReceiveBufferSize = 4096;

/**
 * The receive loop wakes up this often even when the relay is silent. Closing a socket does not
 * reliably abort a blocking receive on every platform, so the loop polls its own stop flag instead
 * of relying on that.
 */
constexpr int ReceiveTimeoutMs = 500;

constexpr u64 FragmentTimeoutMs = 5000;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocketHandle = INVALID_SOCKET;

int GetLastSocketError() {
    return WSAGetLastError();
}

void CloseSocketHandle(SocketHandle handle) {
    closesocket(handle);
}

/**
 * On Windows a UDP socket fails its next receive with WSAECONNRESET when a previous send drew back
 * an ICMP "port unreachable", which happens whenever the relay is momentarily down. Neither Linux
 * nor macOS does this, and neither does a real console, so it is turned off.
 */
void DisableUdpConnectionReset(SocketHandle handle) {
    constexpr DWORD SIO_UDP_CONNRESET_CODE = 0x9800000C;

    BOOL enabled = FALSE;
    DWORD returned = 0;

    WSAIoctl(handle, SIO_UDP_CONNRESET_CODE, &enabled, sizeof(enabled), nullptr, 0, &returned,
             nullptr, nullptr);
}

bool IsRecoverableReceiveError(int error) {
    switch (error) {
    case WSAETIMEDOUT:
    case WSAEWOULDBLOCK:
    case WSAECONNRESET:
    case WSAENETRESET:
    case WSAEMSGSIZE:
    case WSAEINTR:
        return true;
    default:
        return false;
    }
}
#else
using SocketHandle = int;
constexpr SocketHandle InvalidSocketHandle = -1;

int GetLastSocketError() {
    return errno;
}

void CloseSocketHandle(SocketHandle handle) {
    close(handle);
}

void DisableUdpConnectionReset(SocketHandle) {}

bool IsRecoverableReceiveError(int error) {
    switch (error) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EINTR:
    case ECONNREFUSED:
    case EMSGSIZE:
        return true;
    default:
        return false;
    }
}
#endif

} // Anonymous namespace

struct Client::Impl {
    SocketHandle handle{InvalidSocketHandle};
    sockaddr_in relay{};
};

Client::Client(Configuration configuration_)
    : configuration{std::move(configuration_)},
      diagnostics{configuration.Describe()}, impl{std::make_unique<Impl>()} {
    for (auto& fragment : fragments) {
        fragment.buffer.resize(Relay::MaxIpv4PacketSize * 4);
    }
}

Client::~Client() {
    Stop();
}

bool Client::Open() {
    impl->relay.sin_family = AF_INET;
    impl->relay.sin_port = htons(configuration.relay_port);

    if (inet_pton(AF_INET, configuration.relay_host.c_str(), &impl->relay.sin_addr) != 1) {
        LOG_ERROR(Network_LanPlay, "\"{}\" is not a usable relay address.",
                  configuration.relay_host);

        return false;
    }

    impl->handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (impl->handle == InvalidSocketHandle) {
        LOG_ERROR(Network_LanPlay, "could not create the relay socket (error {}).",
                  GetLastSocketError());

        return false;
    }

    DisableUdpConnectionReset(impl->handle);

#ifdef _WIN32
    DWORD timeout = ReceiveTimeoutMs;
    setsockopt(impl->handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = ReceiveTimeoutMs / 1000;
    timeout.tv_usec = (ReceiveTimeoutMs % 1000) * 1000;
    setsockopt(impl->handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;

    if (bind(impl->handle, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        LOG_ERROR(Network_LanPlay, "could not bind the relay socket (error {}).",
                  GetLastSocketError());

        CloseSocketHandle(impl->handle);
        impl->handle = InvalidSocketHandle;

        return false;
    }

    return true;
}

void Client::Start() {
    if (is_running.load(std::memory_order_relaxed) || impl->handle == InvalidSocketHandle) {
        return;
    }

    is_running.store(true, std::memory_order_relaxed);
    stop_requested.store(false, std::memory_order_relaxed);

    receive_thread = std::thread{[this] { ReceiveLoop(); }};
    keep_alive_thread = std::thread{[this] { KeepAliveLoop(); }};

    LOG_INFO(Network_LanPlay,
             "connected to relay {}. Enable the Network.LanPlay debug log for a line per packet.",
             configuration.Describe());
}

void Client::Stop() {
    if (!is_running.exchange(false, std::memory_order_relaxed)) {
        // Still make sure a socket opened but never started is released.
        if (impl->handle != InvalidSocketHandle) {
            CloseSocketHandle(impl->handle);
            impl->handle = InvalidSocketHandle;
        }

        return;
    }

    stop_requested.store(true, std::memory_order_relaxed);

    diagnostics.LogFinalReport();

    if (impl->handle != InvalidSocketHandle) {
        CloseSocketHandle(impl->handle);
        impl->handle = InvalidSocketHandle;
    }

    // Both loops wake up at least once per receive timeout, so this returns quickly on every
    // platform rather than depending on close() aborting a blocking recvfrom.
    if (receive_thread.joinable()) {
        receive_thread.join();
    }

    if (keep_alive_thread.joinable()) {
        keep_alive_thread.join();
    }

    {
        std::scoped_lock lock{handler_mutex};

        handlers.clear();
    }

    {
        std::scoped_lock lock{fragment_mutex};

        for (auto& fragment : fragments) {
            fragment.in_use = false;
        }
    }
}

std::size_t Client::AddIpv4Handler(Ipv4Handler handler) {
    std::scoped_lock lock{handler_mutex};

    const std::size_t token = next_handler_token++;

    handlers.emplace_back(token, std::move(handler));

    return token;
}

void Client::SetInfoHandler(InfoHandler handler) {
    std::scoped_lock lock{handler_mutex};

    info_handler = std::move(handler);
}

void Client::RemoveIpv4Handler(std::size_t token) {
    std::scoped_lock lock{handler_mutex};

    std::erase_if(handlers, [token](const auto& entry) { return entry.first == token; });
}

void Client::SendIpv4(std::span<const u8> packet) {
    if (!IsRunning() || packet.size() < Ipv4::MinHeaderSize) {
        return;
    }

    if (configuration.path_mtu > 0 && packet.size() > configuration.path_mtu) {
        SendFragmented(packet, configuration.path_mtu);

        return;
    }

    Send(Relay::PacketType::Ipv4, packet);
}

void Client::SendFragmented(std::span<const u8> packet, std::size_t path_mtu) {
    const std::size_t total_parts = (packet.size() + path_mtu - 1) / path_mtu;

    if (total_parts > Relay::MaxFragmentParts) {
        LOG_WARNING(Network_LanPlay,
                    "dropping a {} byte packet, too large for the configured path MTU.",
                    packet.size());

        diagnostics.Dropped(DropReason::RelayFragment,
                            fmt::format("{} bytes needs {} fragments", packet.size(), total_parts));

        return;
    }

    Relay::FragmentHeader header{};
    header.source = ReadBE32(packet.subspan(Ipv4::SourceOffset));
    header.destination = ReadBE32(packet.subspan(Ipv4::DestinationOffset));
    header.total_parts = static_cast<u8>(total_parts);
    header.path_mtu = static_cast<u16>(path_mtu);

    {
        std::scoped_lock lock{send_mutex};

        header.id = fragment_id++;
    }

    std::vector<u8> buffer(Relay::FragmentHeaderSize + path_mtu);

    for (std::size_t part = 0; part < total_parts; part++) {
        const std::size_t offset = part * path_mtu;
        const std::size_t length = std::min(path_mtu, packet.size() - offset);

        header.part = static_cast<u8>(part);
        header.length = static_cast<u16>(length);

        Relay::WriteFragmentHeader(buffer, header);
        std::memcpy(buffer.data() + Relay::FragmentHeaderSize, packet.data() + offset, length);

        Send(Relay::PacketType::Ipv4Fragment,
             std::span{buffer}.first(Relay::FragmentHeaderSize + length));
    }
}

void Client::Send(Relay::PacketType type, std::span<const u8> payload) {
    if (impl->handle == InvalidSocketHandle) {
        return;
    }

    std::vector<u8> datagram(payload.size() + 1);

    datagram[0] = static_cast<u8>(type);

    if (!payload.empty()) {
        std::memcpy(datagram.data() + 1, payload.data(), payload.size());
    }

    ssize_t sent = 0;

    {
        std::scoped_lock lock{send_mutex};

        sent = sendto(impl->handle, reinterpret_cast<const char*>(datagram.data()),
                      static_cast<int>(datagram.size()), 0,
                      reinterpret_cast<const sockaddr*>(&impl->relay), sizeof(impl->relay));
    }

    if (sent < 0) {
        const int error = GetLastSocketError();

        LOG_WARNING(Network_LanPlay, "failed to send {} bytes to the relay (error {}).",
                    datagram.size(), error);

        return;
    }

    diagnostics.RelayDatagramSent(type, datagram.size());
}

void Client::KeepAliveLoop() {
    u64 next_keep_alive = 0;

    while (!stop_requested.load(std::memory_order_relaxed)) {
        const u64 now = GetTickCountMs();

        if (now >= next_keep_alive) {
            next_keep_alive = now + Relay::KeepAliveIntervalMs;

            Send(Relay::PacketType::KeepAlive, {});
        }

        // Short sleeps so that a teardown is not held up by the keepalive interval.
        std::this_thread::sleep_for(std::chrono::milliseconds{ReceiveTimeoutMs});
    }
}

void Client::ReceiveLoop() {
    std::vector<u8> buffer(ReceiveBufferSize);

    while (!stop_requested.load(std::memory_order_relaxed)) {
        sockaddr_in remote{};
        socklen_t remote_length = sizeof(remote);

        const auto size = recvfrom(impl->handle, reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(buffer.size()), 0,
                                   reinterpret_cast<sockaddr*>(&remote), &remote_length);

        if (size < 0) {
            if (stop_requested.load(std::memory_order_relaxed)) {
                break;
            }

            const int error = GetLastSocketError();

            // A timeout is the normal case: it is what lets this loop notice the stop flag. An ICMP
            // error queued against the socket, a truncated datagram or a signal must not end the
            // session either, so that one bad moment cannot drop the player out of a game.
            if (IsRecoverableReceiveError(error)) {
                continue;
            }

            LOG_WARNING(Network_LanPlay, "relay socket error {}, stopping the receive loop.", error);

            break;
        }

        if (size < 1) {
            continue;
        }

        last_receive_time.store(GetTickCountMs(), std::memory_order_relaxed);

        HandleDatagram(std::span{buffer}.first(static_cast<std::size_t>(size)));
    }
}

void Client::HandleDatagram(std::span<const u8> datagram) {
    // The high bit marks an encrypted packet, which no known relay uses.
    const auto type = static_cast<Relay::PacketType>(datagram[0] & 0x7F);
    const auto payload = datagram.subspan(1);

    diagnostics.RelayDatagramReceived(type, datagram);

    switch (type) {
    case Relay::PacketType::KeepAlive:
    case Relay::PacketType::Ping:
        break;

    case Relay::PacketType::Ipv4:
        DeliverIpv4(payload);
        break;

    case Relay::PacketType::Ipv4Fragment:
        HandleFragment(payload);
        break;

    case Relay::PacketType::AuthMe:
        HandleAuthMe(payload);
        break;

    case Relay::PacketType::Info: {
        const std::string_view message{reinterpret_cast<const char*>(payload.data()),
                                       payload.size()};

        LOG_INFO(Network_LanPlay, "relay message: {}", message);

        InfoHandler handler;
        {
            std::scoped_lock lock{handler_mutex};
            handler = info_handler;
        }

        if (handler) {
            handler(message);
        }

        break;
    }

    default:
        LOG_DEBUG(Network_LanPlay, "ignoring unknown relay packet type 0x{:02x}",
                  static_cast<u8>(type));
        break;
    }
}

void Client::DeliverIpv4(std::span<const u8> packet) {
    if (packet.size() < Ipv4::MinHeaderSize) {
        diagnostics.Dropped(DropReason::Malformed,
                            fmt::format("{} byte relay payload", packet.size()));

        return;
    }

    std::vector<std::pair<std::size_t, Ipv4Handler>> current;

    {
        std::scoped_lock lock{handler_mutex};

        current = handlers;
    }

    for (const auto& [token, handler] : current) {
        handler(packet);
    }
}

void Client::HandleFragment(std::span<const u8> payload) {
    Relay::FragmentHeader header{};

    if (!Relay::TryReadFragmentHeader(payload, header)) {
        diagnostics.Dropped(DropReason::RelayFragment, "invalid fragment header");

        return;
    }

    std::vector<u8> completed;

    {
        std::scoped_lock lock{fragment_mutex};

        const u64 now = GetTickCountMs();

        Fragment* fragment = nullptr;
        Fragment* free_slot = nullptr;

        for (auto& candidate : fragments) {
            if (!candidate.in_use) {
                if (free_slot == nullptr) {
                    free_slot = &candidate;
                }

                continue;
            }

            if (now - candidate.timestamp > FragmentTimeoutMs) {
                candidate.in_use = false;

                if (free_slot == nullptr) {
                    free_slot = &candidate;
                }

                continue;
            }

            if (candidate.id == header.id && candidate.source == header.source) {
                fragment = &candidate;

                break;
            }
        }

        if (fragment == nullptr) {
            if (free_slot == nullptr) {
                diagnostics.Dropped(DropReason::RelayFragment, "fragment buffer is full");

                return;
            }

            fragment = free_slot;
            fragment->in_use = true;
            fragment->id = header.id;
            fragment->source = header.source;
            fragment->received_parts = 0;
            fragment->total_length = 0;
        }

        fragment->timestamp = now;

        const std::size_t offset = static_cast<std::size_t>(header.path_mtu) * header.part;

        if (offset + header.length > fragment->buffer.size()) {
            diagnostics.Dropped(DropReason::RelayFragment,
                                fmt::format("fragment {} of {} is out of range", header.part,
                                            header.total_parts));

            return;
        }

        std::memcpy(fragment->buffer.data() + offset,
                    payload.data() + Relay::FragmentHeaderSize, header.length);

        fragment->received_parts |= 1u << header.part;

        if (header.part == header.total_parts - 1) {
            fragment->total_length = offset + header.length;
        }

        const u32 complete_mask = header.total_parts == Relay::MaxFragmentParts
                                      ? ~0u
                                      : (1u << header.total_parts) - 1;

        if (fragment->received_parts == complete_mask && fragment->total_length != 0) {
            completed.assign(fragment->buffer.begin(),
                             fragment->buffer.begin() + fragment->total_length);

            fragment->in_use = false;
        }
    }

    if (!completed.empty()) {
        DeliverIpv4(completed);
    }
}

void Client::HandleAuthMe(std::span<const u8> payload) {
    if (payload.empty()) {
        return;
    }

    if (configuration.user_name.empty() || configuration.password_hash.empty()) {
        LOG_WARNING(Network_LanPlay,
                    "the relay requires a user name and password. Set them in the LAN Play server "
                    "field as user:password@host:port.");

        return;
    }

    const u8 auth_type = payload[0];

    if (auth_type != 0) {
        LOG_WARNING(Network_LanPlay, "unsupported relay authentication type {}.", auth_type);

        return;
    }

    const auto response = Relay::BuildAuthResponse(configuration.password_hash,
                                                      payload.subspan(1), configuration.user_name);

    Send(Relay::PacketType::AuthMe, response);
}

} // namespace Network::LanPlay
