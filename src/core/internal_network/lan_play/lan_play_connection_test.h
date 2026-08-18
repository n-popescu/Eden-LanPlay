// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace Network::LanPlay {

/// Outcome of a connection test, ready to be shown to the user.
struct ConnectionTestResult {
    bool success{};
    std::string message;
};

/**
 * Checks a LAN Play relay without starting a game, so a broken setup can be told apart from a game
 * specific problem.
 *
 * It joins the relay exactly the way the emulator does, picks a virtual address, sends the same LDN
 * scan request a console sends, listens for a few seconds and reports what answered. Blocks for
 * several seconds, so callers must not run it on the UI thread.
 */
ConnectionTestResult RunConnectionTest(const std::string& server,
                                       const std::string& virtual_address_setting);

} // namespace Network::LanPlay
