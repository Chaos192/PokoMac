# SPDX-License-Identifier: GPL-3.0-or-later
"""A loaded guest module: the memory image the lifter reads, independent of where it came from.

An `Image` holds the module's bytes as they sit in memory (offset 0 = the module's lowest address), the .text range,
the dynamic-linking tables (symbols, relocations, PLT) when there are any, and the unwind-table location. Three loaders:

  Image.from_nso(bytes, base)       a Switch NSO (LZ4 segments, MOD0 header)          -- the game
  Image.from_nx_elf(path, base)     the ELF tools/extract's nso_to_elf writes         -- the game, via an ELF
  Image.from_static_elf(path)       an ordinary static (non-PIE) ELF64 AArch64        -- the end-to-end test programs

Everything else in the package (function discovery, the lifter, the oracles) goes through this class, so the lifter
needs no game-specific file format. Adapted from smo-recomp tools/extract/functions.py (Module) and
tools/lifter/recomp.py (_elf_layout), GPL-3.0-or-later.
"""
import struct

NX_ELF_OFF = 0x1000          # file offset of the image in the ELF files nso_to_elf writes


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Image:
    def __init__(self, name, base, mem, text, bss=(0, 0)):
        self.name = name
        self.base = base                 # guest address of mem[0]
        self.mem = mem                   # bytes; offset = guest address - base
        self.text = text                 # (offset, size) of .text
        self.bss = bss                   # (offset, size) beyond mem (zero-filled)
        self.dt = {}                     # DT_* tag -> value (offsets / sizes), from .dynamic when there is one
        self.sym_off = self.str_off = 0  # symbol / string table offsets
        self.nsym = 0
        self.symtab_is_dynamic = True
        self.mod0 = 0                    # NSO: offset of the MOD0 record
        self.eh_hdr = 0                  # offset of .eh_frame_hdr (0 = none)
        self.eh_frame = (0, 0)           # (offset, size) of .eh_frame when known (static ELF)
        self.sections = {}               # static ELF: name -> (offset, size)
        self.segments = []               # (offset, size, flags) of the loadable segments
        self.meta = {}
        self.abs_is_addr = False         # absolute pointers in unwind data are guest addresses (static ELF), not offsets
        self._rel = None

    # ---- raw access ---------------------------------------------------------------------------------------
    @property
    def text_size(self):
        return self.text[1]

    @property
    def text_start(self):
        return self.text[0]

    @property
    def end(self):
        """First guest address beyond the image (data + bss)."""
        return self.base + max(len(self.mem), self.bss[0] + self.bss[1])

    def u32(self, off):
        return struct.unpack_from('<I', self.mem, off)[0]

    def word(self, addr):
        return struct.unpack_from('<I', self.mem, addr - self.base)[0]

    def cstr(self, off):
        e = self.mem.index(b'\0', off)
        return bytes(self.mem[off:e]).decode('latin-1')

    def words(self):
        """Every instruction word of .text."""
        return struct.unpack_from('<%dI' % (self.text[1] // 4), self.mem, self.text[0])

    def ptr_at(self, off):
        """The pointer stored at module offset `off`, as an absolute guest address: a RELATIVE relocation there wins
        (the NSO's raw bytes are unrelocated); otherwise the raw 8 bytes (a static ELF is already linked)."""
        if self._rel is None:
            self._rel = {o: a for o, info, a in self.relocs() if (info & 0xffffffff) == 0x403}
        a = self._rel.get(off)
        if a is not None:
            return self.base + a
        return struct.unpack_from('<Q', self.mem, off)[0]

    def reloc_map(self):
        """{offset: (type, symbol index, addend)} of DT_RELA and DT_JMPREL."""
        if getattr(self, '_relmap', None) is None:
            m = {}
            for tbl in (self.relocs(), self.jmprel()):
                for o, info, a in tbl:
                    m[o] = (info & 0xffffffff, info >> 32, a)
            self._relmap = m
        return self._relmap

    def in_text(self, addr):
        o = addr - self.base
        return self.text[0] <= o < self.text[0] + self.text[1]

    # ---- symbols and relocations --------------------------------------------------------------------------
    def symbols(self):
        """[(name, type, bind, shndx, value, size)]; value is an OFFSET (module-relative) for NSO, an ADDRESS for
        static ELF (the callers use sym_value())."""
        out = []
        for i in range(self.nsym):
            name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', self.mem, self.sym_off + i * 24)
            out.append((self.cstr(self.str_off + name_off), info & 0xf, info >> 4, shndx, value, size))
        return out

    def relocs(self):
        """[(offset, info, addend)] of DT_RELA (R_AARCH64_RELATIVE etc.), then the DT_RELR entries as RELATIVE
        relocations whose addend is the value stored in place; offset is module-relative."""
        out = []
        if 7 in self.dt:
            rela, sz = self.dt[7], self.dt[8]
            out = [struct.unpack_from('<QQq', self.mem, rela + i * 24) for i in range(sz // 24)]
        out += [(o, 0x403, struct.unpack_from('<q', self.mem, o)[0]) for o in self.relr_offsets()]
        return out

    def relr_offsets(self):
        """Module offsets relocated by DT_RELR (36, size DT_RELRSZ 35; SDK 20 modules use it for most RELATIVE
        relocations). An even entry is an address; an odd one is a bitmap of the 63 words after the last address."""
        if 36 not in self.dt:
            return []
        off, sz = self.dt[36], self.dt.get(35, 0)
        out, where = [], 0
        for i in range(sz // 8):
            e = struct.unpack_from('<Q', self.mem, off + i * 8)[0]
            if not e & 1:
                out.append(e)
                where = e + 8
            else:
                for bit in range(1, 64):
                    if e >> bit & 1:
                        out.append(where + (bit - 1) * 8)
                where += 63 * 8
        return out

    def jmprel(self):
        """[(slot offset, info, addend)] of DT_JMPREL (the PLT's GOT slots)."""
        if 23 not in self.dt:
            return []
        off, sz = self.dt[23], self.dt.get(2, 0)
        return [struct.unpack_from('<QQq', self.mem, off + i * 24) for i in range(sz // 24)]

    # ---- unwind tables ------------------------------------------------------------------------------------
    def eh_frame_hdr(self):
        """(eh_frame offset, [(function start offset, FDE offset)]) from .eh_frame_hdr's binary-search table."""
        if not self.eh_hdr:
            return 0, []
        h = self.eh_hdr
        ver, ptr_enc, cnt_enc, tbl_enc = self.mem[h:h + 4]
        if ver != 1 or cnt_enc != 0x03 or tbl_enc != 0x3b:
            raise ValueError('unexpected .eh_frame_hdr encodings %x %x %x %x' % (ver, ptr_enc, cnt_enc, tbl_enc))
        # eh_frame_ptr: pcrel sdata4 (0x1b) in practice
        if ptr_enc == 0x1b:
            frame = h + 4 + struct.unpack_from('<i', self.mem, h + 4)[0]
        elif ptr_enc == 0x03:
            frame = self.u32(h + 4)
        else:
            frame = 0
        count = self.u32(h + 8)
        out = []
        for i in range(count):
            loc, fde_rel = struct.unpack_from('<ii', self.mem, h + 12 + i * 8)
            out.append((h + loc, h + fde_rel))
        return frame, out

    # ---- loaders ------------------------------------------------------------------------------------------
    @classmethod
    def from_nso(cls, data, base, name='main'):
        from . import nso
        info = nso.nso_info(data)
        (tv, text), (rv, ro), (dv, dat) = [(s['vaddr'], s['data']) for s in info['segments']]
        mem = bytearray(dv + len(dat))
        for v, r in ((tv, text), (rv, ro), (dv, dat)):
            mem[v:v + len(r)] = r
        img = cls(name, base, bytes(mem), (tv, len(text)), bss=(dv + len(dat), info['bss']))
        img.segments = [(tv, len(text), 5), (rv, len(ro), 4), (dv, len(dat), 6)]
        img.mod0 = struct.unpack_from('<I', mem, 4)[0]
        if bytes(mem[img.mod0:img.mod0 + 4]) != b'MOD0':
            raise ValueError('no MOD0 header')
        dyn_off, bss_s, bss_e, eh_off = struct.unpack_from('<iiii', mem, img.mod0 + 4)
        img.bss = (bss_s, bss_e - bss_s)
        dyn = img.mod0 + dyn_off
        n = 0
        while True:
            t, v = struct.unpack_from('<qQ', mem, dyn + n * 16)
            n += 1
            if t == 0:
                break
            img.dt.setdefault(t, v)
        img.sym_off, img.str_off = img.dt[6], img.dt[5]
        img.nsym = struct.unpack_from('<I', mem, img.dt[4] + 4)[0] if 4 in img.dt else (img.str_off - img.sym_off) // 24
        img.eh_hdr = img.mod0 + eh_off
        img.meta = {'build_id': info['build_id'], 'segments_sha_ok': all(s['sha_ok'] for s in info['segments']),
                    'dynamic': dyn, 'dynstr_size': img.dt.get(10, 0)}
        return img

    @classmethod
    def from_nro(cls, data, base, name='nro'):
        """An NRO: a module a game loads while it runs (nn::ro, the jit service's plugin; runtime src/services/jit.cpp). It is not
        compressed and its file is its memory image: .text, .rodata and .data at their offsets, word 1 the MOD0 offset, the header at
        0x10 ('NRO0', file size, the three segments as (offset, size), the .bss size at 0x38, the build id at 0x40)."""
        if bytes(data[0x10:0x14]) != b'NRO0':
            raise ValueError('not an NRO (no NRO0 at 0x10)')
        (tv, ts), (rv, rs), (dv, ds) = [struct.unpack_from('<II', data, 0x20 + 8 * i) for i in range(3)]
        mem = bytes(data[:dv + ds]).ljust(dv + ds, b'\0')
        img = cls(name, base, mem, (tv, ts), bss=(dv + ds, struct.unpack_from('<I', data, 0x38)[0]))
        img.segments = [(tv, ts, 5), (rv, rs, 4), (dv, ds, 6)]
        img.mod0 = struct.unpack_from('<I', mem, 4)[0]
        if bytes(mem[img.mod0:img.mod0 + 4]) != b'MOD0':
            raise ValueError('no MOD0 header')
        dyn_off, bss_s, bss_e, eh_off = struct.unpack_from('<iiii', mem, img.mod0 + 4)
        img.bss = (img.mod0 + bss_s, bss_e - bss_s)
        dyn = img.mod0 + dyn_off
        n = 0
        while True:
            t, v = struct.unpack_from('<qQ', mem, dyn + n * 16)
            n += 1
            if t == 0:
                break
            img.dt.setdefault(t, v)
        img.sym_off, img.str_off = img.dt[6], img.dt[5]
        img.nsym = struct.unpack_from('<I', mem, img.dt[4] + 4)[0] if 4 in img.dt else (img.str_off - img.sym_off) // 24
        img.eh_hdr = img.mod0 + eh_off
        img.meta = {'build_id': bytes(data[0x40:0x60]).hex(), 'dynamic': dyn, 'dynstr_size': img.dt.get(10, 0)}
        return img

    @classmethod
    def from_nx_elf(cls, path, base, name=None):
        d = open(path, 'rb').read()
        off = NX_ELF_OFF
        shoff, = struct.unpack_from('<Q', d, 0x28)
        shnum, shstrndx = struct.unpack_from('<HH', d, 0x3c)
        secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * 64) for i in range(shnum)]
        st = secs[shstrndx]
        names = d[st[4]:st[4] + st[5]]
        sec = {names[x[0]:names.index(b'\0', x[0])].decode(): (x[3], x[5]) for x in secs}
        last = max(a + s for n, (a, s) in sec.items() if n not in ('.bss', '.shstrtab', ''))
        mem = bytes(d[off:off + last])
        img = cls(name or path, base, mem, sec['.text'], bss=sec.get('.bss', (0, 0)))
        dyn, dsz = sec['.dynamic']
        for i in range(dsz // 16):
            t, v = struct.unpack_from('<qQ', mem, dyn + i * 16)
            if t == 0:
                break
            img.dt.setdefault(t, v)
        img.sym_off, img.str_off = img.dt[6], img.dt[5]
        img.nsym = sec['.dynsym'][1] // 24
        img.mod0 = struct.unpack_from('<I', mem, 4)[0]
        if bytes(mem[img.mod0:img.mod0 + 4]) == b'MOD0':
            eh_off = struct.unpack_from('<i', mem, img.mod0 + 0x10)[0]
            img.eh_hdr = img.mod0 + eh_off
        img.sections = dict(sec)
        return img

    @classmethod
    def from_static_elf(cls, path, name=None):
        """A static AArch64 ELF64 linked at its final address (p_vaddr is used as is)."""
        d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[4] != 2 or struct.unpack_from('<H', d, 0x12)[0] != 183:
            raise ValueError('not an AArch64 ELF64')
        entry, phoff, shoff = struct.unpack_from('<QQQ', d, 0x18)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x36)
        loads = []
        for i in range(phnum):
            p_type, p_flags, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
            if p_type == 1:
                loads.append((p_vaddr, p_off, p_filesz, p_memsz, p_flags))
        lo = min(l[0] for l in loads)
        lo -= lo % 0x1000
        top_file = max(l[0] + l[2] for l in loads)
        top_mem = max(l[0] + l[3] for l in loads)
        mem = bytearray(top_file - lo)
        for vaddr, off, fsz, msz, fl in loads:
            mem[vaddr - lo:vaddr - lo + fsz] = d[off:off + fsz]
        secs = []
        for i in range(shnum):
            secs.append(struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize))
        st = secs[shstrndx]
        names = d[st[4]:st[4] + st[5]]
        sec = {}
        for s in secs:
            n = names[s[0]:names.index(b'\0', s[0])].decode()
            if n:
                sec[n] = s
        t = sec['.text']
        img = cls(name or path, lo, bytes(mem), (t[3] - lo, t[5]), bss=(len(mem), top_mem - lo - len(mem)) if top_mem - lo > len(mem) else (0, 0))
        img.segments = [(v - lo, m, f) for v, o, fs, m, f in loads]
        img.sections = {n: (s[3] - lo, s[5]) for n, s in sec.items()}
        img.meta = {'entry': entry}
        img.abs_is_addr = True
        if '.symtab' in sec:
            sy, sstr = sec['.symtab'], secs[sec['.symtab'][6]]
            # the symbol table is not inside a loaded segment: keep it as a private blob appended after the image
            img.symbols = lambda sy=sy, sstr=sstr, d=d: _static_symbols(d, sy, sstr)       # noqa: E731
            img.symtab_is_dynamic = False
        if '.eh_frame_hdr' in sec:
            img.eh_hdr = sec['.eh_frame_hdr'][3] - lo
        if '.eh_frame' in sec:
            img.eh_frame = (sec['.eh_frame'][3] - lo, sec['.eh_frame'][5])
        return img

    def sym_value(self, value):
        """Module-relative offset of a symbol value (NSO symbols are already offsets; static ELF ones are addresses)."""
        return value if self.symtab_is_dynamic else value - self.base


def _static_symbols(d, sy, sstr):
    out = []
    names = d[sstr[4]:sstr[4] + sstr[5]]
    for i in range(sy[5] // 24):
        name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, sy[4] + i * 24)
        out.append((names[name_off:names.index(b'\0', name_off)].decode('latin-1'), info & 0xf, info >> 4, shndx, value, size))
    return out
