// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Export a cheat-table entry or a pointer path as an Atmosphère cheat, so it runs on this engine, a Switch, Ryujinx or a yuzu fork. The
// opcodes used are the ones the VM (cheat_vm.*) implements and the tests cover: 0 (static write), 5 (load through a pointer), 4 (load an
// immediate) and A (store a register through reg + offset). The generated text parses back through ParseFile.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "scanner.h"       // Value, ScanType

namespace nxrt::cheats {

// A static write: write `v` to base(mem_type) + offset. mem_type is the Atmosphère nybble (0 main, 1 heap, 2 alias, ...). The width is the
// value's type. Returns the opcode words as a text block (no header).
std::string export_static_body(unsigned mem_type, uint64_t offset, const Value& v);

// A pointer path: base(mem_type) + base_offset is a pointer; each of `offsets` is added then dereferenced, except the last, which is the
// final field offset where `v` is written. (offsets empty = write at *[base+base_offset] + 0.) Follows pointers as 8-byte reads.
std::string export_pointer_body(unsigned mem_type, uint64_t base_offset, const std::vector<uint64_t>& offsets, const Value& v);

// Wrap a body as a named toggleable cheat ([Name]) or the master ({Name}).
std::string export_cheat(const std::string& name, const std::string& body, bool master = false);

}  // namespace nxrt::cheats
