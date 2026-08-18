// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "core/internal_network/lan_play/lan_play_stack.h"
#include "core/internal_network/sockets.h"

namespace Network::LanPlay {

class TcpConnection;
class TcpListener;
class UdpEndpoint;

/**
 * A socket of the emulated console carried by the LAN Play virtual network interface.
 *
 * Only traffic addressed to the LAN Play network goes to the relay. Everything else — the online
 * service, the DNS MITM, the NAT check — is handed to a host socket created on demand, so selecting
 * LAN Play does not take the console off the internet. Options the game set before that fallback
 * existed are replayed on it, SO_BROADCAST included, without which a broadcast fails with EACCES on
 * Linux and macOS.
 */
class LanPlaySocket final : public SocketBase {
public:
    explicit LanPlaySocket(std::shared_ptr<Stack> stack_);
    ~LanPlaySocket() override;

    Errno Initialize(Domain domain, Type type, Protocol protocol) override;

    Errno Close() override;

    std::pair<AcceptResult, Errno> Accept() override;

    Errno Connect(SockAddrIn addr_in) override;

    std::pair<SockAddrIn, Errno> GetPeerName() override;

    std::pair<SockAddrIn, Errno> GetSockName() override;

    Errno Bind(SockAddrIn addr) override;

    Errno Listen(s32 backlog) override;

    Errno Shutdown(ShutdownHow how) override;

    std::pair<s32, Errno> Recv(int flags, std::span<u8> message) override;

    std::pair<s32, Errno> RecvFrom(int flags, std::span<u8> message, SockAddrIn* addr) override;

    std::pair<s32, Errno> Send(std::span<const u8> message, int flags) override;

    std::pair<s32, Errno> SendTo(u32 flags, std::span<const u8> message,
                                 const SockAddrIn* addr) override;

    Errno SetLinger(bool enable, u32 linger) override;

    Errno SetReuseAddr(bool enable) override;

    Errno SetKeepAlive(bool enable) override;

    Errno SetBroadcast(bool enable) override;

    Errno SetSndBuf(u32 value) override;

    Errno SetRcvBuf(u32 value) override;

    Errno SetSndTimeo(u32 value) override;

    Errno SetRcvTimeo(u32 value) override;

    Errno SetNonBlock(bool enable) override;

    std::pair<Errno, Errno> GetPendingError() override;

    bool IsOpened() const override;

    void HandleProxyPacket(const ProxyPacket& packet) override;

    LanPlaySocket* AsLanPlaySocket() override {
        return this;
    }

    /// True when a guest poll or select should report this socket as readable.
    [[nodiscard]] bool IsReadable();

    /// True when a guest poll or select should report this socket as writable.
    [[nodiscard]] bool IsWritable();

private:
    /// Socket options a guest may set before we know whether the socket needs a host fallback.
    struct PendingOptions {
        std::optional<std::pair<bool, u32>> linger;
        std::optional<bool> reuse_addr;
        std::optional<bool> keep_alive;
        std::optional<bool> broadcast;
        std::optional<u32> send_buffer;
        std::optional<u32> receive_buffer;
        std::optional<u32> send_timeout;
        std::optional<u32> receive_timeout;
        std::optional<bool> non_block;
    };

    /// Lazily creates the host socket and replays every option the guest already set on us.
    SocketBase* GetHostSocket();

    /// True when this address belongs on the LAN Play network rather than on the host network.
    [[nodiscard]] bool IsLanPlayAddress(const SockAddrIn& addr) const;

    void MarkGuestActive(std::string_view reason);

    [[nodiscard]] int GetReceiveTimeout() const;

    std::shared_ptr<Stack> stack;

    Domain domain{Domain::INET};
    Type type{Type::DGRAM};
    Protocol protocol{Protocol::UDP};

    bool closed{false};
    bool blocking{true};
    bool bound{false};
    bool listening{false};

    u16 local_port{};

    /// Destination remembered by Connect() on a UDP socket, so Send() can behave like SendTo().
    u32 connected_address{};
    u16 connected_port{};

    u32 send_timeout{};
    u32 receive_timeout{};

    PendingOptions pending_options;

    /// Set for a UDP socket bound on the virtual interface.
    std::shared_ptr<UdpEndpoint> udp_endpoint;

    /// Set for a TCP socket that is connected or accepted on the virtual interface.
    std::shared_ptr<TcpConnection> tcp_connection;

    /// Set for a TCP socket listening on the virtual interface.
    std::shared_ptr<TcpListener> tcp_listener;

    /// The host socket used for everything that is not LAN Play traffic.
    std::unique_ptr<Socket> host_socket;
    bool host_fallback_logged{false};
};

} // namespace Network::LanPlay
