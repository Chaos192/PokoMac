// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Local wireless between two processes (two PCs, or two copies on one): one hosts an ldn network, the other scans, joins, and both
// exchange UDP datagrams through bsd on the network's addresses, as a game's local play does. Every request goes through ldn:u and
// bsd:u as the SDK sends it; the network is the runtime's Link Play relay ([net] protocol = linkplay), so this covers the relay inside
// the host, LAN discovery, room codes and the datagram path end to end, with no game.
//
// It runs only when NXRT_LDN_PEER is set (the plain test run skips it):
//   NXRT_LDN_PEER          host | visit | relay (a relay alone, for NXRT_LDN_PEER_SECONDS: both peers set its address as server,
//                          so the ldn host can sit behind a firewall that only lets connections out)
//   NXRT_LDN_PEER_SERVER   [net] server (default auto: the host runs the relay, the visitor finds it by LAN discovery)
//   NXRT_LDN_PEER_CODE     [net] room_code (both sides; empty: an open room)
//   NXRT_LDN_PEER_LISTEN   [net] host_listen (127.0.0.1: two copies on one PC, nothing for a firewall to ask about); for relay, its address
//   NXRT_LDN_PEER_RELAY_PORT  relay: its TCP and UDP port (default 39660)
//   NXRT_LDN_PEER_SECONDS  how long each side waits for the other (default 120)
//   NXRT_LDN_PEER_PINGS    datagrams the visitor sends and expects answered (default 200)
//   NXRT_LDN_PEER_TCP      bytes the visitor sends over a TCP connection to the host after the pings, which the host echoes back
//                          (default 262144; 0: no TCP; the co-op protocol has none): TCP between the nodes (vsock.h, vtcp)
//   NXRT_LDN_PEER_LEAVE    visit (default: the visitor disconnects; the host must see the node leave) | host (the host destroys the
//                          network; the visitor must land in Station with DisconnectReason DestroyedByUser) | crash (the host process
//                          ends without a word, its relay with it: the visitor must land in Station with SignalLost)
//   NXRT_LDN_PEER_PROTOCOL [net] protocol: linkplay (default, every title) | native (Pokopia's: pokopia-native's co-op module, port
//                          47600, DNS-SD on the LAN; NXRT_LDN_PEER_MODE sets its [net] mode, e.g. join:<host address>) | lan (ldn_mitm's
//                          LAN protocol, UDP/TCP 11452, the machines' LAN addresses)
//   NXRT_LDN_PEER_LAN_IP   lan: [net] lan_ip (two copies on one Windows PC: 127.0.0.1 and 127.0.0.2)
//   NXRT_LDN_PEER_LAN_PEERS lan: [net] lan_peers (the other copy's address: the loopback has no broadcast)
//   NXRT_LDN_PEER_BEACONS  0: the visitor does not wait for the host's broadcasts (the loopback carries none)
// Run: NXRT_LDN_PEER=host nxrt_services_test ldnpeer   and   NXRT_LDN_PEER=visit nxrt_services_test ldnpeer   (TMP / TMPDIR apart
// for two copies on one machine: each test run clears its temporary folder).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "../../src/net/relay_server.h"
#include "nxrt/services.h"
#include "svc_test.h"

using namespace svctest;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kLcid = 0x0100A55717C40000ull;   // a made-up local communication id: only peers of this test meet
constexpr uint16_t kScene = 7;
constexpr uint16_t kVersion = 3;
constexpr uint16_t kPort = 49512;                    // the test's UDP port on the ldn network
constexpr uint16_t kTcpPort = 49513;                 // its TCP port (the host listens)
constexpr char kAdvert[] = "AnySwitch ldn peer test";
constexpr char kPass[] = "anyswitch-ldn-peer";
constexpr size_t kInfo = 0x480;                      // NetworkInfo

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

std::string ip_str(uint32_t a) {
    char b[24];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return b;
}

// ---- ldn:u raw layouts (services/ldn.cpp, Switchbrew) ----------------------------------------------------------------------------
std::vector<uint8_t> security_config() {   // 0x44: u16 mode, u16 passphrase size, 64 B passphrase
    std::vector<uint8_t> s(0x44, 0);
    const uint16_t mode = 1, n = static_cast<uint16_t>(std::strlen(kPass));
    std::memcpy(&s[0], &mode, 2);
    std::memcpy(&s[2], &n, 2);
    std::memcpy(&s[4], kPass, n);
    return s;
}
std::vector<uint8_t> user_config(const char* name) {   // 0x30: 33 B name, 15 reserved
    std::vector<uint8_t> u(0x30, 0);
    std::memcpy(&u[0], name, std::min<size_t>(std::strlen(name), 32));
    return u;
}
std::vector<uint8_t> network_config() {   // 0x20: IntentId (u64 lcid, u16, u16 scene, u32), u16 channel, u8 max, u8, u16 version, 10 B
    std::vector<uint8_t> c(0x20, 0);
    std::memcpy(&c[0], &kLcid, 8);
    std::memcpy(&c[10], &kScene, 2);
    c[0x12] = 8;                                // node count max (channel 0: auto)
    std::memcpy(&c[0x14], &kVersion, 2);
    return c;
}

struct Node {
    uint32_t ip;
    uint8_t mac[6];
    int8_t id;
    uint8_t connected;
    std::string name;
};
Node node_at(const uint8_t* ni, int i) {   // NetworkInfo: LdnNetworkInfo at 0x50, nodes at +0x18, 0x40 each
    const uint8_t* n = ni + 0x50 + 0x18 + 0x40 * i;
    Node o{};
    std::memcpy(&o.ip, n, 4);
    std::memcpy(o.mac, n + 4, 6);
    o.id = static_cast<int8_t>(n[10]);
    o.connected = n[11];
    o.name.assign(reinterpret_cast<const char*>(n + 12), strnlen(reinterpret_cast<const char*>(n + 12), 32));
    return o;
}
std::string advert_of(const uint8_t* ni) {   // advertise data size at 0x50 + 0x21A, data at + 0x21C
    uint16_t n = 0;
    std::memcpy(&n, ni + 0x50 + 0x21A, 2);
    return std::string(reinterpret_cast<const char*>(ni + 0x50 + 0x21C), std::min<size_t>(n, 384));
}

uint32_t state_of(const Session& s) {
    Reply r = s.call(0);
    return r.ok && r.result == 0 ? r.get<uint32_t>(0) : 0xFFFFFFFFu;
}

// ---- bsd:u ---------------------------------------------------------------------------------------------------------------------------
struct Sockaddr {   // FreeBSD sockaddr_in as bsd carries it
    uint8_t len = 16, family = 2;
    uint16_t port_be = 0;
    uint32_t ip_be = 0;
    uint8_t zero[8] = {};
};
Sockaddr sa(uint32_t ip, uint16_t port) {
    Sockaddr a;
    a.port_be = static_cast<uint16_t>((port >> 8) | (port << 8));
    a.ip_be = ((ip & 0xFF) << 24) | ((ip & 0xFF00) << 8) | ((ip >> 8) & 0xFF00) | (ip >> 24);
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
        Sockaddr a = sa(0, port);
        Reply b = bsd.call(13, Req().put(fd).buf(A(&a, sizeof a)));                              // Bind(0.0.0.0:port)
        return b.ok && b.get<int32_t>(0) == 0;
    }
    bool send(uint32_t ip, uint16_t port, const std::string& m) {
        Sockaddr a = sa(ip, port);
        Reply r = bsd.call(11, Req().put(fd).put<int32_t>(0).buf(A(m.data(), m.size())).buf(A(&a, sizeof a)));   // SendTo
        return r.ok && r.get<int32_t>(0) == static_cast<int32_t>(m.size());
    }
    // Waits up to ms for a datagram to be queued (Poll, POLLIN).
    void wait(int ms) {
        struct {
            int32_t fd;
            int16_t events, revents;
        } p{fd, 1, 0};
        bsd.call(6, Req().put<uint32_t>(1).put<int32_t>(ms).buf(A(&p, sizeof p)).buf(B(&p, sizeof p)));
    }
    // One datagram if one is queued (MSG_DONTWAIT): its text and sender.
    bool recv(std::string* m, uint32_t* from_ip) {
        char buf[512];
        Sockaddr from;
        Reply r = bsd.call(9, Req().put(fd).put<int32_t>(0x80).buf(B(buf, sizeof buf)).buf(B(&from, sizeof from)));   // RecvFrom
        if (!r.ok || r.get<int32_t>(0) <= 0) return false;
        m->assign(buf, static_cast<size_t>(r.get<int32_t>(0)));
        const uint32_t be = from.ip_be;
        *from_ip = ((be & 0xFF) << 24) | ((be & 0xFF00) << 8) | ((be >> 8) & 0xFF00) | (be >> 24);
        return true;
    }
};

// ---- TCP over the ldn network (bsd:u stream sockets) ---------------------------------------------------------------------------
int32_t tcp_socket(const Session& bsd) { return bsd.call(2, Req().put<int32_t>(2).put<int32_t>(1).put<int32_t>(0)).get<int32_t>(0); }
void tcp_timeout(const Session& bsd, int32_t fd, int seconds) {
    const int64_t tv[2] = {seconds, 0};
    bsd.call(21, Req().put(fd).put<int32_t>(0xFFFF).put<int32_t>(0x1006).buf(A(tv, sizeof tv)));   // SO_RCVTIMEO
}
int32_t tcp_send(const Session& bsd, int32_t fd, const uint8_t* p, size_t n) {
    return bsd.call(10, Req().put(fd).put<int32_t>(0).buf(A(p, n))).get<int32_t>(0);
}
int32_t tcp_recv(const Session& bsd, int32_t fd, uint8_t* p, size_t n) {
    return bsd.call(8, Req().put(fd).put<int32_t>(0).buf(B(p, n))).get<int32_t>(0);
}
std::vector<uint8_t> tcp_pattern(size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t x = 0x12345678;
    for (auto& b : v) {
        x = x * 1103515245u + 12345u;
        b = static_cast<uint8_t>(x >> 16);
    }
    return v;
}

// The host's echo: accepts one connection on kTcpPort and sends back every byte until the end of data. Its own bsd session.
void tcp_echo_host(int seconds, std::atomic<size_t>* echoed) {
    Session bsd = svctest::open("bsd:u");
    const int32_t ls = tcp_socket(bsd);
    Sockaddr a = sa(0, kTcpPort);
    CHECK_EQ(bsd.call(13, Req().put(ls).buf(A(&a, sizeof a))).get<int32_t>(0), 0);   // Bind
    CHECK_EQ(bsd.call(18, Req().put(ls).put<int32_t>(1)).get<int32_t>(0), 0);        // Listen
    tcp_timeout(bsd, ls, seconds);
    Sockaddr from;
    const int32_t c = bsd.call(12, Req().put(ls).buf(B(&from, sizeof from))).get<int32_t>(0);   // Accept
    if (c >= 0) {
        tcp_timeout(bsd, c, 30);
        std::vector<uint8_t> buf(16384);
        for (;;) {
            const int32_t n = tcp_recv(bsd, c, buf.data(), buf.size());
            if (n <= 0) break;
            for (int32_t done = 0; done < n;) {
                const int32_t k = tcp_send(bsd, c, buf.data() + done, static_cast<size_t>(n - done));
                if (k <= 0) break;
                done += k;
            }
            *echoed += static_cast<size_t>(n);
        }
        bsd.call(22, Req().put(c).put<int32_t>(1));   // Shutdown(SHUT_WR): the visitor reads its end of data
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        bsd.call(26, Req().put(c));
    }
    bsd.call(26, Req().put(ls));
}

// The visitor: one connection to the host's echo, n bytes out and back.
void tcp_visit(uint32_t host_ip, size_t n) {
    Session bsd = svctest::open("bsd:u");
    const int32_t fd = tcp_socket(bsd);
    tcp_timeout(bsd, fd, 30);
    Sockaddr to = sa(host_ip, kTcpPort);
    const auto t0 = Clock::now();
    Reply c = bsd.call(14, Req().put(fd).buf(A(&to, sizeof to)));   // Connect
    CHECK_EQ(c.get<int32_t>(0), 0);
    if (c.get<int32_t>(0) != 0) {
        line("visit: tcp connect to the host failed: errno %u", c.get<uint32_t>(4));
        return;
    }
    const std::vector<uint8_t> out = tcp_pattern(n);
    std::thread writer([&] {
        Session w = svctest::open("bsd:u");   // its own session (requests on one session are served one at a time)
        for (size_t done = 0; done < n;) {
            const int32_t k = tcp_send(w, fd, out.data() + done, std::min<size_t>(16384, n - done));
            if (k <= 0) break;
            done += static_cast<size_t>(k);
        }
        w.call(22, Req().put(fd).put<int32_t>(1));   // Shutdown(SHUT_WR)
    });
    std::vector<uint8_t> back;
    std::vector<uint8_t> buf(16384);
    for (;;) {
        const int32_t k = tcp_recv(bsd, fd, buf.data(), buf.size());
        if (k <= 0) break;
        back.insert(back.end(), buf.begin(), buf.begin() + k);
    }
    writer.join();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    line("visit: tcp %zu bytes out and %zu back in %.0f ms (%.1f MB/s each way)", n, back.size(), ms, ms > 0 ? n / ms / 1000.0 : 0.0);
    CHECK_EQ(back.size(), n);
    CHECK(back == out);
    if (back != out) {
        size_t i = 0;
        while (i < back.size() && i < out.size() && back[i] == out[i]) ++i;
        line("visit: tcp data differs from byte %zu on (%zu bytes back)", i, back.size());
    }
    bsd.call(26, Req().put(fd));
}

void configure(const char* nick) {
    nxrt::config_set("net", "protocol", env("NXRT_LDN_PEER_PROTOCOL", "linkplay").c_str());
    if (!env("NXRT_LDN_PEER_MODE").empty()) nxrt::config_set("net", "mode", env("NXRT_LDN_PEER_MODE").c_str());
    if (!env("NXRT_LDN_PEER_LAN_IP").empty()) nxrt::config_set("net", "lan_ip", env("NXRT_LDN_PEER_LAN_IP").c_str());
    if (!env("NXRT_LDN_PEER_LAN_PEERS").empty()) nxrt::config_set("net", "lan_peers", env("NXRT_LDN_PEER_LAN_PEERS").c_str());
    nxrt::config_set("net", "server", env("NXRT_LDN_PEER_SERVER", "auto").c_str());
    nxrt::config_set("net", "room_code", env("NXRT_LDN_PEER_CODE").c_str());
    if (!env("NXRT_LDN_PEER_LISTEN").empty()) nxrt::config_set("net", "host_listen", env("NXRT_LDN_PEER_LISTEN").c_str());
    nxrt::config_set("account", "nickname", nick);
    nxrt::services::profile_reload();   // the relay's player name comes from the profile
}

Session ldn_session() {
    Session l = open("ldn:u");
    Reply c = l.call(0);   // CreateUserLocalCommunicationService
    return l.object(c);
}

void run_host(int seconds, bool host_leaves, int pings) {
    configure("PeerHost");
    Session s = ldn_session();
    CHECK(s.valid());
    CHECK(s.call(400, Req().put<uint64_t>(0).with_pid()).result == 0);   // Initialize
    CHECK(s.call(200).result == 0);                                      // OpenAccessPoint
    std::vector<uint8_t> raw = security_config();
    const std::vector<uint8_t> u = user_config("PeerHost"), c = network_config();
    raw.insert(raw.end(), u.begin(), u.end());
    raw.resize(0x78, 0);
    raw.insert(raw.end(), c.begin(), c.end());
    Reply cr = s.call(202, Req().bytes(raw.data(), raw.size()));            // CreateNetwork
    CHECK_EQ(cr.result, 0u);
    if (cr.result) return;
    CHECK(s.call(206, Req().buf(A(kAdvert, sizeof kAdvert))).result == 0);   // SetAdvertiseData
    CHECK_EQ(state_of(s), 3u);
    Reply ip = s.call(2);                                                    // GetIpv4Address
    const uint32_t me = ip.get<uint32_t>(0), mask = ip.get<uint32_t>(4);
    line("host: network created, this node %s/%u, waiting %d s for a visitor", ip_str(me).c_str(), __builtin_popcount(mask), seconds);
    Udp udp;
    CHECK(udp.open(kPort));
    const size_t tcp_bytes = static_cast<size_t>(std::atoll(env("NXRT_LDN_PEER_TCP", env("NXRT_LDN_PEER_PROTOCOL") == "native" ? "0" : "262144").c_str()));
    std::atomic<size_t> echoed{0};
    std::thread echo;
    if (tcp_bytes) echo = std::thread([seconds, &echoed] { tcp_echo_host(seconds, &echoed); });
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    uint32_t visitor = 0;
    int answered = 0, joins = 0, leaves = 0, beacons = 0;
    auto last_beacon = Clock::now();
    bool left = false;
    while (Clock::now() < end && !left) {
        std::vector<uint8_t> ni(kInfo);
        uint8_t up[64] = {};
        Reply r = s.call(101, Req().buf(B(ni.data(), ni.size())).buf(B(up, sizeof up)));   // GetNetworkInfoLatestUpdate
        if (r.result) break;
        for (int i = 1; i < 8; ++i) {
            const Node n = node_at(ni.data(), i);
            if (up[i * 8] & 1) {
                ++joins;
                visitor = n.ip;
                line("host: node %d joined: %s \"%s\" mac %02x:%02x:%02x:%02x:%02x:%02x", i, ip_str(n.ip).c_str(), n.name.c_str(), n.mac[0],
                     n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
            }
            if (up[i * 8] & 2) {
                ++leaves;
                line("host: node %d left", i);
                left = visitor != 0 && !host_leaves;
            }
        }
        std::string m;
        uint32_t from = 0;
        while (udp.recv(&m, &from)) {
            if (m.rfind("ping ", 0) == 0) {
                CHECK(udp.send(from, kPort, "pong " + m.substr(5)));
                ++answered;
            }
        }
        if (visitor && Clock::now() - last_beacon > std::chrono::milliseconds(500)) {   // the network's broadcast address
            udp.send(me | ~mask, kPort, "beacon " + std::to_string(beacons++));
            last_beacon = Clock::now();
        }
        if (host_leaves && answered >= pings && (!tcp_bytes || echoed.load() >= tcp_bytes) && env("NXRT_LDN_PEER_LEAVE") == "crash") {
            // the echoed bytes are queued, not delivered: the stream ends first (the echo thread closes it), then the process
            if (echo.joinable()) echo.join();
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            line("host: %d pings answered, ending the process", answered);
            std::fflush(stdout);
            std::_Exit(0);
        }
        if (host_leaves && answered >= pings && (!tcp_bytes || echoed.load() >= tcp_bytes)) {
            line("host: %d pings answered, destroying the network", answered);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            break;
        }
        udp.wait(5);
    }
    line("host: %d joins, %d leaves, %d pings answered, %d beacons sent, %zu tcp bytes echoed", joins, leaves, answered, beacons, echoed.load());
    if (echo.joinable()) echo.join();
    if (tcp_bytes) CHECK_EQ(echoed.load(), tcp_bytes);
    CHECK(joins >= 1);
    CHECK(answered >= pings);
    if (!host_leaves) CHECK(leaves >= 1);
    CHECK(s.call(204).result == 0);   // DestroyNetwork
    CHECK_EQ(state_of(s), 2u);
    CHECK(s.call(201).result == 0);   // CloseAccessPoint
    CHECK(s.call(401).result == 0);   // Finalize
    // A game goes on running after it closes its network; the relay inside this process (server = auto) still has to tell the
    // visitors the room is gone. Ending the process at once can cut that message off (a visitor on another machine then sees the
    // connection drop: SignalLost, which is NXRT_LDN_PEER_LEAVE=crash's case, not this one).
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

void run_visit(int seconds, bool host_leaves, int pings) {
    configure("PeerVisit");
    Session s = ldn_session();
    CHECK(s.valid());
    CHECK(s.call(400, Req().put<uint64_t>(0).with_pid()).result == 0);   // Initialize
    CHECK(s.call(300).result == 0);                                      // OpenStation
    // Scan(s16 channel, ScanFilter at 8): the test's local communication id
    std::vector<uint8_t> f(0x60, 0);
    std::memcpy(&f[0], &kLcid, 8);
    const uint32_t flag = 1;   // LocalCommunicationId
    std::memcpy(&f[0x5C], &flag, 4);
    std::vector<uint8_t> found(kInfo * 8);
    int count = 0;
    const auto t0 = Clock::now();
    const auto end = t0 + std::chrono::seconds(seconds);
    while (Clock::now() < end) {
        Reply r = s.call(102, Req().put<int16_t>(0).bytes("\0\0\0\0\0\0", 6).bytes(f.data(), f.size()).buf(B(found.data(), found.size())));
        count = r.result == 0 ? static_cast<int>(r.get<uint32_t>(0)) : 0;
        if (count) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    const double scan_s = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK(count >= 1);
    if (!count) {
        line("visit: no network found in %d s", seconds);
        return;
    }
    const Node h = node_at(found.data(), 0);
    line("visit: found %d network(s) after %.1f s; host %s \"%s\", advertise \"%s\"", count, scan_s, ip_str(h.ip).c_str(), h.name.c_str(),
         advert_of(found.data()).c_str());
    CHECK(advert_of(found.data()) == std::string(kAdvert, sizeof kAdvert));
    std::vector<uint8_t> raw = security_config();
    const std::vector<uint8_t> u = user_config("PeerVisit");
    raw.insert(raw.end(), u.begin(), u.end());
    const uint32_t version = kVersion, option = 0;
    raw.resize(0x74);
    raw.insert(raw.end(), reinterpret_cast<const uint8_t*>(&version), reinterpret_cast<const uint8_t*>(&version) + 4);
    raw.insert(raw.end(), reinterpret_cast<const uint8_t*>(&option), reinterpret_cast<const uint8_t*>(&option) + 4);
    const auto c0 = Clock::now();
    Reply cr = s.call(302, Req().bytes(raw.data(), raw.size()).buf(A(found.data(), kInfo)));   // Connect
    CHECK_EQ(cr.result, 0u);
    if (cr.result) return;
    line("visit: connected in %.0f ms", std::chrono::duration<double, std::milli>(Clock::now() - c0).count());
    CHECK_EQ(state_of(s), 5u);
    Reply ip = s.call(2);
    const uint32_t me = ip.get<uint32_t>(0);
    std::vector<uint8_t> ni(kInfo);
    CHECK(s.call(1, Req().buf(B(ni.data(), ni.size()))).result == 0);   // GetNetworkInfo: node 0 is the host
    const Node host = node_at(ni.data(), 0);
    bool listed = false;
    for (int i = 1; i < 8; ++i) listed |= node_at(ni.data(), i).ip == me && node_at(ni.data(), i).connected;
    CHECK(listed && host.connected);
    line("visit: this node %s, host %s", ip_str(me).c_str(), ip_str(host.ip).c_str());
    Udp udp;
    CHECK(udp.open(kPort));
    int answered = 0, beacons = 0, sent = 0;
    std::vector<double> rtt;
    auto t_send = Clock::now();
    bool waiting = false;
    const auto pend = Clock::now() + std::chrono::seconds(seconds);
    while (answered < pings && Clock::now() < pend) {
        if (!waiting || Clock::now() - t_send > std::chrono::milliseconds(500)) {   // a lost datagram is sent again after 500 ms
            CHECK(udp.send(host.ip, kPort, "ping " + std::to_string(answered)));
            ++sent;
            t_send = Clock::now();
            waiting = true;
        }
        std::string m;
        uint32_t from = 0;
        while (udp.recv(&m, &from)) {
            if (m == "pong " + std::to_string(answered) && from == host.ip) {
                rtt.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t_send).count());
                ++answered;
                waiting = false;
            } else if (m.rfind("beacon ", 0) == 0) {
                ++beacons;
            }
        }
        udp.wait(2);
    }
    // the host's beacons (its network's broadcast address) come every 500 ms once it has seen this node: on a fast link the pings can
    // be over before the first one, so wait for it a while
    const bool want_beacons = env("NXRT_LDN_PEER_BEACONS", "1") != "0";
    for (const auto bend = Clock::now() + std::chrono::seconds(3); want_beacons && beacons == 0 && Clock::now() < bend;) {
        std::string m;
        uint32_t from = 0;
        while (udp.recv(&m, &from))
            if (m.rfind("beacon ", 0) == 0) ++beacons;
        udp.wait(20);
    }
    std::sort(rtt.begin(), rtt.end());
    line("visit: %d of %d pings answered (%d sent), %d beacons received; round trip median %.1f ms, 90th %.1f ms, max %.1f ms", answered,
         pings, sent, beacons, rtt.empty() ? 0 : rtt[rtt.size() / 2], rtt.empty() ? 0 : rtt[rtt.size() * 9 / 10], rtt.empty() ? 0 : rtt.back());
    CHECK(answered >= pings);
    if (want_beacons) CHECK(beacons >= 1);
    const size_t tcp_bytes = static_cast<size_t>(std::atoll(env("NXRT_LDN_PEER_TCP", env("NXRT_LDN_PEER_PROTOCOL") == "native" ? "0" : "262144").c_str()));
    if (tcp_bytes) tcp_visit(host.ip, tcp_bytes);
    if (host_leaves) {
        const auto wend = Clock::now() + std::chrono::seconds(30);
        uint32_t st = 5;
        while (Clock::now() < wend && (st = state_of(s)) == 5) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int16_t reason = s.call(3).get<int16_t>(0);   // GetDisconnectReason
        line("visit: the host left: state %u, disconnect reason %d", st, reason);
        CHECK_EQ(st, 4u);
        CHECK_EQ(reason, env("NXRT_LDN_PEER_LEAVE") == "crash" ? 6 : 3);   // SignalLost, DestroyedByUser
        CHECK(s.call(1, Req().buf(B(ni.data(), ni.size()))).result != 0);   // no network info outside a network
    } else {
        CHECK(s.call(304).result == 0);   // Disconnect
        CHECK_EQ(state_of(s), 4u);
        line("visit: disconnected");
    }
    CHECK(s.call(301).result == 0);   // CloseStation
    CHECK(s.call(401).result == 0);   // Finalize
}

}  // namespace

SVC_TEST(ldnpeer, run) {
    const std::string role = env("NXRT_LDN_PEER");
    if (role.empty()) return;   // only on request: it needs a second process
    const int seconds = std::atoi(env("NXRT_LDN_PEER_SECONDS", "120").c_str());
    const int pings = std::atoi(env("NXRT_LDN_PEER_PINGS", "200").c_str());
    const bool host_leaves = env("NXRT_LDN_PEER_LEAVE", "visit") != "visit";
    if (role == "relay") {
        nxrt::vnet::RelayServerOptions o;
        o.nickname = "Relay";
        if (!env("NXRT_LDN_PEER_RELAY_PORT").empty()) o.port = static_cast<uint16_t>(std::atoi(env("NXRT_LDN_PEER_RELAY_PORT").c_str()));
        if (env("NXRT_LDN_PEER_LISTEN") == "127.0.0.1") o.listen_ip = 0x7F000001;   // this PC only: nothing for a firewall to ask about
        o.server_version = "nxrt ldnpeer relay";
        std::string why;
        bool in_use = false;
        auto r = nxrt::vnet::RelayServer::start(o, &why, &in_use);
        CHECK(r != nullptr);
        if (!r) {
            line("relay: cannot start: %s", why.c_str());
            return;
        }
        line("relay: listening on port %u for %d s", r->port(), seconds);
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        r->stop();
        return;
    }
    if (role == "host") run_host(seconds, host_leaves, pings);
    else if (role == "visit") run_visit(seconds, host_leaves, pings);
    else CHECK(!"NXRT_LDN_PEER is host or visit");
}
