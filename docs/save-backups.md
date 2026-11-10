# Save backups

What the launcher's **Settings > Saves > Backups** does, how it is stored, and how another part of the project (the in-game F1 menu, another launcher page) can build on it without changing it. Nothing in the runtime was touched: it works on the save folder from outside.

## What it does
- While the game runs and the player switched it on, the launcher copies the active profile's save every 5, 10 or 15 minutes.
- When the game closes (any way: the launcher's Play, or a `--play` shortcut), one more copy is made if backups are on.
- **Back up now** makes a copy on request, any time.
- **Restore** puts a chosen copy back. The game must be closed. The current save is copied first (kind `before-restore`), so a restore can be undone by restoring that copy.
- With **Keep my saves in iCloud Drive** on (macOS, `launcher/src-tauri/src/icloud.rs`), the save is copied first (kind `before-icloud`) whenever a newer save from another Mac replaces it before the game starts. It counts with the automatic, exit and manual copies.
- Nothing runs unless the player chose a time. Updates never touch the copies (`save-backups` is in the update's keep list).

## Where
`<game folder>/save-backups/<account folder>/<yyyymmdd-hhmmss>-<kind>/`, one folder per copy, for the active profile's account folder (the folder name the runtime uses under `save/account/`).

Inside a copy:
- `data/`: a copy of `save/account/<account folder>/` (the committed save).
- `meta`: a copy of `save/account/<account folder>.meta`.
- `manifest.json`: `{ version: 1, name, created (UTC, ISO 8601), kind, key (account folder), profile (name at the time, a label only), files, bytes, hash }`. `kind` is `auto`, `exit`, `manual`, `before-restore` or `before-icloud`. `hash` is a SHA-256 over the file paths and contents, so two copies of the same save have the same hash (an unchanged save is not copied again).

A folder without a readable `manifest.json` is not a copy. Half-made copies are `.tmp-*` folders and are removed.

## Settings (`pkp.ini`, section `[backup]`)
| key | default | meaning |
|---|---|---|
| `interval_minutes` | `0` | `0` is off. The launcher offers 5, 10, 15; any whole number up to 240 works; anything else counts as off. |
| `keep` | `10` | how many `auto`, `exit` and `manual` copies stay (1 to 50). Three `before-restore` copies are kept apart. |
| `on_exit` | `1` | one more copy when the game closes (only while `interval_minutes` is not 0). |

The launcher reads these every few seconds while the game runs, so any other writer of `pkp.ini` (a future F1 page) takes effect at once. The keys are documented in `runtime/docs/CONFIG.md` as launcher-only.

## Why copying a live save is safe
The runtime keeps the committed save in `account/<key>/` and only changes it in the second phase of its commit (`runtime/src/services/fs_volume.h`). A copy is accepted only when no commit marker (`<key>.commit`, `<key>.journal.<pid>`) exists before or after it and the folder listing (path, size, time) is the same before and after; otherwise it is discarded and tried again a second later (three tries, then skipped until the next tick). During a restore the game is closed, and leftovers of a killed game (`.commit`, `.journal.*`, `.work.*`) are removed so an old unfinished commit cannot be applied over the restored save.

## Building on it (nothing here is built yet)
- **In-game F1 page:** read the `[backup]` keys and the `manifest.json` files to show the state and the list; write the keys to change the interval. No change to this module is needed.
- **Restore from the game:** the game cannot be restored while it runs. A page could write a request (for example `save-backups/restore.request` holding the folder name) and the launcher could apply it the next time it starts the game; that protocol is open and not implemented.
- **Another launcher page:** `savebackup.rs` exposes `status`, `list`, `snapshot` and `restore` and the commands `backup_status`, `backup_list`, `backup_now`, `backup_restore`, `open_backup_dir`.
- **A runtime-side copier** could replace the timer later: it only has to write the same layout and manifest.

## Not touched on purpose
The runtime (`runtime/`), the F1 pages, `controls.ini`, profiles and how a profile picks its save. The launcher's save import keeps its own `save.backup-<date>` folders.
