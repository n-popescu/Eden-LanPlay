// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/internal_network/lan_play/lan_play_protocol.h"

namespace Network::LanPlay {

/// Settings needed to join a LAN Play relay, parsed from the user facing server string.
struct Configuration {
    /// Relay address, already resolved to an IPv4 literal string, and its port.
    std::string relay_host;
    u16 relay_port{Relay::DefaultRelayPort};

    /// Relay credentials, only used if the relay sends an AuthMe challenge.
    std::string user_name;
    std::vector<u8> password_hash;

    /// Virtual address the emulated console should use, or empty to pick one automatically.
    std::optional<u32> virtual_address;

    /**
     * Maximum payload size of a relay datagram before the LAN Play fragmentation layer kicks in.
     * 0 disables it, matching the reference client's default (no --pmtu).
     */
    u16 path_mtu{0};

    [[nodiscard]] std::string Describe() const;

    /**
     * Parses a server string of the form `[user[:password]@]host[:port]`, resolving the host.
     * `virtual_address_setting` may be empty or "auto" for an automatic address.
     */
    static bool TryParse(const std::string& server, const std::string& virtual_address_setting,
                         Configuration& out_configuration);
};

} // namespace Network::LanPlay
