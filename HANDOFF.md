# LAN Play on Eden — handoff

Everything a fresh session needs to continue this work. Written at the point where the code is
complete and compiles, but has never been linked or run.

---

## 1. Situation and constraints

**Repositories.**

- `n-popescu/mirror` — a **private** fork of [Eden](https://git.eden-emu.dev/eden-emu/eden) (C++20,
  CMake, a yuzu descendant). This is where all the work landed.
- `n-popescu/Ryubing-LanPlay` — a fork of Ryubing/Ryujinx (C#, .NET). **Reference implementation
  only; it was not modified.** Its `docs/lan-play.md` is the specification this port follows.

**Branch:** `claude/lan-play-3c0ac7d64df08073afec00a9f8c3d4e9` on `mirror`, two commits, open as
[PR #1](https://github.com/n-popescu/mirror/pull/1) against `master`.

**Policy constraint you must respect.** `mirror` contains `CLAUDE.md` and `AGENTS.md`, both of which
say AI/LLM use is *strictly prohibited* in the codebase, including for docs and commit messages.
That is upstream Eden's rule, inherited by this fork through commit `defb8bf`. The owner
(`n-popescu`) knows, has made the fork private, and has said explicitly this will never be PR'd
upstream and is for friends only. **Do not propose any of this upstream, and do not remove or edit
those two files.** If the owner ever changes their mind about upstreaming, the answer is that a human
must reimplement from scratch — cleaning up this code is not sufficient.

**What the owner asked for**, all three delivered: port LAN Play from Ryubing to Eden; add release CI
for every platform modelled on Ryubing's; write this handoff.

---

## 2. The core problem this solves, and why the two emulators needed different work

A [switch-lan-play](https://github.com/spacemeowx2/switch-lan-play) relay forwards **complete raw
IPv4 packets** between clients on a virtual `10.13.0.0/16` network. It is game agnostic: it routes on
the destination address inside the IPv4 header, and floods `10.13.255.255` to everyone.

Neither emulator had anything that could talk to that:

- **Neither has an emulated Ethernet/ARP/IPv4/UDP layer.** Guest BSD socket calls are translated one
  to one into host socket calls and the host OS stack does the packet work. So both needed a
  user-space IP stack added. That part of the port is a fairly direct translation.
- **The LDN layer is where they differ, and it is the whole difficulty.**
  - Ryujinx's `ldn_mitm` mode already spoke the **ldn_mitm wire protocol** (UDP + TCP port 11452),
    byte compatible with a real Switch running the ldn_mitm homebrew. Ryubing's LAN Play work just
    had to carry those existing packets over the relay instead of over host sockets, which it did by
    extracting an `ILdnNetworkProvider` abstraction.
  - **Eden's LDN is not console compatible at all.** `LANDiscovery`
    (`src/core/hle/service/ldn/lan_discovery.cpp`) wraps `NetworkInfo`/`NodeInfo` in Eden's own
    `Network::LDNPacket` and routes them through a yuzu-style **multiplayer room**
    (`room_member->SendLdnPacket()`), using fake IPs the room hands out. There is no ldn_mitm
    protocol implementation anywhere in Eden.

So the decision that shaped this port: **implement the ldn_mitm protocol in Eden from scratch**
rather than tunnel Eden's `LDNPacket` over the relay. Tunnelling would have been far less work but
would only ever have interoperated with other Eden instances — not with Ryujinx, not with a console.
The owner asked for "a solution that could work for both of them", so interop was the point.

---

## 3. Architecture

```
   Switch game
        |
   BSD sockets (bsd.cpp)              LDN service (user_local_communication_service.cpp)
        |                                        |
   LanPlaySocket                       LanPlay::Discovery  (ldn_mitm protocol, port 11452)
        |                                        |
        +----------------+-----------------------+
                         |
             LanPlay::NetworkInterface     10.13.x.x, IPv4 + UDP + TCP + ICMP,
                         |                 fragmentation and reassembly
                  LanPlay::Client          relay framing, keepalive, relay fragments, auth
                         |
                    host UDP socket
                         |
                  LAN Play relay
```

### Relay wire format (from switch-lan-play's `src/lan-client.c`)

One UDP datagram per packet: `[1 byte type][payload]`. The top bit of the type byte marks encryption
and no known relay uses it.

| Type | Meaning |
| --- | --- |
| `0x00` KEEPALIVE | empty, every 10 s |
| `0x01` IPV4 | a complete IPv4 packet, starting at the IP header |
| `0x02` PING | ignored |
| `0x03` IPV4_FRAG | 16-byte header (`src[4] dst[4] id:u16 part:u8 total:u8 len:u16 pmtu:u16`, **big endian**) then a chunk |
| `0x04` AUTH_ME | challenge; answer is `sha1(sha1(password) + challenge)` then the user name |
| `0x10` INFO | text from the relay |

`10.13.37.1` is the address a switch-lan-play client answers on itself (its gateway / "fake internet"
feature), so it is never used by a console and never allocated here.

---

## 4. File map

### New: `src/core/internal_network/lan_play/` (transport + virtual stack)

| File | Role |
| --- | --- |
| `lan_play_protocol.{h,cpp}` | Relay wire format, constants, sha1 auth, `GetTickCountMs()`, `FormatAddress()`, and the `ReadBE*`/`WriteBE*` helpers everything else uses |
| `ipv4_packet.{h,cpp}` | IPv4 header parse/write, RFC 1071 checksum, transport (pseudo-header) checksum |
| `lan_play_client.{h,cpp}` | The embedded switch-lan-play client. Owns the host UDP socket, receive thread, keepalive thread, relay-level fragment reassembly, auth |
| `lan_play_config.{h,cpp}` | Parses `[user[:password]@]host[:port]`, resolves the host, validates the virtual IP setting |
| `lan_play_network_interface.{h,cpp}` | The virtual NIC. IPv4 in/out, IPv4 fragmentation and reassembly, UDP/TCP/ICMP demultiplexing, ICMP echo replies, port tables, the 100 ms timer thread |
| `lan_play_udp_endpoint.{h,cpp}` | A bound virtual UDP port. Queue mode (guest sockets) or push mode (LDN) |
| `lan_play_tcp_connection.{h,cpp}` | User-space TCP. **The most intricate file.** Handshake both directions, cumulative acks, single retransmission timer, orderly and abortive close |
| `lan_play_tcp_listener.{h,cpp}` | Listening virtual TCP port, accept queue or push mode |
| `virtual_address_allocator.{h,cpp}` | Picks and ICMP-probes the 10.13.x.x address |
| `lan_play_stack.{h,cpp}` | Per-session object (client + interface). Also the global `ApplySettings()` / `GetStack()` / `Shutdown()` entry points |
| `lan_play_socket.{h,cpp}` | `Network::SocketBase` implementation for guest sockets, with lazy host fallback |
| `lan_play_diagnostics.{h,cpp}` | Counters, per-packet tracing, 30 s summary, relay-silence warning |

### New: `src/core/hle/service/ldn/lan_play/` (LDN interop)

| File | Role |
| --- | --- |
| `ldn_mitm_protocol.{h,cpp}` | ldn_mitm frame header (magic `0x11451400`, 12 bytes, **little endian**), its run-length encoding, `BuildPacket()` |
| `lan_play_discovery.{h,cpp}` | The LDN backend. UDP scan/response, TCP session host and station, node table, `NetworkInfo` maintenance. Mirrors `LANDiscovery`'s public surface so the service can hold either |

### Modified

| File | Change |
| --- | --- |
| `src/core/hle/service/sockets/bsd.cpp` | Creates a `LanPlaySocket` in `SocketImpl` when a stack is active; special-cases `PollImpl` for LAN Play sockets (see §7) |
| `src/core/hle/service/ldn/user_local_communication_service.{h,cpp}` | New `lan_play_discovery` member; `LDN_DISPATCH` macro routes every call to whichever backend the session picked; `Initialize` chooses; `GetIpv4Address` returns the virtual address |
| `src/core/hle/service/nifm/nifm.cpp` | `GetLanPlayAddress()` helper; `GetCurrentIpAddress` and `GetCurrentIpConfigInfo` report the virtual address, but only once `IsGuestActive()` |
| `src/core/core.cpp` | `LanPlay::ApplySettings()` in `Impl::Initialize`, `LanPlay::Shutdown()` in `ShutdownMainProcess` |
| `src/common/settings.h` | `lan_play_enabled`, `lan_play_server`, `lan_play_virtual_ip` in `Category::Network` |
| `src/common/log_classes.inc` | `SUB(Network, LanPlay)` → the `Network_LanPlay` log class |
| `src/yuzu/configuration/configure_network.{h,cpp,ui}` | The LAN Play group box |
| `src/core/CMakeLists.txt` | All new sources |
| `docs/LanPlay.md` | Full documentation (new) |
| `.github/workflows/release.yml` | Release CI (new) |

---

## 5. Design decisions worth not re-litigating

**Namespace `Network::LanPlay::Relay`, not `::Protocol`.** The obvious name collides with Eden's
`Network::Protocol` socket enum and produces baffling errors. It was renamed for that reason.

**`GetTickCountMs()` is `steady_clock`, not `Common::Uptime`.** Self-contained and monotonic; every
deadline in the stack is in these milliseconds.

**All packet fields go through `ReadBE*`/`WriteBE*`, never memcpy of a struct.** No dependence on host
byte order or struct padding. The one deliberate exception is the LDN payload bodies (`NetworkInfo`,
`NodeInfo`), which *are* memcpy'd, because that is exactly what ldn_mitm and Ryujinx do — see §6.

**`shared_ptr` ownership for connections/endpoints/listeners.** The relay receive thread can be inside
`HandleSegment` while the owner closes the socket. `TcpConnection` uses `enable_shared_from_this` and
must be created via `TcpConnection::Create`. `Cleanup()` deliberately takes a `keep_alive` reference
before unregistering, because unregistering may drop the interface's last reference to `this`.

**The LDN backend is chosen at `Initialize` and never migrates.** A session already running stays on
the transport it started with. Re-entering the game's local multiplayer menu makes the game
re-initialise LDN, which is when the new backend takes effect. Same semantics as Ryubing.

**`nifm` vs `ldn` report different addresses on purpose.** `ldn::GetIpv4Address` always returns the
virtual address (it is only asked in the context of a local session). `nifm` returns the *host*
address until `Stack::IsGuestActive()`, so that merely enabling LAN Play does not disturb online play.
`IsGuestActive` is set by hosting/joining an LDN session, or by any socket traffic on the LAN Play
network (which is what entering a game's own LAN mode does). If you "fix" this to always report the
virtual address, you will break online play — that asymmetry is the feature.

**Host fallback in `LanPlaySocket`.** Only `10.13.0.0/16`, the broadcast addresses, and the host's own
network broadcast address go to the relay. Everything else lazily creates a real `Network::Socket` and
**replays every option the guest already set**, `SO_BROADCAST` included — without that a broadcast
fails with `EACCES` on Linux and macOS. That last address class (e.g. `192.168.0.255`) is a game's own
LAN mode broadcasting before the console started presenting its LAN Play address; it is re-addressed
to `10.13.255.255` on the way out, because the relay only floods that address.

**Address probing uses a throwaway source address.** A relay routes to whichever client last used an
address, so probing "as" the candidate would deliver the probe back to yourself and prove nothing.
Only an echo *reply* with the right identifier counts as "taken".

---

## 6. The byte-compatibility argument (read before touching the LDN path)

This is what makes cross-emulator play work, and the easiest thing to break by accident.

1. **Struct sizes already match.** Eden's `NetworkInfo` is `0x480` and `NodeInfo` is `0x40`
   (`static_assert`ed in `ldn_types.h`); Ryujinx's are declared with the same explicit sizes. Both
   mirror the real LDN structures. Bodies are sent as raw structs inside an ldn_mitm packet.

2. **`NodeInfo::ipv4_address` byte order.** Ryujinx stores a `uint` read big-endian from the address
   (10.13.4.7 → `0x0A0D0407`), which serialises little-endian on x86 as `07 04 0D 0A`. Eden's field is
   `std::array<u8,4>`, and Eden's *existing* room code already does `std::reverse` on it (commented
   "ntohl" in `lan_discovery.cpp`), producing the same `07 04 0D 0A`. **They already agreed.**
   `ToLdnAddress()` / `FromLdnAddress()` in `lan_play_discovery.cpp` are the single place this
   conversion happens — if addresses come out garbled cross-emulator, look there first and nowhere
   else.

3. **The ldn_mitm header is written field by field, little endian**, not memcpy'd, so it does not
   depend on padding. 12 bytes: `magic:u32, type:u8, compressed:u8, length:u16, decompress_length:u16,
   reserved[2]`.

4. **Run-length encoding.** A zero byte is followed by a count of *additional* zero bytes. Ported
   directly from Ryujinx's `LanProtocol.Compress`/`Decompress`, including the `BufferSize` (2048)
   bail-outs. `NetworkInfo` is mostly zeros, so nearly every real packet is compressed — this path is
   always exercised, not an edge case.

5. **MAC address** is `02:00:<the four address bytes>`, as ldn_mitm derives it, so two clients on one
   relay never collide.

---

## 7. Verification status — read this honestly

**What was verified:** every new and modified translation unit compiles clean with
`g++ -std=c++20 -fsyntax-only` against Eden's real headers. Several genuine bugs were found and fixed
that way (the `Protocol` namespace collision, missing `<fmt/format.h>`, missing `connected_address`
members).

To reproduce that check in a sandbox without Eden's toolchain:

```sh
apt-get install -y libfmt-dev libssl-dev libboost-dev
curl -sL https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.tar.gz | tar xz -C /tmp
mkdir -p /tmp/extra/ankerl && curl -sL \
  https://raw.githubusercontent.com/martinus/unordered_dense/v4.4.0/include/ankerl/unordered_dense.h \
  -o /tmp/extra/ankerl/unordered_dense.h

# The distro fmt is v9; Eden needs 10+, hence the header-only override.
g++ -std=c++20 -fsyntax-only -DFMT_HEADER_ONLY \
  -I/tmp/fmt-11.0.2/include -I/tmp/extra -Isrc -I. \
  src/core/internal_network/lan_play/lan_play_tcp_connection.cpp
```

**What was NOT verified — treat all of it as suspect:**

- **Never linked.** No full CMake build happened: the sandbox had CMake 3.28 against a required 3.31,
  and no bundled dependencies. Expect link errors and `-Werror` warnings on the first real build.
  The CI run on this branch is the first genuine compile.
- **Never run.** Not one packet has crossed a real relay. No game has been launched.
- **No automated tests.** Ryubing has `src/Ryujinx.Tests/HLE/LanPlayTests.cs` with an in-process relay
  covering broadcast, unicast, fragmentation, the TCP handshake, duplicate address detection,
  teardown/reconnect and a full LDN session. **Porting that harness is the single highest-value next
  task** — see §9.
- **Interop is by construction, not observed.** §6 is an argument, not a measurement.

---

## 8. Where the bugs most likely are, roughly in order

1. **`lan_play_tcp_connection.cpp` — the TCP state machine.** Most intricate file, most invariants,
   zero runtime testing. Specific worries:
   - `Connect()` has an awkward double-check around `state_changed.wait_for` that could in principle
     miss a fast transition; it is a polling loop with a 50 ms wait so it should recover, but it wants
     a careful read.
   - `HandleAcknowledgementLocked` does `acked--` when the FIN is acked; the interaction between
     `snd_nxt`, `fin_sent` and `fin_acked` is the fiddliest arithmetic in the file.
   - `TransmitPendingLocked` ignores the peer's window entirely by design (retransmission covers it).
     Fine for LDN's small exchanges, wrong for bulk transfer.
   - `SendSegmentLocked` is called both with and without the lock held via different paths — audit
     that every caller actually holds it. `Tick()`'s retransmit branch reacquires deliberately.
2. **`Discovery::OnStationData` swaps buffers in and out of a temporary `StreamReader`.** It works but
   it is ugly, and it is the kind of thing that breaks the moment a frame spans two TCP segments.
   Worth restructuring so `Station` holds a real `StreamReader`.
3. **`bsd.cpp` `PollImpl`.** A `poll` set containing any LAN Play socket falls back to a 5 ms polling
   loop, because the virtual interface cannot wake the platform's `poll`. Correct but CPU-hungry when
   a game polls with long timeouts. Also: the per-socket `Network::Poll(single, 0)` call inside that
   loop is unusual and deserves a second look. The right fix is an eventfd/self-pipe the interface can
   write to, so a mixed set can block properly.
4. **Lifetime around `Stack` teardown while sockets are open.** `ApplySettings()` can reset the stack
   while a `LanPlaySocket` still holds a `shared_ptr<Stack>` — that is intentional (the socket keeps
   working, its virtual side goes quiet) but it means the interface and client outlive the "active"
   stack. Verify no thread is left running after a mode change mid-game.
5. **`Discovery` locking.** `mutex` is held across some callback invocations (`lan_event()`,
   `UpdateNodes()` → `SendTo()` → the TCP stack). A deadlock between `Discovery::mutex`,
   `NetworkInterface::tcp_mutex` and `TcpConnection::mutex` is plausible. **Map the lock order before
   changing anything here.** This is the most likely source of a hang.
6. **Scan timing.** `Scan()` sleeps 1000 ms, copied from Eden's room backend. Over a relay with real
   latency that may be too short; Ryubing's connection test listens for 4 s.
7. **`Ipv4::TryParse` clamps `total_length` to the received size** when a sender pads the frame. Right
   for tolerance, but it means a truncated packet is silently treated as complete.

---

## 9. Suggested next steps, in order

1. **Get CI green.** Push the branch or dispatch the workflow; fix whatever the first real build says.
   Expect `-Werror` fallout (it is disabled in the workflow via `ENABLE_WERROR=OFF`, so also try a
   local build with it on), unused-parameter warnings, and possibly `<unordered_map>`-style missing
   includes that the sandbox's transitive includes happened to satisfy.
2. **Port the test harness** from `Ryubing-LanPlay/src/Ryujinx.Tests/HLE/{LanPlayTests.cs,
   TestLanPlayRelay.cs}` into `src/tests/`. An in-process relay that reproduces the routing behaviour
   (route on destination, flood `10.13.255.255`) plus the checksum/fragmentation/handshake cases would
   convert most of §8 from speculation into pass/fail. Eden already uses Catch2 and has
   `src/tests/core/internal_network/network.cpp` as a starting point.
3. **Two-instance manual test.** Two Eden builds, same relay, same game, enter local multiplayer. The
   log lines to watch:
   `connected to relay ...` → `using the virtual address ...` → `hosting an LDN session on ...` →
   `scan found N session(s)`. Enable `Network.LanPlay:Debug` for a line per packet.
4. **Cross-emulator test** — Eden hosting, Ryubing joining, and the reverse. This is what the whole
   ldn_mitm decision was for. If scanning finds nothing, dump the first packets on both sides
   (`Network.LanPlay:Trace` gives hex of the first eight relay datagrams) and compare the 12-byte
   ldn_mitm header and the `NodeInfo` address bytes against §6.
5. **Android UI.** The settings are read from the config file by category so they already work if
   written by hand, but nothing exposes them in the Android front end.
6. **Consider a connection-test button** like Ryubing's (`LanPlayConnectionTest.cs`): joins the relay
   without a game, sends a scan, reports what answered. It is the single most useful debugging
   affordance for users and separates "my relay setting is wrong" from "the game is not doing what I
   expect".

---

## 10. Reference material

- `docs/LanPlay.md` in this branch — full architecture, packet flow, platform notes, debugging,
  limitations. Start here.
- `Ryubing-LanPlay/docs/lan-play.md` — the spec this port follows. Its §11 (runtime mode changes) and
  §12 (platform support) explain reasoning that is only summarised on the Eden side.
- `Ryubing-LanPlay/src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator/LanPlay/` — the C# original,
  file for file the same layering as the C++ port.
- `Ryubing-LanPlay/src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator/LdnMitm/LanProtocol.cs` — the
  ldn_mitm protocol the C++ `ldn_mitm_protocol.cpp` was written against.
- [switch-lan-play](https://github.com/spacemeowx2/switch-lan-play) — `src/lan-client.c`,
  `src/packet.c`, `src/ipv4/ipv4.c`, `src/config.h` for the relay protocol.
- [ldn_mitm](https://github.com/spacemeowx2/ldn_mitm) — the homebrew a real console runs.

## 11. Settings quick reference

*LAN Play → Configure LAN Play* in the menu bar, or the same panel under *Emulation → Configure →
Network*. Stored in the Network category:

| Setting | Meaning |
| --- | --- |
| `lan_play_enabled` | Master switch |
| `lan_play_server` | `[user[:password]@]host[:port]`, port defaults to 11451 |
| `lan_play_virtual_ip` | Empty or `auto` for automatic, or force one inside `10.13.0.0/16` |

Changing them while a game runs joins or leaves the relay immediately. Re-saving unchanged values is
a no-op by design (`ApplySettings` compares against `active_server`/`active_virtual_ip`).
