// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Input trace file: text serialisation and recording logic. The format is documented in include/nxrt/tt_inputfile.h.
#include "nxrt/tt_inputfile.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::tt {

namespace {

enum class Num { ok, bad, range };

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

std::vector<std::string> split_tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) i++;
        size_t b = i;
        while (i < s.size() && !is_space(s[i])) i++;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

Num parse_dec(const std::string& s, uint64_t& out) {
    if (s.empty()) return Num::bad;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return Num::bad;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return Num::range;
        v = v * 10 + d;
    }
    out = v;
    return Num::ok;
}

// "0x" followed by hex digits (either case).
Num parse_hex(const std::string& s, uint64_t& out) {
    if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return Num::bad;
    uint64_t v = 0;
    for (size_t i = 2; i < s.size(); i++) {
        char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
        else return Num::bad;
        if ((v >> 60) != 0) return Num::range;
        v = (v << 4) | d;
    }
    out = v;
    return Num::ok;
}

// Optional sign, decimal digits, must lie in -32767..32767.
Num parse_stick(const std::string& s, int& out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        i++;
    }
    if (i >= s.size()) return Num::bad;
    long long v = 0;
    bool big = false;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return Num::bad;
        if (v < 100000000LL) v = v * 10 + (s[i] - '0');
        else big = true;
    }
    if (neg) v = -v;
    if (big || v < -32767 || v > 32767) return Num::range;
    out = static_cast<int>(v);
    return Num::ok;
}

bool parse_float(const std::string& s, float& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

}  // namespace

void InputTrace::record(uint64_t frame, const PadFrame& pad) {
    // Calls must come in increasing frame order; a repeated or earlier frame is ignored.
    if (have_last_ && frame <= last_frame_) return;

    const bool impulse = pad.mouse_dx != 0 || pad.mouse_dy != 0;
    const bool add = !have_last_ || !pad.held_equal(last_recorded_) || impulse || last_had_impulse_;
    if (add) {
        entries.push_back(Entry{frame, pad});
        last_recorded_ = pad;
        last_had_impulse_ = impulse;
    }
    last_frame_ = frame;
    have_last_ = true;
    end_frame = frame + 1;
}

PadFrame InputTrace::at(uint64_t frame) const {
    // Find the first entry with entry.frame > frame; the one before it is the last entry at or before frame.
    size_t lo = 0, hi = entries.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].frame <= frame) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return PadFrame{};
    const Entry& e = entries[lo - 1];
    PadFrame out = e.pad;
    if (e.frame != frame) {
        out.mouse_dx = 0;
        out.mouse_dy = 0;
    }
    return out;
}

std::string InputTrace::to_text() const {
    std::string out = "# nxin 1\n";
    char buf[256];
    for (const Entry& e : entries) {
        std::snprintf(buf, sizeof buf, "%llu 0x%llx %d %d %d %d %.9g %.9g\n", static_cast<unsigned long long>(e.frame),
                      static_cast<unsigned long long>(e.pad.buttons), static_cast<int>(e.pad.lx), static_cast<int>(e.pad.ly),
                      static_cast<int>(e.pad.rx), static_cast<int>(e.pad.ry), static_cast<double>(e.pad.mouse_dx),
                      static_cast<double>(e.pad.mouse_dy));
        out += buf;
    }
    if (end_frame != 0) {
        std::snprintf(buf, sizeof buf, "end %llu\n", static_cast<unsigned long long>(end_frame));
        out += buf;
    }
    return out;
}

bool InputTrace::from_text(const std::string& text, std::string* err) {
    std::string local_err;
    std::string& e = err ? *err : local_err;

    std::vector<Entry> ents;
    uint64_t end = 0;
    bool have_end = false;
    int end_line = 0;
    bool header_seen = false;

    auto fail = [&](int line, const std::string& msg) -> bool {
        e = "line " + std::to_string(line) + ": " + msg;
        return false;
    };
    auto field_err = [&](int line, const std::string& what, const std::string& tok, Num r) -> bool {
        if (r == Num::range) return fail(line, "out of range " + what + " '" + tok + "'");
        return fail(line, "non-numeric " + what + " '" + tok + "'");
    };

    int lineno = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t stop = nl == std::string::npos ? text.size() : nl;
        std::string line = text.substr(pos, stop - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        lineno++;

        std::vector<std::string> t = split_tokens(line);
        if (t.empty()) continue;  // blank

        if (!header_seen) {
            if (t.size() != 3 || t[0] != "#" || t[1] != "nxin" || t[2] != "1")
                return fail(lineno, "expected header '# nxin 1' as the first line");
            header_seen = true;
            continue;
        }
        if (t[0][0] == '#') continue;  // comment

        if (t[0] == "end") {
            if (have_end) return fail(lineno, "duplicate 'end' line");
            if (t.size() != 2) return fail(lineno, "'end' line needs exactly one frame number");
            uint64_t v = 0;
            if (Num r = parse_dec(t[1], v); r != Num::ok) return field_err(lineno, "end frame", t[1], r);
            if (!ents.empty() && v < ents.back().frame)
                return fail(lineno, "end frame " + t[1] + " is below last entry frame " + std::to_string(ents.back().frame));
            end = v;
            have_end = true;
            end_line = lineno;
            continue;
        }

        if (have_end) return fail(end_line, "'end' must be the last line");
        if (t.size() != 8) return fail(lineno, "expected 8 fields, got " + std::to_string(t.size()));

        Entry en{0, PadFrame{}};
        if (Num r = parse_dec(t[0], en.frame); r != Num::ok) return field_err(lineno, "frame", t[0], r);
        if (!ents.empty() && en.frame <= ents.back().frame)
            return fail(lineno, "frame " + t[0] + " is not above previous frame " + std::to_string(ents.back().frame));
        if (Num r = parse_hex(t[1], en.pad.buttons); r != Num::ok) return field_err(lineno, "buttons", t[1], r);

        const char* stick_names[4] = {"lx", "ly", "rx", "ry"};
        int16_t* sticks[4] = {&en.pad.lx, &en.pad.ly, &en.pad.rx, &en.pad.ry};
        for (int i = 0; i < 4; i++) {
            int v = 0;
            Num r = parse_stick(t[2 + i], v);
            if (r != Num::ok) return field_err(lineno, std::string("stick ") + stick_names[i], t[2 + i], r);
            *sticks[i] = static_cast<int16_t>(v);
        }
        if (!parse_float(t[6], en.pad.mouse_dx)) return fail(lineno, "non-numeric mouse-dx '" + t[6] + "'");
        if (!parse_float(t[7], en.pad.mouse_dy)) return fail(lineno, "non-numeric mouse-dy '" + t[7] + "'");
        ents.push_back(en);
    }

    if (!header_seen) return fail(1, "missing header '# nxin 1'");

    // Success: replace the contents and resynchronise the recording state with the loaded entries.
    entries = std::move(ents);
    end_frame = have_end ? end : 0;
    if (!entries.empty()) {
        last_recorded_ = entries.back().pad;
        last_frame_ = entries.back().frame;
        last_had_impulse_ = last_recorded_.mouse_dx != 0 || last_recorded_.mouse_dy != 0;
        have_last_ = true;
    } else {
        last_recorded_ = PadFrame{};
        last_frame_ = 0;
        last_had_impulse_ = false;
        have_last_ = false;
    }
    return true;
}

bool InputTrace::write(const std::string& path, std::string* err) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::string text = to_text();
    size_t n = std::fwrite(text.data(), 1, text.size(), f);
    int rc = std::fclose(f);
    if (n != text.size() || rc != 0) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    return true;
}

bool InputTrace::read(const std::string& path, std::string* err) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    std::string local_err;
    return from_text(text, err ? err : &local_err);
}

}  // namespace nxrt::tt
