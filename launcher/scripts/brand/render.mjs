// SPDX-License-Identifier: GPL-3.0-or-later
// Renders the PokoMac brand from the SVG sources beside this file (mark.svg, mark-testing.svg, wordmark.svg) into everything that shows
// it: the launcher's art (src/art: logo.png, mark.svg, mark-testing.png), the app icons (src-tauri/icons and icons-testing: the PNG sizes,
// icon.ico, icon.icns, icon-source.svg) and the Steam artwork (src-tauri/assets/steam: icon, logo, the grids on the island picture, the
// hero). Runs on macOS (sips reads the island's WebP, iconutil writes the .icns), with resvg for the SVGs:
//   cd launcher && npm i --no-save @resvg/resvg-js@2.6.2 && node scripts/brand/render.mjs
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const { Resvg } = require('@resvg/resvg-js');

const here = dirname(fileURLToPath(import.meta.url));
const launcher = join(here, '..', '..');
const art = join(launcher, 'src', 'art');
const tauri = join(launcher, 'src-tauri');
const tmp = mkdtempSync(join(tmpdir(), 'pokomac-brand-'));

const svg = (name) => readFileSync(join(here, name), 'utf8');
/** PNG bytes of an SVG text at a width (the height follows the viewBox). */
const png = (text, width) => new Resvg(text, { fitTo: { mode: 'width', value: width }, background: 'rgba(0,0,0,0)' }).render().asPng();
const put = (path, bytes) => {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, bytes);
  console.log(`wrote ${path.slice(launcher.length + 1)}`);
};

/** An .ico of PNG images (Vista and newer read PNG entries): 6-byte header, 16 bytes per entry, then the images. */
function ico(text, sizes) {
  const images = sizes.map((s) => png(text, s));
  const head = Buffer.alloc(6 + 16 * sizes.length);
  head.writeUInt16LE(0, 0);
  head.writeUInt16LE(1, 2);
  head.writeUInt16LE(sizes.length, 4);
  let offset = head.length;
  sizes.forEach((s, i) => {
    const e = 6 + 16 * i;
    head.writeUInt8(s >= 256 ? 0 : s, e);
    head.writeUInt8(s >= 256 ? 0 : s, e + 1);
    head.writeUInt16LE(1, e + 4);
    head.writeUInt16LE(32, e + 6);
    head.writeUInt32LE(images[i].length, e + 8);
    head.writeUInt32LE(offset, e + 12);
    offset += images[i].length;
  });
  return Buffer.concat([head, ...images]);
}

/** An .icns through iconutil, from an iconset of the sizes macOS asks for. */
function icns(text) {
  const set = join(tmp, `${Math.random().toString(36).slice(2)}.iconset`);
  mkdirSync(set);
  for (const s of [16, 32, 128, 256, 512]) {
    writeFileSync(join(set, `icon_${s}x${s}.png`), png(text, s));
    writeFileSync(join(set, `icon_${s}x${s}@2x.png`), png(text, s * 2));
  }
  const out = join(tmp, 'icon.icns');
  execFileSync('iconutil', ['-c', 'icns', set, '-o', out]);
  return readFileSync(out);
}

function icons(dir, source) {
  const text = svg(source);
  put(join(dir, '32x32.png'), png(text, 32));
  put(join(dir, '128x128.png'), png(text, 128));
  put(join(dir, '128x128@2x.png'), png(text, 256));
  put(join(dir, 'icon.png'), png(text, 512));
  put(join(dir, 'icon.ico'), ico(text, [16, 24, 32, 48, 64, 128, 256]));
  put(join(dir, 'icon.icns'), icns(text));
  put(join(dir, 'icon-source.svg'), Buffer.from(text));
}

// the wordmark's own drawing, without its <svg> wrapper, to place it on a picture
const word = svg('wordmark.svg');
const [vx, vy, vw, vh] = word.match(/viewBox="([^"]+)"/)[1].split(/\s+/).map(Number);
const wordInner = word.replace(/^[\s\S]*?<svg[^>]*>/, '').replace(/<\/svg>\s*$/, '');

/** The island picture (src/art/island-sharp.webp, the launcher's own background) as a PNG data URI. */
function island() {
  const out = join(tmp, 'island.png');
  execFileSync('sips', ['-s', 'format', 'png', join(art, 'island-sharp.webp'), '--out', out], { stdio: 'ignore' });
  return `data:image/png;base64,${readFileSync(out).toString('base64')}`;
}

/** A w x h picture: the island, cropped to fill, and the wordmark `scale` of the width wide, centred at (cx, cy) as fractions. */
function poster(w, h, picture, mark) {
  let g = '';
  if (mark) {
    const ww = w * mark.scale;
    const s = ww / vw;
    const x = w * mark.cx - ww / 2;
    const y = h * mark.cy - (vh * s) / 2;
    g = `<g transform="translate(${x} ${y}) scale(${s}) translate(${-vx} ${-vy})">${wordInner}</g>`;
  }
  const text = `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">
    <image href="${picture}" x="0" y="0" width="${w}" height="${h}" preserveAspectRatio="xMidYMid slice"/>${g}</svg>`;
  return new Resvg(text, { fitTo: { mode: 'original' } }).render().asPng();
}

/** The wordmark centred on a transparent w x h canvas, `scale` of the width wide. */
function logo(w, h, scale) {
  const ww = w * scale;
  const s = ww / vw;
  const text = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">
    <g transform="translate(${(w - ww) / 2} ${(h - vh * s) / 2}) scale(${s}) translate(${-vx} ${-vy})">${wordInner}</g></svg>`;
  return new Resvg(text, { fitTo: { mode: 'original' } }).render().asPng();
}

try {
  // the launcher page: the wordmark (home, first run, sidebar) and the sidebar marks
  put(join(art, 'logo.png'), png(word, 1000));
  copyFileSync(join(here, 'mark.svg'), join(art, 'mark.svg'));
  console.log('wrote src/art/mark.svg');
  put(join(art, 'mark-testing.png'), png(svg('mark-testing.svg'), 128));
  // the app's icons (window, dock, taskbar, bundle)
  icons(join(tauri, 'icons'), 'mark.svg');
  icons(join(tauri, 'icons-testing'), 'mark-testing.svg');
  // Add to Steam: Steam's sizes (the hero carries no logo: Steam lays logo.png over it)
  const steam = join(tauri, 'assets', 'steam');
  const pic = island();
  put(join(steam, 'icon.png'), png(svg('mark.svg'), 256));
  put(join(steam, 'icon_testing.png'), png(svg('mark-testing.svg'), 256));
  put(join(steam, 'logo.png'), logo(1280, 486, 0.86));
  put(join(steam, 'grid_wide.png'), poster(920, 430, pic, { scale: 0.62, cx: 0.5, cy: 0.3 }));
  put(join(steam, 'grid_portrait.png'), poster(600, 900, pic, { scale: 0.8, cx: 0.5, cy: 0.2 }));
  put(join(steam, 'hero.png'), poster(1920, 620, pic, null));
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
