# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""pl:u / pl:s (nn::pl::sharedresource::ISharedFontManager): every command with argument and buffer variants, and an id sweep.
Commands from Switchbrew "Shared Database services" (pl:u): 0 RequestLoad, 1 GetLoadState, 2 GetSize, 3 GetSharedMemoryAddressOffset,
4 GetSharedMemoryNativeHandle, 5 GetSharedFontInOrderOfPriority, 6 GetSharedFontInOrderOfPriorityForSystem, 100 RequestApplicationFunctionAuthorization,
101 RequestApplicationFunctionAuthorizationByProcessId, 102 RequestApplicationFunctionAuthorizationByApplicationId, 103 RefreshApplicationFunctionBlackListDebugRecord,
104 RequestApplicationFunctionAuthorizationByProgramId, 105 GetFunctionBlackListSystemVersionToAuthorize, 106 GetFunctionBlackListVersion,
1000 LoadNgWordDataForPlatformRegionChina, 1001 GetNgWordDataSizeForPlatformRegionChina; Switch 2: 2000 / 2010 built-in fonts."""
import struct

CMDS = [0, 1, 2, 3, 4, 5, 6, 100, 101, 102, 103, 104, 105, 106, 1000, 1001, 2000, 2010]


def lang(code):
    return code.encode().ljust(8, b"\0")


def generate(s, variants):
    for name in ("pl:u", "pl:s"):
        s.reset()
        s.comment(name + ": the font sequence a game's nn::pl::InitializeSharedFont sends, per type")
        sid = s.open(name)
        s.q(sid, name, 4)                                         # GetSharedMemoryNativeHandle
        for t in list(range(0, 8)) + [0xFF, 0xFFFFFFFF]:
            raw = struct.pack("<I", t)
            s.q(sid, name, 0, raw=raw)                            # RequestLoad
            s.q(sid, name, 1, raw=raw)                            # GetLoadState
            s.q(sid, name, 2, raw=raw)                            # GetSize
            s.q(sid, name, 3, raw=raw)                            # GetSharedMemoryAddressOffset
        for code in ("en-US", "ja", "zh-CN", "zh-Hans", "zh-TW", "zh-Hant", "ko", "fr", "", "xx"):
            for size in (0x18, 0x0, 0x4, 0x8, 0x100):
                for kind in ("b", "c"):
                    s.q(sid, name, 5, raw=lang(code), bufs=((kind, size, None),) * 3)
                    s.q(sid, name, 6, raw=lang(code), bufs=((kind, size, None),) * 3)
        s.q(sid, name, 2000)
        for t in list(range(0, 10)) + [0xFF]:
            for flag in (0, 1, 2):
                s.q(sid, name, 2010, raw=struct.pack("<BB", t, flag))
        s.comment(name + ": fuzz")
        for cmd in CMDS:
            s.fuzz(sid, name, cmd, n=variants)
        s.sweep(sid, name, list(range(0, 16)) + list(range(96, 112)) + [999, 1002, 1999, 2001, 2009, 2011, 3000])
        s.q(sid, name, 4)


KEY = 0x06186249
MAGIC = 0x18029A7F
DUMPS = ["FontStandard.bfttf", "FontChineseSimplified.bfttf", "FontChineseSimplifiedExtension.bfttf", "FontChineseTraditional.bfttf",
         "FontKorean.bfttf", "FontNintendoExtended.bfttf"]
XBOX = "xbox/b,xbox/a,xbox/y,xbox/x,xbox/lb,xbox/rb,xbox/lt,xbox/rt,-,-,xbox/menu,xbox/view,-,-,-,-,-,-"
PS = "ps/circle,ps/cross,ps/triangle,ps/square,ps/l1,ps/r1,ps/l2,ps/r2,-,-,ps/options,ps/share,-,-,-,-,-,-"


def bfttf(data, extra=b""):
    """A system font file: magic and a size word, then every u32 of `data` XOR the key (`extra`: trailing bytes that are not a word)."""
    import array
    words = array.array("I", data[: len(data) // 4 * 4])   # little-endian hosts (x64, arm64)
    words = array.array("I", [w ^ KEY for w in words])
    return struct.pack("<II", MAGIC ^ KEY, 0x12345678) + words.tobytes() + extra


def probe(s, sid, name):
    s.q(sid, name, 4)
    for t in range(0, 7):
        s.q(sid, name, 2, raw=struct.pack("<I", t))
        s.q(sid, name, 3, raw=struct.pack("<I", t))
    s.q(sid, name, 5, raw=lang("en-US"), bufs=(("b", 0x18, None),) * 3)
    s.q(sid, name, 2000)
    for t in range(0, 7):
        s.q(sid, name, 2010, raw=struct.pack("<BB", t, 1))


def generate_dirs(s):
    """[pl] font_dir: dumps of some fonts, broken files, a DejaVuSans.ttf of its own, a font too big for the memory."""
    from pathlib import Path
    aux = Path(s.aux_dir)
    sets = {
        "korean": {"FontKorean.bfttf": bfttf(bytes(range(256)) * 16)},
        "all": {n: bfttf(bytes([i]) * (0x1000 + 4 * i)) for i, n in enumerate(DUMPS)},
        "broken": {"FontStandard.bfttf": b"not a font file at all", "FontKorean.bfttf": bfttf(b"")[:8],
                   "FontChineseTraditional.bfttf": bfttf(b"abcdefgh", b"xyz"), "FontNintendoExtended.bfttf": b"\x00" * 64},
        "dejavu": {"DejaVuSans.ttf": b"\x00\x01\x00\x00" + bytes(range(200)) * 50, "FontChineseSimplified.bfttf": bfttf(b"\x11" * 400)},
        "huge": {"FontStandard.bfttf": bfttf(b"\x22" * 0x1000000), "FontChineseSimplified.bfttf": bfttf(b"\x33" * 0x0F8000)},
        "empty": {"FontStandard.bfttf": b""},
    }
    for label, files in sets.items():
        d = aux / ("fonts_" + label)
        d.mkdir(parents=True, exist_ok=True)
        for n, data in files.items():
            (d / n).write_bytes(data)
        s.comment("[pl] font_dir " + label)
        s.config("pl", "font_dir", str(d))
        s.reset()
        sid = s.open("pl:u")
        probe(s, sid, "pl:u")
        s.lines.append(f"api face_set ps {PS}")       # font 5: generated unless it came from a dump
        probe(s, sid, "pl:u")
    s.config("pl", "font_dir", str(aux / "no_such_folder"))
    s.reset()
    sid = s.open("pl:u")
    probe(s, sid, "pl:u")
    s.config("pl", "font_dir", "")


def generate_faces(s):
    """The button face: the provider asked once at the first build, set_face before and after a build, the same face again, pl_reset."""
    s.lines.append("api face_get")
    s.lines.append("api face_provider")
    s.lines.append("api face_get")
    s.reset()
    sid = s.open("pl:u")
    s.q(sid, "pl:u", 1)                                  # GetLoadState does not build
    s.lines.append("api face_get")
    s.q(sid, "pl:u", 4)                                  # builds: the provider's face
    s.lines.append("api face_get")
    probe(s, sid, "pl:u")
    s.lines.append(f"api face_set ps {PS}")
    s.lines.append(f"api face_set ps {PS}")              # the same again: nothing
    s.lines.append(f"api face_set ps2 {PS}")             # only the style differs: equal, nothing
    s.lines.append(f"api face_set xbox {XBOX}")
    s.lines.append("api face_set switch -,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-")
    s.lines.append("api face_get")
    s.reset()                                            # the face survives pl_reset
    s.lines.append("api face_get")
    s.lines.append(f"api face_set ps {PS}")              # before the rebuild: used by it
    sid = s.open("pl:u")
    probe(s, sid, "pl:u")
    s.lines.append("api face_provider")                  # a provider set after a face: never asked
    s.reset()
    sid = s.open("pl:u")
    probe(s, sid, "pl:u")
    s.lines.append("api face_get")


_generate_main = generate


def generate(s, variants, variant="main"):
    if variant == "main":
        _generate_main(s, variants)
    elif variant == "dirs":
        generate_dirs(s)
    elif variant == "faces":
        generate_faces(s)
