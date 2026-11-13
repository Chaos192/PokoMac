# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""Recompile on demand: lift the code holding one guest address into a unit, a small shared library the runtime loads while the game
runs (recomp/include/nxrecomp/ondemand.h; docs/ARCHITECTURE.md, "Code the lift did not see"). Never an interpreter, never a JIT: the
code is lifted by this lifter with the build's own lift map and compiled by the build's own toolchain (pkp_build.py --ondemand).

The lift map (`lift --map FILE`, written at the end of a module's lift): everything the build's lifter workers were given (the input
module, its base and index, the game config, patches and sites, the function list after discovery with every end, jump tables, indirect
tail calls, landing pads, catch functions, the code addresses taken, the entry level). program.make_env() takes it as is, so a unit is
lifted exactly as the build would have lifted that function.

What a unit holds, for an address A:
  * A inside a function F of the build: F again with A an entry (and a function m_<A> = u_F(c, A) that the runtime registers at A, so
    the next call there is a plain hash hit). A small F (up to UNIT_ALL_MAX instructions) gets every instruction as an entry, so any
    other address of it resolves to the same unit; a bigger one only A and its landing pads (entries.py: many gotos into a big body
    make clang's compile time explode), so another address of it is another unit.
  * A in no function: a new function [A, next start), entered at its start (small: every instruction an entry), registered at A.
NxOdUnit.start / end is the range the unit answers for (the runtime's cache index uses it): the function for a unit with every
instruction an entry, else [A, A + 4).
Calls out of the unit go through call_indirect (the unit sees no symbol of the build). A unit for code made at run time (the jit
service; lift_code) lifts a byte range given by the runtime instead of a module.

    python -m nxrecomp lift-at --map main.map.json.gz --addr 0x7101234568 --out DIR [--input main.nso]
"""
import bisect
import gzip
import json
import os

from . import entries as entriesmod
from . import program

MAP_FORMAT = 1
UNIT_ALL_MAX = 512     # instructions: a unit of a function up to this size makes every instruction an entry
# NXRECOMP_UNIT_ALL_MAX lowers it for the runtime's missed-code test (a fixture function then counts as a big one)
UNIT_ALL_MAX = int(os.environ.get('NXRECOMP_UNIT_ALL_MAX') or UNIT_ALL_MAX)


# ---------------------------------------------------------------------------------------------------- the lift map
def save_map(spec, all_funcs, path):
    """Writes the lift map of a module lifted with `spec` (lift_module's final spec: inputs plus discovery). gzip when path ends .gz."""
    ends = spec['ends']
    m = {
        'format': MAP_FORMAT,
        'input': {k: spec.get(k) for k in ('kind', 'path', 'base', 'name', 'game', 'patches', 'sites', 'index', 'spin_yield', 'mid_entries')},
        'ends': [[a, ends.get(a, a + 4)] for a in all_funcs],
        'jt': [[int(k), v] for k, v in sorted(spec['jt'].items(), key=lambda kv: int(kv[0]))],
        'tailbr': sorted(spec.get('tailbr', ())),
        'pads': list(spec.get('pads', ())),
        'catchers': sorted(spec.get('catchers', ())),
        'code_refs': list(spec.get('code_refs', ())),
        'forced': list(spec.get('forced', ())),
    }
    for k in ('path', 'patches', 'sites'):
        if m['input'][k]:
            m['input'][k] = os.path.abspath(m['input'][k])
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    tmp = path + '.tmp'
    data = json.dumps(m, separators=(',', ':')).encode('utf-8')
    with (gzip.open(tmp, 'wb', compresslevel=6) if path.endswith('.gz') else open(tmp, 'wb')) as f:
        f.write(data)
    os.replace(tmp, path)


def load_map(path, input_path=None):
    """The spec program.make_env() takes, from a lift map. input_path replaces the module file the build read (the runtime passes the
    one it loaded, so a moved game folder still works)."""
    with (gzip.open(path, 'rb') if path.endswith('.gz') else open(path, 'rb')) as f:
        m = json.loads(f.read().decode('utf-8'))
    if m.get('format') != MAP_FORMAT:
        raise ValueError('%s: lift map format %r, this lifter reads %d' % (path, m.get('format'), MAP_FORMAT))
    spec = dict(m['input'])
    if input_path:
        spec['path'] = input_path
    spec['ends'] = {a: e for a, e in m['ends']}
    spec['jt'] = {str(b): t for b, t in m['jt']}
    for k in ('tailbr', 'pads', 'catchers', 'code_refs', 'forced'):
        spec[k] = m.get(k, [])
    return spec


# ---------------------------------------------------------------------------------------------------- lifting a unit
SHIM = [
    ('void', 'call_indirect', 'Ctx *c, uint64_t a', 'c, a'),
    ('void', 'host_import', 'Ctx *c, uint32_t id', 'c, id'),
    ('void', 'guest_trap', 'const char *w, uint64_t a', 'w, a'),
    ('void', 'guest_unimpl', 'uint64_t pc, uint32_t insn, const char *w', 'pc, insn, w'),
    ('void', 'guest_svc', 'Ctx *c, uint32_t n', 'c, n'),
    ('uint64_t', 'guest_mrs', 'Ctx *c, uint32_t r', 'c, r'),
    ('void', 'guest_msr', 'Ctx *c, uint32_t r, uint64_t v', 'c, r, v'),
    ('void', 'nx_site_inline', 'Ctx *c, uint64_t a', 'c, a'),
    ('int', 'nx_site_midhook', 'Ctx *c, uint64_t a', 'c, a'),
    ('int', 'nx_hook_entry', 'Ctx *c, uint64_t a', 'c, a'),
    ('void', 'stx_diag', 'Ctx *c, uint64_t a, uint64_t e, uint64_t s, uint64_t v, int b', 'c, a, e, s, v, b'),
    ('void', 'nx_catch_call', 'Ctx *c, void (*b)(Ctx *, uint64_t)', 'c, b'),
    ('void', 'nx_no_entry', 'Ctx *c, uint64_t a', 'c, a'),
]

HEADER = '''/* generated by nxrecomp (recompiled on demand: nxrecomp/ondemand.py) -- do not edit */
#include <nxrecomp/cxx.h>
#include <nxrecomp/ondemand.h>
#if defined(_WIN32)
#define NXOD_EXPORT __declspec(dllexport)
#else
#define NXOD_EXPORT __attribute__((visibility("default")))
#endif
#define NX_OPTNONE __attribute__((optnone, noinline))
'''


def containing(env, addr):
    """(start, end) of the build's function holding addr (the innermost when ranges nest: a midhook or missed.txt split), or None."""
    k = bisect.bisect_right(env.starts, addr) - 1
    for i in range(k, max(-1, k - 16), -1):
        s = env.starts[i]
        if addr < env.ends[s]:
            return s, env.ends[s]
    return None


def unit_source(env, addr, start, end, kind, module_name, flags, extra_entries=()):
    """C++ of one unit: the lifted function [start, end), its registrations and the runtime shim. extra_entries: more mid-function entries
    (lift_code: the loop heads, where a live code patch takes over a running loop: ctx.h NX_OVL)."""
    env.ondemand = True
    env.unit = {start}
    whole = (end - start) // 4 <= UNIT_ALL_MAX
    env.mid_level = entriesmod.LEVELS.index('all' if whole else 'pads')
    env.forced_entries = ([addr] if addr != start else []) + sorted(a for a in extra_entries if start < a < end)
    env.entry_rows = []
    env.ends[start] = end
    if start not in env.starts:
        bisect.insort(env.starts, start)
    text, n, bad, _ = program.lift_function(env, start)
    two = bool(env.entry_rows) or start in env.catchers
    out = [HEADER, 'extern "C" {', 'void f_%x(Ctx *__restrict c);' % start]
    if two:
        out.append('void u_%x(Ctx *__restrict c, uint64_t rp);' % start)
    out += ['}', '', text, '']
    fns = []
    if kind == 'mid' and two and not flags:
        # the next call of addr is a hash hit; a catch function is entered through the runtime's catch frame instead (entries + flags)
        out.append('static void m_%x(Ctx *__restrict c) { u_%x(c, 0x%xULL); }' % (addr, start, addr))
        fns.append((addr, 'm_%x' % addr))
    elif kind != 'mid':
        fns.append((start, 'f_%x' % start))
    out.append('static const FnEntry od_fns[] = {')
    out += ['  { 0x%xULL, %s },' % f for f in fns] or ['  { 0, 0 },']
    out.append('};')
    out.append('static const NxEntryFn od_entries[] = {')
    out += ['  { 0x%xULL, 0x%xULL, u_%x, %du },' % (s, e, s, f) for s, e, f in env.entry_rows] or ['  { 0, 0, 0, 0 },']
    out.append('};')
    lo, hi = (start, end) if whole else (addr, addr + 4)             # what the unit answers for
    out.append('static const NxOdUnit od_unit = { NXOD_VERSION, NXRECOMP_ABI_VERSION, "%s", 0x%xULL, 0x%xULL, 0x%xULL, 0x%xULL, od_fns, %d, '
               'od_entries, %d };' % (module_name, env.base, addr, lo, hi, len(fns), len(env.entry_rows)))
    out.append('static const NxOdApi *od_api;')
    out.append('extern "C" {')
    for ret, name, params, args in SHIM:
        out.append('%s %s(%s) { %sod_api->%s(%s); }' % (ret, name, params, '' if ret == 'void' else 'return ', name, args))
    out.append('NXOD_EXPORT const NxOdUnit *nxod_unit(const NxOdApi *api) {')
    out.append('  if (!api || api->size < sizeof(NxOdApi) || api->abi != NXRECOMP_ABI_VERSION) return 0;')
    out.append('  od_api = api;')
    out.append('  nx_spin_hook = api->spin_hook;')
    out.append('  return &od_unit;')
    out.append('}')
    out.append('}')
    out.append('')
    return '\n'.join(out), n, bad, fns, whole


def lift_at(map_path, addr, outdir, input_path=None, log=print):
    """Lifts the unit for guest address `addr` of the module whose lift map is map_path into outdir/od_<addr>.cpp.
    -> dict(source, kind, start, end, instructions, not_translated, fns, flags)."""
    spec = load_map(map_path, input_path)
    env = program.make_env(spec)
    if not (env.text_lo <= addr < env.text_hi) or addr % 4:
        raise ValueError('0x%x is not an instruction address of %s (code 0x%x..0x%x)' % (addr, env.img.name, env.text_lo, env.text_hi))
    hit = containing(env, addr)
    if hit:
        start, end = hit
        kind = 'start' if start == addr else 'mid'
    else:
        i = bisect.bisect_right(env.starts, addr)
        start, end, kind = addr, (env.starts[i] if i < len(env.starts) else env.text_hi), 'new'
    flags = 1 if start in env.catchers else 0             # NX_ENTRY_CATCHER
    src, n, bad, fns, whole = unit_source(env, addr, start, end, kind, env.img.name, flags)
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, 'od_%x.cpp' % addr)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(src)
    log('%s: unit for 0x%x: %s function 0x%x..0x%x, %d instructions, %d not translated, entries: %s'
        % (env.img.name, addr, kind, start, end, n, len(bad), 'every instruction' if whole else 'this address'))
    return {'source': path, 'kind': kind, 'start': start, 'end': end, 'instructions': n, 'not_translated': len(bad),
            'fns': [a for a, _ in fns], 'flags': flags, 'module': env.img.name, 'whole': whole}


# ---------------------------------------------------------------------------------------------------- code made at run time
def code_extent(word, addr, end):
    """Where the code met at addr ends, in bytes nothing else describes: after the first unconditional branch or return that no branch
    before it jumps past (what follows is reached only from elsewhere: another block the generator wrote, or free space). At most end."""
    far, pc = addr, addr
    while pc < end:
        w = word(pc)
        t = None
        if (w & 0x7c000000) == 0x14000000 and not w >> 31:                       # b (not bl)
            t = pc + _sext(w & 0x3ffffff, 26) * 4
        elif (w & 0xff000010) == 0x54000000:                                    # b.cond
            t = pc + _sext((w >> 5) & 0x7ffff, 19) * 4
        elif (w & 0x7e000000) == 0x34000000:                                    # cbz / cbnz
            t = pc + _sext((w >> 5) & 0x7ffff, 19) * 4
        elif (w & 0x7e000000) == 0x36000000:                                    # tbz / tbnz
            t = pc + _sext((w >> 5) & 0x3fff, 14) * 4
        if t is not None and addr <= t < end:
            far = max(far, t)
        unconditional = (w & 0xfc000000) == 0x14000000 or (w & 0xfe1f0000) == 0xd61f0000 and (w >> 21) & 0xf in (0, 2)   # b, br, ret
        if unconditional and pc + 4 > far:
            return pc + 4
        pc += 4
    return end


def _sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def lift_code(code, base, addr, outdir, name='jit', log=print, sites=()):
    """A unit for guest code that exists only at run time (the jit service's code memory): `code` is the bytes the runtime read at
    guest address `base`, `addr` the address met. The function runs from addr to the end code_extent() finds (a small one gets every
    instruction as an entry); nothing about it is known in advance (no unwind data, symbols or jump tables). `sites`: addresses that get
    an inline site (`nx_site_inline(c, addr)` before the instruction; the cheats overlay's breakpoints on a function it lifted again)."""
    from .image import Image
    img = Image(name, base, bytes(code), (0, len(code) - len(code) % 4))
    env = program.Env(img)
    env.full = False
    env.sites['inline'] = set(int(a) for a in sites)
    env.ends = {addr: code_extent(img.word, addr, base + len(code) - len(code) % 4)}
    env.starts = [addr]
    # the loop heads (targets of backward branches) are entries too: a module lifted with --live-patch sends a running loop of the patched
    # function there (ctx.h NX_OVL); a function of up to UNIT_ALL_MAX instructions has every instruction as an entry anyway
    heads = set()
    for pc in range(addr, env.ends[addr], 4):
        w = img.word(pc)
        t = None
        if (w >> 26) == 0x05:
            t = pc + _sext(w & 0x3ffffff, 26) * 4
        elif (w & 0xFF000010) == 0x54000000 or (w & 0x7E000000) == 0x34000000:
            t = pc + _sext((w >> 5) & 0x7ffff, 19) * 4
        elif (w & 0x7E000000) == 0x36000000:
            t = pc + _sext((w >> 5) & 0x3fff, 14) * 4
        if t is not None and addr < t <= pc:
            heads.add(t)
    src, n, bad, fns, whole = unit_source(env, addr, addr, env.ends[addr], 'new', name, 0, heads)
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, 'od_%x.cpp' % addr)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(src)
    log('%s: unit for 0x%x: code made at run time 0x%x..0x%x, %d instructions, %d not translated' % (name, addr, base, base + len(code), n, len(bad)))
    return {'source': path, 'kind': 'code', 'start': addr, 'end': env.ends[addr], 'instructions': n, 'not_translated': len(bad),
            'fns': [a for a, _ in fns], 'flags': 0, 'module': name}


# ---------------------------------------------------------------------------------------------------- a module loaded at run time
MODULE_SHIM = HEADER + '''extern "C" {
extern const NxModule %(nx)s;
extern const uint32_t %(nx)s_abi;
}
static const NxOdApi *od_api;
static NxOdModuleUnit od_module;
extern "C" {
%(forwards)s
NXOD_EXPORT const NxOdModuleUnit *nxod_module(const NxOdApi *api) {
  if (!api || api->size < sizeof(NxOdApi) || api->abi != NXRECOMP_ABI_VERSION) return 0;
  od_api = api;
  nx_spin_hook = api->spin_hook;
  od_module.version = NXOD_VERSION;
  od_module.abi = %(nx)s_abi;
  od_module.module = &%(nx)s;
  return &od_module;
}
}
'''


def lift_module_unit(path, base, index, name, outdir, jobs=4, log=print):
    """A whole module the game loads while it runs (an NRO: the jit service's plugin) lifted as the build lifts its own (discovery,
    unwind and jump tables, the default entry level) at guest address `base` with NxModule.index `index`, plus od_shim.cpp: the runtime
    calls through the table the runtime hands over, and the export nxod_module (ondemand.h). -> dict(sources, functions, ...)."""
    spec = {'kind': 'nro', 'path': path, 'base': base, 'name': name, 'game': None, 'patches': None, 'sites': None, 'index': index,
            'spin_yield': None}
    os.makedirs(outdir, exist_ok=True)
    r = program.lift_module(spec, outdir, jobs=jobs, shards=0, log=log)
    forwards = '\n'.join('%s %s(%s) { %sod_api->%s(%s); }' % (ret, fn, params, '' if ret == 'void' else 'return ', fn, args)
                         for ret, fn, params, args in SHIM)
    shim = os.path.join(outdir, 'od_shim.cpp')
    with open(shim, 'w', encoding='utf-8', newline='\n') as f:
        f.write(MODULE_SHIM % {'nx': 'nxmod_' + program.mangle(name), 'forwards': forwards})
    sources = sorted(os.path.join(outdir, n) for n in os.listdir(outdir) if n.endswith('.cpp'))
    return dict(r, sources=sources, module=name)
