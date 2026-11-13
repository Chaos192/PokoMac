// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Reader for .nup game packages (https://github.com/a55uka/nup, SPEC.md format 1): one file holding a title's ExeFS and RomFS trees.
// The game is served straight from it: RomFS reads and ExeFS module loads become positioned reads of the package file, so nothing is
// unpacked to disk. A stored file (codec 0, "raw") is read in place. A zstd file (codec 1) is one frame for the whole file, with no
// random access into it: it is decompressed whole when it is read (fs_romfs.cpp keeps recent ones in memory). Packages made by `nup
// pack` with zstandard compress most small files (Super Mario Bros. Wonder's: 11,992 of 19,034 RomFS files, every ExeFS file).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::nup {

constexpr uint8_t kCodecStored = 0, kCodecZstd = 1;

struct File {
    std::string path;           // inside its subtree, '/'-separated, no leading slash ("data/a.bin")
    uint64_t size = 0;          // bytes of the file
    uint64_t offset = 0;        // absolute offset of the data (stored bytes or the zstd frame) in the package file
    uint64_t stored = 0;        // bytes at offset: size for a stored file, the frame's size for a zstd one
    uint8_t codec = kCodecStored;
};

struct Content {
    uint32_t version = 0;       // of the package used (0 = base game)
    std::vector<File> exefs;    // empty when the package has no ExeFS
    std::vector<File> romfs;    // empty when the package has no RomFS
};

// True when `path` names a regular file whose first bytes are the .nup magic.
bool is_nup(const std::string& path);

// Reads the table of contents and lists the ExeFS and RomFS of the package to run: the highest update, else the base game (add-ons
// are ignored). False with *err set when the file is not a valid package or holds a file of a codec this reader does not know.
bool open(const std::string& path, Content& out, std::string* err = nullptr);

// The add-on content packages (kind 2, with their kind 3 updates layered on top as for the program) of the package: one entry per add-on
// title with a RomFS. A Switch 2 Edition keeps its upgrade pack here (Super Mario Bros. Wonder: 010015100B515001, 5 GiB).
struct AddOn {
    uint64_t title_id = 0;
    uint32_t version = 0;
    std::vector<File> romfs;
};
bool open_addons(const std::string& path, std::vector<AddOn>& out, std::string* err = nullptr);

// A RomFS "folder" name for an add-on served in place: `<package>#aoc=<16 hex digits>` (fs_romfs.cpp builds its image from it).
std::string addon_romfs_name(const std::string& package, uint64_t title_id);
// Splits such a name; false for any other path.
bool split_addon_romfs_name(const std::string& name, std::string& package, uint64_t& title_id);

// Reads a whole file of the package (an ExeFS module), decompressed.
bool read_file(const std::string& path, const File& f, std::vector<uint8_t>& out, std::string* err = nullptr);

// The file's bytes from its stored extent (`frame`, f.stored bytes read at f.offset): a copy for a stored file, the decompressed frame
// for a zstd one, which must give exactly f.size bytes. False with *err set otherwise (or when this build has no zstd decoder).
bool decode(const File& f, const uint8_t* frame, uint64_t frame_size, std::vector<uint8_t>& out, std::string* err = nullptr);

}  // namespace nxrt::nup
