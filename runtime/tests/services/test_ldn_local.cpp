// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Local wireless (ldn:u, [net] protocol = linkplay) semantics in one process: several ldn service objects, each on its own client of an
// in-process relay (vnet::LoopbackRelay), as several consoles in one room. Every request goes through ldn:u as the SDK sends it.
//   - the station accept policy and filter (SetStationAcceptPolicy, AddAcceptFilterEntry, ClearAcceptFilter): a station the host's
//     policy refuses gets ConnectRejected and never appears in the host's node table; nodes already in stay; a bad policy is refused.
// Layouts: services/ldn.cpp and test_ldn_peer.cpp (Switchbrew "LDN services", Ryujinx Ldn/Types).
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "../../src/net/lobby.h"
#include "../../src/services/ldn.h"
#include "../../src/services/net_status.h"
#include "svc_test.h"

namespace nxrt::services {
void ldn_reset();   // services/ldn.cpp: the process lobby goes back to the config
}

using namespace svctest;

namespace {

constexpr uint64_t kLcid = 0x0100A55717C40001ull;   // made up: only this test's nodes meet
constexpr size_t kInfo = 0x480;
constexpr uint32_t kConnectRejected = 203 | (67u << 9), kInvalidArgument = 203 | (96u << 9);

std::vector<uint8_t> security_config() {   // 0x44: u16 mode, u16 passphrase size, 64 B passphrase
    std::vector<uint8_t> s(0x44, 0);
    const uint16_t mode = 1;
    std::memcpy(&s[0], &mode, 2);
    return s;
}
std::vector<uint8_t> user_config(const char* name) {   // 0x30: 33 B name, 15 reserved
    std::vector<uint8_t> u(0x30, 0);
    std::memcpy(&u[0], name, std::strlen(name));
    return u;
}
std::vector<uint8_t> network_config() {   // 0x20: IntentId, u16 channel, u8 node count max, u8, u16 version, 10 B
    std::vector<uint8_t> c(0x20, 0);
    std::memcpy(&c[0], &kLcid, 8);
    c[0x12] = 8;
    const uint16_t version = 1;
    std::memcpy(&c[0x14], &version, 2);
    return c;
}

struct Node {
    uint32_t ip = 0;
    std::array<uint8_t, 6> mac{};
    uint8_t connected = 0;
};
Node node_at(const uint8_t* ni, int i) {   // NetworkInfo: LdnNetworkInfo at 0x50, nodes at +0x18, 0x40 each
    const uint8_t* n = ni + 0x50 + 0x18 + 0x40 * i;
    Node o;
    std::memcpy(&o.ip, n, 4);
    std::memcpy(o.mac.data(), n + 4, 6);
    o.connected = n[11];
    return o;
}

uint32_t state_of(const Session& s) {
    Reply r = s.call(0);
    return r.ok && r.result == 0 ? r.get<uint32_t>(0) : 0xFFFFFFFFu;
}

// A node: an ldn service object on its own client of the relay.
Session node(nxrt::vnet::LoopbackRelay& relay, const char* nick) {
    nxrt::services::ldn_set_lobby(relay.client(nick));   // taken by the next Initialize
    Session l = open("ldn:u");
    Session s = l.object(l.call(0));                       // CreateUserLocalCommunicationService
    CHECK(s.call(400, Req().put<uint64_t>(0).with_pid()).result == 0);   // Initialize
    return s;
}

uint32_t create(const Session& h) {
    std::vector<uint8_t> raw = security_config();
    const std::vector<uint8_t> u = user_config("Host"), c = network_config();
    raw.insert(raw.end(), u.begin(), u.end());
    raw.resize(0x78, 0);
    raw.insert(raw.end(), c.begin(), c.end());
    return h.call(202, Req().bytes(raw.data(), raw.size())).result;   // CreateNetwork
}

// OpenStation, Scan for the test's network, Connect: the Connect result.
uint32_t visit(const Session& g, const char* nick) {
    CHECK(g.call(300).result == 0);   // OpenStation
    std::vector<uint8_t> f(0x60, 0);
    std::memcpy(&f[0], &kLcid, 8);
    const uint32_t flag = 1;          // filter by LocalCommunicationId
    std::memcpy(&f[0x5C], &flag, 4);
    std::vector<uint8_t> found(kInfo * 4);
    Reply r = g.call(102, Req().put<int16_t>(0).bytes("\0\0\0\0\0\0", 6).bytes(f.data(), f.size()).buf(B(found.data(), found.size())));
    CHECK(r.result == 0 && r.get<uint32_t>(0) == 1);
    std::vector<uint8_t> raw = security_config();
    const std::vector<uint8_t> u = user_config(nick);
    raw.insert(raw.end(), u.begin(), u.end());
    raw.resize(0x74);
    const uint32_t version = 1, option = 0;
    raw.insert(raw.end(), reinterpret_cast<const uint8_t*>(&version), reinterpret_cast<const uint8_t*>(&version) + 4);
    raw.insert(raw.end(), reinterpret_cast<const uint8_t*>(&option), reinterpret_cast<const uint8_t*>(&option) + 4);
    return g.call(302, Req().bytes(raw.data(), raw.size()).buf(A(found.data(), kInfo))).result;   // Connect
}

std::vector<Node> members(const Session& h) {
    std::vector<uint8_t> ni(kInfo);
    CHECK(h.call(1, Req().buf(B(ni.data(), ni.size()))).result == 0);   // GetNetworkInfo
    std::vector<Node> v;
    for (int i = 1; i < 8; ++i)
        if (node_at(ni.data(), i).connected) v.push_back(node_at(ni.data(), i));
    return v;
}

uint32_t add_filter(const Session& h, const std::array<uint8_t, 6>& mac) { return h.call(208, Req().bytes(mac.data(), 6)).result; }

// A station the host's policy refuses: Connect answers ConnectRejected when the host's REJECT comes within Connect's wait (services/
// ldn.cpp join: two JOIN round trips, at least 100 ms); on a loaded machine it can come after, and the station is then put out of the
// network at once with DisconnectReason Rejected. Either way it never reaches the host's node table.
bool refused(const Session& g, const char* nick) {
    const uint32_t r = visit(g, nick);
    if (r == kConnectRejected) return state_of(g) == 4;
    if (r != 0) return false;
    for (int i = 0; i < 200 && state_of(g) == 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return state_of(g) == 4 && g.call(3).get<int16_t>(0) == 5;   // GetDisconnectReason: Rejected
}

}  // namespace

SVC_TEST(ldn_local, the_station_accept_policy_and_filter) {
    const std::string proto = nxrt::config_get_str("net", "protocol", "");
    nxrt::config_set("net", "protocol", "linkplay");
    nxrt::vnet::LoopbackRelay relay;
    Session host = node(relay, "Host");
    CHECK(host.call(200).result == 0);                                  // OpenAccessPoint
    CHECK_EQ(host.call(207, Req().put<uint8_t>(4)).result, kInvalidArgument);   // AcceptPolicy is 0..3
    CHECK(host.call(207, Req().put<uint8_t>(1)).result == 0);           // SetStationAcceptPolicy: reject all
    CHECK_EQ(create(host), 0u);
    CHECK_EQ(state_of(host), 3u);

    Session g1 = node(relay, "G1");
    CHECK(refused(g1, "G1"));                      // turned away, as the console's access point refuses the association
    CHECK_EQ(state_of(g1), 4u);                    // still a station, in no network
    CHECK(members(host).empty());

    CHECK(host.call(207, Req().put<uint8_t>(0)).result == 0);   // accept all
    Session g2 = node(relay, "G2");
    CHECK_EQ(visit(g2, "G2"), 0u);
    CHECK_EQ(state_of(g2), 5u);
    std::vector<Node> m = members(host);
    CHECK_EQ(m.size(), 1u);
    if (m.size() != 1) return;
    std::array<uint8_t, 6> next = m[0].mac;   // the MAC the next station gets: the next node slot of this room
    next[5] = static_cast<uint8_t>(next[5] + 1);

    CHECK(host.call(207, Req().put<uint8_t>(2)).result == 0);   // blacklist
    CHECK_EQ(add_filter(host, next), 0u);
    Session g3 = node(relay, "G3");
    CHECK(refused(g3, "G3"));
    CHECK_EQ(members(host).size(), 1u);         // G2 stays: a policy change never removes a node that is in

    CHECK(host.call(209).result == 0);          // ClearAcceptFilter
    CHECK(host.call(207, Req().put<uint8_t>(3)).result == 0);   // whitelist
    CHECK_EQ(add_filter(host, next), 0u);
    Session g4 = node(relay, "G4");
    CHECK_EQ(visit(g4, "G4"), 0u);              // its MAC is on the list
    CHECK_EQ(members(host).size(), 2u);
    Session g5 = node(relay, "G5");
    CHECK(refused(g5, "G5"));                      // the next slot's MAC is not
    CHECK_EQ(members(host).size(), 2u);

    for (const Session* g : {&g2, &g4}) CHECK(g->call(304).result == 0);   // Disconnect
    CHECK(host.call(204).result == 0);   // DestroyNetwork
    CHECK(host.call(201).result == 0);   // CloseAccessPoint: the policy and the filter go with it
    for (const Session* s : {&host, &g1, &g2, &g3, &g4, &g5}) s->call(401);   // Finalize
    nxrt::services::ldn_reset();   // the lobby goes back to the config
    nxrt::config_set("net", "protocol", proto.c_str());
}

// Private networks: the host creates with the game's own SecurityParameter (CreateNetworkPrivate), a station connects with it and no
// Scan (ConnectPrivate), and a scanning station sees the network under that session id and joins it with Connect. The session id rides
// at the end of the room's advertise data on the relay; no node's game sees it there.
SVC_TEST(ldn_local, private_networks) {
    const std::string proto = nxrt::config_get_str("net", "protocol", "");
    nxrt::config_set("net", "protocol", "linkplay");
    nxrt::vnet::LoopbackRelay relay;
    uint8_t sp[0x20];   // SecurityParameter: 16 B data, 16 B session id
    for (int i = 0; i < 16; ++i) {
        sp[i] = static_cast<uint8_t>(0x10 + i);
        sp[16 + i] = static_cast<uint8_t>(0xC0 + i);
    }
    const char adv[] = "private room";
    Session host = node(relay, "Host");
    CHECK(host.call(200).result == 0);                                   // OpenAccessPoint
    CHECK(host.call(206, Req().buf(A(adv, sizeof adv))).result == 0);    // SetAdvertiseData
    {
        std::vector<uint8_t> raw = security_config();
        raw.insert(raw.end(), sp, sp + sizeof sp);
        const std::vector<uint8_t> u = user_config("Host"), c = network_config();
        raw.insert(raw.end(), u.begin(), u.end());
        raw.resize(0x98, 0);
        raw.insert(raw.end(), c.begin(), c.end());
        uint8_t addresses[0x60] = {};
        CHECK_EQ(host.call(203, Req().bytes(raw.data(), raw.size()).buf(X(addresses, sizeof addresses))).result, 0u);   // CreateNetworkPrivate
    }
    Reply hs = host.call(4);   // GetSecurityParameter: the game's own
    CHECK(hs.result == 0 && hs.data.size() >= 0x20 && std::memcmp(hs.data.data(), sp, 0x20) == 0);
    std::vector<uint8_t> ni(kInfo);
    CHECK(host.call(1, Req().buf(B(ni.data(), ni.size()))).result == 0);
    CHECK(std::memcmp(ni.data() + 0x10, sp + 16, 16) == 0);                            // NetworkId.session_id at 0x10
    uint16_t adv_len = 0;
    std::memcpy(&adv_len, ni.data() + 0x50 + 0x21A, 2);
    CHECK_EQ(adv_len, sizeof adv);                                                       // the game's data only
    CHECK(std::memcmp(ni.data() + 0x50 + 0x21C, adv, sizeof adv) == 0);

    // ConnectPrivate: SecurityConfig, SecurityParameter, UserConfig, u32 version, u32 option, NetworkConfig at 0xA0; no Scan before it
    Session g1 = node(relay, "G1");
    CHECK(g1.call(300).result == 0);   // OpenStation
    {
        std::vector<uint8_t> raw = security_config();
        raw.insert(raw.end(), sp, sp + sizeof sp);
        const std::vector<uint8_t> u = user_config("G1"), c = network_config();
        raw.insert(raw.end(), u.begin(), u.end());
        const uint32_t version = 1, option = 0;
        raw.insert(raw.end(), reinterpret_cast<const uint8_t*>(&version), reinterpret_cast<const uint8_t*>(&version) + 4);
        raw.insert(raw.end(), reinterpret_cast<const uint8_t*>(&option), reinterpret_cast<const uint8_t*>(&option) + 4);
        raw.resize(0xA0, 0);
        raw.insert(raw.end(), c.begin(), c.end());
        CHECK_EQ(g1.call(303, Req().bytes(raw.data(), raw.size())).result, 0u);   // ConnectPrivate
    }
    CHECK_EQ(state_of(g1), 5u);
    std::vector<uint8_t> gi(kInfo);
    CHECK(g1.call(1, Req().buf(B(gi.data(), gi.size()))).result == 0);
    CHECK(std::memcmp(gi.data() + 0x10, sp + 16, 16) == 0);
    std::memcpy(&adv_len, gi.data() + 0x50 + 0x21A, 2);
    CHECK_EQ(adv_len, sizeof adv);
    Reply gs = g1.call(4);
    CHECK(gs.result == 0 && gs.data.size() >= 0x20 && std::memcmp(gs.data.data(), sp, 0x20) == 0);

    // a station that scans sees it under the game's session id, and Connect with that NetworkInfo joins it
    Session g2 = node(relay, "G2");
    CHECK_EQ(visit(g2, "G2"), 0u);
    CHECK_EQ(members(host).size(), 2u);

    for (const Session* g : {&g1, &g2}) CHECK(g->call(304).result == 0);
    CHECK(host.call(204).result == 0);
    for (const Session* x : {&host, &g1, &g2}) x->call(401);
    nxrt::services::ldn_reset();
    nxrt::config_set("net", "protocol", proto.c_str());
}

// The launcher's room page (coord app round 2): net-status.json's "link_play" part follows the host's room (state, players, ...), and a
// "leave" line in net-control.txt takes the host out of its room as the system would (DisconnectReason DestroyedBySystem), the station
// with it (the room closes: DestroyedByUser).
SVC_TEST(ldn_local, the_launchers_status_and_leave) {
    namespace ns = nxrt::services::net_status;
    ns::reset_for_tests();
    const std::string proto = nxrt::config_get_str("net", "protocol", "");
    nxrt::config_set("net", "protocol", "linkplay");
    nxrt::vnet::LoopbackRelay relay;
    Session host = node(relay, "Host");
    CHECK(host.call(200).result == 0);
    CHECK_EQ(create(host), 0u);
    Session g = node(relay, "G1");
    CHECK_EQ(visit(g, "G1"), 0u);
    CHECK(host.call(0).result == 0);   // any command writes the part
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ns::flush();
    std::string st;
    if (FILE* f = std::fopen(ns::status_path().c_str(), "rb")) {
        char b[4096];
        st.assign(b, std::fread(b, 1, sizeof b, f));
        std::fclose(f);
    }
    CHECK(st.find("\"link_play\"") != std::string::npos);
    CHECK(st.find("\"state\":\"hosting\"") != std::string::npos || st.find("\"state\":\"visiting\"") != std::string::npos);
    CHECK(st.find("\"Host\"") != std::string::npos && st.find("\"G1\"") != std::string::npos);   // the players
    // "<n> leave", appended while the game runs
    if (FILE* f = std::fopen(ns::control_path().c_str(), "ab")) {
        std::fputs("1 leave\n", f);
        std::fclose(f);
    }
    for (int i = 0; i < 300 && state_of(host) == 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(state_of(host), 2u);                       // AccessPoint: the network is gone
    CHECK_EQ(host.call(3).get<int16_t>(0), 4);          // DestroyedBySystem
    for (int i = 0; i < 300 && state_of(g) == 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(state_of(g), 4u);
    for (const Session* x : {&host, &g}) x->call(401);
    ns::reset_for_tests();
    std::remove(ns::control_path().c_str());
    std::remove(ns::status_path().c_str());
    nxrt::services::ldn_reset();
    nxrt::config_set("net", "protocol", proto.c_str());
}
