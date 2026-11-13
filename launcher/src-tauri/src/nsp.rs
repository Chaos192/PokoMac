// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//! Unpacks the add-on (DLC) RomFS of a decrypted `.nsp` into `<aoc>/<16-digit hex id>/romfs/`, the layout dump.rs
//! finds and pkp's `[fs] aoc_dir` serves.
//!
//! An .nsp is a PFS0 of NCAs. Each add-on is one data NCA whose first section is a RomFS under an IVFC hash tree.
//! Like the game files, the NCAs must already be decrypted (header magic readable, section encryption "none"); a
//! package that is still encrypted is refused in words. The IVFC hashes are not re-checked: the data level is read as
//! it is. Nothing is written outside `<aoc>/<id>/`, and names that could leave it are refused.

use std::fs::{self, File};
use std::io::{BufReader, Read, Seek, SeekFrom, Write};
use std::path::Path;

/// NCA media unit and the size of the header block (0x400) plus four section headers (0x200 each).
const MEDIA: u64 = 0x200;
const NCA_HEADER: usize = 0xC00;
const CONTENT_DATA: u8 = 4;
/// The add-on ids of the game: its program id + 0x1000 + index, so they share all but the lowest 12 bits.
const ADDON_PREFIX: u64 = 0x0400_A3A0_1EE4_D;

fn u32_at(b: &[u8], o: usize) -> Result<u32, String> {
    b.get(o..o + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap())).ok_or_else(damaged)
}

fn u64_at(b: &[u8], o: usize) -> Result<u64, String> {
    b.get(o..o + 8).map(|s| u64::from_le_bytes(s.try_into().unwrap())).ok_or_else(damaged)
}

fn damaged() -> String {
    "The .nsp file is damaged or incomplete.".into()
}

/// One file of the PFS0: its name and where its bytes are in the .nsp.
struct Entry {
    name: String,
    offset: u64,
    size: u64,
}

/// Reads the PFS0 table of contents; every entry is checked to lie inside the file.
fn pfs0(f: &mut File) -> Result<Vec<Entry>, String> {
    let len = f.metadata().map_err(|e| e.to_string())?.len();
    let mut head = [0u8; 16];
    f.seek(SeekFrom::Start(0)).and_then(|_| f.read_exact(&mut head)).map_err(|_| damaged())?;
    if &head[..4] != b"PFS0" {
        return Err("This is not an .nsp file (no PFS0 header).".into());
    }
    let (count, strtab) = (u32_at(&head, 4)? as usize, u32_at(&head, 8)? as u64);
    let table = 16 + count as u64 * 24 + strtab;
    if count > 4096 || table > len {
        return Err(damaged());
    }
    let mut toc = vec![0u8; table as usize];
    f.seek(SeekFrom::Start(0)).and_then(|_| f.read_exact(&mut toc)).map_err(|_| damaged())?;
    let names = &toc[16 + count * 24..];
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let o = 16 + i * 24;
        let (offset, size, at) = (u64_at(&toc, o)?, u64_at(&toc, o + 8)?, u32_at(&toc, o + 16)? as usize);
        let name = names.get(at..).and_then(|s| s.split(|&c| c == 0).next()).ok_or_else(damaged)?;
        let start = table.checked_add(offset).ok_or_else(damaged)?;
        if start.checked_add(size).map_or(true, |end| end > len) {
            return Err(damaged());
        }
        out.push(Entry { name: String::from_utf8_lossy(name).into_owned(), offset: start, size });
    }
    Ok(out)
}

/// Where the RomFS bytes of an add-on NCA are (absolute in the .nsp) and the add-on's id.
struct AddOn {
    id: u64,
    start: u64,
    size: u64,
}

/// Finds the RomFS of a data NCA from its header, or None for any other kind of NCA (a program, the metadata).
fn addon_romfs(nca: &Entry, h: &[u8]) -> Result<Option<AddOn>, String> {
    if h.get(0x200..0x204) != Some(b"NCA3") {
        return Err("This .nsp is still encrypted. Use decrypted NCA files, as for the game.".into());
    }
    if h[0x205] != CONTENT_DATA || h[0x230..0x240].iter().any(|&b| b != 0) {
        return Ok(None);   // not add-on data, or tied to a ticket (encrypted)
    }
    let id = u64_at(h, 0x210)?;
    if id >> 12 != ADDON_PREFIX {
        return Err(format!("Add-on {id:016X} is not for this game."));
    }
    let (first, last) = (u32_at(h, 0x240)? as u64 * MEDIA, u32_at(h, 0x244)? as u64 * MEDIA);
    let fs = &h[0x400..0x600];
    let (fs_type, hash_type, crypt) = (fs[2], fs[3], fs[4]);
    if last == 0 || fs_type != 0 || hash_type != 3 {
        return Err(format!("{}: the first section is not a RomFS.", nca.name));
    }
    if crypt != 1 {
        return Err("This .nsp is still encrypted. Use decrypted NCA files, as for the game.".into());
    }
    // IVFC: magic, version, master hash size, level count, then (offset, size, block size, reserved) per level; the last one is the data
    let levels = u32_at(fs, 0x14)? as usize;
    if &fs[8..12] != b"IVFC" || !(2..=7).contains(&levels) {
        return Err(damaged());
    }
    let at = 0x18 + (levels - 2) * 24;
    let (off, size) = (u64_at(fs, at)?, u64_at(fs, at + 8)?);
    if first.checked_add(off).and_then(|s| s.checked_add(size)).map_or(true, |end| end > last.min(nca.size)) {
        return Err(damaged());
    }
    Ok(Some(AddOn { id, start: nca.offset + first + off, size }))
}

/// A RomFS image read in one piece (add-ons are small) plus the offset of its file data.
struct Romfs {
    img: Vec<u8>,
    dirs: (usize, usize),
    files: (usize, usize),
    data: usize,
}

impl Romfs {
    /// Checks the header's tables against the image size.
    fn new(img: Vec<u8>) -> Result<Romfs, String> {
        let at = |i: usize| u64_at(&img, i * 8).map(|v| v as usize);
        let (dirs, files, data) = ((at(3)?, at(4)?), (at(7)?, at(8)?), at(9)?);
        for (o, n) in [dirs, files] {
            if o.checked_add(n).map_or(true, |e| e > img.len()) {
                return Err(damaged());
            }
        }
        Ok(Romfs { img, dirs, files, data })
    }

    /// The name stored after a 0x18 (directory) or 0x20 (file) byte entry header, refused when it could leave the folder.
    fn name(&self, table: (usize, usize), entry: usize, header: usize) -> Result<String, String> {
        let at = table.0 + entry;
        let len = u32_at(&self.img, at + header - 4)? as usize;
        let raw = self.img.get(at + header..at + header + len).filter(|_| at + header + len <= table.0 + table.1).ok_or_else(damaged)?;
        let name = String::from_utf8(raw.to_vec()).map_err(|_| damaged())?;
        if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\\', ':', '\0']) {
            return Err(format!("The add-on holds a file name that is not allowed: {name:?}."));
        }
        Ok(name)
    }

    /// Writes the directory at `entry` (its files, then its subdirectories) below `to`. `budget` bounds the walk against loops.
    fn write(&self, entry: usize, to: &Path, budget: &mut usize) -> Result<(), String> {
        fs::create_dir_all(to).map_err(|e| format!("Cannot create {}: {e}", to.display()))?;
        let dir = self.dirs.0 + entry;
        let mut file = u32_at(&self.img, dir + 12)?;
        while file != u32::MAX {
            *budget = budget.checked_sub(1).ok_or_else(damaged)?;
            let at = self.files.0 + file as usize;
            let (off, size) = (u64_at(&self.img, at + 8)? as usize, u64_at(&self.img, at + 16)? as usize);
            let bytes = self.data.checked_add(off).and_then(|s| self.img.get(s..s.checked_add(size)?)).ok_or_else(damaged)?;
            let name = self.name(self.files, file as usize, 0x20)?;
            File::create(to.join(&name)).and_then(|mut f| f.write_all(bytes)).map_err(|e| format!("Cannot write {name}: {e}"))?;
            file = u32_at(&self.img, at + 4)?;
        }
        let mut sub = u32_at(&self.img, dir + 8)?;
        while sub != u32::MAX {
            *budget = budget.checked_sub(1).ok_or_else(damaged)?;
            let name = self.name(self.dirs, sub as usize, 0x18)?;
            self.write(sub as usize, &to.join(name), budget)?;
            sub = u32_at(&self.img, self.dirs.0 + sub as usize + 4)?;
        }
        Ok(())
    }
}

/// Unpacks every add-on of `nsp` into `<aoc>/<ID>/romfs` and returns their ids (16 hex digits). An add-on already there is
/// replaced; it is built in `romfs.part` first, so a failure leaves the old one alone.
pub fn extract(nsp: &Path, aoc: &Path) -> Result<Vec<String>, String> {
    let mut f = File::open(nsp).map_err(|e| format!("Cannot open {}: {e}", nsp.display()))?;
    let mut done = Vec::new();
    for nca in pfs0(&mut f)?.iter().filter(|e| e.name.ends_with(".nca") && !e.name.ends_with(".cnmt.nca")) {
        let mut h = vec![0u8; NCA_HEADER];
        if nca.size < NCA_HEADER as u64 {
            continue;
        }
        let mut r = BufReader::new(&mut f);
        r.seek(SeekFrom::Start(nca.offset)).and_then(|_| r.read_exact(&mut h)).map_err(|_| damaged())?;
        let Some(a) = addon_romfs(nca, &h)? else { continue };
        if a.size > 1 << 30 {
            return Err(format!("Add-on {:016X} is larger than this launcher unpacks (1 GB).", a.id));
        }
        let mut img = vec![0u8; a.size as usize];
        r.seek(SeekFrom::Start(a.start)).and_then(|_| r.read_exact(&mut img)).map_err(|_| damaged())?;
        let base = aoc.join(format!("{:016X}", a.id));
        let part = base.join("romfs.part");
        let _ = fs::remove_dir_all(&part);
        let mut budget = 1 << 20;
        if let Err(e) = Romfs::new(img).and_then(|rom| rom.write(0, &part, &mut budget)) {
            let _ = fs::remove_dir_all(&part);
            return Err(e);
        }
        let fin = base.join("romfs");
        let _ = fs::remove_dir_all(&fin);
        fs::rename(&part, &fin).map_err(|e| format!("Cannot place {}: {e}", fin.display()))?;
        done.push(format!("{:016X}", a.id));
    }
    if done.is_empty() {
        return Err("This .nsp holds no add-on content for this game.".into());
    }
    Ok(done)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    /// A RomFS image with `a.txt` at the root and `sub/b.bin`.
    fn romfs_image() -> Vec<u8> {
        let (a, b) = (b"hello".as_slice(), b"\x01\x02\x03".as_slice());
        let dir = |parent: u32, sib: i32, child: i32, file: i32, name: &str| {
            let mut d = Vec::new();
            for v in [parent as i32, sib, child, file, -1] {
                d.extend(v.to_le_bytes());
            }
            d.extend((name.len() as u32).to_le_bytes());
            d.extend(name.as_bytes());
            d.resize(d.len().div_ceil(4) * 4, 0);
            d
        };
        let file = |sib: i32, off: i64, size: i64, name: &str| {
            let mut d = Vec::new();
            d.extend(0i32.to_le_bytes());
            d.extend(sib.to_le_bytes());
            d.extend(off.to_le_bytes());
            d.extend(size.to_le_bytes());
            d.extend((-1i32).to_le_bytes());
            d.extend((name.len() as u32).to_le_bytes());
            d.extend(name.as_bytes());
            d.resize(d.len().div_ceil(4) * 4, 0);
            d
        };
        let root = dir(0, -1, 0x18, 0, "");
        let f1 = file(-1, 0, a.len() as i64, "a.txt");
        let f2 = file(-1, 8, b.len() as i64, "b.bin");
        let sub = dir(0, -1, -1, f1.len() as i32, "sub");
        let mut dirs = root;
        dirs.extend(&sub);
        let mut files = f1;
        files.extend(&f2);
        let mut img = vec![0u8; 0x50];
        let (dir_off, file_off) = (0x50u64, 0x50 + dirs.len() as u64);
        let data = file_off + files.len() as u64;
        for (i, v) in [0x50, 0, 0, dir_off, dirs.len() as u64, 0, 0, file_off, files.len() as u64, data].iter().enumerate() {
            img[i * 8..i * 8 + 8].copy_from_slice(&v.to_le_bytes());
        }
        img.extend(dirs);
        img.extend(files);
        img.extend(a);
        img.resize(data as usize + 8, 0);
        img.extend(b);
        img
    }

    /// An .nsp holding one add-on NCA (decrypted, IVFC with the data as the only real level) and a metadata NCA.
    fn nsp(id: u64, crypt: u8, content: u8) -> Vec<u8> {
        let img = romfs_image();
        let mut nca = vec![0u8; NCA_HEADER];
        nca[0x200..0x204].copy_from_slice(b"NCA3");
        nca[0x205] = content;
        nca[0x210..0x218].copy_from_slice(&id.to_le_bytes());
        let end = (NCA_HEADER as u64 + img.len() as u64).div_ceil(MEDIA) as u32;
        nca[0x240..0x244].copy_from_slice(&((NCA_HEADER as u64 / MEDIA) as u32).to_le_bytes());
        nca[0x244..0x248].copy_from_slice(&end.to_le_bytes());
        let fs = &mut nca[0x400..0x600];
        fs[3] = 3;
        fs[4] = crypt;
        fs[8..12].copy_from_slice(b"IVFC");
        fs[0x14..0x18].copy_from_slice(&3u32.to_le_bytes());   // 3 levels listed as 2: the data is the last
        fs[0x18 + 24..0x18 + 32].copy_from_slice(&0u64.to_le_bytes());
        fs[0x18 + 32..0x18 + 40].copy_from_slice(&(img.len() as u64).to_le_bytes());
        nca.extend(&img);
        nca.resize((end as u64 * MEDIA) as usize, 0);
        let (n1, n2) = ("aa.nca", "bb.cnmt.nca");
        let names = format!("{n1}\0{n2}\0");
        let mut out = b"PFS0".to_vec();
        out.extend(2u32.to_le_bytes());
        out.extend((names.len() as u32).to_le_bytes());
        out.extend(0u32.to_le_bytes());
        out.extend(0u64.to_le_bytes());
        out.extend((nca.len() as u64).to_le_bytes());
        out.extend(0u32.to_le_bytes());
        out.extend(0u32.to_le_bytes());
        out.extend((nca.len() as u64).to_le_bytes());
        out.extend(16u64.to_le_bytes());
        out.extend((n1.len() as u32 + 1).to_le_bytes());
        out.extend(0u32.to_le_bytes());
        out.extend(names.as_bytes());
        out.extend(&nca);
        out.extend([0u8; 16]);
        out
    }

    fn run(bytes: Vec<u8>) -> (Result<Vec<String>, String>, PathBuf) {
        let dir = std::env::temp_dir().join(format!("pkp-nsp-{}-{:?}", std::process::id(), std::thread::current().id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("x.nsp"), bytes).unwrap();
        (extract(&dir.join("x.nsp"), &dir.join("aoc")), dir)
    }

    #[test]
    fn unpacks_the_add_on_romfs() {
        let (res, dir) = run(nsp(0x0400A3A01EE4D001, 1, CONTENT_DATA));
        assert_eq!(res.unwrap(), vec!["0400A3A01EE4D001"]);
        let root = dir.join("aoc/0400A3A01EE4D001/romfs");
        assert_eq!(fs::read(root.join("a.txt")).unwrap(), b"hello");
        assert_eq!(fs::read(root.join("sub/b.bin")).unwrap(), [1, 2, 3]);
        assert!(!root.with_file_name("romfs.part").exists());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn refuses_encrypted_other_games_and_non_add_ons() {
        let (e, d1) = run(nsp(0x0400A3A01EE4D001, 3, CONTENT_DATA));
        assert!(e.unwrap_err().contains("encrypted"));
        let (e, d2) = run(nsp(0x0100000000001000, 1, CONTENT_DATA));
        assert!(e.unwrap_err().contains("not for this game"));
        let (e, d3) = run(nsp(0x0400A3A01EE4D001, 1, 0));
        assert!(e.unwrap_err().contains("no add-on content"));
        let (e, d4) = run(b"not an nsp at all, really".to_vec());
        assert!(e.is_err());
        let (e, d5) = run(nsp(0x0400A3A01EE4D001, 1, CONTENT_DATA)[..200].to_vec());
        assert!(e.unwrap_err().contains("damaged"));
        for d in [d1, d2, d3, d4, d5] {
            let _ = fs::remove_dir_all(d);
        }
    }

    #[test]
    fn a_name_that_leaves_the_folder_is_refused() {
        let mut bytes = nsp(0x0400A3A01EE4D001, 1, CONTENT_DATA);
        let at = bytes.windows(5).position(|w| w == b"a.txt").unwrap();
        bytes[at..at + 5].copy_from_slice(b"..\\x\0"[..5].as_ref());
        let (e, d) = run(bytes);
        assert!(e.unwrap_err().contains("not allowed"));
        let _ = fs::remove_dir_all(d);
    }
}
