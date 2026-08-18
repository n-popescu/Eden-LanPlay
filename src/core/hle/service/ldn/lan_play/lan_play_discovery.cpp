// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <unordered_map>

#include "common/logging.h"
#include "core/hle/service/ldn/lan_play/lan_play_discovery.h"
#include "core/hle/service/ldn/ldn_results.h"
#include "core/internal_network/lan_play/lan_play_network_interface.h"
#include "core/internal_network/lan_play/lan_play_tcp_connection.h"
#include "core/internal_network/lan_play/lan_play_tcp_listener.h"
#include "core/internal_network/lan_play/lan_play_udp_endpoint.h"

namespace Service::LDN::LanPlay {

namespace {

using Network::LanPlay::FormatAddress;

/// How long a scan collects replies. A console answers immediately, so this is dominated by latency.
constexpr int ScanWaitMs = 1000;

constexpr int ConnectTimeoutMs = 4000;

/// How long Connect() waits for the host's first SyncNetwork before giving up.
constexpr int SyncTimeoutMs = 4000;

/**
 * ldn_mitm stores a node's address as the little endian form of the host ordered integer, which is
 * what a console puts on the wire and what Ryujinx's implementation reads back.
 */
Ipv4Address ToLdnAddress(u32 address) {
    return Ipv4Address{static_cast<u8>(address), static_cast<u8>(address >> 8),
                       static_cast<u8>(address >> 16), static_cast<u8>(address >> 24)};
}

u32 FromLdnAddress(const Ipv4Address& address) {
    return (static_cast<u32>(address[3]) << 24) | (static_cast<u32>(address[2]) << 16) |
           (static_cast<u32>(address[1]) << 8) | static_cast<u32>(address[0]);
}

bool IsFlagSet(ScanFilterFlag flag, ScanFilterFlag search_flag) {
    const auto flag_value = static_cast<u32>(flag);
    const auto search_flag_value = static_cast<u32>(search_flag);

    return (flag_value & search_flag_value) == search_flag_value;
}

template <typename T>
std::span<const u8> AsBytes(const T& value) {
    return std::span{reinterpret_cast<const u8*>(&value), sizeof(T)};
}

/// Copies a received body into a trivially copyable LDN struct, refusing a short one.
template <typename T>
bool TryReadStruct(std::span<const u8> data, T& out_value) {
    if (data.size() < sizeof(T)) {
        return false;
    }

    std::memcpy(&out_value, data.data(), sizeof(T));

    return true;
}

u64 MakeReaderKey(u32 address, u16 port) {
    return (static_cast<u64>(address) << 16) | port;
}

} // Anonymous namespace

Discovery::Discovery(std::shared_ptr<Network::LanPlay::Stack> stack_) : stack{std::move(stack_)} {
    host_reader = StreamReader{};

    // A console advertises a random SSID; games only ever compare it, never interpret it.
    std::mt19937 engine{std::random_device{}()};
    std::uniform_int_distribution<int> byte{0, 255};

    fake_ssid.length = static_cast<u8>(SsidLengthMax);

    for (std::size_t i = 0; i < SsidLengthMax; i++) {
        fake_ssid.raw[i] = static_cast<char>('a' + (byte(engine) % 26));
    }

    fake_ssid.raw[SsidLengthMax] = 0;
}

Discovery::~Discovery() {
    if (initialized) {
        Finalize();
    }
}

u32 Discovery::GetLocalAddress() const {
    return stack->GetAddress();
}

State Discovery::GetState() const {
    std::scoped_lock lock{mutex};

    return state;
}

void Discovery::SetState(State new_state) {
    std::scoped_lock lock{mutex};

    state = new_state;
}

Result Discovery::GetNetworkInfo(NetworkInfo& out_network) const {
    std::scoped_lock lock{mutex};

    if (state == State::AccessPointCreated || state == State::StationConnected) {
        out_network = network_info;

        return ResultSuccess;
    }

    return ResultBadState;
}

Result Discovery::GetNetworkInfo(NetworkInfo& out_network,
                                 std::span<NodeLatestUpdate> out_updates) {
    if (out_updates.size() > NodeCountMax) {
        return ResultInvalidBufferCount;
    }

    std::scoped_lock lock{mutex};

    if (state != State::AccessPointCreated && state != State::StationConnected) {
        return ResultBadState;
    }

    out_network = network_info;

    for (std::size_t i = 0; i < out_updates.size(); i++) {
        out_updates[i].state_change = node_changes[i].state_change;
        node_changes[i].state_change = NodeStateChange::None;
    }

    return ResultSuccess;
}

DisconnectReason Discovery::GetDisconnectReason() const {
    std::scoped_lock lock{mutex};

    return disconnect_reason;
}

MacAddress Discovery::GetFakeMac() const {
    // ldn_mitm derives the MAC from the address, so two consoles on the same relay never collide.
    const u32 address = stack->GetAddress();

    MacAddress mac{};

    mac.raw[0] = 0x02;
    mac.raw[1] = 0x00;
    mac.raw[2] = static_cast<u8>(address >> 24);
    mac.raw[3] = static_cast<u8>(address >> 16);
    mac.raw[4] = static_cast<u8>(address >> 8);
    mac.raw[5] = static_cast<u8>(address);

    return mac;
}

Result Discovery::GetNodeInfo(NodeInfo& node, const UserConfig& user_config,
                              u16 local_communication_version) const {
    node.mac_address = GetFakeMac();
    node.is_connected = 1;

    std::memcpy(node.user_name.data(), user_config.user_name.data(), UserNameBytesMax + 1);

    node.local_communication_version = static_cast<s16>(local_communication_version);
    node.ipv4_address = ToLdnAddress(stack->GetAddress());

    return ResultSuccess;
}

void Discovery::InitNetworkInfo() {
    network_info.common.bssid = GetFakeMac();
    network_info.common.channel = WifiChannel::Wifi24_6;
    network_info.common.link_level = LinkLevel::Good;
    network_info.common.network_type = PackedNetworkType::Ldn;
    network_info.common.ssid = fake_ssid;

    for (std::size_t i = 0; i < NodeCountMax; i++) {
        network_info.ldn.nodes[i] = {};
        network_info.ldn.nodes[i].node_id = static_cast<s8>(i);
        network_info.ldn.nodes[i].is_connected = 0;
    }
}

void Discovery::InitNodeStateChange() {
    for (auto& change : node_changes) {
        change.state_change = NodeStateChange::None;
    }

    for (auto& last_state : node_last_states) {
        last_state = 0;
    }
}

bool Discovery::IsNodeStateChanged() {
    bool changed = false;

    for (int i = 0; i < NodeCountMax; i++) {
        const u8 is_connected = network_info.ldn.nodes[i].is_connected;

        if (is_connected != node_last_states[i]) {
            node_changes[i].state_change |=
                is_connected ? NodeStateChange::Connect : NodeStateChange::Disconnect;
            node_last_states[i] = is_connected;
            changed = true;
        }
    }

    return changed;
}

s8 Discovery::LocateEmptyNode() const {
    for (int i = 1; i < NodeCountMax; i++) {
        if (network_info.ldn.nodes[i].is_connected == 0) {
            return static_cast<s8>(i);
        }
    }

    return -1;
}

bool Discovery::SendTo(const std::shared_ptr<Network::LanPlay::TcpConnection>& connection,
                       Ldn::PacketType type, std::span<const u8> data) {
    if (!connection) {
        return false;
    }

    const auto packet = Ldn::BuildPacket(type, data);

    return connection->Send(packet) == static_cast<s32>(packet.size());
}

bool Discovery::StartUdp() {
    udp_endpoint = stack->GetNetworkInterface().BindUdp(Ldn::DefaultPort);

    if (!udp_endpoint) {
        LOG_ERROR(Service_LDN, "LAN Play: could not bind the LDN discovery port {}.",
                  Ldn::DefaultPort);

        return false;
    }

    udp_endpoint->SetPacketHandler(
        [this](u32 source_address, u16 source_port, std::span<const u8> data) {
            OnUdpPacket(source_address, source_port, data);
        });

    return true;
}

void Discovery::OnUdpPacket(u32 source_address, u16 source_port, std::span<const u8> data) {
    // Our own broadcasts are already dropped by the interface, but a relay may still reflect them.
    if (source_address == stack->GetAddress()) {
        return;
    }

    std::scoped_lock lock{mutex};

    auto& reader = udp_readers[MakeReaderKey(source_address, source_port)];

    ReadFramed(reader, data, [&](Ldn::PacketType type, std::span<const u8> body) {
        switch (type) {
        case Ldn::PacketType::Scan:
            HandleScan(source_address, source_port);
            break;

        case Ldn::PacketType::ScanResponse:
            HandleScanResponse(body);
            break;

        default:
            LOG_DEBUG(Service_LDN, "LAN Play: ignoring ldn packet type {} on the discovery port.",
                      static_cast<int>(type));
            break;
        }
    });

    // A datagram is a complete message; never carry a partial frame across sources.
    reader.buffer_end = 0;
}

void Discovery::ReadFramed(StreamReader& reader, std::span<const u8> data,
                           const std::function<void(Ldn::PacketType, std::span<const u8>)>& handle) {
    std::size_t index = 0;

    while (index < data.size()) {
        if (reader.buffer_end < Ldn::HeaderSize) {
            const std::size_t copyable =
                std::min(data.size() - index, Ldn::HeaderSize - reader.buffer_end);

            std::memcpy(reader.buffer.data() + reader.buffer_end, data.data() + index, copyable);

            index += copyable;
            reader.buffer_end += copyable;
        }

        if (reader.buffer_end < Ldn::HeaderSize) {
            return;
        }

        Ldn::PacketHeader header{};

        if (!Ldn::TryReadHeader(reader.buffer, header) || header.magic != Ldn::LanMagic) {
            reader.buffer_end = 0;

            LOG_WARNING(Service_LDN, "LAN Play: invalid magic 0x{:08x} in an ldn packet.",
                        header.magic);

            return;
        }

        const std::size_t total_size = Ldn::HeaderSize + header.length;

        if (total_size > Ldn::BufferSize) {
            reader.buffer_end = 0;

            LOG_ERROR(Service_LDN, "LAN Play: ldn packet of {} bytes exceeds the {} byte limit.",
                      total_size, Ldn::BufferSize);

            return;
        }

        const std::size_t copyable = std::min(data.size() - index, total_size - reader.buffer_end);

        std::memcpy(reader.buffer.data() + reader.buffer_end, data.data() + index, copyable);

        index += copyable;
        reader.buffer_end += copyable;

        if (reader.buffer_end != total_size) {
            return;
        }

        std::span<const u8> body{reader.buffer.data() + Ldn::HeaderSize, header.length};

        std::vector<u8> decompressed;

        if (header.compressed == 1) {
            if (!Ldn::Decompress(body, decompressed)) {
                LOG_ERROR(Service_LDN, "LAN Play: could not decompress an ldn packet body.");

                reader.buffer_end = 0;

                return;
            }

            if (decompressed.size() != header.decompress_length) {
                LOG_ERROR(Service_LDN,
                          "LAN Play: decompressed ldn body is {} bytes, expected {}.",
                          decompressed.size(), header.decompress_length);

                reader.buffer_end = 0;

                return;
            }

            body = decompressed;
        }

        handle(header.type, body);

        reader.buffer_end = 0;
    }
}

void Discovery::HandleScan(u32 source_address, u16 source_port) {
    if (state != State::AccessPointCreated) {
        return;
    }

    LOG_DEBUG(Service_LDN, "LAN Play: answering a scan from {}.", FormatAddress(source_address));

    const auto packet = Ldn::BuildPacket(Ldn::PacketType::ScanResponse, AsBytes(network_info));

    udp_endpoint->SendTo(source_address, source_port, packet);
}

void Discovery::HandleScanResponse(std::span<const u8> data) {
    NetworkInfo info{};

    if (!TryReadStruct(data, info)) {
        LOG_WARNING(Service_LDN, "LAN Play: a scan response was {} bytes, expected {}.", data.size(),
                    sizeof(NetworkInfo));

        return;
    }

    const auto existing = std::find_if(scan_results.begin(), scan_results.end(),
                                       [&info](const NetworkInfo& entry) {
                                           return entry.common.bssid == info.common.bssid;
                                       });

    if (existing != scan_results.end()) {
        *existing = info;
    } else {
        scan_results.push_back(info);
    }
}

Result Discovery::Scan(std::span<NetworkInfo> out_networks, s16& out_count,
                       const ScanFilter& filter) {
    {
        std::scoped_lock lock{mutex};

        if (!udp_endpoint) {
            return ResultBadState;
        }

        scan_results.clear();

        const auto packet = Ldn::BuildPacket(Ldn::PacketType::Scan, {});

        udp_endpoint->SendTo(Network::LanPlay::Relay::SubnetBroadcast, Ldn::DefaultPort, packet);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{ScanWaitMs});

    std::scoped_lock lock{mutex};

    for (const auto& info : scan_results) {
        if (out_count >= static_cast<s16>(out_networks.size())) {
            break;
        }

        if (IsFlagSet(filter.flag, ScanFilterFlag::LocalCommunicationId) &&
            filter.network_id.intent_id.local_communication_id !=
                info.network_id.intent_id.local_communication_id) {
            continue;
        }

        if (IsFlagSet(filter.flag, ScanFilterFlag::SessionId) &&
            filter.network_id.session_id != info.network_id.session_id) {
            continue;
        }

        if (IsFlagSet(filter.flag, ScanFilterFlag::NetworkType) &&
            filter.network_type != static_cast<NetworkType>(info.common.network_type)) {
            continue;
        }

        if (IsFlagSet(filter.flag, ScanFilterFlag::Ssid) && filter.ssid != info.common.ssid) {
            continue;
        }

        if (IsFlagSet(filter.flag, ScanFilterFlag::SceneId) &&
            filter.network_id.intent_id.scene_id != info.network_id.intent_id.scene_id) {
            continue;
        }

        // A host that has not finished filling in its own node yet would show up as a nameless
        // session the game cannot join.
        if (info.ldn.nodes[0].user_name[0] == 0) {
            LOG_WARNING(Service_LDN, "LAN Play: ignoring a scan result with an empty user name.");

            continue;
        }

        out_networks[out_count++] = info;
    }

    LOG_INFO(Service_LDN, "LAN Play: scan found {} session(s) on the relay.", out_count);

    return ResultSuccess;
}

Result Discovery::SetAdvertiseData(std::span<const u8> data) {
    if (data.size() > AdvertiseDataSizeMax) {
        return ResultAdvertiseDataTooLarge;
    }

    std::scoped_lock lock{mutex};

    std::memcpy(network_info.ldn.advertise_data.data(), data.data(), data.size());
    network_info.ldn.advertise_data_size = static_cast<u16>(data.size());

    // Otherwise a station sees the session change under it and reports SessionKeepFailed.
    if (network_info.ldn.nodes[0].is_connected == 1) {
        UpdateNodes(true);
    }

    return ResultSuccess;
}

Result Discovery::OpenAccessPoint() {
    std::scoped_lock lock{mutex};

    disconnect_reason = DisconnectReason::None;

    if (state == State::None) {
        return ResultBadState;
    }

    ResetStations();
    state = State::AccessPointOpened;

    return ResultSuccess;
}

Result Discovery::CloseAccessPoint() {
    {
        std::scoped_lock lock{mutex};

        if (state == State::None) {
            return ResultBadState;
        }
    }

    if (GetState() == State::AccessPointCreated) {
        DestroyNetwork();
    }

    std::scoped_lock lock{mutex};

    ResetStations();
    state = State::Initialized;

    return ResultSuccess;
}

Result Discovery::OpenStation() {
    std::scoped_lock lock{mutex};

    disconnect_reason = DisconnectReason::None;

    if (state == State::None) {
        return ResultBadState;
    }

    ResetStations();
    state = State::StationOpened;

    return ResultSuccess;
}

Result Discovery::CloseStation() {
    if (GetState() == State::StationConnected) {
        Disconnect();
    }

    std::scoped_lock lock{mutex};

    ResetStations();
    state = State::Initialized;

    return ResultSuccess;
}

bool Discovery::StartHost() {
    tcp_listener = stack->GetNetworkInterface().ListenTcp(Ldn::DefaultPort);

    if (!tcp_listener) {
        LOG_ERROR(Service_LDN, "LAN Play: could not listen on the LDN session port {}.",
                  Ldn::DefaultPort);

        return false;
    }

    tcp_listener->SetConnectionHandler(
        [this](std::shared_ptr<Network::LanPlay::TcpConnection> connection) {
            AcceptStation(std::move(connection));
        });

    LOG_INFO(Service_LDN, "LAN Play: hosting an LDN session on {}:{}.",
             FormatAddress(stack->GetAddress()), Ldn::DefaultPort);

    return true;
}

void Discovery::AcceptStation(std::shared_ptr<Network::LanPlay::TcpConnection> connection) {
    auto* raw = connection.get();

    Station* station_ptr = nullptr;

    {
        std::scoped_lock lock{mutex};

        if (stations.size() >= StationCountMax) {
            LOG_WARNING(Service_LDN, "LAN Play: session is full, refusing {}.",
                        connection->Describe());

            connection->Abort();

            return;
        }

        auto station = std::make_unique<Station>();

        station->connection = connection;
        station->buffer.resize(Ldn::BufferSize);

        station_ptr = station.get();

        stations.push_back(std::move(station));
    }

    LOG_INFO(Service_LDN, "LAN Play: an LDN station connected from {}.", connection->Describe());

    connection->SetDataHandler([this, station_ptr](std::span<const u8> data) {
        OnStationData(*station_ptr, data);
    });

    connection->SetClosedHandler([this, raw] { RemoveStation(raw); });
}

void Discovery::OnStationData(Station& station, std::span<const u8> data) {
    std::scoped_lock lock{mutex};

    StreamReader reader;

    // The station owns its framing state; StreamReader is only a view over it here.
    reader.buffer.swap(station.buffer);
    reader.buffer_end = station.buffer_end;

    ReadFramed(reader, data, [&](Ldn::PacketType type, std::span<const u8> body) {
        switch (type) {
        case Ldn::PacketType::Connect:
            HandleConnect(station, body);
            break;

        default:
            LOG_DEBUG(Service_LDN, "LAN Play: ignoring ldn packet type {} from a station.",
                      static_cast<int>(type));
            break;
        }
    });

    station.buffer.swap(reader.buffer);
    station.buffer_end = reader.buffer_end;
}

void Discovery::HandleConnect(Station& station, std::span<const u8> data) {
    NodeInfo info{};

    if (!TryReadStruct(data, info)) {
        LOG_WARNING(Service_LDN, "LAN Play: a Connect body was {} bytes, expected {}.", data.size(),
                    sizeof(NodeInfo));

        return;
    }

    if (station.connected) {
        return;
    }

    const s8 node_id = LocateEmptyNode();

    if (node_id < 0) {
        LOG_WARNING(Service_LDN, "LAN Play: no free node for {}.", station.connection->Describe());

        station.connection->Abort();

        return;
    }

    station.node_id = node_id;
    station.node_info = info;
    station.node_info.node_id = node_id;
    station.node_info.is_connected = 1;
    station.connected = true;

    network_info.ldn.nodes[node_id] = station.node_info;

    UpdateNodes();
}

void Discovery::RemoveStation(const Network::LanPlay::TcpConnection* connection) {
    std::scoped_lock lock{mutex};

    const auto it = std::find_if(stations.begin(), stations.end(), [connection](const auto& entry) {
        return entry->connection.get() == connection;
    });

    if (it == stations.end()) {
        return;
    }

    const s8 node_id = (*it)->node_id;

    stations.erase(it);

    if (node_id > 0) {
        network_info.ldn.nodes[node_id] = {};
        network_info.ldn.nodes[node_id].node_id = node_id;
        network_info.ldn.nodes[node_id].is_connected = 0;

        UpdateNodes();
    }
}

void Discovery::UpdateNodes(bool force_update) {
    u8 count = 1;

    for (const auto& station : stations) {
        if (!station->connected || !station->connection->IsConnected()) {
            continue;
        }

        count++;

        network_info.ldn.nodes[station->node_id] = station->node_info;
    }

    const bool changed = force_update || network_info.ldn.node_count != count;

    network_info.ldn.node_count = count;

    for (const auto& station : stations) {
        if (!station->connection->IsConnected()) {
            continue;
        }

        if (!SendTo(station->connection, Ldn::PacketType::SyncNetwork, AsBytes(network_info))) {
            LOG_ERROR(Service_LDN, "LAN Play: could not sync the session to node {}.",
                      station->node_id);
        }
    }

    if (IsNodeStateChanged() || changed) {
        LOG_INFO(Service_LDN, "LAN Play: LDN session now has {} node(s).", count);

        if (lan_event) {
            lan_event();
        }
    }
}

void Discovery::ResetStations() {
    for (const auto& station : stations) {
        if (station->connection) {
            station->connection->Abort();
        }
    }

    stations.clear();
}

Result Discovery::CreateNetwork(const SecurityConfig& security_config,
                                const UserConfig& user_config,
                                const NetworkConfig& network_config) {
    {
        std::scoped_lock lock{mutex};

        if (state != State::AccessPointOpened) {
            return ResultBadState;
        }
    }

    if (!StartHost()) {
        return ResultAccessPointConnectionFailed;
    }

    std::scoped_lock lock{mutex};

    is_host = true;

    InitNetworkInfo();

    network_info.ldn.node_count_max = network_config.node_count_max;
    network_info.ldn.security_mode = security_config.security_mode;

    network_info.common.channel = network_config.channel == WifiChannel::Default
                                      ? WifiChannel::Wifi24_6
                                      : network_config.channel;

    std::independent_bits_engine<std::mt19937, 64, u64> bits_engine;

    network_info.network_id.session_id.high = bits_engine();
    network_info.network_id.session_id.low = bits_engine();
    network_info.network_id.intent_id = network_config.intent_id;

    NodeInfo& node0 = network_info.ldn.nodes[0];

    if (GetNodeInfo(node0, user_config, network_config.local_communication_version).IsError()) {
        return ResultAccessPointConnectionFailed;
    }

    node0.node_id = 0;
    node0.is_connected = 1;

    state = State::AccessPointCreated;

    InitNodeStateChange();
    UpdateNodes();

    stack->MarkGuestActive("hosting a local session");

    return ResultSuccess;
}

void Discovery::StopHost() {
    std::vector<std::unique_ptr<Station>> outstanding;
    std::shared_ptr<Network::LanPlay::TcpListener> listener;

    {
        std::scoped_lock lock{mutex};

        outstanding.swap(stations);
        listener = std::move(tcp_listener);
        is_host = false;
    }

    for (const auto& station : outstanding) {
        if (station->connection) {
            station->connection->Abort();
        }
    }

    if (listener) {
        listener->Close();
    }
}

Result Discovery::DestroyNetwork() {
    StopHost();

    std::scoped_lock lock{mutex};

    state = State::AccessPointOpened;

    if (lan_event) {
        lan_event();
    }

    return ResultSuccess;
}

void Discovery::OnHostData(std::span<const u8> data) {
    std::scoped_lock lock{mutex};

    ReadFramed(host_reader, data, [&](Ldn::PacketType type, std::span<const u8> body) {
        switch (type) {
        case Ldn::PacketType::SyncNetwork:
            HandleSyncNetwork(body);
            break;

        default:
            LOG_DEBUG(Service_LDN, "LAN Play: ignoring ldn packet type {} from the session host.",
                      static_cast<int>(type));
            break;
        }
    });
}

void Discovery::HandleSyncNetwork(std::span<const u8> data) {
    NetworkInfo info{};

    if (!TryReadStruct(data, info)) {
        LOG_WARNING(Service_LDN, "LAN Play: a SyncNetwork body was {} bytes, expected {}.",
                    data.size(), sizeof(NetworkInfo));

        return;
    }

    network_info = info;

    if (state == State::StationOpened) {
        state = State::StationConnected;
    }

    sync_seen = true;
    sync_received.notify_all();

    if (IsNodeStateChanged() && lan_event) {
        lan_event();
    }
}

Result Discovery::Connect(const NetworkInfo& network_info_, const UserConfig& user_config,
                          u16 local_communication_version) {
    if (network_info_.ldn.node_count == 0) {
        return ResultInvalidNodeCount;
    }

    const u32 host_address = FromLdnAddress(network_info_.ldn.nodes[0].ipv4_address);

    LOG_INFO(Service_LDN, "LAN Play: joining the LDN session hosted by {}.",
             FormatAddress(host_address));

    auto& network_interface = stack->GetNetworkInterface();

    auto connection = Network::LanPlay::TcpConnection::Create(
        network_interface, network_interface.AllocateTcpPort(), host_address, Ldn::DefaultPort);

    connection->SetDataHandler([this](std::span<const u8> data) { OnHostData(data); });

    if (!connection->Connect(ConnectTimeoutMs)) {
        LOG_ERROR(Service_LDN, "LAN Play: could not reach the LDN session host at {}:{}.",
                  FormatAddress(host_address), Ldn::DefaultPort);

        return ResultConnectionFailed;
    }

    NodeInfo my_node{};

    if (GetNodeInfo(my_node, user_config, local_communication_version).IsError()) {
        connection->Abort();

        return ResultConnectionFailed;
    }

    {
        std::scoped_lock lock{mutex};

        host_connection = connection;
        host_reader.buffer_end = 0;
        sync_seen = false;
        node_info = my_node;

        InitNodeStateChange();
    }

    if (!SendTo(connection, Ldn::PacketType::Connect, AsBytes(my_node))) {
        LOG_ERROR(Service_LDN, "LAN Play: could not send the join request to the session host.");

        return ResultConnectionFailed;
    }

    stack->MarkGuestActive("joining a local session");

    std::unique_lock lock{mutex};

    if (!sync_received.wait_for(lock, std::chrono::milliseconds{SyncTimeoutMs},
                                [this] { return sync_seen; })) {
        LOG_ERROR(Service_LDN, "LAN Play: the session host did not answer the join request.");

        return ResultConnectionFailed;
    }

    return ResultSuccess;
}

void Discovery::StopStation() {
    std::shared_ptr<Network::LanPlay::TcpConnection> connection;

    {
        std::scoped_lock lock{mutex};

        connection = std::move(host_connection);
    }

    if (connection) {
        connection->Close();
    }
}

Result Discovery::Disconnect() {
    StopStation();

    std::scoped_lock lock{mutex};

    state = State::StationOpened;

    if (lan_event) {
        lan_event();
    }

    return ResultSuccess;
}

Result Discovery::Initialize(LanEventFunc lan_event_, bool listening) {
    {
        std::scoped_lock lock{mutex};

        if (initialized) {
            return ResultSuccess;
        }

        lan_event = std::move(lan_event_);
    }

    if (listening && !StartUdp()) {
        return ResultAccessPointConnectionFailed;
    }

    std::scoped_lock lock{mutex};

    state = State::Initialized;
    initialized = true;

    LOG_INFO(Service_LDN, "LAN Play: LDN initialized on {} over the relay.",
             FormatAddress(stack->GetAddress()));

    return ResultSuccess;
}

Result Discovery::Finalize() {
    const State current = GetState();

    if (current == State::AccessPointCreated) {
        DestroyNetwork();
    }

    if (current == State::StationConnected) {
        Disconnect();
    }

    StopHost();
    StopStation();

    std::shared_ptr<Network::LanPlay::UdpEndpoint> endpoint;

    {
        std::scoped_lock lock{mutex};

        endpoint = std::move(udp_endpoint);
        udp_readers.clear();
        initialized = false;
        state = State::None;
    }

    if (endpoint) {
        endpoint->Close();
    }

    return ResultSuccess;
}

} // namespace Service::LDN::LanPlay
