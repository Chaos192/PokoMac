// Save backups: the question and the actions behind Settings > Saves > Backups (savebackup.rs). The list of copies and the restore are
// shown here; the on/off setting and the interval are plain pkp.ini keys ([backup] interval_minutes, keep) drawn by the settings schema.
import { api, confirm, errText, esc, toast } from '../ui.js';

const KIND = { auto: 'Automatic', exit: 'When the game closed', manual: 'Back up now', 'before-restore': 'Before a restore', 'before-icloud': 'Before a newer save came from iCloud Drive' };

export const size = (b) => (b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`);
export const when = (iso) => {
  const d = new Date(iso ?? '');
  return Number.isNaN(+d) ? String(iso ?? '') : d.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
};

/** The short value on the Backups row: how many copies there are. */
export function backupText(status) {
  if (!status) return 'Checking';
  return status.count ? `${status.count} backup${status.count === 1 ? '' : 's'}` : 'None yet';
}

/** The line under it: how big they are together and when the newest was made ('' when there is none). */
export function backupDetail(status) {
  return status?.count ? `Together ${size(status.bytes)}. The newest is from ${when(status.last?.created)}.` : '';
}

/** Radio list of the copies, newest first selected. Resolves the chosen folder name, or null (Cancel, Esc, click outside). */
function pick(list) {
  return new Promise((resolve) => {
    const dlg = document.createElement('dialog');
    dlg.className = 'modal glass strong';
    dlg.setAttribute('aria-labelledby', 'modal-title');
    dlg.innerHTML = `
      <form method="dialog">
        <div class="modal-body">
          <h2 id="modal-title">Restore a backup</h2>
          <p>Pick the moment to go back to. Your current save is copied first, so you can undo this by restoring the copy called "Before a restore".</p>
          <ul class="backup-list">
            ${list.map((e, i) => `<li><label class="backup-choice">
              <input type="radio" name="backup" value="${esc(e.name)}" ${i === 0 ? 'checked' : ''} />
              <span><strong>${esc(when(e.created))}</strong>
                <span class="backup-note">${esc(KIND[e.kind] ?? e.kind)}, ${esc(size(e.bytes))}${e.profile ? `, ${esc(e.profile)}` : ''}</span></span>
            </label></li>`).join('')}
          </ul>
        </div>
        <div class="modal-actions">
          <button class="btn quiet" type="button" data-answer="">Cancel</button>
          <button class="btn primary" type="submit" data-answer="ok">Restore</button>
        </div>
      </form>`;
    document.body.appendChild(dlg);
    dlg.querySelector('form').addEventListener('submit', (e) => {
      e.preventDefault();
      dlg.close('ok');
    });
    dlg.addEventListener('close', () => {
      const chosen = dlg.returnValue === 'ok' ? dlg.querySelector('input[name="backup"]:checked')?.value ?? null : null;
      dlg.remove();
      resolve(chosen);
    });
    dlg.addEventListener('click', (e) => {
      if (e.target.closest('[type="button"][data-answer]')) dlg.close('');
      else if (e.target === dlg) dlg.close('');
    });
    dlg.showModal();
  });
}

/** Back up now. Resolves true when something was copied. */
export async function backupNow() {
  try {
    const r = await api('backup_now');
    if (r.result === 'made') toast('Backup made', 'ok');
    else if (r.result === 'unchanged') toast('Nothing has changed since the last backup', 'ok');
    else toast('There is no save to back up yet', 'ok');
    return r.result === 'made';
  } catch (e) {
    toast(errText(e), 'error');
    return false;
  }
}

/** The whole restore flow: list, pick, one more question, restore. Resolves true when the save was put back. */
export async function runRestore() {
  let list;
  try {
    list = await api('backup_list');
  } catch (e) {
    toast(errText(e), 'error');
    return false;
  }
  if (!list.length) {
    toast('There are no backups yet', 'error');
    return false;
  }
  const name = await pick(list);
  if (!name) return false;
  const e = list.find((x) => x.name === name);
  const yes = await confirm({
    title: 'Restore this backup?',
    html: `<p>Your save goes back to <strong>${esc(when(e.created))}</strong>. Anything you did in the game after that moment is gone from the save, but it is copied first as "Before a restore", so you can come back to it.</p><p>The game must be closed.</p>`,
    ok: 'Restore',
    danger: true,
  });
  if (!yes) return false;
  try {
    await api('backup_restore', { name });
    toast('Save restored', 'ok');
    return true;
  } catch (err) {
    toast(errText(err), 'error');
    return false;
  }
}
