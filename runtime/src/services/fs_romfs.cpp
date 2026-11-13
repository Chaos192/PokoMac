// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// See fs_romfs.h. The sources become one tree (host folder, .nup package or in-place RomFS; the base game under an update, mod layers
// over both), which is laid out as a standard RomFS image: the file data from 0x200, each file 0x10-aligned in path order, then the
// four tables. Entry layouts, the name hash (seed 123456789, rotate right by 5, xor each unsigned name byte, the parent's entry offset
// mixed into the seed) and the bucket index (hash modulo the bucket count) are the format's public description (libnx romfs_dev.h /
// romfs_dev.c, ISC). The tables and the file data are ordered by full image path; bucket counts follow the rule of bucket_count().
// File bytes are read from their source when the game reads them.
#include "fs_romfs.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <list>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/nup.h"
#include "nxrt/write_watch.h"
#include "romfs_source.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace nxrt::services::fs {
namespace {

namespace stdfs = std::filesystem;

constexpr uint32_t kNone = 0xFFFFFFFFu;            // "no entry" in every link field
constexpr uint64_t kHeaderSize = 0x50;
constexpr uint64_t kDataStart = 0x200;             // file data starts here (header field fileDataOff)
constexpr size_t kMaxName = 255;
constexpr size_t kMaxPath = 0x300;                 // longest image path ("/a/b", the leading '/' counted)
constexpr uint64_t kMaxTable = 0xFFFFFFF0ull;      // a table this large cannot be addressed by u32 entry offsets
constexpr uint64_t kInPlaceDirTableMax = 64ull << 20, kInPlaceFileTableMax = 256ull << 20;
constexpr uint64_t kInPlaceMaxVisits = 1ull << 24;
constexpr size_t kOpenFilesMax = 16;
constexpr uint64_t kDecodedMax = 256ull << 20;
constexpr uint64_t kReadChunk = 1ull << 30;

uint64_t align_up(uint64_t v, uint64_t a) {   // a > 0
    const uint64_t over = v % a;
    return over ? v + (a - over) : v;
}

void put_u32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
void put_u64(std::vector<uint8_t>& b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }
uint32_t get_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t get_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// ------------------------------------------------------------------------------------------------------------------- text helpers

bool valid_utf8(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int len;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (int k = 1; k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

#ifdef _WIN32
bool to_utf8(const std::wstring& w, std::string* out) {
    out->clear();
    const int units = static_cast<int>(w.size());
    if (units == 0) return true;
    const DWORD strict = WC_ERR_INVALID_CHARS;   // an unpaired surrogate fails instead of becoming U+FFFD
    const int bytes = WideCharToMultiByte(CP_UTF8, strict, w.data(), units, nullptr, 0, nullptr, nullptr);
    if (bytes < 1) return false;
    out->assign(static_cast<size_t>(bytes), '\0');
    return WideCharToMultiByte(CP_UTF8, strict, w.data(), units, out->data(), bytes, nullptr, nullptr) == bytes;
}

std::wstring to_wide(const std::string& s) {
    std::wstring w;
    const int bytes = static_cast<int>(s.size());
    const int units = bytes ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), bytes, nullptr, 0) : 0;
    if (units < 1) return w;
    w.resize(static_cast<size_t>(units));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), bytes, w.data(), units);
    return w;
}

// Windows refuses paths of MAX_PATH characters and more unless they carry the \\?\ prefix (absolute, backslashes).
std::wstring long_form(std::wstring w) {
    if (w.size() < 240 || w.rfind(L"\\\\?\\", 0) == 0) return w;
    std::replace(w.begin(), w.end(), L'/', L'\\');
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}
#endif

// A host path (UTF-8) as a std::filesystem path the OS can open at any length.
stdfs::path host_path(const std::string& utf8) {
#ifdef _WIN32
    stdfs::path p(to_wide(utf8));
    std::error_code ec;
    if (p.is_relative() && !p.empty()) p = stdfs::absolute(p, ec);
    return stdfs::path(long_form(p.native()));
#else
    return stdfs::path(utf8);
#endif
}

// A host path for messages and for the sources (UTF-8, as the OS spells it; the long-path prefix taken off).
std::string shown_host(const stdfs::path& p) {
#ifdef _WIN32
    std::wstring w = p.native();
    if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) w = L"\\\\" + w.substr(8);
    else if (w.rfind(L"\\\\?\\", 0) == 0) w = w.substr(4);
    std::string s;
    to_utf8(w, &s);
    return s;
#else
    return p.string();
#endif
}

// ------------------------------------------------------------------------------------------------------------------- host files

// One host file opened for positioned reads (any number of threads at once).
class HostFile {
public:
    explicit HostFile(const std::string& utf8) {
#ifdef _WIN32
        const std::wstring w = utf8.empty() ? std::wstring() : host_path(utf8).native();
        if (!w.empty())
            h_ = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        fd_ = ::open(utf8.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    }
    ~HostFile() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    HostFile(const HostFile&) = delete;
    HostFile& operator=(const HostFile&) = delete;

    bool ok() const {
#ifdef _WIN32
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }

    // Exactly n bytes at off, or false (a short read is a failure).
    bool pread(uint64_t off, uint8_t* dst, uint64_t n) const {
        while (n) {
            const uint64_t chunk = std::min<uint64_t>(n, kReadChunk);
#ifdef _WIN32
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD got = 0;
            if (!ReadFile(h_, dst, static_cast<DWORD>(chunk), &got, &ov)) return false;
#else
            const ssize_t got = ::pread(fd_, dst, static_cast<size_t>(chunk), static_cast<off_t>(off));
            if (got < 0) return false;
#endif
            if (got == 0) return false;
            dst += got;
            off += static_cast<uint64_t>(got);
            n -= static_cast<uint64_t>(got);
        }
        return true;
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// The first bytes of a host file (fewer when it is shorter or cannot be read).
size_t read_head(const stdfs::path& p, uint8_t* out, size_t n) {
    HostFile f(shown_host(p));
    if (!f.ok()) return 0;
    for (size_t k = n; k > 0; --k)   // the file may be shorter than n: the longest prefix that reads
        if (f.pread(0, out, k)) return k;
    return 0;
}

// ------------------------------------------------------------------------------------------------------------------- the tree

struct Origin {                    // where a file's bytes come from
    enum Kind : uint8_t { kHost, kNup, kInPlace };
    Kind kind = kHost;
    std::string host;              // kHost: the file; kNup: the package (UTF-8)
    uint64_t offset = 0;           // kHost: 0; kNup: the file's extent in the package; kInPlace: its offset in the image
    nup::File nup;                 // kNup: the package's record (size, stored size, codec)
    std::shared_ptr<const nxsrc::Storage> image;   // kInPlace
};

struct Node {
    std::string name;              // UTF-8 bytes (the root: empty)
    bool dir = false;
    bool from_base = false;        // moved up from a lower layer by an overlay
    bool from_mod = false;         // came from a mod layer
    uint64_t size = 0;
    std::shared_ptr<Origin> origin;
    std::vector<std::unique_ptr<Node>> kids;   // folders: sorted by name (unsigned bytes) once a layer is built
    Node* parent = nullptr;        // layout: the folder that holds it (the root: none)
    uint32_t off = 0;              // layout: entry offset in its table
    uint32_t sibling = 0xFFFFFFFFu;   // layout: the next entry of the same kind in its folder
};

bool by_name(const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b) { return a->name < b->name; }

void sort_tree(Node& n) {
    if (!n.dir) return;
    std::sort(n.kids.begin(), n.kids.end(), by_name);
    for (auto& k : n.kids) sort_tree(*k);
}

void mark(Node& n, bool base, bool mod) {
    if (base) n.from_base = true;
    if (mod) n.from_mod = true;
    for (auto& k : n.kids) mark(*k, base, mod);
}

uint32_t count_files(const Node& n) {
    if (!n.dir) return 1;
    uint32_t c = 0;
    for (const auto& k : n.kids) c += count_files(*k);
    return c;
}

Node* child(Node& dir, const std::string& name) {
    auto it = std::lower_bound(dir.kids.begin(), dir.kids.end(), name,
                               [](const std::unique_ptr<Node>& a, const std::string& n) { return a->name < n; });
    return it != dir.kids.end() && (*it)->name == name ? it->get() : nullptr;
}

struct Counts {
    uint32_t mod_replaced = 0;   // a mod file over a file of the layer below
    uint32_t mod_deleted = 0;    // delete lines that removed something
    uint32_t mod_layers = 0;
};

// `lower` under `upper` (both sorted): the upper layer's entries win; what only the lower one has moves up (from_base).
bool overlay(Node& upper, Node& lower, const std::string& path, bool count_replaced, Counts& counts, std::string* err) {
    const size_t own = upper.kids.size();
    std::vector<std::unique_ptr<Node>> moved;
    for (auto& lk : lower.kids) {
        auto it = std::lower_bound(upper.kids.begin(), upper.kids.begin() + static_cast<std::ptrdiff_t>(own), lk->name,
                                   [](const std::unique_ptr<Node>& a, const std::string& n) { return a->name < n; });
        Node* uk = it != upper.kids.begin() + static_cast<std::ptrdiff_t>(own) && (*it)->name == lk->name ? it->get() : nullptr;
        const std::string sub = path + "/" + lk->name;
        if (!uk) {
            // PokoMac: only the base game's overlay (count_replaced false) marks what moves up as the base's; under a mod layer the
            // game's files keep their own marks, so the "files from the base" count stays right with a mod on (PokoMac's mods commit)
            mark(*lk, !count_replaced, false);
            moved.push_back(std::move(lk));
            continue;
        }
        if (uk->dir != lk->dir) {
            *err = "two RomFS layers disagree on " + sub + " (a file in one, a folder in the other)";
            return false;
        }
        if (uk->dir) {
            if (!overlay(*uk, *lk, sub, count_replaced, counts, err)) return false;
        } else if (count_replaced) {
            ++counts.mod_replaced;
        }
    }
    if (!moved.empty()) {
        for (auto& m : moved) upper.kids.push_back(std::move(m));
        std::sort(upper.kids.begin(), upper.kids.end(), by_name);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------------------------- sources

bool apple_metadata(const stdfs::path& p, const std::string& name) {
    if (name.size() > 2 && name[0] == '.' && name[1] == '_') {
        static const uint8_t kDouble[4] = {0x00, 0x05, 0x16, 0x07};
        uint8_t h[4];
        return read_head(p, h, 4) == 4 && std::memcmp(h, kDouble, 4) == 0;
    }
    if (name == ".DS_Store") {
        static const uint8_t kStore[8] = {0x00, 0x00, 0x00, 0x01, 'B', 'u', 'd', '1'};
        uint8_t h[8];
        return read_head(p, h, 8) == 8 && std::memcmp(h, kStore, 8) == 0;
    }
    return false;
}

// A host folder's entries into `dir` (image path `path`, "" for the root), recursively.
bool scan_folder(const stdfs::path& host, const std::string& path, Node& dir, std::string* err) {
    const std::string shown = path.empty() ? "/" : path;
    std::error_code ec;
#ifdef _WIN32
    const stdfs::path listing(long_form(host.native()));   // a deep folder: its path reaches MAX_PATH only below the root
#else
    const stdfs::path& listing = host;
#endif
    stdfs::directory_iterator it(listing, ec), end;
    if (ec) {
        *err = "cannot read folder " + shown;
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const stdfs::path p = it->path();
        std::string name;
#ifdef _WIN32
        const bool named = to_utf8(p.filename().native(), &name);
#else
        name = p.filename().native();   // the platform's names are UTF-8 already (only the length is checked here)
        const bool named = true;
#endif
        if (!named || name.empty() || name.size() > kMaxName) {
            *err = "name cannot be stored in a RomFS (1 to 255 bytes of UTF-8): " + shown + "/" + name;
            return false;
        }
        if (apple_metadata(p, name)) continue;   // macOS sidecars on exFAT / FAT drives: no part of the game
        const std::string sub = path + "/" + name;
        if (sub.size() > kMaxPath) {
            *err = "path too long for a RomFS (0x300 bytes): " + sub;
            return false;
        }
        std::error_code sec;
        const stdfs::file_status st = it->symlink_status(sec);
        if (sec) {
            *err = "cannot stat " + sub;
            return false;
        }
        auto node = std::make_unique<Node>();
        node->name = name;
        if (stdfs::is_symlink(st)) {
            *err = "symbolic link cannot be described: " + sub;
            return false;
        } else if (stdfs::is_directory(st)) {
            node->dir = true;
            if (!scan_folder(p, sub, *node, err)) return false;
        } else if (stdfs::is_regular_file(st)) {
            std::error_code zec;
            const uintmax_t size = it->file_size(zec);
            if (zec) {
                *err = "cannot read the size of " + sub;
                return false;
            }
            node->size = static_cast<uint64_t>(size);
            node->origin = std::make_shared<Origin>();
            node->origin->kind = Origin::kHost;
            node->origin->host = shown_host(p);
        } else {
            *err = "not a file or folder: " + sub;
            return false;
        }
        dir.kids.push_back(std::move(node));
    }
    if (ec) {
        *err = "cannot read folder " + shown;
        return false;
    }
    std::sort(dir.kids.begin(), dir.kids.end(), by_name);
    return true;
}

// The RomFS of a .nup (title id 0: the program, else that add-on) as a tree.
bool load_nup(const std::string& package, uint64_t title_id, std::unique_ptr<Node>& root, std::string* err) {
    std::vector<nup::File> files;
    if (title_id == 0) {
        nup::Content c;
        if (!nup::open(package, c, err)) return false;
        files = std::move(c.romfs);
        if (files.empty()) {
            *err = package + " has no RomFS";
            return false;
        }
    } else {
        std::vector<nup::AddOn> addons;
        if (!nup::open_addons(package, addons, err)) return false;
        for (auto& a : addons)
            if (a.title_id == title_id) files = a.romfs;   // the last one listed wins
        if (files.empty()) {
            *err = package + " has no RomFS for that add-on";
            return false;
        }
    }
    root = std::make_unique<Node>();
    root->dir = true;
    for (const nup::File& f : files) {
        Node* at = root.get();
        size_t pos = 0;
        std::string name;
        for (;;) {
            const size_t slash = f.path.find('/', pos);
            name = f.path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            if (slash == std::string::npos) break;
            Node* d = nullptr;
            for (auto& k : at->kids)
                if (k->dir && k->name == name) d = k.get();
            if (!d) {
                auto n = std::make_unique<Node>();
                n->name = name;
                n->dir = true;
                d = n.get();
                at->kids.push_back(std::move(n));
            }
            at = d;
            pos = slash + 1;
        }
        if (name.size() > kMaxName || f.path.size() + 1 > kMaxPath) {
            *err = "name or path too long for a RomFS: /" + f.path;
            return false;
        }
        auto n = std::make_unique<Node>();
        n->name = name;
        n->size = f.size;
        n->origin = std::make_shared<Origin>();
        n->origin->kind = Origin::kNup;
        n->origin->host = package;
        n->origin->offset = f.offset;
        n->origin->nup = f;
        at->kids.push_back(std::move(n));
    }
    sort_tree(*root);
    return true;
}

// A RomFS read in place (romfs.nxsrc): its own tables give the tree; every file is a range of the decrypted image.
bool load_inplace(const std::string& file, std::unique_ptr<Node>& root, std::string* err) {
    std::string msg;
    std::shared_ptr<const nxsrc::Storage> img = nxsrc::open(file, &msg);
    if (!img) {
        *err = msg;
        return false;
    }
    uint8_t h[kHeaderSize];
    if (img->size() < kHeaderSize || !img->read(0, h, kHeaderSize)) {
        *err = file + ": the RomFS header cannot be read";
        return false;
    }
    if (get_u64(h) != kHeaderSize) {
        *err = file + ": not a RomFS (wrong key, or a damaged package)";
        return false;
    }
    const uint64_t dir_off = get_u64(h + 0x18), dir_size = get_u64(h + 0x20);
    const uint64_t file_off = get_u64(h + 0x38), file_size = get_u64(h + 0x40), data_off = get_u64(h + 0x48);
    if (dir_size > kInPlaceDirTableMax || file_size > kInPlaceFileTableMax) {
        *err = file + ": RomFS tables too large";
        return false;
    }
    std::vector<uint8_t> dirs(static_cast<size_t>(dir_size)), files(static_cast<size_t>(file_size));
    if ((dir_size && !img->read(dir_off, dirs.data(), dir_size)) || (file_size && !img->read(file_off, files.data(), file_size))) {
        *err = file + ": the RomFS tables cannot be read";
        return false;
    }
    root = std::make_unique<Node>();
    root->dir = true;
    uint64_t folders = 0;   // folders taken off the walk (a looping table ends at the limits, as damage)
    struct Work { uint32_t entry; Node* node; std::string path; };
    std::vector<Work> todo{{0, root.get(), std::string()}};
    while (!todo.empty()) {
        Work w = std::move(todo.back());
        todo.pop_back();
        if (++folders > kInPlaceMaxVisits || static_cast<uint64_t>(w.entry) + 0x18 > dir_size) {
            *err = file + ": the RomFS directory table is damaged";
            return false;
        }
        const uint8_t* d = dirs.data() + w.entry;
        uint64_t chain = 0;
        for (uint32_t f = get_u32(d + 0x0C); f != kNone; ) {
            if (++chain > kInPlaceMaxVisits || static_cast<uint64_t>(f) + 0x20 > file_size) {
                *err = file + ": the RomFS file table is damaged";
                return false;
            }
            const uint8_t* e = files.data() + f;
            const uint32_t len = get_u32(e + 0x1C);
            if (len > kMaxName || static_cast<uint64_t>(f) + 0x20 + len > file_size) {
                *err = file + ": a RomFS file name is damaged";
                return false;
            }
            auto n = std::make_unique<Node>();
            n->name.assign(reinterpret_cast<const char*>(e + 0x20), len);
            const std::string path = w.path + "/" + n->name;
            n->size = get_u64(e + 0x10);
            const uint64_t at = data_off + get_u64(e + 0x08);
            if (at < data_off || at + n->size < at || at + n->size > img->size()) {
                *err = file + ": a RomFS file lies outside the image: " + path;
                return false;
            }
            if (path.size() > kMaxPath) {
                *err = "name or path too long for a RomFS: " + path;
                return false;
            }
            n->origin = std::make_shared<Origin>();
            n->origin->kind = Origin::kInPlace;
            n->origin->offset = at;
            n->origin->image = img;
            w.node->kids.push_back(std::move(n));
            f = get_u32(e + 0x04);
        }
        chain = 0;
        for (uint32_t c = get_u32(d + 0x08); c != kNone; ) {
            if (chain++ > kInPlaceMaxVisits || static_cast<uint64_t>(c) + 0x18 > dir_size) {
                *err = file + ": the RomFS directory table is damaged";
                return false;
            }
            const uint8_t* e = dirs.data() + c;
            const uint32_t len = get_u32(e + 0x14);
            if (len > kMaxName || static_cast<uint64_t>(c) + 0x18 + len > dir_size) {
                *err = file + ": a RomFS directory name is damaged";
                return false;
            }
            auto n = std::make_unique<Node>();
            n->dir = true;
            n->name.assign(reinterpret_cast<const char*>(e + 0x18), len);
            Node* np = n.get();
            const std::string path = w.path + "/" + n->name;
            w.node->kids.push_back(std::move(n));
            todo.push_back({c, np, path});
            c = get_u32(e + 0x04);
        }
    }
    sort_tree(*root);
    return true;
}

// Any source a layer can be (fs_romfs.h build): an add-on of a .nup (`<package>#aoc=<id>`), an in-place RomFS, a .nup, or a host folder.
bool load_source(const std::string& given, std::unique_ptr<Node>& root, std::string* err) {
    std::string path = given, package, file;
    uint64_t id = 0;
    const bool addon = nup::split_addon_romfs_name(given, package, id);
    if (addon) path = package;
    else if (nxsrc::is_source(path, &file)) return load_inplace(file, root, err);
#ifdef _WIN32
    const bool usable = !path.empty() && valid_utf8(path);
#else
    const bool usable = !path.empty();   // the platform's paths are bytes; only an empty one is refused here
#endif
    if (!usable) {
        *err = "host folder name is empty or not valid UTF-8";
        return false;
    }
    std::error_code ec;
    const stdfs::path abs = stdfs::absolute(host_path(path), ec);
    if (ec) {
        *err = "not a folder: " + path;
        return false;
    }
    const std::string abs_s = shown_host(abs);
    if (nup::is_nup(abs_s)) return load_nup(abs_s, id, root, err);
    if (!stdfs::is_directory(abs, ec)) {
        *err = "not a folder: " + path;
        return false;
    }
    root = std::make_unique<Node>();
    root->dir = true;
    return scan_folder(abs, std::string(), *root, err);
}

// ------------------------------------------------------------------------------------------------------------------- mod layer list

std::string trim_end(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// Removes the entry at image path `p` ("/a/b"; exact per component). True when something was removed.
bool remove_path(Node& root, const std::string& p) {
    std::vector<std::string> parts;
    size_t pos = 1;
    for (;;) {
        const size_t slash = p.find('/', pos);
        parts.push_back(p.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos));
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    Node* at = &root;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) return false;
        auto it = std::find_if(at->kids.begin(), at->kids.end(), [&](const std::unique_ptr<Node>& k) { return k->name == parts[i]; });
        if (it == at->kids.end()) return false;
        if (i + 1 == parts.size()) {
            at->kids.erase(it);
            return true;
        }
        if (!(*it)->dir) return false;
        at = it->get();
    }
    return false;
}

bool apply_layer(std::unique_ptr<Node>& tree, const std::string& folder, Counts& counts, std::string* err) {
    std::unique_ptr<Node> layer;
    std::string msg;
    if (!load_source(folder, layer, &msg)) {
        *err = "mod RomFS: " + msg;
        return false;
    }
    mark(*layer, false, true);
    if (!overlay(*layer, *tree, std::string(), true, counts, err)) return false;
    tree = std::move(layer);
    return true;
}

bool apply_layer_list(std::unique_ptr<Node>& tree, const std::string& list, Counts& counts, std::string* err) {
    if (!valid_utf8(list)) {
        *err = "mod layer list name is not valid UTF-8";
        return false;
    }
    std::string text;
    {
        HostFile f(list);
        std::error_code ec;
        const uintmax_t size = stdfs::file_size(host_path(list), ec);
        text.resize(ec ? 0 : static_cast<size_t>(size));
        if (!f.ok() || ec || (size && !f.pread(0, reinterpret_cast<uint8_t*>(text.data()), size))) {
            *err = "mod layer list cannot be read: " + list;
            return false;
        }
    }
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string line = trim_end(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (line.empty() || line.front() == '#') continue;   // blank or a comment
        if (line.rfind("delete ", 0) == 0) {
            std::string p = line.substr(7);
            std::replace(p.begin(), p.end(), '\\', '/');
            if (p.rfind('/', 0) != 0) p.insert(p.begin(), '/');   // an image path starts at the root
            if (remove_path(*tree, p)) ++counts.mod_deleted;
        } else if (line.rfind("layer ", 0) == 0) {
            const std::string folder = line.substr(6);
            if (folder.empty() || !valid_utf8(folder)) {
                *err = "mod layer list " + list + ": a folder name that is not valid UTF-8";
                return false;
            }
            std::string where = folder;
#ifdef _WIN32
            const stdfs::path fp(to_wide(folder));
            if (fp.is_relative() && !to_utf8((stdfs::path(to_wide(list)).parent_path() / fp).native(), &where)) where = folder;
#else
            const stdfs::path fp(folder);
            if (fp.is_relative()) where = (stdfs::path(list).parent_path() / fp).native();
#endif
            if (!apply_layer(tree, where, counts, err)) return false;
            ++counts.mod_layers;
        } else {
            *err = "mod layer list " + list + ": a line that is neither 'layer <folder>' nor 'delete <path>': " + line;
            return false;
        }
    }
    return true;
}

// The plain copies of a package's compressed files (unpacked_dir) put in for them: same shape, other source.
void apply_unpacked(Node& game, Node& copy, RomfsImage::Stats& st) {
    for (auto& ck : copy.kids) {
        Node* gk = child(game, ck->name);
        if (!gk || gk->dir != ck->dir) {
            st.unpacked_ignored += count_files(*ck);
            continue;
        }
        if (gk->dir) {
            apply_unpacked(*gk, *ck, st);
            continue;
        }
        if (!gk->from_mod && gk->origin && gk->origin->kind == Origin::kNup && gk->origin->nup.codec == nup::kCodecZstd && ck->origin &&
            ck->origin->kind == Origin::kHost && ck->size == gk->size) {
            gk->origin = ck->origin;
            ++st.unpacked_files;
            st.unpacked_bytes += gk->size;
        } else {
            ++st.unpacked_ignored;
        }
    }
}

uint32_t name_hash(uint32_t parent, const std::string& name) {
    uint32_t h = 0x075BCD15u ^ parent;   // the seed (123456789), mixed with the parent entry
    for (const char ch : name) {
        h = (h >> 5) | (h << 27);
        h ^= static_cast<unsigned char>(ch);
    }
    return h;
}

// Hash table size for n entries: 3 below 3, odd up to 17, else the first count from n that 2, 3, 5, 7, 11, 13 and 17 do not divide.
uint32_t bucket_count(uint64_t n) {
    if (n < 3) return 3;
    if (n < 19) return static_cast<uint32_t>(n | 1);
    for (uint64_t c = n;; ++c) {
        bool clear = true;
        for (const uint64_t p : {2, 3, 5, 7, 11, 13, 17})
            if (c % p == 0) {
                clear = false;
                break;
            }
        if (clear) return static_cast<uint32_t>(c);
    }
}

struct FileRec {
    const Node* node;
    std::string path;      // image path
    uint64_t data = 0;     // image offset of the data
};

}  // namespace

// ------------------------------------------------------------------------------------------------------------------- the image

struct RomfsImage::Image {
    bool built = false;
    uint64_t size = 0;
    uint64_t tables_at = 0;                 // image offset of the directory hash table
    std::vector<uint8_t> head;              // [0, 0x200): header and zeros
    std::vector<uint8_t> tables;            // [tables_at, size)
    std::vector<FileRec> files;             // data order (= file table order)
    std::vector<std::shared_ptr<Origin>> origins;   // per file (kept alive with the image)
    std::unordered_map<std::string, size_t> by_path;
    std::unique_ptr<std::atomic<bool>[]> touched;
    Stats stats;
    std::unique_ptr<Node> tree;             // owns the nodes FileRec points at

    // host files open at once (least recently used dropped) and decoded zstd files (256 MiB, least recently used dropped)
    mutable std::mutex lock;
    mutable std::list<std::pair<std::string, std::shared_ptr<HostFile>>> open_files;
    mutable std::list<std::pair<const Origin*, std::shared_ptr<const std::vector<uint8_t>>>> decoded;
    mutable uint64_t decoded_bytes = 0;

    std::shared_ptr<HostFile> host(const std::string& path) const {
        {
            std::lock_guard<std::mutex> g(lock);
            for (auto it = open_files.begin(); it != open_files.end(); ++it)
                if (it->first == path) {
                    open_files.splice(open_files.begin(), open_files, it);
                    return it->second;
                }
        }
        auto f = std::make_shared<HostFile>(path);
        if (!f->ok()) return nullptr;
        std::lock_guard<std::mutex> g(lock);
        open_files.emplace_front(path, f);
        while (open_files.size() > kOpenFilesMax) open_files.pop_back();
        return f;
    }

    std::shared_ptr<const std::vector<uint8_t>> decode(const Origin& o) const {
        {
            std::lock_guard<std::mutex> g(lock);
            for (auto it = decoded.begin(); it != decoded.end(); ++it)
                if (it->first == &o) {
                    decoded.splice(decoded.begin(), decoded, it);
                    return it->second;
                }
        }
        std::vector<uint8_t> frame(static_cast<size_t>(o.nup.stored));
        auto out = std::make_shared<std::vector<uint8_t>>();
        std::string err;
        const std::shared_ptr<HostFile> f = host(o.host);
        if (!f || (o.nup.stored && !f->pread(o.nup.offset, frame.data(), o.nup.stored))) {
            err = "cannot read " + o.nup.path + " from " + o.host;
        } else if (!nup::decode(o.nup, frame.data(), frame.size(), *out, &err)) {
            if (err.empty()) err = "cannot decode " + o.nup.path + " from " + o.host;
        } else {
            std::lock_guard<std::mutex> g(lock);
            for (auto& d : decoded)
                if (d.first == &o) return d.second;   // another reader decoded it first: that copy stays
            decoded.emplace_front(&o, out);
            decoded_bytes += out->size();
            while (decoded.size() > 1 && decoded_bytes > kDecodedMax) {
                decoded_bytes -= decoded.back().second->size();
                decoded.pop_back();
            }
            return out;
        }
        NXLOG_E("fs", "romfs: %s", err.c_str());
        return nullptr;
    }

    bool read_file(size_t i, uint64_t at, uint8_t* dst, uint64_t n) const {
        const FileRec& fr = files[i];
        if (!touched[i].exchange(true)) {
            NXLOG_D("fs", "romfs: first read of %s", fr.path.c_str());
            diag_note_file("rom", fr.path.c_str());
        }
        const Origin& o = *origins[i];
        if (o.kind == Origin::kNup && o.nup.codec != nup::kCodecStored) {
            const auto data = decode(o);
            if (!data || at + n > data->size()) return false;
            std::memcpy(dst, data->data() + at, static_cast<size_t>(n));
            return true;
        }
        const mem::ww::OsWriteScope ws(dst, n);   // the OS cannot read into a page the write tracker made read-only
        if (o.kind == Origin::kInPlace) return o.image->read(o.offset + at, dst, n);
        const std::shared_ptr<HostFile> f = host(o.host);
        return f && f->pread(o.offset + at, dst, n);
    }

    void reset() {
        built = false;
        size = tables_at = 0;
        head.clear();
        tables.clear();
        files.clear();
        origins.clear();
        by_path.clear();
        touched.reset();
        stats = Stats();
        tree.reset();
        std::lock_guard<std::mutex> g(lock);
        open_files.clear();
        decoded.clear();
        decoded_bytes = 0;
    }

    bool layout(std::string* err);
};

bool RomfsImage::Image::layout(std::string* err) {
    struct DirRec { Node* node; std::string path; };
    std::vector<DirRec> dirs;
    std::vector<std::pair<Node*, std::string>> flist;
    {
        std::vector<std::pair<Node*, std::string>> todo{{tree.get(), std::string()}};
        while (!todo.empty()) {
            auto [n, path] = std::move(todo.back());
            todo.pop_back();
            dirs.push_back({n, path});
            for (auto& k : n->kids) {
                k->parent = n;
                if (k->dir) todo.push_back({k.get(), path + "/" + k->name});
                else flist.push_back({k.get(), path + "/" + k->name});
            }
        }
    }
    std::sort(dirs.begin(), dirs.end(), [](const DirRec& a, const DirRec& b) { return a.path < b.path; });
    std::sort(flist.begin(), flist.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    uint64_t dir_bytes = 0;
    uint64_t file_bytes = 0;
    for (auto& d : dirs) {
        if (dir_bytes >= kMaxTable) break;
        d.node->off = static_cast<uint32_t>(dir_bytes);
        dir_bytes += 0x18 + align_up(d.node->name.size(), 4);
    }
    for (auto& f : flist) {
        if (file_bytes >= kMaxTable) break;
        f.first->off = static_cast<uint32_t>(file_bytes);
        file_bytes += 0x20 + align_up(f.first->name.size(), 4);
    }
    if (dir_bytes >= kMaxTable || file_bytes >= kMaxTable) {
        *err = "too many entries for a RomFS";
        return false;
    }
    for (const DirRec& d : dirs) {        // sibling chains: each folder's subfolders, and its files, in name order
        Node* prev_dir = nullptr;
        Node* prev_file = nullptr;
        for (auto& k : d.node->kids) {
            Node*& prev = k->dir ? prev_dir : prev_file;
            if (prev) prev->sibling = k->off;
            prev = k.get();
        }
    }
    // file data, path order, each file 0x10-aligned
    uint64_t end = 0;
    files.reserve(flist.size());
    for (auto& f : flist) {
        const uint64_t at = align_up(end, 0x10);
        files.push_back({f.first, f.second, kDataStart + at});
        origins.push_back(f.first->origin);
        end = at + f.first->size;
    }
    const uint32_t dir_buckets = bucket_count(dirs.size()), file_buckets = bucket_count(flist.size());
    tables_at = align_up(kDataStart + end, 4);
    const uint64_t dir_hash_at = 0, dir_tab_at = 4ull * dir_buckets, file_hash_at = dir_tab_at + dir_bytes,
                   file_tab_at = file_hash_at + 4ull * file_buckets;
    tables.assign(static_cast<size_t>(file_tab_at + file_bytes), 0);
    std::vector<uint32_t> dir_heads(dir_buckets, kNone), file_heads(file_buckets, kNone);
    for (const DirRec& d : dirs) {
        const Node& n = *d.node;
        const uint32_t parent_off = n.parent ? n.parent->off : 0;
        uint32_t child_dir = kNone, child_file = kNone;
        for (const auto& k : n.kids) {
            if (k->dir && child_dir == kNone) child_dir = k->off;
            if (!k->dir && child_file == kNone) child_file = k->off;
        }
        const uint32_t b = name_hash(parent_off, n.name) % dir_buckets;
        const size_t at = static_cast<size_t>(dir_tab_at + n.off);
        put_u32(tables, at + 0x00, parent_off);
        put_u32(tables, at + 0x04, n.sibling);
        put_u32(tables, at + 0x08, child_dir);
        put_u32(tables, at + 0x0C, child_file);
        put_u32(tables, at + 0x10, dir_heads[b]);
        put_u32(tables, at + 0x14, static_cast<uint32_t>(n.name.size()));
        std::memcpy(tables.data() + at + 0x18, n.name.data(), n.name.size());
        dir_heads[b] = n.off;
    }
    for (size_t i = 0; i < files.size(); ++i) {
        const Node& n = *files[i].node;
        const uint32_t b = name_hash(n.parent->off, n.name) % file_buckets;
        const size_t at = static_cast<size_t>(file_tab_at + n.off);
        put_u32(tables, at + 0x00, n.parent->off);
        put_u32(tables, at + 0x04, n.sibling);
        put_u64(tables, at + 0x08, files[i].data - kDataStart);
        put_u64(tables, at + 0x10, n.size);
        put_u32(tables, at + 0x18, file_heads[b]);
        put_u32(tables, at + 0x1C, static_cast<uint32_t>(n.name.size()));
        std::memcpy(tables.data() + at + 0x20, n.name.data(), n.name.size());
        file_heads[b] = n.off;
    }
    for (uint32_t b = 0; b < dir_buckets; ++b) put_u32(tables, static_cast<size_t>(dir_hash_at + 4ull * b), dir_heads[b]);
    for (uint32_t b = 0; b < file_buckets; ++b) put_u32(tables, static_cast<size_t>(file_hash_at + 4ull * b), file_heads[b]);
    head.assign(kDataStart, 0);
    put_u64(head, 0x00, kHeaderSize);
    put_u64(head, 0x08, tables_at + dir_hash_at);
    put_u64(head, 0x10, 4ull * dir_buckets);
    put_u64(head, 0x18, tables_at + dir_tab_at);
    put_u64(head, 0x20, dir_bytes);
    put_u64(head, 0x28, tables_at + file_hash_at);
    put_u64(head, 0x30, 4ull * file_buckets);
    put_u64(head, 0x38, tables_at + file_tab_at);
    put_u64(head, 0x40, file_bytes);
    put_u64(head, 0x48, kDataStart);
    size = tables_at + tables.size();
    touched = std::make_unique<std::atomic<bool>[]>(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        touched[i] = false;
        by_path.emplace(files[i].path, i);
        stats.data_bytes += files[i].node->size;
        if (files[i].node->from_base) ++stats.base_files;
        if (files[i].node->from_mod) ++stats.mod_files;
    }
    stats.files = static_cast<uint32_t>(files.size());
    stats.dirs = static_cast<uint32_t>(dirs.size());
    stats.data_offset = kDataStart;
    return true;
}

RomfsImage::RomfsImage() : impl_(std::make_unique<Image>()) {}
RomfsImage::~RomfsImage() = default;

bool RomfsImage::build(const std::string& host_dir, std::string* err,
                       const std::string& base_dir, const std::string& mod_dir, const std::string& unpacked_dir) {
    Image& m = *impl_;
    m.reset();
    std::string e;
    auto fail = [&](const std::string& msg) {
        m.reset();
        if (err) *err = msg;
        return false;
    };
    std::unique_ptr<Node> tree;
    if (!load_source(host_dir, tree, &e)) return fail(e);
    Counts counts;
    if (base_dir.size()) {   // an update over its base game
        std::unique_ptr<Node> base;
        if (!load_source(base_dir, base, &e)) return fail("base game RomFS: " + e);
        if (!overlay(*tree, *base, std::string(), false, counts, &e)) return fail(e);
    }
    if (!mod_dir.empty()) {
        const bool list = mod_dir.size() > 4 && (mod_dir.compare(mod_dir.size() - 4, 4, ".txt") == 0 ||
                                                 mod_dir.compare(mod_dir.size() - 4, 4, ".TXT") == 0);
        if (list) {
            if (!apply_layer_list(tree, mod_dir, counts, &e)) return fail(e);
        } else {
            if (!apply_layer(tree, mod_dir, counts, &e)) return fail(e);
            counts.mod_layers = 1;
        }
    }
    Stats st;
    st.mod_replaced = counts.mod_replaced;
    st.mod_deleted = counts.mod_deleted;
    st.mod_layers = counts.mod_layers;
    {
        std::vector<const Node*> todo{tree.get()};
        while (!todo.empty()) {
            const Node* n = todo.back();
            todo.pop_back();
            for (const auto& k : n->kids) {
                if (k->dir) todo.push_back(k.get());
                else if (k->origin && k->origin->kind == Origin::kNup && k->origin->nup.codec == nup::kCodecZstd) {
                    ++st.packed_files;
                    st.packed_bytes += k->size;
                }
            }
        }
    }
    if (unpacked_dir.size()) {   // plain copies of compressed package files
        std::unique_ptr<Node> copy;
        std::string msg;
        if (nup::is_nup(unpacked_dir)) {
            st.unpacked_error = "it is a package, not a folder";
        } else if (!load_source(unpacked_dir, copy, &msg)) {
            st.unpacked_error = msg;
        } else {
            apply_unpacked(*tree, *copy, st);
        }
    }
    m.tree = std::move(tree);
    m.stats = st;
    if (!m.layout(&e)) return fail(e);
    m.built = true;
    return true;
}

bool RomfsImage::built() const { return impl_->built; }
uint64_t RomfsImage::size() const { return impl_->size; }
RomfsImage::Stats RomfsImage::stats() const { return impl_->stats; }

bool RomfsImage::file_extent(const std::string& path, uint64_t* offset, uint64_t* size) const {
    const Image& m = *impl_;
    if (!m.built) return false;
    const auto it = m.by_path.find(path);
    if (it == m.by_path.end()) return false;
    if (offset) *offset = m.files[it->second].data;
    if (size) *size = m.files[it->second].node->size;
    return true;
}

bool RomfsImage::read(uint64_t offset, void* dst_v, uint64_t len) const {
    const Image& m = *impl_;
    if (!m.built || offset > m.size || len > m.size - offset) return false;
    uint8_t* dst = static_cast<uint8_t*>(dst_v);
    uint64_t pos = offset;
    const uint64_t end = offset + len;
    if (pos < kDataStart && pos < end) {                       // header and zeros
        const uint64_t n = std::min(end, kDataStart) - pos;
        std::memcpy(dst, m.head.data() + pos, static_cast<size_t>(n));
        dst += n;
        pos += n;
    }
    if (pos < m.tables_at && pos < end) {                     // file data and the zero gaps between files
        const uint64_t stop = std::min(end, m.tables_at);
        auto it = std::upper_bound(m.files.begin(), m.files.end(), pos,
                                   [](uint64_t p, const FileRec& f) { return p < f.data + f.node->size; });
        size_t i = static_cast<size_t>(it - m.files.begin());
        while (pos < stop) {
            if (i >= m.files.size() || pos < m.files[i].data) {   // a gap: zeros up to the next file's data (or the tables)
                const uint64_t to = std::min(stop, i < m.files.size() ? m.files[i].data : stop);
                std::memset(dst, 0, static_cast<size_t>(to - pos));
                dst += to - pos;
                pos = to;
                continue;
            }
            const FileRec& f = m.files[i];
            const uint64_t fend = f.data + f.node->size;
            if (pos >= fend) {
                ++i;
                continue;
            }
            const uint64_t n = std::min(stop, fend) - pos;
            if (!m.read_file(i, pos - f.data, dst, n)) return false;
            dst += n;
            pos += n;
            ++i;
        }
    }
    if (pos < end) {                                           // the tables
        std::memcpy(dst, m.tables.data() + (pos - m.tables_at), static_cast<size_t>(end - pos));
    }
    return true;
}

}  // namespace nxrt::services::fs
