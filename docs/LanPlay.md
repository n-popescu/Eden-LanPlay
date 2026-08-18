# LAN Play

Eden can join a [switch-lan-play](https://github.com/spacemeowx2/switch-lan-play) relay by itself.
No second PC, no external `switch-lan-play` process, no libpcap, no virtual adapter, no host routing
changes and no manual 10.13.x.x configuration on the host are needed.

Enable **LAN Play** under *Emulation → Configure → Network*, enter the relay (`host:port`, for
example `switch.example.com:11451`), and launch a game that supports local wireless multiplayer.
Every player on the same relay — Eden, Ryujinx, or a real console running ldn_mitm — can see each
other.

---

## 1. Why this exists as a separate layer

Eden already has two multiplayer paths, and neither could reach a LAN Play relay:

| Layer | Where |
| --- | --- |
| Guest BSD sockets | `core/hle/service/sockets/bsd.cpp` → `Network::SocketBase` |
| Host sockets | `core/internal_network/network.cpp` (`Network::Socket`) |
| Room socket proxy | `core/internal_network/socket_proxy.cpp` (`Network::ProxySocket`) |
| LDN over a room | `core/hle/service/ldn/lan_discovery.cpp` → `RoomMember::SendLdnPacket` |

Two findings drove the design:

* **Eden has no emulated Ethernet, ARP, IPv4 or UDP layer.** Guest socket calls are translated one
  to one into host socket calls, and the host OS stack does all the packet work. A LAN Play relay
  expects complete IPv4 packets, so that layer had to be added.
* **Eden's LDN is not console compatible.** `LANDiscovery` wraps `NetworkInfo` and `NodeInfo` in
  Eden's own `Network::LDNPacket` and routes them through a yuzu style multiplayer room, using fake
  IPs the room hands out. A real console, and Ryujinx's ldn_mitm mode, instead speak the
  **ldn_mitm protocol** on UDP/TCP port 11452. Tunnelling `LDNPacket` over the relay would only ever
  have worked between two Eden instances, so the ldn_mitm protocol is implemented instead.

## 2. How switch-lan-play works

Framing is one UDP datagram per packet: `[1 byte type][payload]`, where the top bit of the type byte
marks encryption (unused by known relays).

| Type | Meaning |
| --- | --- |
| `0x00` KEEPALIVE | empty, sent every 10 seconds |
| `0x01` IPV4 | a complete IPv4 packet, starting at the IP header |
| `0x02` PING | ignored by the client |
| `0x03` IPV4_FRAG | 16 byte header (`src[4] dst[4] id:u16 part:u8 total:u8 len:u16 pmtu:u16`, big endian) followed by a chunk |
| `0x04` AUTH_ME | challenge; the answer is `sha1(sha1(password) + challenge)` followed by the user name |
| `0x10` INFO | text from the relay |

The relay is game agnostic. It routes on the destination address inside the IPv4 packet and floods
the subnet broadcast address `10.13.255.255` to every client. `10.13.37.1` is the address a
switch-lan-play client answers on itself (its gateway / "fake internet" feature), so it is never
used by a console and never handed out here.

## 3. Architecture

```
   Switch game
        |
   BSD sockets (bsd.cpp)                LDN service (user_local_communication_service.cpp)
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

## 4. Files

The transport and virtual stack, in `src/core/internal_network/lan_play/`:

| File | Role |
| --- | --- |
| `lan_play_protocol.{h,cpp}` | relay wire format, network constants, auth response, tick clock |
| `ipv4_packet.{h,cpp}` | IPv4 header reading and writing, internet and transport checksums |
| `lan_play_client.{h,cpp}` | the embedded switch-lan-play client: relay socket, keepalive, relay level fragmentation and reassembly, auth, server messages |
| `lan_play_config.{h,cpp}` | parses `[user[:password]@]host[:port]` and the virtual address setting |
| `lan_play_network_interface.{h,cpp}` | the virtual NIC: addressing, IPv4 in and out, fragmentation, reassembly, demultiplexing, ICMP echo |
| `lan_play_udp_endpoint.{h,cpp}` | a bound UDP port with a receive queue and a push mode |
| `lan_play_tcp_connection.{h,cpp}` | user space TCP: handshake, cumulative acknowledgements, retransmission, orderly and abortive close |
| `lan_play_tcp_listener.{h,cpp}` | listening TCP port and accept queue |
| `virtual_address_allocator.{h,cpp}` | picks and probes the 10.13.x.x address |
| `lan_play_stack.{h,cpp}` | the client plus the interface for one session, and the settings entry point |
| `lan_play_socket.{h,cpp}` | `Network::SocketBase` for the emulated console's sockets, with host fallback |
| `lan_play_diagnostics.{h,cpp}` | counters, per packet tracing, periodic summary, relay silence warning |

The LDN interop, in `src/core/hle/service/ldn/lan_play/`:

| File | Role |
| --- | --- |
| `ldn_mitm_protocol.{h,cpp}` | the ldn_mitm frame header, magic `0x11451400` and its run length encoding |
| `lan_play_discovery.{h,cpp}` | the LDN backend: UDP scan and response, TCP session host and station, node table |

Wiring:

* `core/hle/service/sockets/bsd.cpp` — creates a `LanPlaySocket` when the stack is active, and
  answers a `poll` that contains one directly, because a LAN Play socket has no host descriptor.
* `core/hle/service/ldn/user_local_communication_service.cpp` — picks the LDN backend when the game
  initialises LDN, and reports the virtual address from `ldn::GetIpv4Address`.
* `core/hle/service/nifm/nifm.cpp` — reports the virtual address once the game is actually using the
  LAN Play network.
* `core/core.cpp` — applies the settings at boot and tears the stack down at shutdown.
* `common/settings.h`, `yuzu/configuration/configure_network.*` — the settings and the Qt UI.
* `common/log_classes.inc` — adds the `Network.LanPlay` log class.

## 5. Packet flow

Eden → relay:

```
game sendto(10.13.4.7:11452)  ->  LanPlaySocket  ->  UdpEndpoint
  -> NetworkInterface: UDP header + checksum, IPv4 header (src 10.13.x.x, dst 10.13.4.7),
     fragmented at 1500 bytes if needed
  -> Client: [0x01][IPv4 packet]  (or [0x03][frag header][chunk] when a path MTU is set)
  -> host UDP socket -> relay -> the client that owns 10.13.4.7
```

relay → Eden:

```
relay -> host UDP socket -> Client: type check, relay fragment reassembly
  -> NetworkInterface: parse IPv4, drop packets not addressed to us or looped back,
     reassemble IPv4 fragments
  -> UDP port table / TCP connection table / ICMP
  -> UdpEndpoint queue or TcpConnection buffer
  -> LanPlaySocket::RecvFrom  ->  game recvfrom
```

## 6. Virtual addressing

The host keeps its own address (for example 192.168.1.50); the emulated console gets a 10.13.x.x
address that only exists inside Eden. When *Virtual IP* is empty, an address is picked at random
inside 10.13.0.0/16, skipping `.0`, `.255`, `10.13.0.1` and `10.13.37.1`, and probed first: an ICMP
echo request is sent to the candidate from a throwaway address, and an echo reply means the address
is taken. Up to four candidates are tried.

The probe uses a throwaway source because a relay routes to whichever client last used an address,
so probing "as" the candidate would only reach ourselves.

Right after joining, the interface announces itself with a broadcast ICMP echo request, so the relay
knows which client owns the address before anybody tries to reach it.

## 7. Interoperability with Ryujinx and real consoles

The LDN structures are byte compatible by construction, which is what makes cross-emulator play work:

* `NetworkInfo` is 0x480 bytes and `NodeInfo` is 0x40 bytes in both emulators, matching the real LDN
  structures, and both are sent as raw structures inside an ldn_mitm packet body.
* `NodeInfo::ipv4_address` is stored as the little endian form of the host ordered address, which is
  what Eden's room backend already did (`std::reverse`, commented "ntohl" in `lan_discovery.cpp`) and
  what Ryujinx's `uint Ipv4Address` serialises to. `ToLdnAddress` / `FromLdnAddress` in
  `lan_play_discovery.cpp` are the single place that conversion happens.
* The ldn_mitm header is written field by field in little endian rather than memcpy-ed, so the
  layout does not depend on the host's byte order or on struct padding.
* The MAC address is derived from the virtual address (`02:00:<address>`), as ldn_mitm does, so two
  clients on one relay never collide.

## 8. Relationship with the room backend

The two LDN backends are mutually exclusive and chosen when the game initialises LDN:

* **LAN Play** uses `LanPlay::Discovery` over the virtual interface. Selected whenever a LAN Play
  stack is active.
* **The multiplayer room** keeps using `LANDiscovery` and `ProxySocket`, untouched.

A session that is already running does not migrate: leaving and re-entering the game's local
multiplayer menu makes the game re-initialise LDN, at which point the new backend is used.

## 9. Interaction with online play

LAN Play stays out of the way until it is actually used:

* **Traffic.** Only what is addressed to the LAN Play network goes to the relay: 10.13.0.0/16, the
  broadcast addresses, and the broadcast address of one of the host's own networks (192.168.0.255 for
  a host on 192.168.0.91/24). That last case is a game's own LAN mode broadcasting before the console
  started presenting its LAN Play address, so it is re-addressed to 10.13.255.255 on the way out.
  Everything else keeps using the host network through the socket's host fallback, and a line is
  logged the first time that fallback is used. Options the game set on the socket before the fallback
  existed are replayed on it, `SO_BROADCAST` included, without which a broadcast fails with `EACCES`
  on Linux and macOS.
* **The console's address.** `ldn::GetIpv4Address` always answers with the LAN Play address, because
  it is only asked in the context of a local session. `nifm::GetCurrentIpAddress` and
  `GetCurrentIpConfigInfo` only answer with it once the game is actually using the LAN Play network
  (`Stack::IsGuestActive`): hosting or joining a local session, or exchanging any traffic on the LAN
  Play network. Before that, the host address is reported exactly as it would be with LAN Play off.
* **Ports.** LAN Play binds virtual ports only, so it cannot collide with a port the online stack
  uses. Even a virtual port below 1024 works without privileges, because that port only exists
  inside Eden.

## 10. Platform support

Windows, Linux and macOS behave the same, because everything happens on top of one ordinary UDP
socket:

* **No libpcap, no adapter, no privileges.** Nothing is captured or injected on a host interface.
* **No routing, firewall or interface changes.** Only outbound UDP to the relay is used.
* **Byte order is explicit.** All packet fields are read and written with the `ReadBE*`/`WriteBE*`
  helpers, so nothing depends on the host being little endian.
* **Teardown does not rely on platform behaviour.** Closing a socket does not reliably abort a
  blocking receive on Unix, so the relay receive loop wakes up twice a second and checks its own stop
  flag.
* **The one platform specific call** is `SIO_UDP_CONNRESET`, disabled on Windows only: without it a
  Windows UDP socket fails its *next* receive with `WSAECONNRESET` after an ICMP "port unreachable",
  which happens whenever the relay is briefly down. The receive loop also treats that error,
  `WSAENETRESET`, truncated datagrams and `EINTR` as recoverable.

## 11. Configuration

*Emulation → Configure → Network → LAN Play*:

* **Join a switch-lan-play relay** — the master switch.
* **Relay Server** — `host:port`, for example `switch.example.com:11451`. The port defaults to 11451
  when omitted. If the relay requires a login, use `user:password@host:port`.
* **Virtual IP** — leave empty for an automatic address, or force one inside `10.13.0.0/16`.

Stored in the Network category as `lan_play_enabled`, `lan_play_server` and `lan_play_virtual_ip`.

The settings can be changed while a game is running: saving them joins or leaves the relay
immediately, and re-saving unchanged settings does nothing.

## 12. Debugging a session

**Normal log (info).** Joining, the chosen virtual address, LDN sessions being hosted or joined, scan
results, messages sent by the relay, and a traffic summary every 30 seconds plus a final one when the
session ends:

```
[Network.LanPlay] connected to relay switch.example.com:11451.
[Network.LanPlay] using the virtual address 10.13.42.7.
[Service.LDN]     LAN Play: hosting an LDN session on 10.13.42.7:11452.
[Network.LanPlay] status: relay switch.example.com:11451, sent 214 packets (33012 bytes,
  4 keepalives), received 198 packets (30122 bytes), udp 210/196, tcp 3/1, icmp 1/1,
  dropped NotForUs=87
```

**Warnings that point at the usual causes.** The relay going quiet is reported once, and again when
it comes back:

```
[Network.LanPlay] nothing received from the relay ... in 30 seconds. Check the address and port,
  that outbound UDP is not blocked, and that the relay is up.
[Network.LanPlay] the relay ... is answering again.
```

**Per packet logs.** Add `Network.LanPlay:Debug` to the log filter for a line per packet in each
direction plus a line for every dropped packet with the reason. Drop reasons: `Malformed`,
`NotForUs`, `LoopedBack`, `BadHeaderChecksum`, `BadTransportChecksum`, `NoUdpEndpoint`,
`NoTcpConnection`, `Reassembly`, `RelayFragment`, `UnsupportedProtocol`, `QueueFull`. `NotForUs` and
`LoopedBack` are normal on a relay that floods, and are counted but not logged per packet.

**Trace logs.** With `Network.LanPlay:Trace`, the first eight non-keepalive relay datagrams are
dumped as hex, which is what to attach when the framing itself is suspect.

Incoming packets are validated the way a real stack does: the IPv4 header checksum and the UDP (when
present) and TCP checksums are verified, and failures are counted rather than silently ignored.

## 13. Known limitations

* **Not yet validated against real hardware or against Ryujinx on a live relay.** The
  implementation is byte compatible by construction (see section 7) but the interop has not been
  confirmed end to end.
* **TCP is deliberately minimal.** In order delivery with cumulative acknowledgements, a single
  retransmission timer and no window based pacing: out of order segments are dropped and recovered by
  retransmission. That matches the small, low latency exchanges of LDN sessions, but a game that
  pushes a lot of TCP data over LAN Play may see lower throughput than on hardware.
* **Relay level fragmentation is implemented but off by default** (`path_mtu` is 0), like the
  reference client without `--pmtu`. Oversized packets are fragmented at the IPv4 level instead.
  Incoming relay fragments are always reassembled.
* **No gateway or "fake internet".** `10.13.37.1` is not emulated, and the relay's socks5 based
  internet feature is not implemented. Regular internet traffic keeps using the host stack.
* **`poll` on a mixed set is polled rather than blocking.** A `poll` call that contains a LAN Play
  socket falls back to a 5 ms polling loop, because the virtual interface cannot wake the platform's
  `poll`. Games that poll with long timeouts on such a set pay a little CPU.
* **Address probing is best effort**, because a relay cannot answer "who has this address" the way
  ARP does on a real LAN. With a /16 the chance of a collision is negligible, and an address can be
  forced in the settings.
* **No IPv6 on the virtual network**, matching switch-lan-play.
* **Relay authentication** is implemented from the reference client's auth type 0 only; other auth
  types are logged and ignored.
* **The Android front end has no LAN Play UI yet.** The settings are read from the configuration
  file, so they work if written by hand, but nothing exposes them.
* **No automated tests.** Ryujinx's implementation has an in-process relay and a test suite; the
  equivalent does not exist here yet.
