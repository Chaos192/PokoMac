// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
//! Keep my saves in iCloud Drive (macOS, opt-in, Settings > Saves): the active profile's save travels between the player's Macs by copy,
//! around each Play. The game never plays from iCloud Drive itself: iCloud can turn a file into a placeholder that is only downloaded when
//! it is read, and two Macs could write one save at the same time. So before the game starts, a newer copy in iCloud Drive is brought in
//! (the save here is backed up first, savebackup.rs), and after the game ends the save is copied up: into a temporary folder in iCloud
//! Drive, then renamed into place, so another Mac never takes half a save.
//!
//! In iCloud Drive: `PokoMac/saves/account/<key>/` and `account/<key>.meta`, the layout of the game's save folder (saveimport.rs), and
//! `PokoMac/saves/sync.json`: which Mac wrote each account's copy, when, with which launcher, and its hash (savebackup.rs digest, so a copy
//! iCloud has only partly downloaded is told from a whole one). The launcher config keeps `icloud_last_sync` and the hash of each
//! account's save at its last sync (`icloud_synced`), so both sides know whether they changed since. When both did, nothing is overwritten
//! silently: the newer one is used and the other is kept, as a backup here or in iCloud Drive's `PokoMac/conflicts/`.
//! Only account saves travel, as with Nintendo's own cloud saves: device and cache saves and `coop-uid.txt` (this install's Link Play id,
//! runtime docs/CONFIG.md [net] uid) stay on each Mac.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const SYNC_FILE: &str = "sync.json";
/// How long the start of the game waits for iCloud Drive to download a save it holds only as a placeholder.
const DOWNLOAD_WAIT: Duration = Duration::from_secs(45);
/// The same for sync.json, a small file.
const READ_WAIT: Duration = Duration::from_secs(10);
/// Settings only shows what is there: it does not wait long.
const STATUS_WAIT: Duration = Duration::from_secs(3);
const CONFLICTS: &str = "conflicts";

/// iCloud Drive's folder, when this Mac has one (macOS, signed in to iCloud with iCloud Drive on).
pub fn cloud_docs() -> Option<PathBuf> {
    if !cfg!(target_os = "macos") {
        return None;
    }
    let home = std::env::var_os("HOME").map(PathBuf::from)?;
    let d = home.join("Library").join("Mobile Documents").join("com~apple~CloudDocs");
    d.is_dir().then_some(d)
}

/// `PokoMac/saves` inside iCloud Drive.
pub fn saves_dir(cloud: &Path) -> PathBuf {
    cloud.join("PokoMac").join("saves")
}

/// What to tell the player after a sync. `important`: worth a message of its own (a conflict, something not done), not only a word
/// in the game's status line.
#[derive(Debug, Clone, PartialEq)]
pub struct Note {
    pub text: String,
    pub important: bool,
}

fn note(text: impl Into<String>, important: bool) -> Option<Note> {
    Some(Note { text: text.into(), important })
}

// ---- the launcher config's keys -------------------------------------------------------------------------------------------------------
// Read and written as JSON values, not as main.rs Config: `--play` has no window (play.rs run_cli), and every other key must survive.

#[derive(Default)]
struct Prefs {
    on: bool,
    last_sync: String,
    synced: BTreeMap<String, String>,
}

fn read_json(path: &Path) -> serde_json::Value {
    fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(serde_json::Value::Null)
}

fn read_prefs(cfg: &Path) -> Prefs {
    let v = read_json(cfg);
    Prefs {
        on: v.get("icloud_sync").and_then(|x| x.as_bool()).unwrap_or(false),
        last_sync: v.get("icloud_last_sync").and_then(|x| x.as_str()).unwrap_or_default().to_string(),
        synced: v.get("icloud_synced").and_then(|x| serde_json::from_value(x.clone()).ok()).unwrap_or_default(),
    }
}

/// True when the player switched the sync on and this Mac has iCloud Drive: Play then says it is checking iCloud Drive.
pub fn is_on(cfg: &Path) -> bool {
    read_prefs(cfg).on && cloud_docs().is_some()
}

/// Remembers that account `key` was in sync at `hash` (both sides held it) now.
fn remember(cfg: &Path, key: &str, hash: &str) -> Result<(), String> {
    let mut v = read_json(cfg);
    if !v.is_object() {
        v = serde_json::json!({});
    }
    let obj = v.as_object_mut().ok_or("launcher.json")?;
    obj.insert("icloud_last_sync".into(), now_iso().into());
    let synced = obj.entry("icloud_synced").or_insert_with(|| serde_json::json!({}));
    if !synced.is_object() {
        *synced = serde_json::json!({});
    }
    if let Some(m) = synced.as_object_mut() {
        m.insert(key.into(), hash.into());
    }
    let text = serde_json::to_string_pretty(&v).map_err(|e| e.to_string())?;
    crate::settings::write_atomic(cfg, &text)
}

// ---- sync.json ------------------------------------------------------------------------------------------------------------------------

/// One account's copy in iCloud Drive.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
#[serde(default)]
pub struct CloudCopy {
    /// savebackup.rs digest of the folder and its .meta
    pub hash: String,
    pub files: u64,
    pub bytes: u64,
    /// the Mac that wrote it (its Computer Name)
    pub machine: String,
    /// UTC, ISO 8601, and the same in seconds since 1970
    pub time: String,
    pub unix: u64,
    pub launcher: String,
}

#[derive(Serialize, Deserialize, Debug, Default)]
#[serde(default)]
struct SyncFile {
    /// the last writer, for a person looking at the file
    machine: String,
    time: String,
    launcher: String,
    accounts: BTreeMap<String, CloudCopy>,
}

/// Runs `f` on its own thread and gives up waiting after `wait`. Reading a file iCloud Drive holds only as a placeholder makes it
/// download the file, which can take a while (and forever offline); the thread may finish later, it only reads or fills a staging folder.
fn with_limit<T: Send + 'static>(wait: Duration, f: impl FnOnce() -> T + Send + 'static) -> Option<T> {
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let _ = tx.send(f());
    });
    rx.recv_timeout(wait).ok()
}

/// sync.json, empty when there is none (or it cannot be parsed: the next upload writes it again). Err: iCloud did not hand it over in time.
fn read_sync(saves: &Path, wait: Duration) -> Result<SyncFile, String> {
    let path = saves.join(SYNC_FILE);
    if !path.exists() {
        return Ok(SyncFile::default());
    }
    match with_limit(wait, move || fs::read(&path)) {
        Some(Ok(bytes)) => Ok(serde_json::from_slice(&bytes).unwrap_or_default()),
        Some(Err(e)) => Err(format!("sync.json in iCloud Drive could not be read ({})", e)),
        None => Err("iCloud Drive did not download sync.json in time".into()),
    }
}

// ---- small helpers --------------------------------------------------------------------------------------------------------------------

/// This Mac's name as the player knows it (System Settings > General > Sharing), for sync.json and the messages on the other Mac.
pub fn machine_name() -> String {
    #[cfg(target_os = "macos")]
    if let Ok(o) = crate::host::command("/usr/sbin/scutil").args(["--get", "ComputerName"]).stderr(std::process::Stdio::null()).output() {
        let n = String::from_utf8_lossy(&o.stdout).trim().to_string();
        if !n.is_empty() {
            return n;
        }
    }
    std::env::var("COMPUTERNAME").or_else(|_| std::env::var("HOSTNAME")).unwrap_or_else(|_| "this computer".into())
}

fn now_iso() -> String {
    chrono::Utc::now().format("%Y-%m-%dT%H:%M:%SZ").to_string()
}

fn now_secs() -> u64 {
    secs(SystemTime::now())
}

fn secs(t: SystemTime) -> u64 {
    t.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

fn stamp() -> String {
    chrono::Utc::now().format("%Y%m%d-%H%M%S").to_string()
}

/// A machine name as part of a folder name.
fn file_word(s: &str) -> String {
    let w: String = s.chars().map(|c| if c.is_alphanumeric() || matches!(c, '-' | '_' | '.' | ' ') { c } else { '-' }).collect();
    let w = w.trim().trim_start_matches('.').to_string();
    if w.is_empty() { "another-Mac".into() } else { w.chars().take(60).collect() }
}

/// Staging folders an earlier start left behind (iCloud took too long and the copy finished later): removed after 15 minutes.
fn clean_staging(dir: &Path, prefix: &str) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let old = e.metadata().and_then(|m| m.modified()).ok().and_then(|m| m.elapsed().ok()).is_some_and(|d| d > Duration::from_secs(900));
        if old && e.file_name().to_string_lossy().starts_with(prefix) {
            let _ = fs::remove_dir_all(e.path());
        }
    }
}

/// Moves iCloud Drive's copy of `key` (folder and .meta) to `PokoMac/conflicts/<date-time>-<machine>/account/`, so it is never lost.
fn keep_conflict(saves: &Path, key: &str, from: &str) -> Result<PathBuf, String> {
    let root = saves.parent().unwrap_or(saves).join(CONFLICTS);
    let base = format!("{}-{}", stamp(), file_word(from));
    let target = (1..100).map(|i| root.join(if i == 1 { base.clone() } else { format!("{}-{}", base, i) })).find(|p| !p.exists()).ok_or("too many kept copies")?;
    let acc = target.join("account");
    fs::create_dir_all(&acc).map_err(|e| e.to_string())?;
    let from_acc = saves.join("account");
    fs::rename(from_acc.join(key), acc.join(key)).map_err(|e| format!("the copy in iCloud Drive could not be moved aside ({})", e))?;
    let meta = from_acc.join(format!("{}.meta", key));
    if meta.exists() {
        let _ = fs::rename(&meta, acc.join(format!("{}.meta", key)));
    }
    Ok(target)
}

// ---- deciding ---------------------------------------------------------------------------------------------------------------------------

#[derive(Debug, PartialEq)]
enum Pull {
    /// the save here is the one to play (iCloud's did not change since the last sync, or it is the same save)
    Nothing,
    /// iCloud's copy replaces the one here; `conflict`: both had changed and iCloud's is the newer
    Bring { conflict: bool },
    /// both had changed and the one here is the newer: iCloud's copy is kept aside
    KeepLocal,
}

/// `local` and `cloud`: the hashes of the two saves (`local` None: there is no save here yet); `last`: the hash both had at the last sync;
/// the times: when each last changed, in seconds. A tie keeps the save here.
fn decide(local: Option<&str>, cloud: &str, last: Option<&str>, local_time: u64, cloud_time: u64) -> Pull {
    let Some(local) = local else { return Pull::Bring { conflict: false } };
    if local == cloud || last == Some(cloud) {
        return Pull::Nothing;
    }
    if last == Some(local) {
        return Pull::Bring { conflict: false };
    }
    if cloud_time > local_time {
        Pull::Bring { conflict: true }
    } else {
        Pull::KeepLocal
    }
}

// ---- before the game ------------------------------------------------------------------------------------------------------------------

/// Before the game starts (play.rs start and run_cli): brings in iCloud Drive's copy of the active profile's save when it is newer.
/// Does nothing unless the player switched the sync on. Never fails the start: a problem is a sentence for the player.
pub fn before_play(game_dir: &Path, cfg: &Path) -> Option<Note> {
    let prefs = read_prefs(cfg);
    if !prefs.on {
        return None;
    }
    let Some(cloud) = cloud_docs() else {
        return note("iCloud Drive is not available on this Mac (signed out, or iCloud Drive is off), so your save was not synced.", true);
    };
    pull(game_dir, cfg, &saves_dir(&cloud), &prefs, DOWNLOAD_WAIT).unwrap_or_else(|e| note(format!("Your save was not checked against iCloud Drive: {}.", e), true))
}

fn pull(game_dir: &Path, cfg: &Path, saves: &Path, prefs: &Prefs, wait: Duration) -> Result<Option<Note>, String> {
    let (sd, key) = crate::savebackup::locate(game_dir);
    let cloud_dir = saves.join("account").join(&key);
    let cloud_meta = saves.join("account").join(format!("{}.meta", key));
    if !cloud_dir.is_dir() {
        return Ok(None);   // nothing there yet: this save goes up when the game closes
    }
    let sync = read_sync(saves, READ_WAIT)?;
    let rec = sync.accounts.get(&key).cloned();
    let last = prefs.synced.get(&key).cloned();
    // iCloud's copy is the one of the last sync: nothing to read. Unless this Mac has no save (a new game folder, a save folder moved):
    // then it is brought in, or the game would start a new save and send that up over it.
    let here = sd.join("account").join(&key).is_dir();
    if here && rec.as_ref().is_some_and(|r| Some(&r.hash) == last.as_ref()) {
        return Ok(None);
    }
    let from = rec.as_ref().map(|r| r.machine.clone()).filter(|m| !m.trim().is_empty()).unwrap_or_else(|| "your other Mac".into());
    if crate::savebackup::commit_pending(&sd, &key) {
        return Ok(note(format!("The game was still saving when it last closed on this Mac, so the copy from {} in iCloud Drive was not brought in. It is kept in iCloud Drive.", from), true));
    }

    // iCloud's copy, into a staging folder beside the save (same disk, so it can be renamed into place); reading it downloads it
    let _ = fs::create_dir_all(&sd);
    clean_staging(&sd, ".icloud-");
    let stage = sd.join(format!(".icloud-{}", stamp()));
    let _ = fs::remove_dir_all(&stage);
    let (src, src_meta, dst) = (cloud_dir.clone(), cloud_meta.clone(), stage.clone());
    let copied = with_limit(wait, move || -> Result<String, String> {
        crate::savebackup::copy_tree(&src, &dst.join("data")).map_err(|e| e.to_string())?;
        if src_meta.is_file() {
            fs::copy(&src_meta, dst.join("meta")).map_err(|e| e.to_string())?;
        }
        crate::savebackup::digest(&dst.join("data"), Some(&dst.join("meta"))).map(|d| d.0).map_err(|e| e.to_string())
    });
    let still = || note(format!("iCloud Drive is still downloading your save from {0}, so the game started with the save on this Mac. Nothing is lost: when the game closes, the copy from {0} is kept in iCloud Drive > PokoMac > conflicts.", from), true);
    let cloud_hash = match copied {
        None => return Ok(still()),
        Some(Err(e)) => {
            let _ = fs::remove_dir_all(&stage);
            return Err(format!("the copy in iCloud Drive could not be read ({})", e));
        }
        Some(Ok(h)) => h,
    };
    // a copy whose files have not all arrived yet does not match what its Mac wrote down
    if rec.as_ref().is_some_and(|r| r.hash != cloud_hash) {
        let _ = fs::remove_dir_all(&stage);
        return Ok(still());
    }

    let acc = sd.join("account");
    let live = acc.join(&key);
    let meta = acc.join(format!("{}.meta", key));
    let local_hash = if live.is_dir() { Some(crate::savebackup::digest(&live, Some(&meta)).map_err(|e| e.to_string())?.0) } else { None };
    if local_hash.as_deref() == Some(cloud_hash.as_str()) {
        let _ = fs::remove_dir_all(&stage);
        remember(cfg, &key, &cloud_hash)?;
        return Ok(None);
    }
    let cloud_time = rec.as_ref().map(|r| r.unix).filter(|t| *t > 0).unwrap_or_else(|| secs(crate::saveimport::newest_mtime(&cloud_dir)));
    let local_time = secs(crate::saveimport::newest_mtime(&live));
    match decide(local_hash.as_deref(), &cloud_hash, last.as_deref(), local_time, cloud_time) {
        Pull::Nothing => {
            let _ = fs::remove_dir_all(&stage);
            Ok(None)
        }
        Pull::KeepLocal => {
            let _ = fs::remove_dir_all(&stage);
            keep_conflict(saves, &key, &from)?;
            Ok(note(format!("Your save changed on this Mac and on {} since the last sync. The newer one, on this Mac, is used. The one from {} is kept in iCloud Drive > PokoMac > conflicts.", from, from), true))
        }
        Pull::Bring { conflict } => {
            let had = local_hash.is_some();
            if had {
                if let Err(e) = crate::savebackup::snapshot(game_dir, crate::savebackup::KIND_ICLOUD) {
                    let _ = fs::remove_dir_all(&stage);
                    return Ok(note(format!("The newer save from {} was not brought in, because the save on this Mac could not be backed up first: {}", from, e), true));
                }
            }
            let put = put_in_place(&stage, &acc, &key);
            let _ = fs::remove_dir_all(&stage);
            put?;
            remember(cfg, &key, &cloud_hash)?;
            Ok(match (conflict, had) {
                (true, _) => note(format!("Your save changed on this Mac and on {0} since the last sync. The newer one, from {0}, is used. The save from this Mac is in Settings > Saves > Backups.", from), true),
                (false, true) => note(format!("Your newer save from {} was brought in from iCloud Drive. The save that was here is in Settings > Saves > Backups.", from), true),
                (false, false) => note(format!("Your save from {} was brought in from iCloud Drive.", from), true),
            })
        }
    }
}

/// Puts the staged copy (`data`, `meta`) in place as the account's save, as a restore does (savebackup.rs restore): the old folder is
/// renamed away first and put back when the new one cannot be renamed in, and leftovers of an old unfinished commit are removed so they
/// cannot be applied over it.
fn put_in_place(stage: &Path, acc: &Path, key: &str) -> Result<(), String> {
    fs::create_dir_all(acc).map_err(|e| e.to_string())?;
    let live = acc.join(key);
    let replaced = acc.join(format!("{}.replaced-{}", key, stamp()));
    let had = live.exists();
    if had {
        fs::rename(&live, &replaced).map_err(|e| format!("the save could not be replaced ({}); it was not changed", e))?;
    }
    if let Err(e) = fs::rename(stage.join("data"), &live) {
        if had {
            let _ = fs::rename(&replaced, &live);
        }
        return Err(format!("the copy from iCloud Drive could not be put in place ({}); your save was not changed", e));
    }
    if stage.join("meta").is_file() {
        let _ = fs::copy(stage.join("meta"), acc.join(format!("{}.meta", key)));
    }
    let _ = fs::remove_dir_all(&replaced);
    if let Ok(rd) = fs::read_dir(acc) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n == format!("{}.commit", key) || n.starts_with(&format!("{}.journal.", key)) || n.starts_with(&format!("{}.work.", key)) {
                let p = e.path();
                let _ = if p.is_dir() { fs::remove_dir_all(&p) } else { fs::remove_file(&p) };
            }
        }
    }
    Ok(())
}

// ---- after the game -------------------------------------------------------------------------------------------------------------------

/// After the game has ended (play.rs, next to the closing save backup): copies the active profile's save to iCloud Drive.
pub fn after_play(game_dir: &Path, cfg: &Path) -> Option<Note> {
    let prefs = read_prefs(cfg);
    if !prefs.on {
        return None;
    }
    let Some(cloud) = cloud_docs() else {
        return note("iCloud Drive is not available on this Mac (signed out, or iCloud Drive is off), so your save was not copied there.", true);
    };
    push(game_dir, cfg, &saves_dir(&cloud), &prefs, &machine_name())
        .unwrap_or_else(|e| note(format!("Your save was not copied to iCloud Drive: {}. It is safe on this Mac and goes up after the next game.", e), true))
}

fn push(game_dir: &Path, cfg: &Path, saves: &Path, prefs: &Prefs, machine: &str) -> Result<Option<Note>, String> {
    let (sd, key) = crate::savebackup::locate(game_dir);
    let live = sd.join("account").join(&key);
    let meta = sd.join("account").join(format!("{}.meta", key));
    if !live.is_dir() {
        return Ok(None);
    }
    if crate::savebackup::commit_pending(&sd, &key) {
        return Ok(note("The game closed while it was still saving, so your save was not copied to iCloud Drive this time. It goes up after the next game.", true));
    }
    let (hash, files, bytes) = crate::savebackup::digest(&live, Some(&meta)).map_err(|e| e.to_string())?;
    // not known in time: whatever is there is treated as unseen below, so it is kept
    let mut sync = read_sync(saves, READ_WAIT).unwrap_or_default();
    let rec = sync.accounts.get(&key).cloned();
    let cloud_acc = saves.join("account");
    let cloud_dir = cloud_acc.join(&key);
    if rec.as_ref().is_some_and(|r| r.hash == hash) && cloud_dir.is_dir() {
        remember(cfg, &key, &hash)?;
        return Ok(None);   // the same save is there already
    }
    fs::create_dir_all(&cloud_acc).map_err(|e| format!("cannot create {} ({})", crate::dump::fwd(&cloud_acc), e))?;
    clean_staging(saves, ".upload-");
    let tmp = saves.join(format!(".upload-{}", stamp()));
    let _ = fs::remove_dir_all(&tmp);
    let staged = (|| -> Result<(), String> {
        crate::savebackup::copy_tree(&live, &tmp.join("data")).map_err(|e| e.to_string())?;
        if meta.is_file() {
            fs::copy(&meta, tmp.join("meta")).map_err(|e| e.to_string())?;
        }
        let got = crate::savebackup::digest(&tmp.join("data"), Some(&tmp.join("meta"))).map_err(|e| e.to_string())?.0;
        if got != hash {
            return Err("the save changed while it was being copied".into());
        }
        Ok(())
    })();
    if let Err(e) = staged {
        let _ = fs::remove_dir_all(&tmp);
        return Err(e);
    }
    // the copy there: one this Mac has seen (the last sync) is replaced; any other is kept, never overwritten
    let seen = rec.as_ref().is_some_and(|r| prefs.synced.get(&key) == Some(&r.hash));
    let from = rec.as_ref().map(|r| r.machine.clone()).filter(|m| !m.trim().is_empty()).unwrap_or_else(|| "another Mac".into());
    let mut kept = false;
    if cloud_dir.exists() {
        if seen {
            if let Err(e) = fs::rename(&cloud_dir, tmp.join("old")) {
                let _ = fs::remove_dir_all(&tmp);
                return Err(format!("the old copy in iCloud Drive could not be moved ({})", e));
            }
        } else {
            if let Err(e) = keep_conflict(saves, &key, &from) {
                let _ = fs::remove_dir_all(&tmp);
                return Err(e);
            }
            kept = true;
        }
    }
    if let Err(e) = fs::rename(tmp.join("data"), &cloud_dir) {
        if tmp.join("old").exists() {
            let _ = fs::rename(tmp.join("old"), &cloud_dir);
        }
        let _ = fs::remove_dir_all(&tmp);
        return Err(format!("the copy could not be put in place in iCloud Drive ({})", e));
    }
    if tmp.join("meta").is_file() {
        let _ = fs::rename(tmp.join("meta"), cloud_acc.join(format!("{}.meta", key)));
    }
    let _ = fs::remove_dir_all(&tmp);
    let (time, unix, launcher) = (now_iso(), now_secs(), env!("CARGO_PKG_VERSION").to_string());
    sync.accounts.insert(key.clone(), CloudCopy { hash: hash.clone(), files, bytes, machine: machine.to_string(), time: time.clone(), unix, launcher: launcher.clone() });
    (sync.machine, sync.time, sync.launcher) = (machine.to_string(), time, launcher);
    let text = serde_json::to_string_pretty(&sync).map_err(|e| e.to_string())?;
    crate::settings::write_atomic(&saves.join(SYNC_FILE), &text)?;
    remember(cfg, &key, &hash)?;
    Ok(if kept {
        note(format!("Your save was copied to iCloud Drive. The copy that was there had changed on {} since this Mac last synced, so it is kept in iCloud Drive > PokoMac > conflicts.", from), true)
    } else {
        note("Your save was copied to iCloud Drive.", false)
    })
}

// ---- for Settings -----------------------------------------------------------------------------------------------------------------------

#[derive(Serialize, Debug, Default)]
pub struct Status {
    /// this Mac has iCloud Drive: the switch is offered
    pub available: bool,
    pub on: bool,
    /// UTC, ISO 8601; empty: never synced
    pub last_sync: String,
    /// who wrote the active profile's copy in iCloud Drive and when (empty: none there, or not known in time)
    pub cloud_machine: String,
    pub cloud_time: String,
    pub this_machine: String,
    /// copies kept in PokoMac/conflicts
    pub conflicts: usize,
}

pub fn status(game_dir: &Path, cfg: &Path) -> Status {
    let prefs = read_prefs(cfg);
    let Some(cloud) = cloud_docs() else { return Status { on: prefs.on, ..Default::default() } };
    let saves = saves_dir(&cloud);
    let rec = prefs.on.then(|| read_sync(&saves, STATUS_WAIT).ok()).flatten().and_then(|s| s.accounts.get(&crate::savebackup::locate(game_dir).1).cloned()).unwrap_or_default();
    let conflicts = fs::read_dir(cloud.join("PokoMac").join(CONFLICTS)).map(|rd| rd.flatten().filter(|e| e.path().is_dir()).count()).unwrap_or(0);
    Status { available: true, on: prefs.on, last_sync: prefs.last_sync, cloud_machine: rec.machine, cloud_time: rec.time, this_machine: machine_name(), conflicts }
}

/// PokoMac's folder in iCloud Drive, for Finder.
pub fn folder() -> Option<PathBuf> {
    cloud_docs().map(|c| c.join("PokoMac"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    const KEY: &str = "010000000000000070636552484e4341";

    fn tmp(tag: &str) -> PathBuf {
        static N: AtomicUsize = AtomicUsize::new(0);
        let d = std::env::temp_dir().join(format!("pkp-icloud-{}-{}-{}", tag, std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    /// A game folder whose active save is `slot` (None: no save yet), and its launcher.json with the sync on.
    fn mac(tag: &str, slot: Option<&[u8]>) -> (PathBuf, PathBuf) {
        let d = tmp(tag);
        fs::write(d.join(crate::settings::PKP_INI), "[account]\nuid = 000000000000000141434e4852656370\n").unwrap();
        if let Some(s) = slot {
            write_save(&d, s);
        }
        let cfg = d.join("launcher.json");
        fs::write(&cfg, r#"{ "channel": "stable", "icloud_sync": true }"#).unwrap();
        (d, cfg)
    }

    fn write_save(d: &Path, slot: &[u8]) {
        let live = d.join("save").join("account").join(KEY);
        fs::create_dir_all(live.join("S00")).unwrap();
        fs::write(live.join("S00").join("slot"), slot).unwrap();
        fs::write(d.join("save").join("account").join(format!("{}.meta", KEY)), "size=4194304\nattr=00\n").unwrap();
    }

    fn slot(d: &Path) -> Vec<u8> {
        fs::read(d.join("save").join("account").join(KEY).join("S00").join("slot")).unwrap()
    }

    fn cloud_slot(saves: &Path) -> Vec<u8> {
        fs::read(saves.join("account").join(KEY).join("S00").join("slot")).unwrap()
    }

    fn up(d: &Path, cfg: &Path, saves: &Path, name: &str) -> Option<Note> {
        push(d, cfg, saves, &read_prefs(cfg), name).unwrap()
    }

    fn down(d: &Path, cfg: &Path, saves: &Path) -> Option<Note> {
        pull(d, cfg, saves, &read_prefs(cfg), Duration::from_secs(10)).unwrap()
    }

    #[test]
    fn the_rule_for_two_saves() {
        assert_eq!(decide(None, "c", None, 0, 0), Pull::Bring { conflict: false });
        assert_eq!(decide(Some("x"), "x", None, 0, 0), Pull::Nothing);
        assert_eq!(decide(Some("l"), "c", Some("c"), 0, 9), Pull::Nothing, "iCloud's did not change: this one is newer");
        assert_eq!(decide(Some("l"), "c", Some("l"), 9, 0), Pull::Bring { conflict: false }, "only iCloud's changed");
        assert_eq!(decide(Some("l"), "c", Some("o"), 5, 9), Pull::Bring { conflict: true });
        assert_eq!(decide(Some("l"), "c", Some("o"), 9, 5), Pull::KeepLocal);
        assert_eq!(decide(Some("l"), "c", None, 5, 5), Pull::KeepLocal, "a tie keeps the save here");
    }

    #[test]
    fn a_save_travels_between_two_macs_and_nothing_is_lost() {
        let cloud = tmp("cloud");
        let saves = saves_dir(&cloud);
        let (a, cfg_a) = mac("a", Some(b"one"));
        let (b, cfg_b) = mac("b", None);

        // A plays first: its save goes up; the config remembers it
        assert_eq!(up(&a, &cfg_a, &saves, "Mac A").map(|n| n.important), Some(false));
        assert_eq!(cloud_slot(&saves), b"one");
        let sync: SyncFile = serde_json::from_str(&fs::read_to_string(saves.join(SYNC_FILE)).unwrap()).unwrap();
        assert_eq!(sync.accounts[KEY].machine, "Mac A");
        assert!(saves.join("account").join(format!("{}.meta", KEY)).is_file());
        assert!(read_prefs(&cfg_a).synced.contains_key(KEY) && !read_prefs(&cfg_a).last_sync.is_empty());
        assert_eq!(read_json(&cfg_a)["channel"], "stable", "other keys of launcher.json stay");

        // B had no save: it takes A's, then plays and sends its own up
        assert!(down(&b, &cfg_b, &saves).unwrap().text.contains("Mac A"));
        assert_eq!(slot(&b), b"one");
        assert_eq!(down(&b, &cfg_b, &saves), None, "in sync: nothing to do");
        write_save(&b, b"two");
        up(&b, &cfg_b, &saves, "Mac B");
        assert_eq!(cloud_slot(&saves), b"two");

        // A did not play since: B's newer save comes in, A's is backed up first
        assert!(down(&a, &cfg_a, &saves).unwrap().text.contains("Backups"));
        assert_eq!(slot(&a), b"two");
        assert!(crate::savebackup::list(&a).iter().any(|e| e.kind == crate::savebackup::KIND_ICLOUD));
        assert!(!fs::read_dir(a.join("save")).unwrap().flatten().any(|e| e.file_name().to_string_lossy().starts_with(".icloud-")), "no staging folder is left");

        // both play without syncing in between: A's is newer, so it plays and B's copy is kept in conflicts
        write_save(&a, b"three");
        write_save(&b, b"four");
        up(&b, &cfg_b, &saves, "Mac B");
        let mut sync: SyncFile = serde_json::from_str(&fs::read_to_string(saves.join(SYNC_FILE)).unwrap()).unwrap();
        sync.accounts.get_mut(KEY).unwrap().unix = 1;   // B's went up long ago
        fs::write(saves.join(SYNC_FILE), serde_json::to_string(&sync).unwrap()).unwrap();
        assert!(down(&a, &cfg_a, &saves).unwrap().text.contains("conflicts"));
        assert_eq!(slot(&a), b"three");
        let kept = fs::read_dir(cloud.join("PokoMac").join(CONFLICTS)).unwrap().flatten().next().unwrap().path();
        assert_eq!(fs::read(kept.join("account").join(KEY).join("S00").join("slot")).unwrap(), b"four");
        up(&a, &cfg_a, &saves, "Mac A");
        assert_eq!(cloud_slot(&saves), b"three");

        // B plays on its old save without bringing A's in: the upload keeps A's copy instead of overwriting it
        write_save(&b, b"five");
        let n = up(&b, &cfg_b, &saves, "Mac B").unwrap();
        assert!(n.important && n.text.contains("conflicts"), "{:?}", n);
        assert_eq!(cloud_slot(&saves), b"five");
        assert_eq!(fs::read_dir(cloud.join("PokoMac").join(CONFLICTS)).unwrap().count(), 2);

        for d in [cloud, a, b] {
            let _ = fs::remove_dir_all(d);
        }
    }

    #[test]
    fn a_mac_that_lost_its_save_gets_it_back_instead_of_overwriting_it() {
        let cloud = tmp("lost");
        let saves = saves_dir(&cloud);
        let (a, cfg_a) = mac("la", Some(b"one"));
        up(&a, &cfg_a, &saves, "Mac A");
        fs::remove_dir_all(a.join("save")).unwrap();   // a new game folder, the same launcher config
        assert!(down(&a, &cfg_a, &saves).is_some());
        assert_eq!(slot(&a), b"one");
        for d in [cloud, a] {
            let _ = fs::remove_dir_all(d);
        }
    }

    #[test]
    fn a_copy_that_has_not_fully_arrived_is_not_taken() {
        let cloud = tmp("partial");
        let saves = saves_dir(&cloud);
        let (a, cfg_a) = mac("pa", Some(b"one"));
        let (b, cfg_b) = mac("pb", Some(b"mine"));
        up(&a, &cfg_a, &saves, "Mac A");
        fs::write(saves.join("account").join(KEY).join("S00").join("slot"), b"half").unwrap();   // sync.json says otherwise
        assert!(down(&b, &cfg_b, &saves).unwrap().text.contains("still downloading"));
        assert_eq!(slot(&b), b"mine");
        for d in [cloud, a, b] {
            let _ = fs::remove_dir_all(d);
        }
    }

    #[test]
    fn off_means_nothing_happens() {
        let (a, cfg) = mac("off", Some(b"one"));
        fs::write(&cfg, "{}").unwrap();
        assert_eq!(before_play(&a, &cfg), None);
        assert_eq!(after_play(&a, &cfg), None);
        let _ = fs::remove_dir_all(a);
    }
}
