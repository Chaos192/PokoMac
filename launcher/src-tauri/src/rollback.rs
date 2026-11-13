// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//! The copy of the player's own data that is made before the launcher installs an OLDER build: the save folder, `pkp.ini`, `controls.ini`,
//! `profiles.json` and the launcher's `launcher.json`, in `<game folder>/rollback-backups/<date-time>-before-<tag>/`. The last three are
//! kept. If the copy cannot be made the install is stopped before anything is replaced. `rollback-backups` is in the update's keep list.

use crate::ini::Ini;
use crate::settings::PKP_INI;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

pub const DIR: &str = "rollback-backups";
const KEEP_LAST: usize = 3;
/// Single files next to pkp.exe that are the player's.
const FILES: [&str; 3] = [PKP_INI, "controls.ini", "profiles.json"];

fn copy_tree(src: &Path, dst: &Path) -> io::Result<()> {
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

/// A folder name part: letters, digits, `.` and `-` only.
fn clean(tag: &str) -> String {
    tag.chars().map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '-' { c } else { '_' }).collect()
}

/// Copies the player's data. `config_dir`: where the launcher keeps launcher.json. Returns the folder made.
pub fn backup(game_dir: &Path, config_dir: &Path, tag: &str) -> Result<PathBuf, String> {
    let root = game_dir.join(DIR);
    let name = format!("{}-before-{}", chrono::Utc::now().format("%Y%m%d-%H%M%S"), clean(tag));
    let dir = root.join(&name);
    let made = (|| -> io::Result<()> {
        fs::create_dir_all(&dir)?;
        let ini = fs::read(game_dir.join(PKP_INI)).ok().map(|b| Ini::parse(&String::from_utf8_lossy(&b)));
        let save_dir = crate::saveimport::save_dir_of(game_dir, &ini.and_then(|i| i.get("fs", "save_dir")).unwrap_or_default());
        if save_dir.is_dir() {
            copy_tree(&save_dir, &dir.join("save"))?;
        }
        for f in FILES {
            let from = game_dir.join(f);
            if from.is_file() {
                fs::copy(&from, dir.join(f))?;
            }
        }
        let lj = config_dir.join("launcher.json");
        if lj.is_file() {
            fs::copy(&lj, dir.join("launcher.json"))?;
        }
        fs::write(
            dir.join("README.txt"),
            format!("A copy of your save folder and settings, made by the launcher just before it installed the older build {}.\r\nTo go back to this state, close the game and the launcher and copy these files back over the ones in the game folder.\r\n", tag),
        )
    })();
    if let Err(e) = made {
        let _ = fs::remove_dir_all(&dir);
        return Err(format!("Your saves and settings could not be copied first ({}), so nothing was changed.", e));
    }
    prune(&root);
    Ok(dir)
}

fn prune(root: &Path) {
    let mut names: Vec<String> = fs::read_dir(root).map(|rd| rd.flatten().filter(|e| e.path().is_dir()).filter_map(|e| e.file_name().into_string().ok()).collect()).unwrap_or_default();
    names.sort();
    let drop = names.len().saturating_sub(KEEP_LAST);
    for n in names.into_iter().take(drop) {
        let _ = fs::remove_dir_all(root.join(n));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    fn tmp() -> PathBuf {
        static N: AtomicUsize = AtomicUsize::new(0);
        let d = std::env::temp_dir().join(format!("pkp-rollback-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn game() -> (PathBuf, PathBuf) {
        let (g, c) = (tmp(), tmp());
        fs::create_dir_all(g.join("save").join("account").join("k")).unwrap();
        fs::write(g.join("save").join("account").join("k").join("G00"), b"global").unwrap();
        fs::write(g.join("pkp.ini"), b"[account]\nnickname = Koi\n").unwrap();
        fs::write(g.join("controls.ini"), b"[meta]\n").unwrap();
        fs::write(g.join("profiles.json"), b"{}").unwrap();
        fs::write(c.join("launcher.json"), b"{\"channel\":\"testing\"}").unwrap();
        (g, c)
    }

    #[test]
    fn the_players_data_is_copied_before_an_older_build_goes_in() {
        let (g, c) = game();
        let d = backup(&g, &c, "v0.7.9").unwrap();
        assert!(d.starts_with(g.join(DIR)) && d.file_name().unwrap().to_string_lossy().ends_with("-before-v0.7.9"));
        assert_eq!(fs::read(d.join("save").join("account").join("k").join("G00")).unwrap(), b"global");
        for f in ["pkp.ini", "controls.ini", "profiles.json", "launcher.json", "README.txt"] {
            assert!(d.join(f).is_file(), "{} must be in the copy", f);
        }
        assert_eq!(fs::read(g.join("pkp.ini")).unwrap(), b"[account]\nnickname = Koi\n", "the originals are not touched");
    }

    #[test]
    fn missing_pieces_are_skipped_and_a_folder_name_stays_clean() {
        let g = tmp();
        let c = tmp();
        let d = backup(&g, &c, "v0.8.1/../evil beta").unwrap();
        assert!(d.exists() && d.join("README.txt").is_file());
        assert!(!d.join("save").exists(), "no save folder, none copied");
        assert!(d.parent().unwrap() == g.join(DIR), "the folder is directly under rollback-backups");
        assert!(!d.file_name().unwrap().to_string_lossy().contains(['/', '\\', ' ']));
    }

    #[test]
    fn a_failed_copy_stops_everything_and_leaves_no_half_folder() {
        let (g, c) = game();
        // a file where the backup root must be a folder: the copy cannot be made
        fs::write(g.join(DIR), b"in the way").unwrap();
        let err = backup(&g, &c, "v0.7.9").unwrap_err();
        assert!(err.contains("nothing was changed"), "{}", err);
        assert_eq!(fs::read(g.join(DIR)).unwrap(), b"in the way");
    }

    #[test]
    fn only_the_last_three_copies_stay() {
        let (g, c) = game();
        let root = g.join(DIR);
        for i in 0..5 {
            fs::create_dir_all(root.join(format!("2026110{}-000000-before-v0.7.{}", i, i))).unwrap();
        }
        backup(&g, &c, "v0.7.9").unwrap();
        let left: Vec<String> = fs::read_dir(&root).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        assert_eq!(left.len(), 3, "{:?}", left);
        assert!(left.iter().any(|n| n.ends_with("-before-v0.7.9")), "the new copy stays");
    }
}
