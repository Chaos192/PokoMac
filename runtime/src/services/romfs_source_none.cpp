// SPDX-License-Identifier: GPL-3.0-or-later
// PokoMac reads Pokopia's RomFS from the dumped folders (and the mod manager's layers); it does not take AnySwitch's in-place package
// reader (romfs_source.cpp: a RomFS read from an .nsp / .xci through a romfs.nxsrc file, decrypted on read with Mbed TLS). This answers
// the part of romfs_source.h that AnySwitch's fs_romfs.cpp (by Chaos192, taken as it is) asks: no path is a romfs.nxsrc source here.
#include <memory>
#include <string>

#include "romfs_source.h"

namespace nxrt::services::fs::nxsrc {

bool is_source(const std::string&, std::string*) { return false; }

std::shared_ptr<const Storage> open(const std::string&, std::string* err) {
    if (err) *err = "this build reads the RomFS from folders only (no romfs.nxsrc)";
    return nullptr;
}

std::shared_ptr<const Storage> open_node(const std::string&, std::string* err) {
    if (err) *err = "this build reads the RomFS from folders only (no romfs.nxsrc)";
    return nullptr;
}

}  // namespace nxrt::services::fs::nxsrc
