# Relicensing

PokoMac is distributed under GPL-3.0 today ([LICENSE](LICENSE)). It stays under that licence until every game runs as it should.
After the game work, its owner, Chaos192, moves it to the licence below, as for AnySwitch. Until the switch, the current licence
applies to every file.

## The planned licence

- First, the Commons Clause License Condition v1.0, with Software: PokoMac, License: MIT, Licensor: Chaos192.
- Then the MIT licence, Copyright (c) 2026 Chaos192.
- A required credit: "PokoMac by Chaos192 (github.com/Chaos192/PokoMac)" and "Built on AnySwitch by Chaos192
  (github.com/Chaos192/AnySwitch)" stay visible in the source and on the About or credits screen of any work that uses PokoMac.
- The SPDX line of each file becomes `MIT AND LicenseRef-Commons-Clause-1.0`.

The aim: anyone may use, change and share PokoMac for free, companies included, as long as they keep the credit. Nobody may sell
it. The exact terms are the texts above.

## Steps, in order (after the game work)

1. Code adapted from copyleft projects (smo-recomp, ssbu-recomp, ScarletNX, yuzu), and about 50 lines that follow tinfoil-chewer's
   Linux fork (pokopia-recomp-linux, GPL-3.0), are rewritten by someone who does not open that project's source. The rewrite works
   from public format documents, the games' own SDK behaviour and our tests. A file keeps its "adapted from" notice until it has
   actually been rewritten.
2. If a title needs the zbic package (GPL-2.0), the app fetches it on the player's machine; it is never bundled.
3. Tools and tests taken from GPL projects leave the tree.
4. The switch: LICENSE, the SPDX lines, README, CREDITS, LEGAL and the credit on the About screen. Code under permissive licences
   (MIT, BSD, Apache-2.0, ISC, zlib, CC0 and the like) stays, with its notices ([CREDITS.md](CREDITS.md),
   `runtime/THIRD_PARTY_NOTICES.md`).
