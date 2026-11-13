# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""Mid-function entries: where the runtime may enter a lifted function other than at its start (ctx.h NxEntryFn, ABI 4).

In the lifted program a guest function is one C++ function and a `ret` is a C++ return, so a jump into the middle of a function (an
indirect branch to a label, a landing pad, an address computed from a function's address) has no host function to call. Such a
function is written as u_<entry>(c, rp), whose top switches on rp to the label, and the module lists it in NxModule.entries, so the
runtime enters it at that address, still in compiled code, instead of trapping (docs/ARCHITECTURE.md, "Code the lift did not see").
The table has one row per function (start, end, u_<entry>); the switch is the exact list of its entries, so an entry costs a case label
and nothing in the tables.

Levels (`lift --mid-entries`), each including the one before:
  pads       C++ landing pads (what ABI 3 had) and the addresses of the title's missed.txt (found at run time, `lift --entries`), at
             most MAX_FORCED of those per function                                                                      [default]
  indirect   + code addresses the module takes that fall inside a function (R_AARCH64_RELATIVE addends, ADR and ADRP + ADD targets),
               resolved jump-table targets, and every instruction of a function with an unresolved `br`
  returns    + the return site of every call (the instruction after a BL or BLR)
  leaders    + every basic-block leader: branch targets inside the function and the instruction after every branch
  all        + every instruction (units recompiled on demand use it for a small function)
Every entry is a goto from the top of the function into its body, into loops too: the control flow becomes irreducible and clang's -O1
goes superlinear on a function with many of them. Wonder's main at `indirect` (the default of 8c18715) has a unit that did not compile
in 25 minutes (u_71013855b0: 241 entries, its jump-table targets); see FINDINGS.md. So the build's own tables are minimal: what the
runs met. Everything else is recompiled on demand (ondemand.py) and cached, which costs a wait the first time instead of every build.
The levels above `pads` are for measurement.
"""
import bisect

LEVELS = ('pads', 'indirect', 'returns', 'leaders', 'all')
DEFAULT = 'pads'
MAX_FORCED = 16        # missed.txt entries per function at `pads`; the rest stay with the units already made for them (cached)


def level_index(name):
    if name is None:
        return LEVELS.index(DEFAULT)
    if isinstance(name, int):
        return name
    if name not in LEVELS:
        raise ValueError('unknown mid-entry level %r (one of %s)' % (name, ', '.join(LEVELS)))
    return LEVELS.index(name)


def _is_call(w):
    return (w & 0xfc000000) == 0x94000000 or (w & 0xfffffc1f) == 0xd63f0000          # bl / blr


def _is_branch(w):
    return ((w & 0x7c000000) == 0x14000000 or (w & 0xff000010) == 0x54000000 or (w & 0x7e000000) in (0x34000000, 0x36000000)
            or (w & 0xfe000000) == 0xd6000000)


def _in_range(sorted_addrs, lo, hi):
    i = bisect.bisect_right(sorted_addrs, lo)
    out = []
    while i < len(sorted_addrs) and sorted_addrs[i] < hi:
        out.append(sorted_addrs[i])
        i += 1
    return out


def function_entries(env, fn, entry, end, pcs, pads):
    """The sorted mid-function entries of the lifted function [entry, end) at env.mid_level: pads, the forced ones (missed.txt), then
    what the level adds. pcs: the addresses of its lifted statements (each has a label once it is an entry)."""
    lv = getattr(env, 'mid_level', level_index(DEFAULT))
    out = set(pads)
    forced = _in_range(getattr(env, 'forced_entries', ()), entry, end)
    out.update(forced if lv >= 1 else forced[:MAX_FORCED])
    if lv >= 1:
        out.update(_in_range(getattr(env, 'code_refs', ()), entry, end))
        jt = env.jt
        for pc in pcs:
            tg = jt.get(pc)
            if tg:
                out.update(t for t in tg if entry < t < end)
        if fn.has_br_fallback:
            out.update(pcs)
    if lv >= 2:
        for pc in pcs:
            if _is_call(env.word(pc)):
                out.add(pc + 4)
    if lv >= 3:
        out.update(fn.targets)
        for pc in pcs:
            if _is_branch(env.word(pc)):
                out.add(pc + 4)
    if lv >= 4:
        out.update(pcs)
    lifted = set(pcs)
    return sorted(a for a in out if a != entry and a in lifted)


def switch_lines(mids):
    """The C++ at the top of u_<entry>: the entry switch over `mids` (landing pads included)."""
    out = ['  if (rp) switch (rp) {']
    out.extend('  case 0x%xULL: goto L_%x;' % (a, a) for a in mids)
    out.append('  default: nx_no_entry(c, rp); return; }')
    return out


def table_lines(rows):
    """NxEntryFn rows {start, end, u_<start>, flags} of module_<name>.cpp, sorted by start. rows: [(start, end, flags)]."""
    m = ['static const NxEntryFn entries[] = {']
    m += ['  { 0x%xULL, 0x%xULL, u_%x, %du },' % (s, e, s, f) for s, e, f in sorted(rows)]
    if not rows:
        m.append('  { 0, 0, 0, 0 },')
    m.append('};')
    return m


def load_missed(path, modules=None):
    """missed.txt: one `<module> <offset>` per line (hex offset from the module's base; `#` starts a comment; any further words are
    notes: the build id, the date). Returns {module name: sorted offsets}; `modules` keeps only those names."""
    out = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            t = line.split('#', 1)[0].split()
            if len(t) < 2:
                continue
            if modules is not None and t[0] not in modules:
                continue
            out.setdefault(t[0], set()).add(int(t[1], 16))
    return {k: sorted(v) for k, v in out.items()}
