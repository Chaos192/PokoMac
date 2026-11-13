// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch) (its runtime/tests/services/test_romfs_unpacked.cpp layer-list case,
// without the .nup package part PokoMac's game folder does not use).
// The RomFS mod layers ([fs] romfs_mod, runtime/src/services/fs_romfs.cpp, docs/MODS.md) on host folders made here: a mod layer list
// (the mod manager mods.py writes one: "layer <folder>" and "delete <path>" lines, lowest priority first), a single mod folder (what
// [fs] romfs_mod named before the mod manager, kept as it was), the base game layer under both, and the refusals.
// Run: nxrt_romfs_layers_test [<work dir>]   (default: the system temp folder)
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "../../src/services/fs_romfs.h"

namespace {

int g_fails = 0;

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);            \
            g_fails++;                                                          \
        }                                                                       \
    } while (0)

void put(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    FILE* f = nullptr;
#if defined(_WIN32)
    _wfopen_s(&f, p.wstring().c_str(), L"wb");
#else
    f = std::fopen(p.string().c_str(), "wb");
#endif
    if (!f) {
        std::printf("FAIL cannot write %s\n", p.string().c_str());
        g_fails++;
        return;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

std::string read_whole(const nxrt::services::fs::RomfsImage& img, const char* path) {
    uint64_t off = 0, size = 0;
    if (!img.file_extent(path, &off, &size)) return "<absent>";
    std::string s(static_cast<size_t>(size), '\0');
    return img.read(off, s.data(), size) ? s : "<read failed>";
}

}  // namespace

int main(int argc, char** argv) {
    namespace sfs = std::filesystem;
    using nxrt::services::fs::RomfsImage;
    std::error_code ec;
    const sfs::path dir = (argc > 1 ? sfs::path(argv[1]) : sfs::temp_directory_path()) / "nxrt_romfs_layers_test";
    sfs::remove_all(dir, ec);
    // the game (update) and its base game, then three mods
    put(dir / "game" / "a.bin", "GAME-A");
    put(dir / "game" / "b.bin", "GAME-B");
    put(dir / "game" / "sub" / "c.bin", "GAME-C");
    put(dir / "base" / "base_only.bin", "BASE");
    put(dir / "base" / "sub" / "base_c.bin", "BASE-C");
    put(dir / "mods" / "low" / "a.bin", "LOW-A");
    put(dir / "mods" / "low" / "sub" / "new.bin", "LOW-NEW");
    put(dir / "mods" / "high" / "a.bin", "HIGH-A");
    put(dir / "mods" / "high" / "sub_file.bin", "H");
    const std::string game = (dir / "game").string(), base = (dir / "base").string();

    // 1. a layer list: relative and absolute folders, CRLF, comments; the later layer wins, deletions apply to what is built so far
    //    (the base game's files too)
    put(dir / "mods" / "layers.txt", "# lowest first\nlayer low\ndelete /b.bin\nlayer " + (dir / "mods" / "high").string() +
                                         "\r\ndelete sub/c.bin\ndelete /base_only.bin\ndelete /no/such/file\n");
    {
        std::string err;
        RomfsImage img;
        CHECK(img.build(game, &err, base, (dir / "mods" / "layers.txt").string()));
        const RomfsImage::Stats s = img.stats();
        CHECK(s.mod_layers == 2);
        CHECK(s.mod_deleted == 3);                              // the missing path is not counted
        CHECK(read_whole(img, "/a.bin") == "HIGH-A");          // the later layer wins
        CHECK(read_whole(img, "/b.bin") == "<absent>");        // a game file deleted
        CHECK(read_whole(img, "/sub/c.bin") == "<absent>");    // deleted after both layers
        CHECK(read_whole(img, "/base_only.bin") == "<absent>");  // a base game file deleted
        CHECK(read_whole(img, "/sub/new.bin") == "LOW-NEW");   // a file only a mod has
        CHECK(read_whole(img, "/sub/base_c.bin") == "BASE-C");
        CHECK(read_whole(img, "/sub_file.bin") == "H");
        CHECK(s.files == 4);
        CHECK(s.mod_files == 3);                                // a.bin (high), sub/new.bin, sub_file.bin
        CHECK(s.mod_replaced == 2);                             // low's a.bin over the game's, high's a.bin over low's
        CHECK(s.base_files == 1);                               // only sub/base_c.bin: the game's own files are not the base's
    }

    // 2. a single mod folder, as [fs] romfs_mod named before the mod manager: unchanged
    {
        std::string err;
        RomfsImage img;
        CHECK(img.build(game, &err, base, (dir / "mods" / "low").string()));
        const RomfsImage::Stats s = img.stats();
        CHECK(s.mod_layers == 1);
        CHECK(s.mod_deleted == 0);
        CHECK(read_whole(img, "/a.bin") == "LOW-A");
        CHECK(read_whole(img, "/b.bin") == "GAME-B");
        CHECK(read_whole(img, "/sub/c.bin") == "GAME-C");
        CHECK(read_whole(img, "/base_only.bin") == "BASE");
        CHECK(s.files == 6);
        CHECK(s.mod_files == 2);
        CHECK(s.mod_replaced == 1);
        CHECK(s.base_files == 2);
    }

    // 3. no mod: the game over its base, as before
    {
        std::string err;
        RomfsImage img;
        CHECK(img.build(game, &err, base));
        const RomfsImage::Stats s = img.stats();
        CHECK(s.mod_layers == 0 && s.mod_files == 0 && s.base_files == 2 && s.files == 5);
        CHECK(read_whole(img, "/a.bin") == "GAME-A");
    }

    // 4. refusals, each with its reason: a missing layer folder, a line that is neither layer nor delete, a list that cannot be read
    {
        put(dir / "mods" / "bad1.txt", "layer none\n");
        put(dir / "mods" / "bad2.txt", "layer low\nreplace a.bin\n");
        std::string err;
        RomfsImage img;
        CHECK(!img.build(game, &err, "", (dir / "mods" / "bad1.txt").string()));
        CHECK(err.find("mod RomFS") != std::string::npos);
        err.clear();
        CHECK(!img.build(game, &err, "", (dir / "mods" / "bad2.txt").string()));
        CHECK(err.find("neither") != std::string::npos);
        err.clear();
        CHECK(!img.build(game, &err, "", (dir / "mods" / "missing.txt").string()));
        CHECK(err.find("cannot be read") != std::string::npos);
    }

    sfs::remove_all(dir, ec);
    std::printf("%s (%d failed)\n", g_fails ? "FAILED" : "romfs layers: all passed", g_fails);
    return g_fails ? 1 : 0;
}
