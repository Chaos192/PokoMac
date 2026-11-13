// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// .nup package reader (nxrt/nup.h). Layout from the format's SPEC.md (github.com/a55uka/nup): a 0x80-byte superblock, the table of
// contents at toc_offset (package records 0x40, dir records 0x18, file records 0x50, a u32 children array, the string pool), file
// extents at absolute offsets. The package is a file the player supplies: every index, name and extent is bounds checked.
#include "nxrt/nup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef NXRT_HAVE_ZSTD
#include <zstd.h>
#endif

namespace nxrt::nup {
namespace {

constexpr uint32_t kNoDir = 0xFFFFFFFFu;
constexpr uint8_t kKindBase = 0, kKindUpdate = 1, kKindAddOn = 2, kKindAddOnUpdate = 3;
constexpr uint64_t kMaxToc = 1ull << 30;
constexpr int kMaxDepth = 64;

std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

bool fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
    return false;
}

uint32_t u32(const std::vector<uint8_t>& b, uint64_t o) {
    uint32_t v;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}
uint64_t u64(const std::vector<uint8_t>& b, uint64_t o) {
    uint64_t v;
    std::memcpy(&v, b.data() + o, 8);
    return v;
}

struct Toc {
    std::vector<uint8_t> b;
    uint64_t dirs = 0, files = 0, dir_at = 0, file_at = 0, child_at = 0, child_n = 0, pool_at = 0, pool_n = 0, file_size = 0;
};

bool name_of(const Toc& t, uint64_t rec, std::string& out) {
    const uint64_t off = u32(t.b, rec), len = u32(t.b, rec + 4);
    if (off > t.pool_n || len > t.pool_n - off || len == 0) return false;
    out.assign(reinterpret_cast<const char*>(t.b.data() + t.pool_at + off), len);
    return out != "." && out != ".." && out.find_first_of("/\\:") == std::string::npos && out.find('\0') == std::string::npos;
}

// Lists the files under dir (paths relative to it).
bool walk(const Toc& t, uint32_t dir, const std::string& prefix, int depth, std::vector<File>& out, std::string* err) {
    if (depth > kMaxDepth || dir >= t.dirs) return fail(err, "the .nup folder tree is invalid");
    const uint64_t rec = t.dir_at + 0x18ull * dir;
    const uint64_t first = u32(t.b, rec + 8), count = u32(t.b, rec + 12);
    if (first > t.child_n || count > t.child_n - first) return fail(err, "the .nup folder listing is out of bounds");
    for (uint64_t s = first; s < first + count; s++) {
        const uint32_t id = u32(t.b, t.child_at + 4 * s);
        std::string name;
        if (id < t.dirs) {
            if (!name_of(t, t.dir_at + 0x18ull * id, name)) return fail(err, "the .nup holds an invalid folder name");
            if (!walk(t, id, prefix + name + "/", depth + 1, out, err)) return false;
            continue;
        }
        if (id - t.dirs >= t.files) return fail(err, "the .nup refers to a file that does not exist");
        const uint64_t fr = t.file_at + 0x50ull * (id - t.dirs);
        if (!name_of(t, fr, name)) return fail(err, "the .nup holds an invalid file name");
        File f;
        f.path = prefix + name;
        f.size = u64(t.b, fr + 8);
        f.offset = u64(t.b, fr + 0x10);
        f.stored = u64(t.b, fr + 0x18);
        f.codec = t.b[fr + 0x40];
        if (f.codec != kCodecStored && f.codec != kCodecZstd)
            return fail(err, f.path + ": file codec " + std::to_string(f.codec) + " in the .nup is not one this build knows (0 stored, 1 zstd)");
        if (f.size > 0 && ((f.codec == kCodecStored && f.stored != f.size) || f.stored == 0 || f.offset > t.file_size ||
                           f.stored > t.file_size - f.offset))
            return fail(err, f.path + " points outside the .nup file");
        out.push_back(std::move(f));
    }
    return true;
}

}  // namespace

bool is_nup(const std::string& path) {
    std::ifstream f(fs_path(path), std::ios::binary);
    char m[4] = {};
    return f.read(m, 4) && std::memcmp(m, "NUP\x01", 4) == 0;
}

namespace {

// Reads the superblock and the table of contents into t; npkg = the number of package records.
bool read_toc(const std::string& path, Toc& t, uint64_t& npkg, std::string* err) {
    std::ifstream f(fs_path(path), std::ios::binary | std::ios::ate);
    if (!f) return fail(err, "cannot open " + path);
    const uint64_t real = static_cast<uint64_t>(f.tellg());
    std::vector<uint8_t> sb(0x80);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(sb.data()), 0x80) || std::memcmp(sb.data(), "NUP\x01", 4) != 0) return fail(err, path + " is not a .nup package");
    const uint64_t toc_off = u64(sb, 0x08), toc_size = u64(sb, 0x10);
    t.file_size = u64(sb, 0x40);
    if (u32(sb, 0x4C) != 0) return fail(err, path + " uses .nup features this build does not know");
    if (t.file_size != real || toc_size < 0x30 || toc_size > kMaxToc || toc_off > real || toc_size > real - toc_off)
        return fail(err, path + " is damaged or incomplete (size mismatch)");
    t.b.resize(toc_size);
    f.seekg(static_cast<std::streamoff>(toc_off));
    if (!f.read(reinterpret_cast<char*>(t.b.data()), static_cast<std::streamsize>(toc_size)) || std::memcmp(t.b.data(), "TOC\x01", 4) != 0)
        return fail(err, path + ": the table of contents is invalid");
    npkg = u32(t.b, 4);
    t.dirs = u64(t.b, 8);
    t.files = u64(t.b, 0x10);
    t.pool_at = u64(t.b, 0x18);
    t.pool_n = u64(t.b, 0x20);
    if (npkg > toc_size / 0x40 || t.dirs > toc_size / 0x18 || t.files > toc_size / 0x50) return fail(err, path + ": the table of contents is invalid");
    t.dir_at = 0x30 + 0x40 * npkg;
    t.file_at = t.dir_at + 0x18 * t.dirs;
    t.child_at = t.file_at + 0x50 * t.files;
    if (t.child_at > t.pool_at || (t.pool_at - t.child_at) % 4 || t.pool_at > toc_size || t.pool_n > toc_size - t.pool_at)
        return fail(err, path + ": the table of contents is invalid");
    t.child_n = (t.pool_at - t.child_at) / 4;
    return true;
}

// The files of `top` over those of `base` (a RomFS that is not standalone holds only the files it changes).
bool layered(const Toc& t, uint32_t base, uint32_t top, std::vector<File>& out, std::string* err) {
    std::vector<File> lower, upper;
    if (!walk(t, base, "", 0, lower, err) || !walk(t, top, "", 0, upper, err)) return false;
    std::unordered_map<std::string, size_t> at;
    at.reserve(lower.size());
    for (File& f : lower) {
        at.emplace(f.path, out.size());
        out.push_back(std::move(f));
    }
    for (File& f : upper) {
        auto it = at.find(f.path);
        if (it != at.end()) out[it->second] = std::move(f);
        else out.push_back(std::move(f));
    }
    return true;
}

}  // namespace

bool open_addons(const std::string& path, std::vector<AddOn>& out, std::string* err) {
    out.clear();
    Toc t;
    uint64_t pkgs = 0;
    if (!read_toc(path, t, pkgs, err)) return false;
    struct Pick { uint32_t base = kNoDir, top = kNoDir, version = 0; bool top_standalone = true; int64_t rank = -1; };
    std::unordered_map<uint64_t, Pick> by_title;
    for (uint64_t i = 0; i < pkgs; i++) {
        const uint64_t r = 0x30 + 0x40 * i;
        const uint8_t kind = t.b[r + 0x18];
        if (kind != kKindAddOn && kind != kKindAddOnUpdate) continue;
        const uint32_t rom = u32(t.b, r + 0x10);
        if (rom == kNoDir) continue;
        Pick& p = by_title[u64(t.b, r)];
        const int64_t rank = kind == kKindAddOn ? 0 : 1 + static_cast<int64_t>(u32(t.b, r + 0x14));
        if (kind == kKindAddOn) p.base = rom;
        if (rank > p.rank) {
            p.rank = rank, p.top = rom, p.version = u32(t.b, r + 0x14);
            p.top_standalone = kind == kKindAddOn || (t.b[r + 0x19] & 1) != 0;
        }
    }
    for (const auto& [title, p] : by_title) {
        AddOn a;
        a.title_id = title;
        a.version = p.version;
        const bool ok = (!p.top_standalone && p.base != kNoDir && p.base != p.top) ? layered(t, p.base, p.top, a.romfs, err)
                                                                                    : walk(t, p.top, "", 0, a.romfs, err);
        if (!ok) return false;
        out.push_back(std::move(a));
    }
    return true;
}

std::string addon_romfs_name(const std::string& package, uint64_t title_id) {
    char b[24];
    std::snprintf(b, sizeof b, "#aoc=%016llX", static_cast<unsigned long long>(title_id));
    return package + b;
}

bool split_addon_romfs_name(const std::string& name, std::string& package, uint64_t& title_id) {
    const size_t at = name.rfind("#aoc=");
    if (at == std::string::npos || name.size() != at + 5 + 16) return false;
    package = name.substr(0, at);
    title_id = std::strtoull(name.c_str() + at + 5, nullptr, 16);
    return true;
}

bool open(const std::string& path, Content& out, std::string* err) {
    out = Content{};
    Toc t;
    uint64_t pkgs = 0;
    if (!read_toc(path, t, pkgs, err)) return false;

    // The package to run: the highest update, else the base. ExeFS and RomFS may come from different packages (an update without one).
    int64_t best_exe = -1, best_rom = -1;
    uint32_t exe_dir = kNoDir, rom_dir = kNoDir, base_rom = kNoDir;
    bool rom_standalone = true;
    for (uint64_t i = 0; i < pkgs; i++) {
        const uint64_t r = 0x30 + 0x40 * i;
        const uint8_t kind = t.b[r + 0x18];
        if (kind != kKindBase && kind != kKindUpdate) continue;
        const int64_t rank = kind == kKindBase ? 0 : 1 + static_cast<int64_t>(u32(t.b, r + 0x14));
        if (kind == kKindBase && u32(t.b, r + 0x10) != kNoDir) base_rom = u32(t.b, r + 0x10);
        if (u32(t.b, r + 0x0C) != kNoDir && rank > best_exe) best_exe = rank, exe_dir = u32(t.b, r + 0x0C);
        if (u32(t.b, r + 0x10) != kNoDir && rank > best_rom) {
            best_rom = rank, rom_dir = u32(t.b, r + 0x10), out.version = u32(t.b, r + 0x14);
            rom_standalone = kind == kKindBase || (t.b[r + 0x19] & 1) != 0;   // package flag bit 0: the RomFS is complete on its own
        }
    }
    if (exe_dir == kNoDir && rom_dir == kNoDir) return fail(err, path + " holds no base game or update");
    if (exe_dir != kNoDir && !walk(t, exe_dir, "", 0, out.exefs, err)) return false;
    if (rom_dir != kNoDir) {
        // An update that is not standalone holds only the files it changes (Ocarina of Time's 2.0 update lacks 225 of the base's): the
        // base's RomFS goes underneath and the update's files win, as the launcher's unpacker layers them (launcher nup.rs).
        if (!rom_standalone && base_rom != kNoDir && base_rom != rom_dir) {
            if (!layered(t, base_rom, rom_dir, out.romfs, err)) return false;
        } else if (!walk(t, rom_dir, "", 0, out.romfs, err)) {
            return false;
        }
    }
    return true;
}

bool read_file(const std::string& path, const File& file, std::vector<uint8_t>& out, std::string* err) {
    std::ifstream f(fs_path(path), std::ios::binary);
    const uint64_t n = file.size ? file.stored : 0;
    std::vector<uint8_t> raw(static_cast<size_t>(n));
    f.seekg(static_cast<std::streamoff>(file.offset));
    if (!f || !f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(n)))
        return fail(err, "cannot read " + file.path + " from " + path);
    if (file.codec == kCodecStored) {
        out = std::move(raw);
        return true;
    }
    return decode(file, raw.data(), raw.size(), out, err);
}

bool decode(const File& f, const uint8_t* frame, uint64_t frame_size, std::vector<uint8_t>& out, std::string* err) {
    if (f.codec == kCodecStored) {
        if (frame_size != f.size) return fail(err, f.path + ": stored size mismatch");
        out.assign(frame, frame + frame_size);
        return true;
    }
    if (f.size == 0) {
        out.clear();
        return true;
    }
#ifdef NXRT_HAVE_ZSTD
    out.resize(static_cast<size_t>(f.size));
    const size_t got = ZSTD_decompress(out.data(), out.size(), frame, static_cast<size_t>(frame_size));
    if (ZSTD_isError(got)) return fail(err, f.path + ": the zstd data in the .nup cannot be decoded (" + ZSTD_getErrorName(got) + ")");
    if (got != f.size) return fail(err, f.path + ": the zstd data in the .nup decodes to " + std::to_string(got) + " bytes, not " + std::to_string(f.size));
    return true;
#else
    (void)frame;
    (void)frame_size;
    return fail(err, f.path + " is zstd-compressed in the .nup and this build has no zstd decoder (it comes with the graphics library)");
#endif
}

}  // namespace nxrt::nup
