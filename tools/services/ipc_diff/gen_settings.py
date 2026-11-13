# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""set / set:sys and the nn::settings::operator== hooks: every command with argument and buffer variants, the firmware struct for many
[system] firmware strings, languages, regions, nicknames, the settings items, and the hook over every language and code.
Ids: Switchbrew "Settings services"."""
import struct

SET = list(range(0, 13))
SYS = [3, 4, 7, 8, 23, 24, 37, 38, 68, 77, 79]
CODES = ["ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant",
         "pt-BR", "pl", "th", "zh-Hansx", "", "ZH-CN", "en"]
ITEMS = [("account", "na_required_for_network_service"), ("audio", "uac_speaker_enabled"), ("idle", "dim_level_percent_lcd"),
         ("capsrv", "screenshot_layerstack"), ("Account", "na_required_for_network_service"), ("account", "x"), ("", ""),
         ("a" * 0x47, "b" * 0x47), ("capsrv" + "\0junk", "screenshot_layerstack")]
FIRMWARE = ["23.0.0", "12.1.0", "12.1", "15", "12.1.0-rc", "1.", "300.1.2", "", "abc", "-1", " 12.1.0", "99999999999.1.0", "1.99999999999.3",
            "1.2.3.4", "1..3", "0.0.0", "255.255.255", "21.0.0" + "x" * 40, "18.1.0"]


def bufs_out():
    return [(), (("b", 0x1000, None),), (("b", 0x800, None),), (("c", 0x100, None),), (("b", 0x20, None),), (("b", 0x80, None),),
            (("b", 0x10, None),), (("b", 0x7, None),), (("b", 0x3, None),), (("b", 0x200, None),), (("b", 0x400, None),), (("c", 0x78, None),),
            (("b", 1, None),)]


def generate(s, variants):
    s.reset()
    st = s.open("set")
    ss = s.open("set:sys")
    s.comment("every command, every buffer shape")
    for cmd in SET:
        for raw in (b"", struct.pack("<I", 0), struct.pack("<I", 3), struct.pack("<I", 0xF), struct.pack("<I", 0x11), struct.pack("<I", 0x12),
                    struct.pack("<I", 0xFFFFFFFF)):
            for bv in bufs_out():
                s.q(st, "set", cmd, raw=raw, bufs=bv)
    for cmd in SYS:
        for raw in (b"", b"\x05", b"\x00", struct.pack("<i", 1), struct.pack("<i", -1)):
            for bv in bufs_out()[:6]:
                s.q(ss, "set:sys", cmd, raw=raw, bufs=bv)
    s.comment("lock screen flag and colour set, then reset")
    for raw in (b"\x05", b"\x00", b"", b"\x01\x00\x00\x00", b"\xff"):
        s.q(ss, "set:sys", 8, raw=raw)
        s.q(ss, "set:sys", 7)
    for raw in (struct.pack("<i", 1), struct.pack("<i", -1), b"", struct.pack("<i", 7)):
        s.q(ss, "set:sys", 24, raw=raw)
        s.q(ss, "set:sys", 23)
    s.reset()
    st = s.open("set")
    ss = s.open("set:sys")
    s.q(ss, "set:sys", 7)
    s.q(ss, "set:sys", 23)
    s.comment("settings items")
    for name, key in ITEMS:
        for kind in ("x", "a"):
            nb, kb = (name.encode() + b"\0"), (key.encode() + b"\0")
            for out in (0x8, 0x4, 0x40, 0x10, 0x1, 0):
                bv = ((kind, len(nb), nb), (kind, len(kb), kb)) + ((("b", out, None),) if out else ())
                s.q(ss, "set:sys", 37, bufs=bv)
                s.q(ss, "set:sys", 38, bufs=bv)
        nb = name.encode()[:0x48]
        s.q(ss, "set:sys", 37, bufs=(("x", len(nb), nb), ("x", 0x48, key.encode().ljust(0x48, b"k"))))
    s.q(ss, "set:sys", 37)
    s.q(ss, "set:sys", 38, bufs=(("b", 0x10, None),))
    s.comment("profile: firmware, language, region, nickname")
    for fw in FIRMWARE:
        s.config("system", "firmware", fw)
        for bv in ((("c", 0x100, None),), (("b", 0x20, None),), (("b", 0x200, None),)):
            s.q(ss, "set:sys", 3, bufs=bv)
            s.q(ss, "set:sys", 4, bufs=bv)
            s.q(st, "set", 10, bufs=bv)
    s.config("system", "firmware", "")
    for ounce in ("false", "true"):
        s.config("platform", "ounce", ounce)
        s.q(ss, "set:sys", 3, bufs=(("c", 0x100, None),))
    for lang in ("Japanese", "AmericanEnglish", "French", "SimplifiedChinese", "TraditionalChinese", "BrazilianPortuguese", "Chinese",
                 "Taiwanese", "Korean", "nonsense"):
        s.config("system", "language", lang)
        s.q(st, "set", 0)
        s.q(st, "set", 4)
    s.config("system", "language", "")
    for region in ("0", "1", "2", "3", "4", "5", "6", "7", "-1", "Europe", "China", "x"):
        s.config("system", "region", region)
        s.q(st, "set", 4)
    s.config("system", "region", "")
    for nick in ("nxrt", "", "n" * 0x7F, "m" * 0x80, "o" * 0x100, "a b"):
        s.config("system", "nickname", nick)
        for bv in ((("b", 0x80, None),), (("b", 0x3, None),), (("b", 0x200, None),), (("b", 1, None),), (("c", 0x40, None),), ()):
            s.q(st, "set", 11, bufs=bv)
            s.q(ss, "set:sys", 77, bufs=bv)
    s.config("system", "nickname", "")
    s.comment("fuzz")
    for cmd in SET:
        s.fuzz(st, "set", cmd, n=variants)
    for cmd in SYS:
        s.fuzz(ss, "set:sys", cmd, n=variants)
    s.sweep(st, "set", list(range(13, 40)) + [100, 1000])
    s.sweep(ss, "set:sys", [0, 1, 2, 5, 6, 9, 10, 22, 25, 36, 39, 47, 62, 67, 69, 76, 78, 80, 90, 95, 120, 200])
    s.comment("the compare hooks (both orders, every language against every code)")
    s.lines.append("api lang_hooks")
    eq1 = "_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE"
    eq2 = "_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE"
    for lang in list(range(0, 21)) + [99, 0xFFFFFFFF]:
        for code in CODES:
            cb = "bytes:" + (code.encode()[:8].ljust(8, b"\0")).hex()
            s.lines.append(f"api hle {eq1} u32:{lang:x} {cb}")
            s.lines.append(f"api hle {eq2} {cb} u32:{lang:x}")
    for a, b in (("null", "bytes:6a61000000000000"), ("u32:0", "null"), ("null", "null"), ("u32:0", "bytes:6a61000102030405"),
                 ("u32:6", "bytes:7a682d48616e7378")):
        s.lines.append(f"api hle {eq1} {a} {b}")
        s.lines.append(f"api hle {eq2} {b} {a}")
