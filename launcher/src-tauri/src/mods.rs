// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
//! Mods (docs/MODS.md): the mod manager in the launcher. The work is mods.py's, shipped beside pkp_build.py (tools/build/mods.py): it
//! installs a zip, 7z or folder in any common layout into `<game folder>/mods/`, keeps the order and which mods are on, refuses a mod
//! for another game, another version or something PokoMac cannot use, and writes what the runtime and the next build use
//! (`mods/.active/`). Game file mods apply at the next start, with no build. Code mods (IPS / IPS32 / .pchtxt, a replaced module of the
//! same version) make the effective ExeFS in `mods/.active/code.json`; Home then shows "Build with mods", pkp_build.py lifts that ExeFS
//! (its object cache compiles only the changed units) and, once the build is done, points the runtime at the same modules ([fs] exefs_mod).
//! A game folder with no mod installed has no `mods/.active/code.json` and builds and plays exactly as before.
use serde_json::Value;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tauri::{AppHandle, Manager};
use tauri_plugin_dialog::DialogExt;

use crate::{dump, host};

/// The store inside the game folder.
pub const STORE: &str = "mods";

/// Home's line when the enabled code mods are not the ones pkp was built with.
pub const CODE_CHANGED: &str =
    "Your code mods changed. Press Build with mods to build the game with them: only the changed code is built again, and your saves and shaders are kept.";

fn script(tools_dir: &Path) -> PathBuf {
    tools_dir.join("build").join("mods.py")
}

/// mods.py with these arguments: its JSON answer, or the reason it gives.
pub fn run(tools_dir: &Path, args: &[OsString]) -> Result<Value, String> {
    let s = script(tools_dir);
    if !s.is_file() {
        return Err("The mod manager (tools/build/mods.py) is missing. Download the release again so its tools are complete.".into());
    }
    let py = host::python(tools_dir);
    if !py.is_file() {
        return Err("The bundled Python is missing. Run Set up once: it fetches the build tools the mod manager uses too.".into());
    }
    let mut cmd = host::command(&py);
    cmd.arg("-IB").arg(&s).args(args).stdin(std::process::Stdio::null());
    let out = cmd.output().map_err(|e| format!("The mod manager could not start: {}", e))?;
    let v: Value = serde_json::from_slice(&out.stdout).map_err(|_| {
        format!("The mod manager gave no answer: {}", String::from_utf8_lossy(&out.stderr).trim().chars().take(600).collect::<String>())
    })?;
    if let Some(e) = v.get("error").and_then(|e| e.as_str()) {
        return Err(e.to_string());
    }
    Ok(v)
}

/// The stamp of what the enabled mods make the next build compile (mods/.active/code.json): "" when no code mod is on or no mod is
/// installed. pkp_build.py reads the same file (mods_code).
pub fn code_stamp(dir: &Path) -> String {
    let Ok(text) = fs::read_to_string(dir.join(STORE).join(".active").join("code.json")) else { return String::new() };
    let v: Value = serde_json::from_str(&text).unwrap_or(Value::Null);
    if v["exefs"].as_str().unwrap_or_default().is_empty() {
        return String::new();
    }
    v["stamp"].as_str().unwrap_or_default().to_string()
}

/// Whether the code mods differ from the ones pkp was built with (build/done.json "mods", absent for a build without them).
pub fn code_changed(dir: &Path) -> bool {
    let built = fs::read_to_string(dir.join("build").join("done.json"))
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok())
        .map(|v| v["mods"].as_str().unwrap_or_default().to_string())
        .unwrap_or_default();
    built != code_stamp(dir)
}

/// Mods change only while nothing uses the game folder: Set up builds from it, the running game reads it.
fn game_dir(app: &AppHandle, changing: bool) -> Result<PathBuf, String> {
    if changing {
        if app.state::<Arc<crate::SetupState>>().progress.lock().unwrap().state == "running" {
            return Err("Wait until Set up has finished, then change the mods.".into());
        }
        if app.state::<crate::play::PlayState>().0.lock().unwrap().state == "running" {
            return Err("Close the game first, then change the mods. They apply at its next start.".into());
        }
    }
    let dir = crate::game_dir_of(&crate::load_config(app));
    if !dir.join("pkp.ini").is_file() {
        return Err("Set up the game first: mods are installed into its game folder.".into());
    }
    Ok(dir)
}

async fn blocking<T: Send + 'static>(f: impl FnOnce() -> Result<T, String> + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f).await.map_err(|e| e.to_string())?
}

fn os(s: &str) -> OsString {
    OsString::from(s)
}

/// The installed mods: each with its name, kinds (romfs, code, cheats), on/off, conflicts, and whether the next build changes.
#[tauri::command]
pub async fn mods_list(app: AppHandle) -> Result<Value, String> {
    let dir = game_dir(&app, false)?;
    let tools = crate::tools_dir();
    blocking(move || run(&tools, &[os("list"), dir.into_os_string()])).await
}

/// Installs a mod (a .zip, .7z, .rar or folder). Refused with the reason for another game, another version or what PokoMac cannot use.
#[tauri::command]
pub async fn mods_install(app: AppHandle, path: String) -> Result<Value, String> {
    let dir = game_dir(&app, true)?;
    let tools = crate::tools_dir();
    blocking(move || run(&tools, &[os("install"), dir.into_os_string(), OsString::from(path)])).await
}

/// enable / disable / uninstall a mod, or move it to `pos` (0 = first: it wins over the others).
#[tauri::command]
pub async fn mods_change(app: AppHandle, act: String, mod_id: String, pos: Option<u32>) -> Result<Value, String> {
    let dir = game_dir(&app, true)?;
    let tools = crate::tools_dir();
    let mut args = match act.as_str() {
        "enable" | "disable" | "uninstall" | "move" => vec![os(&act), dir.into_os_string(), OsString::from(mod_id)],
        _ => return Err("That cannot be done to a mod.".into()),
    };
    if act == "move" {
        args.push(OsString::from(pos.unwrap_or(0).to_string()));
    }
    blocking(move || run(&tools, &args)).await
}

/// Make a mod: `export` the game's own files into `out` (its code modules uncompressed, with `romfs` its RomFS or the part under
/// `romfs_path`, and the build ids), or a `template` mod folder named `name` there.
#[tauri::command]
pub async fn mods_make(app: AppHandle, what: String, out: String, name: Option<String>, romfs: bool, romfs_path: Option<String>) -> Result<Value, String> {
    let dir = game_dir(&app, false)?;
    let tools = crate::tools_dir();
    let mut args = vec![os(&what), dir.into_os_string(), os("--out"), OsString::from(out)];
    match what.as_str() {
        "export" => {
            if romfs {
                args.push(os("--romfs"));
                if let Some(p) = romfs_path.filter(|p| !p.trim().is_empty()) {
                    args.push(os("--romfs-path"));
                    args.push(OsString::from(p));
                }
            }
        }
        "template" => {
            args.push(os("--name"));
            args.push(OsString::from(name.unwrap_or_else(|| "My mod".into())));
        }
        _ => return Err("That cannot be made here.".into()),
    }
    blocking(move || run(&tools, &args)).await
}

/// Zips a mod folder to share it (<folder>.zip beside it), in the layout the install reads.
#[tauri::command]
pub async fn mods_pack(folder: String) -> Result<Value, String> {
    let tools = crate::tools_dir();
    blocking(move || run(&tools, &[os("pack"), OsString::from(folder)])).await
}

/// Asks for a mod: an archive (.zip, .7z, .rar) or, with `folder`, a folder.
#[tauri::command]
pub async fn mods_pick(app: AppHandle, folder: bool) -> Option<String> {
    let dlg = app.dialog().file();
    let picked = if folder {
        dlg.set_title("Choose a folder").blocking_pick_folder()
    } else {
        dlg.set_title("Choose a mod").add_filter("Mods", &["zip", "7z", "rar"]).blocking_pick_file()
    };
    picked.and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

/// Opens the mods folder (where the installed mods are).
#[tauri::command]
pub async fn mods_open(app: AppHandle) -> Result<(), String> {
    let dir = game_dir(&app, false)?.join(STORE);
    fs::create_dir_all(&dir).map_err(|e| format!("Cannot create {}: {}", dump::fwd(&dir), e))?;
    crate::play::open_with_shell(&dir)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn no_mods_no_code() {
        let d = std::env::temp_dir().join(format!("pokomac-mods-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(d.join("build")).unwrap();
        assert_eq!(code_stamp(&d), "", "a game folder with no mods: the game's own code, no stamp");
        assert!(!code_changed(&d), "no mods and no build record: nothing changed");
        fs::write(d.join("build/done.json"), br#"{"format": 2}"#).unwrap();
        assert!(!code_changed(&d), "a build from before mods: nothing changed");
        fs::create_dir_all(d.join("mods/.active")).unwrap();
        fs::write(d.join("mods/.active/code.json"), br#"{"stamp": "", "exefs": ""}"#).unwrap();
        assert!(!code_changed(&d), "mods with no code mod on");
        fs::write(d.join("mods/.active/code.json"), br#"{"stamp": "0123abcd", "exefs": "/x/mods/.active/exefs-0123abcd"}"#).unwrap();
        assert_eq!(code_stamp(&d), "0123abcd");
        assert!(code_changed(&d), "a code mod turned on: build with mods");
        fs::write(d.join("build/done.json"), br#"{"format": 2, "mods": "0123abcd"}"#).unwrap();
        assert!(!code_changed(&d), "built with those mods");
        fs::write(d.join("mods/.active/code.json"), br#"{"stamp": "", "exefs": ""}"#).unwrap();
        assert!(code_changed(&d), "the code mods turned off after a build with them: build again");
        let _ = fs::remove_dir_all(&d);
    }
}
