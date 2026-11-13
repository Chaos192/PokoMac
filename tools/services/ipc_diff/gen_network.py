# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Copyright (c) 2026 Chaos192
"""The network services of runtime/src/services/network.cpp: nifm (offline, [net] online_test, a redirect table), bsd (fixed answers, an
empty table fuzzed, loopback TCP, UDP, event descriptors, Poll, Select, RecvMMsg / SendMMsg routing), bsdcfg, ssl / ssl:s (context and
connection counts, PKI ids, closes), nim:eca, friend, prepo, pctl, bcat, nsd, sfdnsres, ntc, ngct:u, ngc:u, notif:a, and the ids other
files add to these interfaces by name (ext_system.cpp), which must stay theirs.
Socket commands only on the process socket table (net/vsock.h, in-process): no host socket is made, nothing listens or connects outside
this process. Every receive is non-blocking or has data waiting, and every wait is short, so no request blocks the replay."""
import struct

NIFM, GEN, REQ, SCAN = ("nn::nifm::detail::IStaticService", "nn::nifm::detail::IGeneralService", "nn::nifm::detail::IRequest",
                        "nn::nifm::detail::IScanRequest")
BSD = "nn::socket::sf::IClient"
SSL, CTX, CONN = "nn::ssl::sf::ISslService", "nn::ssl::sf::ISslContext", "nn::ssl::sf::ISslConnection"
NIM, SRV, ACC, ASYNC = ("nn::nim::detail::IShopServiceAccessServerInterface", "nn::ec::IShopServiceAccessServer",
                        "nn::ec::IShopServiceAccessor", "nn::ec::IShopServiceAsync")
FR = "nn::friends::detail::ipc::"
PCTL_F, PCTL = "nn::pctl::detail::ipc::IParentalControlServiceFactory", "nn::pctl::detail::ipc::IParentalControlService"
BC = "nn::bcat::detail::ipc::"
NSD, DNS = "nn::nsd::detail::IManager", "nn::socket::resolver::IResolver"
NTC, TASK = "nn::ntc::detail::IStaticService", "nn::ntc::detail::IEnsureNetworkClockAvailabilityService"
NGCT, NGC, NOTIF = "nn::ngct::detail::IService", "nn::ngc::detail::IService", "nn::notification::INotificationServicesForApplication"

AF_INET, AF_INET6, STREAM, DGRAM, NONBLOCK = 2, 28, 1, 2, 0x20000000
DONTWAIT, PEEK = 0x80, 0x2
SOL_SOCKET, SO_REUSEADDR, SO_BROADCAST, SO_RCVTIMEO, SO_ERROR, SO_TYPE, SO_RCVBUF = 0xFFFF, 0x4, 0x20, 0x1006, 0x1007, 0x1008, 0x1002
F_GETFL, F_SETFL, O_NONBLOCK = 3, 4, 0x800
POLLIN, POLLOUT = 1, 4
LO = 0x7F000001


def sa(ip, port, family=AF_INET, length=16):
    return bytes([length, family]) + struct.pack(">HI", port, ip) + bytes(8)


def objs(s, sid, iface, cmd, raw=b"", n=1, bufs=(), pid=False):
    return s.q(sid, iface, cmd, raw=raw, bufs=bufs, pid=pid, nobj=n)


def obj(s, sid, iface, cmd, raw=b"", bufs=(), pid=False):
    return objs(s, sid, iface, cmd, raw=raw, bufs=bufs, pid=pid)[0]


def close(s, sid):
    s.lines.append(f"X {sid}")


def every(s, sid, iface, ids, variants, sweep=()):
    for cmd in ids:
        s.fuzz(sid, iface, cmd, n=variants)
    s.sweep(sid, iface, list(sweep))


def text(t):
    return t.encode() + b"\0"


# ---- nifm ---------------------------------------------------------------------------------------------------------------------------
def nifm_getters(s, g):
    for cmd, bufs in ((5, (("c", 0x17C, None),)), (5, (("b", 0x100, None),)), (5, (("c", 0x200, None),)), (5, ()), (12, ()), (15, ()),
                      (18, ()), (21, (("a", 4, b"\1\0\0\0"),)), (22, ()), (33, ())):
        s.q(g, GEN, cmd, bufs=bufs)


def request_machine(s, r):
    for cmd in (0, 4, 0, 0, 0, 4, 4, 0, 0, 4, 1, 0, 4, 3, 0, 0, 1, 3, 4, 0, 1, 0):
        s.q(r, REQ, cmd)


def nifm(s, variants):
    st = s.open("nifm:u")
    g1 = obj(s, st, NIFM, 5, raw=bytes(8), pid=True)
    g2 = obj(s, st, NIFM, 4)
    for g in (g1, g2):
        for bufs in ((("b", 4, None),), (("b", 2, None),), (("c", 8, None),), ()):
            s.q(g, GEN, 1, bufs=bufs)
    nifm_getters(s, g1)
    r = obj(s, g1, GEN, 4, raw=struct.pack("<I", 2))
    r2 = obj(s, g2, GEN, 4)
    s.q(r, REQ, 2)
    s.q(r2, REQ, 2)
    request_machine(s, r)
    s.q(r2, REQ, 4)
    s.q(r, REQ, 0)
    s.q(r2, REQ, 0)
    for raw in (bytes(0x24), bytes(range(0x24)), b"\xff" * 0x24, bytes(range(0x24)), b"\1\2"):
        s.q(r, REQ, 5, raw=raw)
    for p in (0, 1, 2, 0x10, 2):
        s.q(r, REQ, 6, raw=struct.pack("<I", p))
    every(s, r, REQ, [i for i in range(27) if i not in (2, 7)], variants // 4, [7, 27, 28, 100])
    sc = obj(s, g1, GEN, 2)
    s.q(sc, SCAN, 3)
    every(s, sc, SCAN, range(6), variants // 4, [6, 7, 10])
    ids = [i for i in range(1, 58) if i not in (2, 3, 4, 31)] + [101]
    every(s, g1, GEN, ids, variants // 4, [0, 3, 58, 59, 100, 102, 200])
    for cmd in (2, 4, 31, 31):
        s.q(g1, GEN, cmd)
    s.sweep(st, NIFM, [0, 1, 2, 3, 6, 7])
    for name in ("nifm:a", "nifm:s"):
        st2 = s.open(name)
        g3 = obj(s, st2, NIFM, 5, raw=bytes(8), pid=True)
        s.q(g3, GEN, 1, bufs=(("b", 4, None),))
    # online_test: 10.0.0.2/24, gateway 10.0.0.1
    s.config("net", "online_test", "true")
    nifm_getters(s, g1)
    request_machine(s, r)
    s.q(g2, GEN, 12)
    s.config("net", "online_test", "false")
    nifm_getters(s, g1)
    s.q(r, REQ, 4)
    s.q(r, REQ, 0)
    s.q(r, REQ, 0)


# ---- bsd ----------------------------------------------------------------------------------------------------------------------------
class Bsd:
    def __init__(self, s, sid):
        self.s, self.b = s, sid

    def q(self, cmd, raw=b"", bufs=()):
        self.s.q(self.b, BSD, cmd, raw=raw, bufs=bufs)

    def socket(self, dom=AF_INET, typ=DGRAM, proto=0):
        self.q(2, struct.pack("<iii", dom, typ, proto))

    def bind(self, fd, addr):
        self.q(13, struct.pack("<i", fd), (("a", len(addr), addr),))

    def connect(self, fd, addr):
        self.q(14, struct.pack("<i", fd), (("a", len(addr), addr),))

    def listen(self, fd, backlog=1):
        self.q(18, struct.pack("<ii", fd, backlog))

    def accept(self, fd, size=16):
        self.q(12, struct.pack("<i", fd), (("b", size, None),) if size else ())

    def name(self, cmd, fd, size=16):
        self.q(cmd, struct.pack("<i", fd), (("b", size, None),) if size else ())

    def write(self, fd, data):
        self.q(24, struct.pack("<i", fd), (("a", len(data), data),))

    def read(self, fd, size):
        self.q(25, struct.pack("<i", fd), (("b", size, None),))

    def recv(self, fd, size, flags=DONTWAIT):
        self.q(8, struct.pack("<ii", fd, flags), (("b", size, None),))

    def recvfrom(self, fd, size, flags=DONTWAIT, asize=16):
        self.q(9, struct.pack("<ii", fd, flags), (("b", size, None), ("b", asize, None)))

    def send(self, fd, data, flags=0):
        self.q(10, struct.pack("<ii", fd, flags), (("a", len(data), data),))

    def sendto(self, fd, data, addr, flags=0):
        self.q(11, struct.pack("<ii", fd, flags), (("a", len(data), data), ("a", len(addr), addr)))

    def fcntl(self, fd, cmd, arg=0):
        self.q(20, struct.pack("<iii", fd, cmd, arg))

    def getopt(self, fd, level, opt, size):
        self.q(17, struct.pack("<iii", fd, level, opt), (("b", size, None),) if size else ())

    def setopt(self, fd, level, opt, value):
        self.q(21, struct.pack("<iii", fd, level, opt), (("a", len(value), value),) if value is not None else ())

    def shutdown(self, fd, how):
        self.q(22, struct.pack("<ii", fd, how))

    def close(self, fd):
        self.q(26, struct.pack("<i", fd))

    def poll(self, entries, timeout=0, nfds=None, insize=None, outsize=None):
        body = b"".join(struct.pack("<ihh", fd, ev, 0) for fd, ev in entries)
        n = len(entries) if nfds is None else nfds
        bufs = []
        if insize != 0:
            bufs.append(("a", len(body) if insize is None else insize, body))
        if outsize != 0:
            bufs.append(("b", len(body) if outsize is None else outsize, None))
        self.q(6, struct.pack("<Ii", n, timeout), tuple(bufs))

    def select(self, nfds, rd=None, wr=None, ex=None, sec=0, usec=0, forever=0, kind="a", size=128, outsize=128):
        def fdset(fds):
            words = [0] * 16
            for fd in fds:
                words[fd // 64] |= 1 << (fd % 64)
            return struct.pack("<16Q", *words)
        bufs = []
        for fds in (rd, wr, ex):
            bufs.append((kind, size if fds is not None else 0, fdset(fds) if fds is not None else b""))
        for _ in range(3):
            bufs.append(("b", outsize, None))
        self.q(5, struct.pack("<iiqqB", nfds, 0, sec, usec, forever), tuple(bufs))

    def eventfd(self, init, flags):
        self.q(31, struct.pack("<QI", init, flags))


def bsd(s, variants):
    b = s.open("bsd:u")
    B = Bsd(s, b)
    s.q(b, BSD, 0, raw=bytes(0x20) + struct.pack("<QQ", 0, 0x10000), pid=True, hc=("t10000",))
    s.q(b, BSD, 0)
    # the fixed answers
    every(s, b, BSD, [1, 4, 7, 19, 23, 27, 28] + list(range(32, 45)) + [200, 201], variants // 4, [45, 46, 50, 100, 199, 202, 300])
    # an empty table: every descriptor command answers for a descriptor nobody made
    every(s, b, BSD, [8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22, 24, 25, 26, 29, 30], variants // 4)
    # spec vectors: Socket's families, the sockaddr checks
    B.socket(AF_INET6, DGRAM)
    B.socket(AF_INET6, STREAM)
    B.socket(1, DGRAM)
    B.socket(AF_INET, DGRAM)                               # fd 0
    B.sendto(0, b"x", b"\x10\x02\x00\x50")                # 4-byte sockaddr: EINVAL
    B.sendto(0, b"x", sa(LO, 80, family=AF_INET6))        # not AF_INET
    B.sendto(77, b"x", b"\x10\x02\x00\x50")               # not a socket: EBADF
    B.sendto(0, b"x", sa(LO, 80)[:8])                     # 8 bytes are enough
    B.sendto(0, b"x", b"")
    B.q(11, struct.pack("<ii", 0, 0), (("a", 1, b"x"),))  # no address buffer
    B.sendto(0, b"x", sa(0xA9FE0102, 9000))               # a link address: no link
    B.sendto(0, b"x", sa(0x0A000001, 80))
    B.bind(0, b"\x10\x02\x00")
    B.bind(0, sa(LO, 80, family=0))
    B.bind(90, sa(LO, 80, family=0))
    B.bind(0, sa(0, 40100))
    B.name(16, 0)
    B.name(16, 0, 8)
    B.name(16, 0, 0)
    B.name(16, 0, 32)
    B.name(16, 55)
    B.name(15, 0)
    B.recvfrom(0, 0x100)
    B.recvfrom(0, 0x100, flags=DONTWAIT | PEEK, asize=4)
    B.recv(0, 0x10)
    B.getopt(0, SOL_SOCKET, SO_TYPE, 4)
    B.getopt(0, SOL_SOCKET, SO_TYPE, 2)
    B.getopt(0, SOL_SOCKET, SO_TYPE, 0)
    B.getopt(0, SOL_SOCKET, SO_ERROR, 8)
    B.getopt(0, SOL_SOCKET, 0x9999, 4)
    B.getopt(0, 6, 1, 4)
    B.setopt(0, SOL_SOCKET, SO_BROADCAST, struct.pack("<I", 1))
    B.setopt(0, SOL_SOCKET, SO_RCVBUF, struct.pack("<I", 0x8000))
    B.getopt(0, SOL_SOCKET, SO_RCVBUF, 4)
    B.setopt(0, SOL_SOCKET, SO_RCVTIMEO, struct.pack("<qq", 0, 5000))
    B.setopt(0, 6, 1, struct.pack("<I", 1))
    B.getopt(0, 6, 1, 4)
    B.setopt(0, SOL_SOCKET, SO_REUSEADDR, None)
    B.fcntl(0, F_GETFL)
    B.fcntl(0, F_SETFL, O_NONBLOCK)
    B.fcntl(0, F_GETFL)
    B.fcntl(0, 99, 1)
    B.recvfrom(0, 0x100, flags=0)
    B.read(0, 0x10)
    B.send(0, b"abc")
    B.write(0, b"abc")
    B.shutdown(0, 0)
    B.shutdown(0, 7)
    # RecvMMsg / SendMMsg go to bsd_mmsg.cpp for any descriptor that is not the co-op module's
    for fd in (0, 3, -1):
        B.q(29, struct.pack("<iIIIqq", fd, 0, DONTWAIT, 0, 0, 0), (("b", 0x100, None),))
        B.q(29, struct.pack("<iIIIqq", fd, 2, DONTWAIT, 0, 0, 0), (("b", 0x100, None),))
        B.q(29, struct.pack("<iIIIqq", fd, 1, 0, 0, -1, 0), (("b", 0x100, None),))
        B.q(30, struct.pack("<iII", fd, 0, 0), (("b", 0x100, None),))
        B.q(30, struct.pack("<iII", fd, 1, 0x20000), (("b", 0x40, None),))
    B.close(0)
    B.close(0)
    # loopback TCP: libuv's wake-up pair, data both ways, the end of data, a refused port, a closed peer
    B.socket(AF_INET, STREAM)                        # 0: the listener
    B.bind(0, sa(LO, 40200))
    B.listen(0, 1)
    B.name(16, 0)
    B.socket(AF_INET, STREAM | NONBLOCK, 6)          # 1
    B.poll([(0, POLLIN), (1, POLLIN | POLLOUT)], 0)
    B.connect(1, sa(LO, 40200))
    B.poll([(0, POLLIN), (1, POLLIN | POLLOUT)], 10)
    B.accept(0)                                      # 2 (a connection is waiting: the blocking accept returns at once)
    B.name(15, 1)
    B.name(15, 2)
    B.name(16, 2)
    B.write(1, b"hello")
    B.poll([(2, POLLIN), (-1, POLLIN), (77, POLLIN)], 0)
    B.read(2, 3)
    B.recv(2, 0x10, PEEK | DONTWAIT)
    B.recvfrom(2, 0x10, DONTWAIT)
    B.recv(2, 0x10)
    B.fcntl(2, F_SETFL, O_NONBLOCK)
    B.read(2, 0x10)
    B.send(2, b"back", 0)
    B.recvfrom(1, 0x10, 0)
    B.getopt(1, SOL_SOCKET, SO_TYPE, 4)
    B.getopt(0, SOL_SOCKET, SO_TYPE, 4)
    B.getopt(1, SOL_SOCKET, SO_ERROR, 4)
    B.socket(AF_INET, STREAM)                        # 3
    B.connect(3, sa(LO, 40201))                      # nobody listens
    B.connect(3, sa(0x0A000001, 80))                 # offline
    B.connect(3, sa(LO, 40200, family=0))
    B.connect(3, b"\x10\x02")
    B.connect(91, b"\x10\x02")
    B.listen(3, 5)
    B.shutdown(1, 1)
    B.poll([(2, POLLIN), (1, POLLOUT)], 0)
    B.read(2, 0x10)
    B.close(2)
    B.write(1, b"x")
    B.poll([(1, POLLIN | POLLOUT)], 0)
    B.socket(AF_INET, STREAM | NONBLOCK)             # 2 again
    B.bind(2, sa(0, 0))
    B.name(16, 2)
    B.connect(2, sa(LO, 40200))
    B.accept(0, 8)                                   # 4 (the lowest free number), the peer's address cut to 8 bytes
    B.close(4)
    B.fcntl(0, F_SETFL, O_NONBLOCK)
    B.accept(0)                                      # nothing waiting: EAGAIN
    B.accept(0, 0)
    # UDP over the loopback
    B.socket(AF_INET, DGRAM | NONBLOCK)              # 4
    B.socket(AF_INET, DGRAM | NONBLOCK)              # 5
    B.bind(4, sa(LO, 40300))
    B.bind(5, sa(LO, 40300))
    B.bind(5, sa(LO, 40301))
    B.sendto(5, b"ping", sa(LO, 40300))
    B.recvfrom(4, 0x40)
    B.connect(5, sa(LO, 40300))
    B.send(5, b"pong")
    B.recv(4, 0x40)
    B.name(15, 5)
    B.poll([(4, POLLIN), (5, POLLOUT)], 0)
    # event descriptors
    B.eventfd(0, 4)                                   # semaphore off, non-blocking
    B.eventfd(3, 5)
    B.eventfd(0, 0)
    B.read(6, 8)
    B.write(6, struct.pack("<Q", 2))
    B.poll([(6, POLLIN | POLLOUT), (7, POLLIN)], 0)
    B.read(6, 8)
    B.read(6, 8)
    B.read(7, 8)
    B.read(7, 4)
    B.write(7, struct.pack("<I", 1))
    B.write(8, struct.pack("<Q", 1))
    B.read(8, 8)
    B.close(8)
    # Poll: the wire checks, nothing to wait on, short waits
    B.poll([], 0)
    B.poll([], 0, nfds=1, insize=4)
    B.poll([], 0, nfds=0x1001)
    B.poll([], 0, nfds=0x1000, insize=8)
    B.poll([(4, POLLIN)], 0, nfds=2)
    B.poll([(4, POLLIN), (5, POLLOUT)], 0, outsize=8)
    B.poll([(4, POLLIN), (5, POLLOUT)], 0, outsize=0)
    B.poll([(4, POLLIN), (5, POLLOUT)], 0, insize=0)
    B.poll([(-1, POLLIN), (-5, POLLOUT)], -1)
    B.poll([(4, POLLIN)], 5)
    B.poll([(99, POLLIN)], 5)
    B.poll([(4, POLLIN), (6, POLLIN), (1, POLLOUT)], 3)
    B.poll([], 5)
    # Select: sets present or not, A or X, short and long sets, the timeout forms
    B.select(0)
    B.select(0, sec=0, usec=-5000)
    B.select(0, sec=-1, usec=0)
    B.select(0, forever=1)
    B.select(8, wr=[4, 5])
    B.select(8, rd=[4, 6], wr=[5], ex=[4])
    B.select(8, rd=[4], usec=4000)
    B.select(8, wr=[5], kind="x")
    B.select(8, wr=[5], size=8)
    B.select(8, wr=[5], size=0x200)
    B.select(8, wr=[5], outsize=4)
    B.select(8, wr=[5], outsize=0)
    B.select(6, wr=[5, 40])
    B.select(2000, wr=[5, 1023])
    B.select(-1, wr=[5])
    B.select(8, wr=[5], forever=1)
    B.select(100, rd=[90], usec=3000)
    B.select(8, wr=[5], sec=0x7FFFFFFFFFFFFFFF, usec=0x7FFFFFFFFFFFFFFF)
    B.select(8, wr=[5], sec=-0x7FFFFFFFFFFFFFFF, usec=-999999)
    B.select(8, wr=[5], sec=0x400000000000, usec=0)
    for fd in range(9):
        B.close(fd)
    # Socket and EventFd with every argument pattern (they make descriptors: a reset follows)
    every(s, b, BSD, [2, 3, 31], variants // 2)
    for name in ("bsd:s", "bsd:a"):
        b2 = s.open(name)
        Bsd(s, b2).socket(AF_INET, DGRAM)
        s.q(b2, BSD, 1)
    s.reset()
    b = s.open("bsd:u")
    Bsd(s, b).socket(AF_INET, DGRAM)
    Bsd(s, b).eventfd(0, 0)
    c = s.open("bsdcfg")
    s.sweep(c, "nn::bsdsocket::cfg::ServerInterface", range(0, 6))


# ---- ssl ----------------------------------------------------------------------------------------------------------------------------
def ssl(s, variants):
    sv = s.open("ssl")
    sv2 = s.open("ssl:s")
    s.q(sv, SSL, 1)
    c1 = obj(s, sv, SSL, 0, raw=struct.pack("<IIQ", 2, 0, 0), pid=True)
    s.q(sv, SSL, 1)
    c2 = obj(s, sv2, SSL, 0)
    s.q(sv2, SSL, 1)
    for cmd in (4, 12, 5, 8, 10, 4):
        s.q(c1 if cmd != 12 else c2, CTX, cmd, bufs=(("a", 0x20, bytes(range(0x20))),))
    s.q(c1, CTX, 13)
    s.q(c1, CTX, 3)
    k1 = obj(s, c1, CTX, 2)
    k2 = obj(s, c1, CTX, 2)
    s.q(c1, CTX, 3)
    s.q(c2, CTX, 3)
    close(s, k1)
    s.q(c1, CTX, 3)
    s.q(k2, CONN, 1, bufs=(("a", 16, text("game.example.com")),))
    s.q(k2, CONN, 1, bufs=(("a", 4, b"\x01\x02\x03\x04"),))
    s.q(k2, CONN, 1)
    every(s, k2, CONN, range(38), variants // 4, [38, 39, 100])
    close(s, k2)
    s.q(c1, CTX, 3)
    close(s, c2)
    s.q(sv, SSL, 1)
    for size in (4, 1, 0):
        s.q(sv, SSL, 8, bufs=((("b", size, None),) if size else ()))
    every(s, c1, CTX, [0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13], variants // 4, [14, 15, 100])
    every(s, sv, SSL, [1, 4, 5, 6, 7, 8, 9, 10, 11], variants // 4, [12, 13, 100])
    for raw in (b"", struct.pack("<i", 1), struct.pack("<i", -1)):
        s.q(sv, SSL, 3, bufs=(("a", len(raw), raw),) if raw else ())
        s.q(sv, SSL, 2, bufs=((("a", len(raw), raw),) if raw else ()) + (("b", 0x100, None),))
    # a reset with the contexts still held: the count starts over and never goes below 0
    c3 = obj(s, sv, SSL, 0)
    k3 = obj(s, c3, CTX, 2)
    s.lines.append("reset keep")
    s.q(sv, SSL, 1)
    s.q(c3, CTX, 3)
    close(s, k3)
    s.q(c3, CTX, 3)
    close(s, c3)
    close(s, c1)
    s.q(sv, SSL, 1)
    c4 = obj(s, sv, SSL, 0)
    s.q(sv, SSL, 1)
    s.q(c4, CTX, 4)


# ---- nim:eca, friend, prepo, pctl ---------------------------------------------------------------------------------------------------
def nim(s, variants):
    n = s.open("nim:eca")
    sv = obj(s, n, NIM, 0, raw=struct.pack("<Q", 0), pid=True, bufs=())
    sv2 = obj(s, n, NIM, 5)
    s.q(n, NIM, 4)
    a = obj(s, sv, SRV, 0)
    obj(s, sv2, SRV, 0)
    y = obj(s, a, ACC, 0)
    for cmd, raw, bufs in ((1, b"", ()), (2, struct.pack("<Q", 0), (("b", 8, None),)), (5, b"", ()), (4, b"", ()), (1, b"", ()),
                           (2, struct.pack("<Q", 0), (("b", 8, None),)), (2, struct.pack("<Q", 1), (("b", 8, None),)),
                           (2, struct.pack("<Q", 2), (("b", 8, None),)), (2, struct.pack("<Q", 0), (("b", 1, None),)),
                           (2, struct.pack("<Q", 0), ()), (2, struct.pack("<Q", 0xFFFFFFFFFFFFFFFF), (("b", 8, None),)),
                           (3, b"", ()), (0, b"", ()), (1, b"", ())):
        s.q(y, ASYNC, cmd, raw=raw, bufs=bufs)
    y2 = obj(s, a, ACC, 0)
    s.q(y2, ASYNC, 1)
    every(s, y, ASYNC, range(6), variants // 4, [6, 7])
    s.sweep(a, ACC, [1, 2])
    s.sweep(sv, SRV, [1, 2])
    every(s, n, NIM, [4], variants // 4, [1, 2, 3, 6, 7])


FRIEND_IDS = [1, 10100, 10101, 10102, 10110, 10111, 10120, 10121, 10200, 10211, 10400, 10420, 10421, 10500, 10501, 10502, 10503,
              10600, 10601, 10610, 10700, 10701, 10702, 11000, 11001, 20100, 20101, 20102, 20103, 20104, 20105, 20106, 20107, 20108,
              20109, 20110, 20111, 20200, 20201, 20202, 20203, 20300, 20301, 20400, 20401, 20402, 20500, 20501, 20502, 20600, 20601,
              20700, 20701, 20702, 20800, 20801, 20802, 20900, 21000, 22000, 22001, 22002, 22003, 22010, 30100, 30101, 30110, 30120,
              30121, 30130, 30131, 30190, 30200, 30201, 30202, 30203, 30204, 30205, 30210, 30211, 30212, 30213, 30214, 30215, 30216,
              30217, 30218, 30300, 30301, 30400, 30401, 30402, 30403, 30500, 30501, 30700, 30701, 30810, 30811, 30812, 30820, 30830,
              30900, 30901, 30910, 30911, 31000, 31001, 40100, 40400, 49900]


def friend(s, variants):
    for name in ("friend:u", "friend:v", "friend:m", "friend:s", "friend:a"):
        cr = s.open(name)
        f = obj(s, cr, FR + "IServiceCreator", 0)
        s.q(f, FR + "IFriendService", 0)
    no = obj(s, cr, FR + "IServiceCreator", 1, raw=bytes(16))
    d = obj(s, cr, FR + "IServiceCreator", 2)
    s.sweep(d, FR + "IDaemonSuspendSessionService", range(0, 4))
    for cmd in (0, 2, 1, 2, 0):
        s.q(no, FR + "INotificationService", cmd)
    s.sweep(no, FR + "INotificationService", [3, 4, 100])
    s.sweep(cr, FR + "IServiceCreator", [3, 4])
    for size in (0xE0, 0x400, 0x500, 0):
        for cmd in (20600, 20601):
            s.q(f, FR + "IFriendService", cmd, bufs=((("b", size, None),) if size else ()))
    for cmd in (10100, 10101, 10400, 20400):
        s.q(f, FR + "IFriendService", cmd, raw=bytes(0x18), bufs=(("b", 0x400, None),), pid=True)
    for cmd in FRIEND_IDS:
        s.fuzz(f, FR + "IFriendService", cmd, n=max(2, variants // 10))
    s.sweep(f, FR + "IFriendService", [2, 3, 10103, 10422, 20602, 30219, 31002, 49901])


PREPO_IDS = [10100, 10101, 10102, 10103, 10104, 10105, 10106, 10107, 10200, 10400]
PREPO_EXT = [10300, 20100, 20101, 20102, 20103]


def prepo(s, variants):
    for name in ("prepo:u", "prepo:a", "prepo:a2"):
        p = s.open(name)
        s.q(p, "nn::prepo::detail::ipc::IPrepoService", 10106, raw=struct.pack("<Q", 0), pid=True,
            bufs=(("x", 0x20, b"event_id" + bytes(24)), ("a", 0x40, bytes(range(0x40)))))
    every(s, p, "nn::prepo::detail::ipc::IPrepoService", PREPO_IDS, variants // 4, PREPO_EXT + [10500, 0, 10108, 10201, 30100, 40100])


PCTL_IDS = [1] + list(range(1001, 1024)) + list(range(1031, 1040)) + list(range(1042, 1052)) + list(range(1061, 1066)) + \
    list(range(1201, 1208)) + [1403, 1432, 1453, 1457, 1473]
PCTL_EXT = [1451, 1452, 1454, 1455, 1456, 1458, 1459, 1460, 145601, 195101]


def pctl(s, variants):
    for name in ("pctl", "pctl:s", "pctl:r", "pctl:a"):
        f = s.open(name)
        p = obj(s, f, PCTL_F, 0, raw=struct.pack("<Q", 0), pid=True)
        p2 = obj(s, f, PCTL_F, 1)
    s.sweep(f, PCTL_F, [2, 3])
    for cmd in (1207, 1432, 1457, 1473, 1207):
        s.q(p, PCTL, cmd)
    every(s, p, PCTL, PCTL_IDS, max(2, variants // 8), PCTL_EXT + [0, 2, 1000, 1024, 1030, 1040, 1041, 1052, 1066, 1200, 1208, 1400,
                                                                     1404, 1433, 1474, 2001])
    s.q(p2, PCTL, 1031)


# ---- bcat, nsd, sfdnsres, ntc, ngct / ngc, notif ------------------------------------------------------------------------------------
def bcat(s, variants):
    cr = s.open("bcat:u")
    obj(s, s.open("bcat:a"), BC + "IServiceCreator", 0)
    bs = obj(s, cr, BC + "IServiceCreator", 0, raw=struct.pack("<Q", 0), pid=True)
    st = obj(s, cr, BC + "IServiceCreator", 1, raw=struct.pack("<Q", 0), pid=True)
    st2 = obj(s, cr, BC + "IServiceCreator", 2, raw=struct.pack("<Q", 0x0100000000010000))
    dirname = b"dir1" + bytes(0x1C)
    progress = []
    for cmd, raw in ((10100, b""), (10101, dirname), (20100, struct.pack("<Q", 1)), (20101, struct.pack("<Q", 1) + dirname)):
        progress.append(obj(s, bs, BC + "IBcatService", cmd, raw=raw))
    nt = obj(s, bs, BC + "IBcatService", 20300)
    su = obj(s, bs, BC + "IBcatService", 20301)
    s.sweep(su, BC + "IDeliveryTaskSuspensionService", range(0, 3))
    for p in progress[:2]:
        s.q(p, BC + "IDeliveryCacheProgressService", 0)
        for size in (0x200, 0x100, 0x300, 0):
            s.q(p, BC + "IDeliveryCacheProgressService", 1, bufs=((("b", size, None),) if size else ()))
    every(s, progress[2], BC + "IDeliveryCacheProgressService", [0, 1], variants // 8, [2, 3])
    s.q(nt, BC + "INotifierService", 0)
    s.sweep(nt, BC + "INotifierService", [1, 2])
    fs = obj(s, st, BC + "IDeliveryCacheStorageService", 0)
    ds = obj(s, st2, BC + "IDeliveryCacheStorageService", 1)
    s.q(fs, BC + "IDeliveryCacheFileService", 0, raw=dirname + b"file.bin" + bytes(0x18))
    s.q(fs, BC + "IDeliveryCacheFileService", 0, raw=b"\x01\x02" + bytes(0x3E))
    s.q(ds, BC + "IDeliveryCacheDirectoryService", 0, raw=dirname)
    s.q(st, BC + "IDeliveryCacheStorageService", 10, bufs=(("b", 0x400, None),))
    every(s, fs, BC + "IDeliveryCacheFileService", range(4), variants // 4, [4, 5])
    every(s, ds, BC + "IDeliveryCacheDirectoryService", range(3), variants // 4, [3, 4])
    every(s, st, BC + "IDeliveryCacheStorageService", [10], variants // 4, [2, 3, 9, 11])
    every(s, bs, BC + "IBcatService", [10100, 10101, 20100, 20101, 20300, 20301], max(2, variants // 8),
          [0, 10200, 20102, 20200, 20302, 30100, 30200, 90100])
    s.sweep(cr, BC + "IServiceCreator", [3, 4])


def nsd(s, variants):
    for name in ("nsd:u", "nsd:a"):
        n = s.open(name)
        s.q(n, NSD, 100)
    name = b"a.b\0"
    for cmd, bufs in ((11, (("b", 8, None),)), (11, (("b", 0x10, None),)), (11, (("b", 4, None),)), (11, ()),
                      (20, (("a", 4, name), ("b", 0x100, None))), (20, (("x", 0x100, b"srv.example" + bytes(0xF5)), ("c", 0x100, None))),
                      (20, (("a", 0x100, b"long" * 0x40), ("b", 8, None))), (20, (("b", 8, None),)), (20, (("a", 4, name),)),
                      (21, (("a", 4, name), ("b", 0x100, None))), (21, ()), (15, ()), (100, ())):
        s.q(n, NSD, cmd, bufs=bufs)
    every(s, n, NSD, [11, 15, 20, 21, 100], variants // 4, [0, 10, 12, 13, 14, 16, 30, 31, 42, 43, 50, 60, 101, 102])


def sfdnsres(s, variants, sid=None):
    d = sid if sid is not None else s.open("sfdnsres")
    host, service = b"game.example.com\0", b"443\0"
    q6 = struct.pack("<BIQ", 0, 0, 0)
    s.q(d, DNS, 6, raw=q6, pid=True, bufs=(("a", len(host), host), ("a", len(service), service), ("a", 0, b""), ("b", 0x100, None)))
    s.q(d, DNS, 12, raw=q6, bufs=(("a", len(host), host), ("a", len(service), service), ("a", 0, b""), ("a", 0, b""), ("b", 0x100, None)))
    s.q(d, DNS, 2, raw=struct.pack("<BIQ", 0, 0, 0), bufs=(("a", len(host), host), ("b", 0x100, None)))
    s.q(d, DNS, 3, bufs=(("a", 4, bytes([10, 0, 0, 1])), ("b", 0x100, None)))
    s.q(d, DNS, 10, bufs=(("a", len(host), host), ("b", 0x100, None)))
    s.q(d, DNS, 11)
    s.q(d, DNS, 14)
    for _ in range(3):
        s.q(d, DNS, 8)
    s.q(d, DNS, 9, raw=struct.pack("<I", 1))
    for cmd in (4, 5):
        for code in range(-2, 17):
            s.q(d, DNS, cmd, raw=struct.pack("<i", code), bufs=(("b", 0x80, None),))
        for size in (4, 1, 0):
            s.q(d, DNS, cmd, raw=struct.pack("<i", 8), bufs=((("b", size, None),) if size else ()))
        s.q(d, DNS, cmd, raw=struct.pack("<i", 0x7FFFFFFF), bufs=(("b", 0x80, None),))
    s.q(d, DNS, 6, raw=q6, bufs=(("a", 3, b"\x01\x02\x03"), ("a", 0, b"")))
    if sid is None:
        every(s, d, DNS, [2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 14], variants // 4, [0, 1, 7, 13, 15, 16, 100])


def ntc(s, variants):
    n = s.open("ntc")
    t1 = obj(s, n, NTC, 0, raw=struct.pack("<I", 0))
    t2 = obj(s, n, NTC, 0)
    for cmd in (2, 1, 0, 2, 1, 3, 2, 0, 2, 4, 5):
        s.q(t1, TASK, cmd)
    for cmd in (3, 2, 1, 0):
        s.q(t2, TASK, cmd)
    s.config("net", "online_test", "true")
    s.q(t2, TASK, 0)
    s.q(t2, TASK, 2)
    s.config("net", "online_test", "false")
    s.q(t2, TASK, 0)
    every(s, t1, TASK, [0, 2, 3], variants // 8, [4, 5, 6, 7, 100])
    s.sweep(n, NTC, [1, 2, 100, 101, 102])


def words(s, variants):
    c = s.open("ngct:u")
    g = s.open("ngc:u")
    for bufs in ((("a", 6, b"hello\0"), ("b", 16, None)), (("x", 0x20, b"bad word" + bytes(24)), ("c", 8, None)), (("b", 16, None),),
                 (("a", 6, b"hello\0"),), ()):
        s.q(c, NGCT, 0, bufs=bufs)
        s.q(c, NGCT, 1, bufs=bufs)
        s.q(g, NGC, 1, raw=struct.pack("<II", 0, 0xFF), bufs=bufs)
        s.q(g, NGC, 2, raw=struct.pack("<II", 0, 0xFF), bufs=bufs)
        s.q(g, NGC, 4, raw=struct.pack("<II", 0, 0xFF), bufs=bufs)
        s.q(g, NGC, 5, raw=struct.pack("<II", 0, 0xFF), bufs=bufs)
    every(s, c, NGCT, [0, 1], variants // 4, [2, 3, 4])
    every(s, g, NGC, [0, 1, 2, 3], variants // 4, [4, 5, 6, 7, 100])
    no = s.open("notif:a")
    every(s, no, NOTIF, [520, 1000], variants // 4, [500, 510, 530, 540, 0, 1, 521, 1001, 1010])


def redirected(s, variants):
    # a redirect table (a local target: nothing is connected to in this script) turns nifm and ntc online and wraps bsd, sfdnsres
    # and every new ssl connection
    s.config("net", "redirect", "game.example.com=127.0.0.1:9")
    s.reset()
    st = s.open("nifm:u")
    g = obj(s, st, NIFM, 5, raw=bytes(8), pid=True)
    nifm_getters(s, g)
    r = obj(s, g, GEN, 4)
    request_machine(s, r)
    d = s.open("sfdnsres")
    sfdnsres(s, variants, sid=d)
    b = s.open("bsd:u")
    B = Bsd(s, b)
    B.socket(AF_INET, STREAM)
    B.connect(0, sa(0x0A000001, 80))
    B.socket(AF_INET, DGRAM | NONBLOCK)
    B.recvfrom(1, 0x40)
    sv = s.open("ssl")
    c = obj(s, sv, SSL, 0)
    k = obj(s, c, CTX, 2)
    for cmd, bufs in ((1, (("a", 17, b"game.example.com\0"),)), (4, ()), (3, ()), (8, ()), (10, (("b", 0x10, None),)), (0, ())):
        s.q(k, CONN, cmd, raw=struct.pack("<i", 0), bufs=bufs)
    s.q(c, CTX, 3)
    close(s, k)
    s.q(c, CTX, 3)
    n = s.open("ntc")
    t = obj(s, n, NTC, 0)
    s.q(t, TASK, 0)
    s.config("net", "redirect", "")
    s.q(t, TASK, 0)
    s.q(g, GEN, 12)
    s.reset()


def coop(s, variants):
    # [net] protocol = native: the co-op module's virtual descriptors (0x100..). mode host (loopback scope) and only ldn Initialize: the
    # module starts its I/O thread and opens no host socket (it would listen on 127.0.0.1 only once the game hosts, which is not asked)
    s.config("net", "protocol", "native")
    s.config("net", "mode", "host:47699")
    s.config("net", "lan_sweep", "off")
    s.config("net", "uid", "0123456789abcdef0123456789abcdef")
    s.reset()
    b = s.open("bsd:u")
    B = Bsd(s, b)
    B.socket(AF_INET, DGRAM)                          # no ldn yet: the socket table's fd 0
    cr = s.open("ldn:u")
    ldn = obj(s, cr, "nn::ldn::detail::IUserServiceCreator", 0)
    s.q(ldn, "nn::ldn::detail::IUserLocalCommunicationService", 0)
    s.q(ldn, "nn::ldn::detail::IUserLocalCommunicationService", 400, raw=struct.pack("<Q", 0), pid=True)   # Initialize
    s.q(ldn, "nn::ldn::detail::IUserLocalCommunicationService", 0)
    V, W = 0x100, 0x101
    B.socket(AF_INET, DGRAM)                          # 0x100
    B.socket(AF_INET, DGRAM | NONBLOCK, 17)           # 0x101
    B.socket(AF_INET, STREAM)                         # a stream socket stays on the table: 1
    B.socket(AF_INET6, DGRAM)
    B.fcntl(V, F_GETFL)
    B.fcntl(V, F_SETFL, O_NONBLOCK)
    B.fcntl(V, F_GETFL)
    B.fcntl(W, F_GETFL)
    B.fcntl(V, 99, 0)
    B.bind(V, sa(0, 40500))
    B.bind(W, sa(0, 40500))
    B.bind(W, b"\x10\x02\x9e")
    B.bind(W, b"")
    B.bind(W, sa(0, 40501))
    for size in (16, 8, 0):
        B.name(16, V, size)
    B.name(16, 0x11F)
    B.name(15, V)
    B.setopt(V, SOL_SOCKET, SO_BROADCAST, struct.pack("<I", 1))
    B.setopt(V, SOL_SOCKET, SO_RCVBUF, None)
    for size in (4, 1, 0, 0x200):
        B.getopt(V, SOL_SOCKET, SO_BROADCAST, size)
    B.getopt(V, SOL_SOCKET, SO_TYPE, 4)
    B.getopt(V, 0x1234, 0x5678, 4)
    B.recvfrom(V, 0x100, flags=0)
    B.recvfrom(W, 0x100, flags=DONTWAIT, asize=4)
    B.recv(V, 0x40, flags=0)
    B.read(V, 0x40)
    B.sendto(V, b"hello", sa(0xA9FE0102, 40501))
    B.sendto(V, b"hello", b"")
    B.sendto(V, b"hello", b"\x10\x02\x9e\x55")
    B.send(V, b"x")
    B.write(V, b"x")
    B.sendto(V, b"y" * 5000, sa(0xA9FE0102, 40501))
    B.recvfrom(W, 0x100, flags=DONTWAIT)
    for cmd in (12, 18, 14, 15, 22):                  # Accept, Listen, Connect, GetPeerName, Shutdown: no streams here
        if cmd == 14:
            B.connect(V, sa(LO, 80))
            B.connect(V, sa(0x0A000001, 80))
        elif cmd in (12, 15):
            B.name(cmd, V)
        elif cmd == 18:
            B.listen(V, 1)
        else:
            B.shutdown(V, 2)
    B.q(29, struct.pack("<iIIIqq", V, 1, DONTWAIT, 0, 0, 0), (("b", 0x100, None),))
    B.q(30, struct.pack("<iII", V, 1, 0), (("b", 0x100, None),))
    B.poll([(V, POLLIN | POLLOUT), (W, POLLIN)], 0)
    B.poll([(V, POLLIN), (-1, POLLIN)], 3)
    B.poll([(-1, POLLIN)], 0)
    B.poll([(V, POLLIN), (0, POLLOUT)], 0)            # a table descriptor too: the table answers
    B.poll([(0x11E, POLLIN)], 0)                      # never made: not the module's
    B.select(0x110, rd=[V, W], wr=[V])
    B.select(0x110, rd=[V], usec=2000)
    B.select(0x110, wr=[V, 0])
    B.select(0x110, rd=[V], kind="x", size=0x40)
    B.select(0x100, wr=[V])                           # nfds stops below it: the table
    B.select(0x110, wr=[V], forever=1)
    B.eventfd(0, 4)
    B.close(W)
    B.close(W)
    B.socket(AF_INET, DGRAM)                          # 0x101 again
    s.q(ldn, "nn::ldn::detail::IUserLocalCommunicationService", 401)                                       # Finalize
    B.socket(AF_INET, DGRAM)                          # the module is not initialized: the table
    B.recvfrom(V, 0x100, flags=DONTWAIT)
    B.close(V)
    s.config("net", "protocol", "linkplay")
    s.config("net", "mode", "")
    s.config("net", "uid", "")
    s.reset()


def generate(s, variants, variant=None):
    if variant == "coop":
        coop(s, variants)
        return
    s.config("net", "online_test", "false")
    s.config("net", "redirect", "")
    s.reset()
    nifm(s, variants)
    s.reset()
    bsd(s, variants)
    s.reset()
    ssl(s, variants)
    nim(s, variants)
    friend(s, variants)
    prepo(s, variants)
    pctl(s, variants)
    bcat(s, variants)
    nsd(s, variants)
    sfdnsres(s, variants)
    ntc(s, variants)
    words(s, variants)
    s.open("ldn:u")   # the object is ldn.cpp's: only that it is there (none of its commands: they start the local wireless lobby)
    redirected(s, variants)
    # after a reset the counters start over: client id 1, PKI id 1, cancel handle 1, no live context
    st = s.open("nifm:u")
    g = obj(s, st, NIFM, 5, raw=bytes(8), pid=True)
    s.q(g, GEN, 1, bufs=(("b", 4, None),))
    sv = s.open("ssl")
    s.q(sv, SSL, 1)
    c = obj(s, sv, SSL, 0)
    s.q(c, CTX, 4)
    d = s.open("sfdnsres")
    s.q(d, DNS, 8)
