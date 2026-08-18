// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Network::LanPlay {

/**
 * Packet fields are read and written explicitly in network order rather than by memcpy-ing a
 * struct, so nothing here depends on the host's own byte order or on struct padding.
 */
constexpr u16 ReadBE16(std::span<const u8> data) {
    return static_cast<u16>((static_cast<u16>(data[0]) << 8) | data[1]);
}

constexpr u32 ReadBE32(std::span<const u8> data) {
    return (static_cast<u32>(data[0]) << 24) | (static_cast<u32>(data[1]) << 16) |
           (static_cast<u32>(data[2]) << 8) | static_cast<u32>(data[3]);
}

constexpr void WriteBE16(std::span<u8> data, u16 value) {
    data[0] = static_cast<u8>(value >> 8);
    data[1] = static_cast<u8>(value);
}

constexpr void WriteBE32(std::span<u8> data, u32 value) {
    data[0] = static_cast<u8>(value >> 24);
    data[1] = static_cast<u8>(value >> 16);
    data[2] = static_cast<u8>(value >> 8);
    data[3] = static_cast<u8>(value);
}

/// Milliseconds from a monotonic clock, used for every timeout and deadline in the LAN Play stack.
u64 GetTickCountMs();

/// Formats a host ordered IPv4 address as dotted decimal, for logs and reports.
std::string FormatAddress(u32 address);

/**
 * Wire format spoken between a switch-lan-play client and a LAN Play relay server.
 *
 * Every datagram sent to (or received from) the relay is a single type byte followed by a payload.
 * See https://github.com/spacemeowx2/switch-lan-play (src/lan-client.c).
 */
namespace Relay {

/// Virtual network shared by every client of a relay (SUBNET_NET / SUBNET_MASK in switch-lan-play).
constexpr u32 SubnetNetwork = 0x0A0D0000; // 10.13.0.0
constexpr u32 SubnetMask = 0xFFFF0000;    // 255.255.0.0
constexpr u32 SubnetBroadcast = SubnetNetwork | ~SubnetMask; // 10.13.255.255

/**
 * Address the switch-lan-play client itself answers on (its gateway / "fake internet" address).
 * Never handed out to a console, so we must not use it either.
 */
constexpr u32 GatewayAddress = 0x0A0D2501; // 10.13.37.1

constexpr u16 DefaultRelayPort = 11451;

/// The relay drops clients that go quiet; the reference client sends a keepalive every 10 seconds.
constexpr u64 KeepAliveIntervalMs = 10 * 1000;

/// Largest payload the relay is expected to handle in a single datagram (ETHER_MTU).
constexpr std::size_t MaxIpv4PacketSize = 1500;

constexpr std::size_t AuthKeyLength = 20;

enum class PacketType : u8 {
    KeepAlive = 0x00,
    Ipv4 = 0x01,
    Ping = 0x02,
    Ipv4Fragment = 0x03,
    AuthMe = 0x04,
    Info = 0x10,
};

/// Header of an Ipv4Fragment payload (LC_FRAG_* in switch-lan-play).
constexpr std::size_t FragmentHeaderSize = 16;

/// The fragment header carries a 5 bit part index, so a packet never splits into more than this.
constexpr u8 MaxFragmentParts = 32;

struct FragmentHeader {
    u32 source{};
    u32 destination{};
    u16 id{};
    u8 part{};
    u8 total_parts{};
    u16 length{};
    u16 path_mtu{};
};

/// Reads a fragment header and validates it against the payload actually received.
bool TryReadFragmentHeader(std::span<const u8> payload, FragmentHeader& out_header);

void WriteFragmentHeader(std::span<u8> destination, const FragmentHeader& header);

/**
 * Builds the response to an AuthMe challenge of auth type 0:
 * sha1(sha1(password) + challenge) followed by the user name in plain text.
 */
std::vector<u8> BuildAuthResponse(std::span<const u8> password_hash, std::span<const u8> challenge,
                                  const std::string& user_name);

/// sha1 of a passphrase, as stored in the configuration.
std::array<u8, AuthKeyLength> HashPassword(const std::string& password);

} // namespace Relay

} // namespace Network::LanPlay
