// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// A RomFS read in place from the player's own .nsp / .xci (romfs_source.cpp). The intake (tools/intake/nca_intake.py --romfs-in-place)
// writes a romfs.nxsrc instead of extracting the RomFS: a small JSON file describing, as a tree of storage nodes, how to get the decrypted
// RomFS image out of the package: file ranges, split parts, AES-128-CTR, an update's AES-CTR-EX runs and its relocation table over the base
// game (coord/ROMFS_VIEW.md). This reads that file and serves the image's bytes; fs_romfs.cpp walks the image's own tables.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace nxrt::services::fs::nxsrc {

// A read-only byte range (any number of threads may read at once).
class Storage {
public:
    virtual ~Storage() = default;
    // n bytes at off into out; false for a range outside [0, size()) or a failed read.
    virtual bool read(uint64_t off, uint8_t* out, uint64_t n) const = 0;
    uint64_t size() const { return size_; }

protected:
    uint64_t size_ = 0;
};

// Whether `path` (UTF-8) is an in-place RomFS source: a `*.nxsrc` file, or a folder holding only romfs.nxsrc. *file: the source file.
bool is_source(const std::string& path, std::string* file = nullptr);

// The decrypted RomFS image a romfs.nxsrc describes; null with *err set when it cannot be read.
std::shared_ptr<const Storage> open(const std::string& nxsrc_file, std::string* err);

// One storage node given as JSON text (tests); null with *err set.
std::shared_ptr<const Storage> open_node(const std::string& json_node, std::string* err);

}  // namespace nxrt::services::fs::nxsrc
