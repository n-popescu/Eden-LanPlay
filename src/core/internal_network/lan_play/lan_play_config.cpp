// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <charconv>
#include <string_view>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/internal_network/lan_play/lan_play_config.h"
#include "core/internal_network/lan_play/virtual_address_allocator.h"
#include "core/internal_network/network.h"

namespace Network::LanPlay {

namespace {

std::string Trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");

    if (begin == std::string_view::npos) {
        return {};
    }

    const auto end = value.find_last_not_of(" \t\r\n");

    return std::string{value.substr(begin, end - begin + 1)};
}

/// Parses a dotted decimal IPv4 literal into a host ordered integer.
bool TryParseIpv4(const std::string& value, u32& out_address) {
    u32 address = 0;
    std::size_t start = 0;

    for (int part = 0; part < 4; part++) {
        const auto separator = part == 3 ? value.size() : value.find('.', start);

        if (separator == std::string::npos || separator == start) {
            return false;
        }

        unsigned int octet = 0;
        const auto* first = value.data() + start;
        const auto* last = value.data() + separator;
        const auto result = std::from_chars(first, last, octet);

        if (result.ec != std::errc{} || result.ptr != last || octet > 255) {
            return false;
        }

        address = (address << 8) | octet;
        start = separator + 1;
    }

    out_address = address;

    return true;
}

/// Resolves a host name to an IPv4 literal, preferring IPv4 answers.
bool TryResolveHost(const std::string& host, std::string& out_address) {
    u32 literal = 0;

    if (TryParseIpv4(host, literal)) {
        out_address = host;

        return true;
    }

    const auto info = GetAddressInfo(host, std::nullopt);

    if (std::holds_alternative<GetAddrInfoError>(info)) {
        LOG_ERROR(Network_LanPlay, "could not resolve the relay host \"{}\".", host);

        return false;
    }

    for (const auto& entry : std::get<std::vector<AddrInfo>>(info)) {
        if (entry.family != Domain::INET) {
            continue;
        }

        const auto& ip = entry.addr.ip;

        out_address = fmt::format("{}.{}.{}.{}", ip[0], ip[1], ip[2], ip[3]);

        return true;
    }

    LOG_ERROR(Network_LanPlay, "the relay host \"{}\" has no IPv4 address.", host);

    return false;
}

} // Anonymous namespace

std::string Configuration::Describe() const {
    return fmt::format("{}:{}", relay_host, relay_port);
}

bool Configuration::TryParse(const std::string& server, const std::string& virtual_address_setting,
                             Configuration& out_configuration) {
    out_configuration = {};

    std::string address = Trim(server);

    if (address.empty()) {
        LOG_ERROR(Network_LanPlay, "no relay server configured.");

        return false;
    }

    // [user[:password]@]host[:port]
    const auto credentials_end = address.rfind('@');

    if (credentials_end != std::string::npos) {
        const std::string credentials = address.substr(0, credentials_end);

        address = address.substr(credentials_end + 1);

        const auto password_start = credentials.find(':');

        if (password_start == std::string::npos) {
            out_configuration.user_name = credentials;
        } else {
            out_configuration.user_name = credentials.substr(0, password_start);

            const auto hash = Relay::HashPassword(credentials.substr(password_start + 1));

            out_configuration.password_hash.assign(hash.begin(), hash.end());
        }
    }

    std::string host = address;
    unsigned int port = Relay::DefaultRelayPort;

    const auto port_start = address.rfind(':');

    // A bare IPv6 literal also contains colons, so only treat the last one as a port separator when
    // it is the only one.
    if (port_start != std::string::npos && address.find(':') == port_start) {
        const auto* first = address.data() + port_start + 1;
        const auto* last = address.data() + address.size();
        const auto result = std::from_chars(first, last, port);

        if (result.ec != std::errc{} || result.ptr != last || port == 0 || port > 0xFFFF) {
            LOG_ERROR(Network_LanPlay, "\"{}\" has an invalid port.", address);

            return false;
        }

        host = address.substr(0, port_start);
    }

    if (host.empty()) {
        LOG_ERROR(Network_LanPlay, "\"{}\" has no host.", address);

        return false;
    }

    if (!TryResolveHost(host, out_configuration.relay_host)) {
        return false;
    }

    out_configuration.relay_port = static_cast<u16>(port);

    const std::string virtual_address = Trim(virtual_address_setting);

    if (!virtual_address.empty() && virtual_address != "auto" && virtual_address != "Auto") {
        u32 parsed = 0;

        if (TryParseIpv4(virtual_address, parsed) && VirtualAddress::IsUsableAddress(parsed)) {
            out_configuration.virtual_address = parsed;
        } else {
            LOG_WARNING(Network_LanPlay,
                        "\"{}\" is not a usable 10.13.x.x address, one will be picked "
                        "automatically.",
                        virtual_address);
        }
    }

    return true;
}

} // namespace Network::LanPlay
