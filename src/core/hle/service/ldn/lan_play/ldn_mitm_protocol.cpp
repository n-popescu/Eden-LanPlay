// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "common/logging.h"
#include "core/hle/service/ldn/lan_play/ldn_mitm_protocol.h"

namespace Service::LDN::LanPlay::Ldn {

namespace {

constexpr void WriteLE16(std::span<u8> data, u16 value) {
    data[0] = static_cast<u8>(value);
    data[1] = static_cast<u8>(value >> 8);
}

constexpr u16 ReadLE16(std::span<const u8> data) {
    return static_cast<u16>(static_cast<u16>(data[0]) | (static_cast<u16>(data[1]) << 8));
}

constexpr void WriteLE32(std::span<u8> data, u32 value) {
    data[0] = static_cast<u8>(value);
    data[1] = static_cast<u8>(value >> 8);
    data[2] = static_cast<u8>(value >> 16);
    data[3] = static_cast<u8>(value >> 24);
}

constexpr u32 ReadLE32(std::span<const u8> data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
}

} // Anonymous namespace

void WriteHeader(std::span<u8> destination, const PacketHeader& header) {
    WriteLE32(destination, header.magic);
    destination[4] = static_cast<u8>(header.type);
    destination[5] = header.compressed;
    WriteLE16(destination.subspan(6), header.length);
    WriteLE16(destination.subspan(8), header.decompress_length);
    destination[10] = header.reserved[0];
    destination[11] = header.reserved[1];
}

bool TryReadHeader(std::span<const u8> source, PacketHeader& out_header) {
    if (source.size() < HeaderSize) {
        return false;
    }

    out_header.magic = ReadLE32(source);
    out_header.type = static_cast<PacketType>(source[4]);
    out_header.compressed = source[5];
    out_header.length = ReadLE16(source.subspan(6));
    out_header.decompress_length = ReadLE16(source.subspan(8));
    out_header.reserved = {source[10], source[11]};

    return true;
}

bool Compress(std::span<const u8> input, std::vector<u8>& output) {
    constexpr std::size_t MaxRun = 0xFF;

    output.clear();

    std::size_t i = 0;

    while (i < input.size()) {
        const u8 value = input[i++];

        if (value != 0) {
            output.push_back(value);

            continue;
        }

        std::size_t count = 0;

        while (i < input.size() && input[i] == 0 && count < MaxRun) {
            count++;
            i++;
        }

        output.push_back(0);

        if (output.size() == BufferSize) {
            return false;
        }

        output.push_back(static_cast<u8>(count));
    }

    return i == input.size();
}

bool Decompress(std::span<const u8> input, std::vector<u8>& output) {
    output.clear();

    std::size_t i = 0;

    while (i < input.size() && output.size() < BufferSize) {
        const u8 value = input[i++];

        output.push_back(value);

        if (value != 0) {
            continue;
        }

        if (i == input.size()) {
            return false;
        }

        const u8 count = input[i++];

        for (u8 j = 0; j < count; j++) {
            if (output.size() == BufferSize) {
                break;
            }

            output.push_back(value);
        }
    }

    return i == input.size();
}

std::vector<u8> BuildPacket(PacketType type, std::span<const u8> data) {
    PacketHeader header{};

    header.magic = LanMagic;
    header.type = type;
    header.compressed = 0;
    header.length = static_cast<u16>(data.size());
    header.decompress_length = 0;

    if (data.empty()) {
        std::vector<u8> packet(HeaderSize);

        WriteHeader(packet, header);

        return packet;
    }

    std::vector<u8> compressed;

    if (Compress(data, compressed)) {
        header.decompress_length = header.length;
        header.length = static_cast<u16>(compressed.size());
        header.compressed = 1;

        std::vector<u8> packet(HeaderSize + compressed.size());

        WriteHeader(packet, header);
        std::memcpy(packet.data() + HeaderSize, compressed.data(), compressed.size());

        return packet;
    }

    LOG_ERROR(Service_LDN, "compressing an ldn_mitm packet body failed; sending it uncompressed.");

    std::vector<u8> packet(HeaderSize + data.size());

    WriteHeader(packet, header);
    std::memcpy(packet.data() + HeaderSize, data.data(), data.size());

    return packet;
}

} // namespace Service::LDN::LanPlay::Ldn
