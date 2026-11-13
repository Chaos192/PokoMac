// The UI kit. Controls return HTML strings of real inputs, restyled in components.css; pages listen with ordinary
// `change` / `input` events (delegate on the page root). Inside row() they are named by the row's label.
import { invoke } from '@tauri-apps/api/core';
import {
  ArrowDown, ArrowLeft, ArrowRight, ArrowUp, ArrowUpRight, Check, ChevronDown, ChevronRight, CircleAlert, CircleCheck, CircleX, Clipboard, Copy,
  Download, ExternalLink, FileArchive, FileText, Folder, FolderOpen, FolderSearch, Gamepad2, Globe, HardDrive, House, Info,
  Keyboard, Link, LoaderCircle, Maximize2, Minus, Monitor, Mouse, Newspaper, Package, Palette, Pause, Pencil, Play, Plus, Puzzle, RefreshCw, RotateCcw, Save, Search,
  Server, Settings, Shield, ShieldCheck, Sparkles, Square, Trash2, TriangleAlert, Upload, UserCheck, Users, Volume2, Wifi, WifiOff, Wrench, X,
} from 'lucide';

const ICONS = {
  ArrowDown, ArrowLeft, ArrowRight, ArrowUp, ArrowUpRight, Check, ChevronDown, ChevronRight, CircleAlert, CircleCheck, CircleX, Clipboard, Copy,
  Download, ExternalLink, FileArchive, FileText, Folder, FolderOpen, FolderSearch, Gamepad2, Globe, HardDrive, House, Info,
  Keyboard, Link, LoaderCircle, Maximize2, Minus, Monitor, Mouse, Newspaper, Package, Palette, Pause, Pencil, Play, Plus, Puzzle, RefreshCw, RotateCcw, Save, Search,
  Server, Settings, Shield, ShieldCheck, Sparkles, Square, Trash2, TriangleAlert, Upload, UserCheck, Users, Volume2, Wifi, WifiOff, Wrench, X,
};

/** Calls a Rust command. */
export const api = (cmd, args) => invoke(cmd, args);

export const errText = (e) => (typeof e === 'string' ? e : e?.message ?? String(e));

export function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

const pascal = (name) => name.split('-').map((w) => w[0].toUpperCase() + w.slice(1)).join('');

/** Finished SVG markup for a Lucide icon by kebab name ("folder-open"). A new name is added to the import list above. */
export function icon(name, cls = '') {
  const node = ICONS[pascal(name)];
  if (!node) {
    if (import.meta.env?.DEV) console.warn(`icon "${name}" is not in ui.js`);
    return '';
  }
  const parts = node.map(([tag, attrs]) => `<${tag} ${Object.entries(attrs).map(([k, v]) => `${k}="${v}"`).join(' ')}/>`).join('');
  return `<svg class="icon i-${name} ${cls}" viewBox="0 0 24 24" aria-hidden="true">${parts}</svg>`;
}

const MAX_TOASTS = 3;

/** Short status message. kind: '' | 'ok' | 'error' (errors stay longer and are announced at once). ms: how long it stays, for a longer text. */
export function toast(message, kind = '', ms = 0) {
  const host = document.getElementById('toast');
  const el = document.createElement('div');
  el.className = `toast glass strong ${kind}`;
  el.setAttribute('role', kind === 'error' ? 'alert' : 'status');
  el.textContent = message;
  host.appendChild(el);
  while (host.children.length > MAX_TOASTS) host.firstElementChild.remove();
  setTimeout(() => el.remove(), ms || (kind === 'error' ? 7000 : 3500));
}

/**
 * A modal question on a native <dialog>. Resolves true for OK; Esc, Cancel or a click outside give false.
 * opts: { title, body (plain text), html (trusted markup, instead of body), ok = 'OK', cancel = 'Cancel', danger, alert (no Cancel) }
 */
export function confirm(opts = {}) {
  const { title = '', body = '', html = '', ok = 'OK', cancel = 'Cancel', danger = false, alert = false } = opts;
  return new Promise((resolve) => {
    const dlg = document.createElement('dialog');
    dlg.className = 'modal glass strong';
    dlg.setAttribute('aria-labelledby', 'modal-title');
    dlg.innerHTML = `
      <div class="modal-body">
        <h2 id="modal-title">${esc(title)}</h2>
        ${html || (body ? `<p>${esc(body)}</p>` : '')}
      </div>
      <div class="modal-actions">
        ${alert ? '' : `<button class="btn quiet" data-answer="">${esc(cancel)}</button>`}
        <button class="btn ${danger ? 'danger' : 'primary'}" data-answer="ok">${esc(ok)}</button>
      </div>`;
    document.body.appendChild(dlg);
    dlg.addEventListener('close', () => {
      dlg.remove();
      resolve(dlg.returnValue === 'ok');
    });
    dlg.addEventListener('click', (e) => {
      const answer = e.target.closest('[data-answer]')?.dataset.answer;
      if (answer !== undefined) dlg.close(answer);
      else if (e.target === dlg) dlg.close('');   // the dialog has no padding, so only the backdrop hits it
    });
    dlg.showModal();
    // Enter on a dangerous question must not be the dangerous answer
    dlg.querySelector(danger && !alert ? '[data-answer=""]' : '[data-answer="ok"]').focus();
  });
}

/**
 * A modal asking for one line of text. Resolves the trimmed text ('' when sent empty), or null for Esc, Cancel or a click outside.
 * opts: { title, body, label, value, ok = 'OK', max }
 */
export function askText(opts = {}) {
  const { title = '', body = '', label = '', value = '', ok = 'OK', max = 24 } = opts;
  return new Promise((resolve) => {
    const dlg = document.createElement('dialog');
    dlg.className = 'modal glass strong';
    dlg.setAttribute('aria-labelledby', 'modal-title');
    dlg.innerHTML = `
      <form method="dialog">
        <div class="modal-body">
          <h2 id="modal-title">${esc(title)}</h2>
          ${body ? `<p>${esc(body)}</p>` : ''}
          <input class="input" id="ask-text" type="text" maxlength="${max}" value="${esc(value)}" aria-label="${esc(label || title)}" autocomplete="off" spellcheck="false" />
        </div>
        <div class="modal-actions">
          <button class="btn quiet" type="button" data-answer="">Cancel</button>
          <button class="btn primary" type="submit" value="ok" data-answer="ok">${esc(ok)}</button>
        </div>
      </form>`;
    document.body.appendChild(dlg);
    const input = dlg.querySelector('#ask-text');
    // the form answers by itself (button, Enter): close with an explicit 'ok' rather than relying on the button's value
    dlg.querySelector('form').addEventListener('submit', (e) => {
      e.preventDefault();
      dlg.close('ok');
    });
    dlg.addEventListener('close', () => {
      const answer = dlg.returnValue === 'ok' ? input.value.trim() : null;
      dlg.remove();
      resolve(answer);
    });
    dlg.addEventListener('click', (e) => {
      if (e.target.closest('[type="button"][data-answer]')) dlg.close('');
      else if (e.target === dlg) dlg.close('');
    });
    dlg.showModal();
    input.focus();
    input.select();
  });
}

/** Options come as ['a', ...], [['value', 'Label'], ...] or [{ value, label, disabled }, ...]. */
const optionOf = (o) => (Array.isArray(o) ? { value: o[0], label: o[1] ?? o[0] } : typeof o === 'object' ? o : { value: o, label: o });
const named = (label) => (label ? `aria-label="${esc(label)}"` : 'aria-labelledby="@"');

let rowId = 0;

/** One row: label left, control right. The first input or select in `control` gets the label's `for`. */
export function row(label, control) {
  const id = `row-${++rowId}`;
  const target = /<(?:input|select)\b[^>]*?\bid="([^"]+)"/.exec(control)?.[1];
  return `<div class="row-item">
    <label class="row-label" id="${id}" ${target ? `for="${target}"` : ''}>${esc(label)}</label>
    <div class="row-control">${control.replaceAll('aria-labelledby="@"', `aria-labelledby="${id}"`)}</div>
  </div>`;
}

/** A titled glass block of rows; title may be empty. */
export function group(title, rows) {
  return `<section class="group">
    ${title ? `<h2 class="group-title">${esc(title)}</h2>` : ''}
    <div class="group-body glass">${rows.join('')}</div>
  </section>`;
}

/** Pill radio group: read it with a `change` listener, e.target.name / e.target.value. */
export function segmented(name, opts, value, { label = '', disabled = false } = {}) {
  const items = opts.map(optionOf).map((o) => `<label>
      <input type="radio" name="${esc(name)}" value="${esc(o.value)}" ${String(o.value) === String(value) ? 'checked' : ''} ${disabled || o.disabled ? 'disabled' : ''} />
      <span>${esc(o.label)}</span>
    </label>`);
  return `<div class="seg" role="radiogroup" ${named(label)}>${items.join('')}</div>`;
}

/** On/off switch: a checkbox with role=switch. */
export function toggle(id, on, { label = '', disabled = false } = {}) {
  return `<span class="toggle">
    <input type="checkbox" role="switch" id="${esc(id)}" ${on ? 'checked' : ''} ${disabled ? 'disabled' : ''} ${label ? named(label) : ''} />
    <span class="track"></span>
  </span>`;
}

/** Native select. A value that is not in `opts` is added so it is shown and never overwritten by accident. */
export function select(id, opts, value, { label = '', disabled = false } = {}) {
  const list = opts.map(optionOf);
  if (value != null && value !== '' && !list.some((o) => String(o.value) === String(value))) list.push({ value, label: String(value) });
  const items = list.map((o) => `<option value="${esc(o.value)}" ${String(o.value) === String(value) ? 'selected' : ''} ${o.disabled ? 'disabled' : ''}>${esc(o.label)}</option>`);
  return `<span class="select">
    <select id="${esc(id)}" ${disabled ? 'disabled' : ''} ${label ? named(label) : ''}>${items.join('')}</select>
    ${icon('chevron-down')}
  </span>`;
}

const fill = (v, min, max) => `${max > min ? Math.min(100, Math.max(0, (100 * (v - min)) / (max - min))) : 0}%`;

/** Range with a filled track and its value beside it. opts: step, suffix (shown after the value), label, disabled. */
export function slider(id, min, max, value, { step = 1, suffix = '', label = '', disabled = false } = {}) {
  return `<span class="slider">
    <input type="range" id="${esc(id)}" min="${min}" max="${max}" step="${step}" value="${esc(value)}" data-suffix="${esc(suffix)}" style="--pct:${fill(+value, min, max)}" ${disabled ? 'disabled' : ''} ${label ? named(label) : ''} />
    <output for="${esc(id)}">${esc(value)}${esc(suffix)}</output>
  </span>`;
}

/** Text input. opts: type, mono, label, disabled. */
export function field(id, value, placeholder = '', { type = 'text', mono = false, label = '', disabled = false } = {}) {
  return `<input class="input ${mono ? 'mono' : ''}" type="${esc(type)}" id="${esc(id)}" value="${esc(value)}" placeholder="${esc(placeholder)}" spellcheck="false" autocomplete="off" ${disabled ? 'disabled' : ''} ${label ? named(label) : ''} />`;
}

// every slider on every page keeps its fill and its number, so a page only listens for the value
document.addEventListener('input', (e) => {
  const el = e.target;
  if (!el.matches?.('.slider input')) return;
  el.style.setProperty('--pct', fill(+el.value, +el.min, +el.max));
  el.parentElement.querySelector('output').textContent = el.value + el.dataset.suffix;
});
