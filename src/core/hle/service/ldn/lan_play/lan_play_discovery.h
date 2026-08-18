// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "core/hle/result.h"
#include "core/hle/service/ldn/lan_play/ldn_mitm_protocol.h"
#include "core/hle/service/ldn/ldn_types.h"
#include "core/internal_network/lan_play/lan_play_stack.h"

namespace Service::LDN::LanPlay {

/**
 * LDN over a switch-lan-play relay.
 *
 * This speaks the ldn_mitm protocol carried by the LAN Play virtual network interface, so the packets
 * on the relay are the ones a console running ldn_mitm sends. It replaces Eden's room based
 * LANDiscovery for the LAN Play multiplayer mode only; the room mode is untouched.
 *
 * The public surface mirrors LANDiscovery so that IUserLocalCommunicationService can hold either.
 */
class Discovery {
public:
    using LanEventFunc = std::function<void()>;

    explicit Discovery(std::shared_ptr<Network::LanPlay::Stack> stack_);
    ~Discovery();

    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    [[nodiscard]] State GetState() const;
    void SetState(State new_state);

    Result GetNetworkInfo(NetworkInfo& out_network) const;
    Result GetNetworkInfo(NetworkInfo& out_network, std::span<NodeLatestUpdate> out_updates);

    [[nodiscard]] DisconnectReason GetDisconnectReason() const;

    Result Scan(std::span<NetworkInfo> out_networks, s16& out_count, const ScanFilter& filter);

    Result SetAdvertiseData(std::span<const u8> data);

    Result OpenAccessPoint();
    Result CloseAccessPoint();

    Result OpenStation();
    Result CloseStation();

    Result CreateNetwork(const SecurityConfig& security_config, const UserConfig& user_config,
                         const NetworkConfig& network_config);
    Result DestroyNetwork();

    Result Connect(const NetworkInfo& network_info_, const UserConfig& user_config,
                   u16 local_communication_version);
    Result Disconnect();

    Result Initialize(LanEventFunc lan_event_, bool listening);
    Result Finalize();

    /// The virtual address the console presents on the relay, for ldn::GetIpv4Address.
    [[nodiscard]] u32 GetLocalAddress() const;

private:
    /// One station connected to a session we host.
    struct Station {
        std::shared_ptr<Network::LanPlay::TcpConnection> connection;
        std::vector<u8> buffer;
        std::size_t buffer_end{};
        NodeInfo node_info{};
        s8 node_id{-1};
        bool connected{};
    };

    /// Reassembles the ldn_mitm frames of one stream or datagram source.
    struct StreamReader {
        std::vector<u8> buffer;
        std::size_t buffer_end{};

        StreamReader() {
            buffer.resize(Ldn::BufferSize);
        }
    };

    bool StartUdp();
    bool StartHost();
    void StopHost();
    void StopStation();

    void OnUdpPacket(u32 source_address, u16 source_port, std::span<const u8> data);
    void OnStationData(Station& station, std::span<const u8> data);
    void OnHostData(std::span<const u8> data);

    /// Feeds bytes through the ldn_mitm framing and dispatches every complete packet.
    void ReadFramed(StreamReader& reader, std::span<const u8> data,
                    const std::function<void(Ldn::PacketType, std::span<const u8>)>& handle);

    void HandleScan(u32 source_address, u16 source_port);
    void HandleScanResponse(std::span<const u8> data);
    void HandleConnect(Station& station, std::span<const u8> data);
    void HandleSyncNetwork(std::span<const u8> data);

    void AcceptStation(std::shared_ptr<Network::LanPlay::TcpConnection> connection);
    void RemoveStation(const Network::LanPlay::TcpConnection* connection);

    void InitNetworkInfo();
    void InitNodeStateChange();
    void UpdateNodes(bool force_update = false);
    void ResetStations();
    bool IsNodeStateChanged();
    s8 LocateEmptyNode() const;

    [[nodiscard]] MacAddress GetFakeMac() const;
    Result GetNodeInfo(NodeInfo& node, const UserConfig& user_config,
                       u16 local_communication_version) const;

    bool SendTo(const std::shared_ptr<Network::LanPlay::TcpConnection>& connection,
                Ldn::PacketType type, std::span<const u8> data);

    std::shared_ptr<Network::LanPlay::Stack> stack;

    mutable std::mutex mutex;

    std::shared_ptr<Network::LanPlay::UdpEndpoint> udp_endpoint;
    std::shared_ptr<Network::LanPlay::TcpListener> tcp_listener;
    std::shared_ptr<Network::LanPlay::TcpConnection> host_connection;

    /// Per source framing state for the UDP discovery port.
    std::unordered_map<u64, StreamReader> udp_readers;
    StreamReader host_reader;

    std::vector<std::unique_ptr<Station>> stations;

    NetworkInfo network_info{};
    NodeInfo node_info{};

    std::array<NodeLatestUpdate, NodeCountMax> node_changes{};
    std::array<u8, NodeCountMax> node_last_states{};

    /// Scan replies collected between a scan request and its deadline, keyed by BSSID.
    std::vector<NetworkInfo> scan_results;

    State state{State::None};
    DisconnectReason disconnect_reason{DisconnectReason::None};

    bool initialized{};
    bool is_host{};

    Ssid fake_ssid{};

    std::condition_variable sync_received;
    bool sync_seen{};

    LanEventFunc lan_event;
};

} // namespace Service::LDN::LanPlay
