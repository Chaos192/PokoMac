// Every row of the Settings page, and the pure mapping between the files and the controls. No DOM, no Tauri: `node --test` runs it.
// A new setting is one entry in SECTIONS; pages/settings.js draws and saves it without being touched.
// Keys and defaults: runtime/src/gfx/play/play_config.cpp, play_types.h, runtime/docs/CONFIG.md. A missing key reads as the default in
// play_types.h, which is what the game does with it.
//
// A source is { pkp, controls, config, controls_exists }: read_settings' `section.key` maps plus get_config.
// An edit is { file, section, key, value }. file 'pkp' and 'controls' go to write_settings, 'config' to save_config.
//
// Field: { id, label, type, at, def, options, min, max, step, suffix, ... }
//   type     segmented | select | choice | size | toggle | slider | path | clock | accent
//   at       where it lives: controls(), pkp() or config(); an array when it has two keys
//   read     (src) => the control's value, when the plain mapping of `type` does not fit
//   write    (value, src) => edits, likewise
//   writeSame (src) => true to call write even when the value is unchanged (another key may still need resetting)
//   when     (values, env) => shown or not
//   enabled  (src) => false disables the row; the default disables what lives in a controls.ini that does not exist yet
//   lock     (env) => true disables the row for now
//   replaces true: writes other fields too, so the page closes any Custom number field it holds open
//   quality  'value' | 'frame': a value quality presets set, so changing it by hand makes the preset Custom (see markCustom)
// A choice is a list of presets plus Custom with a number field (custom: { min, max, suffix, start, label }), a size likewise with two.
// Rows that are not fields: info (label, value, actions), keycap (label, key), note (a line of text), and { expand } for rows made from env.

// The cheats overlay's look settings: defaults, ranges and the built-in themes, shared with the runtime (docs/CHEATS.md). The cheats menu and
// its settings are ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
import CHEATS_LOOK from '../../runtime/assets/cheats/look.json' with { type: 'json' };

/** The cheats overlay's built-in themes by name (runtime/assets/cheats/themes; CHEATS_LOOK.theme.builtin lists them). */
const CHEATS_THEME_LABELS = { sunrise: 'Sunrise (the app’s)', dusk: 'Dusk', midnight: 'Midnight', daylight: 'Daylight (light)', paper: 'Paper (light)', frost: 'Frost (glass)' };
/** The cheats overlay key's SDL names by the lower-cased word a select reads (wordOf). */
const CHEATS_KEY_ALIASES = Object.fromEntries(['F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12', 'Insert', 'Home', 'End', 'PageUp', 'PageDown', 'Pause']
  .map((k) => [k.toLowerCase(), k]));

// MetalFX (Apple's temporal upscaler) exists in the macOS runtime only, where it is the default (runtime/include/nxrt/extras.h).
const IS_MAC = typeof navigator !== 'undefined' && /Macintosh/.test(navigator.userAgent || '');
const UPSCALERS = [
  { value: 'off', label: 'Off' },
  ...(IS_MAC ? [{ value: 'metalfx', label: 'MetalFX (sharper, smoother edges)' }, { value: 'metalfx_aa', label: 'MetalFX anti-aliasing only' }] : []),
  { value: 'fsr1', label: 'FSR 1' },
  { value: 'nearest', label: 'Nearest (sharp pixels)' },
];

const controls = (section, key) => ({ file: 'controls', section, key });
const pkp = (section, key) => ({ file: 'pkp', section, key });
const config = (key) => ({ file: 'config', section: '', key });

const keyOf = (at) => (at.file === 'config' ? at.key : `${at.section}.${at.key}`);
const rawOf = (src, at) => src[at.file]?.[keyOf(at)];
const has = (src, at) => {
  const raw = rawOf(src, at);
  return raw != null && String(raw).trim() !== '';
};
const edit = (at, value) => ({ ...at, value });
const atsOf = (f) => [f.at ?? []].flat();

const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));
const truthy = (v) => ['1', 'true', 'yes', 'on'].includes(String(v).trim().toLowerCase());
const fwd = (s) => String(s).trim().replace(/\\/g, '/');
/** The runtime's name_key(): case and `-` are forgiven in the words of the file. */
const word = (v) => String(v).trim().toLowerCase().replace(/-/g, '_');

// ---- shader files -------------------------------------------------------------------------------------------------------------------
// env.storage: shader_storage (shader_archive.rs Storage, plus complete and running). env.repack: the running job, or null.

const GB = 2 ** 30;
const gb = (b) => `${(b / GB).toFixed(b >= 10 * GB ? 0 : 1)} GB`;
const count = (n) => Number(n).toLocaleString('en');

export function storageText(env) {
  const st = env.storage;
  const r = env.repack;
  if (r) {
    const what = r.kind === 'expand' ? 'Unpacking' : 'Packing';
    return r.total ? `${what}: ${Math.floor((100 * r.done) / r.total)}%` : `${what}...`;
  }
  if (!st) return 'Checking';
  switch (st.mode) {
    case 'none': return 'Not set up yet';
    case 'loose': return `Loose files, ${gb(st.loose_bytes)}`;
    case 'compact': return `Packed, ${gb(st.archive_bytes + st.loose_bytes)}`;
    default: return `Partly packed, ${gb(st.archive_bytes + st.loose_bytes)}`;
  }
}

export function storageActions(env) {
  if (env.repack) return [{ act: 'stop-repack', label: 'Stop', icon: 'square' }];
  const st = env.storage;
  const off = !st?.complete || env.gameBusy || env.taskBusy;
  if (st?.mode === 'loose' || st?.mode === 'mixed') return [{ act: 'compact-shaders', label: 'Free up space', icon: 'hard-drive', primary: true, disabled: off }];
  if (st?.mode === 'compact') return [{ act: 'expand-shaders', label: 'Use loose files', icon: 'rotate-ccw', disabled: off }];
  return [];
}

export function storageNote(env) {
  const st = env.storage;
  if (!st || env.repack) return null;
  if (st.mode === 'loose' || st.mode === 'mixed') {
    return { text: `Packing frees about ${gb(st.free_estimate)}. Nothing is translated again and the game runs the same. A later update stops reading loose shader files.` };
  }
  if (st.mode === 'compact' && st.archive_raw_bytes > 0) {
    const ratio = st.archive_raw_bytes / Math.max(1, st.archive_bytes);
    return { text: `${count(st.archive_shaders)} shaders in ${gb(st.archive_bytes)}, ${ratio.toFixed(1)}x smaller than loose files (${gb(st.archive_raw_bytes)}).` };
  }
  return null;
}

// ---- quality presets --------------------------------------------------------------------------------------------------------------

// Ids, labels and which ones set the frame values: kQualityPresets in runtime/src/gfx/play/play_types.cpp (labels without its bracket).
// The values: kPresets in runtime/games/pkp/pkp_quality.cpp, which the F1 page applies. The tests parse both files.
// render_scale 0 and fps_limit -1 are written as auto.
export const QUALITY_PRESETS = [
  { id: 'switch2', label: 'Switch 2', lod_bias: 0, anisotropy: 0 },
  { id: 'high', label: 'High', lod_bias: -0.5, anisotropy: 8 },
  { id: 'ultra', label: 'Ultra', lod_bias: -1, anisotropy: 16 },
  { id: 'steamdeck', label: 'Steam Deck', lod_bias: 0, anisotropy: 4, render_scale: 75, fps_limit: 40 },
  { id: 'legion_go_s', label: 'Legion Go S', lod_bias: 0, anisotropy: 8, render_scale: 80, fps_limit: 60 },
  { id: 'laptop', label: 'Laptop', lod_bias: 0, anisotropy: 4, render_scale: 75, fps_limit: 60 },
  { id: 'desktop', label: 'Desktop PC', lod_bias: -0.5, anisotropy: 16, render_scale: 0, fps_limit: -1 },
];

const AT_PRESET = controls('display', 'quality_preset');
const AT_RENDER_SCALE = controls('display', 'render_scale');
const AT_FPS_LIMIT = controls('display', 'fps_limit');

function presetEdits(id) {
  const p = QUALITY_PRESETS.find((q) => q.id === id);
  if (!p) return [];   // Custom is what a preset becomes, never something picked: the F1 list does not offer it either
  const edits = [
    edit(AT_PRESET, id),
    edit(controls('display', 'lod_bias'), String(p.lod_bias)),
    edit(controls('display', 'anisotropy'), String(p.anisotropy)),
  ];
  if (p.render_scale !== undefined) edits.push(edit(AT_RENDER_SCALE, p.render_scale > 0 ? String(p.render_scale) : 'auto'));
  if (p.fps_limit !== undefined) edits.push(edit(AT_FPS_LIMIT, p.fps_limit >= 0 ? String(p.fps_limit) : 'auto'));
  return edits;
}

/** quality_preset_edited() of play_types.cpp: the render scale and the frame limit only belong to the device presets. */
function markCustom(kind, src) {
  const cur = word(has(src, AT_PRESET) ? rawOf(src, AT_PRESET) : 'switch2');
  if (cur === 'custom') return [];
  const setsFrame = QUALITY_PRESETS.find((q) => q.id === cur)?.render_scale !== undefined;
  return kind === 'frame' && !setsFrame ? [] : [edit(AT_PRESET, 'custom')];
}

// ---- volume and mouse ---------------------------------------------------------------------------------------------------------------

const AT_VOLUME = controls('audio', 'volume');
const AT_VOLUME_PKP = pkp('audio', 'volume');

/** [audio] volume in pkp.ini beats controls.ini and undoes the F1 slider, so what it says is what plays. Shown as 0 to 100. */
function readVolume(src) {
  const at = [AT_VOLUME_PKP, AT_VOLUME].find((a) => has(src, a));
  const v = at ? parseFloat(rawOf(src, at)) : 1;
  return Math.round(clamp(Number.isFinite(v) ? v : 1, 0, 1) * 100);
}

function writeVolume(value, src) {
  const text = String(value / 100);
  return has(src, AT_VOLUME_PKP) ? [edit(AT_VOLUME, text), edit(AT_VOLUME_PKP, text)] : [edit(AT_VOLUME, text)];
}

export const SOURCE_RAD = (0.022 * Math.PI) / 180;   // kSourceRadPerCount: radians per mouse count at sensitivity 1
const AT_YAW = controls('mouse', 'yaw');
const AT_PITCH = controls('mouse', 'pitch');
const SENS_MIN = 0.1;
const SENS_MAX = 20;
const SENS_DEFAULT = 2;   // 0.000767945 rad in play_types.h

function readSensitivity(src) {
  const rad = parseFloat(rawOf(src, AT_YAW));
  return Math.round(clamp(Number.isFinite(rad) ? rad / SOURCE_RAD : SENS_DEFAULT, SENS_MIN, SENS_MAX) * 10) / 10;
}

function writeSensitivity(value) {
  const rad = (clamp(value, SENS_MIN, SENS_MAX) * SOURCE_RAD).toPrecision(9);
  return [edit(AT_YAW, rad), edit(AT_PITCH, rad)];
}

// ---- the plain mapping of each type -------------------------------------------------------------------------------------------------

const CLOCK_LIMIT_MIN = 10 * 366 * 24 * 60;
const clockMinutes = (value) => {
  const n = Math.round(Number(String(value).trim()));
  return Number.isFinite(n) ? clamp(n, -CLOCK_LIMIT_MIN, CLOCK_LIMIT_MIN) : 0;
};

const AT_CLOCK_OFFSET = pkp('time', 'clock_offset');
const AT_CLOCK_SCOPE = pkp('time', 'clock_scope');
const AT_EVENT_PRESET = pkp('time', 'event_preset');
const TIMED_EVENTS = [
  { id: 'copycat', label: 'Copycat Challenge (April 2, 2026)' },
  { id: 'hoppip', label: 'More Spores for Hoppip (March 12, 2026)' },
  { id: 'bulbasaur', label: "Bulbasaur's Jump Rope Contest (April 21, 2026)" },
  { id: 'sableye', label: "Sableye's Gem Hunt (May 1, 2026)" },
];
const clockScope = (src) => (has(src, AT_CLOCK_SCOPE) && word(rawOf(src, AT_CLOCK_SCOPE)) === 'all' ? 'all' : 'user');
const scopeNeedsReset = (src) => clockScope(src) === 'all';
const activeEvent = (src) => {
  const id = has(src, AT_EVENT_PRESET) ? word(rawOf(src, AT_EVENT_PRESET)) : 'off';
  return TIMED_EVENTS.some((e) => e.id === id) ? id : 'off';
};

function clockEdits(value, src) {
  const edits = [edit(AT_CLOCK_OFFSET, String(clockMinutes(value)))];
  // Do not add the default key to an older file; only clear a scope left by the earlier preset implementation.
  if (scopeNeedsReset(src)) edits.push(edit(AT_CLOCK_SCOPE, 'user'));
  return edits;
}

const wordOf = (f, src) => {
  const v = word(has(src, f.at) ? rawOf(src, f.at) : f.def);
  return f.aliases?.[v] ?? v;
};

const READ = {
  segmented: wordOf,
  select: wordOf,
  choice: wordOf,
  size: (f, src) => atsOf(f).map((at, i) => (has(src, at) ? String(rawOf(src, at)).trim() : f.def.split('x')[i])).join('x'),
  toggle: (f, src) => truthy(has(src, f.at) ? rawOf(src, f.at) : f.def),
  slider: (f, src) => {
    const v = parseFloat(has(src, f.at) ? rawOf(src, f.at) : f.def);
    const near = Math.round(clamp(Number.isFinite(v) ? v : f.def, f.min, f.max) / f.step) * f.step;
    return +near.toFixed(2);
  },
  path: (f, src) => String(rawOf(src, f.at) ?? ''),
  clock: (f, src) => clockMinutes(has(src, f.at) ? rawOf(src, f.at) : f.def),
};

const WRITE = {
  segmented: (f, v) => [edit(f.at, String(v))],
  select: (f, v) => [edit(f.at, String(v))],
  choice: (f, v) => [edit(f.at, String(v))],
  size: (f, v) => String(v).split('x').map((part, i) => edit(atsOf(f)[i], part)),
  toggle: (f, v) => [edit(f.at, f.at.file === 'config' ? !!v : v ? '1' : '0')],
  slider: (f, v) => [edit(f.at, String(v))],
  path: (f, v) => [edit(f.at, fwd(v))],
  clock: (f, v) => [edit(f.at, String(clockMinutes(v)))],
  accent: (f, v) => [edit(f.at, String(v))],
};

// ---- the page ---------------------------------------------------------------------------------------------------------------------

const REPO_URL = 'https://github.com/Chaos192/PokoMac';
// the credits (Settings > About): the project PokoMac is built on (and takes its menus, cheats, mods and networking from), and the
// project this launcher and port are based on
const ANYSWITCH_URL = 'https://github.com/Chaos192/AnySwitch';
const UPSTREAM_URL = 'https://github.com/Dittobyte/Recompia';
const open = (url, label = 'Open') => ({ act: 'open-url', arg: url, label, icon: 'external-link' });
const hasControls = (src) => src.controls_exists !== false;

export const DLSS_DOWNLOAD = {
  title: 'Download DLSS?',
  html: `<p>NVIDIA licenses this file separately, so it does not come with the launcher. It comes from NVIDIA's DLSS repository on GitHub, and its version and hash are checked.</p>
    <p>By downloading it you accept the NVIDIA RTX SDKs licence: <code class="selectable">licenses/LICENSE.NVIDIA-RTX-SDKs.txt</code></p>`,
  ok: 'Download',
};

export const IMPORT_FACTS = 'Your current save moves to a backup folder first, and the source is only read. A copied save cannot join its original in Link Play.';

// ---- iCloud Drive -----------------------------------------------------------------------------------------------------------------------
// env.icloud: icloud_status (icloud.rs Status): whether this Mac has iCloud Drive, when the save last synced, who wrote the copy there.

const shortTime = (iso) => {
  const d = new Date(iso ?? '');
  return Number.isNaN(+d) ? '' : d.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
};

export function icloudText(st) {
  if (!st) return 'Checking';
  if (!st.last_sync) return 'Not synced yet. It starts the next time you play.';
  const other = st.cloud_machine && st.cloud_machine !== st.this_machine ? `, copy from ${st.cloud_machine}` : '';
  return `Last synced ${shortTime(st.last_sync)}${other}`;
}

export const SECTIONS = [
  {
    id: 'display',
    label: 'Display',
    groups: [
      {
        title: 'Picture',
        rows: [
          {
            id: 'window_mode', label: 'Window', type: 'segmented', at: controls('display', 'window_mode'), def: 'windowed',
            options: [{ value: 'windowed', label: 'Windowed' }, { value: 'borderless', label: 'Borderless' }, { value: 'exclusive', label: 'Fullscreen' }],
          },
          {
            id: 'window_size', label: 'Window size', type: 'size', at: [controls('display', 'width'), controls('display', 'height')], def: '1280x720',
            min: 320, max: 16384, when: (v) => v.window_mode === 'windowed',
            options: [
              { value: '1280x720', label: '1280 x 720' }, { value: '1600x900', label: '1600 x 900' }, { value: '1920x1080', label: '1920 x 1080' },
              { value: '2560x1440', label: '2560 x 1440' }, { value: '3840x2160', label: '3840 x 2160' },
            ],
          },
          {
            // 100 is 1920 x 1080: the percentages are the game's own size, see render_scale in play_types.h
            id: 'render_scale', label: 'Render resolution', type: 'choice', at: AT_RENDER_SCALE, def: '100', quality: 'frame',
            custom: { min: 25, max: 400, suffix: '%', start: 100, label: 'Custom render resolution in percent' },
            options: [
              { value: 'auto', label: 'Match window' }, { value: '67', label: '720p' }, { value: '100', label: '1080p' },
              { value: '133', label: '1440p' }, { value: '200', label: '2160p' },
            ],
          },
          { id: 'keep_aspect', label: 'Keep 16:9 picture', type: 'toggle', at: controls('display', 'keep_aspect'), def: '1' },
        ],
      },
      {
        title: 'Frame rate',
        rows: [
          {
            id: 'fps_limit', label: 'Frame rate limit', type: 'choice', at: AT_FPS_LIMIT, def: '60', quality: 'frame',
            custom: { min: 0, max: 1000, suffix: 'fps', start: 60, label: 'Custom frame rate limit' },
            options: [
              { value: 'auto', label: 'Match display' }, { value: '30', label: '30' }, { value: '60', label: '60' },
              { value: '120', label: '120' }, { value: '144', label: '144' }, { value: '0', label: 'Unlimited' },
            ],
          },
          { id: 'vsync', label: 'VSync', type: 'toggle', at: controls('display', 'vsync'), def: IS_MAC ? '1' : '0' },
          { id: 'fps_show', label: 'Frame rate counter', type: 'toggle', at: controls('fps', 'show'), def: '0' },
        ],
      },
    ],
  },
  {
    id: 'graphics',
    label: 'Graphics',
    groups: [
      {
        title: 'Quality',
        rows: [
          {
            id: 'quality_preset', label: 'Quality preset', type: 'select', at: AT_PRESET, def: 'switch2',
            options: QUALITY_PRESETS.map((q) => ({ value: q.id, label: q.label })),
            write: (value) => presetEdits(value), replaces: true,
          },
        ],
      },
      {
        title: 'Upscaling',
        rows: [
          {
            // spatial is the runtime's other word for fsr1. ponytail: fsr3 / nis / dlss / dlaa / xess are switched off in the runtime until
            // they are reworked (#232) and run as off; add their options back here (and the dlss guard) when they return.
            id: 'upscale_mode', label: 'Upscaler', type: 'select', at: controls('upscale', 'mode'), def: IS_MAC ? 'metalfx' : 'off',
            aliases: { spatial: 'fsr1', xess: 'off', fsr3: 'off', nis: 'off', dlss: 'off', dlaa: 'off', metalfxaa: IS_MAC ? 'metalfx_aa' : 'off',
              ...(IS_MAC ? {} : { metalfx: 'off', metalfx_aa: 'off' }) }, lock: (env) => env.dlssBusy,
            options: UPSCALERS,
          },
          {
            id: 'upscale_quality', label: 'Upscaling quality', type: 'select', at: controls('upscale', 'quality'), def: 'quality',
            when: (v) => ['fsr3', 'dlss', 'metalfx'].includes(v.upscale_mode),
            options: [
              { value: 'native', label: 'Native' }, { value: 'quality', label: 'Quality' }, { value: 'balanced', label: 'Balanced' },
              { value: 'performance', label: 'Performance' }, { value: 'ultra_performance', label: 'Ultra performance' },
            ],
          },
          {
            id: 'dlss_file', label: 'DLSS file', type: 'info', value: (env) => (env.dlssPresent ? 'Downloaded' : 'Missing'),
            when: (v, env) => ['dlss', 'dlaa'].includes(v.upscale_mode) && env.dlssCanFetch,
            actions: (env) => [{ act: 'dlss-get', label: env.dlssPresent ? 'Download again' : 'Download', icon: 'download', disabled: env.dlssBusy }],
          },
        ],
      },
      { title: '', rows: [{ id: 'f1_graphics', label: 'More options in game', type: 'keycap', key: 'F1' }] },
    ],
  },
  {
    id: 'audio',
    label: 'Audio',
    groups: [
      {
        title: '',
        rows: [
          {
            id: 'volume', label: 'Volume', type: 'slider', at: AT_VOLUME, min: 0, max: 100, step: 1, suffix: '%',
            read: readVolume, write: writeVolume, enabled: (src) => hasControls(src) || has(src, AT_VOLUME_PKP),
          },
          { id: 'mute_unfocused', label: 'Mute when not focused', type: 'toggle', at: controls('audio', 'mute_unfocused'), def: '0' },
        ],
      },
    ],
  },
  {
    id: 'controls',
    label: 'Controls',
    groups: [
      {
        title: 'Mouse',
        rows: [
          { id: 'sensitivity', label: 'Mouse sensitivity', type: 'slider', at: AT_YAW, min: SENS_MIN, max: SENS_MAX, step: 0.1, read: readSensitivity, write: writeSensitivity },
          { id: 'invert_x', label: 'Invert horizontal', type: 'toggle', at: controls('mouse', 'invert_x'), def: '0' },
          { id: 'invert_y', label: 'Invert vertical', type: 'toggle', at: controls('mouse', 'invert_y'), def: '0' },
        ],
      },
      {
        title: 'Buttons',
        rows: [
          {
            id: 'prompt_style', label: 'Button icons', type: 'select', at: controls('prompts', 'style'), def: 'auto',
            aliases: { ps: 'playstation', ps4: 'playstation', ps5: 'playstation', dualsense: 'playstation', steamcontroller: 'steam', steam_controller: 'steam', deck: 'steamdeck', steam_deck: 'steamdeck', kbm: 'keyboard', keyboard_mouse: 'keyboard', mouse: 'keyboard' },
            options: [
              { value: 'auto', label: 'Automatic' }, { value: 'switch', label: 'Switch' }, { value: 'xbox', label: 'Xbox' },
              { value: 'playstation', label: 'PlayStation' }, { value: 'steam', label: 'Steam Controller' }, { value: 'steamdeck', label: 'Steam Deck' }, { value: 'keyboard', label: 'Keyboard and mouse' },
            ],
          },
          { id: 'f1_bindings', label: 'Change bindings in game', type: 'keycap', key: 'F1' },
        ],
      },
    ],
  },
  {
    // docs/CHEATS.md: the runtime's cheats overlay ([cheats] of pkp.ini). Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
    id: 'cheats',
    label: 'Cheats',
    groups: [
      {
        title: '',
        rows: [
          { id: 'cheats_enable', label: 'Enable cheats', type: 'toggle', at: pkp('cheats', 'enable'), def: '0' },
          { id: 'cheats_enable_note', type: 'note', note: () => ({ text: 'An in-game overlay to search memory and run cheat files. For single player.' }) },
          {
            // F1 is the settings menu's, so it is not offered. Stored as SDL's names; a select reads its value lower-cased, hence aliases
            id: 'cheats_key', label: 'Overlay key', type: 'select', at: pkp('cheats', 'key'), def: 'F3', aliases: CHEATS_KEY_ALIASES,
            options: [...['F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12'].map((k) => ({ value: k, label: k })),
              { value: 'Insert', label: 'Insert' }, { value: 'Home', label: 'Home' }, { value: 'End', label: 'End' },
              { value: 'PageUp', label: 'Page Up' }, { value: 'PageDown', label: 'Page Down' }, { value: 'Pause', label: 'Pause' }],
          },
          {
            // play_types.cpp parse_pad_combo; never Minus + Plus, the quit combo ([quit] pad)
            id: 'cheats_pad', label: 'Overlay controller combo', type: 'select', at: pkp('cheats', 'pad'), def: '',
            options: [
              { value: '', label: 'None' }, { value: 'minus+rstick', label: 'Minus + right stick press' },
              { value: 'minus+lstick', label: 'Minus + left stick press' }, { value: 'plus+rstick', label: 'Plus + right stick press' },
              { value: 'l+r+minus', label: 'L + R + Minus' }, { value: 'zl+zr+minus', label: 'ZL + ZR + Minus' },
            ],
          },
          {
            id: 'cheats_dir', label: 'Cheats folder', type: 'path', at: pkp('cheats', 'dir'), pick: 'Choose the cheats folder',
            placeholder: () => '<game folder>/cheats',
          },
          {
            id: 'cheats_dir_note', type: 'note',
            note: () => ({ text: 'Cheat files go in <program id>/cheats/<build id>.txt, as on the console, or as <build id>.txt at the top. The overlay also keeps its saved cheat tables there.' }),
          },
          { id: 'cheats_pass_input', label: 'While open: input reaches the game', type: 'toggle', at: pkp('cheats', 'pass_input'), def: '0' },
          { id: 'cheats_pause_game', label: 'While open: pause the game', type: 'toggle', at: pkp('cheats', 'pause_game'), def: '0' },
          { id: 'cheats_mini', label: 'Active cheats strip when hidden', type: 'toggle', at: pkp('cheats', 'mini'), def: '0' },
        ],
      },
      {
        title: 'Overlay look',
        rows: [
          {
            id: 'cheats_theme', label: 'Theme', type: 'select', at: pkp('cheats', 'theme'), def: CHEATS_LOOK.theme.default,
            desc: 'The overlay\u2019s colours, corners and style. Your own themes (JSON files) go in the themes folder.',
            read: (src) => String(has(src, pkp('cheats', 'theme')) ? rawOf(src, pkp('cheats', 'theme')) : CHEATS_LOOK.theme.default).trim(),
            options: (env = {}) => [
              ...CHEATS_LOOK.theme.builtin.map((t) => ({ value: t, label: CHEATS_THEME_LABELS[t] ?? t })),
              ...(env.cheatsThemes ?? []).map((t) => ({ value: t.path, label: `${t.name} (your theme)` })),
            ],
          },
          {
            id: 'cheats_theme_files', label: 'Your themes', type: 'info',
            value: (e) => (e.cheatsThemes?.length ? `${e.cheatsThemes.length} in the themes folder` : 'None yet'),
            actions: [
              { act: 'cheats-theme-import', label: 'Import theme\u2026', icon: 'upload' },
              { act: 'cheats-themes-open', label: 'Open the folder', icon: 'folder-open' },
            ],
          },
          {
            id: 'cheats_style', label: 'Panel', type: 'select', at: pkp('cheats', 'style'), def: '',
            options: [{ value: '', label: 'The theme\u2019s' }, { value: 'solid', label: 'Solid' }, { value: 'glass', label: 'Glass: the game blurred behind it' }],
          },
          {
            id: 'cheats_opacity', label: 'Panel opacity', type: 'choice', at: pkp('cheats', 'opacity'), def: '',
            options: [{ value: '', label: 'The theme\u2019s' }],
            custom: { min: CHEATS_LOOK.opacity.min, max: CHEATS_LOOK.opacity.max, start: 90, label: 'Panel opacity', suffix: '%' },
          },
          {
            id: 'cheats_blur', label: 'Glass blur', type: 'choice', at: pkp('cheats', 'blur'), def: '',
            options: [{ value: '', label: 'The theme\u2019s' }],
            custom: { min: CHEATS_LOOK.blur.min, max: CHEATS_LOOK.blur.max, start: 60, label: 'Blur', suffix: '%' },
          },
          {
            id: 'cheats_tint', label: 'Glass tint', type: 'choice', at: pkp('cheats', 'tint'), def: '',
            options: [{ value: '', label: 'The theme\u2019s' }],
            custom: { min: CHEATS_LOOK.tint.min, max: CHEATS_LOOK.tint.max, start: 55, label: 'Tint', suffix: '%' },
          },
          {
            id: 'cheats_accent', label: 'Accent', type: 'accent', at: pkp('cheats', 'accent'), def: CHEATS_LOOK.accent.default,
            read: (src) => String(has(src, pkp('cheats', 'accent')) ? rawOf(src, pkp('cheats', 'accent')) : CHEATS_LOOK.accent.default).trim(),
            options: [
              { value: 'theme', label: 'The theme\u2019s' },
              { value: 'auto', label: 'From the game picture' },
              ...['blue', 'green', 'amber', 'violet', 'red', 'teal', 'slate'].map((c) => ({ value: c, label: c[0].toUpperCase() + c.slice(1) })),
              { value: 'custom', label: 'Your own colour' },
            ],
            color: { start: '#ffc23d', label: 'Accent colour' },
          },
          {
            id: 'cheats_scale', label: 'Size', type: 'slider', at: pkp('cheats', 'scale'), def: CHEATS_LOOK.scale.default, min: CHEATS_LOOK.scale.min,
            max: CHEATS_LOOK.scale.max, step: CHEATS_LOOK.scale.step, suffix: '%',
          },
          {
            id: 'cheats_hud_opacity', label: 'HUD over the game', type: 'slider', at: pkp('cheats', 'hud_opacity'), def: CHEATS_LOOK.hud_opacity.default,
            min: CHEATS_LOOK.hud_opacity.min, max: CHEATS_LOOK.hud_opacity.max, step: CHEATS_LOOK.hud_opacity.step, suffix: '%',
            desc: 'Values and cheats you pin in the overlay show over the game in a small strip, at this opacity.',
          },
          { id: 'cheats_animations', label: 'Animations', type: 'toggle', at: pkp('cheats', 'animations'), def: CHEATS_LOOK.animations.default ? '1' : '0' },
        ],
      },
    ],
  },
  {
    id: 'saves',
    label: 'Saves',
    groups: [
      {
        title: 'Profiles',
        rows: [
          { id: 'profiles_note', type: 'note', note: () => ({ text: 'Each profile has its own save, so two people can share one install. They share the game files and the shaders. Switch profiles with the game closed.' }) },
          {
            expand: (env) => (env.profiles?.profiles ?? []).map((p) => {
              const on = p.id === env.profiles.active;
              return {
                id: `profile_${p.id}`, label: p.name, type: 'info', value: on ? 'Playing' : '',
                actions: [
                  ...(on ? [] : [{ act: 'profile-use', arg: p.id, name: p.name, label: 'Use', icon: 'user-check', disabled: env.gameBusy }]),
                  { act: 'profile-rename', arg: p.id, name: p.name, label: 'Rename', icon: 'pencil', disabled: env.gameBusy },
                  ...(p.id === 'default' ? [] : [{ act: 'profile-remove', arg: p.id, name: p.name, label: 'Remove', icon: 'trash-2', disabled: env.gameBusy || on }]),
                ],
              };
            }),
          },
          {
            id: 'profile_new', label: 'New profile', type: 'info',
            actions: (env) => [{ act: 'profile-new', label: 'Add', icon: 'plus', disabled: env.gameBusy }],
          },
        ],
      },
      {
        title: '',
        rows: [{
          id: 'save_dir', label: 'Save folder', type: 'path', at: pkp('fs', 'save_dir'), pick: 'Choose the save folder', open: 'save',
          placeholder: (env) => `${env.gameDir}/save`,
        }],
      },
      {
        title: 'Backups',
        rows: [
          {
            id: 'backup_note', type: 'note',
            note: () => ({ text: 'Copies your save every few minutes while you play, so you can go back if a bug stops your progress. It is off until you pick a time. A copy is also made when the game closes. Restoring needs the game closed, and your current save is copied first.' }),
          },
          {
            id: 'backup_minutes', label: 'Back up automatically', type: 'select', at: pkp('backup', 'interval_minutes'), def: '0',
            options: [{ value: '0', label: 'Off' }, { value: '5', label: 'Every 5 minutes' }, { value: '10', label: 'Every 10 minutes' }, { value: '15', label: 'Every 15 minutes' }],
          },
          {
            id: 'backup_keep', label: 'Backups to keep', type: 'select', at: pkp('backup', 'keep'), def: '10', when: (v) => v.backup_minutes !== '0',
            options: [{ value: '5', label: '5' }, { value: '10', label: '10' }, { value: '20', label: '20' }, { value: '30', label: '30' }],
          },
          {
            id: 'backup_state', label: 'Backups', type: 'info', value: (env) => env.backupLine,
            actions: (env) => [
              { act: 'backup-now', label: 'Back up now', icon: 'save' },
              { act: 'backup-restore', label: 'Restore', icon: 'rotate-ccw', disabled: env.gameBusy || !env.backups?.count },
              { act: 'backup-open', label: 'Open folder', icon: 'folder-open' },
            ],
          },
          {
            // savebackup.rs backup_root: new backups go here; ones made before stay where they are
            id: 'backup_dir', label: 'Backup folder', type: 'path', at: pkp('backup', 'dir'), pick: 'Choose the backup folder', open: 'backups',
            placeholder: (env) => `${env.gameDir}/save-backups`,
          },
          { id: 'backup_detail', type: 'note', when: (v, env) => !!env.backupDetail, note: (env) => ({ text: env.backupDetail }) },
        ],
      },
      {
        title: 'iCloud Drive',
        rows: [
          {
            id: 'icloud_note', type: 'note', when: (v, env) => !!env.icloud?.available,
            note: () => ({ text: 'Copies your save to iCloud Drive when the game closes, and brings in a newer one from your other Mac before it starts. The game always plays from this Mac, and your save here is backed up before anything replaces it.' }),
          },
          { id: 'icloud_sync', label: 'Keep my saves in iCloud Drive', type: 'toggle', at: config('icloud_sync'), def: false, when: (v, env) => !!env.icloud?.available, lock: (env) => env.gameBusy },
          {
            id: 'icloud_state', label: 'iCloud Drive', type: 'info', when: (v, env) => !!env.icloud?.available && v.icloud_sync,
            value: (env) => icloudText(env.icloud),
            actions: [{ act: 'icloud-open', label: 'Open folder', icon: 'folder-open' }],
          },
          {
            id: 'icloud_conflicts', type: 'note', when: (v, env) => !!env.icloud?.available && v.icloud_sync && env.icloud.conflicts > 0,
            note: (env) => ({ text: `${env.icloud.conflicts} older ${env.icloud.conflicts === 1 ? 'copy is' : 'copies are'} kept in the conflicts folder there, from times your save changed on two Macs between syncs. The game does not use them: delete them when you no longer need them.` }),
          },
        ],
      },
      {
        title: 'Import a save',
        rows: [
          {
            expand: (env) => env.saveSources.map((s, i) => ({
              id: `source_${i}`, label: s.label, type: 'info', value: s.path, mono: true,
              actions: [{ act: 'import', arg: s.path, name: s.label, label: 'Import', icon: 'upload', disabled: env.gameBusy || env.importing }],
            })),
          },
          {
            id: 'import_folder', label: 'From a folder', type: 'info',
            actions: (env) => [{ act: 'import-folder', label: 'Choose a folder', icon: 'folder-open', disabled: env.gameBusy || env.importing }],
          },
          { id: 'import_result', type: 'note', when: (v, env) => !!env.importResult, note: (env) => env.importResult },
        ],
      },
    ],
  },
  {
    id: 'game',
    label: 'Game',
    groups: [
      {
        title: '',
        rows: [
          {
            id: 'game_files', label: 'Game files', type: 'info', value: (env) => env.gameFiles, mono: true,
            actions: [{ act: 'change-files', label: 'Change', icon: 'folder-open' }],
          },
          {
            id: 'setup_again', label: 'Set up again', type: 'info',
            actions: (env) => [{ act: 'setup-again', label: 'Set up', icon: 'wrench', disabled: !env.canSetUp }],
          },
          { id: 'setup_log', label: 'Setup log', type: 'info', actions: [{ act: 'setup-log', label: 'Open', icon: 'file-text' }] },
        ],
      },
      {
        title: 'Gameplay',
        rows: [
          { id: 'gameplay_note', type: 'note', note: () => ({ text: 'New ways of playing, on by default (switch them off here). They take effect the next time the game starts, and they need the game built with this version (Set up again once after an update).' }) },
          { id: 'camera_aim', label: 'Camera aim', type: 'toggle', at: pkp('pkp', 'camera_aim'), def: '1' },
          { id: 'camera_aim_note', type: 'note', when: (v) => v.camera_aim === true || v.camera_aim === '1', note: () => ({ text: 'Abilities and tools aim where the camera looks instead of where your character faces, and your character turns to the target when the ability starts. They reach 2 blocks farther than the cell next to you (camera_aim_reach in pkp.ini; 0 = the game\'s own range).' }) },
          { id: 'jump_momentum', label: 'Running jump keeps speed', type: 'toggle', at: pkp('pkp', 'jump_momentum'), def: '1' },
        ],
      },
      {
        title: 'Steam',
        rows: [
          {
            id: 'steam_note', type: 'note',
            note: (env) => ({ text: env.steam && !env.steam.found
              ? 'Steam was not found on this PC, or nobody has signed in to it yet. Start Steam once, sign in, close it, and come back.'
              : 'Adds a shortcut with the PokoMac artwork to your Steam library, so it shows in Game Mode and Big Picture. Close Steam first, then start it again afterwards. Add the game to start playing at once, or the launcher to open this window first.' }),
          },
          {
            id: 'steam_game', label: 'Game in Steam', type: 'info', when: (v, env) => !!env.steam?.found,
            value: (env) => (env.steam?.game_added ? `In Steam as ${env.steam.game_name}` : `Not in Steam. Starts the game at once.`),
            actions: (env) => [env.steam?.game_added
              ? { act: 'steam-remove-game', label: 'Remove', icon: 'trash-2', disabled: env.steamBusy || env.steam?.running }
              : { act: 'steam-add-game', label: 'Add to Steam', icon: 'plus', disabled: env.steamBusy || env.steam?.running }],
          },
          {
            id: 'steam_launcher', label: 'Launcher in Steam', type: 'info', when: (v, env) => !!env.steam?.found,
            value: (env) => (env.steam?.launcher_added ? `In Steam as ${env.steam.launcher_name}` : `Not in Steam. Opens this window.`),
            actions: (env) => [env.steam?.launcher_added
              ? { act: 'steam-remove-launcher', label: 'Remove', icon: 'trash-2', disabled: env.steamBusy || env.steam?.running }
              : { act: 'steam-add-launcher', label: 'Add to Steam', icon: 'plus', disabled: env.steamBusy || env.steam?.running }],
          },
          { id: 'steam_running', type: 'note', when: (v, env) => !!env.steam?.running, note: () => ({ text: 'Steam is running. Close it (Steam, Exit) to add or remove a shortcut.' }) },
        ],
      },
      {
        title: 'Shader files',
        rows: [
          { id: 'shader_storage', label: 'Storage', type: 'info', value: storageText, actions: storageActions },
          { id: 'shader_storage_note', type: 'note', when: (v, env) => !!storageNote(env), note: storageNote },
        ],
      },
      {
        title: '',
        rows: [
          {
            // saved on leaving the field, not while typing: half a path would make the whole launcher think the game is gone
            id: 'game_dir', label: 'Install folder', type: 'path', at: config('game_dir'), pick: 'Choose the install folder', open: 'game',
            placeholder: (env) => env.defaultGameDir, commit: 'blur', reloads: true,
          },
          {
            id: 'romfs_mod', label: 'Mods', type: 'path', at: pkp('fs', 'romfs_mod'), pick: 'Choose the mods folder',
            placeholder: () => '',
          },
          {
            // the game's photos (runtime album.cpp, [album] dir; relative paths are inside the install folder)
            id: 'album_dir', label: 'Photos', type: 'path', at: pkp('album', 'dir'), pick: 'Choose the folder for the game’s photos', open: 'photos',
            placeholder: (env) => `${env.gameDir}/album`,
          },
          {
            id: 'romfs_mod_note', type: 'note',
            note: () => ({ text: "Choose the folder that holds the same files and folders as the game's romfs (for example LD_C0_R.BIN at its top level), not the folder around it. Its files replace the game's files with the same path." }),
          },
          { id: 'minimize_on_play', label: 'Minimise while playing', type: 'toggle', at: config('minimize_on_play'), def: false },
        ],
      },
      {
        title: 'Game clock',
        rows: [
          ...TIMED_EVENTS.map((event) => ({
            id: `event_${event.id}`, label: event.label, type: 'toggle', at: AT_EVENT_PRESET, def: false,
            read: (src) => activeEvent(src) === event.id,
            write: (on) => [edit(AT_EVENT_PRESET, on ? event.id : 'off')], replaces: true,
          })),
          {
            id: 'game_clock', label: 'Date and time in the game', type: 'clock', at: AT_CLOCK_OFFSET, def: '0', commit: 'blur',
            write: (value, src) => clockEdits(value, src), writeSame: scopeNeedsReset, enabled: (src) => activeEvent(src) === 'off',
          },
          {
            id: 'game_clock_note', type: 'note',
            note: () => ({ text: 'Use this in single player only for now. Play applies the enabled event\'s start date automatically. Your ordinary Game clock is kept and used again when all events are off.' }),
          },
        ],
      },
    ],
  },
  {
    id: 'about',
    label: 'About',
    groups: [
      {
        title: '',
        rows: [
          { id: 'version', label: 'Version', type: 'info', value: (env) => env.version, selectable: true },
          {
            id: 'channel', label: 'Update channel', type: 'select', at: config('channel'), def: 'stable', guard: 'testing',
            lock: (env) => env.updating || env.gameBusy,
            options: [{ value: 'stable', label: 'Stable' }, { value: 'testing', label: 'Testing' }],
          },
          {
            id: 'channel_note', type: 'note', when: (v) => v.channel === 'testing',
            note: () => ({ text: 'Testing builds come first and may have bugs. As a tester you are expected to tell us what you find and share pkp.log: use Report a problem below. Your saves and settings are kept. To go back to Stable, wait until Stable has a newer version than the one you have, or install it from the releases page.' }),
          },
          {
            id: 'testing_help', label: 'Testing help', type: 'info', when: (v) => v.channel === 'testing',
            value: 'What to test is listed with each Testing build in What\'s New.',
            actions: () => [
              { act: 'report', label: 'Report a problem', icon: 'file-archive' },
              { act: 'open-log', label: 'Show pkp.log', icon: 'file-text' },
            ],
          },
          {
            id: 'builds', label: 'Versions', type: 'info', value: (env) => `You have ${env.version}`,
            actions: (env) => [{ act: 'builds-open', label: 'Choose a version', icon: 'download', disabled: env.gameBusy || env.updating || env.taskBusy }],
          },
          {
            id: 'updates', label: 'Updates', type: 'info', value: (env) => env.updateText,
            actions: (env) => [
              ...(env.canUpdate ? [{ act: 'update', label: 'Update', icon: 'download', primary: true }] : []),
              { act: 'check-updates', label: 'Check for updates', icon: 'refresh-cw', disabled: env.checking },
            ],
          },
          { id: 'licence', label: 'Licence', type: 'info', value: 'GPL-3.0-or-later' },
          { id: 'built_with', label: 'Built with', type: 'info', value: 'Tauri 2, Lucide (ISC), Inter (OFL)' },
          { id: 'repo', label: 'Source code', type: 'info', actions: [open(REPO_URL, 'GitHub')] },
          { id: 'built_on', label: 'Built on', type: 'info', value: 'Built on AnySwitch by Chaos192 — github.com/Chaos192/AnySwitch', actions: [open(ANYSWITCH_URL, 'GitHub')] },
          { id: 'based_on', label: 'Credits', type: 'info', value: 'Based on Recompia by Dittobyte — github.com/Dittobyte/Recompia', actions: [open(UPSTREAM_URL, 'GitHub')] },
          { id: 'koi-credit', label: "Koi's Camp", type: 'info', value: 'Artwork by Koi, hand drawn' },
          {
            id: 'bug', label: 'Report a problem', type: 'info', value: 'Puts the logs in a zip on your Desktop and opens a GitHub issue',
            actions: [{ act: 'report', label: 'Report a problem', icon: 'file-archive' }],
          },
        ],
      },
      {
        title: 'Troubleshooting',
        rows: [
          {
            id: 'repair', label: 'Repair and reset', type: 'info',
            value: 'Settings, caches and old copies. You see what changes first.',
            actions: (env) => [{ act: 'repair-open', label: 'Repair and reset', icon: 'wrench', disabled: env.gameBusy || env.updating || env.taskBusy }],
          },
          {
            id: 'repair_undo', label: 'Undo last repair', type: 'info', when: (v, env) => !!env.repairBackup,
            value: (env) => (env.repairBackup ? `Puts back ${env.repairBackup.files.join(', ')}` : ''),
            actions: (env) => [{ act: 'repair-undo', label: 'Undo', icon: 'rotate-ccw', disabled: env.gameBusy || env.updating || env.taskBusy }],
          },
        ],
      },
      {
        title: '',
        rows: [{
          id: 'legal', type: 'note',
          note: () => ({ text: 'No game files are included. You need your own copy of the game, from a console you own. This project is not affiliated with or endorsed by Nintendo, The Pokemon Company or Koei Tecmo.' }),
        }],
      },
    ],
  },
];

export const FIELDS = Object.fromEntries(
  SECTIONS.flatMap((s) => s.groups.flatMap((g) => g.rows)).filter((r) => r.at).map((f) => [f.id, f]),
);

// ---- reading and writing ------------------------------------------------------------------------------------------------------------

export const makeSource = (dump, cfg) => ({ pkp: dump.pkp, controls: dump.controls, config: cfg, controls_exists: dump.controls_exists });

export const readField = (f, src) => (f.read ? f.read(src) : READ[f.type](f, src));

export const readAll = (src) => Object.fromEntries(Object.values(FIELDS).map((f) => [f.id, readField(f, src)]));

/**
 * What it takes to give field `f` this value: nothing when that is what it already reads as, and never a key that already holds the
 * text or a file that does not exist. Reading and writing back is therefore no edit at all, so a value the page cannot show survives.
 */
export function writeField(f, value, src) {
  if (String(readField(f, src)) === String(value) && !f.writeSame?.(src)) return [];
  const edits = f.write ? f.write(value, src) : WRITE[f.type](f, value);
  if (f.quality) edits.push(...markCustom(f.quality, src));
  return edits.filter((e) => !(e.file === 'controls' && src.controls_exists === false) && String(rawOf(src, e)) !== String(e.value));
}

/** The source as it reads after the edits. */
export function applyEdits(src, edits) {
  const next = { ...src, pkp: { ...src.pkp }, controls: { ...src.controls }, config: { ...src.config } };
  for (const e of edits) next[e.file][keyOf(e)] = e.value;
  return next;
}

/** { ini: edits for write_settings, config: the keys for save_config, or null } */
export function splitEdits(edits) {
  const own = edits.filter((e) => e.file === 'config');
  return { ini: edits.filter((e) => e.file !== 'config'), config: own.length ? Object.fromEntries(own.map((e) => [e.key, e.value])) : null };
}

// ---- what a control shows ---------------------------------------------------------------------------------------------------------

/** The option a value selects, 'custom' when it is none of them. Never changes the value itself. */
export function shownChoice(f, value, env = {}) {
  return optionList(f, env).some((o) => o.value === String(value)) ? String(value) : 'custom';
}

/** A field's options: its list, or what its function makes of the page's env (the cheats overlay's theme files). */
const optionList = (f, env = {}) => (typeof f.options === 'function' ? f.options(env) : f.options);

/** The options to offer. Custom is a real choice on a number control, and on a plain list only the name of a value it cannot show. */
export function optionsFor(f, value, env = {}) {
  const list = optionList(f, env).filter((o) => !o.offer || o.offer(env) || o.value === value).map(({ value: v, label }) => ({ value: v, label }));
  if (f.type === 'choice' || f.type === 'size') return [...list, { value: 'custom', label: 'Custom' }];
  return list.some((o) => o.value === value) ? list : [...list, { value: 'custom', label: 'Custom', disabled: true }];
}

/** What a number field starts from when Custom is picked: the value when it is a number, else the control's starting point. */
export function customStart(f, value) {
  return /^\d+$/.test(String(value)) ? String(value) : String(f.custom.start);
}

/** A whole number from a number field, kept in range; null when the text is not a number. */
export function fitNumber(text, min, max) {
  const n = Math.round(Number(String(text).trim() || NaN));
  return Number.isFinite(n) ? clamp(n, min, max) : null;
}

export const isVisible = (row, values, env) => (row.when ? row.when(values, env) : true);

/** The file is there. A field in controls.ini waits for the game to write it: the launcher never creates that file (settings.rs apply). */
export function isAvailable(f, src) {
  if (f.enabled) return f.enabled(src);
  return !atsOf(f).some((at) => at.file === 'controls') || src.controls_exists !== false;
}

export const isEnabled = (f, src, env) => isAvailable(f, src) && !f.lock?.(env);

/** The rows of a group with `expand` entries unfolded and hidden rows gone. */
export const rowsOf = (group, values, env) => group.rows.flatMap((r) => (r.expand ? r.expand(env) : [r])).filter((r) => isVisible(r, values, env));
