// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "common/common_funcs.h"
#include "common/common_types.h"

namespace Service::LDN::LanPlay {

/**
 * The ldn_mitm wire protocol, as spoken by a real Switch running ldn_mitm and by Ryujinx's
 * ldn_mitm / LAN Play modes.
 *
 * This is deliberately byte compatible rather than convenient: it is what makes a session hosted by
 * one emulator visible to the other, and to a console, on the same relay. Discovery is UDP broadcast
 * and unicast on port 11452; the session channel is TCP on port 11452.
 *
 * See https://github.com/spacemeowx2/ldn_mitm.
 */
namespace Ldn {

constexpr u16 DefaultPort = 11452;

constexpr u32 LanMagic = 0x11451400;

constexpr std::size_t BufferSize = 2048;

/// Values a real console uses, mirrored so that a scan result looks the same from either emulator.
constexpr u16 CommonChannel = 6;
constexpr u8 CommonLinkLevel = 3;
constexpr u8 CommonNetworkType = 2;

enum class PacketType : u8 {
    Scan = 0,
    ScanResponse = 1,
    Connect = 2,
    SyncNetwork = 3,
};

/**
 * Header in front of every ldn_mitm packet.
 *
 * Written and read field by field rather than memcpy-ed, so the layout does not depend on the host's
 * byte order or on how the compiler packs the struct. All fields are little endian on the wire,
 * matching ldn_mitm and the C# implementation it interoperates with.
 */
struct PacketHeader {
    u32 magic{};
    PacketType type{};
    u8 compressed{};
    u16 length{};
    u16 decompress_length{};
    std::array<u8, 2> reserved{};
};

constexpr std::size_t HeaderSize = 12;

void WriteHeader(std::span<u8> destination, const PacketHeader& header);
bool TryReadHeader(std::span<const u8> source, PacketHeader& out_header);

/**
 * The run length encoding ldn_mitm applies to packet bodies: a zero byte is followed by a count of
 * additional zero bytes. Returns false if the input could not be encoded within the buffer limit.
 */
bool Compress(std::span<const u8> input, std::vector<u8>& output);
bool Decompress(std::span<const u8> input, std::vector<u8>& output);

/// Builds a complete packet (header plus optionally compressed body) ready to be sent.
std::vector<u8> BuildPacket(PacketType type, std::span<const u8> data);

} // namespace Ldn

} // namespace Service::LDN::LanPlay
