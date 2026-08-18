// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"

namespace Network::LanPlay {

class Client;

/**
 * Picks the 10.13.x.x address the emulated console presents on the LAN Play network.
 *
 * The host keeps its own address; this one only exists inside the emulator. A candidate is probed
 * before being adopted: we ask the relay to deliver an ICMP echo request to it and watch for a
 * reply, which is how a real console's duplicate address detection behaves on a LAN. This is best
 * effort, because a relay cannot answer "who has this address" the way ARP does.
 */
namespace VirtualAddress {

/**
 * Addresses that must never be handed out: anything outside 10.13.0.0/16, anything ending in .0 or
 * .255, 10.13.0.1, and the address the switch-lan-play client itself answers on (10.13.37.1).
 */
bool IsUsableAddress(u32 address);

/// Picks an address, honouring `preferred` when it is usable, and probing otherwise.
u32 Allocate(Client& client, std::optional<u32> preferred);

/// Asks the relay to deliver an ICMP echo request to `candidate` and reports whether it answered.
bool IsAddressTaken(Client& client, u32 candidate);

} // namespace VirtualAddress

} // namespace Network::LanPlay
