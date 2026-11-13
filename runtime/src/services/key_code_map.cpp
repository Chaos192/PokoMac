// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// nn::kpr::KeyCodeMap built from a keyboard layout (include/nxrt/key_code_map.h has the format and where it comes from).
#include "nxrt/key_code_map.h"

#include <array>
#include <cstring>
#include <mutex>

namespace nxrt::hid {
namespace {

constexpr uint32_t kModeDefault = 1u;            // +0x00: only KeyCodeMapMode 0 (no kana, Hangul, Cyrillic or Zhuyin layer, no AltGr)
constexpr uint32_t kWordsPerEntry = 3;           // flags, normal, shifted
constexpr uint32_t kEntriesAt = 0x40;
constexpr uint16_t kKindKey = 0x1000;            // an ordinary key
constexpr uint16_t kKindKeypad = 0x2000;         // a keypad key: NumLock gives its digit
constexpr uint16_t kLockApplies = 0x0001;        // CapsLock (ordinary key) / NumLock (keypad key) swaps the two columns

// Usage ids whose characters come from the layout: the letter, digit and punctuation block, and the extra key of ISO keyboards.
bool from_layout(int u) { return (u >= 0x04 && u <= 0x38) || u == 0x64; }

// CapsLock acts on a key whose shifted character is its normal one's capital: Latin (ASCII and Latin-1), Greek and Cyrillic.
bool cased_pair(char16_t n, char16_t s) {
    if (n >= u'a' && n <= u'z') return s == n - 0x20;
    if (n >= 0xE0 && n <= 0xFE && n != 0xF7) return s == n - 0x20;
    if (n >= 0x3B1 && n <= 0x3C9 && n != 0x3C2) return s == n - 0x20;
    if (n >= 0x430 && n <= 0x44F) return s == n - 0x20;
    if (n >= 0x450 && n <= 0x45F) return s == n - 0x50;
    return false;
}

std::array<KeyChars, 256> make_us() {
    std::array<KeyChars, 256> t{};
    for (int i = 0; i < 26; ++i) t[0x04 + i] = {static_cast<char16_t>(u'a' + i), static_cast<char16_t>(u'A' + i)};
    const char16_t digits[] = u"1234567890", shifted[] = u"!@#$%^&*()";
    for (int i = 0; i < 10; ++i) t[0x1E + i] = {digits[i], shifted[i]};
    t[0x2C] = {u' ', u' '};
    t[0x2D] = {u'-', u'_'};
    t[0x2E] = {u'=', u'+'};
    t[0x2F] = {u'[', u'{'};
    t[0x30] = {u']', u'}'};
    t[0x31] = {u'\\', u'|'};
    t[0x32] = {u'\\', u'|'};   // the ISO key beside Enter: the backslash key's place on a US layout
    t[0x33] = {u';', u':'};
    t[0x34] = {u'\'', u'"'};
    t[0x35] = {u'`', u'~'};
    t[0x36] = {u',', u'<'};
    t[0x37] = {u'.', u'>'};
    t[0x38] = {u'/', u'?'};
    t[0x64] = {u'\\', u'|'};   // the ISO key beside the left Shift
    return t;
}

const std::array<KeyChars, 256>& us_table() {
    static const std::array<KeyChars, 256> t = make_us();
    return t;
}

std::mutex g_mu;
bool g_have_host = false;
std::array<KeyChars, 256> g_host{};

void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint16_t get16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

}  // namespace

const KeyChars* us_key_chars() { return us_table().data(); }

void set_host_key_chars(const KeyChars* chars) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_have_host = chars != nullptr;
    if (chars) std::memcpy(g_host.data(), chars, sizeof g_host);
}

void build_key_code_map(const KeyChars* chars, uint8_t out[kKeyCodeMapBytes]) {
    std::memset(out, 0, kKeyCodeMapBytes);
    put32(out + 0x0, kModeDefault);
    put32(out + 0x4, kWordsPerEntry);
    put32(out + 0x8, kKeyCodeMapEntries);
    put32(out + 0xC, 0);
    auto entry = [&](int usage, uint16_t flags, char16_t n, char16_t s) {
        uint8_t* e = out + kEntriesAt + static_cast<size_t>(usage - kKeyCodeMapFirstUsage) * kWordsPerEntry * 2;
        put16(e, flags);
        put16(e + 2, n);
        put16(e + 4, s);
    };
    for (int u = kKeyCodeMapFirstUsage; u < kKeyCodeMapFirstUsage + kKeyCodeMapEntries; ++u) {
        if (!from_layout(u)) continue;
        const KeyChars& k = chars[u];
        if (!k.normal && !k.shift) continue;
        entry(u, static_cast<uint16_t>(kKindKey | (cased_pair(k.normal, k.shift) ? kLockApplies : 0)), k.normal, k.shift);
    }
    // the keypad: its operators print alike with and without Shift; its digits and point come with NumLock (the console's maps alike)
    const char16_t ops[] = u"/*-+";
    for (int i = 0; i < 4; ++i) entry(0x54 + i, kKindKey, ops[i], ops[i]);
    const char16_t pad[] = u"1234567890.";
    for (int i = 0; i < 11; ++i) entry(0x59 + i, kKindKeypad | kLockApplies, 0, pad[i]);
    entry(0x67, kKindKey, u'=', u'=');
}

void current_key_code_map(uint8_t out[kKeyCodeMapBytes]) {
    std::array<KeyChars, 256> t;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        t = g_have_host ? g_host : us_table();
    }
    build_key_code_map(t.data(), out);
}

char16_t key_code_of(const uint8_t map[kKeyCodeMapBytes], int usage, uint32_t mods) {
    const int idx = usage - kKeyCodeMapFirstUsage;
    if (idx < 0 || idx >= static_cast<int>(get32(map + 8))) return 0;
    const uint32_t modes = get32(map), words = get32(map + 4);
    const uint8_t* e = map + kEntriesAt + static_cast<size_t>(idx) * words * 2;
    const uint16_t flags = get16(e);
    const uint32_t kind = flags >> 12;
    const bool shift = mods & 2u, caps = mods & 0x100u, num = mods & 0x400u;
    if (kind == 1 && (modes & 0x1000000u) && ((mods & 5u) == 5u || (mods & 8u))) {   // AltGr: Control+LeftAlt, or RightAlt
        const uint32_t at = get32(map + 0xC);
        const char16_t c = get16(e + at * 2), cs = (modes & 0x2000000u) ? get16(e + at * 2 + 2) : 0;
        const char16_t plain = shift ? cs : c, swapped = shift ? c : cs;
        return (flags & 2u) && caps ? swapped : plain;
    }
    const uint32_t lock = (flags & kLockApplies) ? kind : 0;
    const char16_t n = get16(e + 2), s = get16(e + 4);
    if (lock == 2) return num && !shift ? s : n;
    const char16_t plain = shift ? s : n, swapped = shift ? n : s;
    return lock == 1 && caps ? swapped : plain;
}

}  // namespace nxrt::hid
