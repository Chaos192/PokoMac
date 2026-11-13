// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Internal: ELF64 structures and constants (AArch64, little endian) used by the module loader.
#pragma once
#include <cstdint>

namespace nxrt::elf {

constexpr uint16_t ET_EXEC = 2, ET_DYN = 3;
constexpr uint16_t EM_AARCH64 = 183;
constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_GNU_RELRO = 0x6474e552;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;
constexpr uint32_t SHT_SYMTAB = 2, SHT_DYNSYM = 11;
constexpr int64_t DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9,
                  DT_STRSZ = 10, DT_SYMENT = 11, DT_INIT = 12, DT_FINI = 13, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26,
                  DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28, DT_RELRSZ = 35, DT_RELR = 36, DT_RELRENT = 37,
                  DT_GNU_HASH = 0x6ffffef5;
constexpr uint32_t R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027,
                   R_AARCH64_TLS_DTPMOD64 = 1028, R_AARCH64_TLS_DTPREL64 = 1029, R_AARCH64_TLS_TPREL64 = 1030,
                   R_AARCH64_TLSDESC = 1031, R_AARCH64_IRELATIVE = 1032;
constexpr uint16_t SHN_ABS = 0xfff1;

#pragma pack(push, 1)
struct Ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Shdr {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};
struct Dyn {
    int64_t tag;
    uint64_t val;
};
struct Sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};
struct Rela {
    uint64_t offset, info;
    int64_t addend;
};
#pragma pack(pop)
static_assert(sizeof(Ehdr) == 64 && sizeof(Phdr) == 56 && sizeof(Shdr) == 64 && sizeof(Dyn) == 16 && sizeof(Sym) == 24 && sizeof(Rela) == 24);

}  // namespace nxrt::elf
