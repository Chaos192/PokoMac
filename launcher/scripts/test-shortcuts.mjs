// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Tests for Settings > In-game shortcuts (src/settings-schema.js SHORTCUTS_SECTION) and Settings > Cheats: the shortcuts and default pins
// land in the game's pkp.ini, the pins' text form matches the runtime's (play/pin_board.h), clashes are found, and the cheats look reads
// and writes [cheats].
// Run: node launcher/scripts/test-shortcuts.mjs
import { FIELDS, PIN_ITEMS, SECTIONS, applyEdits, makeSource, optionsFor, pinGroupOf, pinsText, readAll, readField, rowsOf, shortcutConflicts, splitEdits, writeField } from '../src/settings-schema.js';

let bad = 0;
const check = (ok, what) => { if (!ok) { console.log(`FAIL ${what}`); bad++; } };

const env = { gameDir: 'C:/x', defaultGameDir: 'C:/x', version: '1.0.0', profiles: { active: 'default', profiles: [] } };
const empty = makeSource({ pkp: {}, controls: {}, controls_exists: true }, {});
const ids = (src = empty) => SECTIONS.find((s) => s.id === 'shortcuts').groups.flatMap((g) => rowsOf(g, readAll(src), env)).map((r) => r.id);

// the rows
check(ids().includes('shortcut_menu_key') && ids().includes('shortcut_hud_key') && ids().includes('pin_fps'), 'the shortcuts and pins rows');
check(!ids().includes('pins_layout'), 'no layout row until a readout is pinned');

// defaults: the runtime's
check(readField(FIELDS.shortcut_menu_key, empty) === 'F1' && readField(FIELDS.shortcut_hud_key, empty) === 'F7', 'defaults F1 and F7');
check(readField(FIELDS.shortcut_menu_pad, empty) === '' && readField(FIELDS.shortcut_hud_pad, empty) === '', 'no controller chords by default');

// the game's pkp.ini
let edits = writeField(FIELDS.shortcut_menu_key, 'Ctrl+F1', empty);
let split = splitEdits(edits);
check(split.config === null && split.ini.length === 1 && split.ini[0].file === 'pkp' && split.ini[0].section === 'menu' && split.ini[0].key === 'key' && split.ini[0].value === 'Ctrl+F1',
  'the menu key goes to pkp.ini [menu] key');
let src = applyEdits(empty, edits);
check(readField(FIELDS.shortcut_menu_key, src) === 'Ctrl+F1', 'and reads back (a combo, case kept)');
edits = writeField(FIELDS.shortcut_hud_pad, 'left_paddle1+right_paddle1', src);
check(edits[0].file === 'pkp' && edits[0].section === 'hud' && edits[0].key === 'pad', 'the pins\' controller chord goes to pkp.ini [hud] pad');

// the default pins: the runtime's text form
check(pinsText('', { items: ['frametime', 'fps'], layout: 'compact', key: 'F8' }) === 'perf:fps+frametime;name=Performance;layout=compact;key=F8', 'pins text in catalogue order');
const g = pinGroupOf('g2:gpu|perf:fps+low1;name=Performance;layout=horizontal;key=F10');
check(g.items.join() === 'fps,low1' && g.layout === 'horizontal' && g.key === 'F10', 'pins text read back');
check(pinsText('g2:gpu|perf:fps;layout=vertical;key=F8', { items: [], layout: 'vertical', key: 'F8' }) === 'g2:gpu', 'no items: the group goes, other groups stay');
edits = writeField(FIELDS.pin_fps, true, empty);
src = applyEdits(empty, edits);
edits = writeField(FIELDS.pin_frametime, true, src);
src = applyEdits(src, edits);
check(src.pkp['hud.default'] === 'perf:fps+frametime;name=Performance;layout=vertical;key=F8', 'pinning two readouts writes [hud] default');
check(readAll(src).pin_fps === true && readAll(src).pin_gpu === false, 'the toggles read the text');
check(ids(src).includes('pins_layout') && ids(src).includes('pins_key'), 'layout and key rows once something is pinned');
check(PIN_ITEMS.length === 10, 'the ten performance readouts (no frame generation in PokoMac)');

// clashes
let c = makeSource({ pkp: { 'menu.key': 'F3' }, controls: {}, controls_exists: true }, {});
check(shortcutConflicts(c).some((t) => t.includes('cheats menu')), 'the menu on the cheats key is a clash');
c = makeSource({ pkp: { 'menu.pad': 'l+r+minus', 'cheats.pad': 'l+r+minus' }, controls: { 'quit.pad': 'back+start' }, controls_exists: true }, {});
check(shortcutConflicts(c).some((t) => t.includes('same controller chord')), 'the same chord twice is a clash');
c = makeSource({ pkp: { 'hud.key': 'Space' }, controls: { 'bindings.play.Space': 'jump' }, controls_exists: true }, {});
check(shortcutConflicts(c).some((t) => t.includes('Space')), 'a binding of the game on a toggle key is a clash');
check(shortcutConflicts(empty).length === 0, 'the defaults do not clash');

// the cheats page: the look reads and writes [cheats] in pkp.ini
const look = makeSource({ pkp: { 'cheats.theme': 'dusk', 'cheats.accent': '#112233', 'cheats.key': 'F4' }, controls: {}, controls_exists: true }, {});
check(readField(FIELDS.cheats_theme, look) === 'dusk' && readField(FIELDS.cheats_accent, look) === '#112233' && readField(FIELDS.cheats_key, look) === 'F4', 'the cheats look reads [cheats]');
check(readField(FIELDS.cheats_enable, empty) === false && readField(FIELDS.cheats_theme, empty) === 'sunrise', 'cheats off and the sunrise theme by default');
edits = writeField(FIELDS.cheats_accent, '#aabbcc', look);
check(edits.length === 1 && edits[0].file === 'pkp' && edits[0].section === 'cheats' && edits[0].key === 'accent' && edits[0].value === '#aabbcc', 'your own accent goes to [cheats] accent');
const opts = optionsFor(FIELDS.cheats_theme, 'dusk', { cheatsThemes: [{ name: 'Mine', path: 'C:/t/mine.json' }] });
check(opts.some((o) => o.value === 'C:/t/mine.json' && o.label === 'Mine (your theme)') && opts.some((o) => o.value === 'frost'), 'the theme picker lists the built-in themes and yours');

console.log(bad ? `${bad} failed` : 'all ok');
process.exit(bad ? 1 : 0);
