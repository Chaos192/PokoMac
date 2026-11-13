// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// The socket table behind the bsd HLE (multiplayer phase 1): UDP sockets over a vnet::Link. Game-independent; the IPC side is
// make_bsd() in services/network.cpp. Every call answers (ret, errno) the way bsd:u does: ret -1 with a Linux errno number on failure
// (Switchbrew "Sockets services"; Ryujinx Sockets/Bsd uses LinuxError the same way).
//
// What is modelled (Linux/FreeBSD UDP behaviour):
//   - AF_INET + SOCK_DGRAM, and AF_INET + SOCK_STREAM over the loopback (below); SOCK_NONBLOCK / SOCK_CLOEXEC flags in the type. AF_INET6:
//     EAFNOSUPPORT (Lichii's answer: dual-stack code then falls back to IPv4). Other domains: ENETDOWN, as the offline bsd answered before. Descriptors are the lowest free from 0 (as Ryujinx hands them out), at most 128.
//   - bind (port 0 = ephemeral from 49152, address 0.0.0.0 or the link's IP), connect (sets the default peer), sendto/send, recvfrom/recv
//     (MSG_DONTWAIT, MSG_PEEK, O_NONBLOCK, SO_RCVTIMEO; a blocking call waits until a datagram arrives), getsockname/getpeername, shutdown,
//     close, fcntl F_GETFL/F_SETFL O_NONBLOCK, set/getsockopt at SOL_SOCKET (SO_BROADCAST, SO_REUSEADDR, SO_REUSEPORT, SO_RCVBUF, SO_SNDBUF,
//     SO_RCVTIMEO, SO_SNDTIMEO, SO_TYPE, SO_ERROR); any other option is accepted, kept for getsockopt and logged once.
//   - Delivery: a datagram goes to sockets bound to its port with address 0.0.0.0 or the destination IP. Unicast reaches one socket (the
//     exact-address one first), a broadcast (x.y.z.255, or 255.255.255.255 sent as the link's /24 broadcast) every 0.0.0.0 socket on the
//     port (Linux: receiving broadcast needs no SO_BROADCAST; sending does, else EACCES). The sender's own broadcast is not looped back
//     (a real NIC does not); a unicast to the node's own IP is. Receive queue: 64 datagrams per socket, the oldest dropped.
//   - Errors: bad fd EBADF; no link (or the link is down) ENETUNREACH; destination outside 169.254.0.0/16 ENETDOWN; > 4096 bytes EMSGSIZE;
//     port in use without SO_REUSEADDR/SO_REUSEPORT on both EADDRINUSE; nothing queued and non-blocking EAGAIN.
//   - Event descriptors (nn::socket::EventFd, FreeBSD eventfd(2) semantics, as Lichii's bsd.cpp serves them): an 8-byte counter in the same
//     descriptor table, written and read with Write/Read (Send/Recv), EFD_SEMAPHORE (1) and EFD_NONBLOCK (4). They need no network: gRPC
//     (Nintendo's NPLN client in SDK 22 titles, Ocarina of Time) wakes its pollers with one.
//   - TCP over the loopback, which a console has with no network (its BSD stack's lo0): a stream socket binds to 127.0.0.0/8, 0.0.0.0 or
//     the link's address, listens, and a connect to a listening port on 127.0.0.0/8 (or the link's own address) completes at once into
//     the listener's queue (backlog), where accept takes it; the two ends then pass bytes (256 KiB in flight per direction; a full one
//     blocks a writer, EAGAIN when non-blocking), shutdown(SHUT_WR) and close give the other end its end of data (recv answers 0), and a
//     write to a closed peer EPIPE. A connect to another node of the link's network (169.254.0.0/16, a local wireless room) is TCP between
//     the nodes (below); anywhere else ENETDOWN (offline), to a port nobody listens on ECONNREFUSED. libuv makes
//     its loop's wake-up pair this way (socket, bind 127.0.0.1:0, listen, getsockname, connect, accept): Star Fox's HTTP loops failed to
//     start while SOCK_STREAM was refused, and their thread then ran the loop with no internal fields and crashed at a null read.
//   - TCP between the nodes of a local wireless network: a stream socket connected to another node's address (or accepted by a listener
//     from one) carries its bytes in the link's datagrams, as a small reliable stream protocol of its own ("vtcp": the datagrams have
//     ports 0 and 0, which no UDP socket uses, and a 24-byte header: "vT", version, type SYN / SYNACK / DATA / ACK / FIN / RST / WINQ,
//     the two ports, a connection id the connecting side draws at random, seq, ack, window). Sequence numbers count bytes; every DATA and
//     FIN is acknowledged at once with the receiver's free window (256 KiB per direction, as the loopback); at most 64 KiB are in flight,
//     in segments of at most 1200 bytes (one relay UDP packet under a 1500-byte MTU); what is not acknowledged within the retransmission
//     timeout (200 ms, doubling to 3 s) is sent again from the first byte not acknowledged; segments that come early wait in order. A
//     connect waits for SYNACK (non-blocking: EINPROGRESS, then writable with SO_ERROR); a port nobody listens on answers RST
//     (ECONNREFUSED). No answer: SYN for about 12 s, data for about 20 s, then ETIMEDOUT. Close sends FIN after the data and keeps the
//     connection until it is acknowledged (10 s at most); bytes that arrive for a closed socket are answered RST (ECONNRESET at the
//     other end). The link carries them like any datagram (a relay over TCP or UDP, a LAN): loss, duplicates and reordering are handled.
//   - poll / select over the table's descriptors: a datagram socket is readable with a datagram queued (or shut down for reading) and always
//     writable; a stream socket readable with bytes or the end of data waiting (a listener: a connection to accept), writable while the
//     peer's buffer has room; an event descriptor readable with a non-zero counter, writable below its maximum; a number the table never gave out is
//     never ready. No descriptor at all: the wait runs out its timeout and answers 0 (an endless one answers after 100 ms, so a poller
//     with nothing to watch does not hang).
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "vnet.h"

namespace nxrt::vsock {

// ---- Switch bsd constants (FreeBSD numbering for flags and options, Linux numbers for errno) ---------------------------------------------
constexpr int kAfInet = 2, kAfInet6 = 28;   // FreeBSD AF_INET6
constexpr int kSockStream = 1, kSockDgram = 2;
constexpr int kSockNonblock = 0x20000000, kSockCloexec = 0x40000000;   // flags in Socket's type
constexpr int kIpprotoTcp = 6, kIpprotoUdp = 17;
constexpr int kMsgPeek = 0x2, kMsgDontwait = 0x80;
constexpr int kSolSocket = 0xFFFF;
constexpr int kSoReuseAddr = 0x4, kSoBroadcast = 0x20, kSoReusePort = 0x200;
constexpr int kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002, kSoSndTimeo = 0x1005, kSoRcvTimeo = 0x1006, kSoError = 0x1007, kSoType = 0x1008;
constexpr int kFGetFl = 3, kFSetFl = 4, kONonblock = 0x800;
constexpr int kShutRd = 0, kShutWr = 1, kShutRdWr = 2;

constexpr uint32_t kEbadf = 9, kEagain = 11, kEacces = 13, kEinval = 22, kEmfile = 24, kEpipe = 32, kEdestaddrreq = 89, kEmsgsize = 90,
                   kEprotonosupport = 93, kEopnotsupp = 95, kEafnosupport = 97, kEaddrinuse = 98, kEaddrnotavail = 99, kEnetdown = 100,
                   kEnetunreach = 101, kEconnreset = 104, kEisconn = 106, kEnotconn = 107, kEtimedout = 110, kEconnrefused = 111,
                   kEalready = 114, kEinprogress = 115;

constexpr int kEfdSemaphore = 1, kEfdNonblock = 4;
constexpr int16_t kPollIn = 0x1, kPollPri = 0x2, kPollOut = 0x4, kPollErr = 0x8, kPollHup = 0x10;
constexpr int kMaxSockets = 128;

struct PollFd {   // struct pollfd as bsd's Poll sends it
    int32_t fd;
    int16_t events;
    int16_t revents;
};
constexpr size_t kQueueCap = 64;
constexpr size_t kStreamBuf = 256 * 1024;   // bytes in flight towards one end of a loopback connection
constexpr int kMaxBacklog = 128;
constexpr uint16_t kEphemeralFirst = 49152;

struct Addr {
    uint32_t ip = 0;     // host order
    uint16_t port = 0;   // host order
};

// (ret, errno): ret -1 and err != 0 on failure.
struct Ret {
    int32_t ret = 0;
    uint32_t err = 0;
};

class Stack {
public:
    // fixed: the link this table always uses. Null: it follows vnet::current_link() (only the process table, stack(), does that).
    explicit Stack(std::shared_ptr<vnet::Link> fixed = nullptr);
    ~Stack();
    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;

    Ret socket(int domain, int type, int protocol);
    Ret bind(int fd, Addr a);
    Ret connect(int fd, Addr a);
    Ret listen(int fd, int backlog);   // stream sockets
    Ret accept(int fd, Addr* from);    // from may be null
    Ret sendto(int fd, const void* p, size_t n, int flags, const Addr* to);   // to null: the connected peer (send)
    Ret recvfrom(int fd, void* p, size_t n, int flags, Addr* from);           // from may be null (recv)
    Ret setsockopt(int fd, int level, int opt, const void* v, size_t n);
    Ret getsockopt(int fd, int level, int opt, void* v, size_t* n);
    Ret fcntl(int fd, int cmd, int arg);
    Ret getsockname(int fd, Addr* a);
    Ret getpeername(int fd, Addr* a);
    Ret shutdown(int fd, int how);
    Ret close(int fd);
    bool is_socket(int fd);
    Ret eventfd(uint64_t initval, uint32_t flags);
    Ret poll(PollFd* fds, size_t n, int timeout_ms);                                      // timeout < 0: no timeout
    Ret select(int nfds, uint64_t* rd, uint64_t* wr, uint64_t* ex, int timeout_ms);     // 1024-bit sets, null when absent
    void reset();   // closes every socket (waiting receivers return EBADF)

    void inbound(const vnet::Datagram& d);   // a datagram from the link

private:
    struct Sock;
    std::shared_ptr<vnet::Link> link() const;
    std::shared_ptr<Sock> get(int fd);
    bool port_free(uint16_t port, uint32_t ip, const Sock& s) const;   // caller holds mu_
    uint32_t autobind(Sock& s);                                        // caller holds mu_; 0 or errno
    Ret wait_recv(const std::shared_ptr<Sock>& s, void* p, size_t n, int flags, Addr* from);
    Ret event_write(const std::shared_ptr<Sock>& s, const void* p, size_t n, int flags);
    Ret stream_write(const std::shared_ptr<Sock>& s, const void* p, size_t n, int flags);
    Ret stream_connect(const std::shared_ptr<Sock>& s, Addr a, uint32_t me);   // caller holds mu_
    // TCP between nodes (vtcp): caller holds lk on mu_; the link is called with it released.
    Ret remote_connect(std::unique_lock<std::mutex>& lk, const std::shared_ptr<Sock>& s, Addr a, uint32_t me, std::shared_ptr<vnet::Link> l);
    Ret remote_write(const std::shared_ptr<Sock>& s, const void* p, size_t n, int flags);
    void segment_in(const vnet::Datagram& d);                                                 // a vtcp datagram from the link
    void pump_locked(Sock& s, int64_t now, std::vector<vnet::Datagram>* out);               // caller holds mu_: what may go out now
    void remote_close_locked(const std::shared_ptr<Sock>& s, std::vector<vnet::Datagram>* out);   // caller holds mu_
    void tick();                                                                              // the timer thread: retransmissions
    void ensure_timer_locked();                                                               // caller holds mu_
    void transmit(std::vector<vnet::Datagram>& out);                                          // mu_ released: out to the link
    static uint32_t vt_window(const Sock& s);                                                 // the receive window it advertises
    static vnet::Datagram vt_ack(const Sock& s);                                              // an ACK with its window
    void stream_hang_up(Sock& s);                                              // caller holds mu_: the other end reads its end of data
    int16_t ready_locked(int fd, int16_t events);                      // caller holds mu_: the revents of one descriptor

    std::shared_ptr<vnet::Link> fixed_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<int, std::shared_ptr<Sock>> socks_;
    std::set<uint64_t> logged_opts_;   // (level << 32 | opt) of the unknown options already logged
    uint16_t next_port_ = kEphemeralFirst;
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<Sock>> conns_;   // vtcp: (peer IP, connection id) -> the socket (closed too)
    std::deque<vnet::Datagram> outq_;   // vtcp answers segment_in made: the timer thread sends them
    std::thread timer_;
    bool timer_stop_ = false;
};

// The process-wide table the bsd services use. It follows vnet::current_link().
Stack& stack();

}  // namespace nxrt::vsock
