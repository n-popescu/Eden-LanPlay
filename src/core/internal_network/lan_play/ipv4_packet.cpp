// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include "core/internal_network/lan_play/ipv4_packet.h"

namespace Network::LanPlay::Ipv4 {

bool TryParse(std::span<const u8> packet, Header& out_header) {
    out_header = {};

    if (packet.size() < MinHeaderSize) {
        return false;
    }

    if ((packet[0] >> 4) != 4) {
        return false;
    }

    const std::size_t header_length = static_cast<std::size_t>(packet[0] & 0xF) * 4;

    if (header_length < MinHeaderSize || header_length > packet.size()) {
        return false;
    }

    std::size_t total_length = ReadBE16(packet.subspan(TotalLengthOffset));

    // Some senders pad the frame; never read past what we actually received.
    if (total_length < header_length || total_length > packet.size()) {
        total_length = packet.size();
    }

    const u16 flags_and_offset = ReadBE16(packet.subspan(FlagsOffset));

    out_header.header_length = header_length;
    out_header.total_length = total_length;
    out_header.identification = ReadBE16(packet.subspan(IdentificationOffset));
    out_header.flags = static_cast<u16>(flags_and_offset & ~FragmentOffsetMask);
    out_header.fragment_offset = static_cast<std::size_t>(flags_and_offset & FragmentOffsetMask) * 8;
    out_header.protocol = packet[ProtocolOffset];
    out_header.source = ReadBE32(packet.subspan(SourceOffset));
    out_header.destination = ReadBE32(packet.subspan(DestinationOffset));

    return true;
}

void WriteHeader(std::span<u8> destination, u32 source, u32 dest, u8 protocol,
                 std::size_t payload_length, u16 identification, u16 flags_and_fragment_offset,
                 u8 ttl) {
    std::fill_n(destination.begin(), MinHeaderSize, u8{0});

    destination[0] = 0x45;
    destination[1] = 0;

    WriteBE16(destination.subspan(TotalLengthOffset),
              static_cast<u16>(MinHeaderSize + payload_length));
    WriteBE16(destination.subspan(IdentificationOffset), identification);
    WriteBE16(destination.subspan(FlagsOffset), flags_and_fragment_offset);

    destination[8] = ttl;
    destination[ProtocolOffset] = protocol;

    WriteBE32(destination.subspan(SourceOffset), source);
    WriteBE32(destination.subspan(DestinationOffset), dest);
    WriteBE16(destination.subspan(ChecksumOffset), Checksum(destination.subspan(0, MinHeaderSize)));
}

u32 PartialChecksum(std::span<const u8> data, u32 sum) {
    std::size_t i = 0;

    for (; i + 1 < data.size(); i += 2) {
        sum += static_cast<u32>((static_cast<u32>(data[i]) << 8) | data[i + 1]);
    }

    if (i < data.size()) {
        sum += static_cast<u32>(data[i]) << 8;
    }

    return sum;
}

u16 FinishChecksum(u32 sum) {
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<u16>(~sum);
}

u16 Checksum(std::span<const u8> data) {
    return FinishChecksum(PartialChecksum(data, 0));
}

u16 TransportChecksum(u32 source, u32 dest, u8 protocol, std::span<const u8> segment) {
    std::array<u8, 12> pseudo_header{};

    WriteBE32(pseudo_header, source);
    WriteBE32(std::span{pseudo_header}.subspan(4), dest);
    pseudo_header[8] = 0;
    pseudo_header[9] = protocol;
    WriteBE16(std::span{pseudo_header}.subspan(10), static_cast<u16>(segment.size()));

    u32 sum = PartialChecksum(pseudo_header, 0);
    sum = PartialChecksum(segment, sum);

    return FinishChecksum(sum);
}

} // namespace Network::LanPlay::Ipv4
