// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// pl:u / pl:s (nn::pl::sharedresource::ISharedFontManager): the six shared system fonts in one 0x1100000-byte shared memory, Switch 2's
// built-in font memory (commands 2000 / 2010, SDK 21 nn::pl::GetBuiltInFontData), and the button icons of font 5, which follow the
// controller the player holds (nxrt/shared_font.h). register_pl also installs the font-engine HLE (font_engine.cpp).
// Command ids, SharedFontType order and the LoadState values: https://switchbrew.org/wiki/Shared_Database_services. The memory layout
// a console has (each font behind an 8-byte header: magic 0x18029A7F, then the size XOR 0x49621806 stored big-endian) and the BFTTF
// encoding of the system's font files (every u32 XOR 0x06186249) as Ryujinx (MIT) src/Ryujinx.HLE/HOS/Services/Sdb/Pl/SharedFontManager.cs
// describes them.
//
// Where each font comes from, slot by slot: the console's own font file in [pl] font_dir (FontStandard.bfttf, ...), else for slot 5 the
// stand-in button-icon font (pl_ext_font.cpp), else DejaVu Sans (the first DejaVuSans.ttf found in [pl] font_dir, <exe folder>/fonts,
// ./assets/fonts or the exe folder; not copied into the sources). Everything is built at the first command that needs it and kept until
// pl_reset.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/shared_font.h"
#include "pl_ext_font.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

constexpr uint64_t kFontMemorySize = 0x1100000;
constexpr uint32_t kSlots = 6;                 // nn::pl::SharedFontType: Standard .. NintendoExtension
constexpr uint32_t kExtensionSlot = 5;
constexpr uint32_t kHeaderMagic = 0x18029A7F;   // in the memory, before each font
constexpr uint32_t kSizeKey = 0x49621806;       // the size beside it: XOR this, big-endian
constexpr uint32_t kFileKey = 0x06186249;       // a .bfttf file: every u32 XOR this
constexpr uint64_t kPage = 0x1000;

const char* const kDumpNames[kSlots] = {
    "FontStandard.bfttf",     "FontChineseSimplified.bfttf", "FontChineseSimplifiedExtension.bfttf",
    "FontChineseTraditional.bfttf", "FontKorean.bfttf",    "FontNintendoExtended.bfttf",
};

// ---- the button face (survives pl_reset) ------------------------------------------------------------------------------------
using FaceSource = ButtonFace (*)();   // shared_font_set_face_provider's function
struct Face {
    std::mutex mu;
    ButtonFace face;
    bool set = false;
    FaceSource provider = nullptr;
};
Face& face_state() {
    static Face f;
    return f;
}
// The face font 5 is made with when it is built: the provider's, asked once, when no face was set before.
ButtonFace start_face() {
    Face& st = face_state();
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.provider != nullptr && !st.set) {
        st.face = (*st.provider)();
        st.set = true;
    }
    return st.face;
}

// ---- the font memory ----------------------------------------------------------------------------------------------------------
struct Slot {
    uint32_t offset = 0;   // of the font data (the header is the 8 bytes before)
    uint32_t size = 0;
};
struct Fonts {
    std::unique_ptr<ServiceSharedMemory> mem;
    std::array<Slot, kSlots> slots{};
    bool extension_generated = false;   // slot 5 is the stand-in (shared_font_set_face may rewrite it)
    std::mutex sub_mu;                  // the built-in memory and the requested flags
    std::array<bool, kSlots> requested{};
    std::unique_ptr<ServiceSharedMemory> builtin;
    std::array<Slot, kSlots> builtin_slots{};
};
struct FontState {
    std::mutex mu;
    std::unique_ptr<Fonts> fonts;   // null until built (and after pl_reset)
};
FontState& font_state() {
    static FontState s;
    return s;
}
bool g_warned_no_font = false;      // once per process

std::string exe_folder() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    char b[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, b, sizeof b);
    if (n == 0 || n >= sizeof b) return {};
    return fs::path(std::string(b, n)).parent_path().string();
#elif defined(__APPLE__)
    char b[4096];
    uint32_t n = sizeof b;
    if (_NSGetExecutablePath(b, &n) != 0) return {};
    std::error_code ec;
    fs::path p = fs::canonical(fs::path(b), ec);
    if (ec) p = fs::path(b);
    return p.parent_path().string();
#elif defined(__linux__) && !defined(__ANDROID__)
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.parent_path().string();
#else
    return {};
#endif
}

// A whole regular file, empty when it is missing, unreadable or empty.
std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return {};
    std::vector<uint8_t> v;
    if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
        uint8_t buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
        std::fclose(f);
    }
    return v;
}

uint32_t rd32le(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// A system font file (.bfttf) decoded to its TrueType bytes; false when it is not one.
bool decode_bfttf(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    if (in.size() < 8 || (rd32le(in.data()) ^ kFileKey) != kHeaderMagic) return false;
    const size_t words = (in.size() - 8) / 4;
    out.resize(words * sizeof(uint32_t));
    for (size_t i = 0; i < words; ++i) {
        const uint32_t w = rd32le(in.data() + 8 + 4 * i) ^ kFileKey;
        std::memcpy(out.data() + 4 * i, &w, 4);
    }
    return true;
}

// DejaVu Sans, the font every slot without a font of its own gets.
std::vector<uint8_t> fallback_font(const std::string& font_dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> where;
    if (!font_dir.empty()) where.push_back(fs::path(font_dir) / "DejaVuSans.ttf");
    const std::string exe = exe_folder();
    if (!exe.empty()) where.push_back(fs::path(exe) / "fonts" / "DejaVuSans.ttf");
    where.push_back(fs::path("assets") / "fonts" / "DejaVuSans.ttf");
    if (!exe.empty()) where.push_back(fs::path(exe) / "DejaVuSans.ttf");
    for (const fs::path& p : where) {
        std::vector<uint8_t> v = read_file(p);
        if (!v.empty()) {
            NXLOG_I("pl", "fallback font: %s (%zu bytes)", p.string().c_str(), v.size());
            return v;
        }
    }
    return {};
}

void write_header(uint8_t* at, uint32_t size) {
    const uint32_t magic = kHeaderMagic;
    std::memcpy(at, &magic, 4);
    const uint32_t s = size ^ kSizeKey;
    const uint8_t be[4] = {uint8_t(s >> 24), uint8_t(s >> 16), uint8_t(s >> 8), uint8_t(s)};
    std::memcpy(at + 4, be, 4);
}

// Builds the font memory (caller holds the font state's lock).
std::unique_ptr<Fonts> build_fonts() {
    namespace fs = std::filesystem;
    auto f = std::make_unique<Fonts>();
    f->mem = std::make_unique<ServiceSharedMemory>(kFontMemorySize, "pl");
    uint8_t* base = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(f->mem->host()));
    if (!base || f->mem->size() != kFontMemorySize) {
        NXLOG_E("pl", "the shared font memory (0x%llx bytes) could not be made: every font is empty",
                static_cast<unsigned long long>(kFontMemorySize));
        return f;
    }
    std::memset(base, 0, kFontMemorySize);
    const std::string font_dir = config_get_str("pl", "font_dir", "");
    const std::vector<uint8_t> fallback = fallback_font(font_dir);
    uint64_t cursor = 0;
    bool any = false;
    for (uint32_t i = 0; i < kSlots; ++i) {
        std::vector<uint8_t> data;
        const char* source = "none";
        std::string style;
        if (!font_dir.empty()) {
            const fs::path p = fs::path(font_dir) / kDumpNames[i];
            const std::vector<uint8_t> file = read_file(p);
            if (!file.empty()) {
                if (decode_bfttf(file, data)) source = "dump";
                else NXLOG_W("pl", "%s is not a system font file (.bfttf): not used", p.string().c_str());
            }
        }
        if (data.empty() && i == kExtensionSlot) {
            const ButtonFace face = start_face();
            data = make_extension_font(face);
            f->extension_generated = true;
            style = "button icons (" + face.style + ")";
            source = style.c_str();
        }
        if (data.empty() && !fallback.empty()) {
            data = fallback;
            source = "fallback";
        }
        if (cursor + 8 + data.size() > kFontMemorySize) {
            NXLOG_W("pl", "font %u (%zu bytes) does not fit in the shared font memory: left empty", i, data.size());
            data.clear();
            source = "none";
        }
        if (cursor + 8 <= kFontMemorySize) write_header(base + cursor, static_cast<uint32_t>(data.size()));
        f->slots[i].offset = static_cast<uint32_t>(cursor + 8);
        f->slots[i].size = static_cast<uint32_t>(data.size());
        if (!data.empty()) std::memcpy(base + cursor + 8, data.data(), data.size());
        NXLOG_I("pl", "font %u: %s, %zu bytes at 0x%x", i, source, data.size(), f->slots[i].offset);
        any |= !data.empty();
        cursor += 8 + data.size();
    }
    if (!any && !g_warned_no_font) {
        g_warned_no_font = true;
        NXLOG_W("pl", "no TTF font found (set [pl] font_dir, or put DejaVuSans.ttf in fonts/ beside the exe): every shared font is empty");
    }
    return f;
}

// The font memory, built at the first need. Callers hold the font state's lock.
Fonts& fonts_locked() {
    FontState& s = font_state();
    if (!s.fonts) s.fonts = build_fonts();
    return *s.fonts;
}

// Switch 2's built-in font memory: each font's TrueType bytes on its own page boundary (caller holds both locks).
void build_builtin(Fonts& f) {
    if (f.builtin) return;
    uint64_t at = 0;
    for (uint32_t i = 0; i < kSlots; ++i) {
        f.builtin_slots[i] = Slot{static_cast<uint32_t>(at), f.slots[i].size};
        at = (at + f.slots[i].size + kPage - 1) & ~(kPage - 1);
    }
    const uint64_t size = at < kPage ? kPage : at;
    f.builtin = std::make_unique<ServiceSharedMemory>(size, "pl builtin");
    uint8_t* dst = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(f.builtin->host()));
    const uint8_t* src = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(f.mem ? f.mem->host() : 0));
    if (dst) {
        std::memset(dst, 0, f.builtin->size());
        if (src)
            for (uint32_t i = 0; i < kSlots; ++i)
                if (f.slots[i].size) std::memcpy(dst + f.builtin_slots[i].offset, src + f.slots[i].offset, f.slots[i].size);
    }
    NXLOG_I("pl", "built-in font memory: 0x%llx bytes", static_cast<unsigned long long>(size));
}

Slot slot_of(uint32_t type, const char* what) {
    FontState& s = font_state();
    std::lock_guard<std::mutex> lk(s.mu);
    Fonts& f = fonts_locked();
    if (type >= kSlots) {
        NXLOG_W("pl", "%s: unknown font type %u", what, type);
        return Slot{};
    }
    return f.slots[type];
}

// GetSharedFontInOrderOfPriority: every font in type order, whatever the language.
uint32_t font_order(ipc::Request& req, ipc::Response& rsp) {
    NXLOG_D("pl", "GetSharedFontInOrderOfPriority(language code 0x%016llx)", static_cast<unsigned long long>(req.arg<uint64_t>(0)));
    std::array<Slot, kSlots> slots;
    {
        FontState& s = font_state();
        std::lock_guard<std::mutex> lk(s.mu);
        slots = fonts_locked().slots;
    }
    const ipc::Buffer types = req.out_buf(0), offsets = req.out_buf(1), sizes = req.out_buf(2);
    uint64_t n = kSlots;
    for (const ipc::Buffer& b : {types, offsets, sizes}) n = std::min<uint64_t>(n, b.addr ? b.size / 4 : 0);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t t = i;
        std::memcpy(buf_mut(types) + 4 * i, &t, 4);
        std::memcpy(buf_mut(offsets) + 4 * i, &slots[i].offset, 4);
        std::memcpy(buf_mut(sizes) + 4 * i, &slots[i].size, 4);
    }
    rsp.push<uint8_t>(1);   // loaded: exactly 1 (newer SDKs reject anything else)
    rsp.push<uint8_t>(0);
    rsp.push<uint16_t>(0);
    rsp.push<uint32_t>(static_cast<uint32_t>(n));
    return 0;
}

std::shared_ptr<ipc::Service> make_font_manager(const char* name) {
    return make_iface(name, [](ipc::Service& s) {
        s.add_cmd(0, "RequestLoad", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint32_t type = req.arg<uint32_t>(0);
            FontState& st = font_state();
            std::lock_guard<std::mutex> lk(st.mu);
            Fonts& f = fonts_locked();
            if (type < kSlots) {
                std::lock_guard<std::mutex> lk2(f.sub_mu);
                f.requested[type] = true;
            } else {
                NXLOG_W("pl", "RequestLoad: unknown font type %u", type);
            }
            return 0;
        });
        s.add_cmd(1, "GetLoadState", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            NXLOG_D("pl", "GetLoadState(%u)", req.arg<uint32_t>(0));
            rsp.push<uint32_t>(1);   // Loaded: every font is there from the start
            return 0;
        });
        s.add_cmd(2, "GetSize", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(slot_of(req.arg<uint32_t>(0), "GetSize").size);
            return 0;
        });
        s.add_cmd(3, "GetSharedMemoryAddressOffset", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(slot_of(req.arg<uint32_t>(0), "GetSharedMemoryAddressOffset").offset);
            return 0;
        });
        s.add_cmd(4, "GetSharedMemoryNativeHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            FontState& st = font_state();
            std::lock_guard<std::mutex> lk(st.mu);
            rsp.out_copy.push_back(fonts_locked().mem->new_handle());
            return 0;
        });
        s.add_cmd(5, "GetSharedFontInOrderOfPriority", font_order);
        s.add_cmd(6, "GetSharedFontInOrderOfPriorityForSystem", font_order);
        s.add_cmd(2000, "GetBuiltInFontSharedMemory", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            FontState& st = font_state();
            std::lock_guard<std::mutex> lk(st.mu);
            Fonts& f = fonts_locked();
            std::lock_guard<std::mutex> lk2(f.sub_mu);
            build_builtin(f);
            rsp.push<uint32_t>(static_cast<uint32_t>(f.builtin->size()));
            rsp.out_copy.push_back(f.builtin->new_handle());
            return 0;
        });
        s.add_cmd(2010, "GetBuiltInFontData", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t type = req.arg<uint32_t>(0) & 0xFF;   // u8 type, then a flag byte
            FontState& st = font_state();
            std::lock_guard<std::mutex> lk(st.mu);
            Fonts& f = fonts_locked();
            std::lock_guard<std::mutex> lk2(f.sub_mu);
            build_builtin(f);
            const bool found = type < kSlots && f.slots[type].size != 0;
            rsp.push<uint32_t>(found ? 1u : 0u);
            rsp.push<uint32_t>(0);
            rsp.push<uint32_t>(found ? f.builtin_slots[type].offset : 0u);
            rsp.push<uint32_t>(found ? f.builtin_slots[type].size : 0u);
            return 0;
        });
    });
}

}  // namespace

void shared_font_set_face_provider(ButtonFace (*fn)()) {
    Face& f = face_state();
    std::lock_guard<std::mutex> lk(f.mu);
    f.provider = fn;
}

ButtonFace shared_font_face() {
    Face& f = face_state();
    std::lock_guard<std::mutex> lk(f.mu);
    return f.face;
}

void shared_font_set_face(const ButtonFace& face) {
    {
        Face& cur = face_state();
        std::lock_guard<std::mutex> lk(cur.mu);
        if (cur.set && cur.face == face) return;   // the same face again: nothing to rebuild
        cur.face = face;
        cur.set = true;
    }
    FontState& st = font_state();
    std::lock_guard<std::mutex> lk(st.mu);
    Fonts* f = st.fonts.get();
    if (!f || !f->extension_generated) {
        NXLOG_I("pl", "button icons: %s", face.style.c_str());
        return;
    }
    const std::vector<uint8_t> font = make_extension_font(face);
    uint8_t* base = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(f->mem ? f->mem->host() : 0));
    const Slot slot = f->slots[kExtensionSlot];
    if (!base || font.size() != slot.size) {
        NXLOG_W("pl", "button icons %s: font 5 cannot be rewritten in place (%zu bytes, the slot has %u)", face.style.c_str(), font.size(),
                slot.size);
        return;
    }
    std::memcpy(base + slot.offset, font.data(), font.size());
    std::lock_guard<std::mutex> lk2(f->sub_mu);
    if (f->builtin) {
        uint8_t* b = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(f->builtin->host()));
        if (b) std::memcpy(b + f->builtin_slots[kExtensionSlot].offset, font.data(), font.size());
    }
    NXLOG_I("pl", "button icons: %s (font 5 rewritten in place)", face.style.c_str());
}

void register_pl() {
    add("pl:u", [] { return make_font_manager("pl:u"); });
    add("pl:s", [] { return make_font_manager("pl:s"); });
    install_font_engine_hle();
}

void pl_reset() {
    FontState& s = font_state();
    std::lock_guard<std::mutex> lk(s.mu);
    s.fonts.reset();
}

}  // namespace nxrt::services
