// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// ldn: ldn:u (IUserServiceCreator -> IUserLocalCommunicationService), Nintendo local wireless, carried by a vnet::Lobby (multiplayer
// phase 2): a network the game creates is a room on the relay server, a scan lists the server's rooms, a connect joins one, and while the
// node is in a room its lobby link is the current vnet link, so the bsd UDP sockets reach the other nodes (169.254.R.N/24).
// Game-independent.
//
// Adapted from Ryujinx (MIT): src/Ryujinx.HLE/HOS/Services/Ldn/Types/* (NetworkInfo, CommonNetworkInfo, LdnNetworkInfo, NodeInfo,
//   NodeLatestUpdate, ScanFilter, NetworkConfig, SecurityConfig, SecurityParameter, UserConfig, IntentId, NetworkId), Ldn/ResultCode.cs,
//   Ldn/UserServiceCreator/IUserLocalCommunicationService.cs (raw argument layouts: CreateNetwork reads SecurityConfig, UserConfig, 4 pad,
//   NetworkConfig; Connect reads SecurityConfig, UserConfig, u32 version, u32 option with the NetworkInfo in buffer 0x19; Scan reads the
//   channel as 8 bytes, then the ScanFilter) and the DisconnectReason enum.
// Switchbrew "LDN services": the command table (Switch 2 ids 105, 403, 500..601 included), the State enum, the struct offsets, the state
//   transitions (Initialize None->Initialized, OpenAccessPoint 1->2, CreateNetwork 2->3, DestroyNetwork 3->2, CloseAccessPoint 2/3->1,
//   OpenStation 1->4, Connect 4->5, Disconnect 5->4, CloseStation 4/5->1, Finalize any->0), GetNetworkInfo needing state 3 or 5, Scan
//   needing 2..5, SetAdvertiseData <= 0x180 bytes in state 2..3, Reject in state 3.
//
// Mapping onto the lobby (lobby.h): SessionId = "pkpR" + u32 room id (so a NetworkInfo from Scan maps back to its room on Connect);
// BSSID = the host node's MAC (02:00:a9:fe:R:1); SSID = 32 hex digits from the local communication id and the room id; channel 1/6/11 by
// room id; link level 3; network type 2; the security parameter's random half = SHA-256 of the session id (host and guests agree). The
// passphrase reaches the server only as its truncated SHA-256 (vnet::passphrase_hash).
// Node table: the server's nicknames ([account] nickname of each player); the node's own entry uses the UserConfig name the game passed.
//
// When nobody leaves on purpose: the host closing the room or the server going away puts a station back to Station and a host back to
// AccessPoint with GetDisconnectReason set (DestroyedByUser 3 when the host destroyed the room, Rejected 5, DestroyedBySystem 4 on server
// shutdown, SignalLost 6 when the connection to the server is lost); the state change event is signalled on every state change and on
// every node join/leave.
// Host: the current link is a vnet::HoldLink in front of the lobby's, so datagrams from a node that just joined wait (up to kHoldMs) until
// the game has read a GetNetworkInfo / GetNetworkInfoLatestUpdate listing it; KTGL drops a sender outside its member table (design from
// pokopia-native (MIT) windows/src/coop/coop_mpnet.c, ACCEPT_WAIT_MS / cmp_host_accepts, @7c3655d).
// Private networks (CreateNetworkPrivate / ConnectPrivate): the game's SecurityParameter is the network's (GetSecurityParameter, the
// NetworkInfo session id); the room carries the session id at the end of its advertise data on the wire (with_private / private_of), so
// stations find it by it; the game never sees that trailer. Station accept policy and filter: on_room_state.
// [net] room_code: the lobby's room key (vnet::room_key); a room hosted with one is hidden from players without it. A Connect refused for a
// wrong code answers ConnectNotFound.
// Session variant (services_api.h ldnapi): a game module's opt-in gameplay change that every player must share rides in bits 12..14 of the
// local communication version on the wire (CreateNetwork's room, Connect's JOIN), so the relay's own version check refuses a player whose
// variant differs. The game sees its own version only (NetworkInfo nodes, GetNetworkConfig). A Connect refused for another variant answers
// ConnectRejected and logs which side has what; the variant of a room comes from the last Scan that listed it.
// No lobby ([net] server empty or off): the state machine runs, Scan finds nothing, Connect answers ConnectNotFound and CreateNetwork
// DeviceNotAvailable (the answers of the offline stub this replaces).
// [net] server = auto (the default): no fixed relay. CreateNetwork starts the relay inside this process (vnet::RelayServer on 0.0.0.0:39660,
// once; it runs until the process exits; a port already taken = another relay on this PC, used as it is) and hosts on it through
// 127.0.0.1. Scan outside a room runs LAN discovery (vnet::discover_relays, at most every 2 s, the last result in between), asks every
// relay found for its rooms in parallel (one kept relay client per relay, at most 1.5 s for all of them) and lists the rooms of all of
// them: several hosts on one network each show up. The session id carries the room's relay (session_of: a tag from the relay's WELCOME
// instance id, the same for the host and its visitors), and the BSSID, SSID, node MACs, security parameter and authentication id are
// derived from it too, so two relays' room 1 never look alike. Connect moves the node to the relay its session id names, then joins.
// Moving = Ldn::adopt(): only while this node is in no room (no current link, no hold link, both point at the lobby in use), the old
// lobby's sinks are cleared before the new one's are set; the old relay client stays as a scan client. host:port mode writes a zero tag:
// every byte as before.
//
// That is the Link Play backend ([net] protocol = linkplay). The default, [net] protocol = native, is pokopia-native's co-op module
// (NativeLdn below): wire-compatible with pokopia-native builds, not with the relay.
#include "ldn.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "coop.h"
#include "ldn_ryu.h"
#include "net_status.h"
#include "../net/lan_mitm.h"
#include "../net/relay_server.h"
#include "../net/vnet.h"
#include "services_internal.h"

namespace nxrt::services {
namespace {

// ---- result codes (Ryujinx Ldn/ResultCode.cs; module 203) -----------------------------------------------------------------------------
constexpr uint32_t kDeviceNotAvailable = res(203, 16);
constexpr uint32_t kInvalidState = res(203, 32);
constexpr uint32_t kNodeNotFound = res(203, 48);
constexpr uint32_t kConnectFailure = res(203, 64);
constexpr uint32_t kConnectNotFound = res(203, 65);
constexpr uint32_t kConnectTimeout = res(203, 66);
constexpr uint32_t kConnectRejected = res(203, 67);
constexpr uint32_t kInvalidArgument = res(203, 96);
constexpr uint32_t kVersionTooLow = res(203, 113);
constexpr uint32_t kVersionTooHigh = res(203, 114);
constexpr uint32_t kTooManyPlayers = res(203, 144);

// ---- enums ----------------------------------------------------------------------------------------------------------------------------
enum State : uint32_t { kNone = 0, kInitialized = 1, kAccessPoint = 2, kAccessPointCreated = 3, kStation = 4, kStationConnected = 5, kError = 6 };
enum Reason : int16_t {
    kReasonNone = 0, kDisconnectedByUser = 1, kDisconnectedBySystem = 2, kDestroyedByUser = 3, kDestroyedBySystem = 4, kRejected = 5,
    kSignalLost = 6,
};
enum : uint8_t { kNodeConnect = 1, kNodeDisconnect = 2 };   // NodeLatestUpdate.StateChange
enum : uint32_t {                                            // ScanFilterFlag
    kFilterLocalCommunicationId = 1 << 0, kFilterSessionId = 1 << 1, kFilterNetworkType = 1 << 2, kFilterBssid = 1 << 3,
    kFilterSsid = 1 << 4, kFilterSceneId = 1 << 5,
};

// ---- layouts (Switchbrew "LDN services", Ryujinx Ldn/Types) ---------------------------------------------------------------------------
struct IntentId {
    uint64_t local_comm_id;
    uint16_t reserved1;
    uint16_t scene_id;
    uint32_t reserved2;
};
struct NetworkId {
    IntentId intent;
    uint8_t session_id[16];
};
struct Ssid {
    uint8_t length;
    char raw[33];
};
struct CommonNetworkInfo {
    uint8_t bssid[6];
    Ssid ssid;
    int16_t channel;
    int8_t link_level;
    uint8_t network_type;
    uint32_t reserved;
};
struct NodeInfo {
    uint32_t ipv4;
    uint8_t mac[6];
    int8_t node_id;
    uint8_t is_connected;
    char user_name[33];
    uint8_t reserved1;   // Platform from 19.0.0 (0 = NX)
    int16_t local_comm_version;
    uint8_t reserved2[16];
};
struct LdnNetworkInfo {
    uint8_t security_parameter[16];
    uint16_t security_mode;
    uint8_t station_accept_policy;
    uint8_t unknown1;
    uint8_t reserved1[2];
    uint8_t node_count_max;
    uint8_t node_count;
    NodeInfo nodes[8];
    uint8_t reserved2[2];
    uint16_t advertise_data_size;
    uint8_t advertise_data[384];
    uint8_t reserved3[140];
    uint64_t authentication_id;
};
struct NetworkInfo {
    NetworkId id;
    CommonNetworkInfo common;
    LdnNetworkInfo ldn;
};
struct NodeLatestUpdate {
    uint8_t state_change;
    uint8_t reserved[7];
};
struct ScanFilter {
    NetworkId id;
    uint32_t network_type;
    uint8_t bssid[6];
    Ssid ssid;
    uint8_t reserved[16];
    uint32_t flag;
};
struct NetworkConfig {
    IntentId intent;
    uint16_t channel;
    uint8_t node_count_max;
    uint8_t reserved1;
    uint16_t local_comm_version;
    uint8_t reserved2[10];
};
struct SecurityConfig {
    uint16_t mode;
    uint16_t passphrase_size;
    uint8_t passphrase[64];
};
struct SecurityParameter {
    uint8_t data[16];
    uint8_t session_id[16];
};
struct UserConfig {
    char user_name[33];
    uint8_t reserved[15];
};

static_assert(sizeof(IntentId) == 0x10 && sizeof(NetworkId) == 0x20 && sizeof(Ssid) == 0x22);
static_assert(sizeof(CommonNetworkInfo) == 0x30 && offsetof(CommonNetworkInfo, channel) == 0x28);
static_assert(sizeof(NodeInfo) == 0x40 && offsetof(NodeInfo, user_name) == 0x0C && offsetof(NodeInfo, local_comm_version) == 0x2E);
static_assert(sizeof(LdnNetworkInfo) == 0x430 && offsetof(LdnNetworkInfo, nodes) == 0x18 && offsetof(LdnNetworkInfo, advertise_data_size) == 0x21A &&
              offsetof(LdnNetworkInfo, advertise_data) == 0x21C && offsetof(LdnNetworkInfo, authentication_id) == 0x428);
static_assert(sizeof(NetworkInfo) == 0x480 && offsetof(NetworkInfo, ldn) == 0x50);
static_assert(sizeof(NodeLatestUpdate) == 8);
static_assert(sizeof(ScanFilter) == 0x60 && offsetof(ScanFilter, ssid) == 0x2A && offsetof(ScanFilter, flag) == 0x5C);
static_assert(sizeof(NetworkConfig) == 0x20 && offsetof(NetworkConfig, local_comm_version) == 0x14);
static_assert(sizeof(SecurityConfig) == 0x44 && sizeof(SecurityParameter) == 0x20 && sizeof(UserConfig) == 0x30);

// Raw argument offsets (CMIF, natural alignment: NetworkConfig is 8-aligned).
constexpr size_t kCreateUser = 0x44, kCreateNetCfg = 0x78;                                 // CreateNetwork
constexpr size_t kCreatePrivSecParam = 0x44, kCreatePrivUser = 0x64, kCreatePrivNetCfg = 0x98;   // CreateNetworkPrivate (+ AddressList: unused)
constexpr size_t kConnectUser = 0x44, kConnectVersion = 0x74;                              // Connect
constexpr size_t kConnPrivSecParam = 0x44, kConnPrivUser = 0x64, kConnPrivVersion = 0x94, kConnPrivNetCfg = 0xA0;   // ConnectPrivate
constexpr uint32_t kSubnetMask = 0xFFFFFF00;   // the game asks MakeSubnetMask(24) too
constexpr uint8_t kSessionTag[4] = {'p', 'k', 'p', 'R'};
// Connect waits this long for the room state that lists the new node (the relay sends ROOM_STATE right behind JOINED); the lobby's own
// request timeout is 5 s too (vnet::RelayOptions::timeout_ms).
constexpr int kRoomStateWaitMs = 5000;
// Host: datagrams from a node the game has not yet seen in a NetworkInfo are held (vnet::HoldLink) until the game reads a table listing
// it, at most this long (pokopia-native windows/src/coop/coop_mpnet.c ACCEPT_WAIT_MS: the game polls GetNetworkInfo every frame), and at
// most kHoldCap of them. KTGL drops a datagram from a sender outside its member table; the new station's first one is its join request.
constexpr int kHoldMs = 300;
constexpr size_t kHoldCap = 32;

// ---- session variant (services_api.h ldnapi) ----------------------------------------------------------------------------------------
struct Variant {
    std::mutex mu;
    uint8_t value = 0;
    std::string what;
};
Variant& variant() {
    static Variant v;
    return v;
}
std::atomic<bool>& story_flag() {
    static std::atomic<bool> f{false};
    return f;
}
uint8_t own_variant(std::string* what = nullptr) {
    Variant& v = variant();
    std::lock_guard<std::mutex> lk(v.mu);
    if (what) *what = v.what;
    return v.value;
}
// The game's version on the wire: its variant in bits 12..14. Only for versions 0..0xfff (the game's own bits never collide); any other is
// sent as it is (logged once while a variant is set).
int16_t wire_version(int16_t game) {
    const uint8_t var = own_variant();
    if (!var) return game;
    if (game < 0 || (static_cast<uint16_t>(game) & ~0x0FFFu)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            NXLOG_W("net", "ldn: the game's local communication version 0x%04x uses bits 12..15: the session variant is not sent",
                    static_cast<uint16_t>(game));
        return game;
    }
    return static_cast<int16_t>(game | (var << ldnapi::kVariantShift));
}
int16_t game_version(int16_t wire) { return wire < 0 ? wire : static_cast<int16_t>(wire & ~ldnapi::kVariantMask); }
uint8_t variant_of(int16_t wire) { return wire < 0 ? 0 : static_cast<uint8_t>((wire & ldnapi::kVariantMask) >> ldnapi::kVariantShift); }

// ---- process lobby ------------------------------------------------------------------------------------------------------------------
struct AutoRelay {                // auto mode: one client per relay address, kept across scans
    std::shared_ptr<vnet::Lobby> client;
    std::chrono::steady_clock::time_point seen{};   // the last discovery that listed it
    std::shared_ptr<std::atomic<bool>> busy = std::make_shared<std::atomic<bool>>(false);   // a scan of it is in flight
};
struct LobbySlot {
    std::mutex mu;
    bool built = false;
    bool auto_mode = false;      // [net] server = auto: lobby follows the relay in use (auto_relay)
    vnet::RelayOptions base;     // auto: the options of every relay client (host and port per relay)
    std::string target;          // auto: host:port of lobby
    std::map<std::string, AutoRelay> relays;   // auto: host:port -> its client (lobby is one of them)
    std::shared_ptr<vnet::Lobby> lobby;
};
LobbySlot& slot() {
    static LobbySlot s;
    return s;
}

// room-code.txt: the code in use, for the launcher to show (launcher room_code_host): beside the log (the game folder for pkp).
std::string room_code_file() {
    const std::string log = log_path();
    const size_t cut = log.find_last_of("/\\");
    return (cut == std::string::npos ? std::string() : log.substr(0, cut + 1)) + "room-code.txt";
}

// [net] room_code -> the lobby's room key (zero: none). While a code is in use it is logged and written to room_code_file(), else
// that file is removed (a code from an earlier run must not be shown). False: the value is not a code (local wireless stays off).
bool room_code_setup(std::array<uint8_t, 16>* key) {
    std::string v = config_get_str("net", "room_code", "");
    const std::string file = room_code_file();
    std::string lower = v, norm;
    for (char& c : lower) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (v.empty()) {
        std::remove(file.c_str());
        return true;
    }
    if (lower == "new") {
        norm = vnet::room_code_new();
        if (norm.empty()) {
            NXLOG_W("net", "ldn: [net] room_code = new: no random source: local wireless off");
            std::remove(file.c_str());
            return false;
        }
    } else if (!vnet::room_code_normalize(v, &norm)) {
        NXLOG_W("net", "ldn: [net] room_code is not a room code (16 letters and digits, XXXX-XXXX-XXXX-XXXX): local wireless off");
        std::remove(file.c_str());
        return false;
    }
    *key = vnet::room_key(norm);
    const std::string shown = vnet::room_code_format(norm);
    if (FILE* f = std::fopen(file.c_str(), "wb")) {
        std::fprintf(f, "%s\n", shown.c_str());
        std::fclose(f);
    }
    NXLOG_I("net", "ldn: room code %s%s: a room hosted here is seen only by players with this code; scans show open rooms and rooms with it",
            shown.c_str(), lower == "new" ? " (new)" : "");
    return true;
}

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return s;
}

std::string nickname() {
    std::string n = profile().user_nickname;
    return n.empty() ? "Player" : n;
}

// Everything of a relay client but its host and port. False: [net] room_code is not a code (local wireless stays off).
bool relay_options(vnet::RelayOptions* o) {
    o->nickname = nickname();
    o->password = config_get_str("net", "password", "");
    if (!room_code_setup(&o->room_key)) return false;
    const std::string udp = lower(config_get_str("net", "udp", "auto"));
    o->udp = !(udp == "off" || udp == "0" || udp == "false" || udp == "no" || udp == "tcp");
    if (o->udp && udp != "auto" && udp != "on" && udp != "1" && udp != "true" && udp != "yes")
        NXLOG_W("net", "ldn: [net] udp = \"%s\" is not auto or off: auto", udp.c_str());
    return true;
}

// [net] server: empty or off = local wireless off; auto (also when the key is absent) = auto mode, no lobby until a relay is picked;
// host:port = a relay client to it. *auto_mode: auto mode.
std::shared_ptr<vnet::Lobby> process_lobby(bool* auto_mode) {
    LobbySlot& s = slot();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.built) {
        s.built = true;
        const std::string server = config_get_str("net", "server", "auto");
        std::string v = lower(server);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        vnet::RelayOptions o;
        if (v.empty() || v == "off") {
            NXLOG_I("net", "ldn: [net] server is %s: local wireless off (scans find nothing)", v.empty() ? "empty" : "off");
            std::remove(room_code_file().c_str());
        } else if (v == "auto") {
            if (relay_options(&o)) {
                s.auto_mode = true;
                s.base = o;
                NXLOG_I("net", "ldn: [net] server = auto: hosting starts a relay in this process (port %u), visiting looks for hosts on the "
                        "local network", vnet::kRelayDefaultPort);
            }
        } else if (!vnet::parse_server(server, &o.host, &o.port)) {
            NXLOG_W("net", "ldn: [net] server = \"%s\" is not host:port, auto or off: local wireless off", server.c_str());
        } else if (relay_options(&o)) {
            s.lobby = vnet::make_relay_client(o);
        }
    }
    if (auto_mode) *auto_mode = s.auto_mode;
    return s.lobby;
}

// Auto mode: the relay client for host:port (made on first use, then kept; caller holds s.mu).
std::shared_ptr<vnet::Lobby> relay_client_locked(LobbySlot& s, const std::string& host, uint16_t port) {
    AutoRelay& r = s.relays[host + ":" + std::to_string(port)];
    if (!r.client) {
        vnet::RelayOptions o = s.base;
        o.host = host;
        o.port = port;
        r.client = vnet::make_relay_client(o);
        r.seen = std::chrono::steady_clock::now();
    }
    return r.client;
}

// Auto mode: makes the client of the relay at host:port the process lobby (the one rooms go through: hosting, a Connect) and returns it;
// the previous one stays in slot().relays as a scan client. Outside auto mode: the process lobby as it is.
std::shared_ptr<vnet::Lobby> auto_relay(const std::string& host, uint16_t port) {
    LobbySlot& s = slot();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.auto_mode) return s.lobby;
    std::shared_ptr<vnet::Lobby> c = relay_client_locked(s, host, port);
    if (s.lobby != c) {
        s.lobby = c;
        s.target = host + ":" + std::to_string(port);
        NXLOG_I("net", "ldn: auto: rooms go through the relay at %s", s.target.c_str());
    }
    return c;
}

// A relay's identity in the session id (bytes 8..13): the first 6 bytes of SHA-256 of its WELCOME server version, which carries a random
// instance id on the embedded and the launcher's relay. The host (at 127.0.0.1) and every visitor (at a LAN address) get the same value
// for one relay, and two relays get different ones. 0: not known (never connected, the loopback relay of tests).
uint64_t relay_tag(const vnet::Lobby& l) {
    const std::string v = l.server_version();
    if (v.empty()) return 0;
    const auto h = vnet::sha256(v.data(), v.size());
    uint64_t t = 0;
    for (int i = 0; i < 6; ++i) t |= uint64_t{h[static_cast<size_t>(i)]} << (8 * i);
    return t ? t : 1;
}

void firewall_hint() {
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        NXLOG_I("net", "ldn: if Windows Firewall asks about this program, allow it on private networks (Link Play uses TCP and UDP port %u)",
                vnet::kRelayDefaultPort);
}

// Auto mode, CreateNetwork: the relay in this process on 0.0.0.0:39660, started on the first call that can start it and never stopped
// (it runs until the process exits). A port already taken (the launcher's relay on this PC, another game) is that relay's: it is used as it
// is. Either way the host is a relay client to 127.0.0.1:39660.
// [net] host_listen: the address the embedded relay listens on. Empty or "all" (the default): every address. An IPv4 address: that one
// only (127.0.0.1: this PC alone, two copies of a game on one PC; a firewall has nothing to ask about). Not an address: every address.
uint32_t host_listen_ip() {
    static const uint32_t ip = [] {
        const std::string v = lower(config_get_str("net", "host_listen", ""));
        if (v.empty() || v == "all") return 0u;
        unsigned a, b, c, d;
        char tail;
        if (std::sscanf(v.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
            return vnet::ip(static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c), static_cast<uint8_t>(d));
        NXLOG_W("net", "ldn: [net] host_listen = \"%s\" is not an IPv4 address: every address", v.c_str());
        return 0u;
    }();
    return ip;
}

std::shared_ptr<vnet::Lobby> host_relay() {
    static std::mutex mu;
    static vnet::RelayServer* server = nullptr;   // deliberately never deleted
    static std::string last;
    const uint32_t listen = host_listen_ip();
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!server) {
            vnet::RelayServerOptions o;
            o.port = vnet::kRelayDefaultPort;
            o.listen_ip = listen;
            o.password = config_get_str("net", "password", "");
            o.nickname = nickname();
            // the random instance id makes relay_tag() unique per relay
            o.server_version = std::string("nxrt relay (") + vnet::build_stamp() + ") #" + vnet::room_code_new();
            std::string why;
            bool in_use = false;
            if (auto srv = vnet::RelayServer::start(o, &why, &in_use)) {
                server = srv.release();
                last.clear();
                for (const vnet::LocalAddr& a : vnet::local_ipv4())
                    if (!vnet::is_link_local(a.ip) && (!listen || listen == a.ip))
                        NXLOG_I("net", "ldn: hosting: visitors with [net] server set reach this PC at %s:%u (%s)", vnet::ip_str(a.ip).c_str(),
                                server->port(), a.adapter.c_str());
                firewall_hint();
            } else if (why != last) {
                last = why;
                if (in_use) NXLOG_I("net", "ldn: hosting: %s on this PC (the launcher's relay or another game): hosting on that relay", why.c_str());
                else NXLOG_W("net", "ldn: hosting: cannot start the relay: %s", why.c_str());
            }
        }
    }
    return auto_relay(listen ? vnet::ip_str(listen) : std::string("127.0.0.1"), vnet::kRelayDefaultPort);
}

// Auto mode, Scan: the last LAN discovery, a new one at most every kDiscoveryEveryMs (waiting kDiscoveryWaitMs for answers). The relays
// found are logged when the result changes, not per discovery.
constexpr int kDiscoveryWaitMs = 400, kDiscoveryEveryMs = 2000;
struct Discovery {
    std::mutex mu;
    bool ran = false;
    std::chrono::steady_clock::time_point at{};
    std::vector<vnet::FoundRelay> found;
    std::string logged;
};
Discovery& discovery() {
    static Discovery d;
    return d;
}

std::vector<vnet::FoundRelay> discover_cached() {
    Discovery& d = discovery();
    std::lock_guard<std::mutex> lk(d.mu);
    const auto now = std::chrono::steady_clock::now();
    if (!d.ran || now - d.at >= std::chrono::milliseconds(kDiscoveryEveryMs)) {
        // [net] host_listen = 127.0.0.1 (this PC only): discovery asks this machine's relay only, from the loopback (no socket on the LAN)
        const uint32_t hl = host_listen_ip();
        d.found = vnet::discover_relays(vnet::kRelayDefaultPort, kDiscoveryWaitMs, (hl >> 24) == 127 ? hl : 0);
        d.at = std::chrono::steady_clock::now();
        d.ran = true;
        std::string seen = "-";
        for (const vnet::FoundRelay& f : d.found) seen += vnet::ip_str(f.ip) + ":" + std::to_string(f.port) + "/" + std::to_string(f.open_rooms) + " ";
        if (seen != d.logged) {
            d.logged = seen;
            if (d.found.empty()) {
                NXLOG_I("net", "ldn: no Link Play host found on the local network (the host picks Invite Others to Visit first; a firewall on "
                        "either PC can block UDP/TCP port %u)", vnet::kRelayDefaultPort);
                firewall_hint();
            }
            for (const vnet::FoundRelay& f : d.found)
                NXLOG_I("net", "ldn: Link Play host %s:%u (%s, %u open room%s)", vnet::ip_str(f.ip).c_str(), f.port, f.nickname.c_str(),
                        f.open_rooms, f.open_rooms == 1 ? "" : "s");
        }
    }
    return d.found;
}

// Auto mode, Scan: the rooms of every relay found, asked in parallel. A relay reporting no open room is skipped unless this player has a
// room code (open rooms count keyless rooms only). Each relay's SCAN runs on a thread of its own; the scan waits for all of them at most
// kScanAllMs, a relay that has not answered by then is left out of this scan (its thread finishes on its own and is not started again for
// that relay while it runs). Clients of relays no discovery listed for kRelayForgetMs are dropped, never the process lobby (the one a room
// goes through).
constexpr int kScanAllMs = 1500, kRelayForgetMs = 10000;
struct RelayRooms {
    std::string host;
    uint16_t port = 0;
    bool loopback = false;
    uint64_t tag = 0;
    std::vector<vnet::Room> rooms;
};

std::vector<RelayRooms> scan_relays(const std::vector<vnet::FoundRelay>& found, uint64_t lcid, uint16_t scene) {
    struct Job {
        RelayRooms rr;
        std::shared_ptr<vnet::Lobby> client;
        std::shared_ptr<std::atomic<bool>> busy;
    };
    std::vector<Job> jobs;
    {
        LobbySlot& s = slot();
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.auto_mode) return {};
        const bool keyed = s.base.room_key != std::array<uint8_t, 16>{};
        const auto now = std::chrono::steady_clock::now();
        for (const vnet::FoundRelay& f : found) {
            const std::string host = vnet::ip_str(f.ip);
            std::shared_ptr<vnet::Lobby> c = relay_client_locked(s, host, f.port);
            AutoRelay& r = s.relays[host + ":" + std::to_string(f.port)];
            r.seen = now;
            if (!keyed && f.open_rooms == 0) continue;
            Job j;
            j.rr.host = host;
            j.rr.port = f.port;
            j.rr.loopback = (f.ip >> 24) == 127;
            j.client = c;
            j.busy = r.busy;
            jobs.push_back(std::move(j));
        }
        for (auto it = s.relays.begin(); it != s.relays.end();) {
            if (it->second.client != s.lobby && now - it->second.seen >= std::chrono::milliseconds(kRelayForgetMs)) it = s.relays.erase(it);
            else ++it;
        }
    }
    struct Shared {
        std::mutex mu;
        std::condition_variable cv;
        size_t pending = 0;
        std::vector<RelayRooms> got;
    };
    auto sh = std::make_shared<Shared>();
    for (Job& j : jobs) {
        if (j.busy->exchange(true)) continue;   // its scan from an earlier call is still waiting
        {
            std::lock_guard<std::mutex> lk(sh->mu);
            ++sh->pending;
        }
        std::thread([sh, j, lcid, scene]() mutable {
            const int e = j.client->scan(lcid, scene, &j.rr.rooms);
            j.rr.tag = relay_tag(*j.client);
            j.busy->store(false);
            if (e) NXLOG_D("net", "ldn: scan of the relay at %s:%u failed: %s", j.rr.host.c_str(), j.rr.port, vnet::lobby_err_str(e));
            std::lock_guard<std::mutex> lk(sh->mu);
            if (!e) sh->got.push_back(std::move(j.rr));
            --sh->pending;
            sh->cv.notify_all();
        }).detach();
    }
    std::unique_lock<std::mutex> lk(sh->mu);
    sh->cv.wait_for(lk, std::chrono::milliseconds(kScanAllMs), [&] { return sh->pending == 0; });
    std::vector<RelayRooms> out = std::move(sh->got);
    sh->got.clear();   // a late answer lands here and is dropped with sh
    return out;
}

// ---- helpers ------------------------------------------------------------------------------------------------------------------------
template <class T>
T arg_at(const ipc::Request& req, size_t off) {
    return req.arg<T>(off);
}

void copy_name(char (&dst)[33], const std::string& s) {
    std::memset(dst, 0, sizeof dst);
    size_t n = s.size() < 32 ? s.size() : 32;
    while (n && n < s.size() && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    std::memcpy(dst, s.data(), n);
}

// SessionId = "pkpR" + u32 room id + 6 B relay tag + 2 zero bytes. The tag (relay_tag()) is set in every mode, so a host:port host and
// an auto visitor on one relay agree; zero only when the relay sent no version.
struct SessionRef {
    uint32_t room = 0;   // 0: not one of ours
    uint64_t tag = 0;
};
SessionRef session_of(const uint8_t (&sid)[16]) {
    SessionRef s;
    if (std::memcmp(sid, kSessionTag, 4) != 0) return s;
    std::memcpy(&s.room, sid + 4, 4);
    for (int i = 0; i < 6; ++i) s.tag |= uint64_t{sid[8 + i]} << (8 * i);
    return s;
}

// Private networks (CreateNetworkPrivate / ConnectPrivate): the game decides the session id itself and hands it to its stations out of
// band, and a station connects by it without a scan. The relay knows rooms by number only, so a private room carries its session id at
// the end of its advertise data on the wire: the game's data, the 16-byte session id, then "pkP1". Every node strips it before the game
// sees the data (make_info); a room with it shows that session id, and Connect / ConnectPrivate find the room by it (the Scans' table).
// Older builds see 20 more advertise bytes. A game whose own advertise data leaves no room for it (more than 364 bytes) gets a normal
// room: its stations still find it by Scan, not by ConnectPrivate alone.
constexpr uint8_t kPrivMagic[4] = {'p', 'k', 'P', '1'};
constexpr size_t kPrivTrailer = 16 + sizeof kPrivMagic;
using SessionKey = std::array<uint8_t, 16>;

std::vector<uint8_t> with_private(const std::vector<uint8_t>& adv, const uint8_t* sid) {
    std::vector<uint8_t> v = adv;
    v.insert(v.end(), sid, sid + 16);
    v.insert(v.end(), kPrivMagic, kPrivMagic + sizeof kPrivMagic);
    return v;
}

// The session id a room's advertise data carries (private room), and the length of the game's own part of the data.
bool private_of(const std::vector<uint8_t>& adv, SessionKey* sid, size_t* game_len) {
    *game_len = adv.size();
    if (adv.size() < kPrivTrailer || std::memcmp(adv.data() + adv.size() - sizeof kPrivMagic, kPrivMagic, sizeof kPrivMagic) != 0) return false;
    std::memcpy(sid->data(), adv.data() + adv.size() - kPrivTrailer, 16);
    *game_len = adv.size() - kPrivTrailer;
    return true;
}

// A node MAC (02:00:a9:fe:R:N from the relay) with a relay tag: bytes 1..3 become the tag's first three, so two relays' room R differ in
// the BSSID and the node table too. Tag 0: unchanged.
std::array<uint8_t, 6> tagged_mac(std::array<uint8_t, 6> m, uint64_t tag) {
    if (tag)
        for (int i = 0; i < 3; ++i) m[static_cast<size_t>(1 + i)] = static_cast<uint8_t>(tag >> (8 * i));
    return m;
}

// Whether a room state lists the host (node 0) and the node with address vip.
bool room_lists(const vnet::Room& r, uint32_t vip) {
    bool host = false, me = false;
    for (const vnet::RoomNode& n : r.nodes) {
        host |= n.node_id == 0;
        me |= n.vip == vip;
    }
    return host && me;
}

// The NetworkInfo of a lobby room. own_vip/own_name: this node's entry uses the game's UserConfig name. tag: the room's relay (relay_tag(),
// every mode; 0 only for a relay without a version = the layout as before). Everything the game could use to tell networks apart carries it: session id,
// BSSID and node MACs (tagged_mac), SSID (tag << 16 | room id), the security parameter (SHA-256 of the session id) and the
// authentication id. The host and its visitors compute the same tag for their relay, so every node sees the same values.
NetworkInfo make_info(const vnet::Room& r, uint8_t accept_policy, uint32_t own_vip, const std::string& own_name, uint64_t tag) {
    NetworkInfo ni{};
    ni.id.intent.local_comm_id = r.local_comm_id;
    ni.id.intent.scene_id = r.scene_id;
    std::memcpy(ni.id.session_id, kSessionTag, 4);
    std::memcpy(ni.id.session_id + 4, &r.room_id, 4);
    for (int i = 0; i < 6; ++i) ni.id.session_id[8 + i] = static_cast<uint8_t>(tag >> (8 * i));
    const uint64_t net = tag ? (tag << 16) | (r.room_id & 0xFFFF) : r.room_id;
    const auto mac = tagged_mac({0x02, 0x00, 0xA9, 0xFE, static_cast<uint8_t>(r.room_id), 1}, tag);
    std::memcpy(ni.common.bssid, mac.data(), 6);
    char ssid[33];
    std::snprintf(ssid, sizeof ssid, "%016llx%016llx", static_cast<unsigned long long>(r.local_comm_id), static_cast<unsigned long long>(net));
    ni.common.ssid.length = 32;
    std::memcpy(ni.common.ssid.raw, ssid, 32);
    static constexpr int16_t kChannels[3] = {1, 6, 11};
    ni.common.channel = kChannels[r.room_id % 3];
    ni.common.link_level = 3;
    ni.common.network_type = 2;
    SessionKey psid{};
    size_t adv_len = r.adv.size();
    if (private_of(r.adv, &psid, &adv_len)) std::memcpy(ni.id.session_id, psid.data(), 16);
    const auto sp = vnet::sha256(ni.id.session_id, sizeof ni.id.session_id);
    std::memcpy(ni.ldn.security_parameter, sp.data(), 16);
    ni.ldn.security_mode = r.security_mode;
    ni.ldn.station_accept_policy = accept_policy;
    ni.ldn.node_count_max = r.node_count_max;
    for (const vnet::RoomNode& n : r.nodes) {
        if (n.node_id >= 8) continue;
        NodeInfo& d = ni.ldn.nodes[n.node_id];
        d.ipv4 = n.vip;
        std::memcpy(d.mac, tagged_mac(n.mac, tag).data(), 6);
        d.node_id = static_cast<int8_t>(n.node_id);
        d.is_connected = 1;
        copy_name(d.user_name, n.vip == own_vip && !own_name.empty() ? own_name : n.nickname);
        d.local_comm_version = game_version(r.comm_version);
        ++ni.ldn.node_count;
    }
    const size_t adv = adv_len < sizeof ni.ldn.advertise_data ? adv_len : sizeof ni.ldn.advertise_data;
    ni.ldn.advertise_data_size = static_cast<uint16_t>(adv);
    if (adv) std::memcpy(ni.ldn.advertise_data, r.adv.data(), adv);
    ni.ldn.authentication_id = net;
    return ni;
}

bool filter_match(const ScanFilter& f, const NetworkInfo& ni) {
    if ((f.flag & kFilterSessionId) && std::memcmp(f.id.session_id, ni.id.session_id, 16) != 0) return false;
    if ((f.flag & kFilterNetworkType) && !(f.network_type & ni.common.network_type)) return false;
    if ((f.flag & kFilterBssid) && std::memcmp(f.bssid, ni.common.bssid, 6) != 0) return false;
    if ((f.flag & kFilterSsid) &&
        (f.ssid.length != ni.common.ssid.length || std::memcmp(f.ssid.raw, ni.common.ssid.raw, ni.common.ssid.length) != 0))
        return false;
    return true;   // local communication id and scene id were filtered by the server
}

// The first 128 bytes of the advertise data as hex for the Debug SetAdvertiseData line ("|" every 16 bytes, "..." when cut). Pokopia puts
// the host's KTGL member id in it (work/mp/join-reject-analysis.md section 3). Only built when Debug is on (NXLOG_D).
std::string hex_head(const std::vector<uint8_t>& b) {
    static const char kHex[] = "0123456789abcdef";
    const size_t n = b.size() < 128 ? b.size() : 128;
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (i) s += (i % 16) ? " " : " | ";
        s += kHex[b[i] >> 4];
        s += kHex[b[i] & 15];
    }
    if (n < b.size()) s += " ...";
    return s;
}

// Logs the first use of a command (one line per command id for the life of the process; network_reset() clears it).
std::mutex g_seen_mu;
std::set<uint32_t> g_seen;
void note(uint32_t id, const char* name) {
    {
        std::lock_guard<std::mutex> lk(g_seen_mu);
        if (!g_seen.insert(id).second) return;
    }
    NXLOG_I("net", "ldn cmd %u %s (first use)", id, name);
}

// =============================================================================================================================================
// One IUserLocalCommunicationService. op_mu serialises the commands (it is held across lobby requests); mu guards the fields and is the only
// lock the lobby's event sinks take, so a sink never waits on a request in flight.
// =============================================================================================================================================

class Ldn : public std::enable_shared_from_this<Ldn> {
public:
    explicit Ldn(bool offline = false) : offline_(offline) {}
    void add_commands(ipc::Service& s);
    void shutdown();   // the service object was released: leave and drop the link

private:
    using H = uint32_t (Ldn::*)(ipc::Request&, ipc::Response&);
    void cmd(ipc::Service& s, uint32_t id, const char* name, H h);

    uint32_t get_state(ipc::Request&, ipc::Response&);
    uint32_t get_network_info(ipc::Request&, ipc::Response&);
    uint32_t get_ipv4(ipc::Request&, ipc::Response&);
    uint32_t get_disconnect_reason(ipc::Request&, ipc::Response&);
    uint32_t get_security_parameter(ipc::Request&, ipc::Response&);
    uint32_t get_network_config(ipc::Request&, ipc::Response&);
    uint32_t attach_event(ipc::Request&, ipc::Response&);
    uint32_t get_latest_update(ipc::Request&, ipc::Response&);
    uint32_t scan(ipc::Request&, ipc::Response&);
    uint32_t accept(ipc::Request&, ipc::Response&) { return 0; }
    uint32_t open_access_point(ipc::Request&, ipc::Response&);
    uint32_t close_access_point(ipc::Request&, ipc::Response&);
    uint32_t create_network(ipc::Request&, ipc::Response&);
    uint32_t create_network_private(ipc::Request&, ipc::Response&);
    uint32_t destroy_network(ipc::Request&, ipc::Response&);
    uint32_t reject(ipc::Request&, ipc::Response&);
    uint32_t set_advertise_data(ipc::Request&, ipc::Response&);
    uint32_t set_accept_policy(ipc::Request&, ipc::Response&);
    uint32_t add_accept_filter(ipc::Request&, ipc::Response&);
    uint32_t clear_accept_filter(ipc::Request&, ipc::Response&);
    uint32_t open_station(ipc::Request&, ipc::Response&);
    uint32_t close_station(ipc::Request&, ipc::Response&);
    uint32_t connect(ipc::Request&, ipc::Response&);
    uint32_t connect_private(ipc::Request&, ipc::Response&);
    uint32_t disconnect(ipc::Request&, ipc::Response&);
    uint32_t initialize(ipc::Request&, ipc::Response&);
    uint32_t finalize(ipc::Request&, ipc::Response&);

    uint32_t create(const SecurityConfig& sec, const UserConfig& user, const NetworkConfig& cfg, const SecurityParameter* priv = nullptr);
    uint32_t join(const SessionRef& sref, int16_t room_version, const SecurityConfig& sec, const UserConfig& user, int32_t version,
                  const SecurityParameter* priv = nullptr);
    // The rooms a Scan lists (every relay found in auto mode outside a room, else the lobby's; own_room left out), with their relay tags;
    // notes their wire versions, relays and private session ids.
    std::vector<std::pair<vnet::Room, uint64_t>> scan_rooms(uint64_t lcid, uint16_t scene);
    SessionRef ref_of(const uint8_t (&sid)[16]);   // a session id from a NetworkInfo: ours ("pkpR"), or a private room's from a Scan
    void on_room_state(const vnet::Room& r);
    void on_left(vnet::LeftReason why);
    bool admits(const std::array<uint8_t, 6>& mac) const;   // caller holds mu: the station accept policy and filter let this MAC join
    void apply_room(const vnet::Room& r);       // caller holds mu
    void set_state(uint32_t s);                 // caller holds mu
    void admit_read();                          // caller holds mu: the game read info_, let its nodes' held datagrams through
    void drop_hold();                           // caller holds mu: the host's hold link is unhooked and gone
    void leave_network(uint32_t next, int16_t reason);   // caller holds op_mu (not mu)
    // net-status.json's "link_play" part (services/net_status: the launcher's room page): status_locked() under mu_ notes that it
    // changed; flush_status() with neither mu_ held nor inside a lobby sink's lock writes it (after each command, each lobby event).
    void status_locked() { status_dirty_ = true; }
    void flush_status();
    // net-control.txt "leave" (net_status::on_command "ldn"): out of the room as when the console's system takes the radio
    // (host: DestroyedBySystem, station: DisconnectedBySystem); the game sees the disconnect and its own menus go on from there.
    void leave_from_launcher();
    vnet::LobbyEvents make_events();
    void adopt(std::shared_ptr<vnet::Lobby> next);       // caller holds op_mu (not mu): auto mode moves to another relay
    bool in_network() const { return state_ == kAccessPointCreated || state_ == kStationConnected; }

    std::mutex op_mu_;
    std::mutex mu_;
    std::condition_variable room_cv_;    // notified (under mu_) on every lobby event; join() waits on it for its first room state
    ServiceEvent ev_;
    uint32_t state_ = kNone;
    int16_t reason_ = kReasonNone;
    std::shared_ptr<vnet::Lobby> lobby_;
    bool auto_ = false;                  // [net] server = auto (set by Initialize): lobby_ follows host_relay() / Connect
    std::vector<uint8_t> adv_;          // host: SetAdvertiseData before or after CreateNetwork
    uint8_t accept_policy_ = 0;          // SetStationAcceptPolicy: 0 accept all, 1 reject all, 2 filter = blacklist, 3 filter = whitelist
    std::set<std::array<uint8_t, 6>> filter_;   // AddAcceptFilterEntry: station MACs, as the game reads them in the node table
    // host: the nodes the policy refused (address, player name), until the room state no longer lists them. The name too: a node that
    // joins later can get the same address (the lowest free slot) before the relay's state without the refused one has come in.
    std::set<std::pair<uint32_t, std::string>> refused_;
    int last_left_ = -1;                 // station: the LeftReason of a room the lobby left while Connect still waited on it (-1: none)
    std::string user_name_;              // the UserConfig name of the last create/connect
    uint32_t room_id_ = 0, vip_ = 0;
    NetworkInfo info_{};
    std::array<uint32_t, 8> node_vip_{};  // the node table last seen (0 = empty slot), for the join/leave flags
    std::array<uint8_t, 8> latest_{};    // NodeLatestUpdate flags since the last GetNetworkInfoLatestUpdate
    std::shared_ptr<vnet::HoldLink> hold_;   // host: the current link, in front of the lobby's (see kHoldMs)
    std::map<std::pair<uint64_t, uint32_t>, int16_t> scan_wire_;   // (relay tag, room id) -> its version on the wire as the last Scan
                                                                    // listed it (the session variant)
    std::map<uint64_t, std::pair<std::string, uint16_t>> tag_relay_;   // auto: relay tag -> its address, from the Scans so far
    uint64_t tag_ = 0;                   // the relay tag of the room this node is in
    bool status_dirty_ = false;
    uint64_t last_seq_ = 0;              // the last room state applied (vnet::Room::seq; 0: a relay over the wire)
    bool priv_ = false;                  // this node is in a private network: priv_sp_ is the game's SecurityParameter
    SecurityParameter priv_sp_{};
    std::map<SessionKey, SessionRef> priv_ref_;   // private rooms' session ids -> their room and relay, from the Scans so far
    bool offline_ = false;               // no lobby ever ([net] protocol = native with the co-op module off): the offline answers
};

void Ldn::cmd(ipc::Service& s, uint32_t id, const char* name, H h) {
    std::weak_ptr<Ldn> w = weak_from_this();
    s.add_cmd(id, name, [w, id, name, h](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        note(id, name);
        auto self = w.lock();
        if (!self) return kInvalidState;
        std::lock_guard<std::mutex> op(self->op_mu_);
        const uint32_t r = ((*self).*h)(req, rsp);
        self->flush_status();
        return r;
    });
}

// {"state":"off|idle|hosting|visiting","room":N,"address":"169.254.R.N","players":[...],"max":N,"relay":"host:port","udp":bool,
// "reason":N} (the contract with the launcher, coord app round 2): players are the node table's names in node order (the host first),
// relay the relay the room is on ("this PC" for a relay inside this game), reason the last DisconnectReason.
void Ldn::flush_status() {
    std::string j;
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!status_dirty_) return;
        status_dirty_ = false;
        const char* st = state_ == kNone ? "off" : state_ == kAccessPointCreated ? "hosting" : state_ == kStationConnected ? "visiting" : "idle";
        j = std::string("{\"state\":\"") + st + "\"";
        if (in_network()) {
            j += ",\"room\":" + std::to_string(room_id_) + ",\"address\":" + net_status::json_str(vnet::ip_str(vip_)) + ",\"players\":[";
            bool first = true;
            for (int i = 0; i < 8; ++i) {
                const NodeInfo& n = info_.ldn.nodes[i];
                if (!n.is_connected) continue;
                j += (first ? "" : ",") + net_status::json_str(std::string(n.user_name, strnlen(n.user_name, sizeof n.user_name)));
                first = false;
            }
            j += "],\"max\":" + std::to_string(info_.ldn.node_count_max);
            std::string relay;
            if (state_ == kAccessPointCreated && auto_) {
                relay = "this PC";
            } else if (const auto it = tag_relay_.find(tag_); it != tag_relay_.end()) {
                relay = it->second.first + ":" + std::to_string(it->second.second);
            } else {
                relay = config_get_str("net", "server", "auto");
            }
            j += ",\"relay\":" + net_status::json_str(relay);
            lobby = lobby_;
        }
        j += ",\"reason\":" + std::to_string(reason_);
    }
    if (lobby) j += std::string(",\"udp\":") + (lobby->udp_active() ? "true" : "false");
    j += "}";
    net_status::set_part("link_play", j);
}

void Ldn::add_commands(ipc::Service& s) {
    cmd(s, 0, "GetState", &Ldn::get_state);
    cmd(s, 1, "GetNetworkInfo", &Ldn::get_network_info);
    cmd(s, 2, "GetIpv4Address", &Ldn::get_ipv4);
    cmd(s, 3, "GetDisconnectReason", &Ldn::get_disconnect_reason);
    cmd(s, 4, "GetSecurityParameter", &Ldn::get_security_parameter);
    cmd(s, 5, "GetNetworkConfig", &Ldn::get_network_config);
    cmd(s, 100, "AttachStateChangeEvent", &Ldn::attach_event);
    cmd(s, 101, "GetNetworkInfoLatestUpdate", &Ldn::get_latest_update);
    cmd(s, 102, "Scan", &Ldn::scan);
    cmd(s, 103, "ScanPrivate", &Ldn::scan);
    cmd(s, 104, "SetWirelessControllerRestriction", &Ldn::accept);
    cmd(s, 105, "SetWirelessAudioPolicy", &Ldn::accept);
    cmd(s, 106, "SetProtocol", &Ldn::accept);
    cmd(s, 200, "OpenAccessPoint", &Ldn::open_access_point);
    cmd(s, 201, "CloseAccessPoint", &Ldn::close_access_point);
    cmd(s, 202, "CreateNetwork", &Ldn::create_network);
    cmd(s, 203, "CreateNetworkPrivate", &Ldn::create_network_private);
    cmd(s, 204, "DestroyNetwork", &Ldn::destroy_network);
    cmd(s, 205, "Reject", &Ldn::reject);
    cmd(s, 206, "SetAdvertiseData", &Ldn::set_advertise_data);
    cmd(s, 207, "SetStationAcceptPolicy", &Ldn::set_accept_policy);
    cmd(s, 208, "AddAcceptFilterEntry", &Ldn::add_accept_filter);
    cmd(s, 209, "ClearAcceptFilter", &Ldn::clear_accept_filter);
    cmd(s, 300, "OpenStation", &Ldn::open_station);
    cmd(s, 301, "CloseStation", &Ldn::close_station);
    cmd(s, 302, "Connect", &Ldn::connect);
    cmd(s, 303, "ConnectPrivate", &Ldn::connect_private);
    cmd(s, 304, "Disconnect", &Ldn::disconnect);
    cmd(s, 400, "Initialize", &Ldn::initialize);
    cmd(s, 401, "Finalize", &Ldn::finalize);
    cmd(s, 402, "InitializeWithVersion", &Ldn::initialize);
    cmd(s, 403, "SetOperationMode", &Ldn::accept);
    // 500..601 (action frames, home channel, tx power; Switchbrew, 18.0.0+) stay unregistered: the IPC layer logs them if a game asks.
}

void Ldn::admit_read() {
    if (!hold_) return;
    std::vector<uint32_t> v;
    for (const uint32_t ip : node_vip_)
        if (ip) v.push_back(ip);
    hold_->admit(v);
}

void Ldn::drop_hold() {
    if (!hold_) return;
    hold_->detach();   // before the lobby's link can be the current link again (a Connect as station)
    hold_.reset();
}

void Ldn::set_state(uint32_t s) {
    if (state_ != s) NXLOG_I("net", "ldn: state %u -> %u", state_, s);
    state_ = s;
    status_locked();
    ev_.signal();
}

vnet::LobbyEvents Ldn::make_events() {
    std::weak_ptr<Ldn> w = weak_from_this();
    vnet::LobbyEvents ev;
    ev.room_state = [w](const vnet::Room& r) {
        if (auto s = w.lock()) s->on_room_state(r);
    };
    ev.left = [w](vnet::LeftReason why) {
        if (auto s = w.lock()) s->on_left(why);
    };
    return ev;
}

// What refers to a lobby: lobby_ (and the process slot, auto_relay()), its event sinks (set_events), the vnet current link (its link() for
// a station, hold_ in front of it for a host), hold_ itself, and the local copies the commands take under op_mu_. Outside a room the last
// three are clear (leave_network / on_left unhook the current link and drop hold_ before the state leaves 3/5), and the commands are
// serialised by op_mu_, which the caller holds, so no command has a copy in flight and none can enter a room meanwhile; sinks only ever
// take the state out of a room. So the move is refused while in a room, and otherwise: swap lobby_ under mu_, then (mu_ released, as a
// sink in progress may be waiting for it) clear the old lobby's sinks, which waits for a sink in progress and guarantees none runs after,
// and set the new one's. The old relay client stays in slot().relays as a scan client (no sinks: a client in no room gets no events)
// until no discovery lists it for 10 s. Scan results are kept per relay (scan_wire_, tag_relay_).
void Ldn::adopt(std::shared_ptr<vnet::Lobby> next) {
    std::shared_ptr<vnet::Lobby> old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (lobby_ == next || in_network()) return;
        drop_hold();   // none outside a room
        old = std::move(lobby_);
        lobby_ = next;
    }
    if (old) {
        if (const auto cur = vnet::current_link(); cur && cur == old->link()) vnet::set_current_link(nullptr);   // none outside a room
        old->set_events({});
    }
    if (next) next->set_events(make_events());
}

// ---- lobby events (lobby thread) ------------------------------------------------------------------------------------------------------

// The host enforces its station accept policy (SetStationAcceptPolicy, AddAcceptFilterEntry) on every node that joins: a node the policy
// refuses never reaches the game's node table (its datagrams stay held, kHoldMs) and is rejected through the relay, which turns it away
// from this room for good (its Connect answers ConnectRejected: join() below). Nodes already in the room stay when the policy changes,
// as on the console; policy 1 rejects every newcomer (pokopia-native's coop_ldn.c does the same at its connect), 2 and 3 compare the
// node's MAC as the game sees it (make_info: tagged_mac) with the filter.
bool Ldn::admits(const std::array<uint8_t, 6>& mac) const {
    switch (accept_policy_) {
        case 1: return false;
        case 2: return filter_.count(mac) == 0;
        case 3: return filter_.count(mac) != 0;
        default: return true;
    }
}

void Ldn::on_room_state(const vnet::Room& r0) {
    std::vector<std::pair<uint32_t, std::string>> refuse;
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        room_cv_.notify_all();
        if (!in_network() || r0.room_id != room_id_) return;
        // a state older than one already applied (an in-process relay delivers on several threads): the newer one stands
        if (r0.seq && r0.seq < last_seq_) return;
        vnet::Room r = r0;
        if (state_ == kAccessPointCreated) {
            std::set<std::pair<uint32_t, std::string>> listed;
            for (const vnet::RoomNode& n : r.nodes) listed.insert({n.vip, n.nickname});
            for (auto it = refused_.begin(); it != refused_.end();) it = listed.count(*it) ? std::next(it) : refused_.erase(it);
            const auto known = [&](uint32_t vip) { return std::find(node_vip_.begin(), node_vip_.end(), vip) != node_vip_.end(); };
            for (auto it = r.nodes.begin(); it != r.nodes.end();) {
                const std::pair<uint32_t, std::string> key{it->vip, it->nickname};
                const bool fresh = it->node_id != 0 && it->vip != vip_ && !known(it->vip);
                if (fresh && !refused_.count(key) && !admits(tagged_mac(it->mac, tag_))) {
                    refused_.insert(key);
                    refuse.push_back(key);
                    NXLOG_I("net", "ldn: %s (%s) refused by the station accept policy %u", vnet::ip_str(it->vip).c_str(), it->nickname.c_str(),
                            accept_policy_);
                }
                it = refused_.count(key) ? r.nodes.erase(it) : std::next(it);
            }
        }
        apply_room(r);
        ev_.signal();
        lobby = lobby_;
    }
    // Off the lobby's event thread: a sink must not call into its own lobby (a loopback relay delivers the room state that follows on
    // the calling thread, inside this sink's delivery). The relay keeps the node out from the REJECT on; the game never saw it.
    // The relay's REJECT names an address only: a node is rejected only while the room still lists that address with that player, so a
    // late thread never turns away the next player who got the slot.
    if (lobby && !refuse.empty())
        std::thread([lobby, refuse] {
            for (const auto& [vip, nick] : refuse) {
                const vnet::Room now = lobby->room();
                const bool still = std::any_of(now.nodes.begin(), now.nodes.end(), [&](const vnet::RoomNode& n) { return n.vip == vip && n.nickname == nick; });
                if (still) lobby->reject(vip);
            }
        }).detach();
    // the status file is written off this sink's thread too: net_status's writer must not run under a lock a lobby sink holds
    if (auto self = weak_from_this().lock())
        std::thread([self] { self->flush_status(); }).detach();
}

void Ldn::on_left(vnet::LeftReason why) {
    std::lock_guard<std::mutex> lk(mu_);
    room_cv_.notify_all();
    if (!in_network()) {
        last_left_ = static_cast<int>(why);   // a Connect still waiting on its room (join) reads it
        return;
    }
    if (auto self = weak_from_this().lock())
        std::thread([self] { self->flush_status(); }).detach();   // after this sink (it takes mu_ itself)
    int16_t reason = kSignalLost;
    switch (why) {
        case vnet::LeftReason::HostClosed: reason = kDestroyedByUser; break;
        case vnet::LeftReason::Rejected: reason = kRejected; break;
        case vnet::LeftReason::Shutdown: reason = kDestroyedBySystem; break;
        default: reason = state_ == kAccessPointCreated ? kDestroyedBySystem : kSignalLost; break;
    }
    NXLOG_I("net", "ldn: left room %u (reason %d)", room_id_, reason);
    vnet::set_current_link(nullptr);
    drop_hold();
    reason_ = reason;
    room_id_ = vip_ = 0;
    info_ = NetworkInfo{};
    node_vip_ = {};
    set_state(state_ == kAccessPointCreated ? kAccessPoint : kStation);
}

void Ldn::apply_room(const vnet::Room& r) {
    status_locked();
    if (r.seq > last_seq_) last_seq_ = r.seq;
    info_ = make_info(r, accept_policy_, vip_, user_name_, tag_);
    if (priv_) {   // the SecurityParameter the game created (host) or connected (station) with
        std::memcpy(info_.ldn.security_parameter, priv_sp_.data, 16);
        std::memcpy(info_.id.session_id, priv_sp_.session_id, 16);
    }
    std::array<uint32_t, 8> now{};
    for (const vnet::RoomNode& n : r.nodes)
        if (n.node_id < 8) now[n.node_id] = n.vip;
    for (size_t i = 0; i < 8; ++i) {
        if (now[i] == node_vip_[i]) continue;
        if (node_vip_[i]) latest_[i] |= kNodeDisconnect;
        if (now[i]) latest_[i] |= kNodeConnect;
    }
    node_vip_ = now;
    if (hold_) {
        std::vector<uint32_t> v;
        for (const uint32_t ip : now)
            if (ip) v.push_back(ip);
        hold_->members(v);
    }
}

// ---- state and info ---------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::get_state(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    rsp.push(state_);
    return 0;
}

uint32_t Ldn::get_network_info(ipc::Request& req, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    buf_write(req.out_buf(0), &info_, sizeof info_);
    admit_read();
    return 0;
}

uint32_t Ldn::get_ipv4(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    rsp.push(vip_);
    rsp.push(kSubnetMask);
    return 0;
}

uint32_t Ldn::get_disconnect_reason(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    rsp.push(reason_);
    return 0;
}

uint32_t Ldn::get_security_parameter(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    SecurityParameter sp{};
    std::memcpy(sp.data, info_.ldn.security_parameter, 16);
    std::memcpy(sp.session_id, info_.id.session_id, 16);
    rsp.push(sp);
    return 0;
}

uint32_t Ldn::get_network_config(ipc::Request&, ipc::Response& rsp) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    NetworkConfig c{};
    c.intent = info_.id.intent;
    c.channel = static_cast<uint16_t>(info_.common.channel);
    c.node_count_max = info_.ldn.node_count_max;
    c.local_comm_version = static_cast<uint16_t>(info_.ldn.nodes[0].local_comm_version);
    rsp.push(c);
    return 0;
}

uint32_t Ldn::attach_event(ipc::Request&, ipc::Response& rsp) {
    rsp.out_copy.push_back(ev_.new_handle());
    return 0;
}

uint32_t Ldn::get_latest_update(ipc::Request& req, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!in_network()) return kInvalidState;
    buf_write(req.out_buf(0), &info_, sizeof info_);
    NodeLatestUpdate up[8] = {};
    for (size_t i = 0; i < 8; ++i) up[i].state_change = latest_[i];
    buf_write(req.out_buf(1), up, sizeof up);
    latest_ = {};
    admit_read();
    return 0;
}

// ---- scan ----------------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::scan(ipc::Request& req, ipc::Response& rsp) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == kNone) return kInvalidState;
        // With no lobby every initialized state answers an empty scan (the offline stub's answer); with one (or auto), Switchbrew's 2..5.
        if ((lobby_ || auto_) && (state_ < kAccessPoint || state_ > kStationConnected)) return kInvalidState;
    }
    const ScanFilter f = arg_at<ScanFilter>(req, 8);   // after the s16 channel, 8-aligned
    const ipc::Buffer out = req.out_buf(0);
    const size_t cap = static_cast<size_t>(out.size / sizeof(NetworkInfo));
    const uint64_t lcid = (f.flag & kFilterLocalCommunicationId) ? f.id.intent.local_comm_id : 0;
    const uint16_t scene = (f.flag & kFilterSceneId) ? f.id.intent.scene_id : 0xFFFF;
    uint16_t count = 0;
    if (cap)
        for (const auto& [r, tag] : scan_rooms(lcid, scene)) {
            if (count >= cap) break;
            const NetworkInfo ni = make_info(r, 0, 0, {}, tag);
            if (!filter_match(f, ni)) continue;
            std::memcpy(buf_mut(out) + count * sizeof(NetworkInfo), &ni, sizeof ni);
            ++count;
        }
    rsp.push(static_cast<uint32_t>(count));   // s16 total (the high half is zero)
    return 0;
}

std::vector<std::pair<vnet::Room, uint64_t>> Ldn::scan_rooms(uint64_t lcid, uint16_t scene) {
    bool all;   // auto mode outside a room: the rooms of every relay on the LAN
    std::shared_ptr<vnet::Lobby> lobby;
    uint32_t own_room;
    {
        std::lock_guard<std::mutex> lk(mu_);
        all = auto_ && !in_network();
        lobby = lobby_;
        own_room = room_id_;
    }
    std::vector<std::pair<vnet::Room, uint64_t>> res;
    auto put = [&](const vnet::Room& r, uint64_t tag) {
        SessionKey sid;
        size_t n = 0;
        if (private_of(r.adv, &sid, &n)) {
            std::lock_guard<std::mutex> lk(mu_);
            priv_ref_[sid] = SessionRef{r.room_id, tag};
        }
        res.emplace_back(r, tag);
    };
    if (all) {
        // Every relay found, merged. One relay reached by two addresses (a relay on this PC answers on 127.0.0.1 and on its LAN address,
        // a PC on two networks on both) has one tag: it is listed once, by its loopback address if it has one.
        std::vector<RelayRooms> got = scan_relays(discover_cached(), lcid, scene);
        std::stable_sort(got.begin(), got.end(), [](const RelayRooms& a, const RelayRooms& b) { return a.loopback && !b.loopback; });
        std::set<uint64_t> tags;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (RelayRooms& rr : got) {
                if (rr.tag && !tags.insert(rr.tag).second) {
                    rr.rooms.clear();
                    continue;
                }
                tag_relay_[rr.tag] = {rr.host, rr.port};
                for (const vnet::Room& r : rr.rooms) scan_wire_[{rr.tag, r.room_id}] = r.comm_version;
            }
        }
        for (const RelayRooms& rr : got)
            for (const vnet::Room& r : rr.rooms) put(r, rr.tag);
    } else if (lobby) {
        std::vector<vnet::Room> rooms;
        const int e = lobby->scan(lcid, scene, &rooms);
        if (e) NXLOG_D("net", "ldn: scan failed: %s", vnet::lobby_err_str(e));
        const uint64_t tag = relay_tag(*lobby);
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const vnet::Room& r : rooms) scan_wire_[{tag, r.room_id}] = r.comm_version;
        }
        for (const vnet::Room& r : rooms)
            if (r.room_id != own_room) put(r, tag);
    }
    return res;
}

SessionRef Ldn::ref_of(const uint8_t (&sid)[16]) {
    const SessionRef s = session_of(sid);
    if (s.room) return s;
    SessionKey k;
    std::memcpy(k.data(), sid, 16);
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = priv_ref_.find(k);
    return it == priv_ref_.end() ? SessionRef{} : it->second;
}

// ---- access point ----------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::open_access_point(ipc::Request&, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != kInitialized) return kInvalidState;
    set_state(kAccessPoint);
    return 0;
}

void Ldn::leave_network(uint32_t next, int16_t reason) {
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (in_network()) {
            vnet::set_current_link(nullptr);
            drop_hold();
            lobby = lobby_;
            room_id_ = vip_ = 0;
            info_ = NetworkInfo{};
            node_vip_ = {};
            reason_ = reason;
        }
        set_state(next);
    }
    if (lobby) lobby->leave();   // outside mu: the loopback relay may run sinks of this very object
}

uint32_t Ldn::close_access_point(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
        accept_policy_ = 0;   // the access point's settings go with it (pokopia-native resets the policy on the way to Initialized)
        filter_.clear();
        refused_.clear();
    }
    leave_network(kInitialized, kDestroyedByUser);
    return 0;
}

uint32_t Ldn::create_network(ipc::Request& req, ipc::Response&) {
    return create(arg_at<SecurityConfig>(req, 0), arg_at<UserConfig>(req, kCreateUser), arg_at<NetworkConfig>(req, kCreateNetCfg));
}

// CreateNetworkPrivate(SecurityConfig, SecurityParameter, UserConfig, NetworkConfig; AddressList in a buffer): the session id and
// security data are the game's (GetSecurityParameter answers them, its stations ConnectPrivate with them). The address list is the
// console's static addressing for a private network; the relay hands out the addresses here, as for any room.
uint32_t Ldn::create_network_private(ipc::Request& req, ipc::Response&) {
    const SecurityParameter sp = arg_at<SecurityParameter>(req, kCreatePrivSecParam);
    return create(arg_at<SecurityConfig>(req, 0), arg_at<UserConfig>(req, kCreatePrivUser), arg_at<NetworkConfig>(req, kCreatePrivNetCfg), &sp);
}

uint32_t Ldn::create(const SecurityConfig& sec, const UserConfig& user, const NetworkConfig& cfg, const SecurityParameter* priv) {
    bool host_auto;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint) return kInvalidState;
        if (cfg.node_count_max < 1 || cfg.node_count_max > vnet::kMaxNodes || sec.passphrase_size > sizeof sec.passphrase)
            return kInvalidArgument;
        host_auto = auto_;
    }
    if (host_auto) adopt(host_relay());   // state 2 (no room) and op_mu_ held: nothing can enter one meanwhile
    std::shared_ptr<vnet::Lobby> lobby;
    vnet::RoomConfig rc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!lobby_) return kDeviceNotAvailable;
        lobby = lobby_;
        rc.local_comm_id = cfg.intent.local_comm_id;
        rc.scene_id = cfg.intent.scene_id;
        rc.comm_version = wire_version(static_cast<int16_t>(cfg.local_comm_version));
        rc.node_count_max = cfg.node_count_max;
        rc.security_mode = static_cast<uint8_t>(sec.mode);
        rc.pass_hash = vnet::passphrase_hash(sec.passphrase, sec.passphrase_size);
        rc.adv = adv_;
        priv_ = priv != nullptr;
        if (priv) {
            priv_sp_ = *priv;
            if (adv_.size() + kPrivTrailer <= vnet::kMaxAdvertise) rc.adv = with_private(adv_, priv->session_id);
            else NXLOG_W("net", "ldn: CreateNetworkPrivate: %zu bytes of advertise data leave no room for the session id: stations find it by Scan only", adv_.size());
        }
        user_name_.assign(user.user_name, strnlen(user.user_name, 32));
    }
    vnet::Joined j;
    const int e = lobby->create(rc, &j);
    if (e) {
        NXLOG_I("net", "ldn: CreateNetwork failed: %s", vnet::lobby_err_str(e));
        return kDeviceNotAvailable;
    }
    const uint64_t tag = relay_tag(*lobby);   // what visitors put in the session id for this relay (every mode: all nodes agree)
    std::lock_guard<std::mutex> lk(mu_);
    tag_ = tag;
    room_id_ = j.room_id;
    vip_ = j.vip;
    reason_ = kReasonNone;
    node_vip_ = {};
    latest_ = {};
    vnet::Room r = lobby->room();
    // No ROOM_STATE yet: the host alone. This is the whole room (node 0 = this node, connected, with the advertise data), so CreateNetwork
    // needs no wait; a ROOM_STATE that comes later replaces it.
    if (r.room_id != j.room_id || r.nodes.empty()) {
        r.room_id = j.room_id;
        r.local_comm_id = rc.local_comm_id;
        r.scene_id = rc.scene_id;
        r.comm_version = rc.comm_version;
        r.node_count_max = rc.node_count_max;
        r.security_mode = rc.security_mode;
        r.adv = rc.adv;   // with a private room's session id: make_info shows the game's part
        r.nodes = {{0, j.vip, {0x02, 0x00, 0xA9, 0xFE, static_cast<uint8_t>(j.vip >> 8), static_cast<uint8_t>(j.vip)}, user_name_}};
    }
    drop_hold();
    hold_ = std::make_shared<vnet::HoldLink>(lobby->link(), kHoldMs, kHoldCap);
    apply_room(r);
    vnet::set_current_link(hold_);
    std::string what;
    if (const uint8_t var = variant_of(rc.comm_version); var && own_variant(&what) == var)
        NXLOG_I("net", "ldn: created room %u as %s with %s (session variant %u: only players with it can join)", room_id_,
                vnet::ip_str(vip_).c_str(), what.c_str(), var);
    else NXLOG_I("net", "ldn: created room %u as %s", room_id_, vnet::ip_str(vip_).c_str());
    set_state(kAccessPointCreated);
    return 0;
}

uint32_t Ldn::destroy_network(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPointCreated) return kInvalidState;
    }
    leave_network(kAccessPoint, kDestroyedByUser);
    return 0;
}

uint32_t Ldn::reject(ipc::Request& req, ipc::Response&) {
    const uint32_t ip = req.arg<uint32_t>(0);
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPointCreated) return kInvalidState;
        bool found = false;
        for (size_t i = 1; i < 8; ++i) found |= node_vip_[i] == ip && ip != 0;
        if (!found) return kNodeNotFound;
        lobby = lobby_;
    }
    lobby->reject(ip);   // the server answers with a ROOM_STATE without that node
    return 0;
}

uint32_t Ldn::set_advertise_data(ipc::Request& req, ipc::Response&) {
    const ipc::Buffer b = req.in_buf(0);
    if (b.size > vnet::kMaxAdvertise) return kInvalidArgument;
    std::shared_ptr<vnet::Lobby> lobby;
    std::vector<uint8_t> adv(buf_ptr(b), buf_ptr(b) + (b.addr ? b.size : 0));
    NXLOG_D("net", "ldn: SetAdvertiseData %zu bytes: %s", adv.size(), hex_head(adv).c_str());
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
        adv_ = adv;
        if (state_ == kAccessPointCreated) {
            lobby = lobby_;
            info_.ldn.advertise_data_size = static_cast<uint16_t>(adv.size());
            std::memset(info_.ldn.advertise_data, 0, sizeof info_.ldn.advertise_data);
            if (!adv.empty()) std::memcpy(info_.ldn.advertise_data, adv.data(), adv.size());
        }
    }
    if (lobby) {
        bool priv;
        uint8_t sid[16];
        {
            std::lock_guard<std::mutex> lk(mu_);
            priv = priv_ && state_ == kAccessPointCreated && adv.size() + kPrivTrailer <= vnet::kMaxAdvertise;
            std::memcpy(sid, priv_sp_.session_id, 16);
        }
        lobby->set_advertise(priv ? with_private(adv, sid) : adv);
    }
    return 0;
}

uint32_t Ldn::set_accept_policy(ipc::Request& req, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint8_t p = req.arg<uint8_t>(0);
    if (p > 3) return kInvalidArgument;   // AcceptPolicy 0..3 (Ryujinx Ldn/Types/AcceptPolicy.cs; pokopia-native coop_ldn.c refuses others)
    if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
    accept_policy_ = p;                     // reported in NetworkInfo, enforced on the next joins (on_room_state)
    info_.ldn.station_accept_policy = accept_policy_;
    return 0;
}

// AddAcceptFilterEntry(MacAddress): a 6-byte station MAC in the raw arguments.
uint32_t Ldn::add_accept_filter(ipc::Request& req, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
    std::array<uint8_t, 6> mac{};
    for (size_t i = 0; i < 6; ++i) mac[i] = req.arg<uint8_t>(i);
    filter_.insert(mac);
    return 0;
}

uint32_t Ldn::clear_accept_filter(ipc::Request&, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
    filter_.clear();
    return 0;
}

// ---- station ----------------------------------------------------------------------------------------------------------------------------

uint32_t Ldn::open_station(ipc::Request&, ipc::Response&) {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != kInitialized) return kInvalidState;
    set_state(kStation);
    return 0;
}

uint32_t Ldn::close_station(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kStation && state_ != kStationConnected) return kInvalidState;
    }
    leave_network(kInitialized, kDisconnectedByUser);
    return 0;
}

uint32_t Ldn::connect(ipc::Request& req, ipc::Response&) {
    NetworkInfo ni{};
    buf_read(req.in_buf(0), &ni, sizeof ni);
    return join(ref_of(ni.id.session_id), ni.ldn.nodes[0].local_comm_version, arg_at<SecurityConfig>(req, 0),
                arg_at<UserConfig>(req, kConnectUser), req.arg<int32_t>(kConnectVersion));
}

uint32_t Ldn::connect_private(ipc::Request& req, ipc::Response&) {
    const SecurityParameter sp = arg_at<SecurityParameter>(req, kConnPrivSecParam);
    const NetworkConfig cfg = arg_at<NetworkConfig>(req, kConnPrivNetCfg);
    SessionRef ref = ref_of(sp.session_id);
    if (!ref.room) {   // not seen yet: the station looks for it, as the console's station does before it associates
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kStation) return kInvalidState;
        }
        scan_rooms(cfg.intent.local_comm_id, cfg.intent.scene_id);
        ref = ref_of(sp.session_id);
    }
    return join(ref, static_cast<int16_t>(cfg.local_comm_version), arg_at<SecurityConfig>(req, 0), arg_at<UserConfig>(req, kConnPrivUser),
                req.arg<int32_t>(kConnPrivVersion), &sp);
}

uint32_t Ldn::join(const SessionRef& sref, int16_t room_version, const SecurityConfig& sec, const UserConfig& user, int32_t version,
                   const SecurityParameter* priv) {
    const uint32_t room_id = sref.room;
    bool is_auto;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == kNone) return kInvalidState;
        is_auto = auto_;
    }
    // Auto mode: the session id names the room's relay; the node moves to that relay's client first (state 4, no room: adopt() is safe).
    // A tag no Scan has listed (another PC's session id, a relay forgotten since): not found, rather than the same room id on another
    // relay. Tag 0: the lobby in use.
    if (is_auto && sref.tag && room_id) {
        std::pair<std::string, uint16_t> where;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kStation) return kInvalidState;
            const auto it = tag_relay_.find(sref.tag);
            if (it == tag_relay_.end()) {
                NXLOG_I("net", "ldn: Connect to room %u: its relay is not one a Scan found", room_id);
                return kConnectNotFound;
            }
            where = it->second;
        }
        adopt(auto_relay(where.first, where.second));
    }
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!lobby_) return kConnectNotFound;   // no radio: nothing to connect to (the offline stub's answer, any state)
        if (state_ != kStation) return kInvalidState;
        if (sec.passphrase_size > sizeof sec.passphrase) return kInvalidArgument;
        lobby = lobby_;
        user_name_.assign(user.user_name, strnlen(user.user_name, 32));
    }
    if (room_id == 0) return kConnectNotFound;
    const uint64_t tag = relay_tag(*lobby);
    if (is_auto && sref.tag && tag != sref.tag) {   // the relay at that address restarted (a new instance id): its rooms are gone
        NXLOG_I("net", "ldn: Connect to room %u: its relay is not the one the Scan listed any more", room_id);
        return kConnectNotFound;
    }
    vnet::Joined j;
    const int16_t wire = wire_version(static_cast<int16_t>(version));
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_left_ = -1;
    }
    const auto join_start = std::chrono::steady_clock::now();
    const int e = lobby->join(room_id, wire, vnet::passphrase_hash(sec.passphrase, sec.passphrase_size), &j);
    const auto join_rtt = std::chrono::steady_clock::now() - join_start;
    if (e) {
        NXLOG_I("net", "ldn: Connect to room %u failed: %s", room_id, vnet::lobby_err_str(e));
        if (e == vnet::kLobbyVersion) {
            int16_t room_wire = room_version;
            bool known = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                const auto it = scan_wire_.find({tag, room_id});
                if (it != scan_wire_.end()) room_wire = it->second, known = true;
            }
            std::string what;
            const uint8_t mine = own_variant(&what), theirs = variant_of(room_wire);
            if (known && mine != theirs && game_version(room_wire) == game_version(wire)) {
                // the same game, another session variant: every player needs the same setting
                const char* name = what.empty() ? "a gameplay setting" : what.c_str();
                if (mine && theirs) NXLOG_W("net", "ldn: Connect to room %u refused: its session variant is %u, this PC's %u (%s)", room_id, theirs, mine, name);
                else if (mine) NXLOG_W("net", "ldn: Connect to room %u refused: the host has %s off, this PC has it on. Every player of a session needs the same setting", room_id, name);
                else NXLOG_W("net", "ldn: Connect to room %u refused: the host has %s on (session variant %u), this PC has it off. Every player of a session needs the same setting", room_id, name, theirs);
                return kConnectRejected;
            }
        }
        switch (e) {
            case vnet::kLobbyNoRoom:
            case vnet::kLobbyRoomCode: return kConnectNotFound;   // a room with another code: as if it were not there
            case vnet::kLobbyRoomFull: return kTooManyPlayers;
            case vnet::kLobbyVersion: return version < room_version ? kVersionTooLow : kVersionTooHigh;
            case vnet::kLobbyRejected: return kConnectRejected;
            case vnet::kLobbyTimeout: return kConnectTimeout;
            default: return kConnectFailure;   // wrong passphrase, no server
        }
    }
    // JOINED carries only the room id and this node's address; the node table and the advertise data come in the ROOM_STATE behind it.
    // The game reads both as soon as Connect returns (GetNetworkInfo: node[0] is the host it sends to, isConnected and the advertise data
    // with the host's session member id), so Connect returns only once the room state that lists the host and this node has been applied.
    // The room events notify room_cv_; the 50 ms slices also catch a state that came without an event. No such state within
    // kRoomStateWaitMs, or the lobby left the room meanwhile (its room() is then cleared): leave and fail like a failed join.
    std::unique_lock<std::mutex> lk(mu_);
    vnet::Room r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRoomStateWaitMs);
    for (;;) {
        r = lobby->room();
        if (r.room_id != j.room_id || room_lists(r, j.vip) || std::chrono::steady_clock::now() >= deadline) break;
        room_cv_.wait_for(lk, std::chrono::milliseconds(50));
    }
    // The lobby clears its room before its "left" sink runs (on_left notes the reason in last_left_): a room seen gone waits a moment for
    // the reason, so a REJECT is told from a lost connection whatever thread got there first.
    const auto left_reason = [&] {
        if (last_left_ < 0) room_cv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return last_left_ >= 0; });
        return last_left_;
    };
    if (r.room_id != j.room_id || !room_lists(r, j.vip)) {
        const bool rejected = r.room_id != j.room_id && left_reason() == static_cast<int>(vnet::LeftReason::Rejected);   // on_room_state
        lk.unlock();
        NXLOG_I("net", "ldn: Connect to room %u: %s, leaving", j.room_id,
                rejected ? "the host's accept policy refused this node"
                : r.room_id != j.room_id ? "out of the room before its state came" : "no room state listing this node within 5 s");
        lobby->leave();
        return rejected ? kConnectRejected : kConnectFailure;
    }
    // The host's station accept policy (on_room_state) answers a join with a REJECT one relay round trip after it saw the node: the
    // console's access point refuses the association, so Connect waits that long (twice the JOIN's own round trip, 100 ms .. 1 s) and
    // answers ConnectRejected when the lobby is turned out meanwhile. The link is current during the wait, so the host's first datagrams
    // are not lost. A room left for another reason in that time fails the Connect as a lost connection would. A REJECT that comes later
    // (a host on a loaded machine) puts the station out of the network at once with DisconnectReason Rejected (on_left).
    vnet::set_current_link(lobby->link());
    {
        const auto grace = std::clamp<std::chrono::steady_clock::duration>(2 * join_rtt, std::chrono::milliseconds(100), std::chrono::seconds(1));
        const auto until = std::chrono::steady_clock::now() + grace;
        while (lobby->room().room_id == j.room_id && last_left_ < 0 && std::chrono::steady_clock::now() < until)
            room_cv_.wait_until(lk, std::min(until, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
        if (lobby->room().room_id != j.room_id || last_left_ >= 0) {
            const bool rejected = left_reason() == static_cast<int>(vnet::LeftReason::Rejected);
            if (const auto cur = vnet::current_link(); cur && cur == lobby->link()) vnet::set_current_link(nullptr);
            NXLOG_I("net", "ldn: Connect to room %u: %s", j.room_id, rejected ? "the host's accept policy refused this node" : "out of the room at once");
            return rejected ? kConnectRejected : kConnectFailure;
        }
        r = lobby->room();
    }
    tag_ = relay_tag(*lobby);   // after JOIN the WELCOME is in: the same value the host computed
    room_id_ = j.room_id;
    vip_ = j.vip;
    reason_ = kReasonNone;
    node_vip_ = {};
    latest_ = {};
    priv_ = priv != nullptr;
    if (priv) priv_sp_ = *priv;
    apply_room(r);
    NXLOG_I("net", "ldn: joined room %u as %s (node %u)", room_id_, vnet::ip_str(vip_).c_str(), j.node_id);
    set_state(kStationConnected);
    return 0;
}

uint32_t Ldn::disconnect(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kStationConnected) return kInvalidState;
    }
    leave_network(kStation, kDisconnectedByUser);
    return 0;
}

// ---- lifetime ---------------------------------------------------------------------------------------------------------------------------

// Every Ldn that has been initialized, for the launcher's "leave" (net-control.txt), which is registered with each Initialize.
std::mutex g_live_mu;
std::vector<std::weak_ptr<Ldn>> g_live;

void Ldn::leave_from_launcher() {
    std::lock_guard<std::mutex> op(op_mu_);
    uint32_t st;
    {
        std::lock_guard<std::mutex> lk(mu_);
        st = state_;
    }
    if (st == kAccessPointCreated) leave_network(kAccessPoint, kDestroyedBySystem);
    else if (st == kStationConnected) leave_network(kStation, kDisconnectedBySystem);
    else return;
    NXLOG_I("net", "ldn: left the room on the launcher's leave command");
    flush_status();
}

uint32_t Ldn::initialize(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        g_live.erase(std::remove_if(g_live.begin(), g_live.end(), [](const std::weak_ptr<Ldn>& w) { return w.expired(); }), g_live.end());
        g_live.push_back(weak_from_this());
    }
    net_status::on_command("ldn", [](const std::string& c) {
        if (c != "leave") return;   // "join": a game joins from its own menus
        std::vector<std::shared_ptr<Ldn>> live;
        {
            std::lock_guard<std::mutex> lk(g_live_mu);
            for (const auto& w : g_live)
                if (auto l = w.lock()) live.push_back(std::move(l));
        }
        for (const auto& l : live) l->leave_from_launcher();
    });
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kNone) return 0;   // already initialized: accepted
        if (!offline_) lobby_ = process_lobby(&auto_);   // auto: the relay picked last in this process, or none yet
        lobby = lobby_;
        reason_ = kReasonNone;
        set_state(kInitialized);
    }
    if (!lobby) return 0;
    lobby->set_events(make_events());
    // Connect now, off the game's thread (Initialize runs at start-up): the first Scan/Create then finds the connection up, or waits for it.
    if (!lobby->connected()) std::thread([lobby] { lobby->connect(); }).detach();
    return 0;
}

uint32_t Ldn::finalize(ipc::Request&, ipc::Response&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == kNone) return kInvalidState;
    }
    shutdown();
    return 0;
}

void Ldn::shutdown() {
    leave_network(kNone, kDisconnectedByUser);
    std::shared_ptr<vnet::Lobby> lobby;
    {
        std::lock_guard<std::mutex> lk(mu_);
        lobby.swap(lobby_);
    }
    if (lobby) lobby->set_events({});
}

// =============================================================================================================================================
// [net] protocol = native (the default): ldn:u on pokopia-native's co-op module (third_party/pokopia-coop, MIT @035bd9c), so this build
// and pokopia-native builds of the same main meet in one Link Play session. The module owns the nn::ldn state machine, the 0x480
// NetworkInfo, discovery (DNS-SD on the LAN), the transport (TCP control links in a star around the host, UDP for game datagrams) and the
// game's UDP sockets on 169.254.S.0/24 (bsd: network.cpp routes descriptors 0x100.. to coop_vnet_*). Each IPC command is one coop_ldn_*
// call (the structs are the SDK's, as the IPC carries them; results are nn::Result module 203). Wiring after pokopia-native's
// windows/src/coop/nxnative/ldn_glue.c.
// Config ([net], read once when the first ldn:u service object is made): mode = auto | join:<IPv4>[:<port>] | off (auto: host on the LAN
// while the game hosts, find LAN hosts with DNS-SD while it scans; without DNS-SD, Wine / Proton, a LAN sweep and mDNS answers: lan_sweep =
// auto | off), internet (0/1: internet play, every link proves the room code),
// room_code (the host's code; a host without one gets a new one, logged and written to room-code.txt; a visitor needs the host's),
// relay (<IPv4>[:<port>], a pokopia-native coop relay; mode auto only), uid (32 hex digits; default: the install's persistent uid,
// coop_uid_load). mode = off or an invalid setting: local wireless off (the Link Play backend's answers with no lobby).
// What peers compare before a join: main's build id (first 16 hex digits) and a mods fingerprint. The fingerprint is
// coop_mods_add(0, name) of the session variant's name (services_api.h ldnapi; Pokopia's "Interact everywhere" while armed, the same
// name pokopia-native adds as COOP_INTERACT_MOD), 0 without one. The variant's bits 12..14 of the local communication version are a
// Link Play (relay) thing and are not sent here: the game's version goes out as the game gave it.
// Story co-op (pokopia-native's "Story co-op (experimental)", docs/story-coop.md; ldnapi::set_story_mod, armed by games/pkp/pkp_linkplay.cpp)
// adds COOP_STORY_MOD after the Interact name, so both on give pokopia-native's fingerprint c75a2da0380038c1.
// The module has no private networks (pokopia-native cuts them): CreateNetworkPrivate makes a network with the module's session id and
// ConnectPrivate joins the one network of the config's game and scene a Scan finds. Not mapped: the accept filter (accepted in access
// point states and ignored; the policy itself is the module's). GetNetworkInfoLatestUpdate's per-node flags are
// built here from the node tables the game reads.
// =============================================================================================================================================

// [net] protocol, read when a service object is made (a game's setting does not change while it runs; tests change it between cases).
bool native_protocol() {
    const std::string p = lower(config_get_str("net", "protocol", "native"));
    if (p == "linkplay" || p == "lan" || p == "ryuldn") return false;
    if (p != "native") {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) NXLOG_W("net", "ldn: [net] protocol = \"%s\" is not native or linkplay: native", p.c_str());
    }
    return true;
}

struct NativeSlot {
    std::mutex mu;
    bool built = false;
    std::atomic<coop_node*> node{nullptr};
    ServiceEvent ev;   // AttachStateChangeEvent of every service object; signalled by the module's on_state
    std::string build;
};
NativeSlot& native() {
    static NativeSlot s;
    return s;
}

void coop_hook_log(void*, const char* line) { NXLOG_I("net", "coop: %s", line); }
void coop_hook_state(void*, int) { native().ev.signal(); }
const char* coop_hook_build(void*) {
    NativeSlot& s = native();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.build.empty()) {
        const Module* m = module_by_name("main");
        if (!m || m->build_id().empty()) m = module_at(mem::kMainBase);
        if (m) s.build = m->build_id();
    }
    return s.build.empty() ? nullptr : s.build.c_str();   // set once and never changed: the pointer stays valid
}
uint64_t coop_hook_mods(void*, char* what, size_t n) {
    std::string name;
    if (!own_variant(&name) || name.empty()) return 0;
    // pokopia-native's coop_hmods order: the cheats (none here), "Interact everywhere", then "Story co-op (experimental)"
    uint64_t h = coop_mods_add(0, name.c_str());
    if (story_flag().load()) {
        h = coop_mods_add(h, COOP_STORY_MOD);
        std::snprintf(what, n, "%s, %s", name.c_str(), COOP_STORY_MOD);
    } else {
        std::snprintf(what, n, "%s", name.c_str());
    }
    return h;
}

// The install's persistent coop uid (nn::account::Uid as 32 hex digits): coop-uid.txt in the save folder ([fs] save_dir), made from the
// OS random source on first use. The save folder, not the game folder: an install copied to another device must not take its identity
// along, and the save folder (per Windows user, per Wine prefix) outlives reinstalls and updates of the game folder. The fs service only
// reads *.meta files there.
std::string coop_uid_load() {
    namespace stdfs = std::filesystem;
    const std::string sd = save_dir();
    const stdfs::path dir(std::u8string(sd.begin(), sd.end())), file = dir / "coop-uid.txt";
    const std::u8string u8 = file.u8string();
    const std::string shown(u8.begin(), u8.end());
    auto valid = [](const std::string& s) {
        if (s.size() != 32 || s == std::string(32, '0')) return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        return true;
    };
    if (FILE* f = _wfopen(file.c_str(), L"rb")) {
        char b[64] = {};
        const size_t got = std::fread(b, 1, sizeof b - 1, f);
        std::fclose(f);
        std::string s(b, got);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (valid(s)) return s;
        NXLOG_W("net", "ldn: %s is not 32 hex digits: a new uid is made", shown.c_str());
    }
    std::random_device rd;   // rand_s on Windows: the OS random source
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    while (!valid(s)) {
        s.clear();
        for (int i = 0; i < 4; ++i) {
            const uint32_t v = rd();
            for (int k = 7; k >= 0; --k) s += kHex[(v >> (4 * k)) & 15];
        }
    }
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    if (FILE* f = _wfopen(file.c_str(), L"wb")) {
        std::fprintf(f, "%s\n", s.c_str());
        std::fclose(f);
        NXLOG_I("net", "ldn: new co-op uid kept in %s", shown.c_str());
    } else {
        NXLOG_W("net", "ldn: cannot write %s: this run has a uid of its own", shown.c_str());
    }
    return s;
}

// The process's coop node from [net] (made once; null: local wireless off).
coop_node* native_node() {
    NativeSlot& s = native();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.built) return s.node.load();
        s.built = true;
    }
    std::string mode = lower(config_get_str("net", "mode", "auto"));
    while (!mode.empty() && (mode.back() == ' ' || mode.back() == '\t')) mode.pop_back();
    while (!mode.empty() && (mode.front() == ' ' || mode.front() == '\t')) mode.erase(mode.begin());
    const bool internet = config_get_bool("net", "internet", false);
    std::string room = config_get_str("net", "room_code", "");
    if (lower(room) == "new") room.clear();   // a host without a code gets a new one
    const std::string relay = config_get_str("net", "relay", "");
    // Internet play through a relay is always mode = auto (the module refuses "join:" with a relay and Link Play would be off). A join:<ip>
    // left in the file from an earlier visit is not a choice in that case: use auto (#239). Every other combination is left as it is.
    if (internet && !relay.empty() && mode.compare(0, 5, "join:") == 0) {
        NXLOG_W("net", "ldn: [net] mode = %s cannot be combined with [net] relay = %s (a relay needs mode = auto): using auto", mode.c_str(), relay.c_str());
        mode = "auto";
    }
    std::string uid = config_get_str("net", "uid", "");
    if (uid.empty() && !mode.empty() && mode != "off" && mode != "0") uid = coop_uid_load();
    coop_cfg c;
    char err[256] = "";
    const int r = coop_cfg_parse_ex(&c, mode.c_str(), uid.c_str(), internet ? "1" : "0", room.c_str(), relay.c_str(), err, sizeof err);
    const std::string code_file = room_code_file();
    if (r <= 0) {
        if (r < 0) NXLOG_W("net", "ldn: [net] (protocol native): %s: local wireless off", err);
        else NXLOG_I("net", "ldn: [net] mode = off: local wireless off");
        std::remove(code_file.c_str());
        return nullptr;
    }
    // [net] lan_sweep = auto | off: auto (the default) finds and answers LAN players without DNS-SD (Wine / Proton: the module's LAN sweep and
    // mDNS responder, coop_disc.c x_*); off: there, visitors join by address. Windows with DNS-SD never uses them
    const std::string sweep = lower(config_get_str("net", "lan_sweep", "auto"));
    if (sweep != "auto" && sweep != "off") NXLOG_W("net", "ldn: [net] lan_sweep = \"%s\" is not auto or off: auto", sweep.c_str());
    c.x_lan_sweep_off = sweep == "off";
    c.hooks.user = nullptr;
    c.hooks.log = &coop_hook_log;
    c.hooks.sleep_ns = nullptr;   // a plain host sleep: ldn and bsd commands run on host threads
    c.hooks.on_state = &coop_hook_state;
    c.hooks.build = &coop_hook_build;
    c.hooks.mods = &coop_hook_mods;
    coop_node* n = coop_create(&c);
    if (!n) {
        NXLOG_W("net", "ldn: the co-op module did not start (no sockets): local wireless off");
        std::remove(code_file.c_str());
        return nullptr;
    }
    const char* role = c.role == COOP_ROLE_JOIN ? "join" : c.role == COOP_ROLE_HOST ? "host" : "auto";
    if (c.role == COOP_ROLE_JOIN)   // a join:<ip> left over from a visit: Invite Others makes a room nobody can reach (#239)
        NXLOG_W("net", "ldn: [net] mode = %s: this PC can only visit that host. To host (Invite Others to Visit), set [net] mode = auto in "
                "pkp.ini, or pick Host on the launcher's Link Play page", mode.c_str());
    NXLOG_I("net", "ldn: protocol native (pokopia-native's co-op), mode %s, port %u, %s", role, c.port,
            c.scope == COOP_SCOPE_INTERNET ? "internet play (links prove the room code; not encrypted)"
            : c.scope == COOP_SCOPE_LOOPBACK ? "loopback peers only" : "LAN peers only");
    if (c.auth) {
        char shown[COOP_ROOM_SHOWN];
        coop_room_format(c.room, shown);
        if (FILE* f = std::fopen(code_file.c_str(), "wb")) {
            std::fprintf(f, "%s\n", shown);
            std::fclose(f);
        }
        NXLOG_I("net", "ldn: internet play room code %s%s", shown, c.room_new ? " (new: visitors need it)" : "");
    } else {
        std::remove(code_file.c_str());
    }
    s.node.store(n);
    return n;
}

class NativeLdn : public std::enable_shared_from_this<NativeLdn> {
public:
    explicit NativeLdn(coop_node* n) : n_(n) {}
    void add_commands(ipc::Service& s);
    void shutdown() {
        coop_ldn_finalize(n_);
        forget();
    }

private:
    using H = uint32_t (NativeLdn::*)(ipc::Request&, ipc::Response&);
    void cmd(ipc::Service& s, uint32_t id, const char* name, H h);
    void forget() {
        std::lock_guard<std::mutex> lk(mu_);
        seen_ = {};
        latest_ = {};
    }
    // The game read `ni`: node joins and leaves since the last table it read go into latest_ (GetNetworkInfoLatestUpdate).
    void saw(const NetworkInfo& ni) {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < 8; ++i) {
            const uint32_t now = ni.ldn.nodes[i].is_connected ? ni.ldn.nodes[i].ipv4 : 0;
            if (now == seen_[i]) continue;
            if (seen_[i]) latest_[i] |= kNodeDisconnect;
            if (now) latest_[i] |= kNodeConnect;
            seen_[i] = now;
        }
    }
    // A command that may have left the network: the node table the game saw is gone.
    uint32_t left(uint32_t r) {
        const int st = coop_ldn_state(n_);
        if (st != COOP_LS_AP_CREATED && st != COOP_LS_STATION_CONNECTED) forget();
        return r;
    }

    uint32_t get_state(ipc::Request&, ipc::Response& rsp) {
        rsp.push(static_cast<uint32_t>(coop_ldn_state(n_)));
        return 0;
    }
    uint32_t get_network_info(ipc::Request& req, ipc::Response&) {
        NetworkInfo ni{};
        if (const uint32_t r = coop_ldn_network_info(n_, &ni)) return r;
        buf_write(req.out_buf(0), &ni, sizeof ni);
        saw(ni);
        return 0;
    }
    uint32_t get_ipv4(ipc::Request&, ipc::Response& rsp) {
        uint32_t ip = 0, mask = 0;
        if (const uint32_t r = coop_ldn_ipv4(n_, &ip, &mask)) return r;
        rsp.push(ip);
        rsp.push(mask);
        return 0;
    }
    uint32_t get_disconnect_reason(ipc::Request&, ipc::Response& rsp) {
        rsp.push(static_cast<int16_t>(coop_ldn_disconnect_reason(n_)));
        return 0;
    }
    uint32_t get_security_parameter(ipc::Request&, ipc::Response& rsp) {
        SecurityParameter sp{};
        if (const uint32_t r = coop_ldn_security_parameter(n_, &sp)) return r;
        rsp.push(sp);
        return 0;
    }
    uint32_t get_network_config(ipc::Request&, ipc::Response& rsp) {
        NetworkConfig c{};
        if (const uint32_t r = coop_ldn_network_config(n_, &c)) return r;
        rsp.push(c);
        return 0;
    }
    uint32_t attach_event(ipc::Request&, ipc::Response& rsp) {
        rsp.out_copy.push_back(native().ev.new_handle());
        return 0;
    }
    uint32_t get_latest_update(ipc::Request& req, ipc::Response&) {
        NetworkInfo ni{};
        if (const uint32_t r = coop_ldn_network_info(n_, &ni)) return r;
        buf_write(req.out_buf(0), &ni, sizeof ni);
        saw(ni);
        NodeLatestUpdate up[8] = {};
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < 8; ++i) up[i].state_change = latest_[i];
            latest_ = {};
        }
        buf_write(req.out_buf(1), up, sizeof up);
        return 0;
    }
    uint32_t scan(ipc::Request& req, ipc::Response& rsp) {
        const int channel = req.arg<int16_t>(0);
        const ScanFilter f = arg_at<ScanFilter>(req, 8);   // after the s16 channel, 8-aligned
        const ipc::Buffer out = req.out_buf(0);
        const int cap = static_cast<int>(std::min<uint64_t>(out.size / sizeof(NetworkInfo), 64));
        std::vector<NetworkInfo> found(static_cast<size_t>(cap > 0 ? cap : 1));
        int count = 0;
        if (const uint32_t r = coop_ldn_scan(n_, found.data(), &count, cap, &f, channel)) return r;
        if (count > 0) buf_write(out, found.data(), sizeof(NetworkInfo) * static_cast<size_t>(count));
        rsp.push(static_cast<uint32_t>(count));   // s16 total (the high half is zero)
        return 0;
    }
    uint32_t accept(ipc::Request&, ipc::Response&) { return 0; }
    uint32_t open_access_point(ipc::Request&, ipc::Response&) { return coop_ldn_open_ap(n_); }
    uint32_t close_access_point(ipc::Request&, ipc::Response&) { return left(coop_ldn_close_ap(n_)); }
    uint32_t create_network(ipc::Request& req, ipc::Response&) {
        const SecurityConfig sec = arg_at<SecurityConfig>(req, 0);
        const UserConfig user = arg_at<UserConfig>(req, kCreateUser);
        const NetworkConfig cfg = arg_at<NetworkConfig>(req, kCreateNetCfg);
        forget();
        return coop_ldn_create_network(n_, &cfg, &sec, &user);
    }
    uint32_t create_network_private(ipc::Request&, ipc::Response&) {
        NXLOG_W("net", "ldn: CreateNetworkPrivate has no co-op counterpart: DeviceNotAvailable");
        return kDeviceNotAvailable;
    }
    uint32_t destroy_network(ipc::Request&, ipc::Response&) { return left(coop_ldn_destroy_network(n_)); }
    uint32_t reject(ipc::Request& req, ipc::Response&) { return coop_ldn_reject(n_, req.arg<uint32_t>(0)); }
    uint32_t set_advertise_data(ipc::Request& req, ipc::Response&) {
        const ipc::Buffer b = req.in_buf(0);
        if (b.size > vnet::kMaxAdvertise) return kInvalidArgument;
        std::vector<uint8_t> adv(buf_ptr(b), buf_ptr(b) + (b.addr ? b.size : 0));
        NXLOG_D("net", "ldn: SetAdvertiseData %zu bytes: %s", adv.size(), hex_head(adv).c_str());
        return coop_ldn_set_advertise(n_, adv.data(), adv.size());
    }
    uint32_t set_accept_policy(ipc::Request& req, ipc::Response&) { return coop_ldn_set_accept_policy(n_, req.arg<uint8_t>(0)); }
    uint32_t accept_filter(ipc::Request&, ipc::Response&) {
        const int st = coop_ldn_state(n_);
        return st == COOP_LS_AP || st == COOP_LS_AP_CREATED ? 0 : kInvalidState;
    }
    uint32_t open_station(ipc::Request&, ipc::Response&) { return coop_ldn_open_station(n_); }
    uint32_t close_station(ipc::Request&, ipc::Response&) { return left(coop_ldn_close_station(n_)); }
    uint32_t connect(ipc::Request& req, ipc::Response&) {
        NetworkInfo ni{};
        buf_read(req.in_buf(0), &ni, sizeof ni);
        const SecurityConfig sec = arg_at<SecurityConfig>(req, 0);
        const UserConfig user = arg_at<UserConfig>(req, kConnectUser);
        forget();
        return coop_ldn_connect(n_, &ni, &sec, &user, req.arg<int32_t>(kConnectVersion), req.arg<int32_t>(kConnectVersion + 4));
    }
    uint32_t connect_private(ipc::Request&, ipc::Response&) {
        NXLOG_W("net", "ldn: ConnectPrivate has no co-op counterpart: ConnectNotFound");
        return kConnectNotFound;
    }
    uint32_t disconnect(ipc::Request&, ipc::Response&) { return left(coop_ldn_disconnect(n_)); }
    uint32_t initialize(ipc::Request&, ipc::Response&) { return coop_ldn_initialize(n_); }
    uint32_t finalize(ipc::Request&, ipc::Response&) {
        if (coop_ldn_state(n_) == COOP_LS_NONE) return kInvalidState;
        shutdown();
        return 0;
    }

    coop_node* n_;
    std::mutex mu_;
    std::array<uint32_t, 8> seen_{};    // node addresses of the last NetworkInfo the game read (0 = empty slot)
    std::array<uint8_t, 8> latest_{};   // NodeLatestUpdate flags since the last GetNetworkInfoLatestUpdate
};

void NativeLdn::cmd(ipc::Service& s, uint32_t id, const char* name, H h) {
    std::weak_ptr<NativeLdn> w = weak_from_this();
    s.add_cmd(id, name, [w, id, name, h](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        note(id, name);
        auto self = w.lock();
        if (!self) return kInvalidState;
        return ((*self).*h)(req, rsp);   // the module is thread-safe: no command lock (Scan and Connect wait inside it)
    });
}

void NativeLdn::add_commands(ipc::Service& s) {
    cmd(s, 0, "GetState", &NativeLdn::get_state);
    cmd(s, 1, "GetNetworkInfo", &NativeLdn::get_network_info);
    cmd(s, 2, "GetIpv4Address", &NativeLdn::get_ipv4);
    cmd(s, 3, "GetDisconnectReason", &NativeLdn::get_disconnect_reason);
    cmd(s, 4, "GetSecurityParameter", &NativeLdn::get_security_parameter);
    cmd(s, 5, "GetNetworkConfig", &NativeLdn::get_network_config);
    cmd(s, 100, "AttachStateChangeEvent", &NativeLdn::attach_event);
    cmd(s, 101, "GetNetworkInfoLatestUpdate", &NativeLdn::get_latest_update);
    cmd(s, 102, "Scan", &NativeLdn::scan);
    cmd(s, 103, "ScanPrivate", &NativeLdn::scan);
    cmd(s, 104, "SetWirelessControllerRestriction", &NativeLdn::accept);
    cmd(s, 105, "SetWirelessAudioPolicy", &NativeLdn::accept);
    cmd(s, 106, "SetProtocol", &NativeLdn::accept);
    cmd(s, 200, "OpenAccessPoint", &NativeLdn::open_access_point);
    cmd(s, 201, "CloseAccessPoint", &NativeLdn::close_access_point);
    cmd(s, 202, "CreateNetwork", &NativeLdn::create_network);
    cmd(s, 203, "CreateNetworkPrivate", &NativeLdn::create_network_private);
    cmd(s, 204, "DestroyNetwork", &NativeLdn::destroy_network);
    cmd(s, 205, "Reject", &NativeLdn::reject);
    cmd(s, 206, "SetAdvertiseData", &NativeLdn::set_advertise_data);
    cmd(s, 207, "SetStationAcceptPolicy", &NativeLdn::set_accept_policy);
    cmd(s, 208, "AddAcceptFilterEntry", &NativeLdn::accept_filter);
    cmd(s, 209, "ClearAcceptFilter", &NativeLdn::accept_filter);
    cmd(s, 300, "OpenStation", &NativeLdn::open_station);
    cmd(s, 301, "CloseStation", &NativeLdn::close_station);
    cmd(s, 302, "Connect", &NativeLdn::connect);
    cmd(s, 303, "ConnectPrivate", &NativeLdn::connect_private);
    cmd(s, 304, "Disconnect", &NativeLdn::disconnect);
    cmd(s, 400, "Initialize", &NativeLdn::initialize);
    cmd(s, 401, "Finalize", &NativeLdn::finalize);
    cmd(s, 402, "InitializeWithVersion", &NativeLdn::initialize);
    cmd(s, 403, "SetOperationMode", &NativeLdn::accept);
}

// =============================================================================================================================================
// [net] protocol = lan: ldn:u over a real LAN in ldn_mitm's format (net/lan_mitm.h), with Ryujinx's ldn_mitm mode and consoles running
// ldn_mitm on the same network. The host owns the NetworkInfo (built here after Ryujinx LdnMitm/LanDiscovery.cs, MIT: BSSID and node
// MACs 02:00 + the LAN address, a random 32-byte SSID and session id, channel 6 unless the game asks for one, link level 3, network type
// 2) and syncs it to its stations; node addresses are the machines' LAN addresses, and the game's sockets run there (mitm::make_lan_link).
// Config: [net] lan_ip = auto | <IPv4> (the interface), lan_peers = <IPv4>[,<IPv4>...] (scanned by unicast too: a network that drops
// broadcasts; two copies on one PC on 127.0.0.1 / 127.0.0.2). Scan waits up to 1 s for the first answer, 33 ms after it (Ryujinx's).
// Station accept policy and filter, node_count_max and Reject: enforced by the host (a refused station's link closes: ConnectRejected).
// CreateNetworkPrivate: the game's SecurityParameter is the network's; ConnectPrivate: the scan's network with that session id. TCP between
// AnySwitch nodes rides vtcp on UDP 11453 (a console or Ryujinx peer gets no TCP from it).
// =============================================================================================================================================
class MitmLdn : public std::enable_shared_from_this<MitmLdn> {
public:
    void add_commands(ipc::Service& s);
    void shutdown() {
        std::unique_ptr<vnet::mitm::Net> net;
        {
            std::lock_guard<std::mutex> lk(mu_);
            net = std::move(net_);
            if (in_network()) vnet::set_current_link(nullptr);
            state_ = kNone;
        }
        net.reset();   // closes its links, joins its threads (their events see no net any more)
    }

private:
    using H = uint32_t (MitmLdn::*)(ipc::Request&, ipc::Response&);
    void cmd(ipc::Service& s, uint32_t id, const char* name, H h) {
        std::weak_ptr<MitmLdn> w = weak_from_this();
        s.add_cmd(id, name, [w, id, name, h](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            note(id, name);
            auto self = w.lock();
            if (!self) return kInvalidState;
            std::lock_guard<std::mutex> op(self->op_mu_);
            return ((*self).*h)(req, rsp);
        });
    }
    bool in_network() const { return state_ == kAccessPointCreated || state_ == kStationConnected; }
    void set_state(uint32_t s) {
        if (state_ != s) NXLOG_I("net", "ldn (lan): state %u -> %u", state_, s);
        state_ = s;
        ev_.signal();
    }
    NetworkInfo info_locked() const {
        NetworkInfo ni{};
        if (net_) {
            const vnet::mitm::Info i = net_->info();
            std::memcpy(&ni, i.data(), sizeof ni);
        }
        return ni;
    }
    void saw(const NetworkInfo& ni) {   // the node joins and leaves since the table the game last read (GetNetworkInfoLatestUpdate)
        for (size_t i = 0; i < 8; ++i) {
            const uint32_t now = ni.ldn.nodes[i].is_connected ? ni.ldn.nodes[i].ipv4 : 0;
            if (now == seen_[i]) continue;
            if (seen_[i]) latest_[i] |= kNodeDisconnect;
            if (now) latest_[i] |= kNodeConnect;
            seen_[i] = now;
        }
    }
    std::array<uint8_t, 6> mac_of(uint32_t ip) const {
        return {0x02, 0x00, static_cast<uint8_t>(ip >> 24), static_cast<uint8_t>(ip >> 16), static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip)};
    }
    bool admits(const vnet::mitm::Node& n) {   // the accept policy on a station's MAC (its link's thread)
        std::lock_guard<std::mutex> lk(mu_);
        std::array<uint8_t, 6> mac;
        std::memcpy(mac.data(), n.data() + vnet::mitm::kNodeMac, 6);
        switch (policy_) {
            case 1: return false;
            case 2: return !filter_.count(mac);
            case 3: return filter_.count(mac) != 0;
            default: return true;
        }
    }
    NodeInfo own_node(const UserConfig& user, uint16_t version) const {
        NodeInfo n{};
        n.ipv4 = ip_;
        const auto mac = mac_of(ip_);
        std::memcpy(n.mac, mac.data(), 6);
        n.is_connected = 1;
        std::memcpy(n.user_name, user.user_name, std::min(sizeof n.user_name, sizeof user.user_name));
        n.user_name[sizeof n.user_name - 1] = 0;
        n.local_comm_version = static_cast<int16_t>(version);
        return n;
    }
    static void random_bytes(uint8_t* p, size_t n) {
        static std::mutex m;
        static std::mt19937_64 rng{std::random_device{}()};
        std::lock_guard<std::mutex> lk(m);
        for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(rng());
    }

    uint32_t initialize(ipc::Request&, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kNone) return 0;
        std::string why;
        uint32_t ip = 0, mask = 0;
        if (!vnet::mitm::pick_interface(config_get_str("net", "lan_ip", "auto"), &ip, &mask, &why)) {
            NXLOG_W("net", "ldn (lan): %s: local wireless off", why.c_str());
            set_state(kInitialized);   // the state machine runs; Scan finds nothing, CreateNetwork answers DeviceNotAvailable
            return 0;
        }
        std::vector<uint32_t> peers;
        const std::string ps = config_get_str("net", "lan_peers", "");
        for (size_t at = 0; at < ps.size();) {
            const size_t end = std::min(ps.find_first_of(",; ", at), ps.size());
            unsigned a, b, c, d;
            if (std::sscanf(ps.substr(at, end - at).c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) peers.push_back((a << 24) | (b << 16) | (c << 8) | d);
            at = end + 1;
        }
        std::weak_ptr<MitmLdn> w = weak_from_this();
        vnet::mitm::Net::Events ev;
        ev.changed = [w] {
            if (auto s = w.lock()) s->ev_.signal();
        };
        ev.lost = [w](bool orderly) {
            if (auto s = w.lock()) s->lost(orderly);
        };
        ev.admit = [w](const vnet::mitm::Node& n) {
            auto s = w.lock();
            return s && s->admits(n);
        };
        net_ = vnet::mitm::Net::start(ip, mask, peers, std::move(ev), &why);
        if (!net_) {
            NXLOG_W("net", "ldn (lan): %s: local wireless off", why.c_str());
        } else {
            ip_ = ip;
            mask_ = mask;
            link_ = vnet::mitm::make_lan_link(ip, mask);
            NXLOG_I("net", "ldn (lan): ldn_mitm LAN play on %s/%d%s", vnet::ip_str(ip).c_str(), __builtin_popcount(mask),
                    peers.empty() ? "" : " (and the [net] lan_peers)");
        }
        reason_ = kReasonNone;
        set_state(kInitialized);
        return 0;
    }
    uint32_t finalize(ipc::Request&, ipc::Response&) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ == kNone) return kInvalidState;
        }
        shutdown();
        return 0;
    }
    void lost(bool orderly) {   // the host's link closed under a connected station
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kStationConnected) return;
        vnet::set_current_link(nullptr);
        reason_ = orderly ? kDestroyedByUser : kSignalLost;
        NXLOG_I("net", "ldn (lan): the host left (reason %d)", reason_);
        seen_ = {};
        set_state(kStation);
    }

    uint32_t get_state(ipc::Request&, ipc::Response& rsp) {
        std::lock_guard<std::mutex> lk(mu_);
        rsp.push(state_);
        return 0;
    }
    uint32_t get_network_info(ipc::Request& req, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_network()) return kInvalidState;
        const NetworkInfo ni = info_locked();
        buf_write(req.out_buf(0), &ni, sizeof ni);
        saw(ni);
        return 0;
    }
    uint32_t get_latest_update(ipc::Request& req, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_network()) return kInvalidState;
        const NetworkInfo ni = info_locked();
        buf_write(req.out_buf(0), &ni, sizeof ni);
        saw(ni);
        NodeLatestUpdate up[8] = {};
        for (size_t i = 0; i < 8; ++i) up[i].state_change = latest_[i];
        latest_ = {};
        buf_write(req.out_buf(1), up, sizeof up);
        return 0;
    }
    uint32_t get_ipv4(ipc::Request&, ipc::Response& rsp) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_network()) return kInvalidState;
        rsp.push(ip_);
        rsp.push(mask_);
        return 0;
    }
    uint32_t get_disconnect_reason(ipc::Request&, ipc::Response& rsp) {
        std::lock_guard<std::mutex> lk(mu_);
        rsp.push(reason_);
        return 0;
    }
    uint32_t get_security_parameter(ipc::Request&, ipc::Response& rsp) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_network()) return kInvalidState;
        const NetworkInfo ni = info_locked();
        SecurityParameter sp{};
        std::memcpy(sp.data, ni.ldn.security_parameter, 16);
        std::memcpy(sp.session_id, ni.id.session_id, 16);
        rsp.push(sp);
        return 0;
    }
    uint32_t get_network_config(ipc::Request&, ipc::Response& rsp) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_network()) return kInvalidState;
        const NetworkInfo ni = info_locked();
        NetworkConfig c{};
        c.intent = ni.id.intent;
        c.channel = static_cast<uint16_t>(ni.common.channel);
        c.node_count_max = ni.ldn.node_count_max;
        c.local_comm_version = static_cast<uint16_t>(ni.ldn.nodes[0].local_comm_version);
        rsp.push(c);
        return 0;
    }
    uint32_t attach_event(ipc::Request&, ipc::Response& rsp) {
        rsp.out_copy.push_back(ev_.new_handle());
        return 0;
    }
    uint32_t accept(ipc::Request&, ipc::Response&) { return 0; }

    // Scan: the hosts that answer on the LAN, filtered as the console filters (the ldn_mitm host does not filter).
    std::vector<NetworkInfo> scan_infos(const ScanFilter& f) {
        vnet::mitm::Net* net;
        {
            std::lock_guard<std::mutex> lk(mu_);
            net = net_.get();
        }
        std::vector<NetworkInfo> out;
        if (!net) return out;
        for (const vnet::mitm::Info& i : net->scan(1000, 33)) {
            NetworkInfo ni;
            std::memcpy(&ni, i.data(), sizeof ni);
            if ((f.flag & kFilterLocalCommunicationId) && ni.id.intent.local_comm_id != f.id.intent.local_comm_id) continue;
            if ((f.flag & kFilterSceneId) && ni.id.intent.scene_id != f.id.intent.scene_id) continue;
            if (!filter_match(f, ni)) continue;
            out.push_back(ni);
        }
        return out;
    }
    uint32_t scan(ipc::Request& req, ipc::Response& rsp) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ == kNone) return kInvalidState;
            if (net_ && (state_ < kAccessPoint || state_ > kStationConnected)) return kInvalidState;
        }
        const ScanFilter f = arg_at<ScanFilter>(req, 8);
        const ipc::Buffer out = req.out_buf(0);
        const size_t cap = static_cast<size_t>(out.size / sizeof(NetworkInfo));
        uint32_t count = 0;
        if (cap)
            for (const NetworkInfo& ni : scan_infos(f)) {
                if (count >= cap) break;
                std::memcpy(buf_mut(out) + count * sizeof(NetworkInfo), &ni, sizeof ni);
                ++count;
            }
        rsp.push(count);
        return 0;
    }

    uint32_t open_access_point(ipc::Request&, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kInitialized) return kInvalidState;
        set_state(kAccessPoint);
        return 0;
    }
    // Closing the network joins the net's threads, which call back into admits() and lost() (they take mu_): never with mu_ held. The
    // commands are serialised by op_mu_, so the state only moves under a station's lost() meanwhile.
    void net_close(bool host) {
        vnet::mitm::Net* net;
        {
            std::lock_guard<std::mutex> lk(mu_);
            net = net_.get();
            vnet::set_current_link(nullptr);
        }
        if (net) {
            if (host) net->host_close();
            else net->disconnect();
        }
    }
    uint32_t close_access_point(ipc::Request&, ipc::Response&) {
        uint32_t st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            st = state_;
        }
        if (st != kAccessPoint && st != kAccessPointCreated) return kInvalidState;
        if (st == kAccessPointCreated) net_close(true);
        std::lock_guard<std::mutex> lk(mu_);
        policy_ = 0;
        filter_.clear();
        set_state(kInitialized);
        return 0;
    }
    uint32_t create_network(ipc::Request& req, ipc::Response&) {
        return create(arg_at<SecurityConfig>(req, 0), arg_at<UserConfig>(req, kCreateUser), arg_at<NetworkConfig>(req, kCreateNetCfg), nullptr);
    }
    uint32_t create_network_private(ipc::Request& req, ipc::Response&) {
        const SecurityParameter sp = arg_at<SecurityParameter>(req, kCreatePrivSecParam);
        return create(arg_at<SecurityConfig>(req, 0), arg_at<UserConfig>(req, kCreatePrivUser), arg_at<NetworkConfig>(req, kCreatePrivNetCfg), &sp);
    }
    uint32_t create(const SecurityConfig& sec, const UserConfig& user, const NetworkConfig& cfg, const SecurityParameter* priv) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint) return kInvalidState;
        if (cfg.node_count_max < 1 || cfg.node_count_max > 8) return kInvalidArgument;
        if (!net_) return kDeviceNotAvailable;
        NetworkInfo ni{};
        ni.id.intent = cfg.intent;
        if (priv) {
            std::memcpy(ni.id.session_id, priv->session_id, 16);
            std::memcpy(ni.ldn.security_parameter, priv->data, 16);
        } else {
            random_bytes(ni.id.session_id, 16);
        }
        const auto mac = mac_of(ip_);
        std::memcpy(ni.common.bssid, mac.data(), 6);
        ni.common.ssid.length = 32;
        random_bytes(reinterpret_cast<uint8_t*>(ni.common.ssid.raw), 32);
        ni.common.channel = cfg.channel ? static_cast<int16_t>(cfg.channel) : 6;
        ni.common.link_level = 3;
        ni.common.network_type = 2;
        ni.ldn.security_mode = static_cast<uint16_t>(sec.mode);
        ni.ldn.station_accept_policy = policy_;
        ni.ldn.node_count_max = cfg.node_count_max;
        ni.ldn.nodes[0] = own_node(user, cfg.local_comm_version);
        ni.ldn.node_count = 1;
        ni.ldn.advertise_data_size = static_cast<uint16_t>(adv_.size());
        if (!adv_.empty()) std::memcpy(ni.ldn.advertise_data, adv_.data(), adv_.size());
        vnet::mitm::Info i;
        std::memcpy(i.data(), &ni, sizeof ni);
        std::string why;
        if (!net_->host(i, &why)) {
            NXLOG_W("net", "ldn (lan): CreateNetwork: %s", why.c_str());
            return kDeviceNotAvailable;
        }
        seen_ = {};
        latest_ = {};
        reason_ = kReasonNone;
        vnet::set_current_link(link_);
        NXLOG_I("net", "ldn (lan): created a network on %s for up to %u players", vnet::ip_str(ip_).c_str(), cfg.node_count_max);
        set_state(kAccessPointCreated);
        return 0;
    }
    uint32_t destroy_network(ipc::Request&, ipc::Response&) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kAccessPointCreated) return kInvalidState;
        }
        net_close(true);
        std::lock_guard<std::mutex> lk(mu_);
        reason_ = kDestroyedByUser;
        seen_ = {};
        set_state(kAccessPoint);
        return 0;
    }
    uint32_t reject(ipc::Request& req, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPointCreated) return kInvalidState;
        return net_->host_reject(req.arg<uint32_t>(0)) ? 0 : kNodeNotFound;
    }
    uint32_t set_advertise_data(ipc::Request& req, ipc::Response&) {
        const ipc::Buffer b = req.in_buf(0);
        if (b.size > vnet::kMaxAdvertise) return kInvalidArgument;
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
        adv_.assign(buf_ptr(b), buf_ptr(b) + (b.addr ? b.size : 0));
        if (state_ == kAccessPointCreated) {
            NetworkInfo ni = info_locked();
            std::memset(ni.ldn.advertise_data, 0, sizeof ni.ldn.advertise_data);
            ni.ldn.advertise_data_size = static_cast<uint16_t>(adv_.size());
            if (!adv_.empty()) std::memcpy(ni.ldn.advertise_data, adv_.data(), adv_.size());
            vnet::mitm::Info i;
            std::memcpy(i.data(), &ni, sizeof ni);
            net_->host_update(i);   // SyncNetwork to every station (Ryujinx re-sends it too)
        }
        return 0;
    }
    uint32_t set_accept_policy(ipc::Request& req, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        const uint8_t p = req.arg<uint8_t>(0);
        if (p > 3) return kInvalidArgument;
        if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
        policy_ = p;
        if (state_ == kAccessPointCreated) {
            NetworkInfo ni = info_locked();
            ni.ldn.station_accept_policy = p;
            vnet::mitm::Info i;
            std::memcpy(i.data(), &ni, sizeof ni);
            net_->host_update(i);
        }
        return 0;
    }
    uint32_t add_accept_filter(ipc::Request& req, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
        std::array<uint8_t, 6> mac{};
        for (size_t i = 0; i < 6; ++i) mac[i] = req.arg<uint8_t>(i);
        filter_.insert(mac);
        return 0;
    }
    uint32_t clear_accept_filter(ipc::Request&, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
        filter_.clear();
        return 0;
    }

    uint32_t open_station(ipc::Request&, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kInitialized) return kInvalidState;
        set_state(kStation);
        return 0;
    }
    uint32_t close_station(ipc::Request&, ipc::Response&) {
        uint32_t st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            st = state_;
        }
        if (st != kStation && st != kStationConnected) return kInvalidState;
        if (st == kStationConnected) net_close(false);
        std::lock_guard<std::mutex> lk(mu_);
        reason_ = kDisconnectedByUser;
        set_state(kInitialized);
        return 0;
    }
    uint32_t connect(ipc::Request& req, ipc::Response&) {
        NetworkInfo ni{};
        buf_read(req.in_buf(0), &ni, sizeof ni);
        return join(ni, arg_at<UserConfig>(req, kConnectUser), req.arg<int32_t>(kConnectVersion));
    }
    uint32_t connect_private(ipc::Request& req, ipc::Response&) {
        const SecurityParameter sp = arg_at<SecurityParameter>(req, kConnPrivSecParam);
        const NetworkConfig cfg = arg_at<NetworkConfig>(req, kConnPrivNetCfg);
        ScanFilter f{};
        std::memcpy(f.id.session_id, sp.session_id, 16);
        f.id.intent = cfg.intent;
        f.flag = kFilterSessionId | kFilterLocalCommunicationId;
        for (const NetworkInfo& ni : scan_infos(f)) return join(ni, arg_at<UserConfig>(req, kConnPrivUser), req.arg<int32_t>(kConnPrivVersion));
        return kConnectNotFound;
    }
    uint32_t join(const NetworkInfo& ni, const UserConfig& user, int32_t version) {
        vnet::mitm::Net* net;
        vnet::mitm::Node self;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kStation) return kInvalidState;
            if (!net_) return kConnectNotFound;
            net = net_.get();
            const NodeInfo n = own_node(user, static_cast<uint16_t>(version));
            std::memcpy(self.data(), &n, sizeof n);
        }
        const uint32_t host = ni.ldn.nodes[0].ipv4;
        const int r = net->connect(host, self, 4000);   // Ryujinx waits 4 s for the host's first SyncNetwork
        if (r) {
            NXLOG_I("net", "ldn (lan): Connect to %s: %s", vnet::ip_str(host).c_str(), r == 1 ? "no answer" : r == 2 ? "turned away" : "no connection");
            return r == 1 ? kConnectTimeout : r == 2 ? kConnectRejected : kConnectFailure;
        }
        std::lock_guard<std::mutex> lk(mu_);
        seen_ = {};
        latest_ = {};
        reason_ = kReasonNone;
        vnet::set_current_link(link_);
        NXLOG_I("net", "ldn (lan): joined %s's network as %s", vnet::ip_str(host).c_str(), vnet::ip_str(ip_).c_str());
        set_state(kStationConnected);
        return 0;
    }
    uint32_t disconnect(ipc::Request&, ipc::Response&) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kStationConnected) return kInvalidState;
        }
        net_close(false);
        std::lock_guard<std::mutex> lk(mu_);
        reason_ = kDisconnectedByUser;
        seen_ = {};
        set_state(kStation);
        return 0;
    }

    std::mutex op_mu_;
    std::mutex mu_;
    ServiceEvent ev_;
    uint32_t state_ = kNone;
    int16_t reason_ = kReasonNone;
    std::unique_ptr<vnet::mitm::Net> net_;
    std::shared_ptr<vnet::Link> link_;
    uint32_t ip_ = 0, mask_ = 0;
    std::vector<uint8_t> adv_;
    uint8_t policy_ = 0;
    std::set<std::array<uint8_t, 6>> filter_;
    std::array<uint32_t, 8> seen_{};
    std::array<uint8_t, 8> latest_{};
};

void MitmLdn::add_commands(ipc::Service& s) {
    cmd(s, 0, "GetState", &MitmLdn::get_state);
    cmd(s, 1, "GetNetworkInfo", &MitmLdn::get_network_info);
    cmd(s, 2, "GetIpv4Address", &MitmLdn::get_ipv4);
    cmd(s, 3, "GetDisconnectReason", &MitmLdn::get_disconnect_reason);
    cmd(s, 4, "GetSecurityParameter", &MitmLdn::get_security_parameter);
    cmd(s, 5, "GetNetworkConfig", &MitmLdn::get_network_config);
    cmd(s, 100, "AttachStateChangeEvent", &MitmLdn::attach_event);
    cmd(s, 101, "GetNetworkInfoLatestUpdate", &MitmLdn::get_latest_update);
    cmd(s, 102, "Scan", &MitmLdn::scan);
    cmd(s, 103, "ScanPrivate", &MitmLdn::scan);
    cmd(s, 104, "SetWirelessControllerRestriction", &MitmLdn::accept);
    cmd(s, 105, "SetWirelessAudioPolicy", &MitmLdn::accept);
    cmd(s, 106, "SetProtocol", &MitmLdn::accept);
    cmd(s, 200, "OpenAccessPoint", &MitmLdn::open_access_point);
    cmd(s, 201, "CloseAccessPoint", &MitmLdn::close_access_point);
    cmd(s, 202, "CreateNetwork", &MitmLdn::create_network);
    cmd(s, 203, "CreateNetworkPrivate", &MitmLdn::create_network_private);
    cmd(s, 204, "DestroyNetwork", &MitmLdn::destroy_network);
    cmd(s, 205, "Reject", &MitmLdn::reject);
    cmd(s, 206, "SetAdvertiseData", &MitmLdn::set_advertise_data);
    cmd(s, 207, "SetStationAcceptPolicy", &MitmLdn::set_accept_policy);
    cmd(s, 208, "AddAcceptFilterEntry", &MitmLdn::add_accept_filter);
    cmd(s, 209, "ClearAcceptFilter", &MitmLdn::clear_accept_filter);
    cmd(s, 300, "OpenStation", &MitmLdn::open_station);
    cmd(s, 301, "CloseStation", &MitmLdn::close_station);
    cmd(s, 302, "Connect", &MitmLdn::connect);
    cmd(s, 303, "ConnectPrivate", &MitmLdn::connect_private);
    cmd(s, 304, "Disconnect", &MitmLdn::disconnect);
    cmd(s, 400, "Initialize", &MitmLdn::initialize);
    cmd(s, 401, "Finalize", &MitmLdn::finalize);
    cmd(s, 402, "InitializeWithVersion", &MitmLdn::initialize);
    cmd(s, 403, "SetOperationMode", &MitmLdn::accept);
}

// [net] protocol picks the backend per service object: native = NativeLdn on the process's coop node (or, with the module off, Ldn with
// no lobby: the offline answers), linkplay = Ldn on the relay lobby, lan = MitmLdn (ldn_mitm on a real LAN), ryuldn = a Ryujinx LDN
// master server (services/ldn_ryu.h).
class LdnService final : public ipc::Service {
public:
    LdnService() : ipc::Service("nn::ldn::detail::IUserLocalCommunicationService") {
        const std::string proto = lower(config_get_str("net", "protocol", "native"));
        if (proto == "lan") {
            mitm_ = std::make_shared<MitmLdn>();
            mitm_->add_commands(*this);
            return;
        }
        if (proto == "ryuldn") {   // a Ryujinx LDN master server (services/ldn_ryu.h)
            ryu_ = ldn_ryu_backend(*this);
            return;
        }
        coop_node* n = native_protocol() ? native_node() : nullptr;
        if (n) {
            native_ = std::make_shared<NativeLdn>(n);
            native_->add_commands(*this);
        } else {
            ldn_ = std::make_shared<Ldn>(native_protocol());
            ldn_->add_commands(*this);
        }
    }
    void on_close() override {
        if (native_) native_->shutdown();
        if (ldn_) ldn_->shutdown();
        if (mitm_) mitm_->shutdown();
        if (ryu_) ryu_->shutdown();
    }

private:
    std::shared_ptr<Ldn> ldn_;
    std::shared_ptr<NativeLdn> native_;
    std::shared_ptr<MitmLdn> mitm_;
    std::shared_ptr<LdnRyuBackend> ryu_;
};

}  // namespace

std::shared_ptr<ipc::Service> make_ldn_user_creator() {
    return make_iface("nn::ldn::detail::IUserServiceCreator", [](ipc::Service& s) {
        s.add_cmd(0, "CreateUserLocalCommunicationService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            note(1000, "IUserServiceCreator CreateUserLocalCommunicationService");
            rsp.push_object(std::make_shared<LdnService>());
            return 0;
        });
        s.add_cmd(1, "CreateClientProcessMonitor", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            note(1001, "IUserServiceCreator CreateClientProcessMonitor");
            rsp.push_object(make_iface("nn::ldn::detail::IClientProcessMonitor", [](ipc::Service& m) {
                m.add_cmd(0, "RegisterClient", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
            }));
            return 0;
        });
    });
}

coop_node* ldn_native_node() { return native().node.load(); }

void ldn_set_lobby(std::shared_ptr<vnet::Lobby> lobby) {
    LobbySlot& s = slot();
    std::lock_guard<std::mutex> lk(s.mu);
    s.built = true;
    s.auto_mode = false;
    s.target.clear();
    s.lobby = std::move(lobby);
}

void ldn_reset() {
    {
        std::lock_guard<std::mutex> lk(g_seen_mu);
        g_seen.clear();
    }
    {
        Discovery& d = discovery();
        std::lock_guard<std::mutex> lk(d.mu);
        d.ran = false;
        d.found.clear();
        d.logged.clear();
    }
    std::shared_ptr<vnet::Lobby> old;
    {
        LobbySlot& s = slot();
        std::lock_guard<std::mutex> lk(s.mu);
        old.swap(s.lobby);
        s.built = false;
        s.auto_mode = false;
        s.target.clear();
        s.relays.clear();
    }
    ldnapi::set_session_variant(0, "");
    ldnapi::set_story_mod(false);
    {
        NativeSlot& n = native();
        coop_node* node = n.node.exchange(nullptr);
        {
            std::lock_guard<std::mutex> lk(n.mu);
            n.built = false;
            n.build.clear();
        }
        if (node) coop_destroy(node);   // finalizes: the host's links and the virtual sockets close
    }
    // old goes away here (a relay client closes its connection)
}

namespace ldnapi {
void set_session_variant(uint8_t v, const char* what) {
    v &= kVariantMask >> kVariantShift;
    Variant& s = variant();
    std::lock_guard<std::mutex> lk(s.mu);
    s.value = v;
    s.what = what ? what : "";
}
uint8_t session_variant() { return own_variant(); }
void set_story_mod(bool on) { story_flag().store(on); }
bool story_mod() { return story_flag().load(); }
bool native_protocol() { return services::native_protocol(); }
}  // namespace ldnapi

}  // namespace nxrt::services
