// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// A game's RomFS as one byte-addressed image, built in memory over its sources (fs_romfs.cpp): a host folder, a .nup package (nxrt/nup.h)
// or a RomFS read in place from an .nsp / .xci (romfs_source.h), with the base game's files under an update, mod layers over both and
// plain copies of a package's compressed files. fsp-srv serves the image (OpenDataStorageByCurrentProcess and its siblings); the game's
// SDK parses it itself, so it is a standard RomFS: a 0x50-byte header, the file data from 0x200, then the directory hash table, the
// directory table, the file hash table and the file table. The layout follows the public descriptions of the format (libnx
// romfs_dev.h / romfs_dev.c, ISC: entry layouts, the name hash, buckets by modulo); file data is read from the sources when the game
// reads it, never copied.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace nxrt::services::fs {

class RomfsImage {
public:
    struct Stats {
        uint32_t files = 0;             // files in the image
        uint32_t dirs = 0;              // folders, the root included
        uint64_t data_bytes = 0;        // sum of the file sizes
        uint64_t data_offset = 0;       // where file data starts in the image (0x200 once built)
        uint32_t base_files = 0;        // files an overlay moved up from a lower layer (the base game under an update, ...)
        uint32_t mod_files = 0;         // files that came from a mod layer
        uint32_t mod_replaced = 0;      // times a mod file stood where the layer below it had a file
        uint32_t mod_deleted = 0;       // 'delete' lines of a layer list that removed an entry
        uint32_t mod_layers = 0;        // 1 for a mod folder, the 'layer' lines applied for a list, 0 without mods
        uint32_t packed_files = 0;      // files stored zstd-compressed in a .nup
        uint64_t packed_bytes = 0;      // their sizes
        uint32_t unpacked_files = 0;    // of those, the files served from their plain copy in the unpacked folder
        uint64_t unpacked_bytes = 0;    // their sizes
        uint32_t unpacked_ignored = 0;  // files of the unpacked folder that stand in for nothing
        std::string unpacked_error;     // why the unpacked folder was not used at all (empty: used, or none given)
    };

    RomfsImage();
    ~RomfsImage();

    // Builds the image of `host_dir` (a folder, a .nup, `<package>#aoc=<id>` or a romfs.nxsrc source) with `base_dir` under it, the mod
    // folder or layer list (`*.txt`) `mod_dir` over both and the plain copies in `unpacked_dir`. False with *err set when it cannot be
    // built; the image is then unbuilt.
    bool build(const std::string& host_dir, std::string* err = nullptr,
               const std::string& base_dir = std::string(), const std::string& mod_dir = std::string(),
               const std::string& unpacked_dir = std::string());
    bool built() const;
    uint64_t size() const;
    // `len` bytes of the image at `offset` into dst (any number of threads at once). False when the range is outside the image or a
    // source cannot be read.
    bool read(uint64_t offset, void* dst, uint64_t len) const;
    Stats stats() const;
    // Where the file at image path `path` ("/dir/name") lies in the image: its data offset and size. False for a folder or no such file.
    bool file_extent(const std::string& path, uint64_t* offset, uint64_t* size) const;

private:
    struct Image;                  // the built image (fs_romfs.cpp)
    std::unique_ptr<Image> impl_;
};

}  // namespace nxrt::services::fs
