// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch) (its cheats_fetch.rs, for PokoMac's one game).
//! The game's public cheat file, fetched on the player's machine: cheat files carry no licence to redistribute, so the release bundles
//! none; the launcher fetches the one for Pokopia's version and names its author. The work is the bundled `tools/build/cheats_fetch.py`
//! (stdlib Python, the same as AnySwitch's): by the program id and main's build id, from tomvita's MyNXCheats first, then
//! switch-cheats-db; only a plain Atmosphère cheat file, into the cheats folder as `<build id>.txt` with `toggles.txt` (every cheat off)
//! and `<build id>.fetched.json` (source, author, page, licence) that Settings > Cheats shows. A cheat file of the player's own is never
//! replaced.
//!
//! When: Settings > Cheats "Find cheats online"; turning "Enable cheats" on; and Play with cheats on whose version has no cheat file yet
//! (asked at most once a day). Only the program id and build id are sent, to GitHub. Settings > Cheats "Find cheat files online" turns
//! it off (launcher.json `online_cheats`); POKOMAC_ONLINE_CHEATS=0 does the same for a command-line run.

use serde::Serialize;
use serde_json::Value;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};
use tauri::AppHandle;

/// Pokopia's program id (pkp_main.cpp [process] program_id), as the cheat collections name the game.
pub const PROGRAM_ID: &str = "0400A3A01EE4C000";

/// How long a version that had no cheat file is not asked again by Play.
const RECHECK: Duration = Duration::from_secs(24 * 3600);

/// Whether cheat files may be fetched: launcher.json `online_cheats` (absent: yes), and POKOMAC_ONLINE_CHEATS=0 for a test run.
pub fn enabled(config: Option<bool>) -> bool {
    if std::env::var("POKOMAC_ONLINE_CHEATS").is_ok_and(|v| v.trim() == "0") {
        return false;
    }
    config.unwrap_or(true)
}

/// launcher.json's `online_cheats` read from the file (Play in the window and `--play` alike).
pub fn configured(cfg_file: &Path) -> Option<bool> {
    let v: Value = serde_json::from_str(&fs::read_to_string(cfg_file).ok()?).ok()?;
    v.get("online_cheats")?.as_bool()
}

/// The cheats folder: `[cheats] dir` of pkp.ini (relative to the game folder), else `<game folder>/cheats` (docs/CHEATS.md).
pub fn cheats_dir(game_dir: &Path) -> PathBuf {
    let dir = crate::settings::pkp_value(game_dir, "cheats", "dir").map(|v| v.trim().to_string()).unwrap_or_default();
    if dir.is_empty() {
        return game_dir.join("cheats");
    }
    let p = PathBuf::from(&dir);
    if p.is_absolute() { p } else { game_dir.join(p) }
}

/// main's build id as the cheat files name it (16 lower-case hex digits): `[fs] exefs`'s main (the dump's), else the game folder's.
pub fn build_id(game_dir: &Path) -> Option<String> {
    let mut places = vec![];
    if let Some(e) = crate::settings::pkp_value(game_dir, "fs", "exefs").filter(|e| !e.trim().is_empty()) {
        let p = PathBuf::from(e.trim());
        places.push(if p.is_absolute() { p } else { game_dir.join(p) }.join("main"));
    }
    places.push(game_dir.join("exefs").join("main"));
    places.iter().find_map(|m| crate::dump::main_build_id(m)).filter(|b| b.len() >= 16).map(|b| b[..16].to_lowercase())
}

/// What Settings > Cheats shows about the cheat file.
#[derive(Serialize, Default)]
pub struct Info {
    /// the cheats folder
    pub dir: String,
    /// main's build id (empty: unknown, the game is not set up)
    pub build: String,
    /// a cheat file for this version is there
    pub present: bool,
    /// it is the player's own (not fetched)
    pub own: bool,
    /// the fetched file's record (source, author, home, url, license, cheats, fetched): null for none
    pub fetched: Value,
    /// fetching is on
    pub online: bool,
}

pub fn info(game_dir: &Path, online: bool) -> Info {
    let dir = cheats_dir(game_dir);
    let Some(build) = build_id(game_dir) else {
        return Info { dir: crate::dump::fwd(&dir), online, ..Default::default() };
    };
    let flat = dir.join(format!("{}.txt", build));
    let fetched: Value = fs::read_to_string(dir.join(format!("{}.fetched.json", build))).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(Value::Null);
    let tree = has_tree_file(&dir, &build);
    let present = flat.is_file() || tree;
    Info { dir: crate::dump::fwd(&dir), own: present && (tree || fetched.is_null()), present, fetched, online, build }
}

fn has_tree_file(dir: &Path, build: &str) -> bool {
    // <dir>/<program id>/cheats/<build id>.txt (the console's layout), any program id folder
    fs::read_dir(dir).into_iter().flatten().flatten().any(|e| e.path().join("cheats").join(format!("{}.txt", build)).is_file())
}

/// Runs the fetcher: its answer (found, message, source, author ...) or why it could not run.
pub fn fetch(game_dir: &Path, tools_dir: &Path, force: bool) -> Result<Value, String> {
    let script = tools_dir.join("build").join("cheats_fetch.py");
    if !script.is_file() {
        return Err("This release has no cheat fetcher (tools/build/cheats_fetch.py). Download the release again so its tools are complete.".into());
    }
    let build = build_id(game_dir).ok_or("The game files are not set up yet, so the game's version is not known.")?;
    let py = crate::host::python(tools_dir);
    if !py.is_file() {
        return Err("The bundled Python is missing. Run Set up once: it fetches the tools the cheat fetcher uses too.".into());
    }
    let dir = cheats_dir(game_dir);
    let mut cmd = crate::host::command(&py);
    cmd.arg("-IB").arg(&script).arg("--title").arg(PROGRAM_ID).arg("--build").arg(&build).arg("--out").arg(&dir);
    if force {
        cmd.arg("--force");
    }
    cmd.stdin(std::process::Stdio::null());
    let out = cmd.output().map_err(|e| format!("The cheat fetcher could not start: {}", e))?;
    let v: Value = serde_json::from_slice(&out.stdout)
        .map_err(|_| format!("The cheat fetcher gave no answer: {}", String::from_utf8_lossy(&out.stderr).trim().chars().take(400).collect::<String>()))?;
    if let Some(e) = v.get("error").and_then(|e| e.as_str()) {
        return Err(e.to_string());
    }
    let _ = fs::create_dir_all(&dir);
    let _ = fs::write(dir.join(format!("{}.fetch_checked", build)), "");
    Ok(v)
}

/// Play with cheats on and no cheat file for this version: fetched once, before the game starts (the runtime reads the file at its
/// start), unless asked within the last day or fetching is off.
pub fn before_play(game_dir: &Path, tools_dir: &Path, online: Option<bool>) {
    if !enabled(online) {
        return;
    }
    let on = crate::settings::pkp_value(game_dir, "cheats", "enable").is_some_and(|v| matches!(v.trim(), "1" | "true" | "on" | "yes"));
    if !on {
        return;
    }
    let i = info(game_dir, true);
    if i.present || i.build.is_empty() {
        return;
    }
    let checked = PathBuf::from(&i.dir).join(format!("{}.fetch_checked", i.build));
    if fs::metadata(&checked).and_then(|m| m.modified()).ok().and_then(|t| SystemTime::now().duration_since(t).ok()).is_some_and(|age| age < RECHECK) {
        return;
    }
    match fetch(game_dir, tools_dir, false) {
        Ok(v) => println!("Cheats: {}", v.get("message").and_then(|m| m.as_str()).unwrap_or("")),
        Err(e) => eprintln!("Cheats were not fetched: {}", e),
    }
}

/// The cheat file: where it is, whose it is (Settings > Cheats).
#[tauri::command]
pub fn cheats_info(app: AppHandle) -> Info {
    let cfg = crate::load_config(&app);
    info(&crate::game_dir_of(&cfg), enabled(cfg.online_cheats))
}

/// "Find cheats online": the fetcher's answer.
#[tauri::command]
pub async fn cheats_fetch(app: AppHandle, force: Option<bool>) -> Result<Value, String> {
    let cfg = crate::load_config(&app);
    if !enabled(cfg.online_cheats) {
        return Err("Finding cheat files online is off (Settings, Cheats).".into());
    }
    let dir = crate::game_dir_of(&cfg);
    let tools = crate::tools_dir();
    tauri::async_runtime::spawn_blocking(move || fetch(&dir, &tools, force.unwrap_or(false))).await.map_err(|e| e.to_string())?
}

/// Opens the cheats folder.
#[tauri::command]
pub fn cheats_dir_open(app: AppHandle) -> Result<(), String> {
    let d = cheats_dir(&crate::game_dir_of(&crate::load_config(&app)));
    fs::create_dir_all(&d).map_err(|e| format!("cannot create {}: {}", d.display(), e))?;
    crate::play::open_with_shell(&d)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pokomac_cheats_fetch_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn nso(dir: &Path, bid: &[u8]) {
        let mut h = vec![0u8; 0x100];
        h[..4].copy_from_slice(b"NSO0");
        h[0x40..0x40 + bid.len()].copy_from_slice(bid);
        fs::create_dir_all(dir).unwrap();
        fs::write(dir.join("main"), h).unwrap();
    }

    #[test]
    fn build_id_and_folder() {
        let d = tmp("bid");
        nso(&d.join("dump").join("exefs"), &[0x76, 0xa4, 0x89, 0xf3, 0xbd, 0x36, 0x42, 0xfa, 0x5e, 0x18]);
        assert_eq!(build_id(&d), None, "no pkp.ini and no exefs in the game folder: not known");
        fs::write(d.join("pkp.ini"), "[fs]\nexefs = dump/exefs\n").unwrap();
        assert_eq!(build_id(&d).as_deref(), Some("76a489f3bd3642fa"), "[fs] exefs's main: the first 8 bytes of its build id, lower-case hex");
        assert_eq!(cheats_dir(&d), d.join("cheats"), "no [cheats] dir: <game>/cheats");
        fs::write(d.join("pkp.ini"), "[fs]\nexefs = dump/exefs\n\n[cheats]\ndir = mine\n").unwrap();
        assert_eq!(cheats_dir(&d), d.join("mine"), "a relative [cheats] dir is the game folder's");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn info_says_whose_file() {
        let d = tmp("info");
        nso(&d.join("exefs"), &[0x76, 0xa4, 0x89, 0xf3, 0xbd, 0x36, 0x42, 0xfa]);
        let i = info(&d, true);
        assert!(!i.present && !i.own && i.fetched.is_null() && i.build == "76a489f3bd3642fa");
        fs::create_dir_all(d.join("cheats")).unwrap();
        fs::write(d.join("cheats").join("76a489f3bd3642fa.txt"), "[A]\n04000000 00000010 00000001\n").unwrap();
        let i = info(&d, true);
        assert!(i.present && i.own, "a file with no record: the player's own");
        fs::write(d.join("cheats").join("76a489f3bd3642fa.fetched.json"), r#"{"author": "tomvita", "cheats": 1}"#).unwrap();
        let i = info(&d, true);
        assert!(i.present && !i.own && i.fetched["author"] == "tomvita", "a fetched file names its author");
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn online_switch() {
        assert!(enabled(None) && enabled(Some(true)) && !enabled(Some(false)));
    }
}
