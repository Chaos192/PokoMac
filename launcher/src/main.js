import '@fontsource-variable/inter/opsz.css';
import './theme.css';
import './layout.css';
import './components.css';
import { getCurrentWindow } from '@tauri-apps/api/window';
import markUrl from './art/mark.svg';
import markTestingUrl from './art/mark-testing.png';
import logoUrl from './art/logo.png';
import { api, errText, esc, icon } from './ui.js';
import { store } from './store.js';
import { isFirstRun } from './action.js';
import { startGamepadNav } from './gamepad-nav.js';
import * as home from './pages/home.js';
import * as linkplay from './pages/linkplay.js';
import * as news from './pages/news.js';
import * as settings from './pages/settings.js';
import * as setup from './pages/setup.js';

// nav: shown in the sidebar. art: 'sharp' shows the island, otherwise the blurred copy.
const PAGES = {
  home: { mod: home, nav: 'Home', icon: 'house', art: 'sharp' },
  linkplay: { mod: linkplay, nav: 'Link Play', icon: 'users' },
  news: { mod: news, nav: "What's New", icon: 'newspaper' },
  settings: { mod: settings, nav: 'Settings', icon: 'settings' },
  setup: { mod: setup },
};

// Dev in a plain browser only: the mock stands in for Rust, ?page=<id> opens a page directly. Vite drops this from production.
let startPage = null;
if (import.meta.env.DEV && !window.__TAURI_INTERNALS__) {
  await import('./dev/mock.js');
  PAGES.kit = { mod: await import('./dev/kit.js') };
  startPage = new URLSearchParams(location.search).get('page');
}

// where the sidebar pill leads
const TASK_PAGE = { setup: 'setup', nup: 'setup', update: 'home', dlss: 'settings', repack: 'home' };

const SWAP_WAIT_MS = 120;   // a page that loads fast swaps in whole; a slow one shows what it has by then

const nav = document.getElementById('nav');
const view = document.getElementById('view');
let current = null;
let token = 0;           // the newest navigation; older ones drop what they rendered
let leaving = [];        // run when the page goes

// Launcher colours: 'sunrise' (amber to red, the default; reads well with red-green colour blindness), 'dusk' (the old violet) or 'koi' (a bright daytime picture).
// Per PC, kept in localStorage: nothing else needs it, and losing it just shows the default.
const THEMES = ['sunrise', 'dusk', 'koi'];
const THEME_NAME = { sunrise: 'Sunrise', dusk: 'Dusk', koi: "Koi's Camp" };
function readTheme() {
  try { return THEMES.includes(localStorage.getItem('theme')) ? localStorage.getItem('theme') : 'sunrise'; } catch { return 'sunrise'; }
}
function applyTheme(t) {
  document.documentElement.dataset.theme = t;
  const b = document.getElementById('theme-btn');
  if (b) b.querySelector('.nav-label').textContent = b.querySelector('.nav-tip').textContent = `Colours: ${THEME_NAME[t]}`;
}
applyTheme(readTheme());

function drawNav() {
  const items = Object.entries(PAGES).filter(([, p]) => p.nav).map(([id, p]) =>
    `<button class="nav-item" data-page="${id}" aria-label="${esc(p.nav)}">${icon(p.icon)}<span class="nav-label">${esc(p.nav)}</span><span class="nav-tip" aria-hidden="true">${esc(p.nav)}</span></button>`);
  nav.innerHTML = `
    <div class="brand" data-tauri-drag-region><img class="brand-mark" src="${markUrl}" alt="" /><img class="brand-mark-testing" src="${markTestingUrl}" alt="Testing" /><img class="brand-name brand-logo" src="${logoUrl}" alt="PokoMac" /><span class="brand-tag">Testing</span></div>
    ${items.join('')}
    <div class="nav-spacer"></div>
    <button class="nav-item" id="theme-btn" aria-label="Change colours">${icon('palette')}<span class="nav-label"></span><span class="nav-tip" aria-hidden="true"></span></button>
    <button class="task-pill" id="task-pill" hidden>
      ${icon('loader-circle', 'spin')}<span class="task-name clip"></span><span class="task-pct"></span>
      <span class="task-track"><span class="task-fill"></span></span>
    </button>`;
  applyTheme(readTheme());
  nav.addEventListener('click', (e) => {
    if (e.target.closest('#theme-btn')) {
      const next = THEMES[(THEMES.indexOf(readTheme()) + 1) % THEMES.length];
      try { localStorage.setItem('theme', next); } catch { /* the choice lasts until the launcher closes */ }
      applyTheme(next);
    }
    const page = e.target.closest('[data-page]')?.dataset.page;
    if (page && page !== current) go(page);
    const target = e.target.closest('#task-pill') && TASK_PAGE[store.state.task?.kind];
    if (target && target !== current) go(target);
  });
}

/** The Testing channel is marked in the sidebar (the Testing icon and a tag), so a tester can tell this launcher from a Stable one at a glance. */
async function syncChannel() {
  try {
    const c = await api('get_config');
    document.documentElement.dataset.channel = c?.channel === 'testing' ? 'testing' : 'stable';
  } catch (e) {
    console.warn('get_config', e);
  }
}

function drawChrome() {
  for (const b of nav.querySelectorAll('[data-page]')) {
    if (b.dataset.page === current) b.setAttribute('aria-current', 'page');
    else b.removeAttribute('aria-current');
  }
  // the update has its own dot beside What's New, where its notes are
  const item = nav.querySelector('[data-page="news"]');
  const hasDot = !!item.querySelector('.nav-dot');
  if (hasDot !== !!store.state.news.update) {
    if (hasDot) item.querySelector('.nav-dot').remove();
    else item.insertAdjacentHTML('beforeend', '<span class="nav-dot"></span>');
  }

  const pill = document.getElementById('task-pill');
  const task = store.state.task;
  pill.hidden = !task;
  if (!task) return;
  const pct = task.pct === null ? '' : `${task.pct}%`;
  const say = pct ? `${task.label}, ${pct}` : task.label;
  pill.querySelector('.task-name').textContent = task.label;
  pill.querySelector('.task-pct').textContent = pct;
  pill.title = say;
  pill.setAttribute('aria-label', say);
  pill.style.setProperty('--pct', pct || '0%');
  pill.toggleAttribute('data-indeterminate', task.pct === null);
}

/** Shows a page. opts land on the page's ctx (ctx.firstRun, ctx.tag...); firstRun on setup hides the sidebar. */
async function go(page, opts = {}) {
  if (!PAGES[page]) return;
  const id = ++token;
  leaving.forEach((fn) => fn());
  leaving = [];
  current = page;
  document.body.dataset.shell = page === 'setup' && opts.firstRun ? 'full' : '';
  document.body.dataset.art = PAGES[page].art || 'soft';
  drawChrome();

  const host = document.createElement('div');
  host.className = 'page-host';
  const ctx = {
    ...opts,
    go,
    on(fn) {
      if (id !== token) return () => {};
      const off = store.subscribe(fn);
      leaving.push(off);
      return off;
    },
  };
  const rendered = (async () => {
    try {
      const dispose = await PAGES[page].mod.render(host, ctx);
      if (typeof dispose === 'function') (id === token ? leaving.push(dispose) : dispose());
    } catch (e) {
      console.error(e);
      host.innerHTML = `<div class="page"><div class="banner danger">${icon('circle-x')}<div>${esc(errText(e))}</div></div></div>`;
    }
  })();
  await Promise.race([rendered, new Promise((r) => setTimeout(r, SWAP_WAIT_MS))]);
  if (id !== token) return;

  host.classList.add('entering');
  host.addEventListener('animationend', (e) => { if (e.target === host) host.classList.remove('entering'); });
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) host.classList.remove('entering');   // no animation, no animationend (#260)
  view.replaceChildren(host);
  view.scrollTop = 0;
  view.focus({ preventScroll: true });
}

function drawWindowButtons() {
  const host = document.getElementById('winbtns');
  const win = getCurrentWindow();
  const button = (id, name, label, cls = '') => `<button class="win-btn ${cls}" id="${id}" aria-label="${label}" title="${label}">${icon(name)}</button>`;
  const quiet = (p) => p.catch((e) => console.warn(e));
  host.innerHTML = button('win-min', 'minus', 'Minimise') + button('win-max', 'square', 'Maximise') + button('win-close', 'x', 'Close', 'close');
  document.getElementById('win-min').addEventListener('click', () => quiet(win.minimize()));
  document.getElementById('win-max').addEventListener('click', () => quiet(win.toggleMaximize()));
  document.getElementById('win-close').addEventListener('click', () => quiet(win.close()));
  document.getElementById('titlebar').addEventListener('pointerdown', (e) => {
    if (e.pointerType !== 'touch' || e.target.closest('button')) return;
    e.preventDefault();
    quiet(win.startDragging());
  });
  const max = document.getElementById('win-max');
  const syncMax = () => quiet(win.isMaximized().then((on) => {
    max.innerHTML = icon(on ? 'copy' : 'square');
    max.title = on ? 'Restore' : 'Maximise';
    max.setAttribute('aria-label', max.title);
  }));
  quiet(win.onResized(syncMax));
  syncMax();
}

function start() {
  startGamepadNav();
  drawNav();
  drawWindowButtons();
  store.subscribe(drawChrome);
  store.subscribe((_s, why) => { if (why === 'news') syncChannel(); });
  syncChannel();
  drawChrome();
  store.start().catch((e) => console.error(e));
  const once = store.subscribe((s, why) => {
    if (why !== 'app') return;
    once();
    if (startPage) go(startPage);
    else go(isFirstRun(s) ? 'setup' : 'home', { firstRun: isFirstRun(s) });
  });
}

start();
