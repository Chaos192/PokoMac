// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// RyuLDN client (ryuldn.h). Ported from Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors):
// src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator/LdnRyu/{RyuLdnProtocol.cs, LdnMasterProxyClient.cs, Proxy/LdnProxy.cs}. Our own code.
#include "ryuldn.h"

#include <winsock2.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "nxrt/log.h"

namespace nxrt::vnet::ryuldn {
namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The id + MAC the server gave this process (InitializeMemory, LdnMasterProxyClient.cs:25: static, so every connection after the first
// one presents them again, :116).
std::mutex g_init_mu;
InitializeMessage g_init{};

constexpr uint32_t kReasonSignalLost = 6;   // nn::ldn DisconnectReason (Ldn/Types/DisconnectReason.cs)
// The reader looks at its stop flag this often: a Winsock shutdown() does not wake a thread blocked in recv() (it waits for the peer's
// close, or FIN_WAIT_2's 120 s), so the reader waits in select() instead (relay_server.cpp does the same).
constexpr long kReaderTickUs = 250000;

// vsock's vtcp segment (vsock.h "TCP between the nodes"; vsock.cpp vt_make / vt_parse): "vT", version 1, type, u16 sport, u16 dport,
// u32 connection id, u32 seq, u32 ack, u32 window. Types SYN 1 .. RST 6 (vsock.cpp kVtSyn..kVtRst).
constexpr size_t kVtHdr = 24;
constexpr uint8_t kVtRst = 6;

}  // namespace

// ---- framing ----------------------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> encode(Packet t, const void* body, size_t n, const void* extra, size_t extra_n) {
    Header h{};
    h.magic = kMagic;
    h.type = static_cast<uint8_t>(t);
    h.version = kVersion;
    h.data_size = static_cast<int32_t>(n + extra_n);
    std::vector<uint8_t> out(sizeof h + n + extra_n);
    std::memcpy(out.data(), &h, sizeof h);
    if (n) std::memcpy(out.data() + sizeof h, body, n);
    if (extra_n) std::memcpy(out.data() + sizeof h + n, extra, extra_n);
    return out;
}

int parse(const uint8_t* b, size_t n, Header* h) {
    if (n < sizeof(Header)) return 0;
    std::memcpy(h, b, sizeof *h);
    if (h->magic != kMagic || h->version != kVersion || h->data_size < 0) return -1;
    const size_t total = sizeof(Header) + static_cast<size_t>(h->data_size);
    if (total >= kMaxPacket) return -1;
    return n < total ? 0 : static_cast<int>(total);
}

bool valid_passphrase(const std::string& p) {
    if (p.empty()) return true;
    if (p.size() != 16 || p.compare(0, 8, "Ryujinx-") != 0) return false;
    for (size_t i = 8; i < 16; ++i)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return true;
}

bool parse_server(const std::string& s0, std::string* host, uint16_t* port) {
    size_t a = 0, b = s0.size();
    while (a < b && (s0[a] == ' ' || s0[a] == '\t')) ++a;
    while (b > a && (s0[b - 1] == ' ' || s0[b - 1] == '\t')) --b;
    const std::string s = s0.substr(a, b - a);
    if (s.empty()) return false;
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos) {
        *host = s;
        *port = kDefaultPort;
        return true;
    }
    if (colon == 0 || s.find(':') != colon) return false;   // no host, or an IPv6 literal (the client dials IPv4)
    unsigned long p = 0;
    for (size_t i = colon + 1; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9' || p > 65535) return false;
        p = p * 10 + static_cast<unsigned long>(s[i] - '0');
    }
    if (p == 0 || p > 65535) return false;
    *host = s.substr(0, colon);
    *port = static_cast<uint16_t>(p);
    return true;
}

// ---- ProxyLink --------------------------------------------------------------------------------------------------------------------------
uint32_t ProxyLink::local_ip() const {
    std::lock_guard<std::mutex> lk(mu_);
    return up_ ? ip_ : 0;
}

uint32_t ProxyLink::netmask() const {
    std::lock_guard<std::mutex> lk(mu_);
    return mask_ ? mask_ : 0xFFFFFF00u;
}

bool ProxyLink::reaches(uint32_t dst) const {
    std::lock_guard<std::mutex> lk(mu_);
    return mask_ != 0 && (dst & mask_) == (ip_ & mask_);
}

void ProxyLink::configure(uint32_t ip, uint32_t mask) {
    std::lock_guard<std::mutex> lk(mu_);
    ip_ = ip;
    mask_ = mask;
    up_ = ip != 0;
}

void ProxyLink::down() {
    std::lock_guard<std::mutex> lk(mu_);
    up_ = false;
}

// A game datagram as ProxyData (LdnProxy.SendTo, LdnProxy.cs:216-230): this node's address and the socket's port as the source.
bool ProxyLink::send(const Datagram& d) {
    if (d.src_port == 0 && d.dst_port == 0) {   // a vtcp segment: TCP between nodes is v2
        refuse_vtcp(d);
        return true;
    }
    uint32_t ip;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!up_) return false;
        ip = ip_;
    }
    const auto c = client_.lock();
    if (!c) return false;
    ProxyDataHeader h{};
    h.info.source_ip = ip;
    h.info.source_port = d.src_port;
    h.info.dest_ip = d.dst_ip;
    h.info.dest_port = d.dst_port;
    h.info.protocol = kProtoUdp;
    h.data_length = static_cast<uint32_t>(d.data.size());
    return c->send_packet(Packet::ProxyData, &h, sizeof h, d.data.data(), d.data.size());
}

// The peer "answers" RST for the segment, on the sending thread (as LoopbackHub delivers; vsock sends with its lock released): the
// connect fails with ECONNREFUSED at once instead of retrying its SYN for 12 s. The RST is the one vsock's segment_in makes for a
// connection nobody listens for (seq = the segment's ack, ack = its seq + payload).
void ProxyLink::refuse_vtcp(const Datagram& d) {
    const std::vector<uint8_t>& b = d.data;
    if (b.size() < kVtHdr || b[0] != 'v' || b[1] != 'T' || b[2] != 1 || b[3] == kVtRst) return;
    uint16_t sport, dport;
    uint32_t conn, seq, ack;
    std::memcpy(&sport, &b[4], 2);
    std::memcpy(&dport, &b[6], 2);
    std::memcpy(&conn, &b[8], 4);
    std::memcpy(&seq, &b[12], 4);
    std::memcpy(&ack, &b[16], 4);
    const uint32_t rseq = ack, rack = seq + static_cast<uint32_t>(b.size() - kVtHdr), wnd = 0;
    Datagram r;
    r.src_ip = d.dst_ip;
    r.dst_ip = d.src_ip;
    r.data.assign(kVtHdr, 0);
    r.data[0] = 'v';
    r.data[1] = 'T';
    r.data[2] = 1;
    r.data[3] = kVtRst;
    std::memcpy(&r.data[4], &dport, 2);
    std::memcpy(&r.data[6], &sport, 2);
    std::memcpy(&r.data[8], &conn, 4);
    std::memcpy(&r.data[12], &rseq, 4);
    std::memcpy(&r.data[16], &rack, 4);
    std::memcpy(&r.data[20], &wnd, 4);
    NXLOG_I("net", "ryuldn: TCP to %s:%u refused (TCP between nodes is not carried over RyuLDN yet)", ip_str(d.dst_ip).c_str(), dport);
    deliver(r);
}

void ProxyLink::from_wire(const ProxyDataHeader& h, const uint8_t* p, size_t n) {
    if (h.info.source_port == 0 && h.info.dest_port == 0) return;   // never a UDP socket's; vtcp never comes from the wire (v2)
    Datagram d;
    d.src_ip = h.info.source_ip;
    d.src_port = h.info.source_port;
    d.dst_ip = h.info.dest_ip;
    d.dst_port = h.info.dest_port;
    d.data.assign(p, p + n);
    deliver(d);
}

// ---- Client -----------------------------------------------------------------------------------------------------------------------------
Client::Client(std::string host, uint16_t port, std::string passphrase, std::string game_version, Events ev)
    : host_(std::move(host)), port_(port), pass_(std::move(passphrase)), ev_(std::move(ev)) {
    std::memcpy(game_version_.data(), game_version.data(), std::min(game_version.size(), game_version_.size()));
}

std::shared_ptr<Client> Client::make(std::string host, uint16_t port, std::string passphrase, std::string game_version, Events ev) {
    std::shared_ptr<Client> c(new Client(std::move(host), port, std::move(passphrase), std::move(game_version), std::move(ev)));
    c->link_ = std::make_shared<ProxyLink>(c);
    c->timer_ = std::thread([p = c.get()] { p->timer_loop(); });
    return c;
}

Client::~Client() { close(); }

void Client::close() {
    bool was_in;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closing_) return;
        closing_ = true;
        was_in = in_net_;
    }
    if (was_in) {   // Dispose of the access point / station: DisconnectNetwork first (AccessPoint.cs:27-35, Station.cs:54-62)
        const DisconnectMessage m{};
        send_packet(Packet::Disconnect, &m, sizeof m);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        in_net_ = false;
        rd_stop_ = true;
        cv_.notify_all();
    }
    std::lock_guard<std::mutex> cl(conn_mu_);
    if (reader_.joinable()) reader_.join();
    if (timer_.joinable()) timer_.join();
    link_->down();
}

bool Client::send_packet(Packet t, const void* body, size_t n, const void* extra, size_t extra_n) {
    const std::vector<uint8_t> b = encode(t, body, n, extra, extra_n);
    std::lock_guard<std::mutex> sl(send_mu_);
    if (sock_ == tcp::kBad) return false;
    if (tcp::send_all(sock_, b.data(), b.size())) return true;
    rd_stop_ = true;   // a failed or stalled write: the reader ends the connection
    return false;
}

// EnsureConnected (LdnMasterProxyClient.cs:101-120) with OnConnected's passphrase (:133-140): connect, Passphrase, Initialize.
bool Client::ensure_connected() {
    std::lock_guard<std::mutex> cl(conn_mu_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closing_) return false;
        if (up_ && !rd_stop_) return true;
    }
    if (reader_.joinable()) reader_.join();   // the reader of a connection that ended or is ending (it closes its socket)
    std::string err;
    const tcp::Handle h = tcp::dial(host_, port_, kFailureTimeoutMs, 2000, &err);
    if (h == tcp::kBad) {
        NXLOG_W("net", "ryuldn: cannot reach the server %s:%u: %s", host_.c_str(), port_, err.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closing_) {
            tcp::close(h);
            return false;
        }
        rd_stop_ = false;
        up_ = true;
    }
    {
        std::lock_guard<std::mutex> sl(send_mu_);
        sock_ = h;
    }
    reader_ = std::thread([this, h] { read_loop(h); });
    char pass[kPassphraseSize] = {};
    std::memcpy(pass, pass_.data(), std::min(pass_.size(), sizeof pass - 1));
    InitializeMessage init;
    {
        std::lock_guard<std::mutex> il(g_init_mu);
        init = g_init;
    }
    const bool ok = send_packet(Packet::Passphrase, pass, sizeof pass) && send_packet(Packet::Initialize, &init, sizeof init);
    if (ok) NXLOG_I("net", "ryuldn: connected to %s:%u (%s rooms)", host_.c_str(), port_, pass_.empty() ? "public" : "private");
    return ok;
}

void Client::read_loop(tcp::Handle h) {
    const SOCKET s = static_cast<SOCKET>(h);
    std::vector<uint8_t> buf, chunk(16384), body;
    bool done = false;
    while (!done && !rd_stop_) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{0, kReaderTickUs};
        const int k = ::select(static_cast<int>(s + 1), &r, nullptr, nullptr, &tv);
        if (k < 0) break;
        if (k == 0) continue;
        const int got = ::recv(s, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
        if (got == 0) break;
        if (got < 0) {
            const int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAETIMEDOUT) continue;
            break;
        }
        buf.insert(buf.end(), chunk.data(), chunk.data() + got);
        size_t at = 0;
        for (;;) {
            Header hd{};
            const int n = parse(buf.data() + at, buf.size() - at, &hd);
            if (n == 0) break;
            if (n < 0) {
                NXLOG_W("net", "ryuldn: malformed frame from the server (magic %08x, version %u, size %d): disconnecting", hd.magic,
                        hd.version, hd.data_size);
                done = true;
                break;
            }
            body.assign(buf.begin() + static_cast<std::ptrdiff_t>(at + sizeof hd), buf.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += static_cast<size_t>(n);
            handle(hd, body);
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(at));
    }
    bool was_in, closing;
    {
        std::lock_guard<std::mutex> lk(mu_);
        up_ = false;
        idle_at_ = 0;
        was_in = in_net_;
        closing = closing_;
        cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> sl(send_mu_);
        if (sock_ == h) sock_ = tcp::kBad;
    }
    tcp::close(h);
    if (!closing) NXLOG_I("net", "ryuldn: the connection to %s:%u ended", host_.c_str(), port_);
    if (was_in && !closing) lost(kReasonSignalLost);
}

void Client::handle(const Header& hd, const std::vector<uint8_t>& b) {
    const Packet t = static_cast<Packet>(hd.type);
    switch (t) {
        case Packet::Initialize:
            if (b.size() >= sizeof(InitializeMessage)) {
                std::lock_guard<std::mutex> il(g_init_mu);
                std::memcpy(&g_init, b.data(), sizeof g_init);
            }
            return;
        case Packet::Connected:
        case Packet::SyncNetwork: {
            if (b.size() < kInfoSize) return;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (t == Packet::SyncNetwork && !in_net_) return;   // a late one for a room we left
                std::memcpy(info_.data(), b.data(), kInfoSize);
                if (t == Packet::Connected) {
                    in_net_ = true;
                    reject_reason_ = 0;
                    idle_at_ = 0;
                    ++connected_seq_;
                }
                cv_.notify_all();
            }
            if (ev_.changed) ev_.changed();
            return;
        }
        case Packet::ScanReply:
            if (b.size() >= kInfoSize) {
                Info i;
                std::memcpy(i.data(), b.data(), kInfoSize);
                std::lock_guard<std::mutex> lk(mu_);
                scan_.push_back(i);
            }
            return;
        case Packet::ScanReplyEnd: {
            std::lock_guard<std::mutex> lk(mu_);
            scan_done_ = true;
            cv_.notify_all();
            return;
        }
        case Packet::Disconnect: {
            uint32_t reason;
            {
                std::lock_guard<std::mutex> lk(mu_);
                reason = reject_reason_;
            }
            lost(reason);
            return;
        }
        case Packet::Reject:   // we were turned out; the server's Disconnect follows (HandleReject, :265-269)
            if (b.size() >= sizeof(RejectRequest)) {
                RejectRequest r;
                std::memcpy(&r, b.data(), sizeof r);
                std::lock_guard<std::mutex> lk(mu_);
                reject_reason_ = r.disconnect_reason;
            }
            return;
        case Packet::RejectReply: {
            std::lock_guard<std::mutex> lk(mu_);
            reject_reply_ = true;
            cv_.notify_all();
            return;
        }
        case Packet::ProxyConfig:
            if (b.size() >= sizeof(ProxyConfig)) {
                ProxyConfig c;
                std::memcpy(&c, b.data(), sizeof c);
                link_->configure(c.proxy_ip, c.proxy_subnet_mask);
                std::lock_guard<std::mutex> lk(mu_);
                proxy_ = c;
                ++proxy_seq_;
                cv_.notify_all();
                NXLOG_I("net", "ryuldn: this node is %s mask %s", ip_str(c.proxy_ip).c_str(), ip_str(c.proxy_subnet_mask).c_str());
            }
            return;
        case Packet::ProxyData:
            if (b.size() >= sizeof(ProxyDataHeader)) {
                ProxyDataHeader h;
                std::memcpy(&h, b.data(), sizeof h);
                const size_t n = std::min<size_t>(h.data_length, b.size() - sizeof h);
                if (h.info.protocol == kProtoUdp) link_->from_wire(h, b.data() + sizeof h, n);
            }
            return;
        case Packet::ProxyConnect:
        case Packet::ProxyConnectReply:
        case Packet::ProxyDisconnect: {
            std::lock_guard<std::mutex> lk(mu_);
            if (!logged_tcp_) NXLOG_W("net", "ryuldn: a peer opened TCP to this node: TCP between nodes is not carried over RyuLDN yet");
            logged_tcp_ = true;
            return;
        }
        case Packet::Ping:
            if (b.size() >= sizeof(PingMessage)) {
                PingMessage p;
                std::memcpy(&p, b.data(), sizeof p);
                if (p.requester == 0) send_packet(Packet::Ping, &p, sizeof p);   // the server asked: it gets its message back
            }
            return;
        case Packet::NetworkError:
            if (b.size() >= sizeof(NetworkErrorMessage)) {
                NetworkErrorMessage e;
                std::memcpy(&e, b.data(), sizeof e);
                if (static_cast<NetError>(e.error) == NetError::PortUnreachable) return;   // about a P2P proxy (ours is never offered)
                std::lock_guard<std::mutex> lk(mu_);
                last_error_ = static_cast<NetError>(e.error);
                cv_.notify_all();
                NXLOG_I("net", "ryuldn: the server answered error %d", e.error);
            }
            return;
        case Packet::ExternalProxy: {
            std::lock_guard<std::mutex> lk(mu_);
            ext_proxy_ = true;
            cv_.notify_all();
            NXLOG_W("net", "ryuldn: the room's traffic runs through its host's P2P proxy, which this build does not follow yet");
            return;
        }
        default:
            return;
    }
}

void Client::lost(uint32_t reason) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_net_) return;
        in_net_ = false;
        info_ = {};
        proxy_ = {};
        if (up_) refresh_idle_locked();
        cv_.notify_all();
    }
    link_->down();
    NXLOG_I("net", "ryuldn: the room ended (reason %u)", reason);
    if (ev_.left) ev_.left(reason);
}

void Client::refresh_idle_locked() {
    idle_at_ = now_ms() + kInactiveTimeoutMs;
    cv_.notify_all();
}

// NetworkTimeout (NetworkTimeout.cs, InactiveTimeout): a connection in no room closes 6 s after the last Scan (or leaving the room).
void Client::timer_loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!closing_) {
        const int64_t now = now_ms();
        if (idle_at_ && now >= idle_at_) {
            idle_at_ = 0;
            if (up_ && !in_net_) {
                NXLOG_I("net", "ryuldn: idle for %d ms in no room: closing the connection", kInactiveTimeoutMs);
                rd_stop_ = true;
            }
            continue;
        }
        if (idle_at_) cv_.wait_for(lk, std::chrono::milliseconds(idle_at_ - now));
        else cv_.wait(lk);
    }
}

// The wait of CreateNetworkCommon / ConnectCommon (:448-471, :579-598): Connected, then the ProxyConfig, within kFailureTimeoutMs.
NetError Client::wait_joined(std::unique_lock<std::mutex>& lk, uint64_t seq0, uint64_t pseq0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFailureTimeoutMs);
    cv_.wait_until(lk, until, [&] { return connected_seq_ != seq0 || last_error_ != NetError::None || !up_ || closing_; });
    const NetError e = last_error_;
    last_error_ = NetError::None;   // ConsumeNetworkError
    if (e != NetError::None) return e;
    if (connected_seq_ == seq0) return up_ ? NetError::ConnectTimeout : NetError::Unknown;
    cv_.wait_until(lk, until, [&] { return proxy_seq_ != pseq0 || ext_proxy_ || !in_net_ || closing_; });
    if (!in_net_) return up_ ? NetError::ConnectFailure : NetError::Unknown;
    if (proxy_seq_ == pseq0) return NetError::ConnectFailure;   // an ExternalProxy room (v2) or a server that sent no ProxyConfig
    return NetError::None;
}

NetError Client::create(bool priv, std::vector<uint8_t> req, const std::vector<uint8_t>& adv) {
    const size_t want = priv ? create_ap_private::kSize : create_ap::kSize;
    if (req.size() != want) return NetError::Unknown;
    {
        std::lock_guard<std::mutex> lk(mu_);
        idle_at_ = 0;   // DisableTimeout
    }
    // ConfigureAccessPoint (:375-377): the game version; no P2P proxy (its fields stay 0: the master relays the room)
    RyuNetworkConfig rc{};
    std::memcpy(rc.game_version, game_version_.data(), sizeof rc.game_version);
    std::memcpy(req.data() + (priv ? create_ap_private::kRyuConfig : create_ap::kRyuConfig), &rc, sizeof rc);
    if (!ensure_connected()) return NetError::Unknown;
    uint64_t seq0, pseq0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_error_ = NetError::None;
        ext_proxy_ = false;
        seq0 = connected_seq_;
        pseq0 = proxy_seq_;
    }
    if (!send_packet(priv ? Packet::CreateAccessPointPrivate : Packet::CreateAccessPoint, req.data(), req.size(), adv.data(), adv.size()))
        return NetError::Unknown;
    std::unique_lock<std::mutex> lk(mu_);
    const NetError e = wait_joined(lk, seq0, pseq0);
    if (e != NetError::None && in_net_) {   // in a room we cannot use: leave it
        lk.unlock();
        leave();
    }
    return e;
}

std::vector<Info> Client::scan(const uint8_t* filter) {
    if (!ensure_connected()) return {};
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_net_) refresh_idle_locked();   // RefreshTimeout (:550-553)
        scan_.clear();
        scan_done_ = false;
    }
    if (!send_packet(Packet::Scan, filter, kScanFilterSize)) return {};
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(kScanTimeoutMs), [&] { return scan_done_ || !up_ || closing_; });
    if (!scan_done_) return {};   // an error or a timeout: no rooms (:570-574)
    return scan_;
}

NetError Client::connect(bool priv, const std::vector<uint8_t>& req) {
    if (req.size() != (priv ? connect_private::kSize : connect_req::kSize)) return NetError::Unknown;
    {
        std::lock_guard<std::mutex> lk(mu_);
        idle_at_ = 0;   // DisableTimeout
    }
    if (!ensure_connected()) return NetError::Unknown;
    uint64_t seq0, pseq0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_error_ = NetError::None;
        ext_proxy_ = false;
        seq0 = connected_seq_;
        pseq0 = proxy_seq_;
    }
    if (!send_packet(priv ? Packet::ConnectPrivate : Packet::Connect, req.data(), req.size())) return NetError::Unknown;
    std::unique_lock<std::mutex> lk(mu_);
    const NetError e = wait_joined(lk, seq0, pseq0);
    const bool ext = ext_proxy_;
    if (e != NetError::None) {
        if (e == NetError::ConnectFailure && ext)
            NXLOG_W("net", "ryuldn: Connect: the host hosts a P2P proxy (ExternalProxy), which this build cannot follow yet");
        if (in_net_) {
            lk.unlock();
            leave();
        } else {
            lk.unlock();
            link_->down();
        }
    }
    return e;
}

// DisconnectNetwork (:309-317).
void Client::leave() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_net_) return;
        in_net_ = false;
        info_ = {};
        proxy_ = {};
        if (up_) refresh_idle_locked();
    }
    link_->down();
    const DisconnectMessage m{};
    send_packet(Packet::Disconnect, &m, sizeof m);
}

bool Client::reject(uint32_t node_id, uint32_t reason) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!in_net_) return false;
        reject_reply_ = false;
        last_error_ = NetError::None;
    }
    const RejectRequest r{node_id, reason};
    if (!send_packet(Packet::Reject, &r, sizeof r)) return false;
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(kInactiveTimeoutMs), [&] { return reject_reply_ || !up_ || closing_; });
    if (!reject_reply_) return false;
    const NetError e = last_error_;
    last_error_ = NetError::None;
    return e == NetError::None;
}

void Client::set_accept_policy(uint8_t policy) {
    if (!in_network()) return;
    send_packet(Packet::SetAcceptPolicy, &policy, 1);
}

void Client::set_advertise_data(const std::vector<uint8_t>& adv) {
    if (!in_network()) return;
    send_packet(Packet::SetAdvertiseData, adv.data(), adv.size());
}

bool Client::in_network() const {
    std::lock_guard<std::mutex> lk(mu_);
    return in_net_;
}

Info Client::info() const {
    std::lock_guard<std::mutex> lk(mu_);
    return info_;
}

ProxyConfig Client::proxy() const {
    std::lock_guard<std::mutex> lk(mu_);
    return proxy_;
}

std::shared_ptr<Link> Client::link() const { return link_; }

}  // namespace nxrt::vnet::ryuldn
