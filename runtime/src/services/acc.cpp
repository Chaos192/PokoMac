// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Account services (S3c): acc:u0 (nn::account::IAccountServiceForApplication) and acc:u1 (IAccountServiceForSystemService), with the
// interfaces they hand out: IProfile, IManagerForApplication and IAsyncContext. The console has one user (services::profile()'s
// [account] user) and a network service account that works on this machine and never reaches a server.
// Command ids and layouts: https://switchbrew.org/wiki/Account_services (Uid as two u64, ProfileBase 0x38, UserData 0x80); result codes:
// https://switchbrew.org/wiki/Error_codes (module 124: 20 InvalidUserId, 32 InvalidArrayLength, 100 UserNotExist). Cross-checked with
// Ryujinx (MIT) src/Ryujinx.HLE/HOS/Services/Account/Acc: the Uid's high half first, the 0x24000-byte save data thumbnail.
//
// What a game sees:
//   * one user, always the "last opened" and preselected one; a profile that is that user's nickname and picture ([account] avatar, a
//     JPEG of the player's own, else acc_avatar.cpp's stand-in), created 2019-01-01;
//   * the one user is open from the start: the open user list is the profile's user at its first use after start or acc_reset
//     (the runtime never calls acc_reset at boot, so a list made there would have been empty in every game run);
//   * a network service account id that differs between machines (Pokopia's Link Play needs two different ids) and stays the same on
//     one machine: [account] network_account_id, else a hash of the Windows machine id with the user id, else the user id's halves xored;
//   * an id token ([net] account_token / account_token_file, the replacement server's sign-in; 0x100 zero bytes without one) and a
//     network service licence only when a token and a [net] redirect table are both set (docs/NETWORK_REDIRECT.md);
//   * asynchronous requests that are already done: their event is signalled from the start.
// Commands 60 of acc:u0/u1 and 130, 136, 150, 160 of IManagerForApplication are answered by ext_account.cpp (ipc::extend): they are not
// registered here, or the extension would never run.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "../net/redirect.h"
#include "services_internal.h"

namespace nxrt::services {

extern const unsigned char kDefaultAvatarJpeg[];   // the built-in picture (defined in acc_avatar.cpp)
extern const size_t kDefaultAvatarJpegSize;

namespace {

const uint32_t kInvalidUserId = res(124, 20);         // 2124-0020
const uint32_t kInvalidArrayLength = res(124, 32);    // 2124-0032
const uint32_t kUserNotExist = res(124, 100);         // 2124-0100
const uint32_t kNoInternet = res(110, 300);           // nifm: what an unlicensed account's request ends with

constexpr uint64_t kUserListBytes = 10 * 16;          // the SDK's array of 10 Uid
constexpr uint64_t kThumbnailBytes = 0x24000;         // StoreSaveDataThumbnail's image
constexpr uint64_t kProfileCreated = 1546300800;      // 2019-01-01 00:00 UTC
constexpr uint64_t kUserDataBytes = 0x80;             // IProfile Get's UserData
constexpr uint64_t kAvatarMax = 1u << 20;             // a picture of the player's own is read up to 1 MiB
constexpr size_t kTokenMax = 0xC00;                   // NetworkServiceAccountIdTokenLengthMax
constexpr size_t kTokenFileRead = kTokenMax + 1;
constexpr uint64_t kNoTokenBytes = 0x100;             // the id token cache without a token: zeros

struct Uid {
    uint64_t hi = 0, lo = 0;
    bool null() const { return hi == 0 && lo == 0; }
    bool operator==(const Uid& other) const { return lo == other.lo && hi == other.hi; }
};
static_assert(sizeof(Uid) == 16, "nn::account::Uid");

Uid local_uid() {
    const TitleProfile& prof = profile();
    Uid me;
    me.hi = prof.user_uid_hi;
    me.lo = prof.user_uid_lo;
    return me;
}
Uid uid_arg(const ipc::Request& req) { return Uid{req.arg<uint64_t>(0), req.arg<uint64_t>(8)}; }
bool user_exists(const Uid& u) {
    if (u.null()) return false;   // the null uid is never a user
    return u == local_uid();
}
std::string uid_text(const Uid& u) {
    char b[40];
    std::snprintf(b, sizeof b, "%016llx_%016llx", static_cast<unsigned long long>(u.hi), static_cast<unsigned long long>(u.lo));
    return b;
}

// Process state (acc_reset puts it back).
struct State {
    std::mutex mu;
    std::vector<Uid> open_users;
    bool open_ready = false;       // the open user list is made at its first use
    bool id_ready = false;
    uint64_t id = 0;
    std::mutex log_mu;
    std::set<std::string> logged;  // first-use log lines already written
};
State& state() {
    static State s;
    return s;
}
bool first_time(const std::string& key) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.log_mu);
    return s.logged.insert(key).second;
}
std::vector<Uid> open_users() {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.open_ready) {
        s.open_users = {local_uid()};
        s.open_ready = true;
    }
    return s.open_users;
}

// The users into the reply buffer (B, else C): as many whole Uids as fit in the first 0xA0 bytes, zeros after them up to there.
void write_users(const ipc::Request& req, const std::vector<Uid>& users) {
    const ipc::Buffer b = req.out_buf(0);
    if (!b.addr) return;
    std::array<uint8_t, kUserListBytes> list{};
    const uint64_t n = std::min<uint64_t>(b.size, kUserListBytes);
    for (size_t i = 0; i < users.size() && 16 * (i + 1) <= n; ++i) std::memcpy(list.data() + 16 * i, &users[i], 16);
    buf_write(b, list.data(), n);
}

// ---- IAsyncContext -------------------------------------------------------------------------------------------------------------
// A request that finished before it was handed out: its event is signalled from the start and stays so. `license`: the licence query's
// context (IAsyncNetworkServiceLicenseKindContext), which also answers 100 with the licence kind (always successfully: the SDK aborts
// otherwise).
std::shared_ptr<ipc::Service> make_async_context(uint32_t result, bool license, bool licensed) {
    auto ev = std::make_shared<ServiceEvent>(true);
    return make_iface("IAsyncContext", [ev, result, license, licensed](ipc::Service& s) {
        s.add_cmd(0, "GetSystemEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
        s.add_cmd(1, "Cancel", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        s.add_cmd(2, "HasDone", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(1);
            return 0;
        });
        s.add_cmd(3, "GetResult", [result](ipc::Request&, ipc::Response&) -> uint32_t { return result; });
        if (license)
            s.add_cmd(100, "GetNetworkServiceLicenseKind", [licensed](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push<uint32_t>(licensed ? 1u : 0u);   // 1 = subscribed
                return 0;
            });
    });
}

// ---- the network service account ----------------------------------------------------------------------------------------------
std::string trim_blanks(std::string s) {
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && blank(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && blank(s[i])) ++i;
    return s.substr(i);
}

// [account] network_account_id: 16 hex digits, not all zero. 0 when unset or unusable (a set but unusable value is logged).
uint64_t configured_account_id() {
    const std::string raw = config_get_str("account", "network_account_id", "");
    if (raw.empty()) return 0;
    const std::string t = trim_blanks(raw);
    uint64_t v = 0;
    bool ok = t.size() == 16;
    for (char c : t) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            ok = false;
            break;
        }
        v = v * 16 + static_cast<uint64_t>(d);   // one hex digit more: 16 of them fill the 64 bits
    }
    if (!ok || v == 0) {
        NXLOG_W("acc", "[account] network_account_id \"%s\" is not 16 hex digits of a non-zero id: ignored", raw.c_str());
        return 0;
    }
    return v;
}

// This machine's id (Windows: the MachineGuid the OS made at installation), "" when there is none.
std::string machine_guid() {
#if defined(_WIN32)
    char buf[128] = {};
    DWORD n = sizeof buf;
    const DWORD want = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;   // a string, from the 64-bit view
    const LSTATUS rc = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", want, nullptr, buf, &n);
    if (rc == ERROR_SUCCESS) {
        buf[sizeof buf - 1] = 0;   // terminated even when the value filled the buffer
        return std::string(buf);
    }
#endif
    return {};
}

// FNV-1a 64 over the machine id and the user id's 16 bytes, then the splitmix64 finalizer: different machines, different ids.
uint64_t hashed_account_id(const std::string& guid, const Uid& u) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&h](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    };
    mix(reinterpret_cast<const uint8_t*>(guid.data()), guid.size());
    uint8_t b[16];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<uint8_t>(u.hi >> (8 * i));
        b[8 + i] = static_cast<uint8_t>(u.lo >> (8 * i));
    }
    mix(b, sizeof b);
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h ? h : 1;
}

// The id GetAccountId answers: made once (until acc_reset) for the first manager that asks.
uint64_t network_account_id(const Uid& u) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.id_ready) return s.id;
    const char* source;
    uint64_t id = configured_account_id();
    if (id) {
        source = config_source("account", "network_account_id");
    } else if (const std::string g = machine_guid(); !g.empty()) {
        id = hashed_account_id(g, u);
        source = "this machine's id and the user id";
    } else {
        id = u.hi ^ u.lo;
        source = "the user id";
    }
    NXLOG_I("acc", "network service account id 0x%016llx (%s)", static_cast<unsigned long long>(id), source ? source : "?");
    s.id = id;
    s.id_ready = true;
    return id;
}

// The first line of [net] account_token_file (its first kTokenFileRead bytes), "" when it is unset or cannot be read.
std::string token_file_line() {
    const std::string token_path = config_get_str("net", "account_token_file", "");
    if (token_path.empty()) return {};
    FILE* fp = std::fopen(token_path.c_str(), "rb");
    if (fp == nullptr) {
        if (first_time("token_file")) NXLOG_W("acc", "[net] account_token_file %s cannot be read: no id token", token_path.c_str());
        return {};
    }
    std::vector<char> chunk(kTokenFileRead);
    const size_t got = std::fread(chunk.data(), 1, chunk.size(), fp);
    std::fclose(fp);
    std::string line(chunk.data(), got);
    const size_t eol = line.find_first_of("\r\n");
    if (eol != std::string::npos) line.resize(eol);
    return line;
}

// The id token: [net] account_token, else the first line of [net] account_token_file; blanks at either end dropped, at most 0xC00 bytes.
std::string account_token() {
    std::string t = config_get_str("net", "account_token", "");   // [net] account_token wins over the file
    if (t.empty()) t = token_file_line();
    t = trim_blanks(t);
    if (t.size() > kTokenMax) {
        if (first_time("token_long")) NXLOG_W("acc", "the account token has %zu bytes: only the first %zu are used", t.size(), kTokenMax);
        t.resize(kTokenMax);
    }
    return t;
}

// LoadIdTokenCache: the token (NUL after it when the buffer has room), or 0x100 zero bytes without one; the reply is the count written.
uint32_t load_id_token(const ipc::Request& req, ipc::Response& rsp, const Uid& u, uint32_t cmd) {
    const ipc::Buffer b = req.out_buf(0);
    const std::string t = account_token();
    uint64_t n = 0;
    if (!t.empty()) {
        if (b.addr) {
            n = std::min<uint64_t>(b.size, t.size());
            buf_write(b, t.data(), n);
            if (b.size > n) buf_mut(b)[n] = 0;
        }
    } else if (b.addr) {
        n = std::min<uint64_t>(b.size, kNoTokenBytes);
        const std::vector<uint8_t> zeros(n, 0);
        buf_write(b, zeros.data(), n);
    }
    if (first_time("mgr" + std::to_string(cmd)))
        NXLOG_I("acc", "IManagerForApplication cmd %u LoadIdTokenCache for %s (first use): %s, %llu bytes", cmd, uid_text(u).c_str(),
                t.empty() ? "no token" : "the account token", static_cast<unsigned long long>(n));
    rsp.push<uint32_t>(static_cast<uint32_t>(n));
    return 0;
}

// ---- IManagerForApplication ------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_manager(Uid u) {
    return make_iface("IManagerForApplication", [u](ipc::Service& s) {
        s.add_cmd(0, "CheckAvailability", [u](ipc::Request&, ipc::Response&) -> uint32_t {
            if (first_time("mgr0")) NXLOG_I("acc", "IManagerForApplication cmd 0 CheckAvailability for %s (first use): available", uid_text(u).c_str());
            return 0;
        });
        s.add_cmd(1, "GetAccountId", [u](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const uint64_t id = network_account_id(u);
            if (first_time("mgr1"))
                NXLOG_I("acc", "IManagerForApplication cmd 1 GetAccountId for %s (first use): 0x%016llx", uid_text(u).c_str(),
                        static_cast<unsigned long long>(id));
            rsp.push<uint64_t>(id);
            return 0;
        });
        s.add_cmd(2, "EnsureIdTokenCacheAsync", [u](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            if (first_time("mgr2")) NXLOG_I("acc", "IManagerForApplication cmd 2 EnsureIdTokenCacheAsync for %s (first use): done", uid_text(u).c_str());
            rsp.push_object(make_async_context(0, false, false));
            return 0;
        });
        s.add_cmd(3, "LoadIdTokenCacheDeprecated", [u](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return load_id_token(req, rsp, u, 3); });
        s.add_cmd(4, "LoadIdTokenCache", [u](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return load_id_token(req, rsp, u, 4); });
        s.add_cmd(170, "LoadNetworkServiceLicenseKindAsync", [u](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const bool has_token = !account_token().empty();
            const bool licensed = has_token && redirect::active();   // the redirect is asked only with a token
            if (first_time("mgr170"))
                NXLOG_I("acc", "IManagerForApplication cmd 170 LoadNetworkServiceLicenseKindAsync for %s (first use): %s", uid_text(u).c_str(),
                        licensed ? "licensed (account token and [net] redirect)" : "no licence");
            rsp.push_object(make_async_context(licensed ? 0 : kNoInternet, true, licensed));
            return 0;
        });
    });
}

// ---- IProfile --------------------------------------------------------------------------------------------------------------------
// The picture: [account] avatar when it is a JPEG (first 1 MiB), else the stand-in. Chosen once per process.
const std::vector<uint8_t>& avatar() {
    static const std::vector<uint8_t> img = [] {
        const std::string picture_path = config_get_str("account", "avatar", "");
        if (!picture_path.empty()) {
            std::vector<uint8_t> v(kAvatarMax);
            size_t n = 0;
            if (FILE* fp = std::fopen(picture_path.c_str(), "rb")) {
                n = std::fread(v.data(), 1, v.size(), fp);
                std::fclose(fp);
            }
            if (n > 4 && v[0] == 0xFF && v[1] == 0xD8) {
                v.resize(n);
                NXLOG_I("acc", "profile picture: %s (%zu bytes)", picture_path.c_str(), n);
                return v;
            }
            NXLOG_W("acc", "[account] avatar %s is not a readable JPEG: the built-in picture is used", picture_path.c_str());
        }
        const unsigned char* const builtin = kDefaultAvatarJpeg;
        return std::vector<uint8_t>(builtin, builtin + kDefaultAvatarJpegSize);
    }();
    return img;
}

// nn::account::ProfileBase: uid, last edit time stamp, nickname (0x20 bytes, no terminator needed).
std::array<uint8_t, 0x38> profile_base(const Uid& u) {
    std::array<uint8_t, 0x38> b{};
    std::memcpy(b.data(), &u, 16);
    const uint64_t t = kProfileCreated;
    std::memcpy(b.data() + 0x10, &t, 8);
    const std::string& user_name = profile().user_nickname;
    std::memcpy(b.data() + 0x18, user_name.data(), std::min<size_t>(user_name.size(), 0x20));
    return b;
}

std::shared_ptr<ipc::Service> make_profile(Uid u) {
    return make_iface("IProfile", [u](ipc::Service& s) {
        s.add_cmd(0, "Get", [u](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const ipc::Buffer b = req.out_buf(0);
            const std::array<uint8_t, kUserDataBytes> user_data{};   // nn::account::UserData: no icon or colour chosen
            buf_write(b, user_data.data(), std::min<uint64_t>(b.size, kUserDataBytes));
            const auto base = profile_base(u);
            rsp.out.insert(rsp.out.end(), base.begin(), base.end());
            return 0;
        });
        s.add_cmd(1, "GetBase", [u](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const auto base = profile_base(u);
            rsp.out.insert(rsp.out.end(), base.begin(), base.end());
            return 0;
        });
        s.add_cmd(10, "GetImageSize", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(static_cast<uint32_t>(avatar().size()));
            return 0;
        });
        s.add_cmd(11, "LoadImage", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const std::vector<uint8_t>& img = avatar();
            const uint64_t n = buf_write(req.out_buf(0), img.data(), img.size());
            rsp.push<uint32_t>(static_cast<uint32_t>(n));
            return 0;
        });
    });
}

// ---- acc:u0 / acc:u1 --------------------------------------------------------------------------------------------------------------
// An object for an existing user (GetProfile, GetBaasAccountManagerForApplication, LoadOpenContext), or the reason there is none.
uint32_t user_object(ipc::Request& req, ipc::Response& rsp, const char* what, std::shared_ptr<ipc::Service> (*make)(Uid)) {
    const Uid u = uid_arg(req);
    const uint32_t r = u.null() ? kInvalidUserId : !user_exists(u) ? kUserNotExist : 0;
    if (first_time(std::string(what) + uid_text(u)))
        NXLOG_I("acc", "%s(%s) -> 0x%x", what, uid_text(u).c_str(), r);
    if (r == 0) rsp.push_object(make(u));
    return r;
}

void add_common(ipc::Service& s) {
    s.add_cmd(0, "GetUserCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int32_t>(1);
        return 0;
    });
    s.add_cmd(1, "GetUserExistence", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(user_exists(uid_arg(req)) ? 1 : 0);
        return 0;
    });
    s.add_cmd(2, "ListAllUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        write_users(req, {local_uid()});
        return 0;
    });
    s.add_cmd(3, "ListOpenUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        write_users(req, open_users());
        return 0;
    });
    s.add_cmd(4, "GetLastOpenedUser", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push(local_uid());
        return 0;
    });
    s.add_cmd(5, "GetProfile", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return user_object(req, rsp, "GetProfile", &make_profile);
    });
    // true: a user can be added (nn::ShowUserCreator aborts on false; the applet then answers cancelled, am.cpp)
    s.add_cmd(50, "IsUserRegistrationRequestPermitted", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(1);
        return 0;
    });
    const auto select = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (req.arg<uint8_t>(0)) NXLOG_D("acc", "TrySelectUserWithoutInteraction: a network service account is asked for");
        rsp.push(local_uid());
        return 0;
    };
    s.add_cmd(51, "TrySelectUserWithoutInteractionDeprecated", select);
    s.add_cmd(52, "TrySelectUserWithoutInteraction", select);
    s.add_cmd(110, "StoreSaveDataThumbnail", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        if (uid_arg(req).null()) return kInvalidUserId;
        return req.in_buf(0).size == kThumbnailBytes ? 0 : kInvalidArrayLength;   // the image is not kept
    });
    s.add_cmd(111, "ClearSaveDataThumbnail", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        return uid_arg(req).null() ? kInvalidUserId : 0;
    });
}

std::shared_ptr<ipc::Service> make_application_service() {
    return make_iface("acc:u0", [](ipc::Service& s) {
        add_common(s);
        const auto init = [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; };
        s.add_cmd(100, "InitializeApplicationInfoV0", init);
        s.add_cmd(140, "InitializeApplicationInfo", init);
        s.add_cmd(160, "InitializeApplicationInfoV2", init);
        s.add_cmd(101, "GetBaasAccountManagerForApplication", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            return user_object(req, rsp, "GetBaasAccountManagerForApplication", &make_manager);
        });
        s.add_cmd(130, "LoadOpenContext", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            return user_object(req, rsp, "LoadOpenContext", &make_manager);
        });
        s.add_cmd(131, "ListOpenContextStoredUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            write_users(req, open_users());
            return 0;
        });
        s.add_cmd(141, "ListQualifiedUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            write_users(req, {local_uid()});
            return 0;
        });
        s.add_cmd(150, "IsUserAccountSwitchLocked", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_system_service() {
    return make_iface("acc:u1", [](ipc::Service& s) {
        add_common(s);
        s.add_cmd(140, "ListQualifiedUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            write_users(req, {local_uid()});
            return 0;
        });
    });
}

}  // namespace

namespace accapi {
void user_uid(uint64_t* hi, uint64_t* lo) {
    const Uid me = local_uid();
    if (hi != nullptr) *hi = me.hi;
    if (lo != nullptr) *lo = me.lo;
}
}  // namespace accapi

void register_acc() {
    add("acc:u0", [] { return make_application_service(); });
    add("acc:u1", [] { return make_system_service(); });
}

void acc_reset() {
    State& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.open_users.clear();
        s.open_ready = false;
        s.id_ready = false;
        s.id = 0;
    }
    std::lock_guard<std::mutex> lk(s.log_mu);
    s.logged.clear();
}

}  // namespace nxrt::services
