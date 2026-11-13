// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// ldn:u over RyuLDN (ldn_ryu.h, net/ryuldn.h). The command table and state machine are services/ldn.cpp's MitmLdn; what the master
// needs follows Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors), src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator:
// IUserLocalCommunicationService.cs, AccessPoint.cs, Station.cs (our own code):
//   - a LocalCommunicationId of -1 (the title's own) becomes the first entry of the title's control.nacp LocalCommunicationId list in
//     Scan, CreateNetwork and Connect (:436-443, :613-619, :948-954): the master matches rooms by it. The list is at 0x30B0 of the NACP
//     (Ryujinx.Horizon/Sdk/Ns/ApplicationControlProperty.cs: after ApplicationErrorCodeCategory at 0x30A8, before Reserved30F4). With no
//     [fs] nacp (a NUP build): the application id, logged.
//   - CreateNetwork sends channel 0 and security mode Retail, Connect security mode Retail (CheckDevelopmentChannel /
//     CheckDevelopmentSecurityMode, :50-58, :634-635, :969); the room's game version is the title's display version (:1211).
//   - Connect refuses option > 1, a version of 0x8000 or more and a passphrase over 64 bytes (InvalidArgument, :973-975); the master's
//     errors become ldn results (Station.cs:64-80); no master: DeviceNotAvailable.
//   - a room that ends under the node puts a host back to AccessPoint (DisconnectReason: a Reject's reason, else DestroyedBySystem,
//     AccessPoint.cs:53) and a station back to Station (else DestroyedByUser, Station.cs:44); a lost connection is SignalLost. Ryujinx
//     goes to Initialized; ldn.cpp's backends keep the access point / station open, as this one does.
//   - Reject waits for the master's RejectReply (6 s; none or an error: InvalidState, LdnMasterProxyClient.cs:319-336); a node id that is
//     not in the table: NodeNotFound.
//   - a station accept policy set before CreateNetwork is sent once the room is up (Ryujinx's client drops it: it sends only in a room,
//     LdnMasterProxyClient.cs:357-367). The accept filter (AddAcceptFilterEntry / ClearAcceptFilter) is kept but not enforced: the
//     protocol has no message for it (a TODO in Ryujinx too, IUserLocalCommunicationService.cs:811-839).
//   - CreateNetworkPrivate sends the AddressList the game passes (X buffer, :646-652); ConnectPrivate is sent as the protocol defines it,
//     but no Ryujinx client sends one (its check of the empty NetworkInfo's id refuses it first, :929-959): untested against Ryujinx.
// While in a room, the client's link is the current vnet link (the bsd sockets run on the ProxyConfig network); leaving clears it only
// when nothing replaced it.
// Layouts of the nn::ldn structs: services/ldn.cpp (Switchbrew "LDN services", Ryujinx Ldn/Types); the raw IPC argument offsets are
// MitmLdn's.
#include "ldn_ryu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "../net/ryuldn.h"
#include "../net/vnet.h"

namespace nxrt::services {
namespace ryu = vnet::ryuldn;

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

enum State : uint32_t { kNone = 0, kInitialized = 1, kAccessPoint = 2, kAccessPointCreated = 3, kStation = 4, kStationConnected = 5 };
enum Reason : int16_t { kReasonNone = 0, kDisconnectedByUser = 1, kDestroyedByUser = 3, kDestroyedBySystem = 4, kRejected = 5 };
enum : uint8_t { kNodeConnect = 1, kNodeDisconnect = 2 };   // NodeLatestUpdate.StateChange
enum : uint32_t {                                            // ScanFilterFlag
    kFilterLocalCommunicationId = 1 << 0, kFilterSessionId = 1 << 1, kFilterNetworkType = 1 << 2, kFilterBssid = 1 << 3,
    kFilterSsid = 1 << 4, kFilterSceneId = 1 << 5,
};

// ---- layouts (services/ldn.cpp; Switchbrew "LDN services", Ryujinx Ldn/Types) ---------------------------------------------------------
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
    uint8_t reserved1;
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
static_assert(sizeof(NetworkInfo) == 0x480 && offsetof(NetworkInfo, ldn) == 0x50 && sizeof(NetworkInfo) == ryu::kInfoSize);
static_assert(sizeof(NodeLatestUpdate) == 8);
static_assert(sizeof(ScanFilter) == 0x60 && offsetof(ScanFilter, ssid) == 0x2A && offsetof(ScanFilter, flag) == 0x5C &&
              sizeof(ScanFilter) == ryu::kScanFilterSize);
static_assert(sizeof(NetworkConfig) == 0x20 && offsetof(NetworkConfig, local_comm_version) == 0x14);
static_assert(sizeof(SecurityConfig) == 0x44 && sizeof(SecurityParameter) == 0x20 && sizeof(UserConfig) == 0x30);
constexpr size_t kAddressList = 0x60;   // Ldn/Types/AddressList.cs: 8 AddressEntry of 0xC

// Raw IPC argument offsets (CMIF, natural alignment: NetworkConfig is 8-aligned; MitmLdn's).
constexpr size_t kCreateUser = 0x44, kCreateNetCfg = 0x78;
constexpr size_t kCreatePrivSecParam = 0x44, kCreatePrivUser = 0x64, kCreatePrivNetCfg = 0x98;
constexpr size_t kConnectUser = 0x44, kConnectVersion = 0x74, kConnectOption = 0x78;
constexpr size_t kConnPrivSecParam = 0x44, kConnPrivUser = 0x64, kConnPrivVersion = 0x94, kConnPrivOption = 0x98, kConnPrivNetCfg = 0xA0;
constexpr uint64_t kOwnLcid = ~0ull;    // LocalCommunicationId -1: the title's own
constexpr uint16_t kSecurityRetail = 1;
constexpr size_t kMaxAdvertise = 384;
constexpr size_t kNacpLcid = 0x30B0;    // ApplicationControlProperty.LocalCommunicationId[0]

void note(uint32_t id, const char* name) {   // first use of each command, once per process
    static std::mutex mu;
    static std::set<uint32_t> seen;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!seen.insert(id).second) return;
    }
    NXLOG_I("net", "ldn (ryuldn) cmd %u %s (first use)", id, name);
}

// The title's own local communication id: control.nacp's LocalCommunicationId[0] ([fs] nacp), else the application id (PokoMac: the
// title profile's program id, which pkp_main.cpp sets for Pokopia).
uint64_t own_lcid() {
    const std::string path = config_get_str("fs", "nacp", "");
    if (!path.empty())
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            uint64_t v = 0;
            const bool ok = std::fseek(f, static_cast<long>(kNacpLcid), SEEK_SET) == 0 && std::fread(&v, 8, 1, f) == 1;
            std::fclose(f);
            if (ok && v) return v;
        }
    static std::atomic<bool> logged{false};
    const uint64_t app = profile().program_id;
    if (!logged.exchange(true))
        NXLOG_W("net", "ldn (ryuldn): no control.nacp LocalCommunicationId ([fs] nacp): the title's own id is taken as its application id %016llx",
                static_cast<unsigned long long>(app));
    return app;
}

bool filter_match(const ScanFilter& f, const NetworkInfo& ni) {   // services/ldn.cpp's, plus the ids the master filtered already
    if ((f.flag & kFilterLocalCommunicationId) && ni.id.intent.local_comm_id != f.id.intent.local_comm_id) return false;
    if ((f.flag & kFilterSceneId) && ni.id.intent.scene_id != f.id.intent.scene_id) return false;
    if ((f.flag & kFilterSessionId) && std::memcmp(f.id.session_id, ni.id.session_id, 16) != 0) return false;
    if ((f.flag & kFilterNetworkType) && !(f.network_type & ni.common.network_type)) return false;
    if ((f.flag & kFilterBssid) && std::memcmp(f.bssid, ni.common.bssid, 6) != 0) return false;
    if ((f.flag & kFilterSsid) &&
        (f.ssid.length != ni.common.ssid.length || std::memcmp(f.ssid.raw, ni.common.ssid.raw, std::min<size_t>(ni.common.ssid.length, 33)) != 0))
        return false;
    return true;
}

uint32_t result_of(ryu::NetError e) {   // Station.cs:64-80
    switch (e) {
        case ryu::NetError::None: return 0;
        case ryu::NetError::VersionTooLow: return kVersionTooLow;
        case ryu::NetError::VersionTooHigh: return kVersionTooHigh;
        case ryu::NetError::TooManyPlayers: return kTooManyPlayers;
        case ryu::NetError::ConnectFailure: return kConnectFailure;
        case ryu::NetError::ConnectNotFound: return kConnectNotFound;
        case ryu::NetError::ConnectTimeout: return kConnectTimeout;
        case ryu::NetError::ConnectRejected: return kConnectRejected;
        default: return kDeviceNotAvailable;
    }
}

template <class T>
void put(std::vector<uint8_t>& v, size_t off, const T& x) {
    std::memcpy(v.data() + off, &x, sizeof x);
}

// =============================================================================================================================================
class RyuLdn final : public LdnRyuBackend, public std::enable_shared_from_this<RyuLdn> {
public:
    ~RyuLdn() override { shutdown(); }
    void add_commands(ipc::Service& s);
    void shutdown() override {
        std::shared_ptr<ryu::Client> c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            c = std::move(client_);
            if (in_network()) drop_link();
            state_ = kNone;
        }
        if (c) c->close();   // leaves the room, joins its threads (their events find no client any more)
    }

private:
    using H = uint32_t (RyuLdn::*)(ipc::Request&, ipc::Response&);
    void cmd(ipc::Service& s, uint32_t id, const char* name, H h) {
        std::weak_ptr<RyuLdn> w = weak_from_this();
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
        if (state_ != s) NXLOG_I("net", "ldn (ryuldn): state %u -> %u", state_, s);
        state_ = s;
        ev_.signal();
    }
    // The client's link stops being the current one, unless another one replaced it meanwhile (two ldn objects in one process).
    void drop_link() {
        if (link_ && vnet::current_link() == link_) vnet::set_current_link(nullptr);
    }
    NetworkInfo info_locked() const {
        NetworkInfo ni{};
        if (client_) {
            const ryu::Info i = client_->info();
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
    // The client's reader thread: the room changed / ended.
    void changed() {
        std::lock_guard<std::mutex> lk(mu_);
        ev_.signal();
    }
    void left(uint32_t reason) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ == kAccessPointCreated) {
            reason_ = static_cast<int16_t>(reason ? reason : kDestroyedBySystem);
            drop_link();
            seen_ = {};
            set_state(kAccessPoint);
        } else if (state_ == kStationConnected) {
            reason_ = static_cast<int16_t>(reason ? reason : kDestroyedByUser);
            drop_link();
            seen_ = {};
            set_state(kStation);
        } else {
            return;
        }
        NXLOG_I("net", "ldn (ryuldn): the room ended (reason %d)", reason_);
    }

    uint32_t initialize(ipc::Request&, ipc::Response&) {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != kNone) return 0;
        const std::string server = config_get_str("net", "ryuldn_server", "");
        const std::string pass = config_get_str("net", "ryuldn_passphrase", "");
        std::string host;
        uint16_t port = 0;
        if (!ryu::parse_server(server, &host, &port)) {
            NXLOG_W("net", "ldn (ryuldn): [net] ryuldn_server %s: local wireless off",
                    server.empty() ? "is not set (host[:port] of a Ryujinx LDN server)" : "is not host[:port]");
        } else if (!ryu::valid_passphrase(pass)) {
            NXLOG_W("net", "ldn (ryuldn): [net] ryuldn_passphrase is not empty or Ryujinx-<8 lowercase hex digits>: local wireless off");
        } else {
            std::weak_ptr<RyuLdn> w = weak_from_this();
            ryu::Client::Events ev;
            ev.changed = [w] {
                if (auto s = w.lock()) s->changed();
            };
            ev.left = [w](uint32_t reason) {
                if (auto s = w.lock()) s->left(reason);
            };
            client_ = ryu::Client::make(host, port, pass, profile().display_version, std::move(ev));
            link_ = client_->link();
            NXLOG_I("net", "ldn (ryuldn): local wireless through the RyuLDN server %s:%u (%s rooms)", host.c_str(), port,
                    pass.empty() ? "public" : "private");
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
    // The ProxyConfig address and mask (IUserLocalCommunicationService.cs:213-218).
    uint32_t get_ipv4(ipc::Request&, ipc::Response& rsp) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_network() || !client_) return kInvalidState;
        const ryu::ProxyConfig c = client_->proxy();
        rsp.push(c.proxy_ip);
        rsp.push(c.proxy_subnet_mask);
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

    uint32_t scan(ipc::Request& req, ipc::Response& rsp) {
        std::shared_ptr<ryu::Client> c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ == kNone) return kInvalidState;
            if (client_ && (state_ < kAccessPoint || state_ > kStationConnected)) return kInvalidState;
            c = client_;
        }
        ScanFilter f = req.arg<ScanFilter>(8);   // after the channel (8 bytes)
        const ipc::Buffer out = req.out_buf(0);
        const size_t cap = static_cast<size_t>(out.size / sizeof(NetworkInfo));
        uint32_t count = 0;
        if (c && cap) {
            if (f.id.intent.local_comm_id == kOwnLcid) f.id.intent.local_comm_id = own_lcid();
            uint8_t raw[ryu::kScanFilterSize];
            std::memcpy(raw, &f, sizeof raw);
            for (const ryu::Info& i : c->scan(raw)) {
                NetworkInfo ni;
                std::memcpy(&ni, i.data(), sizeof ni);
                if (!filter_match(f, ni)) continue;
                if (count >= cap) break;
                std::memcpy(buf_mut(out) + count * sizeof(NetworkInfo), &ni, sizeof ni);
                ++count;
            }
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
    // Leaving a room: the client's calls never with mu_ held (its reader calls left(), which takes mu_).
    void leave_room() {
        std::shared_ptr<ryu::Client> c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            c = client_;
            drop_link();
        }
        if (c) c->leave();
    }
    uint32_t close_access_point(ipc::Request&, ipc::Response&) {
        uint32_t st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            st = state_;
        }
        if (st != kAccessPoint && st != kAccessPointCreated) return kInvalidState;
        if (st == kAccessPointCreated) leave_room();
        std::lock_guard<std::mutex> lk(mu_);
        policy_ = 0;
        filter_.clear();
        set_state(kInitialized);
        return 0;
    }
    uint32_t create_network(ipc::Request& req, ipc::Response&) {
        return create(req.arg<SecurityConfig>(0), req.arg<UserConfig>(kCreateUser), req.arg<NetworkConfig>(kCreateNetCfg), nullptr, nullptr);
    }
    uint32_t create_network_private(ipc::Request& req, ipc::Response&) {
        const SecurityParameter sp = req.arg<SecurityParameter>(kCreatePrivSecParam);
        uint8_t addr[kAddressList] = {};
        buf_read(req.in_buf(0), addr, sizeof addr);
        return create(req.arg<SecurityConfig>(0), req.arg<UserConfig>(kCreatePrivUser), req.arg<NetworkConfig>(kCreatePrivNetCfg), &sp, addr);
    }
    uint32_t create(SecurityConfig sec, const UserConfig& user, NetworkConfig cfg, const SecurityParameter* priv, const uint8_t* addr) {
        std::shared_ptr<ryu::Client> c;
        std::vector<uint8_t> adv;
        uint8_t policy;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kAccessPoint) return kInvalidState;
            if (cfg.node_count_max < 1 || cfg.node_count_max > 8) return kInvalidArgument;
            if (!client_) return kDeviceNotAvailable;
            c = client_;
            adv = adv_;
            policy = policy_;
        }
        if (cfg.intent.local_comm_id == kOwnLcid) cfg.intent.local_comm_id = own_lcid();
        cfg.channel = 0;
        sec.mode = kSecurityRetail;
        std::vector<uint8_t> r;
        if (priv) {
            namespace o = ryu::create_ap_private;
            r.assign(o::kSize, 0);
            put(r, o::kSecurity, sec);
            put(r, o::kSecParam, *priv);
            put(r, o::kUser, user);
            put(r, o::kNetConfig, cfg);
            std::memcpy(r.data() + o::kAddressList, addr, kAddressList);
        } else {
            namespace o = ryu::create_ap;
            r.assign(o::kSize, 0);
            put(r, o::kSecurity, sec);
            put(r, o::kUser, user);
            put(r, o::kNetConfig, cfg);
        }
        const ryu::NetError e = c->create(priv != nullptr, std::move(r), adv);
        if (e != ryu::NetError::None) {
            NXLOG_W("net", "ldn (ryuldn): CreateNetwork%s failed (%d)", priv ? "Private" : "", static_cast<int>(e));
            return e == ryu::NetError::Unknown ? kDeviceNotAvailable : kInvalidState;   // AccessPoint.cs:87-89
        }
        if (policy) c->set_accept_policy(policy);
        std::lock_guard<std::mutex> lk(mu_);
        if (!c->in_network()) return kInvalidState;   // it ended at once
        seen_ = {};
        latest_ = {};
        reason_ = kReasonNone;
        vnet::set_current_link(link_);
        const ryu::ProxyConfig pc = c->proxy();
        NXLOG_I("net", "ldn (ryuldn): created a room for up to %u players as %s", cfg.node_count_max, vnet::ip_str(pc.proxy_ip).c_str());
        set_state(kAccessPointCreated);
        return 0;
    }
    uint32_t destroy_network(ipc::Request&, ipc::Response&) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kAccessPointCreated) return kInvalidState;
        }
        leave_room();
        std::lock_guard<std::mutex> lk(mu_);
        reason_ = kDestroyedByUser;
        seen_ = {};
        set_state(kAccessPoint);
        return 0;
    }
    uint32_t reject(ipc::Request& req, ipc::Response&) {
        const uint32_t node = req.arg<uint32_t>(0);
        std::shared_ptr<ryu::Client> c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kAccessPointCreated || !client_) return kInvalidState;
            const NetworkInfo ni = info_locked();
            if (node == 0 || node >= 8 || !ni.ldn.nodes[node].is_connected) return kNodeNotFound;
            c = client_;
        }
        return c->reject(node, kRejected) ? 0 : kInvalidState;
    }
    uint32_t set_advertise_data(ipc::Request& req, ipc::Response&) {
        const ipc::Buffer b = req.in_buf(0);
        if (b.size > kMaxAdvertise) return kInvalidArgument;
        std::shared_ptr<ryu::Client> c;
        std::vector<uint8_t> adv;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
            adv_.assign(buf_ptr(b), buf_ptr(b) + (b.addr ? b.size : 0));
            if (state_ != kAccessPointCreated) return 0;
            c = client_;
            adv = adv_;
        }
        if (c) c->set_advertise_data(adv);   // the master syncs the room to its members
        return 0;
    }
    uint32_t set_accept_policy(ipc::Request& req, ipc::Response&) {
        const uint8_t p = req.arg<uint8_t>(0);
        if (p > 3) return kInvalidArgument;
        std::shared_ptr<ryu::Client> c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kAccessPoint && state_ != kAccessPointCreated) return kInvalidState;
            policy_ = p;
            if (state_ != kAccessPointCreated) return 0;
            c = client_;
        }
        if (c) c->set_accept_policy(p);
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
        if (st == kStationConnected) leave_room();
        std::lock_guard<std::mutex> lk(mu_);
        reason_ = kDisconnectedByUser;
        set_state(kInitialized);
        return 0;
    }
    static bool connect_args_ok(const SecurityConfig& sec, uint32_t version, uint32_t option) {   // :973-975
        return option <= 1 && (version >> 15) == 0 && sec.passphrase_size <= 64;
    }
    uint32_t connect(ipc::Request& req, ipc::Response&) {
        SecurityConfig sec = req.arg<SecurityConfig>(0);
        const uint32_t version = req.arg<uint32_t>(kConnectVersion), option = req.arg<uint32_t>(kConnectOption);
        if (!connect_args_ok(sec, version, option)) return kInvalidArgument;
        NetworkInfo ni{};
        buf_read(req.in_buf(0), &ni, sizeof ni);
        if (ni.id.intent.local_comm_id == kOwnLcid) ni.id.intent.local_comm_id = own_lcid();
        sec.mode = kSecurityRetail;
        namespace o = ryu::connect_req;
        std::vector<uint8_t> r(o::kSize, 0);
        put(r, o::kSecurity, sec);
        put(r, o::kUser, req.arg<UserConfig>(kConnectUser));
        put(r, o::kVersion, version);
        put(r, o::kOption, option);
        put(r, o::kNetworkInfo, ni);
        return join(false, r);
    }
    uint32_t connect_private(ipc::Request& req, ipc::Response&) {
        SecurityConfig sec = req.arg<SecurityConfig>(0);
        const uint32_t version = req.arg<uint32_t>(kConnPrivVersion), option = req.arg<uint32_t>(kConnPrivOption);
        if (!connect_args_ok(sec, version, option)) return kInvalidArgument;
        NetworkConfig cfg = req.arg<NetworkConfig>(kConnPrivNetCfg);
        if (cfg.intent.local_comm_id == kOwnLcid) cfg.intent.local_comm_id = own_lcid();
        sec.mode = kSecurityRetail;
        namespace o = ryu::connect_private;
        std::vector<uint8_t> r(o::kSize, 0);
        put(r, o::kSecurity, sec);
        put(r, o::kSecParam, req.arg<SecurityParameter>(kConnPrivSecParam));
        put(r, o::kUser, req.arg<UserConfig>(kConnPrivUser));
        put(r, o::kVersion, version);
        put(r, o::kOption, option);
        put(r, o::kNetConfig, cfg);
        return join(true, r);
    }
    uint32_t join(bool priv, const std::vector<uint8_t>& r) {
        std::shared_ptr<ryu::Client> c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kStation) return kInvalidState;
            if (!client_) return kConnectNotFound;
            c = client_;
        }
        const ryu::NetError e = c->connect(priv, r);
        if (e != ryu::NetError::None) {
            NXLOG_I("net", "ldn (ryuldn): Connect%s: the server answered %d", priv ? "Private" : "", static_cast<int>(e));
            return result_of(e);
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (!c->in_network()) return kConnectFailure;   // it ended at once
        seen_ = {};
        latest_ = {};
        reason_ = kReasonNone;
        vnet::set_current_link(link_);
        const ryu::ProxyConfig pc = c->proxy();
        NXLOG_I("net", "ldn (ryuldn): joined a room as %s", vnet::ip_str(pc.proxy_ip).c_str());
        set_state(kStationConnected);
        return 0;
    }
    uint32_t disconnect(ipc::Request&, ipc::Response&) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ != kStationConnected) return kInvalidState;
        }
        leave_room();
        std::lock_guard<std::mutex> lk(mu_);
        reason_ = kDisconnectedByUser;
        seen_ = {};
        set_state(kStation);
        return 0;
    }

    std::mutex op_mu_;   // serialises the commands
    std::mutex mu_;      // the fields
    ServiceEvent ev_;
    uint32_t state_ = kNone;
    int16_t reason_ = kReasonNone;
    std::shared_ptr<ryu::Client> client_;
    std::shared_ptr<vnet::Link> link_;
    std::vector<uint8_t> adv_;
    uint8_t policy_ = 0;
    std::set<std::array<uint8_t, 6>> filter_;
    std::array<uint32_t, 8> seen_{};
    std::array<uint8_t, 8> latest_{};
};

void RyuLdn::add_commands(ipc::Service& s) {
    cmd(s, 0, "GetState", &RyuLdn::get_state);
    cmd(s, 1, "GetNetworkInfo", &RyuLdn::get_network_info);
    cmd(s, 2, "GetIpv4Address", &RyuLdn::get_ipv4);
    cmd(s, 3, "GetDisconnectReason", &RyuLdn::get_disconnect_reason);
    cmd(s, 4, "GetSecurityParameter", &RyuLdn::get_security_parameter);
    cmd(s, 5, "GetNetworkConfig", &RyuLdn::get_network_config);
    cmd(s, 100, "AttachStateChangeEvent", &RyuLdn::attach_event);
    cmd(s, 101, "GetNetworkInfoLatestUpdate", &RyuLdn::get_latest_update);
    cmd(s, 102, "Scan", &RyuLdn::scan);
    cmd(s, 103, "ScanPrivate", &RyuLdn::scan);
    cmd(s, 104, "SetWirelessControllerRestriction", &RyuLdn::accept);
    cmd(s, 105, "SetWirelessAudioPolicy", &RyuLdn::accept);
    cmd(s, 106, "SetProtocol", &RyuLdn::accept);
    cmd(s, 200, "OpenAccessPoint", &RyuLdn::open_access_point);
    cmd(s, 201, "CloseAccessPoint", &RyuLdn::close_access_point);
    cmd(s, 202, "CreateNetwork", &RyuLdn::create_network);
    cmd(s, 203, "CreateNetworkPrivate", &RyuLdn::create_network_private);
    cmd(s, 204, "DestroyNetwork", &RyuLdn::destroy_network);
    cmd(s, 205, "Reject", &RyuLdn::reject);
    cmd(s, 206, "SetAdvertiseData", &RyuLdn::set_advertise_data);
    cmd(s, 207, "SetStationAcceptPolicy", &RyuLdn::set_accept_policy);
    cmd(s, 208, "AddAcceptFilterEntry", &RyuLdn::add_accept_filter);
    cmd(s, 209, "ClearAcceptFilter", &RyuLdn::clear_accept_filter);
    cmd(s, 300, "OpenStation", &RyuLdn::open_station);
    cmd(s, 301, "CloseStation", &RyuLdn::close_station);
    cmd(s, 302, "Connect", &RyuLdn::connect);
    cmd(s, 303, "ConnectPrivate", &RyuLdn::connect_private);
    cmd(s, 304, "Disconnect", &RyuLdn::disconnect);
    cmd(s, 400, "Initialize", &RyuLdn::initialize);
    cmd(s, 401, "Finalize", &RyuLdn::finalize);
    cmd(s, 402, "InitializeWithVersion", &RyuLdn::initialize);
    cmd(s, 403, "SetOperationMode", &RyuLdn::accept);
}

}  // namespace

std::shared_ptr<LdnRyuBackend> ldn_ryu_backend(ipc::Service& s) {
    auto b = std::make_shared<RyuLdn>();
    b->add_commands(s);
    return b;
}

}  // namespace nxrt::services
