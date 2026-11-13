// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Tests for the mods page's words (src/mods-model.js, docs/MODS.md). Run: node launcher/scripts/test-mods-model.mjs
import { conflictLine, installSummary, modLine, moved, stateNote } from '../src/mods-model.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const a = { id: 'big', name: 'Big Textures', kinds: ['romfs'], version: '1.2', author: 'Kiki', conflicts: [] };
const b = { id: 'fast', name: 'FastMode', kinds: ['code', 'cheats'], conflicts: [] };
check(modLine(a) === 'Game files · Version 1.2 · by Kiki', `modLine with version and author: ${modLine(a)}`);
check(modLine(b) === 'Code · Cheats', `modLine of a code mod: ${modLine(b)}`);
check(modLine({ kinds: ['plugins'] }) === 'Plugins', 'plugins');
check(modLine({ kinds: ['frame_rate', 'code'] }) === 'Frame rate · Code', 'a 60 fps mod (frame_rate.ini, e314e165)');

check(conflictLine(a, [a, b]) === '', 'no conflict, no line');
const c1 = { id: 'big', name: 'Big Textures', conflicts: [{ path: 'a.txt', mods: ['big', 'old'], winner: 'big' }, { path: 'b.bin', mods: ['big', 'old'], winner: 'big' }] };
const old = { id: 'old', name: 'Old Pack', conflicts: c1.conflicts };
check(conflictLine(c1, [c1, old]) === 'Changes 2 files that Old Pack also changes: this mod wins.', conflictLine(c1, [c1, old]));
check(conflictLine(old, [c1, old]) === 'Changes 2 files that Big Textures also changes: Big Textures wins.', conflictLine(old, [c1, old]));
const code = { id: 'x', name: 'X', conflicts: [{ module: 'main', offset: '0x104', mods: ['x', 'y'], winner: 'y' }] };
check(conflictLine(code, [code, { id: 'y', name: 'Y' }]) === 'Changes code in main that Y also changes: Y wins.', conflictLine(code, [code, { id: 'y', name: 'Y' }]));

check(stateNote({ mods: [] }).startsWith('No mods yet'), 'empty');
check(stateNote({ mods: [a], code_changed: true }).includes('build the game again'), 'code changed');
check(stateNote({ mods: [a], code_changed: false }).includes('next time the game starts'), 'romfs only');

const r = { installed: [{ name: 'A', warnings: ['1 of its code patches are for other versions of the game (build ee55) and are not used.'] }],
  refused: [{ name: 'B', reason: 'It is a mod for another game (title 0100000000010000); this game is 0100ABCD12340000.' }] };
check(installSummary(r).startsWith('Installed A. 1 of its code patches') && installSummary(r).includes('B: It is a mod for another game'), installSummary(r));

const list = [{ id: 'p' }, { id: 'q' }, { id: 'r' }];
check(moved(list, 'p', -1) === null && moved(list, 'p', 1) === 1 && moved(list, 'r', 1) === null && moved(list, 'q', -1) === 0, 'moves');

if (bad) {
  console.log(`${bad} FAILED`);
  process.exit(1);
}
console.log('mods model: all checks passed');
