// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstring>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "core/internal_network/lan_play/lan_play_protocol.h"

namespace Network::LanPlay {

u64 GetTickCountMs() {
    using namespace std::chrono;

    return static_cast<u64>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string FormatAddress(u32 address) {
    return fmt::format("{}.{}.{}.{}", (address >> 24) & 0xFF, (address >> 16) & 0xFF,
                       (address >> 8) & 0xFF, address & 0xFF);
}

} // namespace Network::LanPlay

namespace Network::LanPlay::Relay {

namespace {

std::array<u8, AuthKeyLength> Sha1(std::span<const u8> data) {
    std::array<u8, AuthKeyLength> digest{};
    unsigned int length = 0;

    EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha1(), nullptr);

    return digest;
}

} // Anonymous namespace

bool TryReadFragmentHeader(std::span<const u8> payload, FragmentHeader& out_header) {
    out_header = {};

    if (payload.size() < FragmentHeaderSize) {
        return false;
    }

    out_header.source = ReadBE32(payload);
    out_header.destination = ReadBE32(payload.subspan(4));
    out_header.id = ReadBE16(payload.subspan(8));
    out_header.part = payload[10];
    out_header.total_parts = payload[11];
    out_header.length = ReadBE16(payload.subspan(12));
    out_header.path_mtu = ReadBE16(payload.subspan(14));

    if (out_header.total_parts == 0 || out_header.total_parts > MaxFragmentParts) {
        return false;
    }

    if (out_header.part >= out_header.total_parts) {
        return false;
    }

    return payload.size() >= FragmentHeaderSize + out_header.length;
}

void WriteFragmentHeader(std::span<u8> destination, const FragmentHeader& header) {
    WriteBE32(destination, header.source);
    WriteBE32(destination.subspan(4), header.destination);
    WriteBE16(destination.subspan(8), header.id);
    destination[10] = header.part;
    destination[11] = header.total_parts;
    WriteBE16(destination.subspan(12), header.length);
    WriteBE16(destination.subspan(14), header.path_mtu);
}

std::vector<u8> BuildAuthResponse(std::span<const u8> password_hash, std::span<const u8> challenge,
                                  const std::string& user_name) {
    std::vector<u8> hash_input(password_hash.size() + challenge.size());

    std::memcpy(hash_input.data(), password_hash.data(), password_hash.size());
    std::memcpy(hash_input.data() + password_hash.size(), challenge.data(), challenge.size());

    const auto digest = Sha1(hash_input);

    std::vector<u8> response(AuthKeyLength + user_name.size());

    std::memcpy(response.data(), digest.data(), AuthKeyLength);
    std::memcpy(response.data() + AuthKeyLength, user_name.data(), user_name.size());

    return response;
}

std::array<u8, AuthKeyLength> HashPassword(const std::string& password) {
    return Sha1(std::span{reinterpret_cast<const u8*>(password.data()), password.size()});
}

} // namespace Network::LanPlay::Relay
