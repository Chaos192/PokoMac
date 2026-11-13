// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pokémon Pokopia: repair of the shared player UUID (pkp_save_uuid.h). CMNDAT layout from berichan/PokoHex (GPL-3.0, SaveFile.cs):
// magic 0x31227687, end of the compressed data at +0x10, zlib stream from 0x130. The rewrite is the one work/tools/pkp_uuid.py made
// (checked in game: a save so changed joins a host made from the same original save).
#include "pkp_save_uuid.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "miniz.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/tt.h"

namespace nxrt::games::pkp {
namespace {

namespace stdfs = std::filesystem;
constexpr const char* kTag = "pkp";

// The UUID every save made before the RandomEntropy fix has (in memory order, as the save stores it).
constexpr uint8_t kShared[16] = {0xb9, 0xae, 0x85, 0xed, 0x20, 0xe7, 0x4f, 0x6b, 0x89, 0xa1, 0x9f, 0x30, 0xfc, 0xf9, 0xf0, 0xf9};
constexpr uint32_t kCmnMagic = 0x31227687;
constexpr size_t kCmnHeader = 0x130;
constexpr size_t kCmnEndAt = 0x10;

using Bytes = std::vector<uint8_t>;

stdfs::path from_u8(const std::string& s) { return stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string u8(const stdfs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

bool read_all(const stdfs::path& p, Bytes* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return f.good() || f.eof();
}

// Writes next to the file, then replaces it with one rename.
bool write_replace(const stdfs::path& p, const Bytes& b) {
    const stdfs::path tmp(p.wstring() + L".uuidtmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        f.flush();
        if (!f) return false;
    }
    if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::error_code ec;
    stdfs::remove(tmp, ec);
    return false;
}

size_t replace_all(Bytes& b, const uint8_t* from, const uint8_t* to) {
    size_t n = 0;
    if (b.size() < 16) return 0;
    for (size_t i = 0; i + 16 <= b.size();) {
        if (std::memcmp(b.data() + i, from, 16) == 0) {
            std::memcpy(b.data() + i, to, 16);
            ++n;
            i += 16;
        } else {
            ++i;
        }
    }
    return n;
}

uint32_t rd32(const Bytes& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

// A file with the UUID replaced: *count places, *out the new contents. False when it cannot be read or is not the expected layout.
bool rewrite(const stdfs::path& p, bool cmndat, const uint8_t* uuid, size_t* count, Bytes* out, std::string* why) {
    Bytes file;
    if (!read_all(p, &file)) { *why = "cannot be read"; return false; }
    if (!cmndat) {
        *out = file;
        *count = replace_all(*out, kShared, uuid);
        return true;
    }
    if (file.size() < kCmnHeader || rd32(file, 0) != kCmnMagic) { *why = "not a Pokopia common save"; return false; }
    const uint32_t end = rd32(file, kCmnEndAt);
    if (end <= kCmnHeader || end > file.size()) { *why = "bad compressed size"; return false; }
    size_t raw_n = 0;
    void* raw_p = tinfl_decompress_mem_to_heap(file.data() + kCmnHeader, end - kCmnHeader, &raw_n, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!raw_p) { *why = "does not decompress"; return false; }
    Bytes raw(static_cast<uint8_t*>(raw_p), static_cast<uint8_t*>(raw_p) + raw_n);
    mz_free(raw_p);
    *count = replace_all(raw, kShared, uuid);
    if (*count == 0) { *out = file; return true; }
    size_t z_n = 0;
    const int flags = static_cast<int>(tdefl_create_comp_flags_from_zip_params(9, 15, MZ_DEFAULT_STRATEGY));   // zlib header, level 9
    void* z_p = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &z_n, flags);
    if (!z_p) { *why = "does not compress"; return false; }
    Bytes z(static_cast<uint8_t*>(z_p), static_cast<uint8_t*>(z_p) + z_n);
    mz_free(z_p);
    // the round trip must give back exactly what was meant to be written
    size_t back_n = 0;
    void* back = tinfl_decompress_mem_to_heap(z.data(), z.size(), &back_n, TINFL_FLAG_PARSE_ZLIB_HEADER);
    const bool same = back && back_n == raw.size() && std::memcmp(back, raw.data(), raw.size()) == 0;
    if (back) mz_free(back);
    if (!same) { *why = "round trip differs"; return false; }
    const size_t new_end = kCmnHeader + z.size();
    if (new_end > 0x7fffffff) { *why = "too large"; return false; }
    out->assign((std::max)(new_end, file.size()), 0);   // as pkp_uuid.py: never shorter than before, zeros after the stream
    std::memcpy(out->data(), file.data(), kCmnHeader);
    std::memcpy(out->data() + kCmnHeader, z.data(), z.size());
    const uint32_t e32 = static_cast<uint32_t>(new_end);
    std::memcpy(out->data() + kCmnEndAt, &e32, 4);
    return true;
}

std::string uuid_str(const uint8_t* u) {
    char s[40];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5], u[6],
                  u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return s;
}

// The folder name fs.cpp gives the account save of the profile's user: the Uid's 16 bytes in memory order, as hex.
std::string account_key() {
    const services::TitleProfile& p = services::profile();
    uint8_t u[16];
    std::memcpy(u, &p.user_uid_hi, 8);
    std::memcpy(u + 8, &p.user_uid_lo, 8);
    std::string k;
    char b[3];
    for (uint8_t c : u) {
        std::snprintf(b, sizeof b, "%02x", c);
        k += b;
    }
    return k;
}

void repair() {
    const stdfs::path base = from_u8(services::save_dir());
    const std::string key = account_key();
    const stdfs::path root = base / "account" / key;
    std::error_code ec;
    if (!stdfs::is_directory(root, ec)) return;   // no save yet: a new game gets a random UUID now
    if (stdfs::exists(stdfs::path(root.wstring() + L".commit"), ec)) {
        NXLOG_W(kTag, "player UUID: not checked, the save has an interrupted commit (%s.commit) the game finishes first; checked again next start",
                u8(root).c_str());
        return;
    }
    struct Part {
        const char* rel;
        bool cmndat;
        size_t count = 0;
        Bytes data;
    };
    Part parts[] = {{"G00/CMNDAT.BIN", true}, {"B00/CMNDAT.BIN", true}, {"S00/SYDAT.BIN", false}};

    uint8_t uuid[16];
    do {
        tt::rng_fill(uuid, sizeof uuid);
        uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0F) | 0x40);   // version 4
        uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3F) | 0x80);   // RFC 4122 variant
    } while (std::memcmp(uuid, kShared, 16) == 0);

    size_t total = 0;
    for (Part& pt : parts) {
        const stdfs::path p = root / stdfs::path(pt.rel);
        if (!stdfs::is_regular_file(p, ec)) continue;
        std::string why;
        if (!rewrite(p, pt.cmndat, uuid, &pt.count, &pt.data, &why)) {
            NXLOG_W(kTag, "player UUID: %s %s; the save is left as it is", u8(p).c_str(), why.c_str());
            return;
        }
        total += pt.count;
    }
    if (total == 0) {
        NXLOG_I(kTag, "player UUID: this save has its own (not the shared one), nothing to repair");
        return;
    }

    // a copy of the whole account folder first
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &now);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    const stdfs::path backup = base / "uuid-backup" / stamp / key;
    stdfs::create_directories(backup, ec);
    if (!ec) stdfs::copy(root, backup, stdfs::copy_options::recursive | stdfs::copy_options::overwrite_existing, ec);
    if (ec) {
        NXLOG_E(kTag, "player UUID: the backup to %s failed (%s); the save is left as it is", u8(backup).c_str(), ec.message().c_str());
        return;
    }

    for (const Part& pt : parts) {
        if (pt.count == 0) continue;
        const stdfs::path p = root / stdfs::path(pt.rel);
        if (write_replace(p, pt.data)) continue;
        const DWORD err = GetLastError();
        // put back what was already written, from the backup
        for (const Part& q : parts) {
            if (q.count == 0) continue;
            std::error_code e2;
            stdfs::copy_file(backup / stdfs::path(q.rel), root / stdfs::path(q.rel), stdfs::copy_options::overwrite_existing, e2);
        }
        NXLOG_E(kTag, "player UUID: writing %s failed (Win32 error %lu); the save was put back from %s", u8(p).c_str(),
                static_cast<unsigned long>(err), u8(backup).c_str());
        return;
    }
    NXLOG_W(kTag, "player UUID: this save had %s, the UUID every save made with older builds shares (Link Play refused visits with error "
            "9005); now %s (%zu places: G00 %zu, B00 %zu, S00 %zu). Backup: %s",
            uuid_str(kShared).c_str(), uuid_str(uuid).c_str(), total, parts[0].count, parts[1].count, parts[2].count, u8(backup).c_str());
}

}  // namespace

void save_uuid_repair() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    if (!config_get_bool("pkp", "uuid_repair", true)) {
        NXLOG_I(kTag, "player UUID: repair off ([pkp] uuid_repair = false)");
        return;
    }
    repair();
}

}  // namespace nxrt::games::pkp
