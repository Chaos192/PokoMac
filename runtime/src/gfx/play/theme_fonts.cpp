// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// theme_fonts.h: the system CJK faces, found and mapped read-only.
#include "theme_fonts.h"

#include <SDL3/SDL_filesystem.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::cheats::ui::theme::fonts {
namespace {
namespace fs = std::filesystem;

fs::path utf8_path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

uint16_t be16(const unsigned char* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const unsigned char* p) { return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3]; }

// The offset of a table of the face `index` (a .ttc's face or a plain font's only one), 0 when absent.
size_t table(const unsigned char* d, size_t n, int index, const char tag[4]) {
    if (n < 12) return 0;
    size_t face = 0;
    if (std::memcmp(d, "ttcf", 4) == 0) {
        const uint32_t count = be32(d + 8);
        if (index < 0 || static_cast<uint32_t>(index) >= count || 12 + 4ull * count > n) return 0;
        face = be32(d + 12 + 4 * static_cast<size_t>(index));
    }
    if (face + 12 > n) return 0;
    const uint16_t tables = be16(d + face + 4);
    if (face + 12 + 16ull * tables > n) return 0;
    for (uint16_t i = 0; i < tables; ++i) {
        const unsigned char* rec = d + face + 12 + 16 * i;
        if (std::memcmp(rec, tag, 4) == 0) {
            const uint32_t off = be32(rec + 8);
            return off < n ? off : 0;
        }
    }
    return 0;
}

struct File { const unsigned char* data = nullptr; size_t size = 0; };

// Maps a file read-only for the life of the process (never unmapped: ImGui keeps pointers into it).
File map_file(const fs::path& p) {
    File f;
#if defined(_WIN32)
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return f;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 31)) {
        HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (m) {
            if (void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0)) {
                f.data = static_cast<const unsigned char*>(v);
                f.size = static_cast<size_t>(sz.QuadPart);
            }
            CloseHandle(m);   // the view keeps the mapping
        }
    }
    CloseHandle(h);
#else
    const int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return f;
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (1ll << 31)) {
        void* v = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (v != MAP_FAILED) {
            f.data = static_cast<const unsigned char*>(v);
            f.size = static_cast<size_t>(st.st_size);
        }
    }
    close(fd);
#endif
    return f;
}

// Where the faces are looked for, in fallback order: a file the player names, a CJK font a package put in fonts/ beside the program, then
// the system's (Simplified Chinese first: most cheat lists are written in it; then Japanese kana and kanji forms, Korean Hangul,
// Traditional Chinese for what is left).
std::vector<fs::path> candidates() {
    std::vector<fs::path> v;
    const std::string own = config_get_str("overlay", "cjk_font", "");
    if (!own.empty()) v.push_back(utf8_path(own));
    if (const char* base = SDL_GetBasePath()) {
        const fs::path fonts = utf8_path(base) / "fonts";
        for (const char* n : {"NotoSansCJK-Regular.ttc", "NotoSansCJKsc-Regular.otf", "NotoSansSC-Regular.otf", "NotoSansJP-Regular.otf", "NotoSansKR-Regular.otf"})
            v.push_back(fonts / n);
    }
#if defined(_WIN32)
    wchar_t win[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    const fs::path dir = (n > 0 && n < MAX_PATH ? fs::path(win) : fs::path(L"C:\\Windows")) / L"Fonts";
    for (const char* f : {"msyh.ttc", "YuGothR.ttc", "msgothic.ttc", "malgun.ttf", "msjh.ttc", "simsun.ttc"}) v.push_back(dir / f);
#elif defined(__APPLE__)
    const fs::path dir = "/System/Library/Fonts";
    v.push_back(dir / "PingFang.ttc");
    v.push_back(dir / "Hiragino Sans GB.ttc");
    v.push_back(dir / utf8_path("\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc"));   // Hiragino Kaku Gothic W3
    v.push_back(dir / "AppleSDGothicNeo.ttc");
    v.push_back(dir / "STHeiti Light.ttc");
    v.push_back("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
#elif defined(__ANDROID__)
    v.push_back("/system/fonts/NotoSansCJK-Regular.ttc");
    v.push_back("/system/fonts/DroidSansFallback.ttf");
#else
    for (const char* f : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                          "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"})
        v.emplace_back(f);
#endif
    return v;
}

std::vector<Mapped> g_fonts;
std::string g_names = "none";
std::once_flag g_once;

void find() {
    if (!config_get_bool("overlay", "cjk", true)) {
        g_names = "off ([overlay] cjk = false)";
        return;
    }
    std::string names;
    std::error_code ec;
    for (const fs::path& p : candidates()) {
        if (g_fonts.size() >= 4) break;           // four faces cover the scripts; more would only cost lookups
        if (!fs::is_regular_file(p, ec)) continue;
        const File f = map_file(p);
        if (!f.data) continue;
        const float em = em_per_height(f.data, f.size, 0);
        if (em <= 0.f) continue;                  // not a font stb_truetype reads (or a damaged one): left out, still mapped
        Mapped m;
        m.data = f.data;
        m.size = f.size;
        m.index = 0;
        m.em = em;
        std::snprintf(m.name, sizeof m.name, "%s", path_utf8(p.filename()).c_str());
        g_fonts.push_back(m);
        if (!names.empty()) names += ", ";
        names += m.name;
    }
    if (!names.empty()) g_names = names;
    NXLOG_I("overlay", "CJK fallback fonts: %s", g_names.c_str());
}
}  // namespace

float em_per_height(const unsigned char* d, size_t n, int index) {
    if (!d) return 0.f;
    const size_t head = table(d, n, index, "head"), hhea = table(d, n, index, "hhea");
    if (!head || !hhea || head + 20 > n || hhea + 8 > n) return 0.f;
    const float upm = be16(d + head + 18);
    const float asc = static_cast<int16_t>(be16(d + hhea + 4)), desc = static_cast<int16_t>(be16(d + hhea + 6));
    if (upm <= 0.f || asc - desc <= 0.f) return 0.f;
    return upm / (asc - desc);
}

const std::vector<Mapped>& system_cjk() {
    std::call_once(g_once, find);
    return g_fonts;
}

const char* system_cjk_names() {
    system_cjk();
    return g_names.c_str();
}

}  // namespace nxrt::cheats::ui::theme::fonts
