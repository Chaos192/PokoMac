#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Portions Copyright (c) 2026 Chaos192
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""make_overlay_fonts.py: the overlay's compiled-in faces (runtime/assets/fonts, cmake/gfx.cmake) from the upstream font files.

    python make_overlay_fonts.py <Inter-Regular.ttf> <Inter-SemiBold.ttf> <CascadiaMono-Regular.ttf> <DejaVuSans.ttf> <out dir>

Inputs: Inter-4.1.zip (github.com/rsms/inter releases, extras/ttf), CascadiaCode-2407.24.zip (github.com/microsoft/cascadia-code
releases, ttf/static), runtime/assets/fonts/DejaVuSans.ttf. Needs fontTools.
Inter: Latin (+ Extended-A/B, Additional), Greek (+ Extended), Cyrillic, punctuation, currency, letterlike, arrows, maths and the symbol
blocks it draws; the tabular figures (GSUB 'tnum') become the default digits, as the launcher's numbers; layout tables and hinting dropped
(ImGui reads neither). AnySwitch Mono: Cascadia Mono (Reserved Font Name "Cascadia Code", so the modified font is renamed) to ASCII,
Latin-1, Latin Extended-A, punctuation, arrows, box drawing and blocks. AnySwitch Symbols: DejaVu Sans to the symbol blocks Inter has not
got (and Armenian, Georgian), renamed as the Bitstream Vera licence asks of a modified font. runtime/THIRD_PARTY_NOTICES.md has the details.
"""
import sys
from fontTools.ttLib import TTFont
from fontTools import subset

def R(a, b): return set(range(a, b + 1))

INTER = set().union(R(0x20, 0x7E), R(0xA0, 0x24F), R(0x250, 0x2FF), R(0x300, 0x36F), R(0x370, 0x3FF), R(0x400, 0x52F), R(0x1E00, 0x1FFF),
                    R(0x2000, 0x206F), R(0x2070, 0x209F), R(0x20A0, 0x20CF), R(0x2100, 0x214F), R(0x2150, 0x218F), R(0x2190, 0x21FF),
                    R(0x2200, 0x22FF), R(0x2300, 0x23FF), R(0x2400, 0x24FF), R(0x2500, 0x25FF), R(0x2600, 0x27BF), R(0x2B00, 0x2BFF),
                    {0xFFFD})
MONO = set().union(R(0x20, 0x7E), R(0xA0, 0x17F), R(0x2000, 0x206F), R(0x20A0, 0x20CF), R(0x2190, 0x21FF), R(0x2500, 0x25FF), {0xFFFD})
SYM_BLOCKS = set().union(R(0x530, 0x58F), R(0x10A0, 0x10FF), R(0x2190, 0x21FF), R(0x2200, 0x22FF), R(0x2300, 0x23FF), R(0x2400, 0x24FF),
                         R(0x2500, 0x259F), R(0x25A0, 0x25FF), R(0x2600, 0x26FF), R(0x2700, 0x27BF), R(0x27C0, 0x27FF), R(0x2900, 0x29FF),
                         R(0x2B00, 0x2BFF), R(0x1F600, 0x1F64F))

def freeze_tnum(font):
    gsub = font['GSUB'].table
    lookups = set()
    for fr in gsub.FeatureList.FeatureRecord:
        if fr.FeatureTag == 'tnum':
            lookups.update(fr.Feature.LookupListIndex)
    sub = {}
    for li in sorted(lookups):
        for st in gsub.LookupList.Lookup[li].SubTable:
            if st.LookupType == 7: st = st.ExtSubTable
            if hasattr(st, 'mapping'): sub.update(st.mapping)
    n = 0
    for t in font['cmap'].tables:
        if not t.isUnicode(): continue
        for cp in list(t.cmap):
            if 0x30 <= cp <= 0x39 and t.cmap[cp] in sub:
                t.cmap[cp] = sub[t.cmap[cp]]; n += 1
    return n

def rename(font, family, full, ps, note):
    nm = font['name']
    for r in list(nm.names):
        if r.nameID in (1, 3, 4, 6, 16, 17, 18, 21, 22, 25): nm.removeNames(nameID=r.nameID)
    for pid, eid, lid in ((3, 1, 0x409), (1, 0, 0)):
        nm.setName(family, 1, pid, eid, lid); nm.setName('Regular', 2, pid, eid, lid)
        nm.setName(ps + ';' + note, 3, pid, eid, lid); nm.setName(full, 4, pid, eid, lid); nm.setName(ps, 6, pid, eid, lid)

def make(src, dst, unicodes, tnum=False, ren=None):
    font = TTFont(src)
    if tnum: print(dst, 'tnum digits frozen:', freeze_tnum(font))
    have = set(font.getBestCmap())
    if isinstance(unicodes, tuple):            # (wanted, excluded): the wanted ones the font has and the excluded font has not
        want, exclude = unicodes
        unicodes = (want & have) - exclude
    o = subset.Options()
    o.layout_features = []
    o.hinting = False
    o.desubroutinize = True
    o.notdef_outline = True
    o.name_IDs = ['*']
    o.name_languages = ['*']
    o.drop_tables += ['GSUB', 'GPOS', 'GDEF', 'kern', 'DSIG', 'STAT', 'meta', 'FFTM']
    s = subset.Subsetter(o)
    s.populate(unicodes=sorted(unicodes & have))
    s.subset(font)
    if ren: rename(font, *ren)
    font.save(dst)
    f2 = TTFont(dst)
    print(dst, 'glyphs', f2['maxp'].numGlyphs, 'cmap', len(f2.getBestCmap()))
    return set(f2.getBestCmap())

if __name__ == '__main__':
    inter_r, inter_sb, mono, dejavu, out = sys.argv[1:6]
    a = make(inter_r, out + '/Inter-Regular.ttf', INTER, tnum=True)
    make(inter_sb, out + '/Inter-SemiBold.ttf', INTER, tnum=True)
    make(mono, out + '/AnySwitchMono-Regular.ttf', MONO,
         ren=('AnySwitch Mono', 'AnySwitch Mono Regular', 'AnySwitchMono-Regular', 'subset of Cascadia Mono 2407.24 (SIL OFL 1.1, Microsoft)'))
    make(dejavu, out + '/AnySwitchSymbols-Regular.ttf', (SYM_BLOCKS, a),
         ren=('AnySwitch Symbols', 'AnySwitch Symbols Regular', 'AnySwitchSymbols-Regular', 'subset of DejaVu Sans 2.37 (Bitstream Vera licence, DejaVu changes public domain)'))
