// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "core/internal_network/lan_play/lan_play_socket.h"
#include "core/internal_network/lan_play/lan_play_tcp_connection.h"
#include "core/internal_network/lan_play/lan_play_tcp_listener.h"
#include "core/internal_network/lan_play/lan_play_udp_endpoint.h"
#include "core/internal_network/network.h"

namespace Network::LanPlay {

namespace {

constexpr int DefaultBlockingTimeoutMs = 5000;

SockAddrIn MakeSockAddr(u32 address, u16 port) {
    SockAddrIn addr{};

    addr.family = Domain::INET;
    addr.ip = {static_cast<u8>(address >> 24), static_cast<u8>(address >> 16),
               static_cast<u8>(address >> 8), static_cast<u8>(address)};
    addr.portno = port;

    return addr;
}

} // Anonymous namespace

LanPlaySocket::LanPlaySocket(std::shared_ptr<Stack> stack_) : stack{std::move(stack_)} {}

LanPlaySocket::~LanPlaySocket() {
    LanPlaySocket::Close();
}

Errno LanPlaySocket::Initialize(Domain domain_, Type type_, Protocol protocol_) {
    domain = domain_;
    type = type_;
    protocol = protocol_;

    return Errno::SUCCESS;
}

bool LanPlaySocket::IsLanPlayAddress(const SockAddrIn& addr) const {
    return NetworkInterface::IsHandledAddress(IPv4AddressToInteger(addr.ip));
}

void LanPlaySocket::MarkGuestActive(std::string_view reason) {
    stack->MarkGuestActive(reason);
}

int LanPlaySocket::GetReceiveTimeout() const {
    if (!blocking) {
        return 0;
    }

    return receive_timeout != 0 ? static_cast<int>(receive_timeout) : DefaultBlockingTimeoutMs;
}

SocketBase* LanPlaySocket::GetHostSocket() {
    if (host_socket) {
        return host_socket.get();
    }

    host_socket = std::make_unique<Socket>();

    if (host_socket->Initialize(domain, type, protocol) != Errno::SUCCESS) {
        LOG_ERROR(Network_LanPlay, "could not create the host socket for non-LAN-Play traffic.");

        host_socket.reset();

        return nullptr;
    }

    // Everything the guest set on us before the fallback existed has to be replayed, or the host
    // socket behaves differently from the one the game thinks it configured.
    if (pending_options.linger) {
        host_socket->SetLinger(pending_options.linger->first, pending_options.linger->second);
    }

    if (pending_options.reuse_addr) {
        host_socket->SetReuseAddr(*pending_options.reuse_addr);
    }

    if (pending_options.keep_alive) {
        host_socket->SetKeepAlive(*pending_options.keep_alive);
    }

    if (pending_options.broadcast) {
        host_socket->SetBroadcast(*pending_options.broadcast);
    }

    if (pending_options.send_buffer) {
        host_socket->SetSndBuf(*pending_options.send_buffer);
    }

    if (pending_options.receive_buffer) {
        host_socket->SetRcvBuf(*pending_options.receive_buffer);
    }

    if (pending_options.send_timeout) {
        host_socket->SetSndTimeo(*pending_options.send_timeout);
    }

    if (pending_options.receive_timeout) {
        host_socket->SetRcvTimeo(*pending_options.receive_timeout);
    }

    if (pending_options.non_block) {
        host_socket->SetNonBlock(*pending_options.non_block);
    }

    // A UDP socket the guest bound on us has to be bound on the host as well, or its replies would
    // arrive on an unrelated ephemeral port.
    if (bound && type == Type::DGRAM) {
        host_socket->Bind(MakeSockAddr(0, local_port));
    }

    if (!host_fallback_logged) {
        host_fallback_logged = true;

        LOG_INFO(Network_LanPlay,
                 "a socket is using the host network for traffic that is not addressed to the LAN "
                 "Play network; online features keep working normally.");
    }

    return host_socket.get();
}

Errno LanPlaySocket::Close() {
    if (closed) {
        return Errno::SUCCESS;
    }

    closed = true;

    if (udp_endpoint) {
        udp_endpoint->Close();
        udp_endpoint.reset();
    }

    if (tcp_connection) {
        tcp_connection->Close();
        tcp_connection.reset();
    }

    if (tcp_listener) {
        tcp_listener->Close();
        tcp_listener.reset();
    }

    if (host_socket) {
        host_socket->Close();
        host_socket.reset();
    }

    return Errno::SUCCESS;
}

Errno LanPlaySocket::Bind(SockAddrIn addr) {
    local_port = addr.portno;
    bound = true;

    if (type == Type::DGRAM) {
        udp_endpoint = stack->GetNetworkInterface().BindUdp(local_port);

        if (!udp_endpoint) {
            return Errno::INVAL;
        }

        // A guest that bound port 0 wants to know which port it actually got.
        local_port = udp_endpoint->GetLocalPort();
    }

    return Errno::SUCCESS;
}

Errno LanPlaySocket::Listen(s32 backlog) {
    if (type != Type::STREAM) {
        return Errno::INVAL;
    }

    tcp_listener = stack->GetNetworkInterface().ListenTcp(local_port);

    if (!tcp_listener) {
        return Errno::INVAL;
    }

    local_port = tcp_listener->GetLocalPort();
    listening = true;

    MarkGuestActive("a game is listening on the LAN Play network");

    return Errno::SUCCESS;
}

std::pair<SocketBase::AcceptResult, Errno> LanPlaySocket::Accept() {
    if (!tcp_listener) {
        return {AcceptResult{}, Errno::INVAL};
    }

    auto connection = tcp_listener->Accept(blocking ? GetReceiveTimeout() : 0);

    if (!connection) {
        return {AcceptResult{}, Errno::AGAIN};
    }

    auto accepted = std::make_unique<LanPlaySocket>(stack);

    accepted->Initialize(domain, type, protocol);
    accepted->tcp_connection = connection;
    accepted->local_port = connection->GetLocalPort();
    accepted->bound = true;

    AcceptResult result;

    result.sockaddr_in = MakeSockAddr(connection->GetRemoteAddress(), connection->GetRemotePort());
    result.socket = std::move(accepted);

    return {std::move(result), Errno::SUCCESS};
}

Errno LanPlaySocket::Connect(SockAddrIn addr_in) {
    if (!IsLanPlayAddress(addr_in)) {
        // Not our network: hand the socket over to the host stack for the rest of its life.
        auto* host = GetHostSocket();

        return host != nullptr ? host->Connect(addr_in) : Errno::OTHER;
    }

    MarkGuestActive("a game connected to an address on the LAN Play network");

    const u32 remote_address = IPv4AddressToInteger(addr_in.ip);

    if (type == Type::DGRAM) {
        // A connected UDP socket only needs a bound endpoint; the destination is remembered by the
        // caller through Send(), which we translate into a SendTo().
        if (!udp_endpoint) {
            udp_endpoint = stack->GetNetworkInterface().BindUdp(local_port);

            if (!udp_endpoint) {
                return Errno::INVAL;
            }

            local_port = udp_endpoint->GetLocalPort();
            bound = true;
        }

        connected_address = remote_address;
        connected_port = addr_in.portno;

        return Errno::SUCCESS;
    }

    auto& network_interface = stack->GetNetworkInterface();

    tcp_connection = TcpConnection::Create(
        network_interface, bound && local_port != 0 ? local_port : network_interface.AllocateTcpPort(),
        remote_address, addr_in.portno);

    local_port = tcp_connection->GetLocalPort();

    if (!tcp_connection->Connect(blocking ? DefaultBlockingTimeoutMs : 0)) {
        tcp_connection.reset();

        return blocking ? Errno::TIMEDOUT : Errno::INPROGRESS;
    }

    return Errno::SUCCESS;
}

std::pair<SockAddrIn, Errno> LanPlaySocket::GetPeerName() {
    if (tcp_connection) {
        return {MakeSockAddr(tcp_connection->GetRemoteAddress(), tcp_connection->GetRemotePort()),
                Errno::SUCCESS};
    }

    if (connected_address != 0) {
        return {MakeSockAddr(connected_address, connected_port), Errno::SUCCESS};
    }

    if (host_socket) {
        return host_socket->GetPeerName();
    }

    return {SockAddrIn{}, Errno::NOTCONN};
}

std::pair<SockAddrIn, Errno> LanPlaySocket::GetSockName() {
    return {MakeSockAddr(stack->GetAddress(), local_port), Errno::SUCCESS};
}

Errno LanPlaySocket::Shutdown(ShutdownHow how) {
    if (tcp_connection) {
        tcp_connection->Close();

        return Errno::SUCCESS;
    }

    if (host_socket) {
        return host_socket->Shutdown(how);
    }

    return Errno::SUCCESS;
}

std::pair<s32, Errno> LanPlaySocket::Recv(int flags, std::span<u8> message) {
    return RecvFrom(flags, message, nullptr);
}

std::pair<s32, Errno> LanPlaySocket::RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) {
    const bool peek = (static_cast<u32>(flags) & FLAG_MSG_PEEK) != 0;
    const bool dont_wait = (static_cast<u32>(flags) & FLAG_MSG_DONTWAIT) != 0;
    const bool should_block = blocking && !dont_wait;

    if (tcp_connection) {
        std::size_t read = 0;

        const auto status = tcp_connection->Receive(message, GetReceiveTimeout(), peek,
                                                    should_block, read);

        switch (status) {
        case TcpReceiveStatus::Ok:
            if (addr != nullptr) {
                *addr = MakeSockAddr(tcp_connection->GetRemoteAddress(),
                                     tcp_connection->GetRemotePort());
            }

            return {static_cast<s32>(read), Errno::SUCCESS};

        case TcpReceiveStatus::Closed:
            return {0, Errno::SUCCESS};

        case TcpReceiveStatus::WouldBlock:
        case TcpReceiveStatus::TimedOut:
            return {-1, Errno::AGAIN};

        case TcpReceiveStatus::Reset:
            return {-1, Errno::CONNRESET};
        }

        return {-1, Errno::OTHER};
    }

    if (udp_endpoint) {
        if (should_block && !udp_endpoint->IsReadable()) {
            udp_endpoint->WaitForData(GetReceiveTimeout());
        }

        UdpEndpoint::Datagram datagram;

        const bool available =
            peek ? udp_endpoint->TryPeek(datagram) : udp_endpoint->TryDequeue(datagram);

        if (!available) {
            // Nothing on the virtual interface. A socket that also has a host fallback may have host
            // traffic waiting, so give it a chance rather than reporting the socket as dead.
            if (host_socket) {
                return host_socket->RecvFrom(flags, message, addr);
            }

            return {-1, Errno::AGAIN};
        }

        const std::size_t length = std::min(message.size(), datagram.data.size());

        std::memcpy(message.data(), datagram.data.data(), length);

        if (addr != nullptr) {
            *addr = MakeSockAddr(datagram.source_address, datagram.source_port);
        }

        return {static_cast<s32>(length), Errno::SUCCESS};
    }

    if (host_socket) {
        return host_socket->RecvFrom(flags, message, addr);
    }

    return {-1, Errno::NOTCONN};
}

std::pair<s32, Errno> LanPlaySocket::Send(std::span<const u8> message, int flags) {
    if (tcp_connection) {
        const s32 sent = tcp_connection->Send(message);

        if (sent < 0) {
            return {-1, tcp_connection->IsAborted() ? Errno::CONNRESET : Errno::NOTCONN};
        }

        if (sent == 0 && !message.empty()) {
            return {-1, Errno::AGAIN};
        }

        return {sent, Errno::SUCCESS};
    }

    if (udp_endpoint && connected_address != 0) {
        const SockAddrIn addr = MakeSockAddr(connected_address, connected_port);

        return SendTo(static_cast<u32>(flags), message, &addr);
    }

    if (host_socket) {
        return host_socket->Send(message, flags);
    }

    return {-1, Errno::NOTCONN};
}

std::pair<s32, Errno> LanPlaySocket::SendTo(u32 flags, std::span<const u8> message,
                                            const SockAddrIn* addr) {
    if (addr == nullptr) {
        return Send(message, static_cast<int>(flags));
    }

    if (!IsLanPlayAddress(*addr)) {
        auto* host = GetHostSocket();

        return host != nullptr ? host->SendTo(flags, message, addr) : std::pair{-1, Errno::OTHER};
    }

    // A game broadcasting on the network it was told it is on is entering its own LAN mode, which is
    // what marks the LAN Play network as in use. Scanning from a menu does not get here, because the
    // LDN service has its own path.
    MarkGuestActive("a game sent traffic on the LAN Play network");

    if (!udp_endpoint) {
        udp_endpoint = stack->GetNetworkInterface().BindUdp(local_port);

        if (!udp_endpoint) {
            return {-1, Errno::INVAL};
        }

        local_port = udp_endpoint->GetLocalPort();
        bound = true;
    }

    const std::size_t sent =
        udp_endpoint->SendTo(IPv4AddressToInteger(addr->ip), addr->portno, message);

    return {static_cast<s32>(sent), Errno::SUCCESS};
}

Errno LanPlaySocket::SetLinger(bool enable, u32 linger) {
    pending_options.linger = std::pair{enable, linger};

    return host_socket ? host_socket->SetLinger(enable, linger) : Errno::SUCCESS;
}

Errno LanPlaySocket::SetReuseAddr(bool enable) {
    pending_options.reuse_addr = enable;

    return host_socket ? host_socket->SetReuseAddr(enable) : Errno::SUCCESS;
}

Errno LanPlaySocket::SetKeepAlive(bool enable) {
    pending_options.keep_alive = enable;

    return host_socket ? host_socket->SetKeepAlive(enable) : Errno::SUCCESS;
}

Errno LanPlaySocket::SetBroadcast(bool enable) {
    pending_options.broadcast = enable;

    return host_socket ? host_socket->SetBroadcast(enable) : Errno::SUCCESS;
}

Errno LanPlaySocket::SetSndBuf(u32 value) {
    pending_options.send_buffer = value;

    return host_socket ? host_socket->SetSndBuf(value) : Errno::SUCCESS;
}

Errno LanPlaySocket::SetRcvBuf(u32 value) {
    pending_options.receive_buffer = value;

    return host_socket ? host_socket->SetRcvBuf(value) : Errno::SUCCESS;
}

Errno LanPlaySocket::SetSndTimeo(u32 value) {
    pending_options.send_timeout = value;
    send_timeout = value;

    return host_socket ? host_socket->SetSndTimeo(value) : Errno::SUCCESS;
}

Errno LanPlaySocket::SetRcvTimeo(u32 value) {
    pending_options.receive_timeout = value;
    receive_timeout = value;

    return host_socket ? host_socket->SetRcvTimeo(value) : Errno::SUCCESS;
}

Errno LanPlaySocket::SetNonBlock(bool enable) {
    pending_options.non_block = enable;
    blocking = !enable;

    return host_socket ? host_socket->SetNonBlock(enable) : Errno::SUCCESS;
}

std::pair<Errno, Errno> LanPlaySocket::GetPendingError() {
    if (tcp_connection && tcp_connection->IsAborted()) {
        return {Errno::SUCCESS, Errno::CONNRESET};
    }

    if (host_socket) {
        return host_socket->GetPendingError();
    }

    return {Errno::SUCCESS, Errno::SUCCESS};
}

bool LanPlaySocket::IsOpened() const {
    return !closed;
}

void LanPlaySocket::HandleProxyPacket(const ProxyPacket&) {
    // LAN Play does not use the multiplayer room's proxy packets; its traffic is real IPv4 on the
    // relay instead.
}

bool LanPlaySocket::IsReadable() {
    if (tcp_connection) {
        return tcp_connection->IsReadable();
    }

    if (tcp_listener) {
        return tcp_listener->IsReadable();
    }

    if (udp_endpoint && udp_endpoint->IsReadable()) {
        return true;
    }

    return false;
}

bool LanPlaySocket::IsWritable() {
    if (tcp_connection) {
        return tcp_connection->IsWritable();
    }

    return true;
}

} // namespace Network::LanPlay
