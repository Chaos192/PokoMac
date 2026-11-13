// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Local wireless over a real LAN in ldn_mitm's wire format ([net] protocol = lan, services/ldn.cpp MitmLdn): the protocol of
// spacemeowx2's ldn_mitm Atmosphere module and of Ryujinx's "ldn_mitm" multiplayer mode, so AnySwitch players meet Ryujinx players and
// consoles running ldn_mitm on the same network. Game-independent; no relay, no room codes (a LAN of its own only).
//
// Wire (after Ryujinx src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator/LdnMitm, MIT, Copyright (c) Ryujinx Team and Contributors:
// LanProtocol.cs, LanDiscovery.cs, Types/LanPacketHeader.cs, Types/LanPacketType.cs; the layouts and the zero run-length scheme are
// ported, the code is our own):
//   UDP and TCP port 11452. Every packet: a 12-byte header (u32 magic 0x11451400, u8 type, u8 compressed, u16 length on the wire, u16
//   length decompressed, 2 reserved; little-endian) and the payload, at most 2048 bytes in all. Types: 0 Scan (UDP broadcast, no payload),
//   1 ScanResponse (UDP unicast back, the host's NetworkInfo, 0x480), 2 Connect (TCP, station -> host, the station's NodeInfo, 0x40),
//   3 SyncNetwork (TCP, host -> stations, the NetworkInfo after every change). Payloads are compressed: a non-zero byte as it is, a run of
//   zeros as 00 n (n = the zeros after the first, 0..255). A packet from this node's own address is ignored.
//   The game's own traffic runs on the real LAN addresses (LanLink): node IPs in the NetworkInfo are the hosts' LAN IPv4 addresses.
// Beyond Ryujinx (its flaws, kept out): the host enforces node_count_max and the station accept policy (a refused station's TCP closes,
// its Connect answers ConnectRejected), and a station learns that the host left (its TCP closes: DestroyedByUser, an error: SignalLost).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vnet.h"

namespace nxrt::vnet::mitm {

constexpr uint16_t kPort = 11452;
constexpr uint16_t kVtcpPort = 11453;   // TCP between AnySwitch nodes of a LAN network (vsock vtcp) rides here; not ldn_mitm's
constexpr uint32_t kMagic = 0x11451400u;
constexpr size_t kHeader = 12, kBufferSize = 2048, kInfoSize = 0x480, kNodeSize = 0x40;
enum Type : uint8_t { kScan = 0, kScanResponse = 1, kConnect = 2, kSyncNetwork = 3 };

using Info = std::array<uint8_t, kInfoSize>;   // nn::ldn NetworkInfo, as the game reads it
using Node = std::array<uint8_t, kNodeSize>;   // nn::ldn NodeInfo

// NetworkInfo / NodeInfo offsets (Switchbrew "LDN services", Ryujinx Ldn/Types).
constexpr size_t kInfoNodeCountMax = 0x66, kInfoNodeCount = 0x67, kInfoNodes = 0x68;
constexpr size_t kNodeIp = 0, kNodeMac = 4, kNodeId = 10, kNodeConnected = 11, kNodeName = 12;

// The zero run-length scheme. compress: false when the result would not fit kBufferSize. decompress: false on a malformed input or one
// that does not come to exactly `want` bytes.
bool compress(const uint8_t* p, size_t n, std::vector<uint8_t>* out);
bool decompress(const uint8_t* p, size_t n, size_t want, std::vector<uint8_t>* out);
std::vector<uint8_t> packet(Type t, const uint8_t* p = nullptr, size_t n = 0);   // header + (compressed) payload
// One packet at the start of b: 0 when it is not complete yet, -1 when it is malformed, else its size on the wire.
int parse(const uint8_t* b, size_t n, Type* t, std::vector<uint8_t>* payload);

// The LAN interface: [net] lan_ip = auto (the first up, non-loopback IPv4 interface with a private address, else the first one) or an
// IPv4 address of this machine. False with *why when there is none.
bool pick_interface(const std::string& setting, uint32_t* ip, uint32_t* mask, std::string* why);

// ---- the game's datagrams on the real LAN -------------------------------------------------------------------------------------------------
// A vnet::Link whose datagrams are host UDP datagrams between LAN addresses: each port a game socket binds (open_port) or sends from gets a
// host UDP socket bound to it on this node's address (on macOS and Linux: every address, so subnet broadcasts arrive), with a reader thread.
// Destinations: the LAN's subnet and its broadcast address (reaches, netmask). vtcp segments (ports 0/0) go between AnySwitch nodes on
// kVtcpPort. Datagrams from this node's own address are dropped (the OS loops broadcasts back).
std::shared_ptr<Link> make_lan_link(uint32_t ip, uint32_t mask);

// ---- discovery and the control links ---------------------------------------------------------------------------------------------------
class Net {
public:
    struct Events {
        std::function<void()> changed;               // the network info changed (host: a station came or went; station: a SyncNetwork)
        std::function<void(bool orderly)> lost;      // station: the host's link closed (orderly: a clean close, else an error)
        std::function<bool(const Node&)> admit;      // host: may this station join (the accept policy)
    };
    // ip/mask: this node's LAN interface; peers: extra unicast scan targets ([net] lan_peers: a network that drops broadcasts, tests).
    static std::unique_ptr<Net> start(uint32_t ip, uint32_t mask, std::vector<uint32_t> peers, Events ev, std::string* why);
    virtual ~Net() = default;
    // Scan: the NetworkInfos that answered within wait_ms (the first answer ends the wait wait_after_ms later).
    virtual std::vector<Info> scan(int wait_ms, int wait_after_ms) = 0;
    // Host: open the network with this info (node 0 = this node) / change it (advertise data, policy) / close it / drop a station.
    virtual bool host(const Info& info, std::string* why) = 0;
    virtual void host_update(const Info& info) = 0;
    virtual void host_close() = 0;
    virtual bool host_reject(uint32_t ip) = 0;
    // Station: join the network whose host is host_ip: 0 joined, 1 no answer in time, 2 the host turned it away, 3 no connection.
    virtual int connect(uint32_t host_ip, const Node& self, int timeout_ms) = 0;
    virtual void disconnect() = 0;
    virtual Info info() const = 0;   // the network as it is now (host: with its stations; station: the last SyncNetwork)
};

}  // namespace nxrt::vnet::mitm
