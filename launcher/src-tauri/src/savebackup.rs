// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
//! Save backups: copies of the active profile's save, made on a timer while the game runs (when the player chose it), once when the game
//! ends, and on request, so a progression-stopping bug can be rolled back. Nothing here touches the runtime: it works on the committed
//! save folder from outside, and the whole contract (folder layout, manifest, pkp.ini keys, how a restore is requested) is written down in
//! docs/save-backups.md so the in-game F1 menu or other launcher pages can build on it without changing this module.
//!
//! Safe to copy while the game runs: the runtime keeps the COMMITTED state in `account/<key>/` and never writes there outside its
//! two-phase commit (runtime/src/services/fs_volume.h). A copy is accepted only when no commit marker (`<key>.commit`,
//! `<key>.journal.<pid>`) was present before or after it and the folder listing (path, size, time) did not change meanwhile; otherwise it
//! is thrown away and tried again a moment later. A backup that cannot be made is skipped quietly (the next tick tries again).

use crate::ini::Ini;
use crate::settings::PKP_INI;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// Next to the save folder's owner, in the game folder: `save-backups/<account key>/<date-time>-<kind>/`.
pub const DIR: &str = "save-backups";
const MANIFEST: &str = "manifest.json";
const DATA: &str = "data";
const META: &str = "meta";
const VERSION: u32 = 1;
const DEFAULT_KEEP: usize = 10;
const MAX_KEEP: usize = 50;
const MAX_MINUTES: u32 = 240;
/// Safety copies made just before a restore are kept apart from the others, this many of them.
const KEEP_SAFETY: usize = 3;

pub const KIND_AUTO: &str = "auto";
pub const KIND_EXIT: &str = "exit";
pub const KIND_MANUAL: &str = "manual";
pub const KIND_SAFETY: &str = "before-restore";
/// The copy made before iCloud Drive's newer save replaces this one (icloud.rs).
pub const KIND_ICLOUD: &str = "before-icloud";

/// pkp.ini `[backup]`: `interval_minutes` (0 = off, the default; the launcher offers 5, 10 and 15), `keep` (how many automatic, exit and
/// manual copies stay, default 10), `on_exit` (one more copy when the game ends, default on, only while backups are on).
#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub struct Settings {
    pub minutes: u32,
    pub keep: usize,
    pub on_exit: bool,
}

pub fn settings_of(game_dir: &Path) -> Settings {
    let ini = fs::read(game_dir.join(PKP_INI)).ok().map(|b| Ini::parse(&String::from_utf8_lossy(&b)));
    let get = |k: &str| ini.as_ref().and_then(|i| i.get("backup", k)).unwrap_or_default();
    let minutes = get("interval_minutes").trim().parse::<u32>().ok().filter(|m| *m <= MAX_MINUTES).unwrap_or(0);
    let keep = get("keep").trim().parse::<usize>().ok().filter(|k| *k >= 1).map_or(DEFAULT_KEEP, |k| k.min(MAX_KEEP));
    let on_exit = !matches!(get("on_exit").trim().to_ascii_lowercase().as_str(), "0" | "false" | "no" | "off");
    Settings { minutes, keep, on_exit }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Entry {
    pub version: u32,
    /// the folder name: `<yyyymmdd-hhmmss>-<kind>`
    pub name: String,
    /// UTC, `yyyy-mm-ddThh:mm:ssZ`
    pub created: String,
    pub kind: String,
    /// the account folder the save belongs to (the runtime's name for it)
    pub key: String,
    /// the profile's name when the copy was made (a label only)
    pub profile: String,
    pub files: u64,
    pub bytes: u64,
    /// SHA-256 over the file paths and contents: two copies of the same save have the same hash
    pub hash: String,
}

#[derive(Serialize, Debug, PartialEq)]
#[serde(tag = "result", rename_all = "snake_case")]
pub enum Outcome {
    /// a new copy was made
    Made { entry: Entry },
    /// the save has not changed since the newest copy
    Unchanged,
    /// there is no save yet
    NoSave,
}

#[derive(Serialize, Debug)]
pub struct Status {
    pub settings: Settings,
    pub count: usize,
    pub bytes: u64,
    pub last: Option<Entry>,
}

// ---- where ----------------------------------------------------------------------------------------------------------------------

/// The runtime's save folder and the account folder the game uses for the active profile, from pkp.ini (the same rule as Import).
pub fn locate(game_dir: &Path) -> (PathBuf, String) {
    let ini = fs::read(game_dir.join(PKP_INI)).ok().map(|b| Ini::parse(&String::from_utf8_lossy(&b)));
    let get = |s: &str, k: &str| ini.as_ref().and_then(|i| i.get(s, k)).unwrap_or_default();
    let env = std::env::var("NXRT_FS_SAVE_DIR").unwrap_or_default();   // overrides the ini in the runtime, so here too
    let sd = crate::saveimport::save_dir_of(game_dir, &if env.trim().is_empty() { get("fs", "save_dir") } else { env });
    let key = crate::profiles::strict_save_key(game_dir).unwrap_or_else(|| crate::saveimport::target_key(&sd, &get("account", "uid")));
    (sd, key)
}

fn root_of(game_dir: &Path, key: &str) -> PathBuf {
    backup_root(game_dir).join(key)
}

/// Where the backups go: pkp.ini `[backup] dir` (Settings > Saves > Backup folder; relative to the game folder), else `save-backups` in
/// the game folder. Backups made before the player chose another folder stay where they were; Open folder shows the chosen one.
pub fn backup_root(game_dir: &Path) -> PathBuf {
    let ini = fs::read(game_dir.join(PKP_INI)).ok().map(|b| Ini::parse(&String::from_utf8_lossy(&b)));
    let set = ini.as_ref().and_then(|i| i.get("backup", "dir")).unwrap_or_default();
    let set = set.trim();
    if set.is_empty() {
        return game_dir.join(DIR);
    }
    let p = PathBuf::from(set);
    if p.is_absolute() { p } else { game_dir.join(p) }
}

fn profile_name(game_dir: &Path) -> String {
    crate::profiles::load(game_dir).active_profile().name.clone()
}

/// A plain folder name: no separators, no `..`, not hidden.
pub fn safe_name(name: &str) -> bool {
    !name.is_empty() && !name.starts_with('.') && !name.contains('/') && !name.contains('\\') && !name.contains('\0') && name != ".."
}

/// True while the runtime has a decided but unfinished commit of this account's save (copying then could see a half-applied commit).
pub(crate) fn commit_pending(sd: &Path, key: &str) -> bool {
    let acc = sd.join("account");
    let (a, b) = (format!("{}.commit", key), format!("{}.journal.", key));
    fs::read_dir(&acc).map(|rd| rd.flatten().any(|e| e.file_name().to_str().map_or(false, |n| n == a || n.starts_with(&b)))).unwrap_or(false)
}

// ---- reading and copying --------------------------------------------------------------------------------------------------------

type Listing = Vec<(String, u64, u128)>;

fn listing(root: &Path) -> io::Result<Listing> {
    fn walk(dir: &Path, base: &Path, out: &mut Listing) -> io::Result<()> {
        let mut entries: Vec<_> = fs::read_dir(dir)?.collect::<Result<_, _>>()?;
        entries.sort_by_key(|e| e.file_name());
        for e in entries {
            let p = e.path();
            let md = e.metadata()?;
            if md.is_dir() {
                walk(&p, base, out)?;
            } else {
                let t = md.modified().ok().and_then(|m| m.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_nanos());
                out.push((p.strip_prefix(base).unwrap_or(&p).to_string_lossy().replace('\\', "/"), md.len(), t));
            }
        }
        Ok(())
    }
    let mut out = Vec::new();
    walk(root, root, &mut out)?;
    Ok(out)
}

pub(crate) fn copy_tree(src: &Path, dst: &Path) -> io::Result<()> {
    fs::create_dir_all(dst)?;
    for e in fs::read_dir(src)? {
        let e = e?;
        let (from, to) = (e.path(), dst.join(e.file_name()));
        if e.metadata()?.is_dir() {
            copy_tree(&from, &to)?;
        } else {
            fs::copy(&from, &to)?;
        }
    }
    Ok(())
}

/// SHA-256 over every file (sorted by path) as path, length and bytes; the meta text is part of it. Also the totals.
pub(crate) fn digest(data: &Path, meta: Option<&Path>) -> io::Result<(String, u64, u64)> {
    let mut h = Sha256::new();
    let (mut files, mut bytes) = (0u64, 0u64);
    for (rel, len, _) in listing(data)? {
        h.update(rel.as_bytes());
        h.update([0u8]);
        h.update(len.to_le_bytes());
        h.update(fs::read(data.join(&rel))?);
        files += 1;
        bytes += len;
    }
    if let Some(m) = meta.filter(|m| m.is_file()) {
        let t = fs::read(m)?;
        h.update(b"meta\0");
        h.update(&t);
        bytes += t.len() as u64;
    }
    Ok((h.finalize().iter().map(|b| format!("{:02x}", b)).collect(), files, bytes))
}

fn stamp(root: &Path, kind: &str) -> String {
    let base = chrono::Utc::now().format("%Y%m%d-%H%M%S").to_string();
    let first = format!("{}-{}", base, kind);
    if !root.join(&first).exists() {
        return first;
    }
    (2..1000).map(|i| format!("{}-{}-{}", base, kind, i)).find(|n| !root.join(n).exists()).unwrap_or(first)
}

// ---- the list -------------------------------------------------------------------------------------------------------------------

/// The copies of one account's save, newest first. Folders without a readable manifest are not copies and are ignored.
pub fn list_key(game_dir: &Path, key: &str) -> Vec<Entry> {
    let mut out: Vec<Entry> = fs::read_dir(root_of(game_dir, key))
        .map(|rd| {
            rd.flatten()
                .filter(|e| e.path().is_dir())
                .filter_map(|e| fs::read_to_string(e.path().join(MANIFEST)).ok())
                .filter_map(|t| serde_json::from_str::<Entry>(&t).ok())
                .collect()
        })
        .unwrap_or_default();
    out.sort_by(|a, b| b.name.cmp(&a.name));
    out
}

pub fn list(game_dir: &Path) -> Vec<Entry> {
    list_key(game_dir, &locate(game_dir).1)
}

pub fn status(game_dir: &Path) -> Status {
    let l = list(game_dir);
    Status { settings: settings_of(game_dir), count: l.len(), bytes: l.iter().map(|e| e.bytes).sum(), last: l.first().cloned() }
}

/// Removes copies beyond `keep` (automatic, exit and manual together) and beyond KEEP_SAFETY safety copies; older half-made copies go too.
fn prune(game_dir: &Path, key: &str, keep: usize) {
    let root = root_of(game_dir, key);
    let all = list_key(game_dir, key);
    let (safety, normal): (Vec<&Entry>, Vec<&Entry>) = all.iter().partition(|e| e.kind == KIND_SAFETY);
    for e in normal.iter().skip(keep).chain(safety.iter().skip(KEEP_SAFETY)) {
        if safe_name(&e.name) {
            let _ = fs::remove_dir_all(root.join(&e.name));
        }
    }
    if let Ok(rd) = fs::read_dir(&root) {
        for e in rd.flatten() {
            let old = e.metadata().and_then(|m| m.modified()).ok().and_then(|m| m.elapsed().ok()).map_or(false, |d| d > Duration::from_secs(900));
            if old && e.file_name().to_str().map_or(false, |n| n.starts_with(".tmp-")) {
                let _ = fs::remove_dir_all(e.path());
            }
        }
    }
}

// ---- making a copy --------------------------------------------------------------------------------------------------------------

/// One copy of the active profile's save. `retry_wait`: how long to wait before trying again when the save is being written.
pub fn snapshot_with(game_dir: &Path, kind: &str, retry_wait: Duration) -> Result<Outcome, String> {
    snapshot_impl(game_dir, kind, retry_wait, false)
}

/// `game_closed`: nothing is writing the save, so a commit marker left by a game that was killed is not a reason to wait (a restore
/// still wants the copy of what is there now); the folder listing must still be the same before and after.
fn snapshot_impl(game_dir: &Path, kind: &str, retry_wait: Duration, game_closed: bool) -> Result<Outcome, String> {
    let (sd, key) = locate(game_dir);
    let live = sd.join("account").join(&key);
    if !live.is_dir() {
        return Ok(Outcome::NoSave);
    }
    let meta = sd.join("account").join(format!("{}.meta", key));
    let root = root_of(game_dir, &key);
    fs::create_dir_all(&root).map_err(|e| format!("Cannot create {}: {}", root.display(), e))?;
    for _ in 0..3 {
        if !game_closed && commit_pending(&sd, &key) {
            std::thread::sleep(retry_wait);
            continue;
        }
        let before = listing(&live).map_err(|e| e.to_string())?;
        let tmp = root.join(format!(".tmp-{}", stamp(&root, kind)));
        let _ = fs::remove_dir_all(&tmp);
        let made = (|| -> io::Result<()> {
            copy_tree(&live, &tmp.join(DATA))?;
            if meta.is_file() {
                fs::copy(&meta, tmp.join(META))?;
            }
            Ok(())
        })();
        let stable = made.is_ok() && (game_closed || !commit_pending(&sd, &key)) && listing(&live).map_or(false, |after| after == before);
        if !stable {
            let _ = fs::remove_dir_all(&tmp);
            std::thread::sleep(retry_wait);
            continue;
        }
        let (hash, files, bytes) = digest(&tmp.join(DATA), Some(&tmp.join(META))).map_err(|e| e.to_string())?;
        if kind != KIND_SAFETY && list_key(game_dir, &key).first().map_or(false, |e| e.hash == hash) {
            let _ = fs::remove_dir_all(&tmp);
            return Ok(Outcome::Unchanged);
        }
        let name = tmp.file_name().and_then(|n| n.to_str()).unwrap_or_default().trim_start_matches(".tmp-").to_string();
        let entry = Entry {
            version: VERSION,
            name: name.clone(),
            created: chrono::Utc::now().format("%Y-%m-%dT%H:%M:%SZ").to_string(),
            kind: kind.to_string(),
            key: key.clone(),
            profile: profile_name(game_dir),
            files,
            bytes,
            hash,
        };
        let text = serde_json::to_string_pretty(&entry).map_err(|e| e.to_string())?;
        crate::settings::write_atomic(&tmp.join(MANIFEST), &text)?;
        fs::rename(&tmp, root.join(&name)).map_err(|e| format!("Cannot finish the backup: {}", e))?;
        prune(game_dir, &key, settings_of(game_dir).keep);
        return Ok(Outcome::Made { entry });
    }
    Err("The save was being written. Nothing was copied; try again in a moment.".into())
}

pub fn snapshot(game_dir: &Path, kind: &str) -> Result<Outcome, String> {
    snapshot_with(game_dir, kind, Duration::from_secs(1))
}

// ---- restoring ------------------------------------------------------------------------------------------------------------------

#[derive(Serialize, Debug)]
pub struct Restored {
    pub restored: Entry,
    /// the copy of the save as it was just before the restore (so the restore can be undone), when there was a save
    pub safety: Option<Entry>,
}

/// Puts the copy `name` back as the active profile's save. The game must be closed (the caller checks). The save as it is now is copied
/// first (kind before-restore); the live folder is replaced by renaming, so a failure part way puts the old one back; leftovers of the
/// runtime's commit (`.commit`, `.journal.*`, `.work.*`) are removed so an old unfinished commit cannot be applied over the restore.
pub fn restore(game_dir: &Path, name: &str) -> Result<Restored, String> {
    if !safe_name(name) {
        return Err("That is not a backup name.".into());
    }
    let (sd, key) = locate(game_dir);
    let entry = list_key(game_dir, &key).into_iter().find(|e| e.name == name).ok_or("That backup does not exist (any more).")?;
    if entry.key != key {
        return Err("That backup belongs to another profile's save.".into());
    }
    let src = root_of(game_dir, &key).join(name);
    if !src.join(DATA).is_dir() {
        return Err("That backup is incomplete.".into());
    }
    let safety = match snapshot_impl(game_dir, KIND_SAFETY, Duration::from_millis(200), true).map_err(|e| format!("Your current progress could not be saved first, so nothing was changed: {}", e))? {
        Outcome::Made { entry } => Some(entry),
        _ => None,
    };
    let acc = sd.join("account");
    fs::create_dir_all(&acc).map_err(|e| e.to_string())?;
    let live = acc.join(&key);
    let st = chrono::Utc::now().format("%Y%m%d%H%M%S").to_string();
    let (incoming, replaced) = (acc.join(format!("{}.restore-{}", key, st)), acc.join(format!("{}.replaced-{}", key, st)));
    let _ = fs::remove_dir_all(&incoming);
    copy_tree(&src.join(DATA), &incoming).map_err(|e| {
        let _ = fs::remove_dir_all(&incoming);
        format!("Cannot copy the backup: {}. Your save was not changed.", e)
    })?;
    if listing(&incoming).map(|l| l.len() as u64).ok() != Some(entry.files) {
        let _ = fs::remove_dir_all(&incoming);
        return Err("The copy of the backup is not complete. Your save was not changed.".into());
    }
    let had = live.exists();
    if had {
        fs::rename(&live, &replaced).map_err(|e| {
            let _ = fs::remove_dir_all(&incoming);
            format!("Cannot replace the save: {}. Your save was not changed.", e)
        })?;
    }
    if let Err(e) = fs::rename(&incoming, &live) {
        if had {
            let _ = fs::rename(&replaced, &live);
        }
        let _ = fs::remove_dir_all(&incoming);
        return Err(format!("Cannot put the backup in place: {}. Your save was not changed.", e));
    }
    let meta_from = src.join(META);
    if meta_from.is_file() {
        let _ = fs::copy(&meta_from, acc.join(format!("{}.meta", key)));
    }
    let _ = fs::remove_dir_all(&replaced);
    if let Ok(rd) = fs::read_dir(&acc) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n == format!("{}.commit", key) || n.starts_with(&format!("{}.journal.", key)) || n.starts_with(&format!("{}.work.", key)) {
                let p = e.path();
                let _ = if p.is_dir() { fs::remove_dir_all(&p) } else { fs::remove_file(&p) };
            }
        }
    }
    Ok(Restored { restored: entry, safety })
}

// ---- while the game runs --------------------------------------------------------------------------------------------------------

/// The timer: started when the game starts, finished when it ends. It re-reads pkp.ini every few seconds, so a change made in the
/// launcher (or in a later in-game page) takes effect without restarting anything.
pub struct Watch {
    stop: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
    dir: PathBuf,
}

const TICK: Duration = Duration::from_secs(5);

impl Watch {
    pub fn start(game_dir: &Path) -> Watch {
        let stop = Arc::new(AtomicBool::new(false));
        let (flag, dir) = (stop.clone(), game_dir.to_path_buf());
        let handle = std::thread::spawn(move || run(&dir, &flag, TICK, Duration::from_secs(1), Duration::from_secs(60)));
        Watch { stop, handle: Some(handle), dir: game_dir.to_path_buf() }
    }

    /// The game has ended: stops the timer, then makes the closing copy (when backups are on). Call it after the process is gone.
    pub fn finish(mut self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
        let s = settings_of(&self.dir);
        if s.minutes > 0 && s.on_exit {
            if let Err(e) = snapshot(&self.dir, KIND_EXIT) {
                eprintln!("Save backup at exit: {}", e);
            }
        }
    }
}

/// Sleeps `total`, in short steps, and returns early when `stop` is set (so finish() does not wait out a whole tick).
fn sleep_unless(stop: &AtomicBool, total: Duration) {
    let step = Duration::from_millis(50).min(total);
    let start = Instant::now();
    while start.elapsed() < total && !stop.load(Ordering::SeqCst) {
        std::thread::sleep(step);
    }
}

/// The timer loop (a function so a test can run it with a short tick and a short "minute"). `retry_wait` is passed on to the copy.
fn run(dir: &Path, stop: &AtomicBool, tick: Duration, retry_wait: Duration, minute: Duration) {
    let mut last = Instant::now();
    while !stop.load(Ordering::SeqCst) {
        sleep_unless(stop, tick);
        if stop.load(Ordering::SeqCst) {
            break;
        }
        let s = settings_of(dir);
        if s.minutes == 0 {
            last = Instant::now();   // off: the clock starts when it is switched on
            continue;
        }
        if last.elapsed() >= minute * s.minutes {
            last = Instant::now();
            if let Err(e) = snapshot_with(dir, KIND_AUTO, retry_wait) {
                eprintln!("Save backup: {}", e);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    fn tmp() -> PathBuf {
        static N: AtomicUsize = AtomicUsize::new(0);
        let d = std::env::temp_dir().join(format!("pkp-backup-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    const KEY: &str = "010000000000000070636552484e4341";

    /// A game folder with a save in the runtime's layout (account/<key>/ + <key>.meta) and a pkp.ini.
    fn game(ini: &str) -> PathBuf {
        let d = tmp();
        fs::write(d.join(PKP_INI), format!("[account]\nuid = 000000000000000141434e4852656370\n\n{}", ini)).unwrap();   // the uid maps to KEY (saveimport::uid_key)
        let live = d.join("save").join("account").join(KEY);
        fs::create_dir_all(live.join("S00")).unwrap();
        fs::write(live.join("S00").join("slot"), b"slot one").unwrap();
        fs::write(live.join("G00"), b"global").unwrap();
        fs::write(d.join("save").join("account").join(format!("{}.meta", KEY)), "size=4194304\nattr=00\n").unwrap();
        d
    }

    fn live(d: &Path) -> PathBuf {
        d.join("save").join("account").join(KEY)
    }

    fn make(d: &Path, kind: &str) -> Entry {
        match snapshot_with(d, kind, Duration::from_millis(10)).unwrap() {
            Outcome::Made { entry } => entry,
            other => panic!("expected a new copy, got {:?}", other),
        }
    }

    #[test]
    fn the_settings_are_read_from_pkp_ini_and_garbage_means_off() {
        let d = tmp();
        assert_eq!(settings_of(&d), Settings { minutes: 0, keep: 10, on_exit: true }, "no file: off");
        for (ini, want) in [("[backup]\ninterval_minutes = 5\n", 5), ("[backup]\ninterval_minutes = 15\nkeep = 3\non_exit = off\n", 15), ("[backup]\ninterval_minutes = soon\n", 0), ("[backup]\ninterval_minutes = 999\n", 0), ("[backup]\ninterval_minutes = -5\n", 0)] {
            fs::write(d.join(PKP_INI), ini).unwrap();
            assert_eq!(settings_of(&d).minutes, want, "{}", ini);
        }
        fs::write(d.join(PKP_INI), "[backup]\ninterval_minutes = 10\nkeep = 3\non_exit = off\n").unwrap();
        assert_eq!(settings_of(&d), Settings { minutes: 10, keep: 3, on_exit: false });
        fs::write(d.join(PKP_INI), "[backup]\nkeep = 0\n").unwrap();
        assert_eq!(settings_of(&d).keep, 10, "keep 0 is not valid");
        fs::write(d.join(PKP_INI), "[backup]\nkeep = 9999\n").unwrap();
        assert_eq!(settings_of(&d).keep, 50);
    }

    #[test]
    fn a_copy_holds_the_save_and_its_meta_and_describes_itself() {
        let d = game("");
        let e = make(&d, KIND_MANUAL);
        assert_eq!((e.kind.as_str(), e.key.as_str(), e.files), (KIND_MANUAL, KEY, 2));
        let dir = d.join(DIR).join(KEY).join(&e.name);
        assert_eq!(fs::read(dir.join(DATA).join("S00").join("slot")).unwrap(), b"slot one");
        assert!(dir.join(META).is_file() && dir.join(MANIFEST).is_file());
        assert_eq!(list(&d), vec![e]);
        assert!(!fs::read_dir(d.join(DIR).join(KEY)).unwrap().flatten().any(|x| x.file_name().to_string_lossy().starts_with(".tmp-")), "no half-made folder is left");
    }

    #[test]
    fn an_unchanged_save_is_not_copied_again_and_a_changed_one_is() {
        let d = game("");
        make(&d, KIND_AUTO);
        assert_eq!(snapshot_with(&d, KIND_AUTO, Duration::from_millis(10)).unwrap(), Outcome::Unchanged);
        assert_eq!(list(&d).len(), 1);
        fs::write(live(&d).join("G00"), b"global changed").unwrap();
        make(&d, KIND_AUTO);
        assert_eq!(list(&d).len(), 2);
    }

    #[test]
    fn no_save_yet_is_not_an_error() {
        let d = tmp();
        fs::write(d.join(PKP_INI), "").unwrap();
        assert_eq!(snapshot(&d, KIND_AUTO).unwrap(), Outcome::NoSave);
        assert!(!d.join(DIR).exists());
    }

    #[test]
    fn a_pending_commit_stops_the_copy_and_nothing_is_left_behind() {
        let d = game("");
        let marker = d.join("save").join("account").join(format!("{}.commit", KEY));
        fs::write(&marker, b"decided").unwrap();
        let err = snapshot_with(&d, KIND_AUTO, Duration::from_millis(5)).unwrap_err();
        assert!(err.contains("being written"), "{}", err);
        assert!(list(&d).is_empty());
        fs::remove_file(&marker).unwrap();
        fs::write(d.join("save").join("account").join(format!("{}.journal.4242", KEY)), b"changes").unwrap();
        assert!(snapshot_with(&d, KIND_AUTO, Duration::from_millis(5)).is_err(), "a journal counts too");
        fs::remove_file(d.join("save").join("account").join(format!("{}.journal.4242", KEY))).unwrap();
        make(&d, KIND_AUTO);
    }

    #[test]
    fn only_the_newest_copies_stay() {
        let d = game("[backup]\nkeep = 3\n");
        for i in 0..6 {
            fs::write(live(&d).join("G00"), format!("global {}", i)).unwrap();
            make(&d, KIND_AUTO);
        }
        let l = list(&d);
        assert_eq!(l.len(), 3);
        assert!(l[0].name > l[1].name && l[1].name > l[2].name, "newest first");
        let on_disk = fs::read_dir(d.join(DIR).join(KEY)).unwrap().count();
        assert_eq!(on_disk, 3, "the pruned folders are gone");
    }

    #[test]
    fn a_restore_puts_the_old_save_back_after_saving_the_current_one_and_removes_stale_commit_files() {
        let d = game("");
        let old = make(&d, KIND_MANUAL);
        fs::write(live(&d).join("G00"), b"after the bug").unwrap();
        fs::write(live(&d).join("S00").join("new_slot"), b"new").unwrap();
        let acc = d.join("save").join("account");
        fs::write(acc.join(format!("{}.commit", KEY)), b"old marker").unwrap();
        fs::write(acc.join(format!("{}.journal.1", KEY)), b"old journal").unwrap();
        fs::create_dir_all(acc.join(format!("{}.work.1", KEY))).unwrap();
        let other = acc.join("other-account");
        fs::create_dir_all(&other).unwrap();
        fs::write(other.join("keep"), b"untouched").unwrap();
        let r = restore(&d, &old.name).unwrap();
        assert_eq!(fs::read(live(&d).join("G00")).unwrap(), b"global");
        assert!(!live(&d).join("S00").join("new_slot").exists(), "the file made after the backup is gone");
        let safety = r.safety.expect("the current save was copied first");
        assert_eq!(safety.kind, KIND_SAFETY);
        assert_eq!(fs::read(d.join(DIR).join(KEY).join(&safety.name).join(DATA).join("G00")).unwrap(), b"after the bug");
        for stale in [format!("{}.commit", KEY), format!("{}.journal.1", KEY), format!("{}.work.1", KEY)] {
            assert!(!acc.join(&stale).exists(), "{} must be removed", stale);
        }
        assert_eq!(fs::read(other.join("keep")).unwrap(), b"untouched", "other accounts are not touched");
        assert!(fs::read_dir(&acc).unwrap().flatten().all(|e| !e.file_name().to_string_lossy().contains(".replaced-") && !e.file_name().to_string_lossy().contains(".restore-")));
        // the restore itself can be undone with the safety copy
        restore(&d, &safety.name).unwrap();
        assert_eq!(fs::read(live(&d).join("G00")).unwrap(), b"after the bug");
    }

    #[test]
    fn a_restore_refuses_names_that_leave_the_folder_and_unknown_copies() {
        let d = game("");
        make(&d, KIND_AUTO);
        for bad in ["", "..", "../x", "a/b", "a\\b", ".tmp-1", "does-not-exist"] {
            assert!(restore(&d, bad).is_err(), "{:?}", bad);
        }
        assert_eq!(fs::read(live(&d).join("G00")).unwrap(), b"global", "a refused restore changes nothing");
    }

    #[test]
    fn a_copy_without_its_data_cannot_be_restored() {
        let d = game("");
        let e = make(&d, KIND_AUTO);
        fs::remove_dir_all(d.join(DIR).join(KEY).join(&e.name).join(DATA)).unwrap();
        assert!(restore(&d, &e.name).unwrap_err().contains("incomplete"));
        assert_eq!(fs::read(live(&d).join("G00")).unwrap(), b"global");
    }

    #[test]
    fn safety_copies_are_kept_apart_and_limited() {
        let d = game("[backup]\nkeep = 2\n");
        make(&d, KIND_MANUAL);
        for i in 0..5 {
            fs::write(live(&d).join("G00"), format!("change {}", i)).unwrap();
            make(&d, KIND_SAFETY);
        }
        let l = list(&d);
        assert_eq!(l.iter().filter(|e| e.kind == KIND_SAFETY).count(), KEEP_SAFETY);
        assert_eq!(l.iter().filter(|e| e.kind == KIND_MANUAL).count(), 1, "a safety copy never pushes a normal copy out");
    }

    #[test]
    fn the_timer_makes_a_copy_when_it_is_on_and_none_when_it_is_off() {
        let run_for = |d: &PathBuf, ms: u64| {
            let stop = AtomicBool::new(false);
            std::thread::scope(|s| {
                let h = s.spawn(|| run(d, &stop, Duration::from_millis(5), Duration::from_millis(5), Duration::from_millis(20)));
                std::thread::sleep(Duration::from_millis(ms));
                stop.store(true, Ordering::SeqCst);
                h.join().unwrap();
            });
        };
        let off = game("[backup]\ninterval_minutes = 0\n");
        run_for(&off, 300);
        assert!(list(&off).is_empty(), "off: no copy");
        let on = game("[backup]\ninterval_minutes = 5\n");   // 5 "minutes" of 20 ms: 100 ms
        run_for(&on, 600);
        let l = list(&on);
        assert_eq!(l.len(), 1, "on: one copy, and the second tick finds the save unchanged");
        assert_eq!(l[0].kind, KIND_AUTO);
        fs::write(live(&on).join("G00"), b"played on").unwrap();
        run_for(&on, 600);
        assert_eq!(list(&on).len(), 2, "a changed save is copied again");
        assert!(Duration::from_secs(5 * 60) > TICK, "the real tick is far below the shortest interval");
    }

    #[test]
    fn finish_makes_the_closing_copy_only_while_backups_are_on() {
        let on = game("[backup]\ninterval_minutes = 5\n");
        Watch::start(&on).finish();
        assert_eq!(list(&on).iter().map(|e| e.kind.as_str()).collect::<Vec<_>>(), vec![KIND_EXIT]);
        let off = game("");
        Watch::start(&off).finish();
        assert!(list(&off).is_empty(), "off means off: nothing is copied at exit either");
        let no_exit = game("[backup]\ninterval_minutes = 5\non_exit = 0\n");
        Watch::start(&no_exit).finish();
        assert!(list(&no_exit).is_empty());
    }
}
