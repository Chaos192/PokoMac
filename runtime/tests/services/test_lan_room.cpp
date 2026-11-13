// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// LAN room (services/lan_room.h): a game's own LAN mode carried on a Link Play relay room. The datagrams follow Pia's LAN mode as the
// NintendoClients wiki ("LAN Protocol", Pia 6.16-7.2) describes it: a browse request broadcast to UDP port 35000, the reply sent back to
// the browser, then session traffic on 49152 (unicast, and keep-alives broadcast). The game side always goes through bsd:u as the SDK
// sends it; the room comes from the relay (vnet::RelayServer), as a game player's would.
//
//   lanroom.off_by_default    no [net] lan: nothing joins, the source answers false
//   lanroom.pia_lan_session   this process creates the title's LAN room on a relay at 127.0.0.1; a second player (a relay client of the
//                             test) joins it; browse broadcast, reply, broadcast keep-alives and 50 unicast datagrams both ways; the room
//                             closes for the other player when this one leaves
//   lanroom.join_and_codes    a LAN room another player opened first is joined (node 2); a player with another room code and another
//                             title's player do not see it
//   lanpeer.run               two processes (two PCs, or two copies on one), both through bsd:u, only when NXRT_LAN_PEER is set:
//     NXRT_LAN_PEER          host (Pia's session host: answers the browse) | browse (finds the host, then pings it) | relay (a relay alone,
//                            NXRT_LAN_PEER_SECONDS long: both peers set its address as server)
//     NXRT_LAN_PEER_SERVER   [net] server (default auto: the first player's game runs the relay, the other finds it by LAN discovery)
//     NXRT_LAN_PEER_CODE     [net] room_code (both sides; empty: the open LAN room)
//     NXRT_LAN_PEER_LISTEN   [net] host_listen, and the relay role's address (127.0.0.1: two copies on one PC). On the PC never leave
//                            it unset for the relay role or server = auto: a listener on every address can raise a firewall prompt.
//     NXRT_LAN_PEER_PORT     the relay role's port (default 39660; net's ldnpeer relay rows use 40660, so concurrent rows stay apart)
//     NXRT_LAN_PEER_SECONDS  how long each side waits for the other (default 120)
//     NXRT_LAN_PEER_PINGS    datagrams the browser sends and expects answered (default 200)
//   Run: NXRT_LAN_PEER=host nxrt_services_test lanpeer   and   NXRT_LAN_PEER=browse nxrt_services_test lanpeer   (TMP / TMPDIR apart for
//   two copies on one machine).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/services.h"
#include "../../src/net/lobby.h"
#include "../../src/net/relay_server.h"
#include "../../src/net/vnet.h"
#include "../../src/services/lan_room.h"
#include "../../src/services/net_status.h"
#include "svc_test.h"

using namespace svctest;
namespace vnet = nxrt::vnet;
namespace lanroom = nxrt::services::lanroom;

namespace {

using Clock = std::chrono::steady_clock;

constexpr char kProgram[] = "0x0100A55717C4A000";      // a made-up program id: only peers of this test meet
constexpr uint64_t kApp = 0x0100A55717C4A000ull;
constexpr uint16_t kBrowsePort = 35000, kSessionPort = 49152;   // Pia 6.16-7.2 LAN mode

std::string env(const char* k, const char* d = "") {
    const char* v = std::getenv(k);
    return v && *v ? v : d;
}

void line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

// ---- bsd:u (FreeBSD sockaddr_in as bsd carries it) --------------------------------------------------------------------------------
struct Sockaddr {
    uint8_t len = 16, family = 2;
    uint16_t port_be = 0;
    uint32_t ip_be = 0;
    uint8_t zero[8] = {};
};
uint32_t bswap(uint32_t v) { return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24); }
Sockaddr sa(uint32_t ip, uint16_t port) {
    Sockaddr a;
    a.port_be = static_cast<uint16_t>((port >> 8) | (port << 8));
    a.ip_be = bswap(ip);
    return a;
}

struct Udp {
    Session bsd;
    int32_t fd = -1;
    bool open(uint16_t port) {
        bsd = svctest::open("bsd:u");
        if (!bsd.valid()) return false;
        Reply s = bsd.call(2, Req().put<int32_t>(2).put<int32_t>(2).put<int32_t>(0));   // Socket(AF_INET, SOCK_DGRAM, 0)
        if (!s.ok || s.get<int32_t>(0) < 0) return false;
        fd = s.get<int32_t>(0);
        const int32_t on = 1;
        bsd.call(21, Req().put(fd).put<int32_t>(0xFFFF).put<int32_t>(0x20).buf(A(&on, 4)));   // SO_BROADCAST
        bsd.call(21, Req().put(fd).put<int32_t>(0xFFFF).put<int32_t>(0x4).buf(A(&on, 4)));    // SO_REUSEADDR
        Sockaddr a = sa(0, port);
        Reply b = bsd.call(13, Req().put(fd).buf(A(&a, sizeof a)));                              // Bind(0.0.0.0:port)
        return b.ok && b.get<int32_t>(0) == 0;
    }
    bool send(uint32_t ip, uint16_t port, const std::string& m) {
        Sockaddr a = sa(ip, port);
        Reply r = bsd.call(11, Req().put(fd).put<int32_t>(0).buf(A(m.data(), m.size())).buf(A(&a, sizeof a)));   // SendTo
        return r.ok && r.get<int32_t>(0) == static_cast<int32_t>(m.size());
    }
    void wait(int ms) {   // Poll(POLLIN) up to ms
        struct {
            int32_t fd;
            int16_t events, revents;
        } p{fd, 1, 0};
        bsd.call(6, Req().put<uint32_t>(1).put<int32_t>(ms).buf(A(&p, sizeof p)).buf(B(&p, sizeof p)));
    }
    bool recv(std::string* m, uint32_t* from_ip, uint16_t* from_port = nullptr) {   // MSG_DONTWAIT
        char buf[1024];
        Sockaddr from;
        Reply r = bsd.call(9, Req().put(fd).put<int32_t>(0x80).buf(B(buf, sizeof buf)).buf(B(&from, sizeof from)));   // RecvFrom
        if (!r.ok || r.get<int32_t>(0) <= 0) return false;
        m->assign(buf, static_cast<size_t>(r.get<int32_t>(0)));
        *from_ip = bswap(from.ip_be);
        if (from_port) *from_port = static_cast<uint16_t>((from.port_be >> 8) | (from.port_be << 8));
        return true;
    }
    // Waits up to ms for a datagram.
    bool recv_wait(std::string* m, uint32_t* from_ip, int ms, uint16_t* from_port = nullptr) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (recv(m, from_ip, from_port)) return true;
            wait(10);
        }
        return recv(m, from_ip, from_port);
    }
    void close() {
        if (fd >= 0) bsd.call(26, Req().put(fd));   // Close
        fd = -1;
    }
};

// ---- a second player of the in-process cases: a relay client of the test, its datagrams queued -----------------------------------
struct Peer {
    std::shared_ptr<vnet::Lobby> lobby;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<vnet::Datagram> in;
    std::atomic<bool> left{false};

    Peer(uint16_t port, const std::string& code, const char* nick) {
        vnet::RelayOptions o;
        o.host = "127.0.0.1";
        o.port = port;
        o.nickname = nick;
        if (!code.empty()) {
            std::string norm;
            vnet::room_code_normalize(code, &norm);
            o.room_key = vnet::room_key(norm);
        }
        lobby = vnet::make_relay_client(o);
        vnet::LobbyEvents ev;
        ev.left = [this](vnet::LeftReason) { left = true; };
        lobby->set_events(std::move(ev));
    }
    ~Peer() {
        lobby->link()->set_receiver(nullptr);
        lobby->set_events({});
        if (lobby->in_room()) lobby->leave();
    }
    std::vector<vnet::Room> lan_rooms(uint64_t app) {
        std::vector<vnet::Room> r;
        if (lobby->connect() == vnet::kLobbyOk) lobby->scan(app, lanroom::kLanScene, &r);
        return r;
    }
    void listen() {
        lobby->link()->set_receiver([this](const vnet::Datagram& d) {
            std::lock_guard<std::mutex> lk(mu);
            in.push_back(d);
            cv.notify_all();
        });
    }
    bool join(uint32_t room, vnet::Joined* j) {
        if (lobby->join(room, lanroom::kLanVersion, vnet::passphrase_hash(lanroom::kLanPass, sizeof lanroom::kLanPass - 1), j) != vnet::kLobbyOk)
            return false;
        listen();
        return true;
    }
    bool create(uint64_t app, vnet::Joined* j) {   // a LAN room as lan_room.cpp opens one
        vnet::RoomConfig c;
        c.local_comm_id = app;
        c.scene_id = lanroom::kLanScene;
        c.comm_version = lanroom::kLanVersion;
        c.node_count_max = vnet::kMaxNodes;
        c.security_mode = 1;
        c.pass_hash = vnet::passphrase_hash(lanroom::kLanPass, sizeof lanroom::kLanPass - 1);
        c.adv.assign(lanroom::kLanAdvert, lanroom::kLanAdvert + sizeof lanroom::kLanAdvert - 1);
        if (lobby->connect() != vnet::kLobbyOk || lobby->create(c, j) != vnet::kLobbyOk) return false;
        listen();
        return true;
    }
    bool send(uint32_t dst, uint16_t dport, uint16_t sport, const std::string& m) {
        vnet::Datagram d;
        d.dst_ip = dst;
        d.dst_port = dport;
        d.src_port = sport;
        d.data.assign(m.begin(), m.end());
        return lobby->link()->send(d);
    }
    // The next datagram to `port` whose text starts with `prefix`, within ms.
    bool next(uint16_t port, const std::string& prefix, vnet::Datagram* out, int ms) {
        std::unique_lock<std::mutex> lk(mu);
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            for (auto it = in.begin(); it != in.end(); ++it) {
                if (it->dst_port == port && it->data.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), it->data.begin())) {
                    *out = *it;
                    in.erase(it);
                    return true;
                }
            }
            if (cv.wait_until(lk, end) == std::cv_status::timeout) return false;
        }
    }
};

// A relay on 127.0.0.1 at a free port in 41000..48999.
std::unique_ptr<vnet::RelayServer> local_relay(uint16_t* port) {
    std::mt19937 rng(std::random_device{}());
    for (int i = 0; i < 20; ++i) {
        vnet::RelayServerOptions o;
        o.port = static_cast<uint16_t>(std::uniform_int_distribution<int>(41000, 48999)(rng));
        o.listen_ip = vnet::ip(127, 0, 0, 1);
        o.server_version = "lan room test relay";
        std::string why;
        bool in_use = false;
        if (auto s = vnet::RelayServer::start(o, &why, &in_use)) {
            *port = o.port;
            return s;
        }
    }
    return nullptr;
}

// The keys configure() sets, with the value each had before (or what reads as unset), put back by unconfigure().
struct Saved {
    const char* section;
    const char* key;
    const char* unset;
    std::string value;
};
std::vector<Saved> g_saved;

void configure(const std::string& server, const std::string& code, const char* nick) {
    if (g_saved.empty()) {
        g_saved = {{"net", "lan", "off", {}}, {"net", "server", "auto", {}}, {"net", "room_code", "", {}}, {"net", "host_listen", "", {}},
                   {"title", "program_id", "0", {}}, {"account", "nickname", "Player", {}}};
        for (Saved& s : g_saved) s.value = nxrt::config_get_str(s.section, s.key, s.unset);
    }
    nxrt::config_set("net", "lan", "room");
    nxrt::config_set("net", "server", server.c_str());
    nxrt::config_set("net", "room_code", code.c_str());
    nxrt::config_set("title", "program_id", kProgram);
    nxrt::config_set("account", "nickname", nick);
    nxrt::services::profile_reload();
}
void unconfigure() {
    lanroom::reset();
    for (const Saved& s : g_saved) nxrt::config_set(s.section, s.key, s.value.c_str());
    if (!g_saved.empty()) nxrt::services::profile_reload();
    g_saved.clear();
}

}  // namespace

SVC_TEST(lanroom, off_by_default) {
    unconfigure();
    CHECK(!lanroom::enabled());
    uint32_t ip = 1, mask = 1;
    CHECK(!lanroom::source(&ip, &mask));
    CHECK(!lanroom::join_now());
    CHECK(!lanroom::ip_config(&ip, &mask));
    CHECK_EQ(lanroom::room_id(), 0u);
    {
        Session n = open("nifm:u");
        Session g = n.object(n.call(4));   // CreateGeneralServiceOld
        CHECK(well_formed_error(g.call(15)));   // no network: GetCurrentIpConfigInfo fails as before
    }
    nxrt::config_set("net", "lan", "Room ");   // case and spaces do not matter
    CHECK(lanroom::enabled());
    nxrt::config_set("net", "lan", "");
}

SVC_TEST(lanroom, pia_lan_session) {
    uint16_t port = 0;
    auto relay = local_relay(&port);
    CHECK(relay != nullptr);
    if (!relay) return;
    const std::string code = vnet::room_code_format(vnet::room_code_new());
    configure("127.0.0.1:" + std::to_string(port), code, "LanHost");
    uint32_t me = 0, mask = 0;
    CHECK(lanroom::source(&me, &mask));   // the first call joins: nobody is there, so this player opens the room
    CHECK(lanroom::ip_config(&me, &mask));
    CHECK_EQ(mask, 0xFFFFFF00u);
    CHECK(vnet::is_link_local(me) && (me & 0xFF) == 1);   // the player who opens a room is node 1
    const uint32_t room = lanroom::room_id();
    CHECK(room != 0);
    CHECK(vnet::current_link() && vnet::current_link()->local_ip() == me);
    // nifm reports that network (net's nifm_now reads the source): GetCurrentIpConfigInfo = IpAddressSetting (dhcp, address, mask,
    // gateway: 0xD bytes, addresses in network order) + DnsSetting (9 bytes)
    {
        Session n = open("nifm:u");
        Session g = n.object(n.call(4));   // CreateGeneralServiceOld
        Reply r = g.call(15);
        CHECK_SHAPE(r, 0x16);
        if (r.data.size() >= 0xD) {
            const uint8_t* a = r.data.data();
            CHECK_EQ(uint32_t{a[1]} << 24 | uint32_t{a[2]} << 16 | uint32_t{a[3]} << 8 | a[4], me);
            CHECK_EQ(uint32_t{a[5]} << 24 | uint32_t{a[6]} << 16 | uint32_t{a[7]} << 8 | a[8], 0xFFFFFF00u);
        }
    }

    Peer b(port, code, "LanBrowser");
    std::vector<vnet::Room> rooms = b.lan_rooms(kApp);
    CHECK_EQ(rooms.size(), size_t{1});
    vnet::Joined j;
    CHECK(!rooms.empty() && b.join(rooms[0].room_id, &j));
    CHECK_EQ(j.room_id, room);
    CHECK(vnet::same_subnet(j.vip, me) && j.vip != me);

    Udp browse, session;
    CHECK(browse.open(kBrowsePort));
    CHECK(session.open(kSessionPort));
    // the browser's request, broadcast on the room's network, reaches the host's port 35000
    CHECK(b.send(me | 0xFF, kBrowsePort, kSessionPort, "PIA-LAN browse 7f3a"));
    std::string m;
    uint32_t from = 0;
    uint16_t from_port = 0;
    CHECK(browse.recv_wait(&m, &from, 3000, &from_port));
    CHECK(m == "PIA-LAN browse 7f3a");
    CHECK_EQ(from, j.vip);
    CHECK_EQ(from_port, kSessionPort);
    // the reply goes back to the browser (Pia sends it three times)
    for (int i = 0; i < 3; ++i) CHECK(session.send(from, from_port, "PIA-LAN reply 7f3a session 1"));
    vnet::Datagram d;
    CHECK(b.next(kSessionPort, "PIA-LAN reply 7f3a", &d, 3000));
    CHECK_EQ(d.src_ip, me);
    // keep-alives broadcast to 255.255.255.255 reach the other player as the room's broadcast, and are not looped back to the sender
    CHECK(session.send(0xFFFFFFFFu, kSessionPort, "PIA-LAN keepalive"));
    CHECK(b.next(kSessionPort, "PIA-LAN keepalive", &d, 3000));
    CHECK_EQ(d.dst_ip, me | 0xFF);
    // 50 session datagrams each way
    int got_host = 0, got_peer = 0;
    for (int i = 0; i < 50; ++i) {
        CHECK(b.send(me, kSessionPort, kSessionPort, "PIA-LAN data " + std::to_string(i)));
        CHECK(session.send(j.vip, kSessionPort, "PIA-LAN data " + std::to_string(i)));
    }
    const auto end = Clock::now() + std::chrono::seconds(5);
    while ((got_host < 50 || got_peer < 50) && Clock::now() < end) {
        while (session.recv(&m, &from))
            if (m.rfind("PIA-LAN data ", 0) == 0 && from == j.vip) ++got_host;
        while (got_peer < 50 && b.next(kSessionPort, "PIA-LAN data ", &d, 5)) ++got_peer;
        session.wait(5);
    }
    CHECK_EQ(got_host, 50);
    CHECK_EQ(got_peer, 50);
    while (session.recv(&m, &from)) CHECK(m != "PIA-LAN keepalive");   // nothing of its own came back
    browse.close();
    session.close();
    // this player leaves (the game ends): the room it opened closes for the other one
    unconfigure();
    CHECK(!vnet::current_link());
    const auto lend = Clock::now() + std::chrono::seconds(5);
    while (!b.left && Clock::now() < lend) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(b.left.load());
}

SVC_TEST(lanroom, join_and_codes) {
    uint16_t port = 0;
    auto relay = local_relay(&port);
    CHECK(relay != nullptr);
    if (!relay) return;
    const std::string code = vnet::room_code_format(vnet::room_code_new());
    Peer first(port, code, "FirstPlayer");
    vnet::Joined fj;
    CHECK(first.create(kApp, &fj));
    configure("127.0.0.1:" + std::to_string(port), code, "SecondPlayer");
    CHECK(lanroom::join_now());   // the room is there: joined, not opened again
    uint32_t me = 0;
    CHECK(lanroom::ip_config(&me, nullptr));
    CHECK_EQ(lanroom::room_id(), fj.room_id);
    CHECK(vnet::same_subnet(me, fj.vip) && (me & 0xFF) == 2);
    Udp u;
    CHECK(u.open(kSessionPort));
    CHECK(first.send(me, kSessionPort, kSessionPort, "PIA-LAN hello"));
    std::string m;
    uint32_t from = 0;
    CHECK(u.recv_wait(&m, &from, 3000));
    CHECK(m == "PIA-LAN hello" && from == fj.vip);
    u.close();
    // another code: the room is not listed; the open (no code) LAN room is a different one too
    Peer other(port, vnet::room_code_format(vnet::room_code_new()), "OtherCode");
    CHECK(other.lan_rooms(kApp).empty());
    Peer open_room(port, "", "NoCode");
    CHECK(open_room.lan_rooms(kApp).empty());
    // another title's players never see it
    CHECK(first.lan_rooms(kApp + 0x1000).empty());
    unconfigure();
}

// ---- the launcher's view: net-status.json and net-control.txt (services/net_status.h) ----------------------------------------------
namespace {
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
// Waits up to ms for the status file to hold every one of `want`.
bool status_has(const std::vector<std::string>& want, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const std::string t = slurp(nxrt::services::net_status::status_path());
        bool all = !t.empty();
        for (const std::string& w : want) all = all && t.find(w) != std::string::npos;
        if (all) return true;
        if (Clock::now() > end) {
            line("status file now: %s", t.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
void control(const std::string& text) {
    std::ofstream f(nxrt::services::net_status::control_path(), std::ios::binary | std::ios::app);
    f << text << "\n";
}
}  // namespace

SVC_TEST(net_status, file_and_commands) {
    namespace ns = nxrt::services::net_status;
    std::remove(ns::status_path().c_str());
    std::remove(ns::control_path().c_str());
    control("7 leave");   // an earlier run's line: never applied
    std::mutex mu;
    std::vector<std::string> got;
    ns::on_command("test", [&](const std::string& c) {
        std::lock_guard<std::mutex> lk(mu);
        got.push_back(c);
    });
    ns::reset_for_tests();
    ns::on_command("test", [&](const std::string& c) {
        std::lock_guard<std::mutex> lk(mu);
        got.push_back(c);
    });
    CHECK(ns::json_str("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    ns::set_common("AAAA-BBBB-CCCC-DDDD", "auto");
    ns::set_part("test_part", "{\"state\":\"x\"}");
    CHECK(status_has({"\"version\":1", "\"pid\":", "\"code\":\"AAAA-BBBB-CCCC-DDDD\"", "\"server\":\"auto\"", "\"test_part\":{\"state\":\"x\"}"}, 2000));
    ns::set_part("test_part", "");
    CHECK(status_has({"\"server\":\"auto\"}"}, 2000));   // the part is gone
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    control("8 leave");
    control("8 join");   // the same number again: once only
    control("9 join");
    const auto end = Clock::now() + std::chrono::seconds(3);
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (got.size() >= 2 || Clock::now() > end) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));   // one more look at the file: nothing new
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK_EQ(got.size(), size_t{2});
        CHECK(got.size() == 2 && got[0] == "leave" && got[1] == "join");
    }
    ns::reset_for_tests();
    std::remove(ns::status_path().c_str());
    std::remove(ns::control_path().c_str());
}

SVC_TEST(lanroom, status_and_leave) {
    namespace ns = nxrt::services::net_status;
    uint16_t port = 0;
    auto relay = local_relay(&port);
    CHECK(relay != nullptr);
    if (!relay) return;
    std::remove(ns::status_path().c_str());
    std::remove(ns::control_path().c_str());
    ns::reset_for_tests();
    const std::string code = vnet::room_code_format(vnet::room_code_new());
    configure("127.0.0.1:" + std::to_string(port), code, "StatusHost");
    uint32_t me = 0;
    CHECK(lanroom::source(&me, nullptr));
    CHECK(status_has({"\"code\":\"" + code + "\"", "\"lan_room\":{\"state\":\"in_room\"", "\"relay\":\"127.0.0.1:" + std::to_string(port) + "\"",
                      "\"address\":\"" + vnet::ip_str(me) + "\"", "\"name\":\"StatusHost\""}, 3000));
    Peer b(port, code, "StatusPeer");
    std::vector<vnet::Room> rooms = b.lan_rooms(kApp);
    vnet::Joined j;
    CHECK(!rooms.empty() && b.join(rooms[0].room_id, &j));
    CHECK(status_has({"\"name\":\"StatusPeer\"", "\"you\":false", "\"you\":true"}, 3000));
    // the launcher's "leave": out of the room, the game's network gone, and no joining again on the game's next request
    control("1 leave");
    CHECK(status_has({"\"state\":\"left\"", "\"reason_code\":\"player_left\""}, 3000));
    CHECK(!lanroom::ip_config(nullptr, nullptr));
    CHECK(!vnet::current_link());
    CHECK(!lanroom::source(&me, nullptr));
    const auto lend = Clock::now() + std::chrono::seconds(5);
    while (!b.left && Clock::now() < lend) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(b.left.load());   // this player opened the room: it closed
    // "join": back in a LAN room (a new one: the old closed with its opener)
    control("2 join");
    CHECK(status_has({"\"state\":\"in_room\"", "\"reason_code\":\"\""}, 5000));
    CHECK(lanroom::ip_config(&me, nullptr));
    CHECK(vnet::current_link() && vnet::current_link()->local_ip() == me);
    unconfigure();
    ns::reset_for_tests();
    std::remove(ns::status_path().c_str());
    std::remove(ns::control_path().c_str());
}

SVC_TEST(lanroom, status_on_failure) {
    namespace ns = nxrt::services::net_status;
    std::remove(ns::status_path().c_str());
    ns::reset_for_tests();
    configure("127.0.0.1:1", "", "NoRelay");   // nobody listens on port 1
    CHECK(!lanroom::join_now());
    CHECK(status_has({"\"lan_room\":{\"state\":\"failed\"", "\"reason_code\":\"relay_unreachable\""}, 3000));
    nxrt::config_set("net", "room_code", "not a code");
    lanroom::reset();
    CHECK(!lanroom::join_now());
    CHECK(status_has({"\"reason_code\":\"bad_code\""}, 3000));
    unconfigure();
    ns::reset_for_tests();
    std::remove(ns::status_path().c_str());
}

// ---- two processes ------------------------------------------------------------------------------------------------------------------
SVC_TEST(lanpeer, run) {
    const std::string role = env("NXRT_LAN_PEER");
    if (role.empty()) return;   // only on request: it needs a second process
    const int seconds = std::atoi(env("NXRT_LAN_PEER_SECONDS", "120").c_str());
    const int pings = std::atoi(env("NXRT_LAN_PEER_PINGS", "200").c_str());
    if (role == "relay") {
        vnet::RelayServerOptions o;
        o.nickname = "Relay";
        const int port = std::atoi(env("NXRT_LAN_PEER_PORT", "39660").c_str());
        if (port > 0 && port < 65536) o.port = static_cast<uint16_t>(port);
        unsigned a = 0, b = 0, c = 0, d = 0;   // NXRT_LAN_PEER_LISTEN: the relay's address (127.0.0.1 on the PC: no listener a firewall asks about)
        if (std::sscanf(env("NXRT_LAN_PEER_LISTEN").c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
            o.listen_ip = vnet::ip(static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c), static_cast<uint8_t>(d));
        std::string why;
        bool in_use = false;
        auto s = vnet::RelayServer::start(o, &why, &in_use);
        CHECK(s != nullptr);
        line("relay: %s on port %u for %d s", s ? "running" : why.c_str(), o.port, seconds);
        if (s) std::this_thread::sleep_for(std::chrono::seconds(seconds));
        return;
    }
    const bool host = role == "host";
    configure(env("NXRT_LAN_PEER_SERVER", "auto"), env("NXRT_LAN_PEER_CODE"), host ? "LanPeerHost" : "LanPeerBrowse");
    if (!env("NXRT_LAN_PEER_LISTEN").empty()) nxrt::config_set("net", "host_listen", env("NXRT_LAN_PEER_LISTEN").c_str());
    uint32_t me = 0, mask = 0;
    const auto t0 = Clock::now();
    while (!lanroom::source(&me, &mask) && Clock::now() - t0 < std::chrono::seconds(seconds))
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(me != 0);
    if (!me) return;
    line("%s: in LAN room %u as %s after %.1f s", role.c_str(), lanroom::room_id(), vnet::ip_str(me).c_str(),
         std::chrono::duration<double>(Clock::now() - t0).count());
    CHECK_EQ(mask, 0xFFFFFF00u);
    Udp browse, session;
    CHECK(session.open(kSessionPort));
    std::string m;
    uint32_t from = 0;
    uint16_t from_port = 0;
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    if (host) {
        CHECK(browse.open(kBrowsePort));
        int answered = 0, browses = 0, keepalives = 0;
        auto last_ka = Clock::now();
        bool done = false;
        while (Clock::now() < end && !done) {
            while (browse.recv(&m, &from, &from_port)) {
                if (m.rfind("PIA-LAN browse", 0) == 0) {
                    ++browses;
                    for (int i = 0; i < 3; ++i) CHECK(session.send(from, from_port, "PIA-LAN reply session 1"));
                }
            }
            while (session.recv(&m, &from)) {
                if (m.rfind("ping ", 0) == 0) {
                    CHECK(session.send(from, kSessionPort, "pong " + m.substr(5)));
                    ++answered;
                } else if (m == "bye") {
                    done = true;
                }
            }
            if (browses && Clock::now() - last_ka > std::chrono::milliseconds(500)) {
                CHECK(session.send(0xFFFFFFFFu, kSessionPort, "PIA-LAN keepalive " + std::to_string(keepalives++)));
                last_ka = Clock::now();
            }
            session.wait(5);
        }
        line("host: %d browse requests, %d pings answered, %d keep-alives sent, %s", browses, answered, keepalives,
             done ? "the browser said bye" : "timed out");
        CHECK(browses >= 1);
        CHECK(answered >= pings);
        CHECK(done);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        uint32_t host_ip = 0;
        auto last = Clock::now() - std::chrono::seconds(1);
        while (!host_ip && Clock::now() < end) {
            if (Clock::now() - last > std::chrono::milliseconds(500)) {   // browse every 500 ms until a host answers
                CHECK(session.send(0xFFFFFFFFu, kBrowsePort, "PIA-LAN browse"));   // as Pia does; the stack sends it as the room's broadcast
                last = Clock::now();
            }
            while (session.recv(&m, &from))
                if (m.rfind("PIA-LAN reply", 0) == 0) host_ip = from;
            session.wait(20);
        }
        CHECK(host_ip != 0);
        if (!host_ip) return;
        line("browse: host found at %s after %.1f s", vnet::ip_str(host_ip).c_str(), std::chrono::duration<double>(Clock::now() - t0).count());
        int answered = 0, keepalives = 0, sent = 0;
        std::vector<double> rtt;
        auto t_send = Clock::now();
        bool waiting = false;
        while (answered < pings && Clock::now() < end) {
            if (!waiting || Clock::now() - t_send > std::chrono::milliseconds(500)) {
                CHECK(session.send(host_ip, kSessionPort, "ping " + std::to_string(answered)));
                ++sent;
                t_send = Clock::now();
                waiting = true;
            }
            while (session.recv(&m, &from)) {
                if (m == "pong " + std::to_string(answered) && from == host_ip) {
                    rtt.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t_send).count());
                    ++answered;
                    waiting = false;
                } else if (m.rfind("PIA-LAN keepalive", 0) == 0) {
                    ++keepalives;
                }
            }
            session.wait(2);
        }
        for (int i = 0; i < 3; ++i) session.send(host_ip, kSessionPort, "bye");
        std::sort(rtt.begin(), rtt.end());
        line("browse: %d of %d pings answered (%d sent), %d keep-alives received; round trip median %.1f ms, 90th %.1f ms, max %.1f ms", answered,
             pings, sent, keepalives, rtt.empty() ? 0 : rtt[rtt.size() / 2], rtt.empty() ? 0 : rtt[rtt.size() * 9 / 10], rtt.empty() ? 0 : rtt.back());
        CHECK(answered >= pings);
        CHECK(keepalives >= 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    browse.close();
    session.close();
    unconfigure();
}
