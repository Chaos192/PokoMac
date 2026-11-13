// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// bsd:u RecvMMsg / SendMMsg (bsd_mmsg.h).
#include "bsd_mmsg.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>

#include "nxrt/services.h"

namespace nxrt::services::mmsg {
namespace {

constexpr size_t kSockaddr = 16, kNameMax = 0x100;
constexpr size_t kBufferMax = 16u << 20;   // a B buffer larger than this is refused (EINVAL)

// The fields of one serialized message, as offsets into the buffer.
struct Field {
    size_t at = 0, size = 0;
};
struct Message {
    Field name, control;
    std::vector<Field> iovs;
    size_t msg_flags = 0, msg_len = 0;   // offsets of the two u32
    size_t capacity = 0;                 // the iovs' bytes
};

// Takes the messages from the buffer in order (Lichii MessageParser).
class Parser {
public:
    explicit Parser(const std::vector<uint8_t>& b) : b_(b) {}
    bool skip(size_t n) {
        if (b_.size() - at_ < n) return false;
        at_ += n;
        return true;
    }
    bool u32(uint32_t* v) {
        if (b_.size() - at_ < 4) return false;
        std::memcpy(v, b_.data() + at_, 4);
        at_ += 4;
        return true;
    }
    bool sized(size_t width, Field* f) {   // a size field (u32 or u64) and that many bytes
        uint64_t n = 0;
        if (b_.size() - at_ < width) return false;
        std::memcpy(&n, b_.data() + at_, width);
        at_ += width;
        if (b_.size() - at_ < n) return false;
        f->at = at_;
        f->size = static_cast<size_t>(n);
        at_ += f->size;
        return true;
    }
    bool message(Message* m) {
        *m = Message{};
        uint32_t count = 0;
        if (!sized(4, &m->name) || !u32(&count)) return false;
        for (uint32_t i = 0; i < count; ++i) {   // each iov takes at least its size field: a count past the buffer fails here
            Field f;
            if (!sized(8, &f)) return false;
            m->iovs.push_back(f);
            m->capacity += f.size;
        }
        if (!sized(4, &m->control)) return false;
        m->msg_flags = at_;
        if (!skip(4)) return false;
        m->msg_len = at_;
        return skip(4);
    }

private:
    const std::vector<uint8_t>& b_;
    size_t at_ = 0;
};

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    const size_t o = v.size();
    v.resize(o + 4);
    std::memcpy(v.data() + o, &x, 4);
}
void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    const size_t o = v.size();
    v.resize(o + 8);
    std::memcpy(v.data() + o, &x, 8);
}

void sockaddr_of(const vsock::Addr& a, uint8_t (&sa)[kSockaddr]) {
    std::memset(sa, 0, sizeof sa);
    sa[0] = static_cast<uint8_t>(kSockaddr);
    sa[1] = static_cast<uint8_t>(vsock::kAfInet);
    sa[2] = static_cast<uint8_t>(a.port >> 8);
    sa[3] = static_cast<uint8_t>(a.port);
    sa[4] = static_cast<uint8_t>(a.ip >> 24);
    sa[5] = static_cast<uint8_t>(a.ip >> 16);
    sa[6] = static_cast<uint8_t>(a.ip >> 8);
    sa[7] = static_cast<uint8_t>(a.ip);
}

// Why a message's name cannot be sent to (0: it can; *to set, or null for no name).
uint32_t destination(const std::vector<uint8_t>& b, const Message& m, std::optional<vsock::Addr>* to) {
    to->reset();
    if (m.name.size == 0) return 0;
    if (m.name.size < kSockaddr || m.name.size > kNameMax) return vsock::kEinval;
    const uint8_t* sa = b.data() + m.name.at;
    if (sa[1] != vsock::kAfInet) return vsock::kEafnosupport;
    vsock::Addr a;
    a.port = static_cast<uint16_t>((sa[2] << 8) | sa[3]);
    a.ip = (uint32_t{sa[4]} << 24) | (uint32_t{sa[5]} << 16) | (uint32_t{sa[6]} << 8) | sa[7];
    *to = a;
    return 0;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<uint8_t> read_b(const ipc::Request& req) {
    const ipc::Buffer b = req.out_buf(0);
    std::vector<uint8_t> v;
    if (!b.addr || b.size > kBufferMax) return v;
    v.resize(static_cast<size_t>(b.size));
    buf_read(b, v.data(), v.size());
    return v;
}

}  // namespace

vsock::Ret recv(const Sock& s, uint32_t vlen, uint32_t flags, int64_t sec, int64_t nsec, std::vector<uint8_t>& buf) {
    if (vlen == 0) return {0, 0};
    if (sec < 0 || nsec < 0 || nsec > 999999999) return {-1, vsock::kEinval};
    if (!s.valid) return {-1, vsock::kEbadf};
    if (flags & ~static_cast<uint32_t>(vsock::kMsgPeek | vsock::kMsgDontwait | kMsgWaitforone)) return {-1, vsock::kEopnotsupp};
    std::vector<Message> msgs;
    Parser p(buf);
    if (!p.skip(1)) return {-1, vsock::kEinval};   // the header byte
    for (uint32_t i = 0; i < vlen; ++i) {   // each message takes at least its size fields: a vlen past the buffer fails the parse
        Message m;
        if (!p.message(&m)) return {-1, vsock::kEinval};
        msgs.push_back(std::move(m));
    }
    const int peek = static_cast<int>(flags & vsock::kMsgPeek);
    const bool timed = sec != 0 || nsec != 0;
    const int64_t timeout_ms = sec > (INT64_MAX / 2000) ? INT64_MAX / 2 : sec * 1000 + (nsec + 999999) / 1000000;
    const int64_t deadline = timed ? now_ms() + timeout_ms : 0;
    std::vector<uint8_t> reply(1, buf[0]);
    std::vector<uint8_t> data;
    uint32_t received = 0;
    for (; received < vlen; ++received) {
        const Message& m = msgs[received];
        data.assign(m.capacity + (s.stream ? 0 : 1), 0);   // a byte more than the iovs hold: a longer datagram shows as cut (MSG_TRUNC)
        vsock::Addr from{};
        vsock::Ret r;
        if (received == 0 && timed) {   // the timeout bounds the first message's wait, whatever the socket's mode
            for (;;) {
                const int64_t left = deadline - now_ms();
                if (left <= 0) return {0, 0};   // none came
                s.wait_in(static_cast<int>(std::min<int64_t>(left, 0x7fffffff)));
                r = s.recvfrom(data.data(), data.size(), peek | vsock::kMsgDontwait, s.stream ? nullptr : &from);
                if (r.err != vsock::kEagain) break;
            }
        } else {
            const bool dont_wait = received > 0 && (flags & (kMsgWaitforone | vsock::kMsgDontwait));
            const int f = peek | static_cast<int>(flags & vsock::kMsgDontwait) | (dont_wait ? vsock::kMsgDontwait : 0);
            r = s.recvfrom(data.data(), data.size(), f, s.stream ? nullptr : &from);
        }
        if (r.err) {
            if (received == 0) return r;
            break;   // the messages so far
        }
        size_t n = static_cast<size_t>(r.ret);
        uint32_t msg_flags = 0;
        if (!s.stream && n > m.capacity) {
            n = m.capacity;
            msg_flags = kMsgTrunc;
        }
        // the name: the sender's sockaddr, cut to the name buffer
        uint8_t sa[kSockaddr];
        sockaddr_of(from, sa);
        const uint32_t name_size = s.stream ? 0 : static_cast<uint32_t>(std::min(m.name.size, kSockaddr));
        put_u32(reply, name_size);
        reply.insert(reply.end(), sa, sa + name_size);
        put_u32(reply, static_cast<uint32_t>(m.iovs.size()));
        size_t done = 0;
        for (const Field& iov : m.iovs) {   // the data scattered over the iovs in order; the rest of each as the request had it
            put_u64(reply, iov.size);
            const size_t at = reply.size();
            reply.insert(reply.end(), buf.begin() + static_cast<std::ptrdiff_t>(iov.at),
                         buf.begin() + static_cast<std::ptrdiff_t>(iov.at + iov.size));
            const size_t k = std::min(iov.size, n - done);
            if (k) std::memcpy(reply.data() + at, data.data() + done, k);
            done += k;
        }
        put_u32(reply, 0);   // no control data is received
        put_u32(reply, msg_flags);
        put_u32(reply, static_cast<uint32_t>(n));
    }
    buf = std::move(reply);
    return {static_cast<int32_t>(received), 0};
}

vsock::Ret send(const Sock& s, uint32_t vlen, uint32_t flags, std::vector<uint8_t>& buf) {
    if (vlen == 0) return {0, 0};
    if (!s.valid) return {-1, vsock::kEbadf};
    if (flags & ~static_cast<uint32_t>(vsock::kMsgDontwait | kMsgNosignal)) return {-1, vsock::kEopnotsupp};
    uint32_t sent = 0;
    auto fail = [&](uint32_t e) -> vsock::Ret { return sent ? vsock::Ret{static_cast<int32_t>(sent), 0} : vsock::Ret{-1, e}; };
    Parser p(buf);
    if (!p.skip(1)) return fail(vsock::kEinval);
    std::vector<uint8_t> data;
    for (; sent < vlen;) {
        Message m;
        if (!p.message(&m)) return fail(vsock::kEinval);
        if (m.control.size) return fail(vsock::kEopnotsupp);
        std::optional<vsock::Addr> to;
        if (const uint32_t e = destination(buf, m, &to)) return fail(e);
        data.clear();
        for (const Field& iov : m.iovs)
            data.insert(data.end(), buf.begin() + static_cast<std::ptrdiff_t>(iov.at), buf.begin() + static_cast<std::ptrdiff_t>(iov.at + iov.size));
        const vsock::Ret r = s.sendto(data.data(), data.size(), static_cast<int>(flags & vsock::kMsgDontwait), to ? &*to : nullptr);
        if (r.err) return fail(r.err);
        const uint32_t len = static_cast<uint32_t>(r.ret);
        std::memcpy(buf.data() + m.msg_len, &len, 4);
        ++sent;
        if (static_cast<size_t>(r.ret) < data.size()) break;   // a stream took part of it: the rest must go before any later message
    }
    return {static_cast<int32_t>(sent), 0};
}

uint32_t recvmmsg(ipc::Request& req, ipc::Response& rsp, const Sock& s) {
    std::vector<uint8_t> buf = read_b(req);
    const vsock::Ret r = recv(s, req.arg<uint32_t>(4), req.arg<uint32_t>(8), req.arg<int64_t>(0x10), req.arg<int64_t>(0x18), buf);
    if (r.ret > 0) buf_write(req.out_buf(0), buf.data(), buf.size());
    rsp.push(r.ret);
    rsp.push(r.err);
    return 0;
}

uint32_t sendmmsg(ipc::Request& req, ipc::Response& rsp, const Sock& s) {
    std::vector<uint8_t> buf = read_b(req);
    const vsock::Ret r = send(s, req.arg<uint32_t>(4), req.arg<uint32_t>(8), buf);
    if (r.ret > 0) buf_write(req.out_buf(0), buf.data(), buf.size());
    rsp.push(r.ret);
    rsp.push(r.err);
    return 0;
}

bool first_destination(const ipc::Request& req, vsock::Addr* a) {
    const std::vector<uint8_t> buf = read_b(req);
    Parser p(buf);
    Message m;
    std::optional<vsock::Addr> to;
    if (req.arg<uint32_t>(4) == 0 || !p.skip(1) || !p.message(&m) || destination(buf, m, &to) || !to) return false;
    *a = *to;
    return true;
}

Sock offline(int fd) {
    vsock::Stack& st = vsock::stack();
    Sock s;
    s.valid = st.is_socket(fd);
    int32_t type = 0;
    size_t tn = sizeof type;
    s.stream = s.valid && !st.getsockopt(fd, vsock::kSolSocket, vsock::kSoType, &type, &tn).err && type == vsock::kSockStream;
    s.recvfrom = [fd](void* p, size_t n, int flags, vsock::Addr* from) { return vsock::stack().recvfrom(fd, p, n, flags, from); };
    s.sendto = [fd](const void* p, size_t n, int flags, const vsock::Addr* to) { return vsock::stack().sendto(fd, p, n, flags, to); };
    s.wait_in = [fd](int ms) -> int16_t {
        vsock::PollFd pf{fd, vsock::kPollIn, 0};
        vsock::stack().poll(&pf, 1, ms);
        return pf.revents;
    };
    return s;
}

}  // namespace nxrt::services::mmsg
