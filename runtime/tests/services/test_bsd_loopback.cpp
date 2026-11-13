// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// TCP over the loopback through bsd (services/network.cpp over net/vsock.cpp), which a console has with no network. The case walks the
// wake-up pair libuv's loop makes when it starts (Star Fox's HTTP loops, main+0xbe32700: socket x2, bind 127.0.0.1:0, listen 1,
// getsockname, connect, accept, close the listener), then what the loop does with it, and the offline answers around it.
#include <cstdint>
#include <cstring>

#include "svc_test.h"

using namespace svctest;

namespace {

constexpr uint32_t kAfInet = 2, kSockStream = 1, kSockDgram = 2, kTcp = 6, kUdp = 17;
constexpr int16_t kPollIn = 0x1, kPollOut = 0x4, kPollHup = 0x10;
constexpr int32_t kEagain = 11, kEpipe = 32, kEnetdown = 100, kEconnrefused = 111;

struct Sa {   // FreeBSD sockaddr_in: len, family, be16 port, be32 address, 8 zero bytes
    uint8_t b[16] = {};
    static Sa make(uint32_t ip, uint16_t port) {
        Sa s;
        s.b[0] = 16;
        s.b[1] = static_cast<uint8_t>(kAfInet);
        s.b[2] = static_cast<uint8_t>(port >> 8);
        s.b[3] = static_cast<uint8_t>(port);
        s.b[4] = static_cast<uint8_t>(ip >> 24);
        s.b[5] = static_cast<uint8_t>(ip >> 16);
        s.b[6] = static_cast<uint8_t>(ip >> 8);
        s.b[7] = static_cast<uint8_t>(ip);
        return s;
    }
    uint16_t port() const { return static_cast<uint16_t>((b[2] << 8) | b[3]); }
    uint32_t ip() const { return (uint32_t{b[4]} << 24) | (uint32_t{b[5]} << 16) | (uint32_t{b[6]} << 8) | b[7]; }
};

struct PollFd {
    int32_t fd;
    int16_t events, revents;
};

struct Bsd {
    Session s;
    // (ret, errno)
    struct R {
        int32_t ret;
        int32_t err;
        uint32_t extra;
    };
    static R of(const Reply& r) { return {r.get<int32_t>(0), r.get<int32_t>(4), r.get<uint32_t>(8)}; }
    R socket(uint32_t type, uint32_t proto) const { return of(s.call(2, Req().put<uint32_t>(kAfInet).put<uint32_t>(type).put<uint32_t>(proto))); }
    R bind(int fd, const Sa& a) const { return of(s.call(13, Req().put<uint32_t>(fd).buf(A(a.b, sizeof a.b)))); }
    R listen(int fd, int backlog) const { return of(s.call(18, Req().put<uint32_t>(fd).put<int32_t>(backlog))); }
    R sockname(int fd, Sa* a) const { return of(s.call(16, Req().put<uint32_t>(fd).buf(B(a->b, sizeof a->b)))); }
    R connect(int fd, const Sa& a) const { return of(s.call(14, Req().put<uint32_t>(fd).buf(A(a.b, sizeof a.b)))); }
    R accept(int fd, Sa* a) const { return of(s.call(12, Req().put<uint32_t>(fd).buf(B(a->b, sizeof a->b)))); }
    R write(int fd, const void* p, uint32_t n) const { return of(s.call(24, Req().put<uint32_t>(fd).buf(A(p, n)))); }
    R read(int fd, void* p, uint32_t n) const { return of(s.call(25, Req().put<uint32_t>(fd).buf(B(p, n)))); }
    R fcntl(int fd, int cmd, int arg) const { return of(s.call(20, Req().put<uint32_t>(fd).put<int32_t>(cmd).put<int32_t>(arg))); }
    R shutdown(int fd, int how) const { return of(s.call(22, Req().put<uint32_t>(fd).put<int32_t>(how))); }
    R close(int fd) const { return of(s.call(26, Req().put<uint32_t>(fd))); }
    R sockopt(int fd, int level, int opt, int32_t* v) const {
        return of(s.call(17, Req().put<uint32_t>(fd).put<int32_t>(level).put<int32_t>(opt).buf(B(v, sizeof *v))));
    }
    // one descriptor: its revents (-1 when Poll failed)
    int16_t poll(int fd, int16_t events, int timeout_ms) const {
        PollFd in{fd, events, 0}, out{};
        const R r = of(s.call(6, Req().put<uint32_t>(1).put<int32_t>(timeout_ms).buf(A(&in, sizeof in)).buf(B(&out, sizeof out))));
        return r.ret < 0 ? int16_t{-1} : out.revents;
    }
};

}  // namespace

SVC_TEST(bsd_loopback, libuv_wakeup_pair) {
    Bsd b{open("bsd:u")};
    CHECK(b.s.valid());
    const uint32_t lo = 0x7F000001;

    const Bsd::R a = b.socket(kSockStream, kTcp), c = b.socket(kSockStream, kTcp);
    CHECK(a.ret >= 0 && a.err == 0);
    CHECK(c.ret >= 0 && c.err == 0 && c.ret != a.ret);
    CHECK_EQ(b.bind(a.ret, Sa::make(lo, 0)).ret, 0);
    CHECK_EQ(b.listen(a.ret, 1).ret, 0);
    Sa name;
    const Bsd::R n = b.sockname(a.ret, &name);
    CHECK_EQ(n.ret, 0);
    CHECK_EQ(n.extra, 16u);
    CHECK_EQ(name.ip(), lo);
    CHECK(name.port() != 0);
    CHECK_EQ(b.poll(a.ret, kPollIn, 0), 0);          // nothing to accept yet
    CHECK_EQ(b.connect(c.ret, name).ret, 0);         // completes into the listener's queue
    CHECK_EQ(b.poll(a.ret, kPollIn, 0), kPollIn);
    Sa peer;
    const Bsd::R s = b.accept(a.ret, &peer);
    CHECK(s.ret >= 0 && s.err == 0);
    CHECK_EQ(peer.ip(), lo);
    Sa cname;
    CHECK_EQ(b.sockname(c.ret, &cname).ret, 0);
    CHECK_EQ(peer.port(), cname.port());
    CHECK_EQ(b.close(a.ret).ret, 0);

    // the loop's wake-up: one end written, the other polled and read
    CHECK_EQ(b.poll(s.ret, kPollIn, 0), 0);
    CHECK_EQ(b.poll(c.ret, kPollOut, 0), kPollOut);
    CHECK_EQ(b.write(c.ret, "wake", 4).ret, 4);
    CHECK_EQ(b.poll(s.ret, kPollIn, 100), kPollIn);
    char buf[16] = {};
    CHECK_EQ(b.read(s.ret, buf, sizeof buf).ret, 4);
    CHECK(std::memcmp(buf, "wake", 4) == 0);
    CHECK_EQ(b.fcntl(s.ret, 4, 0x800).ret, 0);        // F_SETFL O_NONBLOCK
    const Bsd::R empty = b.read(s.ret, buf, sizeof buf);
    CHECK_EQ(empty.ret, -1);
    CHECK_EQ(empty.err, kEagain);
    CHECK_EQ(b.write(s.ret, "x", 1).ret, 1);          // the other way
    CHECK_EQ(b.read(c.ret, buf, sizeof buf).ret, 1);
    CHECK_EQ(buf[0], 'x');
    int32_t type = 0;
    CHECK_EQ(b.sockopt(s.ret, 0xFFFF, 0x1008, &type).ret, 0);   // SO_TYPE
    CHECK_EQ(type, static_cast<int32_t>(kSockStream));

    // offline answers: nobody listens on the closed port; nothing outside this console answers
    const Bsd::R d = b.socket(kSockStream, 0);
    CHECK(d.ret >= 0);
    const Bsd::R refused = b.connect(d.ret, name);
    CHECK_EQ(refused.ret, -1);
    CHECK_EQ(refused.err, kEconnrefused);
    const Bsd::R away = b.connect(d.ret, Sa::make(0x0A000001, 80));
    CHECK_EQ(away.ret, -1);
    CHECK_EQ(away.err, kEnetdown);
    CHECK_EQ(b.close(d.ret).ret, 0);

    // the end of data: a shut-down writer, then a closed end
    CHECK_EQ(b.shutdown(c.ret, 1).ret, 0);            // SHUT_WR
    CHECK_EQ(b.poll(s.ret, kPollIn, 0) & kPollIn, kPollIn);
    CHECK_EQ(b.read(s.ret, buf, sizeof buf).ret, 0);
    CHECK_EQ(b.close(c.ret).ret, 0);
    CHECK_EQ(b.poll(s.ret, kPollIn, 0), static_cast<int16_t>(kPollIn | kPollHup));
    const Bsd::R broken = b.write(s.ret, "y", 1);
    CHECK_EQ(broken.ret, -1);
    CHECK_EQ(broken.err, kEpipe);
    CHECK_EQ(b.close(s.ret).ret, 0);

    // datagram sockets as before
    const Bsd::R u = b.socket(kSockDgram, kUdp);
    CHECK(u.ret >= 0 && u.err == 0);
    CHECK_EQ(b.close(u.ret).ret, 0);
}
