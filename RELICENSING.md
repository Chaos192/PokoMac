# Relicensing

On 2026-11-13 PokoMac moved from GPL-3.0 to the PolyForm Noncommercial License 1.0.0 ([LICENSE](LICENSE)), with the Required
Notices "PokoMac by Chaos192 (github.com/Chaos192/PokoMac)" and "Built on AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch)", as AnySwitch did (its
RELICENSING.md). The SPDX line of its files is `PolyForm-Noncommercial-1.0.0`. Anyone may use, change and share PokoMac for any
noncommercial purpose, keeping the Required Notices, which stay visible in the source and on the About or credits screen. The exact
terms are the text in LICENSE.

## What was done first

1. Code adapted from copyleft projects (ScarletNX and yuzu / Eden in the services, smo-recomp's CPU headers), and the lines that
   followed tinfoil-chewer's Linux fork, were rewritten by implementers who did not open that project's source (ported from
   AnySwitch's clean-room rewrites). A line-by-line similarity check of every rewritten file against PokoMac's own code before it
   finds no identical non-trivial line.
2. The build runs the lifter only as a separate process (recomp/lifter_cli.py).

## What keeps GPL-3.0-or-later

- The lifter, `recomp/nxrecomp/` (its instruction translator and readers adapted from smo-recomp and ssbu-recomp), with what runs
  inside its process: `recomp/lifter_cli.py`, `recomp/games/pkp/config.py`, its tests (`recomp/tests/`) and
  `recomp/cmake/nx_cc_launcher.py`.
- `tools/shaders/smo/` and `tools/shaders/scripts/corpus_to_mxs.py`: smo-recomp's shader translator, a second opinion for the shader
  tests, never shipped.

The text is in [LICENSE.GPL-3.0.txt](LICENSE.GPL-3.0.txt). The build only starts the lifter as a separate process; the C++ it writes
is made on the player's machine and is never part of a release.

## Still to come

`recomp/include/nxrecomp/`, `recomp/games/pkp/` and the game SDK files (`game-build/src/`) are PokoMac's own code but keep their
GPL-3.0-or-later headers until their next change: pkp_build.py's build stamp hashes them, and a header change now would need a new
Build of Pokopia. The lifter's clean-room rewrite (AnySwitch's, then ported) removes the GPL parts above.
