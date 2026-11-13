// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The offline network answers that games read numbers from: sfdnsres's getaddrinfo errors in the console's (FreeBSD's) numbering and
// their text, nsd's server environment, and bsd's answer for an address family the console has no stack for. References: Ryujinx
// Sockets/Sfdnsres/Types/GaiError.cs and Nsd/Types/ApplicationServerEnvironmentType.cs, Lichii sockets.h (GetAddrInfoError), nsd.cpp,
// bsd.cpp (Socket: EAFNOSUPPORT for a domain that is not AF_INET).
#include <cstring>
#include <string>

#include "nxrt/config.h"
#include "lan_room.h"   // nifm_set_lan_source (AnySwitch declares it in net_redirect.h)
#include "svc_test.h"

using namespace svctest;

namespace {

// GetGaiStringErrorRequest(s32 error) -> B: text
std::string gai_text(const Session& dns, int32_t e) {
    char out[128] = {};
    Reply r = dns.call(5, Req().put<int32_t>(e).buf(B(out, sizeof out)));
    if (!r.ok || r.result != 0) return "<failed>";
    return std::string(out, strnlen(out, sizeof out));
}

}  // namespace

SVC_TEST(net_offline, getaddrinfo_errors_are_the_consoles_numbers) {
    Session dns = open("sfdnsres");
    const char host[] = "game.example.com", service[] = "443";
    uint8_t out[256] = {};
    // GetAddrInfoRequest(u8 use_nsd, u32 cancel, u64 pid; A host, A service, A hints; B out) -> (u32 bsd_errno, s32 gai_error, u32 size)
    Reply a = dns.call(6, Req().put<uint8_t>(0).put<uint32_t>(0).put<uint64_t>(0).with_pid().buf(A(host, sizeof host)).buf(A(service, sizeof service))
                              .buf(A(out, 0)).buf(B(out, sizeof out)));
    CHECK(a.ok && a.result == 0);
    CHECK_EQ(a.get<int32_t>(4), 8);    // EAI_NONAME, FreeBSD's 8 (not glibc's -2)
    CHECK_EQ(a.get<uint32_t>(8), 0u);  // nothing written
    // GetAddrInfoRequestWithOptions -> (u32 size, s32 gai_error, s32 netdb_error, u32 bsd_errno)
    Reply b = dns.call(12, Req().put<uint8_t>(0).put<uint32_t>(0).put<uint64_t>(0).buf(A(host, sizeof host)).buf(A(service, sizeof service))
                               .buf(A(out, 0)).buf(A(out, 0)).buf(B(out, sizeof out)));
    CHECK(b.ok && b.result == 0);
    CHECK_EQ(b.get<uint32_t>(0), 0u);
    CHECK_EQ(b.get<int32_t>(4), 8);
    // the text of each number is FreeBSD's gai_strerror
    CHECK(gai_text(dns, 8) == "hostname nor servname provided, or not known");
    CHECK(gai_text(dns, 2) == "Temporary failure in name resolution");
    CHECK(gai_text(dns, 6) == "Memory allocation failure");
    CHECK(gai_text(dns, 7) == "No address associated with hostname");
    CHECK(gai_text(dns, -2) == "Unknown error");
}

SVC_TEST(net_offline, nsd_reports_the_retail_server_environment) {
    Session nsd = open("nsd:u");
    Reply r = nsd.call(100);   // GetApplicationServerEnvironmentType -> u8 (1 = Lp)
    CHECK(r.ok && r.result == 0 && r.words() >= 1);
    CHECK_EQ(r.get<uint8_t>(0), 1u);
}

SVC_TEST(net_offline, an_ipv6_socket_is_an_unsupported_family) {
    Session bsd = open("bsd:u");
    // Socket(AF_INET6 = 28, SOCK_DGRAM, 0) -> (-1, EAFNOSUPPORT 97): dual-stack code falls back to IPv4 on this answer
    Reply s = bsd.call(2, Req().put<int32_t>(28).put<int32_t>(2).put<int32_t>(0));
    CHECK(s.ok && s.result == 0);
    CHECK_EQ(s.get<int32_t>(0), -1);
    CHECK_EQ(s.get<uint32_t>(4), 97u);
    Reply t = bsd.call(2, Req().put<int32_t>(28).put<int32_t>(1).put<int32_t>(0));
    CHECK_EQ(t.get<int32_t>(0), -1);
    CHECK_EQ(t.get<uint32_t>(4), 97u);
}

// nifm's network state is read at each call (services/network.cpp nifm_now): with nothing, no connection; with a LAN source (what
// online-servers' lan_room registers through nifm_set_lan_source), its address and mask in the console's layouts (Ryujinx
// Nifm/StaticService: IpAddressSetting + DnsSetting = 0x16 bytes, NetworkProfileData 0x17C, IpV4Address in network order); with
// [net] online_test, 10.0.0.2/24.
namespace {
bool g_lan_up = false;
bool test_lan(uint32_t* ip, uint32_t* mask) {
    if (!g_lan_up) return false;
    *ip = 0xA9FE0502;   // 169.254.5.2
    *mask = 0xFFFFFF00;
    return true;
}
}  // namespace

SVC_TEST(net_offline, nifm_follows_the_network_the_game_is_on) {
    Session n = open("nifm:u");
    Session gs = n.object(n.call(5, Req().put<uint64_t>(0).with_pid()));   // CreateGeneralService
    Session req = gs.object(gs.call(4, Req().put<uint32_t>(0)));           // CreateRequest
    CHECK_EQ(gs.call(15).result, 110u | (300u << 9));                      // GetCurrentIpConfigInfo: no connection
    CHECK_EQ(gs.call(21).get<uint8_t>(0), 0u);                             // IsAnyInternetRequestAccepted

    nxrt::services::nifm_set_lan_source(&test_lan);
    g_lan_up = true;
    CHECK_EQ(req.call(4).result, 0u);                                      // Submit
    CHECK_EQ(req.call(0).get<uint32_t>(0), 2u);                            // OnHold, once
    CHECK_EQ(req.call(0).get<uint32_t>(0), 3u);                            // Accepted
    CHECK_EQ(req.call(1).result, 0u);                                      // GetResult
    Reply ic = gs.call(15);
    CHECK(ic.result == 0 && ic.data.size() >= 0x16);
    const uint8_t want[13] = {1, 169, 254, 5, 2, 255, 255, 255, 0, 0, 0, 0, 0};
    CHECK(ic.data.size() >= 13 && std::memcmp(ic.data.data(), want, 13) == 0);
    Reply ip = gs.call(12);                                                // GetCurrentIpAddress
    CHECK(ip.result == 0 && ip.data.size() >= 4 && ip.data[0] == 169 && ip.data[1] == 254 && ip.data[2] == 5 && ip.data[3] == 2);
    CHECK_EQ(gs.call(21).get<uint8_t>(0), 1u);
    CHECK_EQ(gs.call(22).get<uint8_t>(0), 1u);
    uint8_t prof[0x17C] = {};
    Reply pr = gs.call(5, Req().buf(C(prof, sizeof prof)));                // GetCurrentNetworkProfile
    CHECK_EQ(pr.result, 0u);
    CHECK(prof[0] == 1 && prof[1] == 169 && prof[4] == 2);                 // IpAddressSetting at 0
    CHECK(std::memcmp(prof + 0xD2, "PokoMac", 7) == 0);                  // the profile's name
    Reply st = gs.call(18);                                                // GetInternetConnectionStatus: Wifi, 3, Connected
    CHECK(st.result == 0 && st.data.size() >= 3 && st.data[0] == 1 && st.data[2] == 4);

    g_lan_up = false;                                                      // the LAN goes away: so does the connection
    CHECK_EQ(gs.call(15).result, 110u | (300u << 9));
    nxrt::services::nifm_set_lan_source(nullptr);

    nxrt::config_set("net", "online_test", "true");
    Reply on = gs.call(12);
    CHECK(on.result == 0 && on.data.size() >= 4 && on.data[0] == 10 && on.data[3] == 2);
    nxrt::config_set("net", "online_test", "false");
    CHECK_EQ(gs.call(12).result, 110u | (300u << 9));
}
