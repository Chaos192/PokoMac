// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// LAN room (lan_room.h): a game's own LAN mode on a relay room. Game-independent.
#include "lan_room.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "../net/lobby.h"
#include "../net/relay_server.h"
#include "../net/vnet.h"
#include "net_status.h"

namespace nxrt::services::lanroom {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kRetryFirstMs = 3000, kRetryMaxMs = 60000;   // background retries after a failure: 3 s, doubling to 60 s

std::string lower_trim(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    return s;
}

const char* left_str(vnet::LeftReason r) {
    switch (r) {
    case vnet::LeftReason::YouLeft: return "left";
    case vnet::LeftReason::HostClosed: return "the player who opened it left";
    case vnet::LeftReason::Rejected: return "removed by the host";
    case vnet::LeftReason::Shutdown: return "the relay shut down";
    case vnet::LeftReason::ConnectionLost: return "the connection to the relay was lost";
    }
    return "?";
}

struct State {
    std::mutex mu;                        // ensure() / reset(); never taken by the lobby's event sinks
    std::shared_ptr<vnet::Lobby> lobby;   // the relay client, kept between attempts
    std::string target;                   // its host:port, for the log
    std::string last_why;                 // the last failure logged (the same one is not logged again)
    std::atomic<uint32_t> vip{0};         // in the room: this node's address (host order)
    std::atomic<uint32_t> room{0};        // in the room: the relay's room id
    std::atomic<size_t> players{0};       // the last room state's node count (logged when it changes)
    // source(): whether the last attempt failed, when the next background one may start, its pause, and whether one runs
    std::atomic<bool> failed{false};
    std::atomic<int64_t> next_ms{0};
    std::atomic<int> backoff_ms{kRetryFirstMs};
    std::atomic<bool> busy{false};
    std::atomic<bool> held{false};        // the player left from the launcher (net-control "leave"): no joining until "join"
    // what net-status.json shows ("lan_room"); info_mu is taken by the event sinks too, and nothing is called while holding it
    std::mutex info_mu;
    std::string state, reason_code, reason, relay, code;
    std::vector<vnet::RoomNode> nodes;
};
State& st() {
    static State* s = new State;   // never destroyed: a background attempt may still run while the process exits
    return *s;
}
int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

// Writes the "lan_room" part of net-status.json from the state (any thread; takes info_mu only).
void publish(State& s) {
    std::string o, code;
    {
        std::lock_guard<std::mutex> lk(s.info_mu);
        if (s.state.empty()) return;
        const uint32_t me = s.vip.load();
        o = "{\"state\":" + net_status::json_str(s.state) + ",\"relay\":" + net_status::json_str(s.relay) + ",\"room\":" +
            std::to_string(s.room.load()) + ",\"address\":" + net_status::json_str(me ? vnet::ip_str(me) : "") + ",\"players\":[";
        bool first = true;
        for (const vnet::RoomNode& n : s.nodes) {
            o += std::string(first ? "" : ",") + "{\"name\":" + net_status::json_str(n.nickname) + ",\"address\":" +
                 net_status::json_str(vnet::ip_str(n.vip)) + ",\"you\":" + (n.vip == me ? "true" : "false") + "}";
            first = false;
        }
        o += "],\"reason_code\":" + net_status::json_str(s.reason_code) + ",\"reason\":" + net_status::json_str(s.reason) + "}";
        code = s.code;
    }
    net_status::set_common(code, config_get_str("net", "server", "auto"));
    net_status::set_part("lan_room", o);
}

// Sets the shown state (and clears the room's players when out of it), then publishes.
void set_state(State& s, const char* state, const char* reason_code = "", const std::string& reason = "") {
    {
        std::lock_guard<std::mutex> lk(s.info_mu);
        s.state = state;
        s.reason_code = reason_code;
        s.reason = reason;
        if (std::string(state) != "in_room") s.nodes.clear();
    }
    publish(s);
}

void fail(State& s, const char* reason_code, const std::string& why) {
    if (why != s.last_why) NXLOG_W("net", "lan room: %s (the game's LAN mode finds no one; trying again at its next network request)", why.c_str());
    s.last_why = why;
    set_state(s, "failed", reason_code, why);
}

// A relay's answer that refuses us (password, build, version, room code ...) or no answer at all.
const char* reason_of(int lobby_err) { return lobby_err < vnet::kLobbyTimeout ? "relay_refused" : "relay_unreachable"; }

// [net] room_code: empty = the title's open LAN room; a code = its key; "new" = one code for this process, logged. False: not a code.
bool room_key_of(std::array<uint8_t, 16>* key, std::string* shown) {
    static std::string made;   // "new": the same code for the whole run
    const std::string v = config_get_str("net", "room_code", "");
    std::string norm;
    if (v.empty()) {
        *key = {};
        shown->clear();
        return true;
    }
    if (lower_trim(v) == "new") {
        if (made.empty()) made = vnet::room_code_new();
        norm = made;
        if (norm.empty()) return false;
    } else if (!vnet::room_code_normalize(v, &norm)) {
        return false;
    }
    *key = vnet::room_key(norm);
    *shown = vnet::room_code_format(norm);
    return true;
}

// [net] host_listen as Link Play reads it: empty or "all" = every address (0), else one IPv4 address.
uint32_t host_listen() {
    const std::string v = lower_trim(config_get_str("net", "host_listen", ""));
    unsigned a, b, c, d;
    char tail;
    if (!v.empty() && v != "all" && std::sscanf(v.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
        return vnet::ip(static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c), static_cast<uint8_t>(d));
    return 0;
}

// The relay this run's LAN room goes through. [net] server = host:port: that one. auto: a relay LAN discovery finds (asked twice, the
// second time after a random pause, so two players starting together do not both start their own), else the relay inside this process
// on port 39660 (started once and never stopped; a port already taken is another relay on this PC and is used as it is). off: none.
bool pick_relay(std::string* host, uint16_t* port, std::string* why) {
    const std::string server = config_get_str("net", "server", "auto");
    const std::string v = lower_trim(server);
    if (v.empty() || v == "off") {
        *why = "[net] server is off: no relay to meet on";
        return false;
    }
    if (v != "auto") {
        if (vnet::parse_server(server, host, port)) return true;
        *why = "[net] server = \"" + server + "\" is not host:port, auto or off";
        return false;
    }
    std::mt19937 rng(std::random_device{}());
    for (int round = 0; round < 2; ++round) {
        const std::vector<vnet::FoundRelay> found = vnet::discover_relays(vnet::kRelayDefaultPort, 400);
        if (!found.empty()) {
            const vnet::FoundRelay* best = &found[0];   // a relay on another machine over this PC's own (127.0.0.1) answer
            for (const vnet::FoundRelay& f : found)
                if ((f.ip >> 24) != 127) { best = &f; break; }
            *host = vnet::ip_str(best->ip);
            *port = best->port;
            return true;
        }
        if (round == 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(200, 1500)(rng)));
    }
    static std::mutex mu;
    static vnet::RelayServer* server_here = nullptr;   // deliberately never deleted: it serves the room until the process exits
    const uint32_t listen = host_listen();
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!server_here) {
            vnet::RelayServerOptions o;
            o.port = vnet::kRelayDefaultPort;
            o.listen_ip = listen;
            o.password = config_get_str("net", "password", "");
            o.nickname = profile().user_nickname.empty() ? "Player" : profile().user_nickname;
            o.server_version = std::string("nxrt relay (") + vnet::build_stamp() + ") #" + vnet::room_code_new();
            std::string err;
            bool in_use = false;
            if (auto srv = vnet::RelayServer::start(o, &err, &in_use)) {
                server_here = srv.release();
                for (const vnet::LocalAddr& a : vnet::local_ipv4())
                    if (!vnet::is_link_local(a.ip) && (!listen || listen == a.ip))
                        NXLOG_I("net", "lan room: the relay runs in this game; players elsewhere set [net] server = %s:%u (%s)", vnet::ip_str(a.ip).c_str(),
                                server_here->port(), a.adapter.c_str());
            } else if (!in_use) {
                *why = "cannot start a relay here: " + err;
                return false;
            }
        }
    }
    *host = listen ? vnet::ip_str(listen) : std::string("127.0.0.1");
    *port = vnet::kRelayDefaultPort;
    return true;
}

// Events of the lobby (its reader thread): atomics, the current link and info_mu, never State::mu.
void wire_events(State& s, const std::shared_ptr<vnet::Lobby>& l) {
    vnet::LobbyEvents ev;
    ev.room_state = [&s](const vnet::Room& r) {
        const size_t n = r.nodes.size();
        if (s.players.exchange(n) != n) {
            std::string names;
            for (const vnet::RoomNode& nd : r.nodes) names += (names.empty() ? "" : ", ") + nd.nickname + " " + vnet::ip_str(nd.vip);
            NXLOG_I("net", "lan room %u: %zu player(s): %s", r.room_id, n, names.c_str());
        }
        {
            std::lock_guard<std::mutex> lk(s.info_mu);
            s.nodes = r.nodes;
        }
        publish(s);
    };
    ev.left = [&s](vnet::LeftReason why) {
        const uint32_t room = s.room.exchange(0);
        s.vip = 0;
        s.players = 0;
        vnet::set_base_link(nullptr);   // a local wireless room the game is in stays current; otherwise the sockets have no network
        NXLOG_W("net", "lan room %u: out of the room (%s): no network until the game asks for one again", room, left_str(why));
        const char* code = why == vnet::LeftReason::HostClosed ? "opener_left" : why == vnet::LeftReason::Rejected ? "removed" : "connection_lost";
        set_state(s, "left", code, std::string("Out of the room: ") + left_str(why) + ". The game's next LAN search joins or opens a room again.");
    };
    l->set_events(std::move(ev));
}

bool advert_is_lan(const vnet::Room& r) {
    return r.comm_version == kLanVersion && r.adv.size() >= sizeof kLanAdvert - 1 &&
           std::equal(r.adv.begin(), r.adv.begin() + (sizeof kLanAdvert - 1), kLanAdvert);
}

// The LAN rooms of this title the relay lists (with our key), lowest room id first, `except` left out.
int lan_rooms(vnet::Lobby& l, uint64_t lcid, uint32_t except, std::vector<vnet::Room>* out) {
    std::vector<vnet::Room> all;
    const int e = l.scan(lcid, kLanScene, &all);
    out->clear();
    for (const vnet::Room& r : all)
        if (r.room_id != except && r.local_comm_id == lcid && r.scene_id == kLanScene && advert_is_lan(r)) out->push_back(r);
    std::sort(out->begin(), out->end(), [](const vnet::Room& a, const vnet::Room& b) { return a.room_id < b.room_id; });
    return e;
}

// Joins the lowest LAN room of the title that takes us, or creates one; after creating, moves to a lower-numbered one another player
// created at the same time. kLobbyOk with *j filled, or the failure.
int join_or_create(vnet::Lobby& l, uint64_t lcid, vnet::Joined* j) {
    const auto pass = vnet::passphrase_hash(kLanPass, sizeof kLanPass - 1);
    std::vector<vnet::Room> rooms;
    int e = lan_rooms(l, lcid, 0, &rooms);
    if (e != vnet::kLobbyOk) return e;
    for (const vnet::Room& r : rooms)
        if (l.join(r.room_id, kLanVersion, pass, j) == vnet::kLobbyOk) return vnet::kLobbyOk;
    vnet::RoomConfig cfg;
    cfg.local_comm_id = lcid;
    cfg.scene_id = kLanScene;
    cfg.comm_version = kLanVersion;
    cfg.node_count_max = vnet::kMaxNodes;
    cfg.security_mode = 1;
    cfg.pass_hash = pass;
    cfg.adv.assign(kLanAdvert, kLanAdvert + sizeof kLanAdvert - 1);
    if ((e = l.create(cfg, j)) != vnet::kLobbyOk) return e;
    if (lan_rooms(l, lcid, j->room_id, &rooms) == vnet::kLobbyOk && !rooms.empty() && rooms.front().room_id < j->room_id) {
        const uint32_t mine = j->room_id, theirs = rooms.front().room_id;
        l.leave();
        if (l.join(theirs, kLanVersion, pass, j) == vnet::kLobbyOk) {
            NXLOG_I("net", "lan room: room %u opened at the same time as ours (%u): joined it instead", theirs, mine);
            return vnet::kLobbyOk;
        }
        return l.create(cfg, j);
    }
    return vnet::kLobbyOk;
}

}  // namespace

bool enabled() { return lower_trim(config_get_str("net", "lan", "off")) == "room"; }

namespace {
void on_control(const std::string& command);

// One attempt (caller holds s.mu): true when in the room afterwards.
bool attempt_locked(State& s) {
    if (s.lobby && s.vip && s.lobby->in_room()) return true;
    net_status::on_command("lan_room", on_control);   // the launcher's leave / join (net-control.txt)
    std::array<uint8_t, 16> key{};
    std::string shown;
    if (!room_key_of(&key, &shown)) {
        fail(s, "bad_code", "[net] room_code is not a room code (16 letters and digits, XXXX-XXXX-XXXX-XXXX)");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(s.info_mu);
        s.code = shown;
    }
    if (!s.lobby) {
        vnet::RelayOptions o;
        std::string why;
        if (!pick_relay(&o.host, &o.port, &why)) {
            fail(s, "no_relay", why);
            return false;
        }
        o.nickname = profile().user_nickname.empty() ? "Player" : profile().user_nickname;
        o.password = config_get_str("net", "password", "");
        o.room_key = key;
        const std::string udp = lower_trim(config_get_str("net", "udp", "auto"));
        o.udp = !(udp == "off" || udp == "0" || udp == "false" || udp == "no" || udp == "tcp");
        s.lobby = vnet::make_relay_client(o);
        s.target = o.host + ":" + std::to_string(o.port);
        wire_events(s, s.lobby);
    }
    {
        std::lock_guard<std::mutex> lk(s.info_mu);
        s.relay = s.target;
    }
    set_state(s, "joining");
    int e = s.lobby->connect();
    if (e != vnet::kLobbyOk) {
        fail(s, reason_of(e), "relay " + s.target + ": " + vnet::lobby_err_str(e));
        s.lobby->set_events({});
        s.lobby.reset();   // a relay picked by discovery may be gone: pick again next time
        return false;
    }
    const uint64_t lcid = program_id_now() & ~uint64_t{0xFFF};   // the application id: every program of one application meets in one room
    vnet::Joined j;
    if ((e = join_or_create(*s.lobby, lcid, &j)) != vnet::kLobbyOk) {
        fail(s, reason_of(e), "relay " + s.target + ": no LAN room: " + vnet::lobby_err_str(e));
        return false;
    }
    s.vip = j.vip;
    s.room = j.room_id;
    s.last_why.clear();
    {
        std::lock_guard<std::mutex> lk(s.info_mu);
        s.nodes = s.lobby->room().nodes;
    }
    set_state(s, "in_room");
    vnet::set_base_link(s.lobby->link());   // current at once unless the game is in a local wireless room (ldn), then when it leaves that
    NXLOG_I("net", "lan room %u of %016llx on relay %s%s%s: this player is %s/24 (node %u); the game's LAN mode plays over it", j.room_id,
            static_cast<unsigned long long>(lcid), s.target.c_str(), shown.empty() ? "" : ", room code ", shown.c_str(),
            vnet::ip_str(j.vip).c_str(), j.node_id);
    return true;
}

bool attempt(State& s) {
    std::lock_guard<std::mutex> lk(s.mu);
    const bool ok = attempt_locked(s);
    if (ok) {
        s.failed = false;
        s.backoff_ms = kRetryFirstMs;
    } else {
        const int b = s.backoff_ms.load();
        s.failed = true;
        s.next_ms = now_ms() + b;
        s.backoff_ms = std::min(b * 2, kRetryMaxMs);
    }
    return ok;
}

// net-control.txt (net_status.h), on its watcher thread. "leave": out of the room now, and no joining until "join"; the game sees its
// network go (nifm reports none, its sockets have no link). "join": joins again at once (in the background).
void on_control(const std::string& command) {
    State& s = st();
    if (command == "leave") {
        s.held = true;
        uint32_t room;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            room = s.room.exchange(0);
            s.vip = 0;
            s.players = 0;
            vnet::set_base_link(nullptr);
            if (s.lobby && s.lobby->in_room()) s.lobby->leave();
        }
        NXLOG_I("net", "lan room %u: left from the launcher; no LAN room until it says join", room);
        set_state(s, "left", "player_left", "You left the room. Join again from the launcher.");
    } else if (command == "join") {
        if (!s.held.exchange(false)) return;
        s.failed = false;
        s.backoff_ms = kRetryFirstMs;
        if (!s.busy.exchange(true))
            std::thread([&s] {
                attempt(s);
                s.busy = false;
            }).detach();
    }
}
}  // namespace

bool join_now() { return enabled() && !st().held && attempt(st()); }

bool source(uint32_t* ip, uint32_t* mask) {
    if (!enabled()) return false;
    State& s = st();
    if (ip_config(ip, mask)) return true;
    if (s.held) return false;   // the player left from the launcher
    if (!s.failed) {   // never tried, or the room was left after a success: the request waits for this attempt
        attempt(s);
        return ip_config(ip, mask);
    }
    if (now_ms() >= s.next_ms && !s.busy.exchange(true)) {
        std::thread([&s] {
            attempt(s);
            s.busy = false;
        }).detach();
    }
    return false;
}

bool ip_config(uint32_t* ip, uint32_t* mask) {
    const uint32_t v = st().vip.load();
    if (!v) return false;
    if (ip) *ip = v;
    if (mask) *mask = 0xFFFFFF00u;
    return true;
}

uint32_t room_id() { return st().room.load(); }

void reset() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.lobby) {
        s.lobby->set_events({});
        vnet::set_base_link(nullptr);
        if (s.lobby->in_room()) s.lobby->leave();
        s.lobby.reset();
    }
    s.target.clear();
    s.failed = false;
    s.next_ms = 0;
    s.backoff_ms = kRetryFirstMs;
    s.last_why.clear();
    s.vip = 0;
    s.room = 0;
    s.players = 0;
    s.held = false;
    net_status::set_part("lan_room", "");
    std::lock_guard<std::mutex> ik(s.info_mu);
    s.state.clear();
    s.reason_code.clear();
    s.reason.clear();
    s.relay.clear();
    s.code.clear();
    s.nodes.clear();
}

}  // namespace nxrt::services::lanroom
