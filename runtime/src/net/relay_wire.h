// SPDX-License-Identifier: GPL-3.0-or-later
// Relay protocol v3 wire codec, shared by the client (lobby.cpp) and the embedded server (relay_server.cpp). Internal to net/.
// Reference: launcher/src-tauri/src/relay/mod.rs. Frame = u32 len (bytes after it, 1..8192) + u8 type + payload, little-endian;
// str = u8 len + UTF-8; blob = u16 len + bytes. UDP packets: u8 type + u64 token (+ the datagram header and payload), no length prefix.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "lobby.h"

namespace nxrt::vnet::wire {

constexpr size_t kMaxFrame = 8192;
constexpr size_t kMaxData = 4096;   // DATAGRAM payload cap (= vnet::kMaxDatagram)

enum : uint8_t {
    kHello = 0x01, kScan = 0x02, kCreate = 0x03, kJoin = 0x04, kLeave = 0x05, kAdvertise = 0x06, kReject = 0x07, kDatagram = 0x08, kPing = 0x09,
    kUdpMode = 0x0A,
    kWelcome = 0x81, kError = 0x82, kRoomList = 0x83, kJoined = 0x84, kRoomState = 0x85, kSDatagram = 0x86, kLeft = 0x87, kPong = 0x88,
    kUdpOffer = 0x89,
};

// v3 UDP packets: u8 type + u64 token, then (DATA) the datagram header and payload.
enum : uint8_t { kUHello = 0x01, kUData = 0x08, kUAck = 0x81, kUSData = 0x86 };
constexpr size_t kUHdr = 9, kUDataHdr = kUHdr + 8, kUSDataHdr = kUHdr + 12;
constexpr size_t kUMaxPacket = kUSDataHdr + kMaxData;

inline void put_le(std::vector<uint8_t>& b, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline uint64_t get_le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Frame builder: Out(type), fields, finish() patches the length.
struct Out {
    std::vector<uint8_t> b;
    explicit Out(uint8_t type) : b(4, 0) { b.push_back(type); }
    Out& u8(uint8_t v) { b.push_back(v); return *this; }
    Out& u16(uint16_t v) { return u8(static_cast<uint8_t>(v)).u8(static_cast<uint8_t>(v >> 8)); }
    Out& u32(uint32_t v) { return u16(static_cast<uint16_t>(v)).u16(static_cast<uint16_t>(v >> 16)); }
    Out& u64(uint64_t v) { return u32(static_cast<uint32_t>(v)).u32(static_cast<uint32_t>(v >> 32)); }
    Out& raw(const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); return *this; }
    Out& str(const std::string& s, size_t cap = 255) {
        size_t n = s.size() < cap ? s.size() : cap;
        while (n && n < s.size() && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;   // never cut inside a UTF-8 sequence
        return u8(static_cast<uint8_t>(n)).raw(s.data(), n);
    }
    Out& blob(const std::vector<uint8_t>& v) { return u16(static_cast<uint16_t>(v.size())).raw(v.data(), v.size()); }
    std::vector<uint8_t> finish() {
        const uint32_t len = static_cast<uint32_t>(b.size() - 4);
        std::memcpy(b.data(), &len, 4);
        return std::move(b);
    }
};

// Payload reader: every read checks the remaining length; a short read clears ok and returns zero / empty.
struct In {
    const uint8_t* p;
    size_t n, o = 0;
    bool ok = true;
    In(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
    bool need(size_t k) { if (o + k > n) ok = false; return ok; }
    uint8_t u8() { return need(1) ? p[o++] : 0; }
    uint16_t u16() { const uint16_t lo = u8(); return static_cast<uint16_t>(lo | (u8() << 8)); }
    uint32_t u32() { const uint32_t lo = u16(); return lo | (uint32_t{u16()} << 16); }
    uint64_t u64() { const uint64_t lo = u32(); return lo | (uint64_t{u32()} << 32); }
    std::array<uint8_t, 16> b16() {   // a room key or passphrase hash
        std::array<uint8_t, 16> a{};
        if (need(16)) {
            std::memcpy(a.data(), p + o, 16);
            o += 16;
        }
        return a;
    }
    std::string str() {
        const size_t k = u8();
        if (!need(k)) return {};
        std::string s(reinterpret_cast<const char*>(p + o), k);
        o += k;
        return s;
    }
    std::vector<uint8_t> blob() {
        const size_t k = u16();
        if (!need(k)) return {};
        std::vector<uint8_t> v(p + o, p + o + k);
        o += k;
        return v;
    }
    bool done() const { return ok && o == n; }   // well formed and used up exactly (the server's rule: no trailing bytes)
};

// ROOM_INFO = u32 room_id, u64 local_comm_id, u16 scene_id, s16 comm_version, u8 node_count_max, u8 security_mode, u8 node_count,
// node_count x {u8 node_id, u32 vip, 6 B mac, str nickname}, u16 adv_len + adv.
// A relay is not trusted with the game's player limit: more than kMaxNodes nodes, or a node id past kMaxNodes or listed twice, clears in.ok
// (the caller drops the room); node_count_max is clamped to kMaxNodes.
inline Room read_room(In& in) {
    Room r;
    r.room_id = in.u32();
    r.local_comm_id = in.u64();
    r.scene_id = in.u16();
    r.comm_version = static_cast<int16_t>(in.u16());
    r.node_count_max = in.u8();
    if (r.node_count_max > kMaxNodes) r.node_count_max = kMaxNodes;
    r.security_mode = in.u8();
    const uint8_t count = in.u8();
    if (count > kMaxNodes) in.ok = false;
    uint32_t ids = 0;   // node ids seen
    for (uint8_t i = 0; i < count && in.ok; ++i) {
        RoomNode nd;
        nd.node_id = in.u8();
        if (nd.node_id >= kMaxNodes || (ids & (1u << nd.node_id))) in.ok = false;
        else ids |= 1u << nd.node_id;
        nd.vip = in.u32();
        if (in.need(6)) {
            std::memcpy(nd.mac.data(), in.p + in.o, 6);
            in.o += 6;
        }
        nd.nickname = in.str();
        r.nodes.push_back(std::move(nd));
    }
    r.adv = in.blob();
    return r;
}

inline void write_room(Out& o, const Room& r) {
    o.u32(r.room_id).u64(r.local_comm_id).u16(r.scene_id).u16(static_cast<uint16_t>(r.comm_version)).u8(r.node_count_max).u8(r.security_mode);
    o.u8(static_cast<uint8_t>(r.nodes.size()));
    for (const RoomNode& n : r.nodes) o.u8(n.node_id).u32(n.vip).raw(n.mac.data(), 6).str(n.nickname);
    o.blob(r.adv);
}

}  // namespace nxrt::vnet::wire
