// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// bsd:u 29 RecvMMsg and 30 SendMMsg (services/bsd_mmsg.h) as the SDK sends them: the raw arguments and one B buffer that holds the
// serialized messages and takes the answer back. On the offline socket table (two datagram sockets of this node on a vnet link: a
// LoopbackHub link made current, the previous one put back after). (AnySwitch's case on a redirected host UDP socket is not part of PokoMac,
// which has no host redirection.)
// Every host socket here is on the loopback (127.0.0.1; the game's sockets are the virtual table's): nothing listens anywhere else.
// vsock.h before the Winsock headers: nxwin makes select a macro, which vsock::Stack::select must not meet
#include "../../src/net/vnet.h"
#include "../../src/net/vsock.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "../../src/net/host_timeo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "svc_test.h"

using namespace svctest;
namespace vn = nxrt::vnet;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kIp = vn::ip(169, 254, 9, 1);
constexpr uint32_t kMsgDontwait = 0x80, kMsgTrunc = 0x10, kMsgWaitforone = 0x80000;
constexpr int32_t kEagain = 11, kEinval = 22, kEbadf = 9, kEopnotsupp = 95, kEafnosupport = 97;

std::vector<uint8_t> sa_of(uint32_t ip, uint16_t port) {
    return {16, 2, static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(port), static_cast<uint8_t>(ip >> 24), static_cast<uint8_t>(ip >> 16),
            static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip), 0, 0, 0, 0, 0, 0, 0, 0};
}
uint32_t ip_of(const uint8_t* sa) { return (uint32_t{sa[4]} << 24) | (uint32_t{sa[5]} << 16) | (uint32_t{sa[6]} << 8) | sa[7]; }
uint16_t port_of(const uint8_t* sa) { return static_cast<uint16_t>(sa[2] << 8 | sa[3]); }

// ---- the serialized messages: a header byte, then per message u32 name size + name, u32 iov count, per iov u64 size + data, u32 control
// size + control, u32 msg_flags, u32 msg_len
struct Msg {
    std::vector<uint8_t> name;
    std::vector<std::vector<uint8_t>> iovs;
    std::vector<uint8_t> control;
    uint32_t flags = 0, len = 0;
};
void u32(std::vector<uint8_t>& b, uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
void u64(std::vector<uint8_t>& b, uint64_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 8); }
std::vector<uint8_t> pack(const std::vector<Msg>& ms) {
    std::vector<uint8_t> b(1, 0xA5);   // the header byte
    for (const Msg& m : ms) {
        u32(b, static_cast<uint32_t>(m.name.size()));
        b.insert(b.end(), m.name.begin(), m.name.end());
        u32(b, static_cast<uint32_t>(m.iovs.size()));
        for (const auto& v : m.iovs) {
            u64(b, v.size());
            b.insert(b.end(), v.begin(), v.end());
        }
        u32(b, static_cast<uint32_t>(m.control.size()));
        b.insert(b.end(), m.control.begin(), m.control.end());
        u32(b, m.flags);
        u32(b, m.len);
    }
    return b;
}
// n messages back from a buffer; false when it does not hold them (or the header byte changed)
bool unpack(const std::vector<uint8_t>& b, size_t n, std::vector<Msg>* out) {
    out->clear();
    size_t at = 1;
    auto take = [&](void* p, size_t k) {
        if (b.size() - at < k) return false;
        std::memcpy(p, b.data() + at, k);
        at += k;
        return true;
    };
    auto bytes = [&](std::vector<uint8_t>* v, size_t k) {
        if (b.size() - at < k) return false;
        v->assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + k));
        at += k;
        return true;
    };
    if (b.empty() || b[0] != 0xA5) return false;
    for (size_t i = 0; i < n; ++i) {
        Msg m;
        uint32_t ns = 0, count = 0, cs = 0;
        if (!take(&ns, 4) || !bytes(&m.name, ns) || !take(&count, 4)) return false;
        for (uint32_t k = 0; k < count; ++k) {
            uint64_t sz = 0;
            std::vector<uint8_t> v;
            if (!take(&sz, 8) || !bytes(&v, static_cast<size_t>(sz))) return false;
            m.iovs.push_back(v);
        }
        if (!take(&cs, 4) || !bytes(&m.control, cs) || !take(&m.flags, 4) || !take(&m.len, 4)) return false;
        out->push_back(m);
    }
    return true;
}
std::vector<uint8_t> text(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }
std::string str(const std::vector<uint8_t>& v, size_t n) { return std::string(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(std::min(n, v.size()))); }

// ---- bsd:u
struct Bsd {
    Session s = svctest::open("bsd:u");
    int32_t udp(uint16_t port) const {   // Socket(AF_INET, SOCK_DGRAM) on the virtual table, bound to 0.0.0.0:port (no host socket)
        const int32_t fd = s.call(2, Req().put<int32_t>(2).put<int32_t>(2).put<int32_t>(0)).get<int32_t>(0);
        if (fd >= 0 && port) {
            const std::vector<uint8_t> a = sa_of(0, port);
            s.call(13, Req().put<int32_t>(fd).buf(A(a.data(), a.size())));
        }
        return fd;
    }
    void rcvtimeo(int32_t fd, int seconds) const {
        const int64_t tv[2] = {seconds, 0};
        s.call(21, Req().put<int32_t>(fd).put<int32_t>(0xFFFF).put<int32_t>(0x1006).buf(A(tv, sizeof tv)));
    }
    std::pair<int32_t, int32_t> sendmmsg(int32_t fd, uint32_t vlen, uint32_t flags, std::vector<uint8_t>& buf) const {
        Reply r = s.call(30, Req().put<int32_t>(fd).put<uint32_t>(vlen).put<uint32_t>(flags).buf(B(buf.data(), buf.size())));
        return {r.get<int32_t>(0), r.get<int32_t>(4)};
    }
    std::pair<int32_t, int32_t> recvmmsg(int32_t fd, uint32_t vlen, uint32_t flags, int64_t sec, int64_t nsec, std::vector<uint8_t>& buf) const {
        Reply r = s.call(29, Req().put<int32_t>(fd).put<uint32_t>(vlen).put<uint32_t>(flags).put<uint32_t>(0).put<int64_t>(sec).put<int64_t>(nsec)
                                 .buf(B(buf.data(), buf.size())));
        return {r.get<int32_t>(0), r.get<int32_t>(4)};
    }
    void close(int32_t fd) const { s.call(26, Req().put<int32_t>(fd)); }
};

// A receive request of n messages: a 16-byte name and two iovs of a and b bytes each (filled with 0xEE).
std::vector<uint8_t> recv_request(size_t n, size_t a, size_t b, size_t name = 16) {
    std::vector<Msg> ms(n);
    for (Msg& m : ms) {
        m.name.assign(name, 0);
        m.iovs = {std::vector<uint8_t>(a, 0xEE), std::vector<uint8_t>(b, 0xEE)};
    }
    std::vector<uint8_t> buf = pack(ms);
    buf.resize(buf.size() + 64, 0);   // room past the request, as a client's buffer has
    return buf;
}

}  // namespace

// Three messages out in one SendMMsg (one of them gathered from two iovs), then in again in one RecvMMsg with their names and the data
// scattered over two iovs; a cut datagram (MSG_TRUNC); MSG_WAITFORONE; the timeout with nothing queued; the argument errors.
SVC_TEST(bsd_mmsg, datagrams_on_the_socket_table) {
    const std::shared_ptr<vn::Link> prev = vn::current_link();
    vn::LoopbackHub hub;
    vn::set_current_link(hub.add(kIp));
    {
        Bsd bsd;
        const int32_t a = bsd.udp(5001), b = bsd.udp(5002);
        CHECK(a >= 0 && b >= 0);
        bsd.rcvtimeo(b, 2);   // a lost datagram fails the case instead of hanging it

        // ---- SendMMsg: 3 messages to this node's b, msg_len written back
        std::vector<Msg> out(3);
        for (Msg& m : out) m.name = sa_of(kIp, 5002);
        out[0].iovs = {text("one")};
        out[1].iovs = {text("tw"), text("o!")};
        out[2].iovs = {text("three")};
        std::vector<uint8_t> sb = pack(out);
        const auto sr = bsd.sendmmsg(a, 3, 0, sb);
        CHECK(sr.first == 3 && sr.second == 0);
        std::vector<Msg> sent;
        CHECK(unpack(sb, 3, &sent));
        if (sent.size() == 3) {
            CHECK_EQ(sent[0].len, 3u);
            CHECK_EQ(sent[1].len, 4u);
            CHECK_EQ(sent[2].len, 5u);
        }

        // ---- RecvMMsg: all three, each over iovs of 2 and 8 bytes, the sender as the name
        std::vector<uint8_t> rb = recv_request(3, 2, 8);
        const auto rr = bsd.recvmmsg(b, 3, 0, 0, 0, rb);
        CHECK(rr.first == 3 && rr.second == 0);
        std::vector<Msg> in;
        CHECK(unpack(rb, 3, &in));
        const char* want[3] = {"one", "two!", "three"};
        for (size_t i = 0; i < in.size(); ++i) {
            CHECK_EQ(in[i].name.size(), 16u);
            CHECK(in[i].name.size() == 16 && ip_of(in[i].name.data()) == kIp && port_of(in[i].name.data()) == 5001);
            CHECK_EQ(in[i].len, std::strlen(want[i]));
            CHECK_EQ(in[i].flags, 0u);
            CHECK(in[i].iovs.size() == 2 && in[i].iovs[0].size() == 2 && in[i].iovs[1].size() == 8);
            if (in[i].iovs.size() == 2) {
                const std::string got = str(in[i].iovs[0], 2) + str(in[i].iovs[1], in[i].len > 2 ? in[i].len - 2 : 0);
                CHECK(got == want[i]);
                CHECK_EQ(in[i].iovs[1][7], 0xEE);   // past the data: as the request had it
            }
            CHECK(in[i].control.empty());
        }

        // ---- a datagram longer than the iovs: cut, MSG_TRUNC; a name buffer of 8 bytes gets the first 8 of the sockaddr
        {
            std::vector<Msg> one(1);
            one[0].name = sa_of(kIp, 5002);
            one[0].iovs = {text("0123456789")};
            std::vector<uint8_t> b1 = pack(one);
            CHECK_EQ(bsd.sendmmsg(a, 1, 0, b1).first, 1);
            std::vector<uint8_t> r1 = recv_request(1, 2, 2, 8);
            CHECK_EQ(bsd.recvmmsg(b, 1, 0, 0, 0, r1).first, 1);
            std::vector<Msg> got;
            CHECK(unpack(r1, 1, &got));
            if (got.size() == 1) {
                CHECK(got[0].len == 4 && got[0].flags == kMsgTrunc && str(got[0].iovs[0], 2) + str(got[0].iovs[1], 2) == "0123");
                CHECK(got[0].name.size() == 8 && port_of(got[0].name.data()) == 5001);
            }
        }

        // ---- MSG_WAITFORONE: one queued, three asked for: the call ends after it
        {
            std::vector<Msg> one(1);
            one[0].name = sa_of(kIp, 5002);
            one[0].iovs = {text("only")};
            std::vector<uint8_t> b1 = pack(one);
            CHECK_EQ(bsd.sendmmsg(a, 1, 0, b1).first, 1);
            std::vector<uint8_t> r3 = recv_request(3, 4, 4);
            const auto t0 = Clock::now();
            const auto w = bsd.recvmmsg(b, 3, kMsgWaitforone, 0, 0, r3);
            CHECK(w.first == 1 && w.second == 0);
            CHECK(Clock::now() - t0 < std::chrono::milliseconds(500));
            std::vector<Msg> got;
            CHECK(unpack(r3, 1, &got) && got[0].len == 4 && str(got[0].iovs[0], 4) == "only");
        }

        // ---- the timeout bounds the wait for the first message: none comes, 0 messages
        {
            std::vector<uint8_t> r2 = recv_request(2, 4, 4);
            const auto t0 = Clock::now();
            const auto w = bsd.recvmmsg(b, 2, 0, 0, 200000000, r2);   // 200 ms
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            CHECK(w.first == 0 && w.second == 0);
            CHECK(ms >= 150 && ms < 1500);
            CHECK(bsd.recvmmsg(b, 2, kMsgDontwait, 0, 0, r2) == std::make_pair(-1, kEagain));   // no timeout, MSG_DONTWAIT: EAGAIN
        }

        // ---- the arguments
        std::vector<uint8_t> r1 = recv_request(1, 4, 4);
        CHECK(bsd.recvmmsg(b, 0, 0, 0, 0, r1) == std::make_pair(0, 0));                          // vlen 0
        CHECK(bsd.recvmmsg(b, 1, 0, 0, 1000000000, r1) == std::make_pair(-1, kEinval));          // nanoseconds past a second
        CHECK(bsd.recvmmsg(b, 1, 0x100, 0, 0, r1) == std::make_pair(-1, kEopnotsupp));           // a flag it cannot honour
        CHECK(bsd.recvmmsg(b, 100, kMsgDontwait, 0, 0, r1) == std::make_pair(-1, kEinval));      // the buffer cannot hold 100
        CHECK(bsd.recvmmsg(77, 1, 0, 0, 0, r1) == std::make_pair(-1, kEbadf));
        std::vector<Msg> bad(1);
        bad[0].name = sa_of(kIp, 5002);
        bad[0].name[1] = 28;   // AF_INET6
        bad[0].iovs = {text("x")};
        std::vector<uint8_t> bb = pack(bad);
        CHECK(bsd.sendmmsg(a, 1, 0, bb) == std::make_pair(-1, kEafnosupport));
        bad[0].name = sa_of(kIp, 5002);
        bad[0].control = {1, 2, 3, 4};
        bb = pack(bad);
        CHECK(bsd.sendmmsg(a, 1, 0, bb) == std::make_pair(-1, kEopnotsupp));                     // control data
        // the second message cannot go: the first is counted
        std::vector<Msg> two(2);
        two[0].name = sa_of(kIp, 5002);
        two[0].iovs = {text("first")};
        two[1].name = std::vector<uint8_t>(8, 0);   // too short a name
        two[1].iovs = {text("second")};
        bb = pack(two);
        CHECK(bsd.sendmmsg(a, 2, 0, bb) == std::make_pair(1, 0));
        CHECK(bsd.recvmmsg(b, 1, 0, 0, 0, r1).first == 1);
        CHECK(bsd.recvmmsg(b, 1, kMsgDontwait, 0, 0, r1) == std::make_pair(-1, kEagain));      // the second never left
        bsd.close(a);
        bsd.close(b);
    }
    vn::set_current_link(prev);
}
