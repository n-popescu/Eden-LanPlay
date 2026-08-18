// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Network::LanPlay {

class NetworkInterface;

enum class TcpState {
    Closed,
    SynSent,
    SynReceived,
    Established,
    FinWait1,
    FinWait2,
    CloseWait,
    LastAck,
    TimeWait,
};

/// Result of a receive attempt, so callers do not need exceptions to tell these cases apart.
enum class TcpReceiveStatus {
    Ok,        ///< Bytes were copied out.
    Closed,    ///< The peer closed its side; a guest read should report 0 bytes.
    WouldBlock,
    TimedOut,
    Reset,
};

/**
 * A TCP connection carried over the LAN Play relay.
 *
 * Only what LAN traffic between consoles needs is implemented: the three way handshake in both
 * directions, in order delivery with cumulative acknowledgements, a single retransmission timer, and
 * orderly or abortive close. Out of order segments are dropped and recovered by the sender's
 * retransmission, which is acceptable on a relayed LAN but is not a general purpose TCP.
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    static constexpr std::size_t HeaderSize = 20;

    using DataHandler = std::function<void(std::span<const u8>)>;
    using StateHandler = std::function<void()>;

    /**
     * Connections are always owned through a shared_ptr: the virtual interface keeps a reference
     * while the connection is registered, and the receive thread may be inside HandleSegment while
     * the owner closes it.
     */
    static std::shared_ptr<TcpConnection> Create(NetworkInterface& network_interface, u16 local_port,
                                                 u32 remote_address, u16 remote_port);

    TcpConnection(NetworkInterface& network_interface_, u16 local_port_, u32 remote_address_,
                  u16 remote_port_);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    [[nodiscard]] u16 GetLocalPort() const {
        return local_port;
    }

    [[nodiscard]] u32 GetRemoteAddress() const {
        return remote_address;
    }

    [[nodiscard]] u16 GetRemotePort() const {
        return remote_port;
    }

    [[nodiscard]] TcpState GetState() const;

    [[nodiscard]] bool IsConnected() const;

    /// True once the peer has closed its side of the connection, or the connection is gone.
    [[nodiscard]] bool IsRemoteClosed() const;

    [[nodiscard]] bool IsAborted() const;

    [[nodiscard]] std::size_t GetAvailable() const;

    [[nodiscard]] bool IsReadable() const;

    [[nodiscard]] bool IsWritable() const {
        return IsConnected();
    }

    [[nodiscard]] std::string Describe() const;

    /**
     * When set, received payloads are handed to this callback instead of being buffered for
     * Receive(). Used by the LDN discovery sockets, which are event driven.
     */
    void SetDataHandler(DataHandler handler);

    /// Called once when the handshake completes.
    void SetEstablishedHandler(StateHandler handler);

    /// Called once when the connection is closed or aborted.
    void SetClosedHandler(StateHandler handler);

    /// Performs an active open. Returns false if the peer never answered or refused the connection.
    bool Connect(int timeout_milliseconds);

    /// Completes a passive open: answers an incoming SYN with SYN|ACK.
    void AcceptSyn(u32 remote_sequence);

    /// Queues data for transmission. Returns the number of bytes accepted, or -1 on error.
    s32 Send(std::span<const u8> data);

    /// Copies received bytes out, waiting up to the timeout for data to arrive.
    TcpReceiveStatus Receive(std::span<u8> buffer, int timeout_milliseconds, bool peek,
                             bool blocking, std::size_t& out_read);

    /// Orderly close: sends FIN after everything that was queued and lets the peer finish.
    void Close();

    /// Abortive close: sends RST (if the connection was live) and forgets everything.
    void Abort();

    /// Called by the virtual interface for every segment belonging to this connection.
    void HandleSegment(std::span<const u8> segment);

    /// Called periodically by the virtual interface to drive retransmissions and TIME_WAIT.
    void Tick();

    /// Answers a segment that belongs to no connection, the way a real stack refuses one.
    static void SendReset(NetworkInterface& network_interface, u16 local_port, u32 remote_address,
                          u16 remote_port, std::span<const u8> incoming);

private:
    static constexpr u8 FlagFin = 0x01;
    static constexpr u8 FlagSyn = 0x02;
    static constexpr u8 FlagRst = 0x04;
    static constexpr u8 FlagPsh = 0x08;
    static constexpr u8 FlagAck = 0x10;

    static constexpr std::size_t MaxSegmentSize = 1400;
    static constexpr std::size_t ReceiveBufferSize = 64 * 1024;
    static constexpr std::size_t SendBufferSize = 64 * 1024;

    static constexpr u64 InitialRetransmitTimeoutMs = 300;
    static constexpr u64 MaxRetransmitTimeoutMs = 4000;
    static constexpr int MaxRetransmits = 10;
    static constexpr u64 TimeWaitMs = 2000;

    static bool SeqLeq(u32 a, u32 b) {
        return static_cast<s32>(a - b) <= 0;
    }

    static bool SeqGt(u32 a, u32 b) {
        return static_cast<s32>(a - b) > 0;
    }

    std::size_t ReadFromBufferLocked(std::span<u8> buffer, bool peek);
    void HandleAcknowledgementLocked(u32 acknowledgement);
    void ResetRetransmitTimerLocked();
    void TransmitPendingLocked();
    void SendSegment(u8 flags, std::span<const u8> payload);
    void SendSegmentLocked(u8 flags, std::span<const u8> payload, u32 sequence);
    void Cleanup();

    NetworkInterface& network_interface;

    const u16 local_port;
    const u32 remote_address;
    const u16 remote_port;

    mutable std::mutex mutex;
    std::condition_variable state_changed;
    std::condition_variable data_ready;

    std::vector<u8> receive_buffer;
    std::size_t receive_head{};
    std::size_t receive_length{};

    std::deque<u8> send_buffer;

    u32 snd_una{};
    u32 snd_nxt{};
    u32 rcv_nxt{};
    bool fin_sent{};
    bool fin_acked{};

    u64 retransmit_deadline{};
    u64 retransmit_timeout{InitialRetransmitTimeoutMs};
    int retransmit_count{};
    u64 time_wait_deadline{};

    TcpState state{TcpState::Closed};
    bool remote_closed{};
    bool aborted{};

    std::mutex handler_mutex;
    DataHandler data_handler;
    StateHandler established_handler;
    StateHandler closed_handler;
};

} // namespace Network::LanPlay
