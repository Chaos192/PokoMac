# Relicensing

PokoMac is distributed under GPL-3.0 today ([LICENSE](LICENSE)). Its owner, Chaos192, plans to move it to the MIT licence with the
Commons Clause and a credit condition, as for AnySwitch. This page records the plan and the permission it rests on. Until the
switch, the current licence applies to every file. The switch is scheduled after the game work: PokoMac keeps its GPL licence until
every game runs as it should, and the rewrites below and the switch come after that.

## The planned licence

- First, the Commons Clause License Condition v1.0, with Software: PokoMac, License: MIT, Licensor: Chaos192.
- Then the MIT licence, Copyright (c) 2026 Chaos192.
- A credit condition: "PokoMac by Chaos192 (github.com/Chaos192/PokoMac)" and "Built on AnySwitch by Chaos192
  (github.com/Chaos192/AnySwitch)" stay visible in the source and on the About or credits screen of any work that uses PokoMac.
- The SPDX line of each file becomes `MIT AND LicenseRef-Commons-Clause-1.0`.

The aim: anyone may use, change and share PokoMac for free, companies included, as long as they keep the credit. Nobody may sell
it. The exact terms are the texts above.

## Recompia's code

PokoMac is based on Recompia ([github.com/Dittobyte/Recompia](https://github.com/Dittobyte/Recompia)). On 2026-11-12 Chaos192
recorded that he had obtained permission to relicense their contributions from Recompia's creator, featureJosh, and from all of
Recompia's contributors. Their credit stays in [CREDITS.md](CREDITS.md).

## Sign-off by contributors

A contributor who wants their agreement on record adds one line here, in a commit of their own, with their GitHub handle, the date
and their agreement to the licence planned above, in their own words. Nobody adds a line for someone else.

(no lines yet)

## Code from other projects

The permission above covers Recompia's own contributions only. At switch time, before the new licence applies, code adapted from
other copyleft projects (smo-recomp, ssbu-recomp, ScarletNX, yuzu) is rewritten by someone who does not open that project's source.
The same holds for about 50 lines that follow tinfoil-chewer's Linux fork (pokopia-recomp-linux, GPL-3.0). The rewrite works from
public format documents, the games' own SDK behaviour and our tests. If a title needs the zbic package (GPL-2.0), the app fetches it
on the player's machine; it is never bundled. A file keeps its "adapted from" notice until it has actually
been rewritten. Code under permissive licences (MIT, BSD, Apache-2.0, ISC, zlib, CC0 and the like) stays, with its notices
([CREDITS.md](CREDITS.md), `runtime/THIRD_PARTY_NOTICES.md`).
