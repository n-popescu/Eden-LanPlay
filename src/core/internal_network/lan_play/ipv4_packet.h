// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"
#include "core/internal_network/lan_play/lan_play_protocol.h"

namespace Network::LanPlay {

/// Reading and writing of the IPv4 headers exchanged with the LAN Play relay.
namespace Ipv4 {

constexpr std::size_t MinHeaderSize = 20;

constexpr std::size_t TotalLengthOffset = 2;
constexpr std::size_t IdentificationOffset = 4;
constexpr std::size_t FlagsOffset = 6;
constexpr std::size_t ProtocolOffset = 9;
constexpr std::size_t ChecksumOffset = 10;
constexpr std::size_t SourceOffset = 12;
constexpr std::size_t DestinationOffset = 16;

constexpr u8 ProtocolIcmp = 1;
constexpr u8 ProtocolTcp = 6;
constexpr u8 ProtocolUdp = 17;

constexpr u16 FlagDontFragment = 0x4000;
constexpr u16 FlagMoreFragments = 0x2000;
constexpr u16 FragmentOffsetMask = 0x1FFF;

/**
 * Largest IPv4 payload we put on the wire before fragmenting, so that a datagram stays within the
 * Ethernet MTU a real console (and every switch-lan-play client) would use.
 */
constexpr std::size_t MaxPayloadSize = Relay::MaxIpv4PacketSize - MinHeaderSize;

struct Header {
    std::size_t header_length{};
    std::size_t total_length{};
    u16 identification{};
    u16 flags{};
    std::size_t fragment_offset{};
    u8 protocol{};
    u32 source{};
    u32 destination{};

    [[nodiscard]] bool MoreFragments() const {
        return (flags & FlagMoreFragments) != 0;
    }

    [[nodiscard]] bool IsFragment() const {
        return MoreFragments() || fragment_offset != 0;
    }
};

bool TryParse(std::span<const u8> packet, Header& out_header);

void WriteHeader(std::span<u8> destination, u32 source, u32 dest, u8 protocol,
                 std::size_t payload_length, u16 identification, u16 flags_and_fragment_offset = 0,
                 u8 ttl = 128);

/// Standard internet checksum (RFC 1071) over the given span.
u16 Checksum(std::span<const u8> data);

u32 PartialChecksum(std::span<const u8> data, u32 sum);

u16 FinishChecksum(u32 sum);

/// Checksum of a TCP or UDP segment, including the IPv4 pseudo header.
u16 TransportChecksum(u32 source, u32 dest, u8 protocol, std::span<const u8> segment);

} // namespace Ipv4

} // namespace Network::LanPlay
