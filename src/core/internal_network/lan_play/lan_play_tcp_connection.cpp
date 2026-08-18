// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/internal_network/lan_play/ipv4_packet.h"
#include "core/internal_network/lan_play/lan_play_network_interface.h"
#include "core/internal_network/lan_play/lan_play_tcp_connection.h"

namespace Network::LanPlay {

namespace {

u32 RandomSequenceNumber() {
    static thread_local std::mt19937 engine{std::random_device{}()};

    return std::uniform_int_distribution<u32>{}(engine);
}

} // Anonymous namespace

std::shared_ptr<TcpConnection> TcpConnection::Create(NetworkInterface& network_interface,
                                                    u16 local_port, u32 remote_address,
                                                    u16 remote_port) {
    return std::make_shared<TcpConnection>(network_interface, local_port, remote_address,
                                          remote_port);
}

TcpConnection::TcpConnection(NetworkInterface& network_interface_, u16 local_port_,
                             u32 remote_address_, u16 remote_port_)
    : network_interface{network_interface_}, local_port{local_port_},
      remote_address{remote_address_}, remote_port{remote_port_} {
    receive_buffer.resize(ReceiveBufferSize);

    snd_una = snd_nxt = RandomSequenceNumber();
}

TcpConnection::~TcpConnection() = default;

TcpState TcpConnection::GetState() const {
    std::scoped_lock lock{mutex};

    return state;
}

bool TcpConnection::IsConnected() const {
    std::scoped_lock lock{mutex};

    return state == TcpState::Established || state == TcpState::CloseWait;
}

bool TcpConnection::IsRemoteClosed() const {
    std::scoped_lock lock{mutex};

    return remote_closed;
}

bool TcpConnection::IsAborted() const {
    std::scoped_lock lock{mutex};

    return aborted;
}

std::size_t TcpConnection::GetAvailable() const {
    std::scoped_lock lock{mutex};

    return receive_length;
}

bool TcpConnection::IsReadable() const {
    std::scoped_lock lock{mutex};

    return receive_length > 0 || remote_closed || aborted;
}

std::string TcpConnection::Describe() const {
    return fmt::format("{}:{}", FormatAddress(remote_address), remote_port);
}

void TcpConnection::SetDataHandler(DataHandler handler) {
    std::scoped_lock lock{handler_mutex};

    data_handler = std::move(handler);
}

void TcpConnection::SetEstablishedHandler(StateHandler handler) {
    std::scoped_lock lock{handler_mutex};

    established_handler = std::move(handler);
}

void TcpConnection::SetClosedHandler(StateHandler handler) {
    std::scoped_lock lock{handler_mutex};

    closed_handler = std::move(handler);
}

bool TcpConnection::Connect(int timeout_milliseconds) {
    {
        std::scoped_lock lock{mutex};

        if (state != TcpState::Closed) {
            return false;
        }

        state = TcpState::SynSent;
    }

    network_interface.RegisterConnection(shared_from_this());

    SendSegment(FlagSyn, {});

    const u64 deadline =
        GetTickCountMs() + (timeout_milliseconds < 0 ? 10000 : static_cast<u64>(timeout_milliseconds));

    while (GetTickCountMs() < deadline) {
        {
            std::unique_lock lock{mutex};

            if (state == TcpState::Established) {
                return true;
            }

            if (aborted || state == TcpState::Closed) {
                return false;
            }

            state_changed.wait_for(lock, std::chrono::milliseconds{50});

            if (state == TcpState::Established) {
                return true;
            }

            if (aborted || state == TcpState::Closed) {
                return false;
            }
        }
    }

    Abort();

    return false;
}

void TcpConnection::AcceptSyn(u32 remote_sequence) {
    {
        std::scoped_lock lock{mutex};

        rcv_nxt = remote_sequence + 1;
        state = TcpState::SynReceived;
    }

    network_interface.RegisterConnection(shared_from_this());

    SendSegment(FlagSyn | FlagAck, {});
}

s32 TcpConnection::Send(std::span<const u8> data) {
    std::scoped_lock lock{mutex};

    if (aborted) {
        return -1;
    }

    if (state != TcpState::Established && state != TcpState::CloseWait) {
        return -1;
    }

    if (send_buffer.size() >= SendBufferSize) {
        return 0;
    }

    const std::size_t length = std::min(SendBufferSize - send_buffer.size(), data.size());

    send_buffer.insert(send_buffer.end(), data.begin(), data.begin() + length);

    TransmitPendingLocked();

    return static_cast<s32>(length);
}

TcpReceiveStatus TcpConnection::Receive(std::span<u8> buffer, int timeout_milliseconds, bool peek,
                                        bool blocking, std::size_t& out_read) {
    out_read = 0;

    std::unique_lock lock{mutex};

    while (true) {
        if (receive_length > 0) {
            out_read = ReadFromBufferLocked(buffer, peek);

            return TcpReceiveStatus::Ok;
        }

        if (aborted) {
            return TcpReceiveStatus::Reset;
        }

        if (remote_closed) {
            return TcpReceiveStatus::Closed;
        }

        if (!blocking) {
            return TcpReceiveStatus::WouldBlock;
        }

        if (timeout_milliseconds <= 0) {
            data_ready.wait(lock);
        } else if (data_ready.wait_for(lock, std::chrono::milliseconds{timeout_milliseconds}) ==
                   std::cv_status::timeout) {
            if (receive_length == 0 && !aborted && !remote_closed) {
                return TcpReceiveStatus::TimedOut;
            }
        }
    }
}

std::size_t TcpConnection::ReadFromBufferLocked(std::span<u8> buffer, bool peek) {
    const std::size_t length = std::min(buffer.size(), receive_length);

    for (std::size_t i = 0; i < length; i++) {
        buffer[i] = receive_buffer[(receive_head + i) % ReceiveBufferSize];
    }

    if (!peek) {
        receive_head = (receive_head + length) % ReceiveBufferSize;
        receive_length -= length;
    }

    return length;
}

void TcpConnection::Close() {
    bool abort_instead = false;

    {
        std::scoped_lock lock{mutex};

        switch (state) {
        case TcpState::Established:
            state = TcpState::FinWait1;
            break;

        case TcpState::CloseWait:
            state = TcpState::LastAck;
            break;

        case TcpState::SynSent:
        case TcpState::SynReceived:
            // Nothing was established yet, so there is nothing to shut down gracefully.
            abort_instead = true;
            break;

        default:
            return;
        }

        if (!abort_instead) {
            fin_sent = true;

            SendSegmentLocked(FlagFin | FlagAck, {}, snd_nxt);

            retransmit_deadline = GetTickCountMs() + retransmit_timeout;

            return;
        }
    }

    Abort();
}

void TcpConnection::Abort() {
    bool send_reset = false;

    {
        std::scoped_lock lock{mutex};

        if (aborted) {
            return;
        }

        send_reset = state == TcpState::Established || state == TcpState::CloseWait ||
                     state == TcpState::SynReceived || state == TcpState::FinWait1 ||
                     state == TcpState::FinWait2;

        aborted = true;
        remote_closed = true;
        state = TcpState::Closed;
    }

    if (send_reset) {
        SendSegment(FlagRst | FlagAck, {});
    }

    Cleanup();
}

void TcpConnection::Cleanup() {
    // Unregistering drops the interface's reference, which may be the last one, so hold on to
    // ourselves until this function returns.
    const std::shared_ptr<TcpConnection> keep_alive = weak_from_this().lock();

    network_interface.UnregisterConnection(this);

    state_changed.notify_all();
    data_ready.notify_all();

    StateHandler handler;

    {
        std::scoped_lock lock{handler_mutex};

        handler = std::move(closed_handler);
        closed_handler = nullptr;
    }

    if (handler) {
        handler();
    }
}

void TcpConnection::HandleSegment(std::span<const u8> segment) {
    if (segment.size() < HeaderSize) {
        return;
    }

    const std::size_t data_offset = static_cast<std::size_t>((segment[12] >> 4) & 0xF) * 4;

    if (data_offset < HeaderSize || data_offset > segment.size()) {
        return;
    }

    const u32 sequence = ReadBE32(segment.subspan(4));
    const u32 acknowledgement = ReadBE32(segment.subspan(8));
    const u8 flags = segment[13];
    const auto data = segment.subspan(data_offset);

    if ((flags & FlagRst) != 0) {
        network_interface.GetDiagnostics().TcpReset();

        LOG_DEBUG(Network_LanPlay, "TCP connection to {} was reset by the peer.", Describe());

        Abort();

        return;
    }

    std::vector<u8> pushed_data;
    bool has_pushed_data = false;
    bool send_ack = false;
    bool finished = false;
    bool established = false;

    {
        std::scoped_lock lock{mutex};

        if (state == TcpState::SynSent) {
            if ((flags & FlagSyn) == 0 || (flags & FlagAck) == 0 ||
                acknowledgement != snd_nxt + 1) {
                return;
            }

            snd_una = snd_nxt = acknowledgement;
            rcv_nxt = sequence + 1;
            state = TcpState::Established;
            established = true;

            ResetRetransmitTimerLocked();
            SendSegmentLocked(FlagAck, {}, snd_nxt);

            state_changed.notify_all();
        } else if (state == TcpState::SynReceived) {
            if ((flags & FlagAck) == 0 || acknowledgement != snd_nxt + 1) {
                return;
            }

            snd_una = snd_nxt = acknowledgement;
            state = TcpState::Established;
            established = true;

            ResetRetransmitTimerLocked();

            state_changed.notify_all();
        } else if ((flags & FlagAck) != 0) {
            HandleAcknowledgementLocked(acknowledgement);
        }

        if (!data.empty()) {
            if (sequence != rcv_nxt) {
                // Out of order: dropped on purpose, the peer will send it again.
                network_interface.GetDiagnostics().Dropped(
                    DropReason::Malformed,
                    fmt::format("out of order tcp segment from {}", Describe()));
            } else {
                bool pushes_directly = false;

                {
                    std::scoped_lock handler_lock{handler_mutex};

                    pushes_directly = static_cast<bool>(data_handler);
                }

                if (pushes_directly) {
                    pushed_data.assign(data.begin(), data.end());
                    has_pushed_data = true;
                    rcv_nxt += static_cast<u32>(data.size());
                } else {
                    const std::size_t accepted =
                        std::min(data.size(), ReceiveBufferSize - receive_length);

                    if (accepted < data.size()) {
                        network_interface.GetDiagnostics().Dropped(
                            DropReason::QueueFull,
                            fmt::format("tcp receive buffer from {}", Describe()));
                    }

                    for (std::size_t i = 0; i < accepted; i++) {
                        receive_buffer[(receive_head + receive_length + i) % ReceiveBufferSize] =
                            data[i];
                    }

                    receive_length += accepted;
                    rcv_nxt += static_cast<u32>(accepted);

                    data_ready.notify_all();
                }
            }

            // Anything out of order (or that did not fit) is dropped: the peer will resend it.
            send_ack = true;
        }

        if ((flags & FlagFin) != 0 && sequence + static_cast<u32>(data.size()) == rcv_nxt) {
            rcv_nxt++;
            remote_closed = true;
            send_ack = true;

            data_ready.notify_all();

            switch (state) {
            case TcpState::Established:
                state = TcpState::CloseWait;
                break;

            case TcpState::FinWait1:
            case TcpState::FinWait2:
                state = TcpState::TimeWait;
                time_wait_deadline = GetTickCountMs() + TimeWaitMs;
                break;

            default:
                break;
            }
        }

        if (fin_acked) {
            switch (state) {
            case TcpState::FinWait1:
                state = TcpState::FinWait2;
                break;

            case TcpState::LastAck:
                state = TcpState::Closed;
                finished = true;
                break;

            default:
                break;
            }
        }
    }

    if (established) {
        LOG_DEBUG(Network_LanPlay, "TCP connection established with {} (local port {}).", Describe(),
                  local_port);

        StateHandler handler;

        {
            std::scoped_lock lock{handler_mutex};

            handler = established_handler;
        }

        if (handler) {
            handler();
        }
    }

    if (has_pushed_data) {
        DataHandler handler;

        {
            std::scoped_lock lock{handler_mutex};

            handler = data_handler;
        }

        if (handler) {
            handler(pushed_data);
        }
    }

    if (send_ack) {
        SendSegment(FlagAck, {});
    }

    if (finished) {
        Cleanup();
    }
}

void TcpConnection::HandleAcknowledgementLocked(u32 acknowledgement) {
    if (SeqLeq(acknowledgement, snd_una) ||
        SeqGt(acknowledgement, snd_nxt + (fin_sent ? 1u : 0u))) {
        return;
    }

    u32 acked = acknowledgement - snd_una;

    if (fin_sent && acknowledgement == snd_nxt + 1) {
        fin_acked = true;
        snd_nxt = acknowledgement;
        acked--;
    }

    if (acked > 0) {
        const std::size_t count = std::min<std::size_t>(acked, send_buffer.size());

        send_buffer.erase(send_buffer.begin(), send_buffer.begin() + count);
        snd_una += static_cast<u32>(count);
    } else if (fin_acked) {
        snd_una = acknowledgement;
    }

    ResetRetransmitTimerLocked();

    TransmitPendingLocked();
}

void TcpConnection::ResetRetransmitTimerLocked() {
    retransmit_count = 0;
    retransmit_timeout = InitialRetransmitTimeoutMs;
    retransmit_deadline =
        (!send_buffer.empty() || fin_sent) ? GetTickCountMs() + retransmit_timeout : 0;
}

void TcpConnection::TransmitPendingLocked() {
    if (send_buffer.empty()) {
        return;
    }

    std::size_t in_flight = static_cast<std::size_t>(snd_nxt - snd_una);

    while (in_flight < send_buffer.size()) {
        const std::size_t length = std::min(send_buffer.size() - in_flight, MaxSegmentSize);

        std::vector<u8> payload(length);

        std::copy_n(send_buffer.begin() + in_flight, length, payload.begin());

        SendSegmentLocked(FlagAck | FlagPsh, payload, snd_nxt);

        snd_nxt += static_cast<u32>(length);
        in_flight += length;
    }

    if (!send_buffer.empty() && retransmit_deadline == 0) {
        retransmit_deadline = GetTickCountMs() + retransmit_timeout;
    }
}

void TcpConnection::Tick() {
    bool retransmit_syn = false;
    bool retransmit_fin = false;
    std::vector<u8> retransmit_data;
    u32 retransmit_sequence = 0;
    bool give_up = false;
    bool finished = false;
    TcpState current_state = TcpState::Closed;

    {
        std::scoped_lock lock{mutex};

        current_state = state;

        if (state == TcpState::TimeWait && GetTickCountMs() >= time_wait_deadline) {
            state = TcpState::Closed;
            finished = true;
        } else if (retransmit_deadline != 0 && GetTickCountMs() >= retransmit_deadline) {
            if (++retransmit_count > MaxRetransmits) {
                give_up = true;
            } else {
                retransmit_timeout = std::min(retransmit_timeout * 2, MaxRetransmitTimeoutMs);
                retransmit_deadline = GetTickCountMs() + retransmit_timeout;

                switch (state) {
                case TcpState::SynSent:
                case TcpState::SynReceived:
                    retransmit_syn = true;
                    break;

                default:
                    if (!send_buffer.empty()) {
                        const std::size_t length = std::min(send_buffer.size(), MaxSegmentSize);

                        retransmit_data.resize(length);
                        std::copy_n(send_buffer.begin(), length, retransmit_data.begin());
                        retransmit_sequence = snd_una;
                    } else if (fin_sent && !fin_acked) {
                        retransmit_fin = true;
                    }
                    break;
                }
            }
        }
    }

    if (give_up) {
        LOG_WARNING(Network_LanPlay,
                    "TCP connection to {} timed out after {} retransmissions.", Describe(),
                    MaxRetransmits);

        Abort();

        return;
    }

    if (retransmit_syn || !retransmit_data.empty() || retransmit_fin) {
        network_interface.GetDiagnostics().TcpRetransmit(retransmit_count);
    }

    if (retransmit_syn) {
        SendSegment(current_state == TcpState::SynSent ? FlagSyn : (FlagSyn | FlagAck), {});
    } else if (!retransmit_data.empty()) {
        std::scoped_lock lock{mutex};

        SendSegmentLocked(FlagAck | FlagPsh, retransmit_data, retransmit_sequence);
    } else if (retransmit_fin) {
        SendSegment(FlagFin | FlagAck, {});
    }

    if (finished) {
        Cleanup();
    }
}

void TcpConnection::SendSegment(u8 flags, std::span<const u8> payload) {
    std::scoped_lock lock{mutex};

    SendSegmentLocked(flags, payload, snd_nxt);

    // SYN and FIN each consume a sequence number and have to be retransmitted until acknowledged.
    if ((flags & (FlagSyn | FlagFin)) != 0) {
        retransmit_deadline = GetTickCountMs() + retransmit_timeout;
    }
}

void TcpConnection::SendSegmentLocked(u8 flags, std::span<const u8> payload, u32 sequence) {
    std::vector<u8> segment(HeaderSize + payload.size());

    WriteBE16(segment, local_port);
    WriteBE16(std::span{segment}.subspan(2), remote_port);
    WriteBE32(std::span{segment}.subspan(4), sequence);
    WriteBE32(std::span{segment}.subspan(8), rcv_nxt);

    segment[12] = 5 << 4;
    segment[13] = flags;

    WriteBE16(std::span{segment}.subspan(14),
              static_cast<u16>(std::min<std::size_t>(ReceiveBufferSize - receive_length, 0xFFFF)));

    if (!payload.empty()) {
        std::memcpy(segment.data() + HeaderSize, payload.data(), payload.size());
    }

    WriteBE16(std::span{segment}.subspan(16),
              Ipv4::TransportChecksum(network_interface.GetAddress(), remote_address,
                                      Ipv4::ProtocolTcp, segment));

    network_interface.SendIpv4(remote_address, Ipv4::ProtocolTcp, segment);
}

void TcpConnection::SendReset(NetworkInterface& network_interface, u16 local_port,
                              u32 remote_address, u16 remote_port, std::span<const u8> incoming) {
    const u8 incoming_flags = incoming[13];

    if ((incoming_flags & FlagRst) != 0) {
        return;
    }

    const u32 incoming_sequence = ReadBE32(incoming.subspan(4));
    const u32 incoming_ack = ReadBE32(incoming.subspan(8));
    const std::size_t data_offset = static_cast<std::size_t>((incoming[12] >> 4) & 0xF) * 4;
    const std::size_t data_length =
        incoming.size() > data_offset ? incoming.size() - data_offset : 0;

    std::vector<u8> segment(HeaderSize);

    WriteBE16(segment, local_port);
    WriteBE16(std::span{segment}.subspan(2), remote_port);

    if ((incoming_flags & FlagAck) != 0) {
        WriteBE32(std::span{segment}.subspan(4), incoming_ack);
        segment[13] = FlagRst;
    } else {
        WriteBE32(std::span{segment}.subspan(8),
                  incoming_sequence + static_cast<u32>(data_length) +
                      ((incoming_flags & FlagSyn) != 0 ? 1u : 0u));
        segment[13] = FlagRst | FlagAck;
    }

    segment[12] = 5 << 4;

    WriteBE16(std::span{segment}.subspan(16),
              Ipv4::TransportChecksum(network_interface.GetAddress(), remote_address,
                                      Ipv4::ProtocolTcp, segment));

    network_interface.SendIpv4(remote_address, Ipv4::ProtocolTcp, segment);
}

} // namespace Network::LanPlay
