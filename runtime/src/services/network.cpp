// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch). PokoMac: no host redirection (AnySwitch's net_redirect) and no
// built-in certificates (ssl_builtin); the network profile is named PokoMac.
// Copyright (c) 2026 Chaos192
// The network side of Horizon (S3c) for a console with no connection, no friends and no BCAT data: nifm, bsd, bsdcfg, ssl / ssl:s,
// nim:eca, friend, prepo, pctl, bcat, nsd, sfdnsres, ntc, ngct:u, ngc:u and notif:a; ldn:u is registered here, its objects are ldn.cpp's.
// Command ids, names and layouts: Switchbrew (Network_Interface_services, Sockets_services, SSL_services and the pages of the other
// services); struct sizes, enum values and result codes: the facts Ryujinx (MIT) src/Ryujinx.HLE/HOS/Services (Nifm, Sockets, Ssl, Nim,
// Ngct, Notification, Pctl, Time) and src/Ryujinx.Horizon (Bcat, Friends, Prepo) give; no code.
//
// What is not offline: bsd's sockets are the process socket table (net/vsock.h: loopback TCP, UDP on a local-wireless link, event
// descriptors), or the Pokopia co-op module's virtual sockets with [net] protocol = native (third_party/pokopia-coop, descriptors
// 0x100..); a title with a redirect table (net/redirect.h) gets its bsd, sfdnsres and ssl connection objects wrapped
// (services/net_redirect.cpp) and nifm / ntc report a connection; nifm reports a LAN room's network (services/lan_room.cpp) while the
// game is in one. Everything else answers as a console with no network does.
//
// Answers that differ from Ryujinx on purpose: the first GetRequestState after a Submit answers OnHold (Pikmin 4's BCAT thread needs the
// wait), and GetResult fails offline; bsd EventFd reads its counter first (libnx order); ntc's StartTask signals the finish event when it
// fails too; friends' empty notification queue is 124-15; an event command hands out a new handle onto its event at every call.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bsd_mmsg.h"
#include "coop.h"
#include "lan_room.h"
#include "ldn.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "services_internal.h"
#include "../net/vnet.h"
#include "../net/vsock.h"

static_assert(nxrt::vsock::kMaxSockets <= COOP_FD_BASE, "the socket table's descriptors must stay below the co-op module's");

namespace nxrt::services {

namespace {

// ---- results ----------------------------------------------------------------------------------------------------------------------
const uint32_t kNoConnection = res(110, 300);         // nifm: no internet connection
const uint32_t kNetTimeUnavailable = res(116, 1000);  // ntc / time: the network clock cannot be had
const uint32_t kNetTimeCanceled = res(116, 1003);     // ntc: the task was cancelled
const uint32_t kSslNoSocket = res(123, 106);          // ssl: no socket behind the connection
const uint32_t kSslBadOption = res(123, 126);         // ssl: an option or request it does not take
const uint32_t kNoSuchPath = res(2, 1002);            // fs: the (empty) delivery cache has no such file or directory
const uint32_t kNoNotification = res(124, 15);        // friends: the notification queue is empty

constexpr uint32_t kAskBudget = 50;                   // lines per kind of "ask:" log

using H = ipc::Handler;

// ---- process state ------------------------------------------------------------------------------------------------------------------
struct Net {
    std::mutex mu;
    std::set<std::string> used;                  // "<svc> <id>": the commands that logged their first use
    std::map<std::string, uint32_t> calls;       // per-call log counts (nifm, bcat, ntc)
    uint32_t client_id = 1;                      // nifm IGeneralService client ids
    uint32_t cancel_handle = 1;                  // sfdnsres GetCancelHandleRequest
    uint64_t pki_id = 1;                         // ssl Import* / Register* ids
    uint32_t contexts = 0;                       // live ISslContext objects
};
Net& net() {
    static Net n;
    return n;
}

// Kept for the process's life (network_reset leaves them alone).
std::atomic<NifmLanSource> g_lan_source{nullptr};
std::atomic<uint32_t> g_ask_dns{0}, g_ask_connect{0}, g_ask_ssl{0};
std::mutex g_requirement_mu;
std::set<uint32_t> g_presets_seen;
std::set<std::string> g_requirements_seen;

bool online_test() { const bool on = config_get_bool("net", "online_test", /*def=*/false); return on; }

// Printable text of a guest string: up to `max` bytes, ending at a 0 byte; "?" when a byte is not printable ASCII.
std::string printable(const uint8_t* p, size_t avail, size_t max) {
    std::string t;
    const size_t n = std::min(avail, max);
    for (size_t i = 0; p && i < n && p[i]; ++i) {
        if (p[i] < 0x20 || p[i] > 0x7E) return "?";
        t += static_cast<char>(p[i]);
    }
    return t;
}
std::string buffer_text(const ipc::Buffer& b) { return printable(buf_ptr(b), b.addr ? static_cast<size_t>(b.size) : 0, 255); }
std::string raw_name(const ipc::Request& req, size_t off) {
    return off < req.in_size ? printable(req.in + off, req.in_size - off, 0x20) : std::string();
}

bool spend(std::atomic<uint32_t>& budget) { return budget.fetch_add(1) < kAskBudget; }

// bcat's per-call lines name the directory and file a game asks about.
std::string bcat_detail(const char* name, const ipc::Request& req) {
    std::string d;
    if (std::strcmp(name, "Open") == 0) {
        d += " path=\"" + raw_name(req, 0) + "\"";
        if (req.in_size >= 0x40) d += " file=\"" + raw_name(req, 0x20) + "\"";
    }
    if (std::strstr(name, "DirectoryName")) d += " dir=\"" + raw_name(req, req.in_size >= 0x28 ? 8 : 0) + "\"";
    return d;
}

// Every command's first step: the first-use line once per (service, id) and, for nifm, bcat and ntc, a line for each of the first calls.
void note_call(const char* svc, const ipc::Service* per_call, uint32_t id, const char* name, const ipc::Request& req) {
    std::string key = svc;
    key += ' ';
    key += std::to_string(id);
    bool first = false;
    uint32_t n = 0;
    {
        Net& s = net();
        std::lock_guard<std::mutex> lk(s.mu);
        first = s.used.insert(key).second;
        if (per_call) n = ++s.calls[key + ' ' + per_call->name() + ' ' + name];
    }
    if (first) NXLOG_I("net", "%s cmd %u %s: answered offline (first use)", svc, id, name);
    if (per_call && n <= kAskBudget) {
        const std::string detail = std::strcmp(svc, "bcat") == 0 ? bcat_detail(name, req) : std::string();
        NXLOG_I("net", "ask: %s cmd %u %s (call %u)%s", svc, id, name, n, detail.c_str());
    }
}

// One interface's command table. `svc` is the label of the log lines.
class Cmds {
public:
    Cmds(ipc::Service& s, const char* svc)
        : s_(s), svc_(svc), per_call_(!std::strcmp(svc, "nifm") || !std::strcmp(svc, "bcat") || !std::strcmp(svc, "ntc")) {}

    void on(uint32_t id, const char* name, H h) {
        const ipc::Service* per_call = per_call_ ? &s_ : nullptr;
        const char* svc = svc_;
        s_.add_cmd(id, name, [svc, per_call, id, name, h = std::move(h)](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            note_call(svc, per_call, id, name, req);
            return h(req, rsp);
        });
    }
    void ok(uint32_t id, const char* name) {
        on(id, name, [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
    }
    void fail(uint32_t id, const char* name, uint32_t result) {
        on(id, name, [result](ipc::Request&, ipc::Response&) -> uint32_t { return result; });
    }
    template <class T>
    void give(uint32_t id, const char* name, T v) {
        on(id, name, [v](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<T>(v);
            return 0;
        });
    }
    void zeros(uint32_t id, const char* name, size_t n) {
        on(id, name, [n](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out.insert(rsp.out.end(), n, 0);
            return 0;
        });
    }
    void object(uint32_t id, const char* name, std::function<std::shared_ptr<ipc::Service>()> make) {
        on(id, name, [make = std::move(make)](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make());
            return 0;
        });
    }
    void event(uint32_t id, const char* name, std::shared_ptr<ServiceEvent> e) {
        on(id, name, [e = std::move(e)](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(e->new_handle());
            return 0;
        });
    }

private:
    ipc::Service& s_;
    const char* svc_;
    bool per_call_;
};

// Zeroes the first min(n, size) bytes of a guest buffer.
void clear_buffer(const ipc::Buffer& b, uint64_t n) {
    if (b.addr && b.size) std::memset(buf_mut(b), 0, static_cast<size_t>(std::min(n, b.size)));
}
// Copies an input buffer's bytes into an output buffer, as far as it takes them.
void echo(const ipc::Buffer& in, const ipc::Buffer& out) {
    if (in.addr) buf_write(out, buf_ptr(in), in.size);
}

// ==== nifm ===========================================================================================================================

// The network the game is on, read fresh at each query.
struct NifmNow {
    bool online = false;
    uint32_t address = 0;   // this console's IPv4 address, host order
    uint32_t netmask = 0;
    uint32_t router = 0;    // the default gateway (none on a LAN room)
};
NifmNow nifm_now() {
    // 1. a LAN the game is on (asked first, at every query); 2. the test switch or a redirect table; 3. nothing
    NifmNow now;
    const NifmLanSource lan = g_lan_source.load();
    if (lan != nullptr && lan(&now.address, &now.netmask)) {
        now.online = true;
        return now;
    }
    now = NifmNow{};   // whatever the source left there
    if (online_test()) now = NifmNow{true, 0x0A000002, 0xFFFFFF00, 0x0A000001};   // 10.0.0.2/24 via 10.0.0.1
    return now;
}

void put_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// IpAddressSetting (is_auto, ip, mask, gateway) and DnsSetting (is_auto, two servers): 0x16 bytes, addresses in network order.
constexpr size_t kIpConfigSize = 0x16;
std::array<uint8_t, kIpConfigSize> ip_config(const NifmNow& n) {
    std::array<uint8_t, kIpConfigSize> c{};
    c[0] = 1;
    put_be32(&c[1], n.address);
    put_be32(&c[5], n.netmask);
    put_be32(&c[9], n.router);
    return c;   // DNS: not automatic, no servers
}

// NetworkProfileData: the ip settings, an empty proxy setting, the profile's uuid and name, no wireless settings.
constexpr size_t kProfileSize = 0x17C;
std::array<uint8_t, kProfileSize> network_profile(const NifmNow& n) {
    std::array<uint8_t, kProfileSize> p{};
    const auto c = ip_config(n);
    std::memcpy(p.data(), c.data(), c.size());
    static const uint8_t uuid[16] = {'P', 'o', 'k', 'o', 'M', 'a', 'c', 0, 0, 'N', 'E', 'T', 1, 0, 0, 1};
    std::memcpy(p.data() + 0xC2, uuid, sizeof uuid);
    std::memcpy(p.data() + 0xD2, "PokoMac", 7);
    return p;
}

// ---- IScanRequest ----------------------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_scan_request() {
    auto ev = std::make_shared<ServiceEvent>(false);
    return make_iface("nn::nifm::detail::IScanRequest", [ev](ipc::Service& s) {
        Cmds t(s, "nifm");
        t.ok(0, "Submit");
        t.give<uint8_t>(1, "IsProcessing", 0);
        t.ok(2, "GetResult");
        t.event(3, "GetSystemEventReadableHandle", ev);
        t.ok(4, "SetChannels");
        t.ok(5, "Cancel");
    });
}

// ---- IRequest --------------------------------------------------------------------------------------------------------------------------
// A submitted request is on hold until the next GetRequestState, which fires both state events; the query after that answers the
// settled state. Settling goes by queries, not by time (Hades waits at most 2,000 of them).
constexpr uint32_t kStateFree = 1, kStateOnHold = 2, kStateAccepted = 3;
class RequestEval {
public:
    // Submit: the network is looked at (a LAN source joins its room here, as a console connects on a submission); on hold from now.
    void submit() {
        (void)nifm_now();
        set_pending(true);
    }
    // GetRequestState: OnHold once after a submission (both state events fire), else what the network settles to.
    uint32_t query() {
        const NifmNow now = nifm_now();
        if (!set_pending(false)) return now.online ? kStateAccepted : kStateFree;
        for (ServiceEvent* e : {a.get(), b.get()}) e->signal();
        return kStateOnHold;
    }
    // GetResult settles the request too.
    uint32_t result() {
        const uint32_t r = nifm_now().online ? 0 : kNoConnection;
        set_pending(false);
        return r;
    }
    void cancel() { set_pending(false); }

    std::shared_ptr<ServiceEvent> a = std::make_shared<ServiceEvent>(false), b = std::make_shared<ServiceEvent>(false);

private:
    bool set_pending(bool v) {   // the old value (the request objects are used from several threads)
        std::lock_guard<std::mutex> lk(mu_);
        return std::exchange(pending_, v);
    }
    std::mutex mu_;
    bool pending_ = false;
};

std::shared_ptr<ipc::Service> make_request() {
    auto r = std::make_shared<RequestEval>();
    return make_iface("nn::nifm::detail::IRequest", [r](ipc::Service& s) {
        Cmds t(s, "nifm");
        t.on(0, "GetRequestState", [r](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(r->query());
            return 0;
        });
        t.on(1, "GetResult", [r](ipc::Request&, ipc::Response&) -> uint32_t { return r->result(); });
        t.on(2, "GetSystemEventReadableHandles", [r](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(r->a->new_handle());
            rsp.out_copy.push_back(r->b->new_handle());
            return 0;
        });
        t.on(3, "Cancel", [r](ipc::Request&, ipc::Response&) -> uint32_t {
            r->cancel();
            return 0;
        });
        t.on(4, "Submit", [r](ipc::Request&, ipc::Response&) -> uint32_t {
            r->submit();
            return 0;
        });
        t.on(5, "SetRequirement", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            uint8_t head[16] = {};
            if (req.in) std::memcpy(head, req.in, std::min<size_t>(req.in_size, sizeof head));
            static const char* hex = "0123456789abcdef";
            std::string h;
            for (uint8_t c : head) {
                h += hex[c >> 4];
                h += hex[c & 15];
            }
            bool fresh;
            {
                std::lock_guard<std::mutex> lk(g_requirement_mu);
                fresh = g_requirements_seen.insert(h).second;
            }
            if (fresh) NXLOG_I("net", "nifm: a request with requirement %s...", h.c_str());
            return 0;
        });
        t.on(6, "SetRequirementPreset", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint32_t preset = req.arg<uint32_t>(0);
            bool fresh;
            {
                std::lock_guard<std::mutex> lk(g_requirement_mu);
                fresh = g_presets_seen.insert(preset).second;
            }
            if (fresh) NXLOG_I("net", "nifm: a request with requirement preset %u", preset);
            return 0;
        });
        t.ok(8, "SetPriority");
        t.ok(9, "SetNetworkProfileId");
        t.ok(10, "SetRejectable");
        t.ok(11, "SetConnectionConfirmationOption");
        t.ok(12, "SetPersistent");
        t.ok(13, "SetInstant");
        t.ok(14, "SetSustainable");
        t.ok(15, "SetRawPriority");
        t.ok(16, "SetGreedy");
        t.ok(17, "SetSharable");
        t.ok(18, "SetRequirementByRevision");
        t.zeros(19, "GetRequirement", 0x24);
        t.give<uint32_t>(20, "GetRevision", 0);
        t.on(21, "GetAppletInfo", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0xE);
            rsp.push<uint32_t>(0);
            rsp.push<uint32_t>(0);
            return 0;
        });
        t.ok(22, "GetAdditionalInfo");
        t.ok(23, "SetKeptInSleep");
        t.ok(24, "RegisterSocketDescriptor");
        t.ok(25, "UnregisterSocketDescriptor");
        t.give<uint32_t>(26, "GetNetworkAccessStatus", 0);
    });
}

// ---- IGeneralService -----------------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_general() {
    uint32_t client_id;
    {
        Net& n = net();
        std::lock_guard<std::mutex> lk(n.mu);
        client_id = n.client_id++;
    }
    auto telemetry = std::make_shared<ServiceEvent>(false);
    return make_iface("nn::nifm::detail::IGeneralService", [client_id, telemetry](ipc::Service& s) {
        Cmds t(s, "nifm");
        t.on(1, "GetClientId", [client_id](ipc::Request& req, ipc::Response&) -> uint32_t {
            buf_write(req.out_buf(0), &client_id, sizeof client_id);
            return 0;
        });
        t.object(2, "CreateScanRequest", make_scan_request);
        t.object(4, "CreateRequest", make_request);
        t.on(5, "GetCurrentNetworkProfile", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const NifmNow n = nifm_now();
            if (!n.online) return kNoConnection;
            const auto p = network_profile(n);
            buf_write(req.out_buf(0), p.data(), p.size());
            return 0;
        });
        t.fail(8, "GetNetworkProfile", kNoConnection);
        t.give<int32_t>(6, "EnumerateNetworkInterfaces", 0);
        t.give<int32_t>(7, "EnumerateNetworkProfiles", 0);
        t.zeros(9, "SetNetworkProfile", 16);
        t.ok(10, "RemoveNetworkProfile");
        t.give<int32_t>(11, "GetScanDataOld", 0);
        t.on(12, "GetCurrentIpAddress", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const NifmNow n = nifm_now();
            if (!n.online) return kNoConnection;
            uint8_t ip[4];
            put_be32(ip, n.address);
            rsp.out.insert(rsp.out.end(), ip, ip + 4);
            return 0;
        });
        t.fail(13, "GetCurrentAccessPointOld", kNoConnection);
        t.fail(14, "CreateTemporaryNetworkProfile", kNoConnection);
        t.on(15, "GetCurrentIpConfigInfo", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const NifmNow n = nifm_now();
            if (!n.online) return kNoConnection;
            const auto c = ip_config(n);
            rsp.out.insert(rsp.out.end(), c.begin(), c.end());
            return 0;
        });
        t.ok(16, "SetWirelessCommunicationEnabled");
        t.give<uint8_t>(17, "IsWirelessCommunicationEnabled", 1);
        t.on(18, "GetInternetConnectionStatus", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            if (!nifm_now().online) return kNoConnection;
            rsp.push<uint8_t>(1);   // type: WiFi
            rsp.push<uint8_t>(3);   // strength
            rsp.push<uint8_t>(4);   // state: Connected
            return 0;
        });
        t.ok(19, "SetEthernetCommunicationEnabled");
        t.give<uint8_t>(20, "IsEthernetCommunicationEnabled", 0);
        const H accepted = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(nifm_now().online ? 1 : 0);
            return 0;
        };
        t.on(21, "IsAnyInternetRequestAccepted", accepted);
        t.on(22, "IsAnyForegroundRequestAccepted", accepted);
        t.ok(23, "PutToSleep");
        t.ok(24, "WakeUp");
        t.give<uint32_t>(25, "GetSsidListVersion", 0);
        t.ok(26, "SetExclusiveClient");
        t.zeros(27, "GetDefaultIpSetting", 0xD);
        t.ok(28, "SetDefaultIpSetting");
        t.ok(29, "SetWirelessCommunicationEnabledForTest");
        t.ok(30, "SetEthernetCommunicationEnabledForTest");
        t.event(31, "GetTelemetorySystemEventReadableHandle", telemetry);
        t.ok(32, "GetTelemetryInfo");
        t.on(33, "ConfirmSystemAvailability", [](ipc::Request&, ipc::Response&) -> uint32_t {
            return nifm_now().online ? 0 : kNoConnection;
        });
        t.ok(34, "SetBackgroundRequestEnabled");
        t.give<int32_t>(35, "GetScanData", 0);
        t.fail(36, "GetCurrentAccessPoint", kNoConnection);
        t.ok(37, "Shutdown");
        t.ok(38, "GetAllowedChannels");
        t.ok(39, "NotifyApplicationSuspended");
        t.ok(40, "SetAcceptableNetworkTypeFlag");
        t.give<uint32_t>(41, "GetAcceptableNetworkTypeFlag", 0);
        t.ok(42, "NotifyConnectionStateChanged");
        t.ok(43, "SetWowlDelayedWakeTime");
        t.give<uint8_t>(44, "IsWiredConnectionAvailable", 0);
        t.give<uint8_t>(45, "IsNetworkEmulationFeatureEnabled", 0);
        t.ok(46, "SelectActiveNetworkEmulationProfileIdForDebug");
        t.give<int32_t>(47, "GetScanData", 0);
        t.ok(48, "ResetActiveNetworkEmulationProfileId");
        t.give<uint32_t>(49, "GetActiveNetworkEmulationProfileId", 0);
        t.give<uint8_t>(50, "IsRewriteFeatureEnabled", 0);
        t.ok(51, "CreateRewriteRule");
        t.ok(52, "DestroyRewriteRule");
        t.give<uint8_t>(53, "IsActiveNetworkEmulationProfileIdSelected", 0);
        t.ok(54, "SelectDefaultNetworkEmulationProfileId");
        t.give<uint32_t>(55, "GetDefaultNetworkEmulationProfileId", 0);
        t.fail(56, "GetNetworkEmulationProfile", kNoConnection);
        t.ok(57, "SetWowlTcpKeepAliveTimeout");
        t.zeros(101, "Cmd101", 0x80);   // SDK 21+ network setup: must succeed
    });
}

std::shared_ptr<ipc::Service> make_nifm() {
    return make_iface("nn::nifm::detail::IStaticService", [](ipc::Service& s) {
        Cmds t(s, "nifm");
        t.object(4, "CreateGeneralServiceOld", make_general);
        t.object(5, "CreateGeneralService", make_general);
    });
}

// ==== bsd ============================================================================================================================
namespace vs = ::nxrt::vsock;

void put_ret(ipc::Response& rsp, vs::Ret r) {
    rsp.push<int32_t>(r.ret);
    rsp.push<uint32_t>(r.err);
}
void put_ret(ipc::Response& rsp, vs::Ret r, uint32_t length) {
    put_ret(rsp, r);
    rsp.push<uint32_t>(length);
}

// The co-op node when fd is one of its virtual descriptors, else null.
coop_node* coop_owner(int32_t fd) {
    if (fd < COOP_FD_BASE) return nullptr;   // the table's numbers (and every negative one) are never the module's
    coop_node* const node = ldn_native_node();
    if (node == nullptr || !coop_vnet_is(node, fd)) return nullptr;
    return node;
}

// The co-op module's FreeBSD errno numbers as bsd's Linux ones: the numbers that differ between the two (the rest are equal).
uint32_t linux_errno(uint32_t freebsd) {
    static const std::pair<uint32_t, uint32_t> differ[] = {
        {COOP_EAGAIN, vs::kEagain},           {39, vs::kEdestaddrreq} /* FreeBSD EDESTADDRREQ */,
        {COOP_EMSGSIZE, vs::kEmsgsize},       {COOP_ENOPROTOOPT, 92} /* Linux ENOPROTOOPT */,
        {COOP_EADDRINUSE, vs::kEaddrinuse},   {COOP_EADDRNOTAVAIL, vs::kEaddrnotavail},
        {COOP_ENETDOWN, vs::kEnetdown},       {COOP_ENETUNREACH, vs::kEnetunreach}};
    for (const auto& [from, to] : differ)
        if (from == freebsd) return to;
    return freebsd;
}
vs::Ret coop_ret(int64_t r) {
    if (r >= 0) return {static_cast<int32_t>(r), 0};
    return {-1, linux_errno(static_cast<uint32_t>(-r))};
}
constexpr vs::Ret kNetDown{-1, vs::kEnetdown};
constexpr vs::Ret kNotSupported{-1, vs::kEopnotsupp};

// sockaddr_in in FreeBSD's form: {u8 len 16, u8 family 2, be16 port, be32 address, 8 zero bytes}.
constexpr size_t kSockaddrSize = 16;
// 0 and the address, or the errno of an address that is too short or not AF_INET.
uint32_t read_sockaddr(const ipc::Buffer& b, vs::Addr* a) {
    uint8_t sa[kSockaddrSize] = {};
    if (buf_read(b, sa, sizeof sa) < 8) return vs::kEinval;
    if (sa[1] != vs::kAfInet) return vs::kEafnosupport;
    a->port = static_cast<uint16_t>(sa[2] << 8 | sa[3]);
    a->ip = uint32_t{sa[4]} << 24 | uint32_t{sa[5]} << 16 | uint32_t{sa[6]} << 8 | sa[7];
    return 0;
}
// A malformed address on a descriptor the table does not know is the descriptor's error.
vs::Ret bad_address(int32_t fd, uint32_t err) { return {-1, vs::stack().is_socket(fd) ? err : vs::kEbadf}; }
// The sockaddr a call answered, when it succeeded: the bytes written (the length the guest gets).
uint32_t write_sockaddr(const ipc::Buffer& b, vs::Ret r, vs::Addr a) {
    if (r.err) return 0;   // a failed call leaves the buffer alone
    std::array<uint8_t, kSockaddrSize> out{};
    out[0] = static_cast<uint8_t>(kSockaddrSize);
    out[1] = static_cast<uint8_t>(vs::kAfInet);
    out[2] = static_cast<uint8_t>(a.port >> 8);
    out[3] = static_cast<uint8_t>(a.port & 0xFF);
    put_be32(&out[4], a.ip);
    const uint64_t written = buf_write(b, out.data(), out.size());
    return static_cast<uint32_t>(written);
}

void ask_connect(vs::Addr a) {
    if (!spend(g_ask_connect)) return;
    NXLOG_I("net", "ask: connect %u.%u.%u.%u:%u", a.ip >> 24, (a.ip >> 16) & 0xFF, (a.ip >> 8) & 0xFF, a.ip & 0xFF, a.port);
}

// Receives on a co-op descriptor; `from` (16 bytes) gets the sender's sockaddr, *from_len its length.
vs::Ret coop_recv(coop_node* n, int32_t fd, const ipc::Buffer& out, int flags, uint8_t* from, uint32_t* from_len) {
    *from_len = kSockaddrSize;
    return coop_ret(coop_vnet_recvfrom(n, fd, buf_mut(out), out.addr ? out.size : 0, flags, from, from_len));
}
vs::Ret coop_send(coop_node* n, int32_t fd, const ipc::Buffer& in, int flags, const uint8_t* to, uint32_t to_len) {
    return coop_ret(coop_vnet_sendto(n, fd, buf_ptr(in), in.addr ? in.size : 0, flags, to, to_len));
}

// 6 Poll {u32 nfds, s32 timeout ms}: A = nfds PollFd entries, B = the same written back with revents.
constexpr uint32_t kPollMax = 4096;
uint32_t bsd_poll(ipc::Request& req, ipc::Response& rsp) {
    static_assert(sizeof(vs::PollFd) == 8 && sizeof(coop_pollfd) == 8, "PollFd entries are 8 bytes");
    const uint32_t nfds = req.arg<uint32_t>(0);
    const int32_t timeout = req.arg<int32_t>(4);
    std::vector<vs::PollFd> fds;
    bool complete = false;
    if (nfds <= kPollMax) {
        fds.resize(nfds);
        complete = buf_read(req.in_buf(0), fds.data(), uint64_t{nfds} * 8) == uint64_t{nfds} * 8;
    }
    coop_node* node = ldn_native_node();
    bool coop = node && nfds >= 1 && complete;
    if (coop) {
        bool any = false;
        for (const vs::PollFd& f : fds) {
            if (f.fd < 0) continue;
            any = true;
            if (!(f.fd >= COOP_FD_BASE && coop_vnet_is(node, f.fd))) coop = false;
        }
        coop = coop && any;
    }
    vs::Ret r;
    if (coop) {
        std::vector<coop_pollfd> c(nfds);
        std::memcpy(c.data(), fds.data(), uint64_t{nfds} * 8);
        r = coop_ret(coop_vnet_poll(node, c.data(), nfds, timeout));
        std::memcpy(fds.data(), c.data(), uint64_t{nfds} * 8);
    } else if (!complete) {
        r = {-1, vs::kEinval};
    } else {
        r = vs::stack().poll(fds.data(), nfds, timeout);
    }
    put_ret(rsp, r);
    if (r.err == 0) buf_write(req.out_buf(0), fds.data(), uint64_t{nfds} * 8);
    return 0;
}

// 5 Select {s32 nfds, pad, s64 tv_sec, s64 tv_usec, u8 no_timeout}: A/X 0..2 = read, write, except fd_sets (1024 bits), B/C 0..2 back.
constexpr size_t kFdSetBytes = 128;
uint32_t bsd_select(ipc::Request& req, ipc::Response& rsp) {
    const int32_t nfds = req.arg<int32_t>(0);
    uint64_t sets[3][kFdSetBytes / 8] = {};
    uint64_t* set[3] = {};
    for (size_t i = 0; i < 3; ++i) {   // a set is there when its buffer is (address and size); the rest of a short one stays zero
        const ipc::Buffer in = req.in_buf(i);
        if (in.addr != 0 && in.size != 0 && buf_read(in, sets[i], kFdSetBytes) > 0) set[i] = sets[i];
    }
    int32_t timeout = -1;
    if (!req.arg<uint8_t>(0x18)) {
        const int64_t sec = req.arg<int64_t>(8), usec = req.arg<int64_t>(0x10);
        int64_t ms = static_cast<int64_t>(static_cast<uint64_t>(sec) * 1000u + static_cast<uint64_t>(usec / 1000));
        if (ms > 0x7FFFFFFF) ms = 0x7FFFFFFF;
        timeout = static_cast<int32_t>(ms);
    }
    coop_node* node = ldn_native_node();
    bool any = false, all = true;
    if (node) {
        const int32_t limit = std::min<int32_t>(nfds, 1024);
        for (size_t i = 0; i < 3; ++i) {
            if (!set[i]) continue;
            for (int32_t fd = 0; fd < limit; ++fd) {
                if (!(set[i][fd / 64] >> (fd % 64) & 1)) continue;
                any = true;
                if (!(fd >= COOP_FD_BASE && coop_vnet_is(node, fd))) all = false;
            }
        }
    }
    const vs::Ret r = node && any && all ? coop_ret(coop_vnet_select(node, nfds, set[0], set[1], set[2], timeout))
                                         : vs::stack().select(nfds, set[0], set[1], set[2], timeout);
    put_ret(rsp, r);
    if (r.err == 0)
        for (size_t i = 0; i < 3; ++i)
            if (set[i]) buf_write(req.out_buf(i), set[i], kFdSetBytes);
    return 0;
}

// A command on one descriptor (raw s32 fd at 0): `table` answers for the socket table's, `coop` for the co-op module's.
using FdFn = std::function<void(ipc::Request&, ipc::Response&, int32_t fd)>;
using CoopFn = std::function<void(ipc::Request&, ipc::Response&, coop_node* n, int32_t fd)>;
H per_fd(FdFn table, CoopFn coop) {
    return [table = std::move(table), coop = std::move(coop)](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int32_t fd = req.arg<int32_t>(0);
        if (coop_node* n = coop_owner(fd)) coop(req, rsp, n, fd);
        else table(req, rsp, fd);
        return 0;
    };
}

std::shared_ptr<ipc::Service> make_bsd() {
    return make_iface("nn::socket::sf::IClient", [](ipc::Service& s) {
        Cmds t(s, "bsd");
        t.give<uint32_t>(0, "RegisterClient", 0);   // the transfer memory is not used
        t.ok(1, "StartMonitoring");
        const H socket = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int32_t domain = req.arg<int32_t>(0), type = req.arg<int32_t>(4), protocol = req.arg<int32_t>(8);
            coop_node* n = ldn_native_node();
            if (n && domain == vs::kAfInet && (type & 0xFFFF) == vs::kSockDgram && coop_ldn_state(n) != COOP_LS_NONE)
                put_ret(rsp, coop_ret(coop_vnet_socket(n, domain, type, protocol)));
            else
                put_ret(rsp, vs::stack().socket(domain, type, protocol));
            return 0;
        };
        t.on(2, "Socket", socket);
        t.on(3, "SocketExempt", socket);
        t.on(4, "Open", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            put_ret(rsp, kNetDown);
            return 0;
        });
        t.on(5, "Select", bsd_select);
        t.on(6, "Poll", bsd_poll);
        t.on(7, "Sysctl", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            put_ret(rsp, kNetDown, 0);
            return 0;
        });
        t.on(8, "Recv", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                const ipc::Buffer o = req.out_buf(0);
                put_ret(rsp, vs::stack().recvfrom(fd, buf_mut(o), o.size, req.arg<int32_t>(4), nullptr));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                uint8_t from[kSockaddrSize];
                uint32_t len;
                put_ret(rsp, coop_recv(n, fd, req.out_buf(0), req.arg<int32_t>(4), from, &len));
            }));
        t.on(9, "RecvFrom", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                const ipc::Buffer o = req.out_buf(0);
                vs::Addr from;
                const vs::Ret r = vs::stack().recvfrom(fd, buf_mut(o), o.size, req.arg<int32_t>(4), &from);
                put_ret(rsp, r, write_sockaddr(req.out_buf(1), r, from));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                uint8_t from[kSockaddrSize] = {};
                uint32_t len = 0;
                const vs::Ret r = coop_recv(n, fd, req.out_buf(0), req.arg<int32_t>(4), from, &len);
                const uint32_t wrote = r.err ? 0 : static_cast<uint32_t>(buf_write(req.out_buf(1), from, std::min<uint32_t>(len, kSockaddrSize)));
                put_ret(rsp, r, wrote);
            }));
        t.on(10, "Send", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                const ipc::Buffer in = req.in_buf(0);
                put_ret(rsp, vs::stack().sendto(fd, buf_ptr(in), in.size, req.arg<int32_t>(4), nullptr));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                put_ret(rsp, coop_send(n, fd, req.in_buf(0), req.arg<int32_t>(4), nullptr, 0));
            }));
        t.on(11, "SendTo", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                vs::Addr to;
                if (const uint32_t e = read_sockaddr(req.in_buf(1), &to)) {
                    put_ret(rsp, bad_address(fd, e));
                    return;
                }
                ask_connect(to);
                const ipc::Buffer in = req.in_buf(0);
                put_ret(rsp, vs::stack().sendto(fd, buf_ptr(in), in.size, req.arg<int32_t>(4), &to));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                uint8_t to[kSockaddrSize] = {};
                const uint32_t len = static_cast<uint32_t>(buf_read(req.in_buf(1), to, sizeof to));
                put_ret(rsp, coop_send(n, fd, req.in_buf(0), req.arg<int32_t>(4), len ? to : nullptr, len));
            }));
        t.on(12, "Accept", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                vs::Addr from;
                const vs::Ret r = vs::stack().accept(fd, &from);
                put_ret(rsp, r, write_sockaddr(req.out_buf(0), r, from));
            },
            [](ipc::Request&, ipc::Response& rsp, coop_node*, int32_t) { put_ret(rsp, kNotSupported, 0); }));
        t.on(13, "Bind", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                vs::Addr a;
                if (const uint32_t e = read_sockaddr(req.in_buf(0), &a)) put_ret(rsp, bad_address(fd, e));
                else put_ret(rsp, vs::stack().bind(fd, a));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                uint8_t sa[kSockaddrSize] = {};
                const uint32_t len = static_cast<uint32_t>(buf_read(req.in_buf(0), sa, sizeof sa));
                put_ret(rsp, coop_ret(coop_vnet_bind(n, fd, sa, len)));
            }));
        t.on(14, "Connect", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int32_t fd = req.arg<int32_t>(0);
            vs::Addr a;
            const uint32_t e = read_sockaddr(req.in_buf(0), &a);
            if (!e && (a.ip >> 24) != 127) ask_connect(a);
            if (coop_owner(fd)) put_ret(rsp, kNetDown);
            else if (e) put_ret(rsp, bad_address(fd, e));
            else put_ret(rsp, vs::stack().connect(fd, a));
            return 0;
        });
        t.on(15, "GetPeerName", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                vs::Addr a;
                const vs::Ret r = vs::stack().getpeername(fd, &a);
                put_ret(rsp, r, write_sockaddr(req.out_buf(0), r, a));
            },
            [](ipc::Request&, ipc::Response& rsp, coop_node*, int32_t) { put_ret(rsp, kNetDown, 0); }));
        t.on(16, "GetSockName", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                vs::Addr a;
                const vs::Ret r = vs::stack().getsockname(fd, &a);
                put_ret(rsp, r, write_sockaddr(req.out_buf(0), r, a));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                uint8_t sa[kSockaddrSize] = {};
                uint32_t len = kSockaddrSize;
                const vs::Ret r = coop_ret(coop_vnet_getsockname(n, fd, sa, &len));
                const uint32_t wrote = r.err ? 0 : static_cast<uint32_t>(buf_write(req.out_buf(0), sa, std::min<uint32_t>(len, kSockaddrSize)));
                put_ret(rsp, r, wrote);
            }));
        t.on(17, "GetSockOpt", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                const ipc::Buffer o = req.out_buf(0);
                size_t n = static_cast<size_t>(o.size);
                const vs::Ret r = vs::stack().getsockopt(fd, req.arg<int32_t>(4), req.arg<int32_t>(8), buf_mut(o), &n);
                put_ret(rsp, r, r.err ? 0 : static_cast<uint32_t>(n));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                const ipc::Buffer o = req.out_buf(0);
                std::array<uint8_t, 256> value{};                                  // the module's answer, at most 256 bytes
                const uint32_t room = static_cast<uint32_t>(std::min<uint64_t>(o.size, value.size()));
                uint32_t given = room;
                const vs::Ret r = coop_ret(coop_vnet_getsockopt(n, fd, req.arg<int32_t>(4), req.arg<int32_t>(8), room ? value.data() : nullptr, &given));
                const uint64_t wrote = r.err ? 0 : buf_write(o, value.data(), std::min(given, room));
                put_ret(rsp, r, static_cast<uint32_t>(wrote));
            }));
        t.on(18, "Listen", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) { put_ret(rsp, vs::stack().listen(fd, req.arg<int32_t>(4))); },
            [](ipc::Request&, ipc::Response& rsp, coop_node*, int32_t) { put_ret(rsp, kNotSupported); }));
        t.on(19, "Ioctl", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            put_ret(rsp, kNetDown);
            return 0;
        });
        t.on(20, "Fcntl", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                put_ret(rsp, vs::stack().fcntl(fd, req.arg<int32_t>(4), req.arg<int32_t>(8)));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                constexpr int kFreeBsdNonblock = 4;   // O_NONBLOCK in the module's (FreeBSD) numbering; bsd's guest uses Linux's 0x800
                const int cmd = req.arg<int32_t>(4);
                int arg = req.arg<int32_t>(8);
                if (cmd == vs::kFSetFl && (arg & vs::kONonblock)) arg |= kFreeBsdNonblock;
                vs::Ret r = coop_ret(coop_vnet_fcntl(n, fd, cmd, arg));
                if (cmd == vs::kFGetFl && !r.err && (r.ret & kFreeBsdNonblock)) r.ret |= vs::kONonblock;
                put_ret(rsp, r);
            }));
        t.on(21, "SetSockOpt", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                const ipc::Buffer in = req.in_buf(0);
                put_ret(rsp, vs::stack().setsockopt(fd, req.arg<int32_t>(4), req.arg<int32_t>(8), buf_ptr(in), in.size));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                const ipc::Buffer in = req.in_buf(0);
                put_ret(rsp, coop_ret(coop_vnet_setsockopt(n, fd, req.arg<int32_t>(4), req.arg<int32_t>(8), buf_ptr(in),
                                                           in.addr ? static_cast<uint32_t>(in.size) : 0)));
            }));
        t.on(22, "Shutdown", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) { put_ret(rsp, vs::stack().shutdown(fd, req.arg<int32_t>(4))); },
            [](ipc::Request&, ipc::Response& rsp, coop_node*, int32_t) { put_ret(rsp, kNetDown); }));
        t.on(23, "ShutdownAllSockets", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            put_ret(rsp, kNetDown);
            return 0;
        });
        t.on(24, "Write", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                const ipc::Buffer in = req.in_buf(0);
                put_ret(rsp, vs::stack().sendto(fd, buf_ptr(in), in.size, 0, nullptr));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) { put_ret(rsp, coop_send(n, fd, req.in_buf(0), 0, nullptr, 0)); }));
        t.on(25, "Read", per_fd(
            [](ipc::Request& req, ipc::Response& rsp, int32_t fd) {
                const ipc::Buffer o = req.out_buf(0);
                put_ret(rsp, vs::stack().recvfrom(fd, buf_mut(o), o.size, 0, nullptr));
            },
            [](ipc::Request& req, ipc::Response& rsp, coop_node* n, int32_t fd) {
                uint8_t from[kSockaddrSize];
                uint32_t len;
                put_ret(rsp, coop_recv(n, fd, req.out_buf(0), 0, from, &len));
            }));
        t.on(26, "Close", per_fd(
            [](ipc::Request&, ipc::Response& rsp, int32_t fd) { put_ret(rsp, vs::stack().close(fd)); },
            [](ipc::Request&, ipc::Response& rsp, coop_node* n, int32_t fd) { put_ret(rsp, coop_ret(coop_vnet_close(n, fd))); }));
        t.on(27, "DuplicateSocket", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            put_ret(rsp, kNetDown);
            return 0;
        });
        t.zeros(28, "GetResourceStatistics", 8);
        t.on(29, "RecvMMsg", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int32_t fd = req.arg<int32_t>(0);
            if (coop_owner(fd)) {
                put_ret(rsp, kNetDown);
                return 0;
            }
            return mmsg::recvmmsg(req, rsp, mmsg::offline(fd));
        });
        t.on(30, "SendMMsg", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int32_t fd = req.arg<int32_t>(0);
            if (coop_owner(fd)) {
                put_ret(rsp, kNetDown);
                return 0;
            }
            return mmsg::sendmmsg(req, rsp, mmsg::offline(fd));
        });
        t.on(31, "EventFd", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            put_ret(rsp, vs::stack().eventfd(req.arg<uint64_t>(0), req.arg<uint32_t>(8)));
            return 0;
        });
        t.give<uint32_t>(32, "RegisterResourceStatisticsName", 0);
        t.give<uint32_t>(33, "RegisterClientShared", 0);
        // the rest of the table: no network behind any of them
        static const std::pair<uint32_t, const char*> down[] = {
            {34, "GetSocketStatistics"}, {35, "NifIoctl"}, {37, "SetSocketBufferAttribute"}, {38, "Cmd38"}, {39, "NoSfEventFd"},
            {40, "ShutdownAllSockets"}, {41, "Cmd41"}, {42, "SendMMsgNoMem"}, {43, "RecvMMsgNoMem"}, {44, "Cmd44"}};
        for (const auto& [id, name] : down)
            t.on(id, name, [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                put_ret(rsp, kNetDown);
                return 0;
            });
        t.give<uint32_t>(36, "Cmd36", 0);
        t.ok(200, "SetThreadCoreMask");
        t.give<uint32_t>(201, "GetThreadCoreMask", 0);
    });
}

// ==== ssl ============================================================================================================================

// An interface that runs `closed` when its last reference goes.
class ClosingService : public ipc::Service {
public:
    ClosingService(const char* name, std::function<void()> closed) : ipc::Service(name), closed_(std::move(closed)) {}
    void on_close() override {
        if (closed_) closed_();
    }

private:
    std::function<void()> closed_;
};

uint64_t next_pki_id() {
    Net& n = net();
    std::lock_guard<std::mutex> lk(n.mu);
    return n.pki_id++;
}

std::shared_ptr<ipc::Service> make_ssl_connection(std::shared_ptr<std::atomic<int32_t>> live) {
    auto c = std::make_shared<ClosingService>("nn::ssl::sf::ISslConnection", [live] { live->fetch_sub(1); });
    Cmds t(*c, "ssl");
    t.fail(0, "SetSocketDescriptor", kSslNoSocket);
    t.on(1, "SetHostName", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        if (spend(g_ask_ssl)) NXLOG_I("net", "ask: ssl host=\"%s\"", buffer_text(req.in_buf(0)).c_str());
        return 0;
    });
    t.ok(2, "SetVerifyOption");
    t.ok(3, "SetIoMode");
    t.give<int32_t>(4, "GetSocketDescriptor", -1);
    t.give<uint32_t>(5, "GetHostName", 0);
    t.give<uint32_t>(6, "GetVerifyOption", 0);
    t.give<uint32_t>(7, "GetIoMode", 0);
    t.fail(8, "DoHandshake", kSslNoSocket);
    t.fail(9, "DoHandshakeGetServerCert", kSslNoSocket);
    t.fail(10, "Read", kSslNoSocket);
    t.fail(11, "Write", kSslNoSocket);
    t.give<int32_t>(12, "Pending", 0);
    t.fail(13, "Peek", kSslNoSocket);
    t.give<uint32_t>(14, "Poll", 0);
    t.ok(15, "GetVerifyCertError");
    t.give<uint32_t>(16, "GetNeededServerCertBufferSize", 0);
    t.ok(17, "SetSessionCacheMode");
    t.give<uint32_t>(18, "GetSessionCacheMode", 0);
    t.ok(19, "FlushSessionCache");
    t.ok(20, "SetRenegotiationMode");
    t.give<uint32_t>(21, "GetRenegotiationMode", 0);
    t.ok(22, "SetOption");
    t.give<uint8_t>(23, "GetOption", 0);
    t.ok(24, "GetVerifyCertErrors");
    t.fail(25, "GetCipherInfo", kSslNoSocket);
    t.ok(26, "SetNextAlpnProto");
    t.zeros(27, "GetNextAlpnProto", 8);
    t.fail(28, "SetDtlsSocketDescriptor", kSslNoSocket);
    t.give<uint64_t>(29, "GetDtlsHandshakeTimeout", 0);
    t.ok(30, "SetPrivateOption");
    t.ok(31, "SetSrtpCiphers");
    t.give<uint32_t>(32, "GetSrtpCipher", 0);
    t.fail(33, "ExportKeyingMaterial", kSslNoSocket);
    t.ok(34, "SetIoTimeout");
    t.give<uint64_t>(35, "GetIoTimeout", 0);
    t.give<uint32_t>(36, "GetSessionTicket", 0);
    t.ok(37, "SetSessionTicket");
    return c;
}

std::shared_ptr<ipc::Service> make_ssl_context() {
    {
        Net& n = net();
        std::lock_guard<std::mutex> lk(n.mu);
        ++n.contexts;
    }
    auto ctx = std::make_shared<ClosingService>("nn::ssl::sf::ISslContext", [] {
        Net& n = net();
        std::lock_guard<std::mutex> lk(n.mu);
        if (n.contexts) --n.contexts;
    });
    auto live = std::make_shared<std::atomic<int32_t>>(0);   // this context's connections
    Cmds t(*ctx, "ssl");
    t.ok(0, "SetOption");
    t.give<int32_t>(1, "GetOption", 0);
    t.on(2, "CreateConnection", [live](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        live->fetch_add(1);
        std::shared_ptr<ipc::Service> c = make_ssl_connection(live);
        rsp.push_object(c);
        return 0;
    });
    t.on(3, "GetConnectionCount", [live](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(static_cast<uint32_t>(live->load()));
        return 0;
    });
    const H pki = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint64_t>(next_pki_id());
        return 0;
    };
    t.on(4, "ImportServerPki", pki);
    t.on(5, "ImportClientPki", pki);
    t.ok(6, "RemoveServerPki");
    t.ok(7, "RemoveClientPki");
    t.on(8, "RegisterInternalPki", pki);
    t.ok(9, "AddPolicyOid");
    t.on(10, "ImportCrl", pki);
    t.ok(11, "RemoveCrl");
    t.on(12, "ImportClientCertKeyPki", pki);
    t.fail(13, "GeneratePrivateKeyAndCert", kSslBadOption);
    return ctx;
}

std::shared_ptr<ipc::Service> make_ssl_service() {
    return make_iface("nn::ssl::sf::ISslService", [](ipc::Service& s) {
        Cmds t(s, "ssl");
        t.object(0, "CreateContext", make_ssl_context);
        t.on(1, "GetContextCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            Net& n = net();
            std::lock_guard<std::mutex> lk(n.mu);
            rsp.push<uint32_t>(n.contexts);
            return 0;
        });
        t.give<uint32_t>(2, "GetCertificates", 0);         // PokoMac: no built-in certificates (AnySwitch's ssl_builtin)
        t.give<uint32_t>(3, "GetCertificateBufSize", 0);
        t.fail(4, "DebugIoctl", kSslBadOption);
        t.ok(5, "SetInterfaceVersion");
        t.give<uint32_t>(6, "FlushSessionCache", 0);
        t.ok(7, "SetDebugOption");
        t.on(8, "GetDebugOption", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint8_t zero = 0;
            buf_write(req.out_buf(0), &zero, 1);
            return 0;
        });
        t.ok(9, "ClearTls12FallbackFlag");
        t.ok(10, "GetCertificateByIndex");
        t.give<uint32_t>(11, "GetTrustedCertificateCount", 0);
    });
}

// ==== nim:eca (the shop) =============================================================================================================
std::shared_ptr<ipc::Service> make_shop_async(std::shared_ptr<ServiceEvent> done) {
    struct Body {
        std::mutex mu;
        std::string bytes;
    };
    auto body = std::make_shared<Body>();
    return make_iface("nn::ec::IShopServiceAsync", [body, done](ipc::Service& s) {
        Cmds t(s, "nim:eca");
        t.ok(0, "Cancel");
        t.on(1, "GetSize", [body](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(body->mu);
            rsp.push<uint64_t>(body->bytes.size());
            return 0;
        });
        t.on(2, "Read", [body](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint64_t off = req.arg<uint64_t>(0);
            uint64_t wrote = 0;
            {
                std::lock_guard<std::mutex> lk(body->mu);
                if (off < body->bytes.size()) wrote = buf_write(req.out_buf(0), body->bytes.data() + off, body->bytes.size() - off);
            }
            rsp.push<uint64_t>(wrote);
            return 0;
        });
        t.give<uint32_t>(3, "GetErrorCode", 0);
        t.on(4, "Request", [body, done](ipc::Request&, ipc::Response&) -> uint32_t {
            {
                std::lock_guard<std::mutex> lk(body->mu);
                body->bytes = "{}";   // an empty JSON answer, at once
            }
            done->signal();
            return 0;
        });
        t.ok(5, "Prepare");
    });
}

std::shared_ptr<ipc::Service> make_shop_accessor() {
    return make_iface("nn::ec::IShopServiceAccessor", [](ipc::Service& s) {
        Cmds t(s, "nim:eca");
        t.on(0, "CreateAsyncInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            auto done = std::make_shared<ServiceEvent>(false);
            rsp.out_copy.push_back(done->new_handle());
            rsp.push_object(make_shop_async(done));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_shop_server() {
    return make_iface("nn::ec::IShopServiceAccessServer", [](ipc::Service& s) {
        Cmds t(s, "nim:eca");
        t.object(0, "CreateAccessorInterface", make_shop_accessor);
    });
}

std::shared_ptr<ipc::Service> make_nim_eca() {
    return make_iface("nn::nim::detail::IShopServiceAccessServerInterface", [](ipc::Service& s) {
        Cmds t(s, "nim:eca");
        t.object(0, "CreateServerInterface", make_shop_server);
        t.give<uint8_t>(4, "IsLargeResourceAvailable", 0);
        t.object(5, "CreateServerInterface2", make_shop_server);
    });
}

// ==== friend =========================================================================================================================
std::shared_ptr<ipc::Service> make_friend_service() {
    auto done = std::make_shared<ServiceEvent>(true);   // nothing is ever pending
    return make_iface("nn::friends::detail::ipc::IFriendService", [done](ipc::Service& s) {
        Cmds t(s, "friend");
        t.event(0, "GetCompletionEvent", done);
        // lists and counts: none (nothing is written into the list buffers)
        static const std::pair<uint32_t, const char*> none[] = {
            {10100, "GetFriendListIds"}, {10101, "GetFriendList"}, {10400, "GetBlockedUserListIds"},
            {10500, "GetProfileListLegacyV1"}, {10501, "GetProfileListLegacyV2"}, {10502, "GetProfileListV1"},
            {10503, "GetProfileListV2"}, {20100, "GetFriendCount"}, {20101, "GetNewlyFriendCount"},
            {20105, "GetFriendListForViewerV1"}, {20108, "GetFriendListForViewerV2"}, {20200, "GetReceivedFriendRequestCount"},
            {20201, "GetFriendRequestListV1"}, {20202, "GetFriendRequestListV2"}, {20203, "GetFriendRequestReceivedNotificationCount"},
            {20300, "GetFriendCandidateList"}, {20400, "GetBlockedUserListV1"}, {20402, "GetBlockedUserListV2"},
            {20500, "GetProfileExtraListV1"}, {20502, "GetProfileExtraListV2"}, {20700, "GetPlayHistoryListV1"},
            {20702, "GetPlayHistoryListV2"}, {22000, "GetReceivedFriendInvitationListV1"}, {22002, "GetReceivedFriendInvitationListV2"},
            {22010, "GetReceivedFriendInvitationCountCache"}};
        for (const auto& [id, name] : none) t.give<int32_t>(id, name, 0);
        t.give<uint8_t>(10120, "CheckFriendListAvailability", 1);
        t.give<uint8_t>(10420, "CheckBlockedUserListAvailability", 1);
        const H presence = [](ipc::Request& req, ipc::Response&) -> uint32_t {
            clear_buffer(req.out_buf(0), 0x400);
            return 0;
        };
        t.on(20600, "GetUserPresenceViewV1", presence);
        t.on(20601, "GetUserPresenceViewV2", presence);
        // accepted, nothing to do or to answer
        static const std::pair<uint32_t, const char*> accepted[] = {
            {1, "Cancel"}, {10102, "UpdateFriendInfo"}, {10110, "GetFriendProfileImage"},
            {10111, "GetFriendProfileImageWithImageSize"}, {10121, "EnsureFriendListAvailable"},
            {10200, "SendFriendRequestForApplication"}, {10211, "AddFacedFriendRequestForApplication"},
            {10421, "EnsureBlockedUserListAvailable"}, {10600, "DeclareOpenOnlinePlaySession"},
            {10601, "DeclareCloseOnlinePlaySession"}, {10610, "UpdateUserPresence"}, {10700, "GetPlayHistoryRegistrationKey"},
            {10701, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId"}, {10702, "AddPlayHistory"},
            {11000, "GetProfileImageUrl"}, {11001, "GetProfileImageUrlV2"}, {20102, "GetFriendDetailedInfo"},
            {20103, "SyncFriendList"}, {20104, "RequestSyncFriendList"}, {20106, "UpdateFriendInfoForViewerV1"},
            {20107, "GetFriendDetailedInfoV2"}, {20109, "UpdateFriendInfoForViewerV2"}, {20110, "LoadFriendSettingV1"},
            {20111, "LoadFriendSettingV2"}, {20301, "GetNintendoNetworkIdInfo"}, {20401, "SyncBlockedUserList"},
            {20501, "GetRelationship"}, {20701, "GetPlayHistoryStatistics"}, {20800, "LoadUserSettingV1"},
            {20801, "SyncUserSetting"}, {20802, "LoadUserSettingV2"}, {20900, "RequestListSummaryOverlayNotification"},
            {21000, "GetExternalApplicationCatalog"}, {22001, "GetReceivedFriendInvitationDetailedInfoV1"},
            {22003, "GetReceivedFriendInvitationDetailedInfoV2"}, {30100, "DropFriendNewlyFlags"}, {30101, "DeleteFriend"},
            {30110, "DropFriendNewlyFlag"}, {30120, "ChangeFriendFavoriteFlag"}, {30121, "ChangeFriendOnlineNotificationFlag"},
            {30130, "SetFriendNote"}, {30131, "RequestUploadPendingNote"}, {30190, "RequestSyncLocalUpdates"},
            {30200, "SendFriendRequest"}, {30201, "SendFriendRequestWithApplicationInfoV1"}, {30202, "CancelFriendRequest"},
            {30203, "AcceptFriendRequest"}, {30204, "RejectFriendRequest"}, {30205, "ReadFriendRequest"},
            {30210, "GetFacedFriendRequestRegistrationKey"}, {30211, "AddFacedFriendRequest"}, {30212, "CancelFacedFriendRequest"},
            {30213, "GetFacedFriendRequestProfileImage"}, {30214, "GetFacedFriendRequestProfileImageFromPath"},
            {30215, "SendFriendRequestWithExternalApplicationCatalogId"}, {30216, "ResendFacedFriendRequest"},
            {30217, "SendFriendRequestWithNintendoNetworkIdInfo"}, {30218, "SendFriendRequestWithApplicationInfoV2"},
            {30300, "GetSnsAccountLinkPageUrl"}, {30301, "UnlinkSnsAccount"}, {30400, "BlockUser"},
            {30401, "BlockUserWithApplicationInfoV1"}, {30402, "UnblockUser"}, {30403, "BlockUserWithApplicationInfoV2"},
            {30500, "GetProfileExtraFromFriendCodeV1"}, {30501, "GetProfileExtraFromFriendCodeV2"}, {30700, "DeletePlayHistory"},
            {30701, "AddPlayHistoryWithApplication"}, {30810, "ChangePresencePermission"}, {30811, "ChangeFriendRequestReception"},
            {30812, "ChangePlayLogPermission"}, {30820, "IssueFriendCode"}, {30830, "ClearPlayLog"},
            {30900, "SendFriendInvitationV1"}, {30901, "SendFriendInvitationV2"}, {30910, "ReadFriendInvitation"},
            {30911, "ReadAllFriendInvitations"}, {31000, "OpenUser"}, {31001, "OpenUserWithPlayLogPolicy"},
            {40100, "DeleteFriendListCache"}, {40400, "DeleteBlockedUserListCache"}, {49900, "DeleteNetworkServiceAccountCache"}};
        for (const auto& [id, name] : accepted) t.ok(id, name);
    });
}

std::shared_ptr<ipc::Service> make_friend_notifications() {
    auto ev = std::make_shared<ServiceEvent>(false);
    return make_iface("nn::friends::detail::ipc::INotificationService", [ev](ipc::Service& s) {
        Cmds t(s, "friend");
        t.event(0, "GetEvent", ev);
        t.ok(1, "Clear");
        t.fail(2, "Pop", kNoNotification);
    });
}

std::shared_ptr<ipc::Service> make_friend_creator() {
    return make_iface("nn::friends::detail::ipc::IServiceCreator", [](ipc::Service& s) {
        Cmds t(s, "friend");
        t.object(0, "CreateFriendService", make_friend_service);
        t.object(1, "CreateNotificationService", make_friend_notifications);
        t.object(2, "CreateDaemonSuspendSessionService",
                 [] { return make_iface("nn::friends::detail::ipc::IDaemonSuspendSessionService", [](ipc::Service&) {}); });
    });
}

// ==== prepo (play reports: accepted and dropped) =====================================================================================
std::shared_ptr<ipc::Service> make_prepo() {
    return make_iface("nn::prepo::detail::ipc::IPrepoService", [](ipc::Service& s) {
        Cmds t(s, "prepo");
        t.ok(10100, "SaveReportOld");
        t.ok(10101, "SaveReportWithUserOld");
        t.ok(10102, "SaveReportOld2");
        t.ok(10103, "SaveReportWithUserOld2");
        t.ok(10104, "SaveReportOld3");
        t.ok(10105, "SaveReportWithUserOld3");
        t.ok(10106, "SaveReport");
        t.ok(10107, "SaveReportWithUser");
        t.ok(10200, "RequestImmediateTransmission");
        t.give<uint64_t>(10400, "GetSystemSessionId", 0);
    });
}

// ==== pctl (no restriction set) ======================================================================================================
std::shared_ptr<ipc::Service> make_pctl_service() {
    std::array<std::shared_ptr<ServiceEvent>, 4> ev;
    for (auto& e : ev) e = std::make_shared<ServiceEvent>(false);
    return make_iface("nn::pctl::detail::ipc::IParentalControlService", [ev](ipc::Service& s) {
        Cmds t(s, "pctl");
        t.ok(1, "Initialize");
        static const std::pair<uint32_t, const char*> accepted[] = {
            {1001, "CheckFreeCommunicationPermission"}, {1002, "ConfirmLaunchApplicationPermission"},
            {1003, "ConfirmResumeApplicationPermission"}, {1004, "ConfirmSnsPostPermission"},
            {1005, "ConfirmSystemSettingsPermission"}, {1007, "RevertRestrictionTemporaryUnlocked"},
            {1008, "EnterRestrictedSystemSettings"}, {1009, "LeaveRestrictedSystemSettings"},
            {1011, "RevertRestrictedSystemSettingsEntered"}, {1013, "ConfirmStereoVisionPermission"},
            {1014, "ConfirmPlayableApplicationVideoOld"}, {1015, "ConfirmPlayableApplicationVideo"},
            {1016, "ConfirmShowNewsPermission"}, {1017, "EndFreeCommunication"}, {1018, "IsFreeCommunicationAvailable"},
            {1019, "ConfirmLaunchApplicationPermission"}, {1020, "ConfirmLaunchSharedApplicationPermission"},
            {1021, "TryBeginFreeCommunicationForStreamPlay"}, {1022, "EndFreeCommunicationForStreamPlay"},
            {1033, "SetSafetyLevel"}, {1034, "GetSafetyLevelSettings"}, {1036, "SetCustomSafetyLevelSettings"},
            {1038, "SetDefaultRatingOrganization"}, {1042, "AddToFreeCommunicationApplicationList"}, {1043, "DeleteSettings"},
            {1045, "UpdateFreeCommunicationApplicationList"}, {1046, "DisableFeaturesForReset"},
            {1047, "NotifyApplicationDownloadStartedOld"}, {1048, "NotifyNetworkProfileCreated"},
            {1049, "ResetFreeCommunicationApplicationList"}, {1050, "AddToFreeCommunicationApplicationList"},
            {1051, "NotifyApplicationDownloadStarted"}, {1061, "ConfirmStereoVisionRestrictionConfigurable"},
            {1063, "SetStereoVisionRestriction"}, {1064, "ResetConfirmedStereoVisionPermission"},
            {1201, "UnlockRestrictionTemporarily"}, {1202, "UnlockSystemSettingsRestriction"}, {1203, "SetPinCode"},
            {1204, "GenerateInquiryCode"}, {1205, "CheckMasterKey"}};
        for (const auto& [id, name] : accepted) t.ok(id, name);
        t.give<uint8_t>(1006, "IsRestrictionTemporaryUnlocked", 0);
        t.give<uint8_t>(1010, "IsRestrictedSystemSettingsEntered", 0);
        t.give<uint32_t>(1012, "GetRestrictedFeatures", 0);
        t.give<uint32_t>(1023, "GetRestrictedFeatureFlagSet", 0);
        t.give<uint8_t>(1031, "IsRestrictionEnabled", 0);
        t.give<int32_t>(1032, "GetSafetyLevel", 0);
        t.zeros(1035, "GetCurrentSettings", 3);
        t.give<uint32_t>(1037, "GetDefaultRatingOrganization", 0);
        t.give<int32_t>(1039, "GetFreeCommunicationApplicationListCount", 0);
        t.give<int32_t>(1044, "GetFreeCommunicationApplicationList", 0);
        t.give<uint8_t>(1062, "GetStereoVisionRestriction", 0);
        t.give<uint8_t>(1065, "IsStereoVisionPermitted", 1);
        t.give<int32_t>(1206, "GetPinCodeLength", 0);
        t.event(1207, "GetPinCodeChangedEvent", ev[0]);
        t.give<uint8_t>(1403, "IsPairingActive", 0);
        t.event(1432, "GetSynchronizationEvent", ev[1]);
        t.give<uint8_t>(1453, "IsPlayTimerEnabled", 0);
        t.event(1457, "GetPlayTimerEventToRequestSuspension", ev[2]);
        t.event(1473, "GetUnlinkedEvent", ev[3]);
    });
}

std::shared_ptr<ipc::Service> make_pctl() {
    return make_iface("nn::pctl::detail::ipc::IParentalControlServiceFactory", [](ipc::Service& s) {
        Cmds t(s, "pctl");
        t.object(0, "CreateService", make_pctl_service);
        t.object(1, "CreateServiceWithoutInitialize", make_pctl_service);
    });
}

// ==== bcat (no delivery cache data, no network) =======================================================================================
#define BCAT "nn::bcat::detail::ipc::"

std::shared_ptr<ipc::Service> make_bcat_progress() {
    auto ev = std::make_shared<ServiceEvent>(true);   // the sync is over at once
    return make_iface(BCAT "IDeliveryCacheProgressService", [ev](ipc::Service& s) {
        Cmds t(s, "bcat");
        t.event(0, "GetEvent", ev);
        t.on(1, "GetImpl", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            clear_buffer(req.out_buf(0), 0x200);   // DeliveryCacheProgressImpl: nothing done, nothing to do
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_service() {
    return make_iface(BCAT "IBcatService", [](ipc::Service& s) {
        Cmds t(s, "bcat");
        t.object(10100, "RequestSyncDeliveryCache", make_bcat_progress);
        t.object(10101, "RequestSyncDeliveryCacheWithDirectoryName", make_bcat_progress);
        t.object(20100, "RequestSyncDeliveryCacheWithApplicationId", make_bcat_progress);
        t.object(20101, "RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName", make_bcat_progress);
        t.object(20300, "GetDeliveryCacheStorageUpdateNotifier", [] {
            auto ev = std::make_shared<ServiceEvent>(false);
            return make_iface(BCAT "INotifierService", [ev](ipc::Service& n) {
                Cmds u(n, "bcat");
                u.event(0, "GetEvent", ev);
            });
        });
        t.object(20301, "RequestSuspendDeliveryTask", [] { return make_iface(BCAT "IDeliveryTaskSuspensionService", [](ipc::Service&) {}); });
    });
}

std::shared_ptr<ipc::Service> make_bcat_storage() {
    return make_iface(BCAT "IDeliveryCacheStorageService", [](ipc::Service& s) {
        Cmds t(s, "bcat");
        t.object(0, "CreateFileService", [] {
            return make_iface(BCAT "IDeliveryCacheFileService", [](ipc::Service& f) {
                Cmds u(f, "bcat");
                u.fail(0, "Open", kNoSuchPath);
                u.give<uint64_t>(1, "Read", 0);
                u.give<uint64_t>(2, "GetSize", 0);
                u.zeros(3, "GetDigest", 16);
            });
        });
        t.object(1, "CreateDirectoryService", [] {
            return make_iface(BCAT "IDeliveryCacheDirectoryService", [](ipc::Service& d) {
                Cmds u(d, "bcat");
                u.fail(0, "Open", kNoSuchPath);
                u.give<int32_t>(1, "Read", 0);
                u.give<int32_t>(2, "GetCount", 0);
            });
        });
        t.give<int32_t>(10, "EnumerateDeliveryCacheDirectory", 0);
    });
}

std::shared_ptr<ipc::Service> make_bcat() {
    return make_iface(BCAT "IServiceCreator", [](ipc::Service& s) {
        Cmds t(s, "bcat");
        t.object(0, "CreateBcatService", make_bcat_service);
        t.object(1, "CreateDeliveryCacheStorageService", make_bcat_storage);
        t.object(2, "CreateDeliveryCacheStorageServiceWithApplicationId", make_bcat_storage);
    });
}
#undef BCAT

// ==== nsd ============================================================================================================================
std::shared_ptr<ipc::Service> make_nsd() {
    return make_iface("nn::nsd::detail::IManager", [](ipc::Service& s) {
        Cmds t(s, "nsd");
        t.on(11, "GetEnvironmentIdentifier", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            static const uint8_t lp1[8] = {'l', 'p', '1', 0, 0, 0, 0, 0};
            buf_write(req.out_buf(0), lp1, sizeof lp1);
            return 0;
        });
        t.ok(15, "SetChangeEnvironmentIdentifierDisabled");
        t.on(20, "Resolve", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            echo(req.in_buf(0), req.out_buf(0));   // every name is its own address
            return 0;
        });
        t.on(21, "ResolveEx", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            echo(req.in_buf(0), req.out_buf(0));
            rsp.push<uint32_t>(0);
            return 0;
        });
        t.give<uint8_t>(100, "GetApplicationServerEnvironmentType", 1);   // Lp: the retail servers
    });
}

// ==== sfdnsres (no name resolves) =====================================================================================================
constexpr uint32_t kHostNotFound = 1;   // netdb h_errno
constexpr int32_t kEaiNoName = 8;       // FreeBSD EAI_NONAME

const char* host_error_text(int32_t e) {
    switch (e) {
        case 1: return "Unknown host";
        case 2: return "Temporary failure in name resolution";
        case 3: return "Non-recoverable failure in name resolution";
        case 4: return "No address associated with name";
        default: return "Unknown error";
    }
}
const char* gai_error_text(int32_t e) {
    static const char* const text[] = {
        "Success", "Address family for hostname not supported", "Temporary failure in name resolution", "Invalid value for ai_flags",
        "Non-recoverable failure in name resolution", "ai_family not supported", "Memory allocation failure",
        "No address associated with hostname", "hostname nor servname provided, or not known", "servname not supported for ai_socktype",
        "ai_socktype not supported", "System error returned in errno", "Invalid value for hints", "Resolved protocol is unknown",
        "Argument buffer overflow"};
    return e >= 0 && e < static_cast<int32_t>(sizeof text / sizeof text[0]) ? text[e] : "Unknown error";
}
void write_text(const ipc::Buffer& b, const char* text) { buf_write(b, text, std::strlen(text) + 1); }

void ask_dns(const ipc::Request& req) {
    if (!spend(g_ask_dns)) return;
    NXLOG_I("net", "ask: dns host=\"%s\" service=\"%s\"", buffer_text(req.in_buf(0)).c_str(), buffer_text(req.in_buf(1)).c_str());
}

std::shared_ptr<ipc::Service> make_resolver() {
    return make_iface("nn::socket::resolver::IResolver", [](ipc::Service& s) {
        Cmds t(s, "sfdnsres");
        const H by_name = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {   // (netdb error, errno, size)
            rsp.push<uint32_t>(kHostNotFound);
            rsp.push<uint32_t>(vs::kEnetdown);
            rsp.push<uint32_t>(0);
            return 0;
        };
        t.on(2, "GetHostByNameRequest", by_name);
        t.on(3, "GetHostByAddrRequest", by_name);
        t.on(4, "GetHostStringErrorRequest", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            write_text(req.out_buf(0), host_error_text(req.arg<int32_t>(0)));
            return 0;
        });
        t.on(5, "GetGaiStringErrorRequest", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            write_text(req.out_buf(0), gai_error_text(req.arg<int32_t>(0)));
            return 0;
        });
        t.on(6, "GetAddrInfoRequest", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {   // (errno, gai error, size)
            ask_dns(req);
            rsp.push<uint32_t>(0);
            rsp.push<int32_t>(kEaiNoName);
            rsp.push<uint32_t>(0);
            return 0;
        });
        t.on(8, "GetCancelHandleRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            Net& n = net();
            std::lock_guard<std::mutex> lk(n.mu);
            rsp.push<uint32_t>(n.cancel_handle++);
            return 0;
        });
        t.ok(9, "CancelRequest");
        const H by_name_opt = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {   // (size, netdb error, errno)
            rsp.push<uint32_t>(0);
            rsp.push<uint32_t>(kHostNotFound);
            rsp.push<uint32_t>(vs::kEnetdown);
            return 0;
        };
        t.on(10, "GetHostByNameRequestWithOptions", by_name_opt);
        t.on(11, "GetHostByAddrRequestWithOptions", by_name_opt);
        t.on(12, "GetAddrInfoRequestWithOptions", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {   // (size, gai, netdb, errno)
            ask_dns(req);
            rsp.push<uint32_t>(0);
            rsp.push<int32_t>(kEaiNoName);
            rsp.push<uint32_t>(0);
            rsp.push<uint32_t>(0);
            return 0;
        });
        t.on(14, "ResolverSetOptionRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<int32_t>(0);
            rsp.push<uint32_t>(0);
            return 0;
        });
    });
}

// ==== ntc (the network clock) ========================================================================================================
std::shared_ptr<ipc::Service> make_ntc_task() {
    struct Task {
        std::atomic<uint32_t> result{0};
        std::shared_ptr<ServiceEvent> finished = std::make_shared<ServiceEvent>(false);
    };
    auto task = std::make_shared<Task>();
    return make_iface("nn::ntc::detail::IEnsureNetworkClockAvailabilityService", [task](ipc::Service& s) {
        Cmds t(s, "ntc");
        t.on(0, "StartTask", [task](ipc::Request&, ipc::Response&) -> uint32_t {
            const uint32_t r = online_test() ? 0 : kNetTimeUnavailable;
            task->result.store(r);
            task->finished->signal();
            return r;
        });
        t.event(1, "GetFinishNotificationEvent", task->finished);
        t.on(2, "GetResult", [task](ipc::Request&, ipc::Response&) -> uint32_t { return task->result.load(); });
        t.on(3, "Cancel", [task](ipc::Request&, ipc::Response&) -> uint32_t {
            task->result.store(kNetTimeCanceled);
            task->finished->signal();
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_ntc() {
    return make_iface("nn::ntc::detail::IStaticService", [](ipc::Service& s) {
        Cmds t(s, "ntc");
        t.object(0, "CreateEnsureNetworkClockAvailabilityService", make_ntc_task);
    });
}

// ==== ngct:u, ngc:u (no word list: nothing matches, text comes back unchanged) ======================================================
std::shared_ptr<ipc::Service> make_ngct() {
    return make_iface("nn::ngct::detail::IService", [](ipc::Service& s) {
        Cmds t(s, "ngct");
        t.give<uint8_t>(0, "Match", 0);
        t.on(1, "Filter", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            echo(req.in_buf(0), req.out_buf(0));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_ngc() {
    return make_iface("nn::ngc::detail::IService", [](ipc::Service& s) {
        Cmds t(s, "ngc");
        t.give<uint32_t>(0, "GetContentVersion", 1);
        t.give<uint32_t>(1, "Check", 0);
        t.on(2, "Mask", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            echo(req.in_buf(0), req.out_buf(0));
            rsp.push<uint32_t>(0);
            return 0;
        });
        t.ok(3, "Reload");
    });
}

// ==== notif:a ========================================================================================================================
std::shared_ptr<ipc::Service> make_notification() {
    return make_iface("nn::notification::INotificationServicesForApplication", [](ipc::Service& s) {
        Cmds t(s, "notif");
        t.give<int32_t>(520, "ListAlarmSettings", 0);
        t.ok(1000, "Initialize");
    });
}

// ==== session roots that depend on the redirect table (read at each session open) ===================================================
std::shared_ptr<ipc::Service> open_bsd() { return make_bsd(); }              // PokoMac: no redirect table
std::shared_ptr<ipc::Service> open_resolver() { return make_resolver(); }
std::shared_ptr<ipc::Service> make_bsdcfg() { return make_iface("nn::bsdsocket::cfg::ServerInterface", [](ipc::Service&) {}); }

}  // namespace

void nifm_set_lan_source(NifmLanSource f) { g_lan_source.store(f); }

void register_network() {
    NXLOG_I("net", "network services: [net] offline=%s (no connection, no friends, no BCAT data in either mode)",
            config_get_bool("net", "offline", true) ? "true" : "false");
    if (online_test()) NXLOG_I("net", "[net] online_test = on: nifm and ntc report online, delivery cache empty, no sockets");
    // every session open makes a new root object; registered in this order
    using Make = std::shared_ptr<ipc::Service> (*)();
    struct Entry {
        const char* names[5];
        Make make;
    };
    static const Entry entries[] = {
        {{"nifm:u", "nifm:a", "nifm:s"}, &make_nifm},
        {{"bsd:u", "bsd:a", "bsd:s"}, &open_bsd},
        {{"bsdcfg"}, &make_bsdcfg},
        {{"ssl", "ssl:s"}, &make_ssl_service},   // ssl:s is what Switch 2 titles open
        {{"nim:eca"}, &make_nim_eca},
        {{"friend:u", "friend:v", "friend:m", "friend:s", "friend:a"}, &make_friend_creator},
        {{"prepo:u", "prepo:a", "prepo:a2"}, &make_prepo},
        {{"pctl", "pctl:s", "pctl:r", "pctl:a"}, &make_pctl},
        {{"bcat:u", "bcat:a"}, &make_bcat},
        {{"ldn:u"}, &make_ldn_user_creator},     // ldn.cpp's objects
        {{"nsd:u", "nsd:a"}, &make_nsd},
        {{"sfdnsres"}, &open_resolver},
        {{"ntc"}, &make_ntc},
        {{"ngct:u"}, &make_ngct},
        {{"ngc:u"}, &make_ngc},
        {{"notif:a"}, &make_notification},
    };
    for (const Entry& e : entries)
        for (const char* name : e.names)
            if (name != nullptr) add(name, e.make);
    nifm_set_lan_source(&lanroom::source);
}

void network_reset() {
    {
        Net& n = net();
        std::lock_guard<std::mutex> lk(n.mu);
        n.used.clear();
        n.calls.clear();
        n.client_id = 1;
        n.cancel_handle = 1;
        n.pki_id = 1;
        n.contexts = 0;
    }
    lanroom::reset();
    ldn_reset();
    vnet::set_current_link(nullptr);
    vsock::stack().reset();
}

}  // namespace nxrt::services
