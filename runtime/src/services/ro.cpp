// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// ldr:ro and ro:1: IRoInterface (nxrt/ro.h). Our code; the facts come from Lichii's service/ro (command ids and argument layouts, the
// checks and results of its ProcessContext and RoContext, the NRR and NRO headers), Atmosphère's ro module as described there (the NRR's
// magic, size and kind checks) and nxnative's notes (docs/reference/services.md: "Loading NROs with ro::LoadModule", "NRR version 1",
// "Online SDKs in NROs are never loaded", "NRO tables sized from the manifest and snapshots taken on demand").
//
// What the console does: nn::ro (in the game's SDK) sends RegisterProcessHandle once, RegisterModuleInfo for each NRR (the signed list of
// the SHA-256 of every NRO the title may load), then for each module MapManualLoadModuleMemory with the NRO the game read into its heap and
// a buffer for its bss. ro checks the NRR and the image and maps both buffers, one after the other, as code memory at a random address of
// the process (MapCodeMemory: the heap pages are moved there and left inaccessible at their old address), text read + execute, rodata
// read, data and bss read-write. Everything else (zeroing the bss, the relocations, binding symbols through rtld's module lists, the
// module's init and fini) is the SDK's own code, which runs here lifted as on the console.
//
// Here:
//   * the checks are ro's, with its results;
//   * the image and the bss are copied to an address of [kNroBase, +kNroSize) that the title keeps for the module from run to run (the
//     title's nro.tsv: module id, SHA-256, address, span, module index, name), so the code lifted for that address stays valid. The heap
//     buffers keep their pages (svcQueryMemory reports them with no permission, as Horizon leaves them) and get the module's bytes back
//     when it is unmapped, as Horizon's pages would hold them;
//   * the module's code runs natively: the whole NRO is lifted and compiled into a module unit (missed::load_module_unit, the build's own
//     lifter and toolchain) the first time, then loaded from the title's cache, which is keyed by the image's hash and its address. Its
//     functions stay registered after an unmap, so mapping it again costs nothing; another module that needs its address range later takes
//     it out (nxmodule_unregister);
//   * where nothing can compile (no on-demand compiler: [missed] mode = record, iPhone and iPad) the map fails with NotLoaded, as every map
//     did before NROs loaded, and the module is listed in nro.tsv.
#include "nxrt/ro.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../cheats/codepatch.h"
#include "../net/lobby.h"
#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/missed.h"
#include "nxrt/nxmodule.h"
#include "nxrt/services.h"
#include "services_internal.h"

namespace nxrt::ro {
namespace {

constexpr const char* kTag = "ro";
using U = unsigned long long;
using services::res;

// ro's results (module 22; Lichii ro_results.h)
constexpr uint32_t kOutOfAddressSpace = res(22, 2);
constexpr uint32_t kAlreadyLoaded = res(22, 3);
constexpr uint32_t kInvalidNro = res(22, 4);
constexpr uint32_t kInvalidNrr = res(22, 6);
constexpr uint32_t kTooManyNro = res(22, 7);
constexpr uint32_t kTooManyNrr = res(22, 8);
constexpr uint32_t kNotAuthorized = res(22, 9);
constexpr uint32_t kInvalidNrrKind = res(22, 10);
constexpr uint32_t kInvalidAddress = res(22, 1025);
constexpr uint32_t kInvalidSize = res(22, 1026);
constexpr uint32_t kNotLoaded = res(22, 1028);
constexpr uint32_t kNotRegistered = res(22, 1029);
constexpr uint32_t kInvalidSession = res(22, 1030);
constexpr uint32_t kInvalidProcess = res(22, 1031);

constexpr size_t kMaxNrr = 0x100, kMaxNro = 0x100;   // per process (Lichii MaxNrrInfos / MaxNroInfos)
constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kSlotAlign = mem::kAllocGranularity;   // a module's address and span: 64 KiB, with a free 64 KiB after it (guard pages)
constexpr uint32_t kMemAliasCode = 0x08, kMemAliasCodeData = 0x09;   // svc::MemoryState of what MapCodeMemory makes

using Hash = std::array<uint8_t, 32>;
using ModuleId = std::array<uint8_t, 32>;

template <typename T>
T rd(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
    return v;
}
uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
bool aligned(uint64_t v) { return (v & (kPage - 1)) == 0; }

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

// ---- the title's NRO table (nro.tsv) ---------------------------------------------------------------------------------------------------
struct Row {
    std::string module_id, sha;
    uint64_t base = 0, span = 0;
    uint32_t index = 0;
    std::string name;
};

std::string exe_dir() {
    char b[4096];
    const DWORD n = GetModuleFileNameA(nullptr, b, sizeof b);
    std::string s(b, n < sizeof b ? n : 0);
    const size_t k = s.find_last_of("/\\");
    return k == std::string::npos ? std::string(".") : s.substr(0, k);
}

// ---- process state ----------------------------------------------------------------------------------------------------------------------
struct Nrr {
    uint64_t addr = 0, size = 0;
    std::vector<Hash> hashes;
};
struct Mapped {
    uint64_t base = 0, span = 0;
    uint64_t nro_addr = 0, nro_size = 0, bss_addr = 0, bss_size = 0;
    uint64_t rx_size = 0, ro_size = 0, rw_size = 0;
    mem::VmInfo nro_src, bss_src;             // the heap buffers' state before the map (put back at the unmap)
    ModuleId module_id{};
    std::string name;
    const NxModule* nx = nullptr;
};
struct State {
    std::mutex mu;
    const void* owner = nullptr;              // the session that sent RegisterProcessHandle (one process context)
    std::vector<Nrr> nrrs;
    std::vector<Mapped> mapped;               // the NROs mapped now (process wide)
    bool table_read = false;
    std::vector<Row> table;
    std::map<std::string, const NxModule*> units;   // module id + address -> its registered unit
};
State& st() {
    static State s;
    return s;
}

void read_table(State& s) {
    if (s.table_read) return;
    s.table_read = true;
    std::ifstream f(table_path());
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        Row r;
        std::string base, span;
        if (!std::getline(ss, r.module_id, '\t') || !std::getline(ss, r.sha, '\t') || !std::getline(ss, base, '\t') ||
            !std::getline(ss, span, '\t'))
            continue;
        std::string index;
        std::getline(ss, index, '\t');
        std::getline(ss, r.name, '\t');
        r.base = std::strtoull(base.c_str(), nullptr, 16);
        r.span = std::strtoull(span.c_str(), nullptr, 16);
        r.index = static_cast<uint32_t>(std::strtoul(index.c_str(), nullptr, 10));
        if (r.module_id.size() == 64 && r.base >= kNroBase && r.span && r.base + r.span <= kNroBase + kNroSize && r.index >= kNroFirstIndex)
            s.table.push_back(r);
    }
}

void write_table(const State& s) {
    const std::string path = table_path(), tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            NXLOG_W(kTag, "cannot write %s", path.c_str());
            return;
        }
        f << "# AnySwitch: the NROs this title loaded (runtime src/services/ro.cpp): module id, SHA-256, address, span, module index, name\n";
        for (const Row& r : s.table) {
            char b[64];
            std::snprintf(b, sizeof b, "%llx\t%llx\t%u", static_cast<U>(r.base), static_cast<U>(r.span), r.index);
            f << r.module_id << '\t' << r.sha << '\t' << b << '\t' << r.name << '\n';
        }
    }
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// A free range of `span` bytes in the NRO region, first fit, avoiding `taken` ([start, end) pairs) and a guard after each.
uint64_t first_fit(std::vector<std::pair<uint64_t, uint64_t>> taken, uint64_t span) {
    std::sort(taken.begin(), taken.end());
    uint64_t cur = kNroBase;
    for (const auto& [a, e] : taken) {
        if (cur + span + kSlotAlign <= a) break;
        cur = std::max(cur, align_up(e + kSlotAlign, kSlotAlign));
    }
    return cur + span <= kNroBase + kNroSize ? cur : 0;
}

bool overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }

// ---- reading an NRO ---------------------------------------------------------------------------------------------------------------------
// The NRO header (Lichii NroHeader): +4 MOD0 offset, +0x10 "NRO0", +0x18 size, +0x20 text/ro/rw (offset, size), +0x38 bss size, +0x40 the
// module id (its build id).
struct NroHeader {
    uint32_t entry, mod0, pad0[2], magic, pad1, size, pad2;
    uint32_t text_off, text_size, ro_off, ro_size, rw_off, rw_size, bss_size, pad3;
    uint8_t module_id[32];
    uint8_t pad4[32];
};
static_assert(sizeof(NroHeader) == 0x80, "the NRO header is 0x80 bytes");

// The module's name: the path Nintendo's linker puts at the start of .rodata (u32 0, u32 length, the path), its file name without the
// extension; else nro_<module id>.
std::string module_name(uint64_t img, const NroHeader& h) {
    if (h.ro_size >= 8 && rd<uint32_t>(img + h.ro_off) == 0) {
        const uint32_t n = rd<uint32_t>(img + h.ro_off + 4);
        if (n > 0 && n <= 0x200 && 8ull + n <= h.ro_size) {
            std::string p(reinterpret_cast<const char*>(img + h.ro_off + 8), n);
            p = p.substr(0, p.find('\0'));
            const bool text = !p.empty() && std::all_of(p.begin(), p.end(), [](char ch) { return ch >= 0x20 && ch < 0x7f; });
            if (text) {
                const size_t k = p.find_last_of("/\\");
                std::string b = k == std::string::npos ? p : p.substr(k + 1);
                b = b.substr(0, b.find('.'));
                std::string out;
                for (char ch : b) out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
                if (!out.empty()) return out;
            }
        }
    }
    return "nro_" + hex(h.module_id, 8);
}

// Whether the image defines `sym` in its dynamic symbol table (MOD0 -> .dynamic -> DT_SYMTAB / DT_STRTAB / DT_HASH).
bool defines(uint64_t img, uint64_t size, const char* sym) {
    const uint64_t mod0 = rd<uint32_t>(img + 4);
    if (mod0 + 0x1c > size || rd<uint32_t>(img + mod0) != 0x30444F4D) return false;   // "MOD0"
    const uint64_t dyn = mod0 + static_cast<int64_t>(rd<int32_t>(img + mod0 + 4));
    std::map<int64_t, uint64_t> dt;
    for (uint64_t d = dyn; d + 16 <= size; d += 16) {
        const int64_t t = rd<int64_t>(img + d);
        if (!t) break;
        dt.emplace(t, rd<uint64_t>(img + d + 8));
    }
    if (!dt.count(5) || !dt.count(6)) return false;
    const uint64_t symtab = dt[6], strtab = dt[5], strsz = dt.count(10) ? dt[10] : 0;
    uint64_t nsym = 0;
    if (dt.count(4) && dt[4] + 8 <= size) nsym = rd<uint32_t>(img + dt[4] + 4);       // DT_HASH: nchain
    else if (strtab > symtab) nsym = (strtab - symtab) / 24;
    const size_t want = std::strlen(sym);
    for (uint64_t i = 1; i < nsym && symtab + (i + 1) * 24 <= size; i++) {
        const uint64_t e = img + symtab + i * 24;
        const uint32_t name = rd<uint32_t>(e);
        if (!rd<uint16_t>(e + 6) || name + want + 1 > strsz || strtab + name + want + 1 > size) continue;   // undefined, or past .dynstr
        if (std::memcmp(reinterpret_cast<const void*>(img + strtab + name), sym, want + 1) == 0) return true;
    }
    return false;
}

// ---- the commands ----------------------------------------------------------------------------------------------------------------------
uint32_t validate_address_and_nonzero_size(uint64_t addr, uint64_t size) {
    if (!aligned(addr)) return kInvalidAddress;
    if (size == 0 || !aligned(size) || addr + size <= addr) return kInvalidSize;
    return 0;
}
uint32_t validate_address_and_size(uint64_t addr, uint64_t size) {
    if (!aligned(addr)) return kInvalidAddress;
    if (!aligned(size) || (size != 0 && addr + size <= addr)) return kInvalidSize;
    return 0;
}

// RegisterModuleInfo: the NRR at [addr, addr + size) of the game's memory. Version 0 (byte +5): the hash list's offset at +0x340 and its
// count at +0x344, the size at +0x338 and the kind at +0x33c. Version 1 (Switch 2) has a header 0x180 bytes longer: the same fields at
// +0x4b8 .. +0x4c4 (nxnative "NRR version 1").
uint32_t register_nrr(State& s, uint64_t addr, uint64_t size, int kind, bool enforce_kind) {
    if (const uint32_t r = validate_address_and_nonzero_size(addr, size)) return r;
    if (s.nrrs.size() >= kMaxNrr) return kTooManyNrr;
    if (size < 0x350 || rd<uint32_t>(addr) != 0x3052524E) return kInvalidNrr;            // "NRR0"
    const uint64_t shift = rd<uint8_t>(addr + 5) == 1 ? 0x180 : 0;
    if (size < 0x350 + shift) return kInvalidNrr;
    if (rd<uint32_t>(addr + 0x338 + shift) != size) return kInvalidSize;
    if (enforce_kind && rd<uint8_t>(addr + 0x33c + shift) != kind) return kInvalidNrrKind;
    const uint64_t off = rd<uint32_t>(addr + 0x340 + shift), count = rd<uint32_t>(addr + 0x344 + shift);
    if (off > size || count > (size - off) / sizeof(Hash)) return kInvalidNrr;
    Nrr n;
    n.addr = addr;
    n.size = size;
    n.hashes.resize(count);
    if (count) std::memcpy(n.hashes.data(), reinterpret_cast<const void*>(addr + off), count * sizeof(Hash));
    s.nrrs.push_back(std::move(n));
    NXLOG_I(kTag, "RegisterModuleInfo: NRR at 0x%llx (version %d) lists %llu modules", static_cast<U>(addr), shift ? 1 : 0, static_cast<U>(count));
    return 0;
}

uint32_t unregister_nrr(State& s, uint64_t addr) {
    if (!aligned(addr)) return kInvalidAddress;
    auto it = std::find_if(s.nrrs.begin(), s.nrrs.end(), [&](const Nrr& n) { return n.addr == addr; });
    if (it == s.nrrs.end()) return kNotRegistered;
    s.nrrs.erase(it);
    return 0;
}

// The module's lifted code at `base`: the unit this service registered for it earlier, else a module unit compiled now or found in the
// title's cache. Units that hold a part of [base, base + span) and belong to no mapped module are taken out first.
const NxModule* native_code(State& s, const Mapped& m, uint32_t index, const uint8_t* image, std::string* err) {
    // keyed by the image's bytes too: a mod's NRO patch keeps the module id, and the patched image mapped once must not lend its code to
    // the original mapped later at the same address (or the other way round)
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < m.nro_size; ++i) h = (h ^ image[i]) * 0x100000001b3ull;
    const std::string key = hex(m.module_id.data(), m.module_id.size()) + "@" + std::to_string(m.base) + "#" + std::to_string(h);
    if (auto it = s.units.find(key); it != s.units.end()) {
        if (nxmodule_by_index(it->second->index) == it->second) return it->second;
        s.units.erase(it);
    }
    for (auto it = s.units.begin(); it != s.units.end();) {
        const NxModule* nx = it->second;
        if (overlaps(nx->base, std::max<uint64_t>(nx->text_end - nx->base, 1), m.base, m.span) || nx->index == index) {
            nxmodule_unregister(nx);
            it = s.units.erase(it);
        } else {
            ++it;
        }
    }
    if (const NxModule* other = nxmodule_by_index(index)) {
        *err = "module index " + std::to_string(index) + " is taken by " + (other->name ? other->name : "another module");
        return nullptr;
    }
    const NxModule* nx = missed::load_module_unit(m.name.c_str(), m.base, index, image, m.nro_size, err);
    if (nx) s.units[key] = nx;
    return nx;
}

// MapManualLoadModuleMemory: the NRO at [nro, nro + nro_size) and its bss buffer at [bss, bss + bss_size).
// [ro] nro_patches (docs/MODS.md): the NRO patches of the title's mods, as Atmosphère's ro applies its nro_patches: after the NRR check,
// to the mapped image, offsets from the NRO's first byte. The mod manager (mods.py resolve) writes the patched image of each NRO a patch
// names and this table, one row per NRO: "<SHA-256 of the original>\t<patched image file>[\t<RomFS path>]" ('#': a comment). The image
// the game maps is replaced by the patched one of the same size; the build compiled that image ahead (pkp_build mod_layers).
std::vector<uint8_t> patched_image(const std::string& sha, uint64_t size) {
    const std::string table = config_get_str("ro", "nro_patches", "");
    if (table.empty()) return {};
    std::ifstream in(table, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos || line.compare(0, t1, sha) != 0) continue;
        const size_t t2 = line.find('\t', t1 + 1);
        const std::string path = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (img.size() != size) {
            NXLOG_W(kTag, "[ro] nro_patches: %s is %zu bytes, the NRO it patches %llu: not used", path.c_str(), img.size(), static_cast<U>(size));
            return {};
        }
        return img;
    }
    return {};
}

uint32_t map_nro(State& s, uint64_t nro, uint64_t nro_size, uint64_t bss, uint64_t bss_size, uint64_t* out) {
    if (const uint32_t r = validate_address_and_nonzero_size(nro, nro_size)) return r;
    if (const uint32_t r = validate_address_and_size(bss, bss_size)) return r;
    if (nro_size + bss_size < nro_size) return kInvalidSize;
    if (s.mapped.size() >= kMaxNro) return kTooManyNro;
    if (nro_size < sizeof(NroHeader) || !mem::is_mapped(nro, nro_size) || (bss_size && !mem::is_mapped(bss, bss_size))) return kInvalidNro;
    NroHeader h;
    std::memcpy(&h, reinterpret_cast<const void*>(nro), sizeof h);
    // ro's ValidateNro
    if (h.magic != 0x304F524E) return kInvalidNro;                                         // "NRO0"
    if (h.size != nro_size || h.bss_size != bss_size) return kInvalidNro;
    if (!aligned(h.text_size) || !aligned(h.ro_size) || !aligned(h.rw_size) || !aligned(h.bss_size)) return kInvalidNro;
    if (h.text_off != 0 || h.text_off + h.text_size != h.ro_off || h.ro_off + h.ro_size != h.rw_off ||
        static_cast<uint64_t>(h.rw_off) + h.rw_size != nro_size)
        return kInvalidNro;
    const Hash hash = vnet::sha256(reinterpret_cast<const void*>(nro), nro_size);
    const bool listed = std::any_of(s.nrrs.begin(), s.nrrs.end(), [&](const Nrr& n) {
        return std::find(n.hashes.begin(), n.hashes.end(), hash) != n.hashes.end();
    });
    const std::string sha = hex(hash.data(), hash.size()), mid = hex(h.module_id, sizeof h.module_id);
    if (!listed) {
        NXLOG_W(kTag, "MapManualLoadModuleMemory: the NRO at 0x%llx (SHA-256 %s) is in no registered NRR", static_cast<U>(nro), sha.c_str());
        return kNotAuthorized;
    }
    for (const Mapped& m : s.mapped)
        if (std::memcmp(m.module_id.data(), h.module_id, sizeof h.module_id) == 0) return kAlreadyLoaded;

    Mapped m;
    m.nro_addr = nro;
    m.nro_size = nro_size;
    m.bss_addr = bss;
    m.bss_size = bss_size;
    m.rx_size = h.text_size;
    m.ro_size = h.ro_size;
    m.rw_size = h.rw_size;
    std::memcpy(m.module_id.data(), h.module_id, sizeof h.module_id);
    m.name = module_name(nro, h);
    if (config_get_str("ro", "online_sdks", "refuse") != "load" && defines(nro, nro_size, "EOS_Platform_Create")) {
        NXLOG_W(kTag, "MapManualLoadModuleMemory: refused %s: an online SDK (Epic Online Services), not loaded ([ro] online_sdks)", m.name.c_str());
        return kNotLoaded;
    }

    // its address: the table's, else the first free range (of every module the title met, else of the ones mapped now)
    read_table(s);
    m.span = align_up(nro_size + bss_size, kSlotAlign);
    auto row = std::find_if(s.table.begin(), s.table.end(), [&](const Row& r) { return r.module_id == mid; });
    std::vector<std::pair<uint64_t, uint64_t>> busy;
    for (const Mapped& o : s.mapped) busy.emplace_back(o.base, o.base + o.span);
    auto free_now = [&](uint64_t a) {
        return std::none_of(busy.begin(), busy.end(), [&](const auto& b) { return overlaps(a, m.span + kSlotAlign, b.first, b.second - b.first + kSlotAlign); });
    };
    if (row != s.table.end() && row->span >= m.span && free_now(row->base)) {
        m.base = row->base;
    } else {
        std::vector<std::pair<uint64_t, uint64_t>> all = busy;
        for (const Row& r : s.table)
            if (row == s.table.end() || &r != &*row) all.emplace_back(r.base, r.base + r.span);
        m.base = first_fit(all, m.span);
        if (!m.base) m.base = first_fit(busy, m.span);   // the region is full of other modules' addresses: share one with an unmapped module
        if (!m.base) return kOutOfAddressSpace;
    }
    uint32_t index = 0;
    if (row != s.table.end()) {
        index = row->index;
        row->base = m.base;
        row->span = std::max(row->span, m.span);
        row->sha = sha;
        row->name = m.name;
    } else {
        index = kNroFirstIndex;
        for (const Row& r : s.table) index = std::max(index, r.index + 1);
        Row r;
        r.module_id = mid;
        r.sha = sha;
        r.base = m.base;
        r.span = m.span;
        r.index = index;
        r.name = m.name;
        s.table.push_back(r);
    }
    write_table(s);

    // the copy, at its address
    if (!mem::reserve_fixed(m.base, m.span, "nro")) return kOutOfAddressSpace;
    if (!mem::commit(m.base, m.span, mem::Prot::RW)) {
        mem::release_fixed(m.base);
        return kOutOfAddressSpace;
    }
    const std::vector<uint8_t> patched = patched_image(sha, nro_size);   // a mod's nro_patches (after the NRR check, as Atmosphère's ro)
    std::memcpy(reinterpret_cast<void*>(m.base), patched.empty() ? reinterpret_cast<const void*>(nro) : patched.data(), nro_size);
    if (bss_size) std::memcpy(reinterpret_cast<void*>(m.base + nro_size), reinterpret_cast<const void*>(bss), bss_size);
    if (!patched.empty()) NXLOG_I(kTag, "MapManualLoadModuleMemory: %s patched by a mod ([ro] nro_patches)", m.name.c_str());

    std::string err;
    const auto* image = patched.empty() ? reinterpret_cast<const uint8_t*>(nro) : reinterpret_cast<const uint8_t*>(m.base);
    m.nx = native_code(s, m, index, image, &err);
    if (!m.nx) {
        mem::decommit(m.base, m.span);
        mem::release_fixed(m.base);
        NXLOG_E(kTag, "MapManualLoadModuleMemory: %s (%llu bytes) cannot run: %s", m.name.c_str(), static_cast<U>(nro_size), err.c_str());
        return kNotLoaded;
    }
    // what svcQueryMemory reports: the module as code memory, its heap buffers with no permission
    mem::vm_set(m.base, m.rx_size, kMemAliasCode, 5);
    mem::vm_set(m.base + m.rx_size, m.ro_size, kMemAliasCode, 1);
    mem::vm_set(m.base + m.rx_size + m.ro_size, m.rw_size + bss_size, kMemAliasCodeData, 3);
    m.nro_src = mem::vm_query(nro);
    mem::vm_set(nro, nro_size, m.nro_src.state, 0);
    if (bss_size) {
        m.bss_src = mem::vm_query(bss);
        mem::vm_set(bss, bss_size, m.bss_src.state, 0);
    }
    NXLOG_I(kTag, "MapManualLoadModuleMemory: %s (module %.16s..., %llu + %llu bytes) mapped at 0x%llx, its code %s (index %u)", m.name.c_str(),
            mid.c_str(), static_cast<U>(nro_size), static_cast<U>(bss_size), static_cast<U>(m.base), m.nx->name, index);
    *out = m.base;
    s.mapped.push_back(std::move(m));
    return 0;
}

void unmap(const Mapped& m) {
    // the heap buffers get the module's bytes as it leaves them (on Horizon they are the same pages), then their permission back
    std::memcpy(reinterpret_cast<void*>(m.nro_addr), reinterpret_cast<const void*>(m.base), m.nro_size);
    if (m.bss_size) std::memcpy(reinterpret_cast<void*>(m.bss_addr), reinterpret_cast<const void*>(m.base + m.nro_size), m.bss_size);
    mem::vm_set(m.nro_addr, m.nro_size, m.nro_src.state, m.nro_src.perm ? m.nro_src.perm : 3);
    if (m.bss_size) mem::vm_set(m.bss_addr, m.bss_size, m.bss_src.state, m.bss_src.perm ? m.bss_src.perm : 3);
    mem::vm_set(m.base, m.span, mem::kMemFree, 0);
    mem::decommit(m.base, m.span);
    mem::release_fixed(m.base);
}

// Whether a part of the module's writable data is lent out: svcMapMemory made it the source of a stack (no permission while mapped). Horizon's
// UnmapCodeMemory refuses pages in that state, and ro passes the refusal on.
bool lent_out(const Mapped& m) {
    const uint64_t rw = m.base + m.rx_size + m.ro_size, end = m.base + m.nro_size + m.bss_size;
    for (uint64_t a = rw; a < end;) {
        const mem::VmInfo b = mem::vm_query(a);
        if (b.state != kMemAliasCodeData || b.perm != 3 || b.size == 0) return true;
        a = b.addr + b.size;
    }
    return false;
}

uint32_t unmap_nro(State& s, uint64_t base) {
    if (!aligned(base)) return kInvalidAddress;
    auto it = std::find_if(s.mapped.begin(), s.mapped.end(), [&](const Mapped& m) { return m.base == base; });
    if (it == s.mapped.end()) return kNotLoaded;
    if (lent_out(*it)) {
        NXLOG_W(kTag, "UnmapManualLoadModuleMemory: %s at 0x%llx still lends memory to a thread stack (svcMapMemory): refused", it->name.c_str(),
                static_cast<U>(base));
        return kernel::kInvalidMemoryState;
    }
    cheats::cp::code_unmapped(it->base, it->span);   // live patches of its code (the overlay's, a mod's) must not outlive it
    unmap(*it);
    NXLOG_I(kTag, "UnmapManualLoadModuleMemory: %s at 0x%llx", it->name.c_str(), static_cast<U>(base));
    s.mapped.erase(it);
    return 0;
}

// ---- IRoInterface ---------------------------------------------------------------------------------------------------------------------
// Raw arguments start with the process id placeholder (u64) the SDK sends with the pid.
class RoInterface : public ipc::Service {
public:
    explicit RoInterface(bool jit_plugin) : Service("IRoInterface"), kind_(jit_plugin ? 1 : 0) {
        add_cmd(0, "MapManualLoadModuleMemory", [this](ipc::Request& q, ipc::Response& r) -> uint32_t {
            State& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (const uint32_t e = check_process(s)) return e;
            uint64_t base = 0;
            const uint32_t rc = map_nro(s, q.arg<uint64_t>(8), q.arg<uint64_t>(16), q.arg<uint64_t>(24), q.arg<uint64_t>(32), &base);
            if (rc) NXLOG_W(kTag, "MapManualLoadModuleMemory(0x%llx, 0x%llx, 0x%llx, 0x%llx): 0x%x", static_cast<U>(q.arg<uint64_t>(8)),
                            static_cast<U>(q.arg<uint64_t>(16)), static_cast<U>(q.arg<uint64_t>(24)), static_cast<U>(q.arg<uint64_t>(32)), rc);
            r.push<uint64_t>(base);
            return rc;
        });
        add_cmd(1, "UnmapManualLoadModuleMemory", [this](ipc::Request& q, ipc::Response&) -> uint32_t {
            State& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (const uint32_t e = check_process(s)) return e;
            return unmap_nro(s, q.arg<uint64_t>(8));
        });
        add_cmd(2, "RegisterModuleInfo", [this](ipc::Request& q, ipc::Response&) -> uint32_t {
            State& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (const uint32_t e = check_process(s)) return e;
            return register_nrr(s, q.arg<uint64_t>(8), q.arg<uint64_t>(16), 0, true);
        });
        add_cmd(3, "UnregisterModuleInfo", [this](ipc::Request& q, ipc::Response&) -> uint32_t {
            State& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (const uint32_t e = check_process(s)) return e;
            return unregister_nrr(s, q.arg<uint64_t>(8));
        });
        // RegisterProcessHandle (the SDK's nn::ro::Initialize): this session's process context. A second one of the process: InvalidSession.
        add_cmd(4, "RegisterProcessHandle", [this](ipc::Request&, ipc::Response&) -> uint32_t {
            State& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (s.owner && s.owner != this) return kInvalidSession;
            s.owner = this;
            return 0;
        });
        // RegisterProcessModuleInfo (10; ldr:ro: the game's NRRs, ro:1: the jit plugin's, whose kind is checked)
        add_cmd(10, "RegisterProcessModuleInfo", [this](ipc::Request& q, ipc::Response&) -> uint32_t {
            State& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (const uint32_t e = check_process(s)) return e;
            return register_nrr(s, q.arg<uint64_t>(8), q.arg<uint64_t>(16), kind_, kind_ == 1);
        });
    }
    // Closing the session finalizes its process context: its NRRs are forgotten (the mapped modules stay, as the memory is the process's).
    void on_close() override {
        State& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.owner != this) return;
        s.owner = nullptr;
        s.nrrs.clear();
    }

private:
    uint32_t check_process(const State& s) const { return s.owner == this ? 0 : kInvalidProcess; }
    int kind_;
};

}  // namespace

std::shared_ptr<ipc::Service> make_service(bool jit_plugin) { return std::make_shared<RoInterface>(jit_plugin); }

std::vector<Loaded> loaded() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    std::vector<Loaded> v;
    for (const Mapped& m : s.mapped) v.push_back({m.base, m.nro_size, m.bss_size, m.name, m.nx});
    return v;
}

std::string table_path() {
    std::string p = config_get_str("ro", "table", "");
    return p.empty() ? exe_dir() + "/nro.tsv" : p;
}

void reset_for_tests() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    for (const Mapped& m : s.mapped) unmap(m);
    s.mapped.clear();
    s.nrrs.clear();
    s.owner = nullptr;
    s.table.clear();
    s.table_read = false;
}

}  // namespace nxrt::ro

namespace nxrt::services {
void register_ro() {
    add("ldr:ro", [] { return ro::make_service(false); });
    add("ro:1", [] { return ro::make_service(true); });
}
}  // namespace nxrt::services
