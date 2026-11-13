// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// UDP socket table over a vnet::Link, and TCP over the loopback (see vsock.h). One mutex guards the table and every socket's state; one condition variable wakes
// blocking receivers. The link is never called with the mutex held (a hub delivers on the sending thread, back into inbound()).
#include "vsock.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "nxrt/log.h"

namespace nxrt::vsock {

struct Stack::Sock {
    bool nonblock = false;
    bool bound = false;
    Addr local;
    bool connected = false;
    Addr peer;
    bool broadcast = false, reuse_addr = false, reuse_port = false;
    bool shut_rd = false, shut_wr = false;
    bool closed = false;
    int32_t rcvbuf = 42080, sndbuf = 9216;   // FreeBSD's UDP defaults (net.inet.udp.recvspace / maxdgram)
    int64_t rcvtimeo_us = 0;                 // 0 = wait forever
    uint8_t sndtimeo[16] = {};
    std::map<uint64_t, std::vector<uint8_t>> other_opts;   // (level << 32 | opt) -> value, for getsockopt
    std::deque<vnet::Datagram> q;
    bool event = false;        // an EventFd descriptor: the counter below instead of a datagram queue
    bool semaphore = false;
    uint64_t counter = 0;
    // SOCK_STREAM over the loopback
    bool stream = false;
    bool listening = false;
    bool accepted = false;     // made by a connect into a listener's queue: it shares the listener's port
    int backlog = 0;
    std::deque<std::shared_ptr<Sock>> pending;   // a listener's connections that accept has not taken yet
    std::weak_ptr<Sock> other;                   // a connected stream socket's other end
    std::deque<uint8_t> rx;                      // the bytes the other end sent
    bool rx_eof = false;                         // the other end shut down writing or closed: recv answers 0 once rx is empty
    // SOCK_STREAM to another node of the link (vtcp, vsock.h): the other end is remote, its bytes come and go in segments
    bool remote = false;
    bool established = false;                    // SYNACK in (connecting side) / SYN in (accepted side)
    bool initiator = false;
    bool vt_reset = false;                       // RST in, refused, or given up: rx_eof is set, so_error says why
    uint32_t conn = 0;                           // connection id (the connecting side's draw)
    uint32_t snd_una = 0, snd_nxt = 0, snd_max = 0;   // first byte not acknowledged, next to send, highest sent
    std::deque<uint8_t> sndq;                    // bytes from snd_una on: sent and not acknowledged, then not sent yet
    uint32_t peer_wnd = 0;
    uint32_t rcv_nxt = 0;                        // next byte expected from the peer
    std::map<uint32_t, std::vector<uint8_t>> ooo;   // segments that came early, by sequence number
    size_t ooo_bytes = 0;
    bool fin_queued = false, fin_sent = false, fin_acked = false;
    uint32_t fin_seq = 0;
    bool peer_fin = false;
    uint32_t peer_fin_seq = 0;
    uint32_t so_error = 0;                       // SO_ERROR: why a connect or the connection failed (read once)
    int64_t rto_ms = 0, rto_at = 0;              // retransmission timeout and when it runs out (0: nothing to time)
    int tries = 0;                               // timeouts in a row without progress
    int64_t linger_until = 0;                    // a closed socket's connection waits for its FIN until then
    int64_t winq_at = 0;                         // a shut window was last asked about
    uint32_t cwnd = 64 * 1024;                   // bytes it may have in flight now: one segment after a timeout, doubled by each
                                                 // acknowledgment that moves on (TCP's slow start), up to kVtInflight
};

namespace {
constexpr uint64_t kEventMax = 0xFFFFFFFFFFFFFFFEull;   // an event counter's largest value (eventfd(2))
Ret ok(int32_t v = 0) { return {v, 0}; }
Ret fail(uint32_t e) { return {-1, e}; }
constexpr uint32_t kLimitedBroadcast = 0xFFFFFFFFu;
bool is_loopback(uint32_t ip) { return (ip >> 24) == 127; }
// The link's network (vnet::Link::reaches / netmask); with no link, the virtual LAN's rules.
bool reaches(const std::shared_ptr<vnet::Link>& l, uint32_t ip) { return l ? l->reaches(ip) : vnet::is_link_local(ip); }
uint32_t mask_of(const std::shared_ptr<vnet::Link>& l) { return l ? l->netmask() : 0xFFFFFF00u; }
bool is_bcast(uint32_t ip, uint32_t mask) { return (ip | mask) == 0xFFFFFFFFu; }   // its host part all ones (x.y.z.255 of a /24)

// The payload's first kDumpBytes bytes as hex for the Debug udp lines ("|" every 16 bytes, "..." when cut). 192 covers Pokopia's KTGL
// join request (171 bytes with its 4-byte routing header, work/mp/join-reject-analysis.md). Only built when Debug is on (NXLOG_D).
constexpr size_t kDumpBytes = 192;
std::string hex_head(const std::vector<uint8_t>& b) {
    static const char kHex[] = "0123456789abcdef";
    const size_t n = b.size() < kDumpBytes ? b.size() : kDumpBytes;
    std::string s;
    s.reserve(n * 3 + n / 16 * 2 + 4);
    for (size_t i = 0; i < n; ++i) {
        if (i) s += (i % 16) ? " " : " | ";
        s += kHex[b[i] >> 4];
        s += kHex[b[i] & 15];
    }
    if (n < b.size()) s += " ...";
    return s;
}
}  // namespace

// ---- TCP between nodes (vtcp, vsock.h) -------------------------------------------------------------------------------------------------
namespace {
constexpr uint8_t kVtMagic0 = 'v', kVtMagic1 = 'T', kVtVersion = 1;
enum : uint8_t { kVtSyn = 1, kVtSynAck = 2, kVtData = 3, kVtAck = 4, kVtFin = 5, kVtRst = 6, kVtWinQ = 7 };
constexpr size_t kVtHdr = 24;
constexpr size_t kVtSeg = 1200;               // payload per segment: one relay UDP packet under a 1500-byte MTU
constexpr uint32_t kVtInflight = 64 * 1024;   // bytes sent and not yet acknowledged
constexpr int64_t kVtRtoMin = 200, kVtRtoMax = 3000;
constexpr int kVtSynTries = 8;                // SYN: ~12 s with the doubling timeout
constexpr int kVtDataTries = 10;              // data / FIN without progress: ~20 s
constexpr int64_t kVtLingerMs = 10000;        // a closed socket's connection stays for its FIN this long
constexpr int64_t kVtTickMs = 50;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool seq_lt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }
bool seq_le(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) <= 0; }

void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint16_t get16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

struct VtHdr {
    uint8_t type = 0;
    uint16_t sport = 0, dport = 0;
    uint32_t conn = 0, seq = 0, ack = 0, wnd = 0;
};

vnet::Datagram vt_make(uint32_t src_ip, uint32_t dst_ip, const VtHdr& h, const uint8_t* p = nullptr, size_t n = 0) {
    vnet::Datagram d;
    d.src_ip = src_ip;
    d.dst_ip = dst_ip;   // ports 0 and 0: never a UDP socket's
    d.data.resize(kVtHdr + n);
    uint8_t* b = d.data.data();
    b[0] = kVtMagic0;
    b[1] = kVtMagic1;
    b[2] = kVtVersion;
    b[3] = h.type;
    put16(b + 4, h.sport);
    put16(b + 6, h.dport);
    put32(b + 8, h.conn);
    put32(b + 12, h.seq);
    put32(b + 16, h.ack);
    put32(b + 20, h.wnd);
    if (n) std::memcpy(b + kVtHdr, p, n);
    return d;
}

bool vt_parse(const vnet::Datagram& d, VtHdr* h) {
    const std::vector<uint8_t>& b = d.data;
    if (d.src_port || d.dst_port || b.size() < kVtHdr || b[0] != kVtMagic0 || b[1] != kVtMagic1 || b[2] != kVtVersion) return false;
    h->type = b[3];
    h->sport = get16(&b[4]);
    h->dport = get16(&b[6]);
    h->conn = get32(&b[8]);
    h->seq = get32(&b[12]);
    h->ack = get32(&b[16]);
    h->wnd = get32(&b[20]);
    return true;
}

uint32_t vt_conn_id() {
    static std::mutex mu;
    static std::mt19937 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lk(mu);
    uint32_t v = 0;
    while (v == 0) v = rng();
    return v;
}
}  // namespace

void Stack::ensure_timer_locked() {
    if (timer_.joinable()) return;
    timer_stop_ = false;
    timer_ = std::thread([this] { tick(); });
}

void Stack::transmit(std::vector<vnet::Datagram>& out) {
    if (out.empty()) return;
    const auto l = link();
    for (const vnet::Datagram& d : out)
        if (l) l->send(d);   // a link that went down loses them: the timers notice
    out.clear();
}

void Stack::pump_locked(Sock& s, int64_t now, std::vector<vnet::Datagram>* out) {
    if (!s.remote || !s.established || s.vt_reset) return;
    const uint32_t inflight = s.snd_nxt - s.snd_una;
    const uint32_t queued = static_cast<uint32_t>(s.sndq.size());
    uint32_t sent = inflight;   // bytes of sndq already sent at least once since snd_una
    const uint32_t limit = std::min<uint32_t>({s.peer_wnd, kVtInflight, std::max<uint32_t>(s.cwnd, static_cast<uint32_t>(kVtSeg))});
    while (sent < queued && sent < limit) {
        const uint32_t n = std::min<uint32_t>({static_cast<uint32_t>(kVtSeg), queued - sent, limit - sent});
        std::vector<uint8_t> chunk(s.sndq.begin() + sent, s.sndq.begin() + sent + n);
        VtHdr h{kVtData, s.local.port, s.peer.port, s.conn, s.snd_nxt, s.rcv_nxt, vt_window(s)};
        out->push_back(vt_make(s.local.ip, s.peer.ip, h, chunk.data(), n));
        s.snd_nxt += n;
        sent += n;
        if (seq_lt(s.snd_max, s.snd_nxt)) s.snd_max = s.snd_nxt;
        if (!s.rto_at) s.rto_at = now + s.rto_ms;
    }
    if (sent == queued && s.fin_queued && !s.fin_sent) {
        s.fin_seq = s.snd_nxt;
        s.fin_sent = true;
        VtHdr h{kVtFin, s.local.port, s.peer.port, s.conn, s.fin_seq, s.rcv_nxt, vt_window(s)};
        out->push_back(vt_make(s.local.ip, s.peer.ip, h));
        if (!s.rto_at) s.rto_at = now + s.rto_ms;
    }
}

Ret Stack::remote_connect(std::unique_lock<std::mutex>& lk, const std::shared_ptr<Sock>& s, Addr a, uint32_t me,
                          std::shared_ptr<vnet::Link> l) {
    if (s->listening) return fail(kEinval);
    if (s->remote) return s->established ? fail(kEisconn) : s->vt_reset ? fail(kEconnrefused) : fail(kEalready);
    if (s->connected) return fail(kEisconn);
    if (!l || me == 0) return fail(kEnetunreach);
    if (s->local.ip != 0 && s->local.ip != me) return fail(kEaddrnotavail);
    s->local.ip = me;
    if (const uint32_t e = autobind(*s)) return fail(e);
    s->remote = true;
    s->connected = true;   // connecting: getpeername answers the peer, as BSD does once connect started
    s->peer = a;
    s->conn = vt_conn_id();
    s->initiator = true;
    s->rto_ms = kVtRtoMin;
    const int64_t now = now_ms();
    s->rto_at = now + s->rto_ms;
    conns_[{a.ip, s->conn}] = s;
    ensure_timer_locked();
    std::vector<vnet::Datagram> out{vt_make(me, a.ip, VtHdr{kVtSyn, s->local.port, a.port, s->conn, 0, 0, vt_window(*s)})};
    NXLOG_D("net", "vtcp connect %s:%u -> %s:%u (conn %08x)", vnet::ip_str(me).c_str(), s->local.port, vnet::ip_str(a.ip).c_str(), a.port, s->conn);
    lk.unlock();
    transmit(out);
    lk.lock();
    if (s->nonblock) return s->established ? ok() : fail(kEinprogress);
    cv_.wait(lk, [&] { return s->established || s->vt_reset || s->closed; });
    if (s->closed) return fail(kEbadf);
    if (s->vt_reset) {
        const uint32_t e = s->so_error ? s->so_error : kEconnrefused;
        s->so_error = 0;
        s->connected = false;
        return fail(e);
    }
    return ok();
}

// Bytes into the send queue (256 KiB, as the loopback's buffer), as many as fit; a full queue blocks the writer until the peer
// acknowledges some. What the window allows goes out at once.
Ret Stack::remote_write(const std::shared_ptr<Sock>& s, const void* p, size_t n, int flags) {
    std::unique_lock<std::mutex> lk(mu_);
    const bool nb = s->nonblock || (flags & kMsgDontwait);
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t done = 0;
    std::vector<vnet::Datagram> out;
    for (;;) {
        if (s->closed) return fail(kEbadf);
        if (s->vt_reset) return done ? ok(static_cast<int32_t>(done)) : fail(s->established ? kEconnreset : kEnotconn);
        if (!s->established) {
            if (nb) return fail(kEagain);
            cv_.wait(lk);
            continue;
        }
        if (s->shut_wr || s->fin_queued) return done ? ok(static_cast<int32_t>(done)) : fail(kEpipe);
        const size_t room = s->sndq.size() < kStreamBuf ? kStreamBuf - s->sndq.size() : 0;
        const size_t m = std::min(room, n - done);
        if (m) {
            s->sndq.insert(s->sndq.end(), b + done, b + done + m);
            done += m;
            pump_locked(*s, now_ms(), &out);
        }
        if (!out.empty()) {
            lk.unlock();
            transmit(out);
            lk.lock();
        }
        if (done == n) return ok(static_cast<int32_t>(n));
        if (nb) return done ? ok(static_cast<int32_t>(done)) : fail(kEagain);
        cv_.wait(lk);   // the peer acknowledges bytes, or the connection goes away
    }
}

// Close of a vtcp socket's descriptor: FIN after the queued bytes (the connection stays in conns_ until it is acknowledged, at most
// kVtLingerMs); a connection nobody accepted, or one that is not up, ends with RST.
void Stack::remote_close_locked(const std::shared_ptr<Sock>& s, std::vector<vnet::Datagram>* out) {
    if (!s->remote) return;
    if (!s->established || s->vt_reset) {
        if (!s->vt_reset) out->push_back(vt_make(s->local.ip, s->peer.ip, VtHdr{kVtRst, s->local.port, s->peer.port, s->conn, s->snd_nxt, s->rcv_nxt, 0}));
        conns_.erase({s->peer.ip, s->conn});
        return;
    }
    s->fin_queued = true;
    s->linger_until = now_ms() + kVtLingerMs;
    pump_locked(*s, now_ms(), out);
}

void Stack::segment_in(const vnet::Datagram& d) {
    VtHdr h;
    if (!vt_parse(d, &h)) return;
    const uint8_t* payload = d.data.data() + kVtHdr;
    const size_t plen = d.data.size() - kVtHdr;
    std::vector<vnet::Datagram> out;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int64_t now = now_ms();
        auto it = conns_.find({d.src_ip, h.conn});
        std::shared_ptr<Sock> s = it == conns_.end() ? nullptr : it->second;
        auto rst = [&] {
            if (h.type != kVtRst)
                out.push_back(vt_make(d.dst_ip, d.src_ip, VtHdr{kVtRst, h.dport, h.sport, h.conn, h.ack, h.seq + static_cast<uint32_t>(plen), 0}));
        };
        if (h.type == kVtSyn) {
            if (s) {   // our SYNACK was lost: again
                if (!s->initiator)
                    out.push_back(vt_make(s->local.ip, s->peer.ip, VtHdr{kVtSynAck, s->local.port, s->peer.port, s->conn, 0, s->rcv_nxt, vt_window(*s)}));
            } else {
                std::shared_ptr<Sock> l;
                for (const auto& [fd, o] : socks_)
                    if (o->stream && o->listening && o->local.port == h.dport && (o->local.ip == 0 || o->local.ip == d.dst_ip)) {
                        l = o;
                        break;
                    }
                if (!l || l->pending.size() > static_cast<size_t>(3 * l->backlog / 2)) {
                    NXLOG_D("net", "vtcp connect from %s:%u to port %u refused: nobody listens", vnet::ip_str(d.src_ip).c_str(), h.sport, h.dport);
                    rst();
                } else {
                    auto c = std::make_shared<Sock>();
                    c->stream = true;
                    c->remote = true;
                    c->established = true;
                    c->accepted = true;
                    c->bound = true;
                    c->connected = true;
                    c->local = {d.dst_ip, h.dport};
                    c->peer = {d.src_ip, h.sport};
                    c->conn = h.conn;
                    c->peer_wnd = h.wnd;
                    c->nonblock = l->nonblock;
                    c->rto_ms = kVtRtoMin;
                    conns_[{d.src_ip, h.conn}] = c;
                    l->pending.push_back(c);
                    ensure_timer_locked();
                    out.push_back(vt_make(c->local.ip, c->peer.ip, VtHdr{kVtSynAck, c->local.port, c->peer.port, c->conn, 0, 0, vt_window(*c)}));
                    NXLOG_D("net", "vtcp accept %s:%u -> %s:%u (conn %08x)", vnet::ip_str(d.src_ip).c_str(), h.sport, vnet::ip_str(d.dst_ip).c_str(),
                            h.dport, h.conn);
                    cv_.notify_all();
                }
            }
        } else if (!s) {
            rst();   // a connection this end does not know (closed and gone, or never made)
        } else if (h.type == kVtRst) {
            s->vt_reset = true;
            s->so_error = s->established ? kEconnreset : kEconnrefused;
            s->rx_eof = true;
            conns_.erase({d.src_ip, h.conn});
            cv_.notify_all();
        } else if (h.type == kVtSynAck) {
            if (s->initiator && !s->established) {
                s->established = true;
                s->peer_wnd = h.wnd;
                s->rto_at = 0;
                s->rto_ms = kVtRtoMin;
                s->tries = 0;
                cv_.notify_all();
                pump_locked(*s, now, &out);
            }
        } else {
            // every other segment carries the peer's acknowledgment and window; the peer answers, so it is there: a shut window or a
            // slow reader is no reason to give the connection up
            s->tries = 0;
            if (s->established) {
                const uint32_t top = s->snd_max + (s->fin_sent ? 1u : 0u);
                if (seq_lt(s->snd_una, h.ack) && seq_le(h.ack, top)) {
                    const uint32_t data_ack = s->fin_sent && h.ack == s->fin_seq + 1 ? s->fin_seq : h.ack;
                    const uint32_t gone = std::min<uint32_t>(data_ack - s->snd_una, static_cast<uint32_t>(s->sndq.size()));
                    s->sndq.erase(s->sndq.begin(), s->sndq.begin() + gone);
                    s->snd_una = data_ack;
                    if (seq_lt(s->snd_nxt, s->snd_una)) s->snd_nxt = s->snd_una;
                    if (s->fin_sent && h.ack == s->fin_seq + 1) s->fin_acked = true;
                    s->tries = 0;
                    s->rto_ms = kVtRtoMin;
                    s->cwnd = std::min<uint32_t>(s->cwnd * 2, kVtInflight);
                    const bool waiting = s->snd_una != s->snd_nxt || (s->fin_sent && !s->fin_acked);
                    s->rto_at = waiting ? now + s->rto_ms : 0;
                    cv_.notify_all();
                }
                s->peer_wnd = h.wnd;
            }
            bool answer = false;
            if (h.type == kVtData && plen) {
                answer = true;
                if (s->closed) {   // nobody reads any more: the peer learns it
                    rst();
                    s->vt_reset = true;
                    conns_.erase({d.src_ip, h.conn});
                    answer = false;
                } else if (seq_le(h.seq, s->rcv_nxt) && seq_lt(s->rcv_nxt, h.seq + static_cast<uint32_t>(plen))) {
                    // the next bytes (a resent segment may start before them: segments are cut again from the first byte not
                    // acknowledged), then whatever came early and now follows on
                    const size_t off = s->rcv_nxt - h.seq;
                    const size_t take = std::min<size_t>(plen - off, vt_window(*s));
                    s->rx.insert(s->rx.end(), payload + off, payload + off + take);
                    s->rcv_nxt += static_cast<uint32_t>(take);
                    for (bool more = true; more;) {
                        more = false;
                        for (auto o = s->ooo.begin(); o != s->ooo.end(); ++o) {
                            if (!seq_le(o->first, s->rcv_nxt)) continue;
                            const uint32_t end = o->first + static_cast<uint32_t>(o->second.size());
                            if (seq_lt(s->rcv_nxt, end)) {
                                const size_t k = s->rcv_nxt - o->first;
                                s->rx.insert(s->rx.end(), o->second.begin() + static_cast<std::ptrdiff_t>(k), o->second.end());
                                s->rcv_nxt = end;
                            }
                            s->ooo_bytes -= o->second.size();
                            s->ooo.erase(o);
                            more = true;
                            break;
                        }
                    }
                    cv_.notify_all();
                } else if (seq_lt(s->rcv_nxt, h.seq) && !s->ooo.count(h.seq) && s->ooo_bytes + plen <= kVtInflight) {
                    // early segments wait apart from the window, at most what can be in flight: resent segments are cut again from the
                    // first byte not acknowledged, so overlapping copies pile up here, and counting them against the window could shut it
                    // on the very bytes that would drain them
                    s->ooo[h.seq].assign(payload, payload + plen);   // early: kept until the bytes before it come
                    s->ooo_bytes += plen;
                }
            } else if (h.type == kVtFin) {
                answer = true;
                s->peer_fin = true;
                s->peer_fin_seq = h.seq;
            } else if (h.type == kVtWinQ) {
                answer = true;
            }
            if (s->peer_fin && !s->rx_eof && s->rcv_nxt == s->peer_fin_seq) {
                s->rx_eof = true;   // every byte before the FIN is in: the reader gets its end of data
                s->rcv_nxt += 1;
                cv_.notify_all();
            }
            if (answer && !s->vt_reset)
                out.push_back(vt_make(s->local.ip, s->peer.ip, VtHdr{kVtAck, s->local.port, s->peer.port, s->conn, s->snd_nxt, s->rcv_nxt, vt_window(*s)}));
            if (s->closed && s->fin_acked && s->rx_eof) conns_.erase({s->peer.ip, s->conn});   // both directions done
            else pump_locked(*s, now, &out);
        }
        // The answers go out from the timer thread, never from here: a link that delivers on the sending thread (LoopbackHub) holds its
        // receiver lock across this call, and an answer delivered back into this table's link would need that lock again.
        if (!out.empty()) {
            for (vnet::Datagram& x : out) outq_.push_back(std::move(x));
            ensure_timer_locked();
            cv_.notify_all();
        }
    }
}

// Every kVtTickMs: SYN, data and FIN not acknowledged within their timeout go again (from the first byte not acknowledged), a peer
// whose window stays shut is asked again (WINQ), and connections that gave up or lingered long enough are dropped.
void Stack::tick() {
    std::unique_lock<std::mutex> lk(mu_);
    int64_t next_tick = now_ms() + kVtTickMs;
    while (!timer_stop_) {
        if (outq_.empty()) cv_.wait_for(lk, std::chrono::milliseconds(kVtTickMs));
        if (timer_stop_) break;
        if (!outq_.empty()) {   // answers segment_in queued
            std::vector<vnet::Datagram> q(std::make_move_iterator(outq_.begin()), std::make_move_iterator(outq_.end()));
            outq_.clear();
            lk.unlock();
            transmit(q);
            lk.lock();
        }
        const int64_t now = now_ms();
        if (now < next_tick) continue;
        next_tick = now + kVtTickMs;
        std::vector<vnet::Datagram> out;
        for (auto it = conns_.begin(); it != conns_.end();) {
            Sock& s = *it->second;
            const bool linger_over = s.closed && s.linger_until && now >= s.linger_until;
            if (s.vt_reset || linger_over || (s.closed && s.fin_acked && s.rx_eof)) {
                it = conns_.erase(it);
                continue;
            }
            if (s.rto_at && now >= s.rto_at) {
                const bool syn = !s.established;
                if (++s.tries > (syn ? kVtSynTries : kVtDataTries)) {
                    NXLOG_I("net", "vtcp %s:%u -> %s:%u: no answer, connection dropped (ETIMEDOUT)", vnet::ip_str(s.local.ip).c_str(), s.local.port,
                            vnet::ip_str(s.peer.ip).c_str(), s.peer.port);
                    s.vt_reset = true;
                    s.so_error = kEtimedout;
                    s.rx_eof = true;
                    cv_.notify_all();
                    it = conns_.erase(it);
                    continue;
                }
                s.rto_ms = std::min(s.rto_ms * 2, kVtRtoMax);
                s.rto_at = now + s.rto_ms;
                if (syn) {
                    out.push_back(vt_make(s.local.ip, s.peer.ip, VtHdr{kVtSyn, s.local.port, s.peer.port, s.conn, 0, 0, vt_window(s)}));
                } else {
                    s.snd_nxt = s.snd_una;   // go back to the first byte not acknowledged
                    // and resend from there one segment at a time, growing again as acknowledgments come (slow start): a whole window resent
                    // at once on every timeout can meet the same loss each time (a link that drops every Nth packet saw a livelock)
                    s.cwnd = static_cast<uint32_t>(kVtSeg);
                    s.fin_sent = s.fin_sent && s.fin_acked;
                    s.rto_at = 0;
                    pump_locked(s, now, &out);
                    if (!s.rto_at) s.rto_at = now + s.rto_ms;
                }
            } else if (s.established && !s.vt_reset && s.peer_wnd == 0 && s.snd_una == s.snd_nxt && !s.sndq.empty() &&
                       (!s.winq_at || now >= s.winq_at)) {
                s.winq_at = now + 500;
                out.push_back(vt_make(s.local.ip, s.peer.ip, VtHdr{kVtWinQ, s.local.port, s.peer.port, s.conn, s.snd_nxt, s.rcv_nxt, vt_window(s)}));
            }
            ++it;
        }
        if (!out.empty()) {
            lk.unlock();
            transmit(out);
            lk.lock();
        }
    }
}

// The receive window this end advertises: what its buffer still takes (bytes waiting out of order count as taken).
uint32_t Stack::vt_window(const Sock& s) {
    const size_t used = s.rx.size();   // what the reader has not taken yet (early segments wait apart, kVtInflight at most)
    return used < kStreamBuf ? static_cast<uint32_t>(kStreamBuf - used) : 0;
}

vnet::Datagram Stack::vt_ack(const Sock& s) {
    return vt_make(s.local.ip, s.peer.ip, VtHdr{kVtAck, s.local.port, s.peer.port, s.conn, s.snd_nxt, s.rcv_nxt, vt_window(s)});
}

Stack::Stack(std::shared_ptr<vnet::Link> fixed) : fixed_(std::move(fixed)) {
    if (fixed_) fixed_->set_receiver([this](const vnet::Datagram& d) { inbound(d); });
}

Stack::~Stack() {
    if (fixed_) fixed_->set_receiver(nullptr);
    {
        std::lock_guard<std::mutex> lk(mu_);
        timer_stop_ = true;
        cv_.notify_all();
    }
    if (timer_.joinable()) timer_.join();
    reset();
}

std::shared_ptr<vnet::Link> Stack::link() const { return fixed_ ? fixed_ : vnet::current_link(); }

std::shared_ptr<Stack::Sock> Stack::get(int fd) {
    auto it = socks_.find(fd);
    return it == socks_.end() ? nullptr : it->second;
}

bool Stack::is_socket(int fd) {
    std::lock_guard<std::mutex> lk(mu_);
    return get(fd) != nullptr;
}

Ret Stack::socket(int domain, int type, int protocol) {
    const int base = type & 0x0FFFFFFF;
    // AF_INET6: the address family is not supported (Lichii bsd.cpp Socket answers EAFNOSUPPORT for it), so dual-stack code falls back
    // to IPv4 at once; the rest: the network stays down
    if (domain == kAfInet6) return fail(kEafnosupport);
    if (domain != kAfInet || (base != kSockDgram && base != kSockStream)) return fail(kEnetdown);
    const bool stream = base == kSockStream;
    if (protocol != 0 && protocol != (stream ? kIpprotoTcp : kIpprotoUdp)) return fail(kEprotonosupport);
    std::lock_guard<std::mutex> lk(mu_);
    int fd = 0;
    while (fd < kMaxSockets && socks_.count(fd)) ++fd;
    if (fd == kMaxSockets) return fail(kEmfile);
    auto s = std::make_shared<Sock>();
    s->nonblock = (type & kSockNonblock) != 0;
    s->stream = stream;
    socks_[fd] = s;
    return ok(fd);
}

bool Stack::port_free(uint16_t port, uint32_t ip, const Sock& s) const {
    for (const auto& [fd, o] : socks_) {
        if (o.get() == &s || !o->bound || o->stream != s.stream || o->accepted || o->local.port != port) continue;   // TCP and UDP ports apart
        if (o->local.ip != 0 && ip != 0 && o->local.ip != ip) continue;     // different addresses do not overlap
        if ((s.reuse_addr && o->reuse_addr) || (s.reuse_port && o->reuse_port)) continue;
        return false;
    }
    return true;
}

uint32_t Stack::autobind(Sock& s) {
    if (s.bound) return 0;
    for (int i = 0; i < 65536 - kEphemeralFirst; ++i) {
        const uint16_t p = next_port_;
        next_port_ = next_port_ == 65535 ? kEphemeralFirst : static_cast<uint16_t>(next_port_ + 1);
        if (port_free(p, s.local.ip, s)) {
            s.local.port = p;
            s.bound = true;
            return 0;
        }
    }
    return kEaddrinuse;
}

Ret Stack::bind(int fd, Addr a) {
    const auto l = link();
    const uint32_t me = l ? l->local_ip() : 0;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto s = get(fd);
        if (!s) return fail(kEbadf);
        if (s->bound) return fail(kEinval);
        if (a.ip != 0 && !(s->stream && is_loopback(a.ip)) && (me == 0 || a.ip != me)) return fail(kEaddrnotavail);
        s->local.ip = a.ip;
        if (a.port == 0) {
            if (autobind(*s)) return fail(kEaddrinuse);
        } else {
            if (!port_free(a.port, a.ip, *s)) return fail(kEaddrinuse);
            s->local.port = a.port;
            s->bound = true;
        }
        if (!s->stream) port = s->local.port;
    }
    if (l && port) l->open_port(port);   // a real LAN link opens its host socket for the port (lan_mitm.h); outside mu_
    return ok();
}

Ret Stack::connect(int fd, Addr a) {
    const auto l = link();
    const uint32_t me = l ? l->local_ip() : 0;
    std::unique_lock<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    // another node of the link's network (not this node, not a broadcast, and the loopback only when the network itself is on it: a test
    // LAN on 127.0.0.x; elsewhere 127.0.0.0/8 is this console's own, libuv's wake-up pairs)
    if (s->stream && (s->remote || (reaches(l, a.ip) && a.ip != me && !is_bcast(a.ip, mask_of(l)) && (!is_loopback(a.ip) || is_loopback(me)))))
        return remote_connect(lk, s, a, me, l);   // another node of the local wireless network: TCP between the nodes
    if (s->stream) return stream_connect(s, a, me);
    if (!reaches(l, a.ip)) return fail(kEnetdown);
    if (me == 0) return fail(kEnetunreach);
    if (is_bcast(a.ip, mask_of(l)) && !s->broadcast) return fail(kEacces);
    if (const uint32_t e = autobind(*s)) return fail(e);
    s->connected = true;
    s->peer = a;
    return ok();
}

// A loopback connection is made at once (the console's lo0 completes the handshake in its stack): the client's end is connected, and the
// server's end waits in the listener's queue for accept. Only this console answers while offline.
Ret Stack::stream_connect(const std::shared_ptr<Sock>& s, Addr a, uint32_t me) {
    if (s->listening) return fail(kEinval);
    if (s->connected) return fail(kEisconn);
    if (!is_loopback(a.ip) && (me == 0 || a.ip != me)) return fail(kEnetdown);
    std::shared_ptr<Sock> l;
    for (const auto& [fd, o] : socks_)
        if (o->stream && o->listening && o->local.port == a.port && (o->local.ip == 0 || o->local.ip == a.ip)) {
            l = o;
            break;
        }
    if (!l || l->pending.size() > static_cast<size_t>(3 * l->backlog / 2)) return fail(kEconnrefused);   // FreeBSD's queue limit
    if (s->local.ip == 0) s->local.ip = a.ip;   // the source of a loopback connection is the address it goes to
    if (const uint32_t e = autobind(*s)) return fail(e);
    auto c = std::make_shared<Sock>();
    c->stream = true;
    c->accepted = true;
    c->bound = true;
    c->local = a;
    c->connected = true;
    c->peer = s->local;
    c->other = s;
    c->nonblock = l->nonblock;   // FreeBSD: an accepted socket keeps the listener's O_NONBLOCK
    s->connected = true;
    s->peer = a;
    s->other = c;
    l->pending.push_back(c);
    cv_.notify_all();
    NXLOG_D("net", "tcp loopback connect %s:%u -> %s:%u", vnet::ip_str(s->local.ip).c_str(), s->local.port, vnet::ip_str(a.ip).c_str(), a.port);
    return ok();
}

Ret Stack::listen(int fd, int backlog) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (!s->stream) return fail(kEopnotsupp);
    if (s->connected) return fail(kEinval);
    if (const uint32_t e = autobind(*s)) return fail(e);
    s->listening = true;
    s->backlog = backlog < 0 || backlog > kMaxBacklog ? kMaxBacklog : backlog;
    return ok();
}

Ret Stack::accept(int fd, Addr* from) {
    std::unique_lock<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (!s->stream || !s->listening) return fail(kEinval);
    auto ready = [&] { return !s->pending.empty() || s->closed; };
    if (!ready()) {
        if (s->nonblock) return fail(kEagain);
        if (s->rcvtimeo_us > 0) {
            if (!cv_.wait_for(lk, std::chrono::microseconds(s->rcvtimeo_us), ready)) return fail(kEagain);
        } else {
            cv_.wait(lk, ready);
        }
    }
    if (s->closed) return fail(kEbadf);
    int nfd = 0;
    while (nfd < kMaxSockets && socks_.count(nfd)) ++nfd;
    if (nfd == kMaxSockets) return fail(kEmfile);
    auto c = s->pending.front();
    s->pending.pop_front();
    socks_[nfd] = c;
    if (from) *from = c->peer;
    return ok(nfd);
}

// The bytes go into the other end's buffer, as many as it has room for; a full buffer blocks the writer until the reader takes some.
Ret Stack::stream_write(const std::shared_ptr<Sock>& s, const void* p, size_t n, int flags) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!s->connected) return fail(kEnotconn);
    const bool nb = s->nonblock || (flags & kMsgDontwait);
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t done = 0;
    for (;;) {
        if (s->closed) return fail(kEbadf);
        auto o = s->other.lock();
        if (s->shut_wr || !o || o->closed) return done ? ok(static_cast<int32_t>(done)) : fail(kEpipe);
        const size_t room = o->rx.size() < kStreamBuf ? kStreamBuf - o->rx.size() : 0;
        const size_t m = std::min(room, n - done);
        if (m) {
            o->rx.insert(o->rx.end(), b + done, b + done + m);
            done += m;
            cv_.notify_all();
        }
        if (done == n) return ok(static_cast<int32_t>(n));
        if (nb) return done ? ok(static_cast<int32_t>(done)) : fail(kEagain);
        cv_.wait(lk);   // the reader takes bytes, or an end goes away
    }
}

void Stack::stream_hang_up(Sock& s) {
    if (auto o = s.other.lock()) o->rx_eof = true;
    cv_.notify_all();
}

Ret Stack::sendto(int fd, const void* p, size_t n, int flags, const Addr* to) {
    (void)flags;   // MSG_DONTWAIT: a send never blocks here
    const auto l = link();
    const uint32_t me = l ? l->local_ip() : 0;   // asked before taking mu_: a link may lock inside
    vnet::Datagram d;
    {
        std::unique_lock<std::mutex> lk(mu_);
        auto s = get(fd);
        if (!s) return fail(kEbadf);
        if (s->event) {
            lk.unlock();
            return event_write(s, p, n, flags);
        }
        if (s->stream) {   // a destination given for a connected stream is ignored, as BSD does
            lk.unlock();
            return s->remote ? remote_write(s, p, n, flags) : stream_write(s, p, n, flags);
        }
        if (s->shut_wr) return fail(kEpipe);
        Addr dst;
        if (to) dst = *to;
        else if (s->connected) dst = s->peer;
        else return fail(kEdestaddrreq);
        if (n > vnet::kMaxDatagram) return fail(kEmsgsize);
        const uint32_t mask = mask_of(l);
        if (dst.ip == kLimitedBroadcast) {   // 255.255.255.255 leaves as the subnet broadcast
            if (me == 0) return fail(kEnetunreach);
            dst.ip = me | ~mask;
        }
        if (!reaches(l, dst.ip)) return fail(kEnetdown);
        if (me == 0) return fail(kEnetunreach);
        if (dst.port == 0) return fail(kEinval);
        if (is_bcast(dst.ip, mask) && !s->broadcast) return fail(kEacces);
        if (const uint32_t e = autobind(*s)) return fail(e);
        d.src_ip = me;
        d.src_port = s->local.port;
        d.dst_ip = dst.ip;
        d.dst_port = dst.port;
        d.data.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    }
    NXLOG_D("net", "udp fd %d send %s:%u -> %s:%u, %zu bytes: %s", fd, vnet::ip_str(d.src_ip).c_str(), d.src_port,
            vnet::ip_str(d.dst_ip).c_str(), d.dst_port, n, hex_head(d.data).c_str());
    if (d.dst_ip == d.src_ip) inbound(d);          // to our own address: the loopback path, not the wire
    else if (!l->send(d)) return fail(kEnetunreach);
    return ok(static_cast<int32_t>(n));
}

void Stack::inbound(const vnet::Datagram& d) {
    if (d.src_port == 0 && d.dst_port == 0) {   // ports 0 and 0: a vtcp segment, never a UDP socket's datagram
        segment_in(d);
        return;
    }
    const bool bcast = vnet::is_subnet_broadcast(d.dst_ip);
    std::lock_guard<std::mutex> lk(mu_);
    std::shared_ptr<Sock> wild;   // unicast: an exact-address socket wins over a 0.0.0.0 one
    int wild_fd = -1;
    bool any = false;
    auto put = [&](int fd, Sock& s) {
        if (s.q.size() >= kQueueCap) s.q.pop_front();
        s.q.push_back(d);
        any = true;
        NXLOG_D("net", "udp fd %d recv %s:%u -> %s:%u, %zu bytes: %s", fd, vnet::ip_str(d.src_ip).c_str(), d.src_port,
                vnet::ip_str(d.dst_ip).c_str(), d.dst_port, d.data.size(), hex_head(d.data).c_str());
    };
    for (const auto& [fd, s] : socks_) {
        if (s->stream || !s->bound || s->shut_rd || s->local.port != d.dst_port) continue;
        if (s->connected && (s->peer.ip != d.src_ip || s->peer.port != d.src_port)) continue;
        if (bcast) {
            if (s->local.ip == 0 || s->local.ip == d.dst_ip) put(fd, *s);
        } else if (s->local.ip == d.dst_ip) {
            put(fd, *s);
            wild = nullptr;
            break;
        } else if (s->local.ip == 0 && !wild) {
            wild = s;
            wild_fd = fd;
        }
    }
    if (wild) put(wild_fd, *wild);
    if (any) cv_.notify_all();
    else NXLOG_D("net", "udp drop %s:%u -> %s:%u: no socket on the port", vnet::ip_str(d.src_ip).c_str(), d.src_port,
                 vnet::ip_str(d.dst_ip).c_str(), d.dst_port);
}

Ret Stack::recvfrom(int fd, void* p, size_t n, int flags, Addr* from) {
    std::shared_ptr<Sock> s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        s = get(fd);
        if (!s) return fail(kEbadf);
    }
    return wait_recv(s, p, n, flags, from);
}

// An event descriptor's write: adds the 8-byte value to the counter; a sum past the maximum waits (EAGAIN when non-blocking).
Ret Stack::event_write(const std::shared_ptr<Sock>& s, const void* p, size_t n, int flags) {
    if (n < 8) return fail(kEinval);
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    if (v == ~0ull) return fail(kEinval);
    std::unique_lock<std::mutex> lk(mu_);
    auto fits = [&] { return s->closed || v <= kEventMax - s->counter; };
    if (!fits()) {
        if (s->nonblock || (flags & kMsgDontwait)) return fail(kEagain);
        cv_.wait(lk, fits);
    }
    if (s->closed) return fail(kEbadf);
    s->counter += v;
    cv_.notify_all();
    return ok(8);
}

Ret Stack::wait_recv(const std::shared_ptr<Sock>& s, void* p, size_t n, int flags, Addr* from) {
    std::unique_lock<std::mutex> lk(mu_);
    const bool nb = s->nonblock || (flags & kMsgDontwait);
    if (s->event) {   // an event descriptor's read: the counter (1 for a semaphore), which it then lowers; zero waits
        if (n < 8) return fail(kEinval);
        auto set = [&] { return s->closed || s->counter != 0; };
        if (!set()) {
            if (nb) return fail(kEagain);
            cv_.wait(lk, set);
        }
        if (s->closed) return fail(kEbadf);
        const uint64_t v = s->semaphore ? 1 : s->counter;
        s->counter -= v;
        std::memcpy(p, &v, 8);
        if (from) *from = {};
        cv_.notify_all();
        return ok(8);
    }
    if (s->stream) {   // the bytes the other end sent; with none, 0 at the end of data, else wait
        if (!s->connected) return fail(kEnotconn);
        auto has = [&] { return !s->rx.empty() || s->rx_eof || s->closed || s->shut_rd || s->vt_reset; };
        if (!has()) {
            if (nb) return fail(kEagain);
            if (s->rcvtimeo_us > 0) {
                if (!cv_.wait_for(lk, std::chrono::microseconds(s->rcvtimeo_us), has)) return fail(kEagain);
            } else {
                cv_.wait(lk, has);
            }
        }
        if (s->closed) return fail(kEbadf);
        if (s->remote && s->rx.empty() && s->vt_reset && s->so_error) {   // RST or given up: the error once, then end of data
            const uint32_t e = s->so_error;
            s->so_error = 0;
            return fail(e);
        }
        const uint32_t wnd_before = s->remote ? vt_window(*s) : 0;
        const size_t m = std::min(n, s->rx.size());
        if (m) {
            std::copy(s->rx.begin(), s->rx.begin() + static_cast<std::ptrdiff_t>(m), static_cast<uint8_t*>(p));
            if (!(flags & kMsgPeek)) {
                s->rx.erase(s->rx.begin(), s->rx.begin() + static_cast<std::ptrdiff_t>(m));
                cv_.notify_all();   // a writer waiting for room
            }
        }
        if (from) *from = s->peer;
        // vtcp: a window that was nearly shut and is open again is told to the peer at once (else it waits for its WINQ)
        if (s->remote && s->established && !s->vt_reset && wnd_before < kVtSeg && vt_window(*s) >= kStreamBuf / 4) {
            std::vector<vnet::Datagram> out{vt_ack(*s)};
            lk.unlock();
            transmit(out);
        }
        return ok(static_cast<int32_t>(m));
    }
    auto ready = [&] { return !s->q.empty() || s->closed || s->shut_rd; };
    if (!ready()) {
        if (nb) return fail(kEagain);
        if (s->rcvtimeo_us > 0) {
            if (!cv_.wait_for(lk, std::chrono::microseconds(s->rcvtimeo_us), ready)) return fail(kEagain);
        } else {
            cv_.wait(lk, ready);
        }
    }
    if (s->closed) return fail(kEbadf);
    if (s->q.empty()) return ok(0);   // shut down for reading: end of data
    const vnet::Datagram& d = s->q.front();
    const size_t m = d.data.size() < n ? d.data.size() : n;   // the rest of a longer datagram is lost, as on BSD
    if (m) std::memcpy(p, d.data.data(), m);
    if (from) *from = {d.src_ip, d.src_port};
    if (!(flags & kMsgPeek)) s->q.pop_front();
    return ok(static_cast<int32_t>(m));
}

Ret Stack::setsockopt(int fd, int level, int opt, const void* v, size_t n) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    int32_t iv = 0;
    if (v && n >= 4) std::memcpy(&iv, v, 4);
    if (level == kSolSocket) {
        switch (opt) {
            case kSoBroadcast: s->broadcast = iv != 0; return ok();
            case kSoReuseAddr: s->reuse_addr = iv != 0; return ok();
            case kSoReusePort: s->reuse_port = iv != 0; return ok();
            case kSoRcvBuf: s->rcvbuf = iv; return ok();
            case kSoSndBuf: s->sndbuf = iv; return ok();
            case kSoRcvTimeo: {   // struct timeval {i64 tv_sec, i64 tv_usec}
                int64_t tv[2] = {0, 0};
                if (v && n >= sizeof tv) std::memcpy(tv, v, sizeof tv);
                s->rcvtimeo_us = tv[0] * 1000000 + tv[1];
                return ok();
            }
            case kSoSndTimeo:   // sends never block: kept for getsockopt only
                std::memset(s->sndtimeo, 0, sizeof s->sndtimeo);
                if (v) std::memcpy(s->sndtimeo, v, n < sizeof s->sndtimeo ? n : sizeof s->sndtimeo);
                return ok();
            default: break;
        }
    }
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(level)) << 32) | static_cast<uint32_t>(opt);
    const uint8_t* b = static_cast<const uint8_t*>(v);
    s->other_opts[key].assign(b, b + (v ? n : 0));
    if (logged_opts_.insert(key).second)
        NXLOG_I("net", "bsd setsockopt level 0x%x opt 0x%x (%zu bytes): accepted without effect", level, opt, n);
    return ok();
}

Ret Stack::getsockopt(int fd, int level, int opt, void* v, size_t* n) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    auto put_int = [&](int32_t x) {
        if (*n >= 4) {
            std::memcpy(v, &x, 4);
            *n = 4;
        }
        return *n >= 4 ? ok() : fail(kEinval);
    };
    if (level == kSolSocket) {
        switch (opt) {
            case kSoBroadcast: return put_int(s->broadcast ? opt : 0);   // FreeBSD answers the option bit, not 1
            case kSoReuseAddr: return put_int(s->reuse_addr ? opt : 0);
            case kSoReusePort: return put_int(s->reuse_port ? opt : 0);
            case kSoRcvBuf: return put_int(s->rcvbuf);
            case kSoSndBuf: return put_int(s->sndbuf);
            case kSoType: return put_int(s->stream ? kSockStream : kSockDgram);
            case kSoError: {   // a vtcp connect or connection that failed answers why, once
                const int32_t e = static_cast<int32_t>(s->so_error);
                s->so_error = 0;
                return put_int(e);
            }
            case kSoRcvTimeo: {
                const int64_t tv[2] = {s->rcvtimeo_us / 1000000, s->rcvtimeo_us % 1000000};
                if (*n < sizeof tv) return fail(kEinval);
                std::memcpy(v, tv, sizeof tv);
                *n = sizeof tv;
                return ok();
            }
            case kSoSndTimeo:
                if (*n < sizeof s->sndtimeo) return fail(kEinval);
                std::memcpy(v, s->sndtimeo, sizeof s->sndtimeo);
                *n = sizeof s->sndtimeo;
                return ok();
            default: break;
        }
    }
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(level)) << 32) | static_cast<uint32_t>(opt);
    auto it = s->other_opts.find(key);
    if (it == s->other_opts.end()) return put_int(0);
    const size_t m = it->second.size() < *n ? it->second.size() : *n;
    if (m) std::memcpy(v, it->second.data(), m);
    *n = m;
    return ok();
}

Ret Stack::fcntl(int fd, int cmd, int arg) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (cmd == kFGetFl) return ok(s->nonblock ? kONonblock : 0);
    if (cmd == kFSetFl) {
        s->nonblock = (arg & kONonblock) != 0;
        return ok();
    }
    return fail(kEinval);
}

Ret Stack::getsockname(int fd, Addr* a) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    *a = s->local;   // 0.0.0.0:0 before bind, as on BSD
    return ok();
}

Ret Stack::getpeername(int fd, Addr* a) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (!s->connected) return fail(kEnotconn);
    *a = s->peer;
    return ok();
}

Ret Stack::shutdown(int fd, int how) {
    std::unique_lock<std::mutex> lk(mu_);
    auto s = get(fd);
    if (!s) return fail(kEbadf);
    if (how < kShutRd || how > kShutRdWr) return fail(kEinval);
    if (!s->connected) return fail(kEnotconn);   // BSD and Linux: an unconnected socket has nothing to shut down
    if (how != kShutWr) s->shut_rd = true;
    std::vector<vnet::Datagram> out;
    if (how != kShutRd) {
        s->shut_wr = true;
        if (s->remote) {   // FIN after the queued bytes
            if (s->established && !s->vt_reset) {
                s->fin_queued = true;
                pump_locked(*s, now_ms(), &out);
            }
        } else if (s->stream) {
            stream_hang_up(*s);   // the other end reads its end of data
        }
    }
    cv_.notify_all();
    if (!out.empty()) {
        lk.unlock();
        transmit(out);
    }
    return ok();
}

Ret Stack::close(int fd) {
    std::unique_lock<std::mutex> lk(mu_);
    auto it = socks_.find(fd);
    if (it == socks_.end()) return fail(kEbadf);
    const std::shared_ptr<Sock> sp = it->second;
    Sock& s = *sp;
    s.closed = true;
    std::vector<vnet::Datagram> out;
    if (s.stream) {
        if (s.remote) remote_close_locked(sp, &out);
        else stream_hang_up(s);
        for (const auto& c : s.pending) {   // connections nobody accepted: their clients read the end of data (a remote one: RST)
            c->closed = true;
            if (c->remote) {
                c->established = false;
                remote_close_locked(c, &out);
            } else {
                stream_hang_up(*c);
            }
        }
        s.pending.clear();
    }
    socks_.erase(it);
    cv_.notify_all();
    if (!out.empty()) {
        lk.unlock();
        transmit(out);
    }
    return ok();
}

Ret Stack::eventfd(uint64_t initval, uint32_t flags) {
    if (initval > kEventMax) return fail(kEinval);
    std::lock_guard<std::mutex> lk(mu_);
    int fd = 0;
    while (fd < kMaxSockets && socks_.count(fd)) ++fd;
    if (fd == kMaxSockets) return fail(kEmfile);
    auto s = std::make_shared<Sock>();
    s->event = true;
    s->counter = initval;
    s->semaphore = (flags & kEfdSemaphore) != 0;
    s->nonblock = (flags & kEfdNonblock) != 0;
    socks_[fd] = s;
    return ok(fd);
}

int16_t Stack::ready_locked(int fd, int16_t events) {
    auto s = get(fd);
    // A descriptor the table never made has nothing to say (nxnative services.md, "Event descriptors in an fd_set"): games pass stale or
    // never-valid numbers beside their real ones (Star Fox's HTTP thread), and an immediate POLLNVAL would spin or end their loop.
    if (!s) return 0;
    int16_t r = 0;
    if (s->event) {
        if (s->counter != 0) r |= kPollIn;
        if (s->counter < kEventMax) r |= kPollOut;
    } else if (s->stream) {
        if (s->listening) {
            if (!s->pending.empty()) r |= kPollIn;   // a connection to accept
        } else if (s->remote) {
            if (s->vt_reset) r |= kPollIn | kPollOut | kPollErr;   // a refused or reset connection: readable (the error), writable (SO_ERROR)
            if (!s->rx.empty() || s->rx_eof || s->shut_rd) r |= kPollIn;
            if (s->established && !s->vt_reset && !s->shut_wr && !s->fin_queued && s->sndq.size() < kStreamBuf) r |= kPollOut;
            if (s->rx_eof && (s->shut_wr || s->fin_queued || s->vt_reset)) r |= kPollHup;
        } else if (s->connected) {
            const auto o = s->other.lock();
            const bool gone = !o || o->closed;
            if (!s->rx.empty() || s->rx_eof || s->shut_rd) r |= kPollIn;
            if (!s->shut_wr && !gone && o->rx.size() < kStreamBuf) r |= kPollOut;
            if (s->rx_eof && (gone || s->shut_wr)) r |= kPollHup;
        }
    } else {
        if (!s->q.empty() || s->shut_rd) r |= kPollIn;
        if (!s->shut_wr) r |= kPollOut;
        if (s->shut_rd && s->shut_wr) r |= kPollHup;
    }
    return static_cast<int16_t>(r & (events | kPollHup | kPollErr));   // HUP and ERR are reported whether asked for or not
}

namespace {
// Waits on cv until ready() holds or the timeout (ms; < 0: none) runs out.
template <class F>
void wait_ready(std::condition_variable& cv, std::unique_lock<std::mutex>& lk, int timeout_ms, F ready) {
    if (ready() || timeout_ms == 0) return;
    if (timeout_ms < 0) cv.wait(lk, ready);
    else cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), ready);
}
constexpr int kIdleWaitMs = 100;   // an endless wait on no descriptor at all
}  // namespace

Ret Stack::poll(PollFd* fds, size_t n, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    bool any = false;
    for (size_t i = 0; i < n; ++i) any |= fds[i].fd >= 0 && get(fds[i].fd);
    if (!any) {
        for (size_t i = 0; i < n; ++i) fds[i].revents = 0;
        cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms < 0 ? kIdleWaitMs : timeout_ms), [] { return false; });
        return ok(0);
    }
    int count = 0;
    auto scan = [&] {
        count = 0;
        for (size_t i = 0; i < n; ++i) {
            fds[i].revents = fds[i].fd < 0 ? 0 : ready_locked(fds[i].fd, fds[i].events);
            if (fds[i].revents) ++count;
        }
        return count > 0;
    };
    wait_ready(cv_, lk, timeout_ms, scan);
    return ok(count);
}

Ret Stack::select(int nfds, uint64_t* rd, uint64_t* wr, uint64_t* ex, int timeout_ms) {
    if (nfds < 0 || nfds > 1024) return fail(kEinval);
    uint64_t want[3][16] = {};
    uint64_t* sets[3] = {rd, wr, ex};
    std::unique_lock<std::mutex> lk(mu_);
    bool any = false;
    for (int k = 0; k < 3; ++k)
        if (sets[k]) {
            std::memcpy(want[k], sets[k], sizeof want[k]);
            for (int fd = 0; fd < nfds; ++fd) any |= (want[k][fd >> 6] >> (fd & 63) & 1) != 0 && get(fd);
        }
    if (!any) {
        cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms < 0 ? kIdleWaitMs : timeout_ms), [] { return false; });
        for (int k = 0; k < 3; ++k)
            if (sets[k]) std::memset(sets[k], 0, sizeof want[k]);
        return ok(0);
    }
    uint64_t got[3][16];
    int count = 0;
    auto scan = [&] {
        std::memset(got, 0, sizeof got);
        count = 0;
        const int16_t ev[3] = {kPollIn, kPollOut, kPollPri};
        for (int k = 0; k < 3; ++k)
            for (int fd = 0; fd < nfds; ++fd)
                if ((want[k][fd >> 6] >> (fd & 63) & 1) && (ready_locked(fd, ev[k]) & (k == 0 ? (kPollIn | kPollHup) : ev[k]))) {
                    got[k][fd >> 6] |= 1ull << (fd & 63);
                    ++count;
                }
        return count > 0;
    };
    wait_ready(cv_, lk, timeout_ms, scan);
    for (int k = 0; k < 3; ++k)
        if (sets[k]) std::memcpy(sets[k], got[k], sizeof got[k]);
    return ok(count);
}

void Stack::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [fd, s] : socks_) {
        s->closed = true;
        for (const auto& c : s->pending) c->closed = true;
        s->pending.clear();
    }
    socks_.clear();
    for (auto& [key, c] : conns_) {
        c->closed = true;
        c->vt_reset = true;
    }
    conns_.clear();
    outq_.clear();
    logged_opts_.clear();
    next_port_ = kEphemeralFirst;
    cv_.notify_all();
}

Stack& stack() {
    static Stack* s = [] {
        auto* t = new Stack();   // never destroyed: guest threads may still be blocked in it at exit
        vnet::set_inbound([t](const vnet::Datagram& d) { t->inbound(d); });
        return t;
    }();
    return *s;
}

}  // namespace nxrt::vsock
