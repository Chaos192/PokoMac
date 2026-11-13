// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright (c) 2026 Chaos192
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
//! The cheats overlay's theme files (docs/CHEATS.md "Themes"): the player's own themes live in `<app config dir>/cheats-themes/`, one
//! JSON file each (and the .ttf / .otf it may name, next to it). The runtime reads the one `[cheats] theme` names (the settings
//! page writes its full path into pkp.ini) and checks it; a file that does not read falls back to the default theme there. The built-in
//! themes are compiled into the runtime (runtime/assets/cheats/themes) and the settings page lists them from its own import.
use std::fs;
use std::path::{Path, PathBuf};

use serde::Serialize;
use tauri::{AppHandle, Manager};
use tauri_plugin_dialog::DialogExt;

/// A theme file in the themes folder, as the theme picker lists it.
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct ThemeFile {
    /// The file's full path ([cheats] theme takes it as it is)
    pub path: String,
    /// The theme's own "name", else the file name without .json
    pub name: String,
}

/// The largest theme file read (the runtime refuses bigger ones too).
const THEME_MAX_BYTES: u64 = 64 * 1024;

fn themes_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?.join("cheats-themes");
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    Ok(dir)
}

/// A theme file's text when it is a JSON object of at most 64 KB (the full checks are the runtime's).
fn read_theme(path: &Path) -> Result<serde_json::Value, String> {
    let size = fs::metadata(path).map_err(|e| format!("cannot read {}: {e}", path.display()))?.len();
    if size > THEME_MAX_BYTES {
        return Err(format!("{} is bigger than 64 KB: not a theme", path.display()));
    }
    let text = fs::read_to_string(path).map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    let v: serde_json::Value = serde_json::from_str(&text).map_err(|e| format!("{} is not JSON: {e}", path.display()))?;
    if !v.is_object() {
        return Err(format!("{} is not a theme (a JSON object)", path.display()));
    }
    Ok(v)
}

/// The picker's entry for a theme file; None when it is not one.
pub fn theme_entry(path: &Path) -> Option<ThemeFile> {
    let v = read_theme(path).ok()?;
    let stem = path.file_stem()?.to_string_lossy().to_string();
    let name = v.get("name").and_then(|n| n.as_str()).map(|s| s.trim()).filter(|s| !s.is_empty() && s.len() <= 64).map(str::to_string).unwrap_or(stem);
    Some(ThemeFile { path: crate::dump::fwd(path), name })
}

/// Every theme file of `dir`, by name.
pub fn list_dir(dir: &Path) -> Vec<ThemeFile> {
    let mut out: Vec<ThemeFile> = fs::read_dir(dir)
        .map(|rd| rd.flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|x| x.eq_ignore_ascii_case("json"))).filter_map(|p| theme_entry(&p)).collect())
        .unwrap_or_default();
    out.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()).then(a.path.cmp(&b.path)));
    out
}

/// A file name in `dir` not taken yet: `name.ext`, else `name 2.ext`, `name 3.ext` ...
fn free_name(dir: &Path, stem: &str, ext: &str) -> PathBuf {
    let first = dir.join(format!("{stem}.{ext}"));
    if !first.exists() {
        return first;
    }
    (2..).map(|n| dir.join(format!("{stem} {n}.{ext}"))).find(|p| !p.exists()).expect("a free name")
}

/// Copies a theme into `dir` (a new name when one is taken), with the font it names when that font is next to it: the copy's "font" then
/// names the copied font. Returns the copy.
pub fn import_into(src: &Path, dir: &Path) -> Result<PathBuf, String> {
    let mut v = read_theme(src)?;
    let stem = src.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "theme".into());
    let font = v.get("font").and_then(|f| f.as_str()).map(str::to_string).filter(|f| !f.trim().is_empty());
    if let Some(font) = font {
        let from = src.parent().unwrap_or(Path::new(".")).join(&font);
        let ext = from.extension().map(|x| x.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
        if (ext == "ttf" || ext == "otf") && from.is_file() {
            let fstem = from.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "font".into());
            let to = free_name(dir, &fstem, &ext);
            fs::copy(&from, &to).map_err(|e| format!("cannot copy the font {}: {e}", from.display()))?;
            v["font"] = serde_json::Value::String(to.file_name().unwrap().to_string_lossy().to_string());
        }
    }
    let to = free_name(dir, &stem, "json");
    let text = serde_json::to_string_pretty(&v).map_err(|e| e.to_string())?;
    fs::write(&to, text + "\n").map_err(|e| format!("cannot write {}: {e}", to.display()))?;
    Ok(to)
}

/// The player's theme files (the theme picker's list).
#[tauri::command]
pub fn cheats_themes(app: AppHandle) -> Result<Vec<ThemeFile>, String> {
    Ok(list_dir(&themes_dir(&app)?))
}

/// "Import theme": an Open dialog (.json); the theme (and its font) copied into the themes folder; None when the dialog is closed.
#[tauri::command]
pub async fn cheats_theme_import(app: AppHandle) -> Result<Option<ThemeFile>, String> {
    let dlg = app.dialog().file().set_title("Import a cheats overlay theme").add_filter("Theme", &["json"]);
    let Some(path) = dlg.blocking_pick_file().and_then(|p| p.into_path().ok()) else { return Ok(None) };
    let to = import_into(&path, &themes_dir(&app)?)?;
    theme_entry(&to).map(Some).ok_or_else(|| format!("{} is not a theme", to.display()))
}

/// "Open the themes folder" in the file manager.
#[tauri::command]
pub fn cheats_themes_open(app: AppHandle) -> Result<(), String> {
    crate::play::open_with_shell(&themes_dir(&app)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-cheats-themes-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn lists_themes_by_name_and_skips_the_rest() {
        let d = temp("list");
        fs::write(d.join("b.json"), r##"{"name": "Forest", "accent": "#9be15d"}"##).unwrap();
        fs::write(d.join("a.json"), r##"{"accent": "#ffffff"}"##).unwrap();
        fs::write(d.join("broken.json"), "theme = forest").unwrap();
        fs::write(d.join("list.json"), "[1, 2]").unwrap();
        fs::write(d.join("notes.txt"), "{}").unwrap();
        let l = list_dir(&d);
        let names: Vec<&str> = l.iter().map(|t| t.name.as_str()).collect();
        assert_eq!(names, ["a", "Forest"]);
        assert!(l[1].path.ends_with("/b.json"));
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn import_copies_the_theme_and_its_font_under_free_names() {
        let src = temp("src");
        let dir = temp("dst");
        fs::write(src.join("forest.json"), r#"{"name": "Forest", "font": "Leaf.ttf"}"#).unwrap();
        fs::write(src.join("Leaf.ttf"), b"font bytes").unwrap();
        let a = import_into(&src.join("forest.json"), &dir).unwrap();
        assert_eq!(a.file_name().unwrap(), "forest.json");
        assert_eq!(fs::read(dir.join("Leaf.ttf")).unwrap(), b"font bytes");
        let b = import_into(&src.join("forest.json"), &dir).unwrap();   // again: new names, its font renamed with it
        assert_eq!(b.file_name().unwrap(), "forest 2.json");
        let v: serde_json::Value = serde_json::from_str(&fs::read_to_string(&b).unwrap()).unwrap();
        assert_eq!(v["font"], "Leaf 2.ttf");
        assert!(dir.join("Leaf 2.ttf").is_file());
        // not a theme: refused, nothing written
        fs::write(src.join("bad.json"), "nope").unwrap();
        assert!(import_into(&src.join("bad.json"), &dir).is_err());
        assert!(!dir.join("bad.json").exists());
        let _ = fs::remove_dir_all(&src);
        let _ = fs::remove_dir_all(&dir);
    }
}
