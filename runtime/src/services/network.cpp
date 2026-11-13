// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Offline network services (S3c): nifm, bsd, bsdcfg, ssl, nim:eca, friend, prepo, pctl, bcat, ldn, nsd, sfdnsres, ntc, ngct, ngc, notif.
// The console has no network connection, no friends and no BCAT data. Every command answers what a console in that state gives. The one
// exceptions are ldn (local wireless over the relay server: services/ldn.cpp registers ldn:u's objects, this file the name) and bsd's UDP
// sockets, which run on the virtual local-wireless LAN (net/vsock.h) when ldn has a vnet link up, or on pokopia-native's co-op module
// ([net] protocol = native: descriptors 0x100..0x11F, coop_vnet_*).
// Adapted from ScarletNX (GPL-3.0-or-later): src/nx/hle/services/network/nifm.swipc, bsd.swipc, ssl.swipc, dns.swipc, ldn.swipc and
//   src/nx/hle/services/misc.swipc (pctl, friend, prepo, bcat, ngct, nsd). Command ids, names and layouts.
// Adapted from Ryujinx (MIT): src/Ryujinx.HLE/HOS/Services/Nifm/*, Sockets/Bsd/IClient.cs, Ssl/ResultCode.cs, Nim/Ntc/*,
//   Notification/*, Ldn/UserServiceCreator/IUserLocalCommunicationService.cs, Pctl/*, Ngct/*, Time/ResultCode.cs. The offline answers
//   and the result codes.
// Switchbrew pages followed for the command tables: Network_Interface_services, Sockets_services, SSL_services, Friend_services,
//   Parental_Control_services. The result codes are module | (description << 9) (services.h).
//
// Each command that is answered logs once at Info with tag "net" (the first use per command). Process-wide state (client ids, the ssl
// context count, the pki ids) lives in st(); network_reset() puts it back to the start-up value (and closes every socket, drops the
// current vnet link and the ldn relay connection). Per-datagram traffic is logged at Debug with tag "net" (vsock.cpp).
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "../net/vnet.h"
#include "../net/vsock.h"
#include "bsd_mmsg.h"
#include "coop.h"
#include "lan_room.h"
#include "ldn.h"
#include "services_internal.h"

namespace nxrt::services {
namespace {

// ---- result codes ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kNifmNoConnection = res(110, 300);         // nifm NoInternetConnection (Ryujinx Nifm/ResultCode.cs)
constexpr uint32_t kTimeNetworkNotAvailable = res(116, 1000); // time NetworkTimeNotAvailable (Ryujinx Time/ResultCode.cs)
constexpr uint32_t kTimeTaskCanceled = res(116, 1003);        // time NetworkTimeTaskCanceled
constexpr uint32_t kSslInvalidSocket = res(123, 106);         // ssl InvalidSocket (Ryujinx Ssl/ResultCode.cs)
constexpr uint32_t kSslInvalidOption = res(123, 126);         // ssl InvalidOption
constexpr uint32_t kFsPathNotFound = res(2, 1002);            // fs PathNotFound 2002: the bcat delivery cache is empty
constexpr uint32_t kBsdNetDown = 100;                         // ENETDOWN (Linux errno numbers, as Switchbrew's bsd page says)
constexpr uint32_t kNetdbHostNotFound = 1;                    // HOST_NOT_FOUND
constexpr int32_t kEaiNoName = 8;                             // EAI_NONAME: the console's resolver numbers them as FreeBSD does

// ---- process state -----------------------------------------------------------------------------------------------------------------------
struct NetState {
    std::mutex mu;
    std::set<std::string> seen;          // "<service> <cmd>" already logged
    std::map<std::string, int> calls;    // "<service> <cmd> <name>" -> calls logged so far (at most kMaxCallLogs)
    uint32_t next_client_id = 1;         // nifm ClientId, one per IGeneralService
    uint32_t next_cancel_handle = 1;     // sfdnsres cancel handle
    uint64_t next_pki_id = 1;            // ssl Import*/Register* ids
    int ssl_contexts = 0;                // live ISslContext objects
};
NetState& st() {
    static NetState s;
    return s;
}

// Logs the first use of a command. One line per (service, command) for the life of the process (network_reset() clears it).
void note(const char* svc, uint32_t id, const char* name) {
    NetState& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.seen.insert(std::string(svc) + " " + std::to_string(id)).second) return;
    }
    NXLOG_I("net", "%s cmd %u %s: answered offline (first use)", svc, id, name);
}

// [net] online_test: nifm and ntc report online (nothing reaches the network: bsd and BCAT answer as before). Read per call (the setting
// does not change while a game runs; tests change it between cases). PokoMac has no host redirection (AnySwitch's net/redirect.h).
bool online_test() { return config_get_bool("net", "online_test", false); }

// ---- the network nifm offers the game ----------------------------------------------------------------------------------------------------
// One state behind every nifm answer, read at each call (not when the service object is made), from these sources in order:
//   the LAN room (nifm_set_lan_source: online-servers' services/lan_room, a game's own LAN mode over a relay room): its address and mask;
//   [net] online_test (online_test above): online on 10.0.0.2/24, gateway 10.0.0.1 (no host address is shown);
//   otherwise no connection (nifm 110-300), as before.
std::atomic<NifmLanSource>& lan_source() {
    static std::atomic<NifmLanSource> f{nullptr};
    return f;
}

struct NifmNow {
    bool online = false;
    uint32_t ip = 0, mask = 0, gateway = 0;   // host order
};

NifmNow nifm_now() {
    NifmNow n;
    if (const NifmLanSource f = lan_source().load()) {
        uint32_t ip = 0, mask = 0;
        if (f(&ip, &mask)) {
            n.online = true;
            n.ip = ip;
            n.mask = mask;
            return n;
        }
    }
    if (online_test()) {
        n.online = true;
        n.ip = 0x0A000002;
        n.mask = 0xFFFFFF00;
        n.gateway = 0x0A000001;
    }
    return n;
}

// nn::nifm::IpV4Address: the four bytes in network order (Ryujinx Nifm/StaticService/Types/IpV4Address.cs).
void put_ip(std::vector<uint8_t>& b, uint32_t ip) {
    for (int k = 24; k >= 0; k -= 8) b.push_back(static_cast<uint8_t>(ip >> k));
}

// IpAddressSetting (0xD: u8 dhcp, address, mask, gateway) then DnsSetting (9: u8 dynamic, primary, secondary): 0x16 bytes
// (Ryujinx Nifm/StaticService/Types/IpAddressSetting.cs, DnsSetting.cs; Lichii nifm.cpp pushes the same two raw).
std::vector<uint8_t> ip_config(const NifmNow& n) {
    std::vector<uint8_t> b;
    b.push_back(1);
    put_ip(b, n.ip);
    put_ip(b, n.mask);
    put_ip(b, n.gateway);
    b.push_back(0);
    put_ip(b, 0);
    put_ip(b, 0);
    return b;
}

constexpr int kMaxCallLogs = 50;

// Printable text of a 0x20-byte name field in a request's raw arguments ("?" when it is not plain text).
std::string raw_name(const ipc::Request& req, size_t off) {
    std::string out;
    for (size_t i = 0; i < 0x20 && off + i < req.in_size; ++i) {
        const uint8_t c = req.in[off + i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return "?";
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Logs one call of a command at Info, the first kMaxCallLogs calls of each (service, command id, name). BCAT directory and file opens and
// the sync requests with a directory name also log the name(s) they were given; no buffers, nothing else from the arguments.
void log_call(const char* svc, const std::string& iface, uint32_t id, const char* name, const ipc::Request& req) {
    // only the services events could depend on: bsd, ssl and the others stay off this path (bsd runs per packet in Link Play)
    if (std::strcmp(svc, "nifm") != 0 && std::strcmp(svc, "bcat") != 0 && std::strcmp(svc, "ntc") != 0) return;
    NetState& s = st();
    int n;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        n = ++s.calls[std::string(svc) + " " + iface + " " + std::to_string(id) + " " + name];
    }
    if (n > kMaxCallLogs) return;
    std::string arg;
    if (std::strcmp(svc, "bcat") == 0) {
        const std::string nm = name;
        if (nm == "Open") {
            arg = " path=\"" + raw_name(req, 0) + "\"";
            if (req.in_size >= 0x40) arg += " file=\"" + raw_name(req, 0x20) + "\"";
        } else if (nm.find("DirectoryName") != std::string::npos) {
            arg = " dir=\"" + raw_name(req, req.in_size >= 0x28 ? 8 : 0) + "\"";
        }
    }
    NXLOG_I("net", "ask: %s cmd %u %s (call %d)%s", svc, id, name, n, arg.c_str());
}

// "ask:" lines for what the game tries to reach: DNS names, connect targets, TLS host names. The first kMaxCallLogs of each kind; only
// the names and the address are logged, never a buffer or payload.
bool ask_budget(int kind) {
    static std::atomic<int> counts[3];
    return counts[kind].fetch_add(1) < kMaxCallLogs;
}

// Printable ASCII text of an input buffer (up to 255 bytes, up to the first 0); "?" when it holds anything else.
std::string buf_text(const ipc::Buffer& b) {
    uint8_t tmp[255] = {};
    const uint64_t n = buf_read(b, tmp, b.size < sizeof tmp ? b.size : sizeof tmp);
    std::string out;
    for (uint64_t i = 0; i < n; ++i) {
        if (tmp[i] == 0) break;
        if (tmp[i] < 0x20 || tmp[i] > 0x7E) return "?";
        out.push_back(static_cast<char>(tmp[i]));
    }
    return out;
}

void ask_dns(const ipc::Request& req) {
    if (!ask_budget(0)) return;
    // switchbrew sfdnsres: GetAddrInfoRequest[WithOptions] takes the host name in input buffer 0 and the service name in buffer 1
    const std::string host = buf_text(req.in_buf(0));
    const std::string serv = buf_text(req.in_buf(1));
    NXLOG_I("net", "ask: dns host=\"%s\" service=\"%s\"", host.c_str(), serv.c_str());
}

void ask_connect(const vsock::Addr& a) {
    if (!ask_budget(1)) return;
    NXLOG_I("net", "ask: connect %u.%u.%u.%u:%u", (a.ip >> 24) & 255, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255, static_cast<unsigned>(a.port));
}

void ask_ssl_host(const ipc::Request& req) {
    if (!ask_budget(2)) return;
    // switchbrew ISslConnection::SetHostName: the name in input buffer 0
    NXLOG_I("net", "ask: ssl host=\"%s\"", buf_text(req.in_buf(0)).c_str());
}

uint32_t alloc_client_id() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_client_id++;
}
uint64_t alloc_pki_id() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_pki_id++;
}
uint32_t alloc_cancel_handle() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_cancel_handle++;
}

// ---- command tables -----------------------------------------------------------------------------------------------------------------------
// The common answer shapes. Rows are the commands whose answer is fixed (no state, no object, no event).
enum class K : uint8_t {
    Ok,       // result 0, no output (accepted and ignored)
    U8,       // result 0, one u8 = v
    U32,      // result 0, one u32 = v
    S32,      // result 0, one s32 = v
    U64,      // result 0, one u64 = v
    Fail,     // result v (no network, no such object ...)
    Zeros,    // result 0, n zero bytes of output
    BufZero,  // result 0, n zero bytes written into the output buffer
    Errno,    // BSD: (i32 ret = -1, u32 errno = ENETDOWN, then n u32 zeros)
    ErrnoOk,  // BSD: (i32 ret = 0, u32 errno = 0)
    Bytes,    // result 0, the n low bytes of v, lowest first
};
struct Row {
    uint32_t id;
    const char* name;
    K k;
    uint64_t v = 0;   // U8/U32/S32/U64 value, Fail result
    uint32_t n = 0;   // Zeros/BufZero byte count, Errno: extra u32 zeros
};

uint32_t run(const Row& r, ipc::Request& req, ipc::Response& rsp) {
    switch (r.k) {
        case K::Ok: return 0;
        case K::U8: rsp.push(static_cast<uint8_t>(r.v)); return 0;
        case K::U32: rsp.push(static_cast<uint32_t>(r.v)); return 0;
        case K::S32: rsp.push(static_cast<int32_t>(r.v)); return 0;
        case K::U64: rsp.push(static_cast<uint64_t>(r.v)); return 0;
        case K::Fail: return static_cast<uint32_t>(r.v);
        case K::Zeros: rsp.out.insert(rsp.out.end(), r.n, uint8_t{0}); return 0;
        case K::BufZero: {
            std::vector<uint8_t> z(r.n, 0);
            buf_write(req.out_buf(0), z.data(), r.n);
            return 0;
        }
        case K::Errno:
            rsp.push(static_cast<int32_t>(-1));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            for (uint32_t i = 0; i < r.n; ++i) rsp.push(static_cast<uint32_t>(0));
            return 0;
        case K::ErrnoOk:
            rsp.push(static_cast<int32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        case K::Bytes:
            for (uint32_t i = 0; i < r.n && i < 8; ++i) rsp.push(static_cast<uint8_t>(r.v >> (8 * i)));
            return 0;
    }
    return 0;
}

// One command with its own handler; logs the first use.
void cmd(ipc::Service& s, const char* svc, uint32_t id, const char* name, ipc::Handler h) {
    s.add_cmd(id, name, [svc, iface = s.name(), id, name, h = std::move(h)](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        note(svc, id, name);
        log_call(svc, iface, id, name, req);
        return h(req, rsp);
    });
}

// Every row of a table becomes a command.
void rows(ipc::Service& s, const char* svc, std::initializer_list<Row> list) {
    for (const Row& r : list) {
        Row row = r;
        cmd(s, svc, row.id, row.name, [row](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return run(row, req, rsp); });
    }
}

// A service object whose release is observed (ssl context and connection counts). on_close runs when the last reference goes.
class Tracked final : public ipc::Service {
public:
    Tracked(const char* name, std::function<void()> released) : ipc::Service(name), released_(std::move(released)) {}
    void on_close() override {
        if (released_) released_();
    }

private:
    std::function<void()> released_;
};

std::shared_ptr<ipc::Service> tracked(const char* name, std::function<void()> released, const std::function<void(ipc::Service&)>& setup) {
    auto s = std::make_shared<Tracked>(name, std::move(released));
    setup(*s);
    return s;
}

// Copy handles belong to the client (HIPC copies them: the guest closes its own afterwards), so there is nothing to close here (debug note 122).
void close_copied(const ipc::Request&) {}

// Event pairs held by the object that hands them out.
struct TwoEvents {
    ServiceEvent a;
    ServiceEvent b;
};

// A request's evaluation (nxnative services.md "Network requests stay on hold before they settle"): on the console nifm evaluates a
// submitted request while it serves the queries, so the first query after Submit still finds it OnHold, and a game that starts a thread
// that submits before it publishes its own state relies on that (Pikmin 4's BCAT runnable: answered settled at once, the thread's
// "unavailable" was overwritten and the boot waited for good, 1 boot in 6). So: the first GetRequestState after Submit answers OnHold and
// the state-change events are signalled (a waiter wakes and asks again), the next answers settled. Settled by query count, not by time:
// a game that bounds its wait by the number of queries (Hades: 2,000) ends it either way.
struct RequestEval {
    std::mutex mu;
    bool on_hold = false;   // submitted and not yet queried
};

// =============================================================================================================================================
// nifm: nifm:u, nifm:a, nifm:s (IStaticService). Switchbrew "Network Interface services", ScarletNX nifm.swipc, Ryujinx Nifm/.
// No connection: GetCurrentIpAddress and the other address/profile answers return nifm 300 (NoInternetConnection); with [net] online_test = on
// they report online instead.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_scan_request() {
    auto ev = std::make_shared<ServiceEvent>();
    return make_iface("nn::nifm::detail::IScanRequest", [ev](ipc::Service& s) {
        rows(s, "nifm", {
            {0, "Submit", K::Ok},
            {1, "IsProcessing", K::U8, 0},
            {2, "GetResult", K::Ok},            // a scan that is over at once and found nothing
            {4, "SetChannels", K::Ok},
            {5, "Cancel", K::Ok},
        });
        cmd(s, "nifm", 3, "GetSystemEventReadableHandle", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_request() {
    auto ev = std::make_shared<TwoEvents>();
    auto eval = std::make_shared<RequestEval>();
    return make_iface("nn::nifm::detail::IRequest", [ev, eval](ipc::Service& s) {
        // 1 = Free: nothing was accepted; 2 = OnHold: being evaluated; 3 = Accepted (nifm_now online)
        cmd(s, "nifm", 0, "GetRequestState", [ev, eval](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const bool online = nifm_now().online;
            bool hold;
            {
                std::lock_guard<std::mutex> lk(eval->mu);
                hold = eval->on_hold;
                eval->on_hold = false;
            }
            if (hold) {   // settled by the next query: the state changes, so its events fire
                ev->a.signal();
                ev->b.signal();
            }
            rsp.push(static_cast<uint32_t>(hold ? 2 : online ? 3 : 1));
            return 0;
        });
        cmd(s, "nifm", 1, "GetResult", [eval](ipc::Request&, ipc::Response&) -> uint32_t {
            const bool online = nifm_now().online;
            std::lock_guard<std::mutex> lk(eval->mu);
            eval->on_hold = false;   // the result is asked for: evaluated
            return online ? 0 : kNifmNoConnection;
        });
        cmd(s, "nifm", 3, "Cancel", [eval](ipc::Request&, ipc::Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(eval->mu);
            eval->on_hold = false;
            return 0;
        });
        cmd(s, "nifm", 4, "Submit", [eval](ipc::Request&, ipc::Response&) -> uint32_t {
            nifm_now();   // the sources hear of the request now (the LAN room joins its relay room on the first one)
            std::lock_guard<std::mutex> lk(eval->mu);
            eval->on_hold = true;
            return 0;
        });
        // SetRequirement (5: the 0x24-byte Requirement) and SetRequirementPreset (6: u32 preset) are accepted; each distinct preset, and
        // each distinct requirement's first 16 bytes, is logged once, so a run shows which one a game's local-network mode asks for
        // (nn::nifm::SetLocalNetworkMode) and which its internet sign-in asks for.
        cmd(s, "nifm", 6, "SetRequirementPreset", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            static std::mutex mu;
            static std::set<uint32_t> seen;
            const uint32_t preset = req.arg<uint32_t>(0);
            std::lock_guard<std::mutex> lk(mu);
            if (seen.insert(preset).second) NXLOG_I("net", "nifm: a request with requirement preset %u", preset);
            return 0;
        });
        cmd(s, "nifm", 5, "SetRequirement", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            static std::mutex mu;
            static std::set<std::string> seen;
            char hex[33] = {};
            for (size_t i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", req.arg<uint8_t>(i));
            std::lock_guard<std::mutex> lk(mu);
            if (seen.insert(hex).second) NXLOG_I("net", "nifm: a request with requirement %s...", hex);
            return 0;
        });
        rows(s, "nifm", {
            {8, "SetPriority", K::Ok},
            {9, "SetNetworkProfileId", K::Ok},
            {10, "SetRejectable", K::Ok},
            {11, "SetConnectionConfirmationOption", K::Ok},
            {12, "SetPersistent", K::Ok},
            {13, "SetInstant", K::Ok},
            {14, "SetSustainable", K::Ok},
            {15, "SetRawPriority", K::Ok},
            {16, "SetGreedy", K::Ok},
            {17, "SetSharable", K::Ok},
            {18, "SetRequirementByRevision", K::Ok},
            {19, "GetRequirement", K::Zeros, 0, 0x24},           // 0x20 bytes, plus 4 from 3.0.0
            {20, "GetRevision", K::U32, 0},
            {22, "GetAdditionalInfo", K::Ok},
            {23, "SetKeptInSleep", K::Ok},
            {24, "RegisterSocketDescriptor", K::Ok},
            {25, "UnregisterSocketDescriptor", K::Ok},
            {26, "GetNetworkAccessStatus", K::U32, 0},
        });
        cmd(s, "nifm", 2, "GetSystemEventReadableHandles", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->a.new_handle());   // signals when RequestState changes: never does here
            rsp.out_copy.push_back(ev->b.new_handle());
            return 0;
        });
        cmd(s, "nifm", 21, "GetAppletInfo", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            // The error applet (0xE) with no data, as Ryujinx answers it when the request has failed.
            rsp.push(static_cast<uint32_t>(0xE));
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_general() {
    const uint32_t client_id = alloc_client_id();
    auto ev = std::make_shared<ServiceEvent>();
    return make_iface("nn::nifm::detail::IGeneralService", [client_id, ev](ipc::Service& s) {
        cmd(s, "nifm", 1, "GetClientId", [client_id](ipc::Request& req, ipc::Response&) -> uint32_t {
            buf_write(req.out_buf(0), &client_id, sizeof client_id);
            return 0;
        });
        cmd(s, "nifm", 2, "CreateScanRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_scan_request());
            return 0;
        });
        cmd(s, "nifm", 4, "CreateRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_request());
            return 0;
        });
        cmd(s, "nifm", 31, "GetTelemetorySystemEventReadableHandle", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
        // GetCurrentNetworkProfile -> buffer 0x1A, NetworkProfileData (0x17C: IpSettingData 0xC2 = IpAddressSetting, DnsSetting, 0xAA
        // ProxySetting, u16 MTU; u128 uuid; 64 B name; 4 B; WirelessSettingData 0x65; 1 B): Ryujinx Nifm/StaticService/IGeneralService.cs
        cmd(s, "nifm", 5, "GetCurrentNetworkProfile", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const NifmNow n = nifm_now();
            if (!n.online) return kNifmNoConnection;
            std::vector<uint8_t> p = ip_config(n);
            p.resize(0xC2, 0);                                  // ProxySetting and MTU: none
            static const uint8_t kUuid[16] = {'P', 'o', 'k', 'o', 'M', 'a', 'c', 0, 0, 0x4E, 0x45, 0x54, 0x01, 0, 0, 1};
            p.insert(p.end(), kUuid, kUuid + 16);
            const char kName[] = "PokoMac";
            p.insert(p.end(), kName, kName + sizeof kName - 1);
            p.resize(0x17C, 0);
            buf_write(req.out_buf(0), p.data(), p.size());
            return 0;
        });
        cmd(s, "nifm", 12, "GetCurrentIpAddress", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const NifmNow n = nifm_now();
            if (!n.online) return kNifmNoConnection;
            std::vector<uint8_t> b;
            put_ip(b, n.ip);
            rsp.out.insert(rsp.out.end(), b.begin(), b.end());
            return 0;
        });
        cmd(s, "nifm", 15, "GetCurrentIpConfigInfo", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const NifmNow n = nifm_now();
            if (!n.online) return kNifmNoConnection;
            const std::vector<uint8_t> b = ip_config(n);
            rsp.out.insert(rsp.out.end(), b.begin(), b.end());
            return 0;
        });
        cmd(s, "nifm", 18, "GetInternetConnectionStatus", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            if (!nifm_now().online) return kNifmNoConnection;
            const uint8_t st[3] = {1, 3, 4};                    // Wifi, strength 3, Connected (Ryujinx's answer)
            rsp.out.insert(rsp.out.end(), st, st + sizeof st);
            return 0;
        });
        for (uint32_t id : {21u, 22u}) {
            cmd(s, "nifm", id, id == 21 ? "IsAnyInternetRequestAccepted" : "IsAnyForegroundRequestAccepted",
                [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                    rsp.push(static_cast<uint8_t>(nifm_now().online ? 1 : 0));
                    return 0;
                });
        }
        cmd(s, "nifm", 33, "ConfirmSystemAvailability", [](ipc::Request&, ipc::Response&) -> uint32_t {
            return nifm_now().online ? 0 : kNifmNoConnection;
        });
        rows(s, "nifm", {
            {6, "EnumerateNetworkInterfaces", K::S32, 0},
            {7, "EnumerateNetworkProfiles", K::S32, 0},
            {8, "GetNetworkProfile", K::Fail, kNifmNoConnection},
            {9, "SetNetworkProfile", K::Zeros, 0, 16},          // returns a Uuid (16 zero bytes); the profile is not kept
            {10, "RemoveNetworkProfile", K::Ok},
            {11, "GetScanDataOld", K::S32, 0},
            {13, "GetCurrentAccessPointOld", K::Fail, kNifmNoConnection},
            {14, "CreateTemporaryNetworkProfile", K::Fail, kNifmNoConnection},
            {16, "SetWirelessCommunicationEnabled", K::Ok},
            {17, "IsWirelessCommunicationEnabled", K::U8, 1},   // radio on, no access point
            {19, "SetEthernetCommunicationEnabled", K::Ok},
            {20, "IsEthernetCommunicationEnabled", K::U8, 0},   // no adapter
            {23, "PutToSleep", K::Ok},
            {24, "WakeUp", K::Ok},
            {25, "GetSsidListVersion", K::U32, 0},
            {26, "SetExclusiveClient", K::Ok},
            {27, "GetDefaultIpSetting", K::Zeros, 0, 0xD},
            {28, "SetDefaultIpSetting", K::Ok},
            {29, "SetWirelessCommunicationEnabledForTest", K::Ok},
            {30, "SetEthernetCommunicationEnabledForTest", K::Ok},
            {32, "GetTelemetryInfo", K::Ok},
            {34, "SetBackgroundRequestEnabled", K::Ok},
            {35, "GetScanData", K::S32, 0},
            {36, "GetCurrentAccessPoint", K::Fail, kNifmNoConnection},
            {37, "Shutdown", K::Ok},
            {38, "GetAllowedChannels", K::Ok},
            {39, "NotifyApplicationSuspended", K::Ok},
            {40, "SetAcceptableNetworkTypeFlag", K::Ok},
            {41, "GetAcceptableNetworkTypeFlag", K::U32, 0},
            {42, "NotifyConnectionStateChanged", K::Ok},
            {43, "SetWowlDelayedWakeTime", K::Ok},
            {44, "IsWiredConnectionAvailable", K::U8, 0},
            {45, "IsNetworkEmulationFeatureEnabled", K::U8, 0},
            {46, "SelectActiveNetworkEmulationProfileIdForDebug", K::Ok},
            {47, "GetScanData", K::S32, 0},
            {48, "ResetActiveNetworkEmulationProfileId", K::Ok},
            {49, "GetActiveNetworkEmulationProfileId", K::U32, 0},
            {50, "IsRewriteFeatureEnabled", K::U8, 0},
            {51, "CreateRewriteRule", K::Ok},
            {52, "DestroyRewriteRule", K::Ok},
            {53, "IsActiveNetworkEmulationProfileIdSelected", K::U8, 0},
            {54, "SelectDefaultNetworkEmulationProfileId", K::Ok},
            {55, "GetDefaultNetworkEmulationProfileId", K::U32, 0},
            {56, "GetNetworkEmulationProfile", K::Fail, kNifmNoConnection},
            {57, "SetWowlTcpKeepAliveTimeout", K::Ok},
            // 101 (SDK 21+, no name known): Lichii answers success with 32 zero words; Ocarina of Time's network setup aborts on a refusal
            {101, "Cmd101", K::Zeros, 0, 0x80},
        });
    });
}

std::shared_ptr<ipc::Service> make_nifm_static() {
    return make_iface("nn::nifm::detail::IStaticService", [](ipc::Service& s) {
        cmd(s, "nifm", 4, "CreateGeneralServiceOld", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_general());
            return 0;
        });
        cmd(s, "nifm", 5, "CreateGeneralService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_general());
            return 0;
        });
    });
}

// =============================================================================================================================================
// bsd: bsd:u, bsd:s, bsd:a (IClient). Switchbrew "Sockets services". The UDP commands are a thin adapter over the vsock socket table
// (net/vsock.h: UDP datagrams on the vnet link ldn brings up); SOCK_STREAM and the other domains still answer ENETDOWN, and every other
// socket-level command returns -1 with errno ENETDOWN, or the success pair where the command is a control call. bsdcfg has an empty
// command table (ScarletNX bsd.swipc).
// Layouts from libnx nx/source/services/bsd.c (ISC) and Ryujinx Sockets/Bsd/IClient.cs (MIT): input (i32 fd, i32 flags/level/...) raw;
// data and addresses in AutoSelect buffers (type 0x21 in = A or X, 0x22 out = B or C: in_buf()/out_buf() take either); output (i32 ret,
// u32 errno), then u32 addrlen / optlen for RecvFrom, GetSockName, GetPeerName and GetSockOpt. The socket table is process-wide (every
// bsd session sees the same descriptors).
// sockaddr_in on the Switch is FreeBSD's: u8 sin_len (16), u8 sin_family (AF_INET 2), be16 port, be32 address, 8 zero bytes.
// =============================================================================================================================================

constexpr size_t kSockaddrSize = 16;

// (i32 ret, u32 errno) and the optional trailing u32.
uint32_t bsd_ret(ipc::Response& rsp, const vsock::Ret& r) {
    rsp.push(r.ret);
    rsp.push(r.err);
    return 0;
}

// Reads a sockaddr_in from an input buffer. 0 or an errno (EINVAL: too short, EAFNOSUPPORT: not AF_INET).
uint32_t read_sockaddr(const ipc::Buffer& b, vsock::Addr& a) {
    uint8_t sa[kSockaddrSize] = {};
    if (buf_read(b, sa, sizeof sa) < 8) return vsock::kEinval;
    if (sa[1] != vsock::kAfInet) return vsock::kEafnosupport;
    a.port = static_cast<uint16_t>((sa[2] << 8) | sa[3]);
    a.ip = (uint32_t{sa[4]} << 24) | (uint32_t{sa[5]} << 16) | (uint32_t{sa[6]} << 8) | sa[7];
    return 0;
}

// Writes a sockaddr_in to an output buffer; returns the bytes written (the addrlen answer).
uint32_t write_sockaddr(const ipc::Buffer& b, const vsock::Addr& a) {
    const uint8_t sa[kSockaddrSize] = {static_cast<uint8_t>(kSockaddrSize), static_cast<uint8_t>(vsock::kAfInet),
                                       static_cast<uint8_t>(a.port >> 8), static_cast<uint8_t>(a.port),
                                       static_cast<uint8_t>(a.ip >> 24), static_cast<uint8_t>(a.ip >> 16),
                                       static_cast<uint8_t>(a.ip >> 8), static_cast<uint8_t>(a.ip)};
    return static_cast<uint32_t>(buf_write(b, sa, sizeof sa));
}

// A command whose sockaddr input failed to parse answers -1 with that errno, unless the descriptor is bad (EBADF comes first).
vsock::Ret addr_fail(int fd, uint32_t e) { return {-1, vsock::stack().is_socket(fd) ? e : vsock::kEbadf}; }

uint32_t bsd_send(ipc::Response& rsp, int fd, int flags, const ipc::Buffer& data, const vsock::Addr* to) {
    return bsd_ret(rsp, vsock::stack().sendto(fd, buf_ptr(data), static_cast<size_t>(data.size), flags, to));
}

uint32_t bsd_recv(ipc::Response& rsp, int fd, int flags, const ipc::Buffer& data, vsock::Addr* from) {
    return bsd_ret(rsp, vsock::stack().recvfrom(fd, buf_mut(data), static_cast<size_t>(data.size), flags, from));
}

// ---- [net] protocol = native: the co-op module's virtual sockets (third_party/pokopia-coop coop.h, after pokopia-native's
// windows/src/coop/nxnative/sock_glue.c). AF_INET datagram sockets made while its ldn is initialized are its descriptors (0x100..0x11F);
// every command on one of them goes to coop_vnet_*, everything else stays on the vsock table. The module answers >= 0 or -errno in
// FreeBSD numbers; bsd answers Linux numbers (as vsock does), so they are translated. It has no connected sockets: Send/Write on one
// answers EDESTADDRREQ, Connect, GetPeerName and Shutdown ENETDOWN (pokopia-native's answer). Poll and Select wait on the module when
// every descriptor in the set is one of its; any other set waits on the socket table (vsock.h).
static_assert(vsock::kMaxSockets <= COOP_FD_BASE, "vsock descriptors (0..kMaxSockets-1) must stay below the co-op module's");

constexpr int kCoopFamily = 2, kCoopDgram = 2;
constexpr int kBsdONonblock = 4;   // FreeBSD O_NONBLOCK (the module's); vsock::kONonblock is Linux's 0x800: Fcntl accepts either

uint32_t coop_errno(int64_t e) {
    switch (e) {
        case COOP_EAGAIN: return vsock::kEagain;
        case 39: return vsock::kEdestaddrreq;   // EDESTADDRREQ
        case COOP_EMSGSIZE: return vsock::kEmsgsize;
        case COOP_ENOPROTOOPT: return 92;       // Linux ENOPROTOOPT
        case COOP_EADDRINUSE: return vsock::kEaddrinuse;
        case COOP_EADDRNOTAVAIL: return vsock::kEaddrnotavail;
        case COOP_ENETDOWN: return vsock::kEnetdown;
        case COOP_ENETUNREACH: return vsock::kEnetunreach;
        default: return static_cast<uint32_t>(e);   // EBADF 9, EACCES 13, EINVAL 22, EMFILE 24: the same in both
    }
}
vsock::Ret coop_r(int64_t r) { return r >= 0 ? vsock::Ret{static_cast<int32_t>(r), 0} : vsock::Ret{-1, coop_errno(-r)}; }

// The co-op node when fd is one of its descriptors, else null.
coop_node* coop_fd(int fd) {
    coop_node* n = ldn_native_node();
    return n && fd >= COOP_FD_BASE && coop_vnet_is(n, fd) ? n : nullptr;
}

uint32_t coop_send(ipc::Response& rsp, coop_node* n, int fd, int flags, const ipc::Buffer& data, const ipc::Buffer* to) {
    uint8_t sa[kSockaddrSize] = {};
    uint32_t salen = 0;
    if (to) salen = static_cast<uint32_t>(buf_read(*to, sa, sizeof sa));
    return bsd_ret(rsp, coop_r(coop_vnet_sendto(n, fd, buf_ptr(data), data.addr ? data.size : 0, flags, salen ? sa : nullptr, salen)));
}

uint32_t coop_recv(ipc::Response& rsp, coop_node* n, int fd, int flags, const ipc::Buffer& data, const ipc::Buffer* from) {
    uint8_t sa[kSockaddrSize] = {};
    uint32_t salen = sizeof sa;
    const vsock::Ret r = coop_r(coop_vnet_recvfrom(n, fd, buf_mut(data), data.addr ? data.size : 0, flags, sa, &salen));
    bsd_ret(rsp, r);
    if (from) rsp.push(r.err ? uint32_t{0} : static_cast<uint32_t>(buf_write(*from, sa, salen < sizeof sa ? salen : sizeof sa)));
    return 0;
}

// Poll (libnx bsdPoll: u32 nfds, s32 timeout; the pollfd array in buffer 0 in and out, 8 bytes each: i32 fd, i16 events, i16 revents).
uint32_t bsd_poll(ipc::Request& req, ipc::Response& rsp) {
    const uint32_t nfds = req.arg<uint32_t>(0);
    const int32_t timeout = req.arg<int32_t>(4);
    coop_node* n = ldn_native_node();
    std::vector<coop_pollfd> fds(nfds <= 4096 ? nfds : 0);
    const uint64_t want = fds.size() * sizeof(coop_pollfd);
    bool virt = n && !fds.empty() && buf_read(req.in_buf(0), fds.data(), want) == want;
    bool any = false;
    for (const coop_pollfd& p : fds) {
        if (p.fd < 0) continue;
        any = true;
        if (!virt || !coop_fd(p.fd)) virt = false;
    }
    if (!virt || !any) {
        // The descriptors of the socket table (UDP sockets, event descriptors), or none at all: poll waits on them (vsock.h), as the
        // console's does with no network. gRPC (SDK 22 titles) polls its event descriptors this way; a set it cannot wait on would fail.
        static_assert(sizeof(coop_pollfd) == sizeof(vsock::PollFd), "pollfd layouts");
        if (fds.size() != nfds || buf_read(req.in_buf(0), fds.data(), want) != want) return bsd_ret(rsp, {-1, vsock::kEinval});
        const vsock::Ret r = vsock::stack().poll(reinterpret_cast<vsock::PollFd*>(fds.data()), fds.size(), timeout);
        if (!r.err) buf_write(req.out_buf(0), fds.data(), want);
        return bsd_ret(rsp, r);
    }
    const vsock::Ret r = coop_r(coop_vnet_poll(n, fds.data(), fds.size(), timeout));
    if (!r.err) buf_write(req.out_buf(0), fds.data(), want);
    return bsd_ret(rsp, r);
}

// Select (libnx bsdSelect: u32 nfds, timeval {i64 sec, i64 usec} at 8, bool null-timeout at 0x18; the read, write and except fd_sets
// (1024 bits) in buffers 0..2 in and 0..2 out, an absent set as an empty buffer).
uint32_t bsd_select(ipc::Request& req, ipc::Response& rsp) {
    const int32_t nfds = req.arg<int32_t>(0);
    const int64_t sec = req.arg<int64_t>(8), usec = req.arg<int64_t>(0x10);
    const bool no_timeout = req.arg<uint8_t>(0x18) != 0;
    coop_node* n = ldn_native_node();
    uint64_t sets[3][16] = {};
    bool have[3] = {};
    bool virt = n != nullptr, any = false;
    for (int k = 0; k < 3; ++k) {
        const ipc::Buffer b = req.in_buf(static_cast<size_t>(k));
        if (!b.addr || !b.size) continue;
        have[k] = true;
        buf_read(b, sets[k], sizeof sets[k]);
        for (int fd = 0; fd < nfds && fd < 1024; ++fd) {
            if (!(sets[k][fd >> 6] >> (fd & 63) & 1)) continue;
            any = true;
            if (!virt || !coop_fd(fd)) virt = false;
        }
    }
    int64_t ms = no_timeout ? -1 : sec * 1000 + usec / 1000;
    if (ms > 0x7fffffff) ms = 0x7fffffff;
    if (!virt || !any) {   // the socket table's descriptors, or none: as Poll above
        const vsock::Ret r = vsock::stack().select(nfds, have[0] ? sets[0] : nullptr, have[1] ? sets[1] : nullptr, have[2] ? sets[2] : nullptr,
                                                   static_cast<int>(ms));
        if (!r.err)
            for (int k = 0; k < 3; ++k)
                if (have[k]) buf_write(req.out_buf(static_cast<size_t>(k)), sets[k], sizeof sets[k]);
        return bsd_ret(rsp, r);
    }
    const vsock::Ret r = coop_r(coop_vnet_select(n, nfds, have[0] ? sets[0] : nullptr, have[1] ? sets[1] : nullptr,
                                                 have[2] ? sets[2] : nullptr, static_cast<int>(ms)));
    if (!r.err)
        for (int k = 0; k < 3; ++k)
            if (have[k]) buf_write(req.out_buf(static_cast<size_t>(k)), sets[k], sizeof sets[k]);
    return bsd_ret(rsp, r);
}

void add_bsd_udp(ipc::Service& s) {
    auto socket = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int domain = req.arg<int32_t>(0), type = req.arg<int32_t>(4), proto = req.arg<int32_t>(8);
        coop_node* n = ldn_native_node();
        if (n && domain == kCoopFamily && (type & 0xFFFF) == kCoopDgram && coop_ldn_state(n) != COOP_LS_NONE)
            return bsd_ret(rsp, coop_r(coop_vnet_socket(n, domain, type, proto)));
        return bsd_ret(rsp, vsock::stack().socket(domain, type, proto));
    };
    cmd(s, "bsd", 2, "Socket", socket);
    cmd(s, "bsd", 3, "SocketExempt", socket);
    cmd(s, "bsd", 8, "Recv", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return coop_recv(rsp, n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.out_buf(0), nullptr);
        return bsd_recv(rsp, req.arg<int32_t>(0), req.arg<int32_t>(4), req.out_buf(0), nullptr);
    });
    cmd(s, "bsd", 9, "RecvFrom", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) {
            const ipc::Buffer from = req.out_buf(1);
            return coop_recv(rsp, n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.out_buf(0), &from);
        }
        vsock::Addr from;
        const vsock::Ret r = vsock::stack().recvfrom(req.arg<int32_t>(0), buf_mut(req.out_buf(0)), static_cast<size_t>(req.out_buf(0).size),
                                                     req.arg<int32_t>(4), &from);
        bsd_ret(rsp, r);
        rsp.push(r.err ? uint32_t{0} : write_sockaddr(req.out_buf(1), from));
        return 0;
    });
    cmd(s, "bsd", 10, "Send", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return coop_send(rsp, n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.in_buf(0), nullptr);
        return bsd_send(rsp, req.arg<int32_t>(0), req.arg<int32_t>(4), req.in_buf(0), nullptr);
    });
    cmd(s, "bsd", 11, "SendTo", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int fd = req.arg<int32_t>(0);
        if (coop_node* n = coop_fd(fd)) {
            const ipc::Buffer to = req.in_buf(1);
            return coop_send(rsp, n, fd, req.arg<int32_t>(4), req.in_buf(0), &to);
        }
        vsock::Addr to;
        if (const uint32_t e = read_sockaddr(req.in_buf(1), to)) return bsd_ret(rsp, addr_fail(fd, e));
        ask_connect(to);
        return bsd_send(rsp, fd, req.arg<int32_t>(4), req.in_buf(0), &to);
    });
    cmd(s, "bsd", 13, "Bind", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int fd = req.arg<int32_t>(0);
        if (coop_node* n = coop_fd(fd)) {
            uint8_t sa[kSockaddrSize] = {};
            const uint32_t len = static_cast<uint32_t>(buf_read(req.in_buf(0), sa, sizeof sa));
            return bsd_ret(rsp, coop_r(coop_vnet_bind(n, fd, sa, len)));
        }
        vsock::Addr a;
        if (const uint32_t e = read_sockaddr(req.in_buf(0), a)) return bsd_ret(rsp, addr_fail(fd, e));
        return bsd_ret(rsp, vsock::stack().bind(fd, a));
    });
    // Listen (u32 fd, s32 backlog) and Accept (u32 fd; the peer's sockaddr in buffer 0, then u32 addrlen): TCP over the loopback (vsock.h)
    cmd(s, "bsd", 18, "Listen", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_fd(req.arg<int32_t>(0))) return bsd_ret(rsp, {-1, vsock::kEopnotsupp});
        return bsd_ret(rsp, vsock::stack().listen(req.arg<int32_t>(0), req.arg<int32_t>(4)));
    });
    cmd(s, "bsd", 12, "Accept", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        vsock::Addr from;
        const vsock::Ret r = coop_fd(req.arg<int32_t>(0)) ? vsock::Ret{-1, vsock::kEopnotsupp} : vsock::stack().accept(req.arg<int32_t>(0), &from);
        bsd_ret(rsp, r);
        rsp.push(r.err ? uint32_t{0} : write_sockaddr(req.out_buf(0), from));
        return 0;
    });
    cmd(s, "bsd", 14, "Connect", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int fd = req.arg<int32_t>(0);
        vsock::Addr a;
        if (read_sockaddr(req.in_buf(0), a) == 0 && (a.ip >> 24) != 127) ask_connect(a);   // the loopback is this console's own
        if (coop_fd(fd)) return bsd_ret(rsp, {-1, kBsdNetDown});
        if (const uint32_t e = read_sockaddr(req.in_buf(0), a)) return bsd_ret(rsp, addr_fail(fd, e));
        return bsd_ret(rsp, vsock::stack().connect(fd, a));
    });
    auto name = [](bool peer) {
        return [peer](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            vsock::Addr a;
            const int fd = req.arg<int32_t>(0);
            if (coop_node* n = coop_fd(fd)) {
                if (peer) {
                    bsd_ret(rsp, {-1, kBsdNetDown});
                    rsp.push(uint32_t{0});
                    return 0;
                }
                uint8_t sa[kSockaddrSize] = {};
                uint32_t len = sizeof sa;
                const vsock::Ret r = coop_r(coop_vnet_getsockname(n, fd, sa, &len));
                bsd_ret(rsp, r);
                rsp.push(r.err ? uint32_t{0} : static_cast<uint32_t>(buf_write(req.out_buf(0), sa, len < sizeof sa ? len : sizeof sa)));
                return 0;
            }
            const vsock::Ret r = peer ? vsock::stack().getpeername(fd, &a) : vsock::stack().getsockname(fd, &a);
            bsd_ret(rsp, r);
            rsp.push(r.err ? uint32_t{0} : write_sockaddr(req.out_buf(0), a));
            return 0;
        };
    };
    cmd(s, "bsd", 15, "GetPeerName", name(true));
    cmd(s, "bsd", 16, "GetSockName", name(false));
    cmd(s, "bsd", 17, "GetSockOpt", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const ipc::Buffer out = req.out_buf(0);
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) {
            std::vector<uint8_t> v(static_cast<size_t>(out.size < 256 ? out.size : 256));
            uint32_t len = static_cast<uint32_t>(v.size());
            const vsock::Ret r = coop_r(coop_vnet_getsockopt(n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8),
                                                             v.empty() ? nullptr : v.data(), &len));
            bsd_ret(rsp, r);
            rsp.push(r.err ? uint32_t{0} : static_cast<uint32_t>(buf_write(out, v.data(), len)));
            return 0;
        }
        size_t n = static_cast<size_t>(out.size);
        const vsock::Ret r = vsock::stack().getsockopt(req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8), buf_mut(out), &n);
        bsd_ret(rsp, r);
        rsp.push(r.err ? uint32_t{0} : static_cast<uint32_t>(n));
        return 0;
    });
    cmd(s, "bsd", 20, "Fcntl", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) {
            const int c = req.arg<int32_t>(4);
            int v = req.arg<int32_t>(8);
            if (c == vsock::kFSetFl && (v & vsock::kONonblock)) v |= kBsdONonblock;
            vsock::Ret r = coop_r(coop_vnet_fcntl(n, req.arg<int32_t>(0), c, v));
            if (c == vsock::kFGetFl && !r.err && (r.ret & kBsdONonblock)) r.ret |= vsock::kONonblock;
            return bsd_ret(rsp, r);
        }
        return bsd_ret(rsp, vsock::stack().fcntl(req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8)));
    });
    cmd(s, "bsd", 21, "SetSockOpt", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const ipc::Buffer in = req.in_buf(0);
        if (coop_node* n = coop_fd(req.arg<int32_t>(0)))
            return bsd_ret(rsp, coop_r(coop_vnet_setsockopt(n, req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8), buf_ptr(in),
                                                            static_cast<uint32_t>(in.addr ? in.size : 0))));
        return bsd_ret(rsp, vsock::stack().setsockopt(req.arg<int32_t>(0), req.arg<int32_t>(4), req.arg<int32_t>(8), buf_ptr(in),
                                                      static_cast<size_t>(in.size)));
    });
    cmd(s, "bsd", 22, "Shutdown", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_fd(req.arg<int32_t>(0))) return bsd_ret(rsp, {-1, kBsdNetDown});
        return bsd_ret(rsp, vsock::stack().shutdown(req.arg<int32_t>(0), req.arg<int32_t>(4)));
    });
    cmd(s, "bsd", 24, "Write", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return coop_send(rsp, n, req.arg<int32_t>(0), 0, req.in_buf(0), nullptr);
        return bsd_send(rsp, req.arg<int32_t>(0), 0, req.in_buf(0), nullptr);
    });
    cmd(s, "bsd", 25, "Read", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return coop_recv(rsp, n, req.arg<int32_t>(0), 0, req.out_buf(0), nullptr);
        return bsd_recv(rsp, req.arg<int32_t>(0), 0, req.out_buf(0), nullptr);
    });
    cmd(s, "bsd", 26, "Close", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (coop_node* n = coop_fd(req.arg<int32_t>(0))) return bsd_ret(rsp, coop_r(coop_vnet_close(n, req.arg<int32_t>(0))));
        return bsd_ret(rsp, vsock::stack().close(req.arg<int32_t>(0)));
    });
    cmd(s, "bsd", 5, "Select", bsd_select);
    cmd(s, "bsd", 6, "Poll", bsd_poll);
    // EventFd (u64 initval, u32 flags): an event descriptor in the socket table (vsock.h). It needs no network, and gRPC's pollers in SDK
    // 22 titles abort without one (Ocarina of Time under grpc_init; nxnative services.md "EventFd needs no network", MIT).
    cmd(s, "bsd", 31, "EventFd", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return bsd_ret(rsp, vsock::stack().eventfd(req.arg<uint64_t>(0), req.arg<uint32_t>(8)));
    });
}

std::shared_ptr<ipc::Service> make_bsd() {
    return make_iface("nn::socket::sf::IClient", [](ipc::Service& s) {
        cmd(s, "bsd", 0, "RegisterClient", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            close_copied(req);                  // the transfer memory is not used
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        add_bsd_udp(s);
        rows(s, "bsd", {
            {1, "StartMonitoring", K::Ok},
            {4, "Open", K::Errno},
            {7, "Sysctl", K::Errno, 0, 1},
            {19, "Ioctl", K::Errno},
            {23, "ShutdownAllSockets", K::Errno},
            {27, "DuplicateSocket", K::Errno},
            {28, "GetResourceStatistics", K::Zeros, 0, 8},
            {32, "RegisterResourceStatisticsName", K::U32, 0},
            {33, "RegisterClientShared", K::U32, 0},
            {34, "GetSocketStatistics", K::Errno},
            {35, "NifIoctl", K::Errno},
            {36, "Cmd36", K::U32, 0},            // Switchbrew: 4 bytes out, stubbed to 0 on NX
            {37, "SetSocketBufferAttribute", K::Errno},
            {38, "Cmd38", K::Errno},            // no name in Switchbrew's table
            {39, "NoSfEventFd", K::Errno},
            {40, "ShutdownAllSockets", K::Errno},
            {41, "Cmd41", K::Errno},
            {42, "SendMMsgNoMem", K::Errno},
            {43, "RecvMMsgNoMem", K::Errno},
            {44, "Cmd44", K::Errno},
            {200, "SetThreadCoreMask", K::Ok},
            {201, "GetThreadCoreMask", K::U32, 0},
        });
        // 29 RecvMMsg, 30 SendMMsg on the socket table's descriptors (bsd_mmsg.h); the co-op module's have none (ENETDOWN, as before)
        cmd(s, "bsd", 29, "RecvMMsg", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            if (coop_fd(req.arg<int32_t>(0))) return bsd_ret(rsp, {-1, kBsdNetDown});
            return mmsg::recvmmsg(req, rsp, mmsg::offline(req.arg<int32_t>(0)));
        });
        cmd(s, "bsd", 30, "SendMMsg", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            if (coop_fd(req.arg<int32_t>(0))) return bsd_ret(rsp, {-1, kBsdNetDown});
            return mmsg::sendmmsg(req, rsp, mmsg::offline(req.arg<int32_t>(0)));
        });
    });
}

// =============================================================================================================================================
// ssl: ssl (ISslService). Switchbrew "SSL services". CreateContext answers. Connection calls fail with ssl InvalidSocket: there is no
// socket to run TLS over. Local settings (options, session cache mode, ALPN, verify option) are accepted.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_ssl_connection(const std::shared_ptr<int>& conns);

std::shared_ptr<ipc::Service> make_ssl_context() {
    {
        NetState& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        s.ssl_contexts++;
    }
    auto conns = std::make_shared<int>(0);     // live connections of this context
    return tracked(
        "nn::ssl::sf::ISslContext",
        [] {
            NetState& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (s.ssl_contexts > 0) s.ssl_contexts--;
        },
        [conns](ipc::Service& svc) {
            rows(svc, "ssl", {
                {0, "SetOption", K::Ok},
                {1, "GetOption", K::S32, 0},
                {6, "RemoveServerPki", K::Ok},
                {7, "RemoveClientPki", K::Ok},
                {9, "AddPolicyOid", K::Ok},
                {11, "RemoveCrl", K::Ok},
                {13, "GeneratePrivateKeyAndCert", K::Fail, kSslInvalidOption},
            });
            cmd(svc, "ssl", 2, "CreateConnection", [conns](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push_object(make_ssl_connection(conns));
                return 0;
            });
            cmd(svc, "ssl", 3, "GetConnectionCount", [conns](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push(static_cast<uint32_t>(*conns));
                return 0;
            });
            for (uint32_t id : {4u, 5u, 8u, 10u, 12u}) {
                const char* name = id == 4 ? "ImportServerPki" : id == 5 ? "ImportClientPki" : id == 8 ? "RegisterInternalPki"
                                 : id == 10 ? "ImportCrl" : "ImportClientCertKeyPki";
                cmd(svc, "ssl", id, name, [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                    rsp.push(alloc_pki_id());
                    return 0;
                });
            }
        });
}

std::shared_ptr<ipc::Service> make_ssl_connection(const std::shared_ptr<int>& conns) {
    ++(*conns);
    std::shared_ptr<int> c = conns;
    return tracked(
        "nn::ssl::sf::ISslConnection",
        [c] { --(*c); },
        [](ipc::Service& svc) {
            cmd(svc, "ssl", 1, "SetHostName", [](ipc::Request& req, ipc::Response&) -> uint32_t {
                ask_ssl_host(req);
                return 0;
            });
            rows(svc, "ssl", {
                {0, "SetSocketDescriptor", K::Fail, kSslInvalidSocket},
                {2, "SetVerifyOption", K::Ok},
                {3, "SetIoMode", K::Ok},
                {4, "GetSocketDescriptor", K::S32, 0xFFFFFFFFu},   // -1: none was set
                {5, "GetHostName", K::U32, 0},
                {6, "GetVerifyOption", K::U32, 0},
                {7, "GetIoMode", K::U32, 0},
                {8, "DoHandshake", K::Fail, kSslInvalidSocket},
                {9, "DoHandshakeGetServerCert", K::Fail, kSslInvalidSocket},
                {10, "Read", K::Fail, kSslInvalidSocket},
                {11, "Write", K::Fail, kSslInvalidSocket},
                {12, "Pending", K::S32, 0},
                {13, "Peek", K::Fail, kSslInvalidSocket},
                {14, "Poll", K::U32, 0},
                {15, "GetVerifyCertError", K::Ok},                  // no output: the result is the error (Switchbrew, Lichii)
                {16, "GetNeededServerCertBufferSize", K::U32, 0},
                {17, "SetSessionCacheMode", K::Ok},
                {18, "GetSessionCacheMode", K::U32, 0},
                {19, "FlushSessionCache", K::Ok},
                {20, "SetRenegotiationMode", K::Ok},
                {21, "GetRenegotiationMode", K::U32, 0},
                {22, "SetOption", K::Ok},
                {23, "GetOption", K::U8, 0},
                {24, "GetVerifyCertErrors", K::Ok},
                {25, "GetCipherInfo", K::Fail, kSslInvalidSocket},
                {26, "SetNextAlpnProto", K::Ok},
                {27, "GetNextAlpnProto", K::Zeros, 0, 8},          // (u32 state, u32 size) = (0, 0)
                {28, "SetDtlsSocketDescriptor", K::Fail, kSslInvalidSocket},
                {29, "GetDtlsHandshakeTimeout", K::U64, 0},
                {30, "SetPrivateOption", K::Ok},
                {31, "SetSrtpCiphers", K::Ok},
                {32, "GetSrtpCipher", K::U32, 0},
                {33, "ExportKeyingMaterial", K::Fail, kSslInvalidSocket},
                {34, "SetIoTimeout", K::Ok},
                {35, "GetIoTimeout", K::U64, 0},
                {36, "GetSessionTicket", K::U32, 0},
                {37, "SetSessionTicket", K::Ok},
            });
        });
}

std::shared_ptr<ipc::Service> make_ssl_service() {
    return make_iface("nn::ssl::sf::ISslService", [](ipc::Service& s) {
        cmd(s, "ssl", 0, "CreateContext", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ssl_context());
            return 0;
        });
        cmd(s, "ssl", 1, "GetContextCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            NetState& n = st();
            std::lock_guard<std::mutex> lk(n.mu);
            rsp.push(static_cast<uint32_t>(n.ssl_contexts));
            return 0;
        });
        rows(s, "ssl", {
            {2, "GetCertificates", K::U32, 0},
            {3, "GetCertificateBufSize", K::U32, 0},
            {4, "DebugIoctl", K::Fail, kSslInvalidOption},
            {5, "SetInterfaceVersion", K::Ok},
            {6, "FlushSessionCache", K::U32, 0},
            {7, "SetDebugOption", K::Ok},
            {8, "GetDebugOption", K::BufZero, 0, 1},
            {9, "ClearTls12FallbackFlag", K::Ok},
            {10, "GetCertificateByIndex", K::Ok},
            {11, "GetTrustedCertificateCount", K::U32, 0},
        });
    });
}

// =============================================================================================================================================
// nim:eca: the shop (ECA) interface. Creation succeeds; the shop objects answer their creation commands, nothing needs the network.
// =============================================================================================================================================

// nn::ec::IShopServiceAsync (Lichii nim/nim.cpp): one shop request. Request completes at once and signals the completion event the
// accessor handed out with the object; the response is "{}" (an empty JSON object: Lichii found the SDK fails on an empty body) and the
// error code 0, so the game sees an empty shop rather than a transfer that never ends. Nothing is sent anywhere.
struct ShopAsync {
    ServiceEvent done;
    std::mutex mu;
    std::string data;
};
std::shared_ptr<ipc::Service> make_shop_async(const std::shared_ptr<ShopAsync>& a) {
    return make_iface("nn::ec::IShopServiceAsync", [a](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "Cancel", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        cmd(s, "nim:eca", 1, "GetSize", [a](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(a->mu);
            rsp.push<uint64_t>(a->data.size());
            return 0;
        });
        cmd(s, "nim:eca", 2, "Read", [a](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint64_t off = req.arg<uint64_t>(0);
            std::lock_guard<std::mutex> lk(a->mu);
            uint64_t n = 0;
            if (off < a->data.size()) n = buf_write(req.out_buf(0), a->data.data() + off, a->data.size() - off);
            rsp.push<uint64_t>(n);
            return 0;
        });
        cmd(s, "nim:eca", 3, "GetErrorCode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        cmd(s, "nim:eca", 4, "Request", [a](ipc::Request&, ipc::Response&) -> uint32_t {
            {
                std::lock_guard<std::mutex> lk(a->mu);
                a->data = "{}";
            }
            a->done.signal();
            return 0;
        });
        cmd(s, "nim:eca", 5, "Prepare", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
    });
}

std::shared_ptr<ipc::Service> make_shop_accessor() {
    return make_iface("nn::ec::IShopServiceAccessor", [](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "CreateAsyncInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            auto a = std::make_shared<ShopAsync>();
            rsp.out_copy.push_back(a->done.new_handle());   // the completion event, then the object (Lichii: copy handle + interface)
            rsp.push_object(make_shop_async(a));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_shop_server() {
    return make_iface("nn::ec::IShopServiceAccessServer", [](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "CreateAccessorInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_shop_accessor());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_nim_eca() {
    return make_iface("nn::nim::detail::IShopServiceAccessServerInterface", [](ipc::Service& s) {
        auto create = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            close_copied(req);
            rsp.push_object(make_shop_server());
            return 0;
        };
        cmd(s, "nim:eca", 0, "CreateServerInterface", create);
        rows(s, "nim:eca", {
            {4, "IsLargeResourceAvailable", K::U8, 0},
        });
        cmd(s, "nim:eca", 5, "CreateServerInterface2", create);
    });
}

// =============================================================================================================================================
// friend: friend:u, friend:v, friend:m, friend:s, friend:a (IServiceCreator). Switchbrew "Friend services". No friends, no network: the
// list commands answer count 0, the network-sending commands are accepted and do nothing (see the report).
// =============================================================================================================================================

struct FriendEvents {
    ServiceEvent completion{true};   // nothing is pending, so the completion is at once
};

std::shared_ptr<ipc::Service> make_friend_service() {
    auto ev = std::make_shared<FriendEvents>();
    return make_iface("nn::friends::detail::ipc::IFriendService", [ev](ipc::Service& s) {
        cmd(s, "friend", 0, "GetCompletionEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->completion.new_handle());
            return 0;
        });
        rows(s, "friend", {
            {1, "Cancel", K::Ok},
            {10100, "GetFriendListIds", K::S32, 0},
            {10101, "GetFriendList", K::S32, 0},
            {10102, "UpdateFriendInfo", K::Ok},
            {10110, "GetFriendProfileImage", K::Ok},
            {10111, "GetFriendProfileImageWithImageSize", K::Ok},
            {10120, "CheckFriendListAvailability", K::U8, 1},
            {10121, "EnsureFriendListAvailable", K::Ok},
            {10200, "SendFriendRequestForApplication", K::Ok},
            {10211, "AddFacedFriendRequestForApplication", K::Ok},
            {10400, "GetBlockedUserListIds", K::S32, 0},
            {10420, "CheckBlockedUserListAvailability", K::U8, 1},
            {10421, "EnsureBlockedUserListAvailable", K::Ok},
            {10500, "GetProfileListLegacyV1", K::S32, 0},
            {10501, "GetProfileListLegacyV2", K::S32, 0},
            {10502, "GetProfileListV1", K::S32, 0},
            {10503, "GetProfileListV2", K::S32, 0},
            {10600, "DeclareOpenOnlinePlaySession", K::Ok},
            {10601, "DeclareCloseOnlinePlaySession", K::Ok},
            {10610, "UpdateUserPresence", K::Ok},
            {10700, "GetPlayHistoryRegistrationKey", K::Ok},
            {10701, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId", K::Ok},
            {10702, "AddPlayHistory", K::Ok},
            {11000, "GetProfileImageUrl", K::Ok},
            {11001, "GetProfileImageUrlV2", K::Ok},
            {20100, "GetFriendCount", K::S32, 0},
            {20101, "GetNewlyFriendCount", K::S32, 0},
            {20102, "GetFriendDetailedInfo", K::Ok},
            {20103, "SyncFriendList", K::Ok},
            {20104, "RequestSyncFriendList", K::Ok},
            {20105, "GetFriendListForViewerV1", K::S32, 0},
            {20106, "UpdateFriendInfoForViewerV1", K::Ok},
            {20107, "GetFriendDetailedInfoV2", K::Ok},
            {20108, "GetFriendListForViewerV2", K::S32, 0},
            {20109, "UpdateFriendInfoForViewerV2", K::Ok},
            {20110, "LoadFriendSettingV1", K::Ok},
            {20111, "LoadFriendSettingV2", K::Ok},
            {20200, "GetReceivedFriendRequestCount", K::S32, 0},
            {20201, "GetFriendRequestListV1", K::S32, 0},
            {20202, "GetFriendRequestListV2", K::S32, 0},
            {20203, "GetFriendRequestReceivedNotificationCount", K::S32, 0},
            {20300, "GetFriendCandidateList", K::S32, 0},
            {20301, "GetNintendoNetworkIdInfo", K::Ok},
            {20400, "GetBlockedUserListV1", K::S32, 0},
            {20401, "SyncBlockedUserList", K::Ok},
            {20402, "GetBlockedUserListV2", K::S32, 0},
            {20500, "GetProfileExtraListV1", K::S32, 0},
            {20501, "GetRelationship", K::Ok},
            {20502, "GetProfileExtraListV2", K::S32, 0},
            {20600, "GetUserPresenceViewV1", K::BufZero, 0, 0x400},   // the view (0xE0 bytes, Lichii) zeroed: offline, no presence
            {20601, "GetUserPresenceViewV2", K::BufZero, 0, 0x400},
            {20700, "GetPlayHistoryListV1", K::S32, 0},
            {20701, "GetPlayHistoryStatistics", K::Ok},
            {20702, "GetPlayHistoryListV2", K::S32, 0},
            {20800, "LoadUserSettingV1", K::Ok},
            {20801, "SyncUserSetting", K::Ok},
            {20802, "LoadUserSettingV2", K::Ok},
            {20900, "RequestListSummaryOverlayNotification", K::Ok},
            {21000, "GetExternalApplicationCatalog", K::Ok},
            {22000, "GetReceivedFriendInvitationListV1", K::S32, 0},
            {22001, "GetReceivedFriendInvitationDetailedInfoV1", K::Ok},
            {22002, "GetReceivedFriendInvitationListV2", K::S32, 0},
            {22003, "GetReceivedFriendInvitationDetailedInfoV2", K::Ok},
            {22010, "GetReceivedFriendInvitationCountCache", K::S32, 0},
            {30100, "DropFriendNewlyFlags", K::Ok},
            {30101, "DeleteFriend", K::Ok},
            {30110, "DropFriendNewlyFlag", K::Ok},
            {30120, "ChangeFriendFavoriteFlag", K::Ok},
            {30121, "ChangeFriendOnlineNotificationFlag", K::Ok},
            {30130, "SetFriendNote", K::Ok},
            {30131, "RequestUploadPendingNote", K::Ok},
            {30190, "RequestSyncLocalUpdates", K::Ok},
            {30200, "SendFriendRequest", K::Ok},
            {30201, "SendFriendRequestWithApplicationInfoV1", K::Ok},
            {30202, "CancelFriendRequest", K::Ok},
            {30203, "AcceptFriendRequest", K::Ok},
            {30204, "RejectFriendRequest", K::Ok},
            {30205, "ReadFriendRequest", K::Ok},
            {30210, "GetFacedFriendRequestRegistrationKey", K::Ok},
            {30211, "AddFacedFriendRequest", K::Ok},
            {30212, "CancelFacedFriendRequest", K::Ok},
            {30213, "GetFacedFriendRequestProfileImage", K::Ok},
            {30214, "GetFacedFriendRequestProfileImageFromPath", K::Ok},
            {30215, "SendFriendRequestWithExternalApplicationCatalogId", K::Ok},
            {30216, "ResendFacedFriendRequest", K::Ok},
            {30217, "SendFriendRequestWithNintendoNetworkIdInfo", K::Ok},
            {30218, "SendFriendRequestWithApplicationInfoV2", K::Ok},
            {30300, "GetSnsAccountLinkPageUrl", K::Ok},
            {30301, "UnlinkSnsAccount", K::Ok},
            {30400, "BlockUser", K::Ok},
            {30401, "BlockUserWithApplicationInfoV1", K::Ok},
            {30402, "UnblockUser", K::Ok},
            {30403, "BlockUserWithApplicationInfoV2", K::Ok},
            {30500, "GetProfileExtraFromFriendCodeV1", K::Ok},
            {30501, "GetProfileExtraFromFriendCodeV2", K::Ok},
            {30700, "DeletePlayHistory", K::Ok},
            {30701, "AddPlayHistoryWithApplication", K::Ok},
            {30810, "ChangePresencePermission", K::Ok},
            {30811, "ChangeFriendRequestReception", K::Ok},
            {30812, "ChangePlayLogPermission", K::Ok},
            {30820, "IssueFriendCode", K::Ok},
            {30830, "ClearPlayLog", K::Ok},
            {30900, "SendFriendInvitationV1", K::Ok},
            {30901, "SendFriendInvitationV2", K::Ok},
            {30910, "ReadFriendInvitation", K::Ok},
            {30911, "ReadAllFriendInvitations", K::Ok},
            {31000, "OpenUser", K::Ok},
            {31001, "OpenUserWithPlayLogPolicy", K::Ok},
            {40100, "DeleteFriendListCache", K::Ok},
            {40400, "DeleteBlockedUserListCache", K::Ok},
            {49900, "DeleteNetworkServiceAccountCache", K::Ok},
        });
    });
}

std::shared_ptr<ipc::Service> make_friend_notification() {
    auto ev = std::make_shared<ServiceEvent>();   // never signalled: no notifications
    return make_iface("nn::friends::detail::ipc::INotificationService", [ev](ipc::Service& s) {
        cmd(s, "friend", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
        rows(s, "friend", {
            {1, "Clear", K::Ok},
            {2, "Pop", K::Fail, res(124, 15)},    // nothing is queued: ResultNoNotifications (Lichii), not an all-zero notification
        });
    });
}

std::shared_ptr<ipc::Service> make_friend_creator() {
    return make_iface("nn::friends::detail::ipc::IServiceCreator", [](ipc::Service& s) {
        cmd(s, "friend", 0, "CreateFriendService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_friend_service());
            return 0;
        });
        cmd(s, "friend", 1, "CreateNotificationService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_friend_notification());
            return 0;
        });
        cmd(s, "friend", 2, "CreateDaemonSuspendSessionService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("nn::friends::detail::ipc::IDaemonSuspendSessionService", [](ipc::Service&) {}));
            return 0;
        });
    });
}

// =============================================================================================================================================
// prepo: prepo:u, prepo:a, prepo:a2 (IPrepoService). Play reports are accepted and dropped. ScarletNX misc.swipc lists 10105, 10200, 10400.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_prepo() {
    return make_iface("nn::prepo::detail::ipc::IPrepoService", [](ipc::Service& s) {
        // every form of SaveReport (Lichii's prepo.cpp ids: 10100..10105 the older layouts, 10106 / 10107 the current one, which SDK
        // 21+ titles send: Ocarina of Time) is accepted and dropped
        rows(s, "prepo", {
            {10100, "SaveReportOld", K::Ok},
            {10101, "SaveReportWithUserOld", K::Ok},
            {10102, "SaveReportOld2", K::Ok},
            {10103, "SaveReportWithUserOld2", K::Ok},
            {10104, "SaveReportOld3", K::Ok},
            {10105, "SaveReportWithUserOld3", K::Ok},
            {10106, "SaveReport", K::Ok},
            {10107, "SaveReportWithUser", K::Ok},
            {10200, "RequestImmediateTransmission", K::Ok},
            {10400, "GetSystemSessionId", K::U64, 0},
        });
    });
}

// =============================================================================================================================================
// pctl: pctl, pctl:s, pctl:r, pctl:a (IParentalControlServiceFactory). No restriction is set: everything answers "allowed".
// =============================================================================================================================================

struct PctlEvents {
    ServiceEvent pin_changed;
    ServiceEvent sync;
    ServiceEvent play_timer;
    ServiceEvent unlinked;
};

std::shared_ptr<ipc::Service> make_pctl_service() {
    auto ev = std::make_shared<PctlEvents>();
    return make_iface("nn::pctl::detail::ipc::IParentalControlService", [ev](ipc::Service& s) {
        rows(s, "pctl", {
            {1, "Initialize", K::Ok},
            {1001, "CheckFreeCommunicationPermission", K::Ok},
            {1002, "ConfirmLaunchApplicationPermission", K::Ok},
            {1003, "ConfirmResumeApplicationPermission", K::Ok},
            {1004, "ConfirmSnsPostPermission", K::Ok},
            {1005, "ConfirmSystemSettingsPermission", K::Ok},
            {1006, "IsRestrictionTemporaryUnlocked", K::U8, 0},
            {1007, "RevertRestrictionTemporaryUnlocked", K::Ok},
            {1008, "EnterRestrictedSystemSettings", K::Ok},
            {1009, "LeaveRestrictedSystemSettings", K::Ok},
            {1010, "IsRestrictedSystemSettingsEntered", K::U8, 0},
            {1011, "RevertRestrictedSystemSettingsEntered", K::Ok},
            {1012, "GetRestrictedFeatures", K::U32, 0},
            {1013, "ConfirmStereoVisionPermission", K::Ok},
            {1014, "ConfirmPlayableApplicationVideoOld", K::Ok},
            {1015, "ConfirmPlayableApplicationVideo", K::Ok},
            {1016, "ConfirmShowNewsPermission", K::Ok},
            {1017, "EndFreeCommunication", K::Ok},
            {1018, "IsFreeCommunicationAvailable", K::Ok},          // no output: result 0 is "available" (Switchbrew, Lichii)
            {1019, "ConfirmLaunchApplicationPermission", K::Ok},
            {1020, "ConfirmLaunchSharedApplicationPermission", K::Ok},
            {1021, "TryBeginFreeCommunicationForStreamPlay", K::Ok},
            {1022, "EndFreeCommunicationForStreamPlay", K::Ok},
            {1023, "GetRestrictedFeatureFlagSet", K::U32, 0},
            {1031, "IsRestrictionEnabled", K::U8, 0},
            {1032, "GetSafetyLevel", K::S32, 0},
            {1033, "SetSafetyLevel", K::Ok},
            {1034, "GetSafetyLevelSettings", K::Ok},
            {1035, "GetCurrentSettings", K::Zeros, 0, 3},           // RestrictionSettings: rating age 0, nothing restricted
            {1036, "SetCustomSafetyLevelSettings", K::Ok},
            {1037, "GetDefaultRatingOrganization", K::U32, 0},
            {1038, "SetDefaultRatingOrganization", K::Ok},
            {1039, "GetFreeCommunicationApplicationListCount", K::S32, 0},
            {1042, "AddToFreeCommunicationApplicationList", K::Ok},
            {1043, "DeleteSettings", K::Ok},
            {1044, "GetFreeCommunicationApplicationList", K::S32, 0},
            {1045, "UpdateFreeCommunicationApplicationList", K::Ok},
            {1046, "DisableFeaturesForReset", K::Ok},
            {1047, "NotifyApplicationDownloadStartedOld", K::Ok},
            {1048, "NotifyNetworkProfileCreated", K::Ok},
            {1049, "ResetFreeCommunicationApplicationList", K::Ok},
            {1050, "AddToFreeCommunicationApplicationList", K::Ok},
            {1051, "NotifyApplicationDownloadStarted", K::Ok},
            {1061, "ConfirmStereoVisionRestrictionConfigurable", K::Ok},
            {1062, "GetStereoVisionRestriction", K::U8, 0},
            {1063, "SetStereoVisionRestriction", K::Ok},
            {1064, "ResetConfirmedStereoVisionPermission", K::Ok},
            {1065, "IsStereoVisionPermitted", K::U8, 1},
            {1201, "UnlockRestrictionTemporarily", K::Ok},
            {1202, "UnlockSystemSettingsRestriction", K::Ok},
            {1203, "SetPinCode", K::Ok},
            {1204, "GenerateInquiryCode", K::Ok},
            {1205, "CheckMasterKey", K::Ok},
            {1206, "GetPinCodeLength", K::S32, 0},                  // no PIN set
            {1403, "IsPairingActive", K::U8, 0},
            {1453, "IsPlayTimerEnabled", K::U8, 0},
        });
        cmd(s, "pctl", 1207, "GetPinCodeChangedEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->pin_changed.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1432, "GetSynchronizationEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->sync.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1457, "GetPlayTimerEventToRequestSuspension", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->play_timer.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1473, "GetUnlinkedEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->unlinked.new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_pctl_factory() {
    return make_iface("nn::pctl::detail::ipc::IParentalControlServiceFactory", [](ipc::Service& s) {
        auto create = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_pctl_service());
            return 0;
        };
        cmd(s, "pctl", 0, "CreateService", create);
        cmd(s, "pctl", 1, "CreateServiceWithoutInitialize", create);
    });
}

// =============================================================================================================================================
// bcat: bcat:u, bcat:a (IServiceCreator). No BCAT data for this title and no network: the sync requests finish at once with an empty cache.
// =============================================================================================================================================

struct ProgressEvents {
    ServiceEvent done{true};   // the sync is over
};

std::shared_ptr<ipc::Service> make_bcat_progress() {
    auto ev = std::make_shared<ProgressEvents>();
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheProgressService", [ev](ipc::Service& s) {
        cmd(s, "bcat", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->done.new_handle());
            return 0;
        });
        rows(s, "bcat", {
            {1, "GetImpl", K::BufZero, 0, 0x200},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_notifier() {
    auto ev = std::make_shared<ServiceEvent>();   // the storage is never updated
    return make_iface("nn::bcat::detail::ipc::INotifierService", [ev](ipc::Service& s) {
        cmd(s, "bcat", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_file() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheFileService", [](ipc::Service& s) {
        rows(s, "bcat", {
            {0, "Open", K::Fail, kFsPathNotFound},
            {1, "Read", K::U64, 0},
            {2, "GetSize", K::U64, 0},
            {3, "GetDigest", K::Zeros, 0, 0x10},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_directory() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheDirectoryService", [](ipc::Service& s) {
        rows(s, "bcat", {
            {0, "Open", K::Fail, kFsPathNotFound},
            {1, "Read", K::S32, 0},
            {2, "GetCount", K::S32, 0},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_storage() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheStorageService", [](ipc::Service& s) {
        cmd(s, "bcat", 0, "CreateFileService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_file());
            return 0;
        });
        cmd(s, "bcat", 1, "CreateDirectoryService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_directory());
            return 0;
        });
        rows(s, "bcat", {
            {10, "EnumerateDeliveryCacheDirectory", K::S32, 0},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_service() {
    auto progress = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_bcat_progress());
        return 0;
    };
    return make_iface("nn::bcat::detail::ipc::IBcatService", [progress](ipc::Service& s) {
        cmd(s, "bcat", 10100, "RequestSyncDeliveryCache", progress);
        cmd(s, "bcat", 10101, "RequestSyncDeliveryCacheWithDirectoryName", progress);
        cmd(s, "bcat", 20100, "RequestSyncDeliveryCacheWithApplicationId", progress);
        cmd(s, "bcat", 20101, "RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName", progress);
        cmd(s, "bcat", 20300, "GetDeliveryCacheStorageUpdateNotifier", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_notifier());
            return 0;
        });
        cmd(s, "bcat", 20301, "RequestSuspendDeliveryTask", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("nn::bcat::detail::ipc::IDeliveryTaskSuspensionService", [](ipc::Service&) {}));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_creator() {
    return make_iface("nn::bcat::detail::ipc::IServiceCreator", [](ipc::Service& s) {
        auto bcat = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_service());
            return 0;
        };
        auto storage = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_storage());
            return 0;
        };
        cmd(s, "bcat", 0, "CreateBcatService", bcat);
        cmd(s, "bcat", 1, "CreateDeliveryCacheStorageService", storage);
        cmd(s, "bcat", 2, "CreateDeliveryCacheStorageServiceWithApplicationId", storage);
    });
}

// =============================================================================================================================================
// nsd: nsd:u, nsd:a (IManager). The environment is the retail one (lp1); Resolve returns the name as it was given.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_nsd() {
    return make_iface("nn::nsd::detail::IManager", [](ipc::Service& s) {
        cmd(s, "nsd", 11, "GetEnvironmentIdentifier", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            static const uint8_t kEnv[8] = {'l', 'p', '1', 0, 0, 0, 0, 0};
            buf_write(req.out_buf(0), kEnv, sizeof kEnv);
            return 0;
        });
        rows(s, "nsd", {
            {15, "SetChangeEnvironmentIdentifierDisabled", K::Ok},
            {100, "GetApplicationServerEnvironmentType", K::U8, 1},   // 1 = Lp, the retail servers (Ryujinx Nsd/IManager.cs, Lichii nsd.cpp)
        });
        // Resolve(fqdn_in) -> fqdn_out is the same bytes; ResolveEx answers the same with inner_result 0.
        auto copy_name = [](ipc::Request& req) {
            const ipc::Buffer in = req.in_buf(0);
            const ipc::Buffer out = req.out_buf(0);
            std::vector<uint8_t> tmp(static_cast<size_t>(in.size));
            const uint64_t n = buf_read(in, tmp.data(), in.size);
            buf_write(out, tmp.data(), n);
        };
        cmd(s, "nsd", 20, "Resolve", [copy_name](ipc::Request& req, ipc::Response&) -> uint32_t {
            copy_name(req);
            return 0;
        });
        cmd(s, "nsd", 21, "ResolveEx", [copy_name](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            copy_name(req);
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

// =============================================================================================================================================
// sfdnsres: sfdnsres (IResolver). Every lookup fails: host not found (netdb 1, EAI_NONAME). getaddrinfo errors are FreeBSD's numbers
// (EAI_AGAIN 2, EAI_FAIL 4, EAI_MEMORY 6, EAI_NODATA 7, EAI_NONAME 8: Ryujinx Sockets/Sfdnsres/Types/GaiError.cs, Lichii sockets.h
// GetAddrInfoError) and GetGaiStringErrorRequest answers FreeBSD's gai_strerror text for them.
// =============================================================================================================================================

std::string netdb_text(int32_t e) {
    switch (e) {
        case 1: return "Unknown host";
        case 2: return "Temporary failure in name resolution";
        case 3: return "Non-recoverable failure in name resolution";
        case 4: return "No address associated with name";
        default: return "Unknown error";
    }
}

std::string gai_text(int32_t e) {
    switch (e) {
        case 0: return "Success";
        case 1: return "Address family for hostname not supported";
        case 2: return "Temporary failure in name resolution";
        case 3: return "Invalid value for ai_flags";
        case 4: return "Non-recoverable failure in name resolution";
        case 5: return "ai_family not supported";
        case 6: return "Memory allocation failure";
        case 7: return "No address associated with hostname";
        case 8: return "hostname nor servname provided, or not known";
        case 9: return "servname not supported for ai_socktype";
        case 10: return "ai_socktype not supported";
        case 11: return "System error returned in errno";
        case 12: return "Invalid value for hints";
        case 13: return "Resolved protocol is unknown";
        case 14: return "Argument buffer overflow";
        default: return "Unknown error";
    }
}

void write_text(const ipc::Buffer& out, const std::string& text) {
    std::vector<uint8_t> z(text.size() + 1, 0);
    std::memcpy(z.data(), text.data(), text.size());
    buf_write(out, z.data(), z.size());
}

std::shared_ptr<ipc::Service> make_resolver() {
    return make_iface("nn::socket::resolver::IResolver", [](ipc::Service& s) {
        // GetHostByNameRequest / GetHostByAddrRequest: (netdb_error, bsd_errno, size)
        cmd(s, "sfdnsres", 2, "GetHostByNameRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 3, "GetHostByAddrRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 4, "GetHostStringErrorRequest", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            int32_t e = req.arg<int32_t>(0);
            write_text(req.out_buf(0), netdb_text(e));
            return 0;
        });
        cmd(s, "sfdnsres", 5, "GetGaiStringErrorRequest", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            int32_t e = req.arg<int32_t>(0);
            write_text(req.out_buf(0), gai_text(e));
            return 0;
        });
        // GetAddrInfoRequest: (bsd_errno, gai_error, size)
        cmd(s, "sfdnsres", 6, "GetAddrInfoRequest", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            ask_dns(req);
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<int32_t>(kEaiNoName));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 8, "GetCancelHandleRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(alloc_cancel_handle());
            return 0;
        });
        rows(s, "sfdnsres", {
            {9, "CancelRequest", K::Ok},
        });
        // GetHostByNameRequestWithOptions / GetHostByAddrRequestWithOptions: (size, netdb_error, bsd_errno)
        cmd(s, "sfdnsres", 10, "GetHostByNameRequestWithOptions", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            return 0;
        });
        cmd(s, "sfdnsres", 11, "GetHostByAddrRequestWithOptions", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            return 0;
        });
        // GetAddrInfoRequestWithOptions: (size, gai_error, netdb_error, bsd_errno)
        cmd(s, "sfdnsres", 12, "GetAddrInfoRequestWithOptions", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            ask_dns(req);
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<int32_t>(kEaiNoName));
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        // ResolverSetOptionRequest: (i32 ret, u32 bsd_errno), accepted
        cmd(s, "sfdnsres", 14, "ResolverSetOptionRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<int32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

// =============================================================================================================================================
// ntc: ntc (IStaticService) and IEnsureNetworkClockAvailabilityService. The network clock task fails at once with "network not available"
// (time 1000), or succeeds at once with [net] online_test = on; Cancel makes it "task canceled" (time 1003). The finish event is signalled when the task ends.
// =============================================================================================================================================

struct NtcTask {
    ServiceEvent finish;
    uint32_t result = 0;
};

std::shared_ptr<ipc::Service> make_ntc_task() {
    auto t = std::make_shared<NtcTask>();
    return make_iface("nn::ntc::detail::IEnsureNetworkClockAvailabilityService", [t](ipc::Service& s) {
        cmd(s, "ntc", 0, "StartTask", [t](ipc::Request&, ipc::Response&) -> uint32_t {
            t->result = online_test() ? 0 : kTimeNetworkNotAvailable;
            t->finish.signal();
            return t->result;
        });
        cmd(s, "ntc", 1, "GetFinishNotificationEvent", [t](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(t->finish.new_handle());
            return 0;
        });
        cmd(s, "ntc", 2, "GetResult", [t](ipc::Request&, ipc::Response&) -> uint32_t { return t->result; });
        cmd(s, "ntc", 3, "Cancel", [t](ipc::Request&, ipc::Response&) -> uint32_t {
            t->result = kTimeTaskCanceled;
            t->finish.signal();
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_ntc_static() {
    return make_iface("nn::ntc::detail::IStaticService", [](ipc::Service& s) {
        cmd(s, "ntc", 0, "CreateEnsureNetworkClockAvailabilityService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ntc_task());
            return 0;
        });
    });
}

// =============================================================================================================================================
// ngct: ngct:u (IService). No word list: Match finds nothing, Filter returns the text as it was sent.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_ngct() {
    return make_iface("nn::ngct::detail::IService", [](ipc::Service& s) {
        rows(s, "ngct", {
            {0, "Match", K::U8, 0},               // b8 matched: false
        });
        cmd(s, "ngct", 1, "Filter", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const ipc::Buffer in = req.in_buf(0);
            const ipc::Buffer out = req.out_buf(0);
            std::vector<uint8_t> tmp(static_cast<size_t>(in.size));
            const uint64_t n = buf_read(in, tmp.data(), in.size);
            buf_write(out, tmp.data(), n);
            return 0;
        });
    });
}

// =============================================================================================================================================
// ngc: ngc:u (nn::ngc::detail::IService), the profanity filter behind nn::ngc::ProfanityFilter. No word list: Check finds nothing and
// Mask returns the text as it was sent, with no word masked. Pokopia's name check (the keyboard's text check) calls Mask (cmd 2, text in
// buffer A, masked text in buffer B) and refuses the name when the call fails: with the generic stub's error every name was "a word or
// character that isn't allowed" and the keyboard reopened forever (fork issue #6).
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_ngc() {
    return make_iface("nn::ngc::detail::IService", [](ipc::Service& s) {
        rows(s, "ngc", {
            {0, "GetContentVersion", K::U32, 1},
            {1, "Check", K::U32, 0},              // u32 mask of the checks that matched: none
            {3, "Reload", K::Ok},
        });
        cmd(s, "ngc", 2, "Mask", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const ipc::Buffer in = req.in_buf(0);
            const ipc::Buffer out = req.out_buf(0);
            std::vector<uint8_t> tmp(static_cast<size_t>(in.size));
            const uint64_t n = buf_read(in, tmp.data(), in.size);
            buf_write(out, tmp.data(), n);
            rsp.push(static_cast<uint32_t>(0));   // words masked
            return 0;
        });
    });
}

// =============================================================================================================================================
// notif: notif:a (INotificationServicesForApplication). No alarms; Initialize accepted.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_notif() {
    return make_iface("nn::notification::INotificationServicesForApplication", [](ipc::Service& s) {
        rows(s, "notif", {
            {520, "ListAlarmSettings", K::S32, 0},
            {1000, "Initialize", K::Ok},
        });
    });
}

}  // namespace

// =============================================================================================================================================
// Registration and reset.
// =============================================================================================================================================

void nifm_set_lan_source(NifmLanSource f) { lan_source().store(f); }

void register_network() {
    const bool offline = config_get_bool("net", "offline", true);
    NXLOG_I("net", "network services: [net] offline=%s (no connection, no friends, no BCAT data in either mode)", offline ? "true" : "false");

    if (online_test()) NXLOG_I("net", "[net] online_test = on: nifm and ntc report online, delivery cache empty, no sockets");

    for (const char* n : {"nifm:u", "nifm:a", "nifm:s"}) add(n, [] { return make_nifm_static(); });
    for (const char* n : {"bsd:u", "bsd:a", "bsd:s"}) add(n, [] { return make_bsd(); });
    add("bsdcfg", [] { return make_iface("nn::bsdsocket::cfg::ServerInterface", [](ipc::Service&) {}); });
    // ssl:s (Switch 2; the game opens it, not ssl) is the same ISslService: CreateContext must hand out a real context object.
    for (const char* n : {"ssl", "ssl:s"}) add(n, [] { return make_ssl_service(); });
    add("nim:eca", [] { return make_nim_eca(); });
    for (const char* n : {"friend:u", "friend:v", "friend:m", "friend:s", "friend:a"}) add(n, [] { return make_friend_creator(); });
    for (const char* n : {"prepo:u", "prepo:a", "prepo:a2"}) add(n, [] { return make_prepo(); });
    for (const char* n : {"pctl", "pctl:s", "pctl:r", "pctl:a"}) add(n, [] { return make_pctl_factory(); });
    for (const char* n : {"bcat:u", "bcat:a"}) add(n, [] { return make_bcat_creator(); });
    add("ldn:u", [] { return make_ldn_user_creator(); });
    for (const char* n : {"nsd:u", "nsd:a"}) add(n, [] { return make_nsd(); });
    add("sfdnsres", [] { return make_resolver(); });
    add("ntc", [] { return make_ntc_static(); });
    add("ngct:u", [] { return make_ngct(); });
    add("ngc:u", [] { return make_ngc(); });
    add("notif:a", [] { return make_notif(); });
    nifm_set_lan_source(&lanroom::source);   // [net] lan = room: a game's own LAN mode over a relay room (services/lan_room.h)
}

void network_reset() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.seen.clear();
    s.calls.clear();
    s.next_client_id = 1;
    s.next_cancel_handle = 1;
    s.next_pki_id = 1;
    s.ssl_contexts = 0;
    lanroom::reset();   // leaves the LAN room, drops its relay connection (services/lan_room.cpp)
    ldn_reset();   // drops the relay connection (services/ldn.cpp)
    vnet::set_current_link(nullptr);
    vsock::stack().reset();
}

}  // namespace nxrt::services
