// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nxrt/hipc.h"

#include <cstring>

namespace nxrt::hipc {
namespace {

constexpr size_t kMaxRawWords = 0x3FF;              // header1 bits 0-9
constexpr uint64_t kMaxAddr = (1ull << 39) - 1;     // X and A/B/W addresses: 39 bits
constexpr uint64_t kMaxMapSize = (1ull << 36) - 1;  // A/B/W sizes: 36 bits
constexpr uint64_t kMaxRecvAddr = (1ull << 48) - 1; // receive list addresses: 48 bits
constexpr uint32_t kMaxPtrSize = 0xFFFF;            // X and receive list sizes: 16 bits

bool fail_(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

// Receive-list entries that follow the raw data: 0 for flags 0 and 1 (inline), 1 for flags 2, flags - 2 for 3 and up.
size_t recv_list_entries(uint32_t flags) {
    if (flags < 2) return 0;
    if (flags == 2) return 1;
    return flags - 2;
}

void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// Word and byte layout of a message, computed once and shared by build() and built_size_words().
struct Plan {
    bool special = false;
    bool aligned = false;
    size_t desc_end_bytes = 0;  // header + special header + descriptors, in bytes from the message start
    size_t pad_before = 0;      // bytes
    size_t pad_after = 0;       // bytes; pad_before + pad_after is 0x10 for aligned raw data
    size_t payload_bytes = 0;   // raw payload rounded up to a whole word
    size_t raw_words = 0;       // header1 raw size: pad_before + payload + pad_after + u16 sizes, rounded to words
    size_t recv_entries = 0;
    size_t total_words = 0;
};

bool make_plan(const Message& m, Plan& p, std::string* err) {
    if (m.ptrs.size() > 15) return fail_(err, "more than 15 X (pointer) descriptors");
    if (m.send.size() > 15) return fail_(err, "more than 15 A (send) descriptors");
    if (m.recv.size() > 15) return fail_(err, "more than 15 B (receive) descriptors");
    if (m.exch.size() > 15) return fail_(err, "more than 15 W (exchange) descriptors");
    if (m.copy.size() > 15) return fail_(err, "more than 15 copy handles");
    if (m.move.size() > 15) return fail_(err, "more than 15 move handles");
    if (m.recv_list_flags > 15) return fail_(err, "receive list flags above 4 bits");
    p.recv_entries = recv_list_entries(m.recv_list_flags);
    if (m.recv_list.size() != p.recv_entries) return fail_(err, "receive list entry count does not match its flags");

    for (const PtrDesc& d : m.ptrs) {
        if (d.addr > kMaxAddr) return fail_(err, "X address above 39 bits");
        if (d.size > kMaxPtrSize) return fail_(err, "X size above 16 bits");
        if (d.index > 15) return fail_(err, "X index above 4 bits");
    }
    for (const std::vector<BufDesc>* list : {&m.send, &m.recv, &m.exch}) {
        for (const BufDesc& d : *list) {
            if (d.addr > kMaxAddr) return fail_(err, "A/B/W address above 39 bits");
            if (d.size > kMaxMapSize) return fail_(err, "A/B/W size above 36 bits");
            if (d.attr > 3) return fail_(err, "A/B/W attribute above 2 bits");
        }
    }
    for (const RecvDesc& r : m.recv_list) {
        if (r.addr > kMaxRecvAddr) return fail_(err, "receive list address above 48 bits");
        if (r.size > kMaxPtrSize) return fail_(err, "receive list size above 16 bits");
    }

    p.special = m.has_pid || !m.copy.empty() || !m.move.empty();
    const size_t special_bytes = p.special ? 4 + (m.has_pid ? 8 : 0) + 4 * (m.copy.size() + m.move.size()) : 0;
    const size_t desc_bytes = 8 * m.ptrs.size() + 12 * (m.send.size() + m.recv.size() + m.exch.size());
    p.desc_end_bytes = 8 + special_bytes + desc_bytes;

    p.aligned = has_aligned_raw(m.type);
    p.pad_before = p.aligned ? (16 - p.desc_end_bytes % 16) % 16 : 0;
    p.pad_after = p.aligned ? 16 - p.pad_before : 0;
    p.payload_bytes = (m.raw.size() + 3) & ~size_t{3};
    const size_t raw_bytes = p.pad_before + p.payload_bytes + p.pad_after + 2 * m.out_ptr_sizes.size();
    p.raw_words = (raw_bytes + 3) / 4;
    if (p.raw_words > kMaxRawWords) return fail_(err, "raw data above 1023 words (10-bit size field)");

    const size_t special_words = p.special ? 1 + (m.has_pid ? 2 : 0) + m.copy.size() + m.move.size() : 0;
    const size_t desc_words = 2 * m.ptrs.size() + 3 * (m.send.size() + m.recv.size() + m.exch.size());
    p.total_words = 2 + special_words + desc_words + p.raw_words + 2 * p.recv_entries;
    return true;
}

}  // namespace

bool parse(const uint32_t* words, size_t nwords, Message& out, std::string* err) {
    out = Message{};
    if (err) err->clear();
    if (nwords < 2) return fail_(err, "message shorter than its 2-word header");

    // pos is always <= nwords; every read is preceded by a check against nwords - pos.
    size_t pos = 2;
    const uint32_t h0 = words[0];
    const uint32_t h1 = words[1];
    out.type = static_cast<uint16_t>(h0 & 0xFFFF);
    const size_t n_x = (h0 >> 16) & 0xF;
    const size_t n_a = (h0 >> 20) & 0xF;
    const size_t n_b = (h0 >> 24) & 0xF;
    const size_t n_w = (h0 >> 28) & 0xF;
    const bool special = ((h1 >> 31) & 1u) != 0;
    const uint32_t flags = (h1 >> 10) & 0xF;
    const size_t raw_words = h1 & 0x3FF;

    if (special) {
        if (nwords - pos < 1) return fail_(err, "special header announced but missing");
        const uint32_t sp = words[pos++];
        out.has_pid = (sp & 1u) != 0;
        const size_t n_copy = (sp >> 1) & 0xF;
        const size_t n_move = (sp >> 5) & 0xF;
        if (out.has_pid) {
            if (nwords - pos < 2) return fail_(err, "pid announced but truncated");
            out.pid = uint64_t{words[pos]} | (uint64_t{words[pos + 1]} << 32);
            pos += 2;
        }
        if (nwords - pos < n_copy + n_move) return fail_(err, "copy/move handle list truncated");
        for (size_t i = 0; i < n_copy; ++i) out.copy.push_back(words[pos++]);
        for (size_t i = 0; i < n_move; ++i) out.move.push_back(words[pos++]);
    }

    if (nwords - pos < 2 * n_x) return fail_(err, "X descriptor list truncated");
    for (size_t i = 0; i < n_x; ++i, pos += 2) {
        const uint32_t w0 = words[pos];
        const uint32_t w1 = words[pos + 1];
        PtrDesc d;
        d.addr = uint64_t{w1} | (uint64_t{(w0 >> 6) & 7} << 36) | (uint64_t{(w0 >> 12) & 0xF} << 32);
        d.size = w0 >> 16;
        d.index = w0 & 0xF;
        out.ptrs.push_back(d);
    }

    if (nwords - pos < 3 * (n_a + n_b + n_w)) return fail_(err, "A/B/W descriptor list truncated");
    auto read_map = [&](size_t count, std::vector<BufDesc>& dst) {
        for (size_t i = 0; i < count; ++i, pos += 3) {
            const uint32_t w0 = words[pos];
            const uint32_t w1 = words[pos + 1];
            const uint32_t w2 = words[pos + 2];
            BufDesc d;
            d.size = uint64_t{w0} | (uint64_t{(w2 >> 24) & 0xF} << 32);
            d.addr = uint64_t{w1} | (uint64_t{(w2 >> 2) & 7} << 36) | (uint64_t{(w2 >> 28) & 0xF} << 32);
            d.attr = w2 & 3;
            dst.push_back(d);
        }
    };
    read_map(n_a, out.send);
    read_map(n_b, out.recv);
    read_map(n_w, out.exch);

    // Raw section: leading pad to the next 16-byte boundary (CMIF and response types only), then the rest of raw_words.
    const size_t desc_bytes = pos * 4;
    const size_t pad = has_aligned_raw(out.type) ? (16 - desc_bytes % 16) % 16 : 0;
    if (nwords - pos < raw_words) return fail_(err, "raw data truncated: header size exceeds the words given");
    if (pad > raw_words * 4) return fail_(err, "raw section shorter than its leading alignment padding");
    out.pad_before = static_cast<uint32_t>(pad);
    const uint8_t* raw_bytes = reinterpret_cast<const uint8_t*>(words + pos) + pad;
    out.raw.assign(raw_bytes, raw_bytes + (raw_words * 4 - pad));
    pos += raw_words;

    out.recv_list_flags = flags;
    const size_t n_rl = recv_list_entries(flags);
    if (nwords - pos < 2 * n_rl) return fail_(err, "receive list truncated");
    for (size_t i = 0; i < n_rl; ++i, pos += 2) {
        RecvDesc r;
        r.addr = uint64_t{words[pos]} | (uint64_t{words[pos + 1] & 0xFFFF} << 32);
        r.size = words[pos + 1] >> 16;
        out.recv_list.push_back(r);
    }
    return true;
}

size_t build(const Message& m, uint32_t* out, size_t cap_words, std::string* err) {
    if (err) err->clear();
    Plan p;
    if (!make_plan(m, p, err)) return 0;
    if (p.total_words > cap_words) {
        fail_(err, "output buffer too small");
        return 0;
    }
    if (!out) {
        fail_(err, "null output buffer");
        return 0;
    }

    // Assemble in a byte buffer (zero-filled, so every padding word is zero), then copy out in one step.
    std::vector<uint8_t> buf(p.total_words * 4, 0);
    uint8_t* d = buf.data();
    put32(d, static_cast<uint32_t>(m.type) | (static_cast<uint32_t>(m.ptrs.size()) << 16) |
                 (static_cast<uint32_t>(m.send.size()) << 20) | (static_cast<uint32_t>(m.recv.size()) << 24) |
                 (static_cast<uint32_t>(m.exch.size()) << 28));
    put32(d + 4, static_cast<uint32_t>(p.raw_words) | (m.recv_list_flags << 10) | (p.special ? 0x80000000u : 0u));

    size_t pos = 8;
    if (p.special) {
        put32(d + pos, (m.has_pid ? 1u : 0u) | (static_cast<uint32_t>(m.copy.size()) << 1) |
                           (static_cast<uint32_t>(m.move.size()) << 5));
        pos += 4;
        if (m.has_pid) {
            put32(d + pos, static_cast<uint32_t>(m.pid));
            put32(d + pos + 4, static_cast<uint32_t>(m.pid >> 32));
            pos += 8;
        }
        for (uint32_t h : m.copy) {
            put32(d + pos, h);
            pos += 4;
        }
        for (uint32_t h : m.move) {
            put32(d + pos, h);
            pos += 4;
        }
    }
    for (const PtrDesc& x : m.ptrs) {
        put32(d + pos, (x.index & 0xF) | (static_cast<uint32_t>((x.addr >> 36) & 7) << 6) |
                           (static_cast<uint32_t>((x.addr >> 32) & 0xF) << 12) | (x.size << 16));
        put32(d + pos + 4, static_cast<uint32_t>(x.addr));
        pos += 8;
    }
    for (const std::vector<BufDesc>* list : {&m.send, &m.recv, &m.exch}) {
        for (const BufDesc& a : *list) {
            put32(d + pos, static_cast<uint32_t>(a.size));
            put32(d + pos + 4, static_cast<uint32_t>(a.addr));
            put32(d + pos + 8, (a.attr & 3) | (static_cast<uint32_t>((a.addr >> 36) & 7) << 2) |
                                   (static_cast<uint32_t>((a.size >> 32) & 0xF) << 24) |
                                   (static_cast<uint32_t>((a.addr >> 32) & 0xF) << 28));
            pos += 12;
        }
    }

    // Raw section: [pad_before zeros][payload][pad_after zeros][u16 out-pointer sizes][zeros to a word boundary].
    pos += p.pad_before;
    if (!m.raw.empty()) std::memcpy(d + pos, m.raw.data(), m.raw.size());
    pos += p.payload_bytes + p.pad_after;
    for (uint16_t s : m.out_ptr_sizes) {
        put16(d + pos, s);
        pos += 2;
    }

    // Receive list entries follow the raw section.
    pos = p.desc_end_bytes + p.raw_words * 4;
    for (const RecvDesc& r : m.recv_list) {
        put32(d + pos, static_cast<uint32_t>(r.addr));
        put32(d + pos + 4, static_cast<uint32_t>((r.addr >> 32) & 0xFFFF) | (r.size << 16));
        pos += 8;
    }

    std::memcpy(out, d, buf.size());
    return p.total_words;
}

size_t built_size_words(const Message& m) {
    Plan p;
    return make_plan(m, p, nullptr) ? p.total_words : 0;
}

}  // namespace nxrt::hipc
