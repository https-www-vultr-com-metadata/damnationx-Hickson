//! Adds the qualified Rust converter to an existing launcher archive, updating
//! its manifest and checksum without invoking Python. Source archive is only
//! replaced after a complete new archive has been written.
use flate2::{read::GzDecoder, Compression, GzBuilder};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{self, Read, Write},
    path::{Component, Path, PathBuf},
};
use zip::{write::SimpleFileOptions, ZipArchive, ZipWriter};

fn safe(path: &Path) -> bool {
    path.components().all(|c| matches!(c, Component::Normal(_)))
}
fn digest_file(path: &Path) -> io::Result<String> {
    let mut file = fs::File::open(path)?;
    let mut hash = Sha256::new();
    let mut block = [0u8; 65536];
    loop {
        let n = file.read(&mut block)?;
        if n == 0 {
            break;
        }
        hash.update(&block[..n]);
    }
    Ok(format!("{:x}", hash.finalize()))
}
fn collect(dir: &Path, root: &Path, out: &mut Vec<PathBuf>) -> io::Result<()> {
    for ent in fs::read_dir(dir)? {
        let ent = ent?;
        let ty = ent.file_type()?;
        let path = ent.path();
        if ty.is_dir() {
            collect(&path, root, out)?;
        } else if ty.is_file() {
            out.push(path.strip_prefix(root).unwrap().to_owned());
        } else {
            return Err(io::Error::other("unexpected special file"));
        }
    }
    Ok(())
}
fn run(archive: &Path, converter: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let zip = archive.extension().is_some_and(|e| e == "zip");
    if !zip && !archive.to_string_lossy().ends_with(".tar.gz") {
        return Err("expected .zip or .tar.gz".into());
    }
    if !converter.is_file() {
        return Err("converter executable missing".into());
    }
    let parent = archive
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let staging = tempfile::tempdir_in(parent)?;
    let mut modes = std::collections::BTreeMap::<PathBuf, u32>::new();
    if zip {
        let mut source = ZipArchive::new(fs::File::open(archive)?)?;
        for i in 0..source.len() {
            let mut entry = source.by_index(i)?;
            let path = entry.enclosed_name().ok_or("unsafe ZIP path")?;
            if !safe(&path) || entry.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000) {
                return Err("unsafe ZIP entry".into());
            }
            if !entry.is_dir() {
                modes.insert(path.clone(), entry.unix_mode().unwrap_or(0o644) & 0o777);
            }
            let dest = staging.path().join(path);
            if entry.is_dir() {
                fs::create_dir_all(dest)?;
            } else {
                fs::create_dir_all(dest.parent().unwrap())?;
                io::copy(&mut entry, &mut fs::File::create(dest)?)?;
            }
        }
    } else {
        let mut source = tar::Archive::new(GzDecoder::new(fs::File::open(archive)?));
        for entry in source.entries()? {
            let mut entry = entry?;
            let kind = entry.header().entry_type();
            let path = entry.path()?.into_owned();
            if !safe(&path) || !(kind.is_file() || kind.is_dir()) {
                return Err("unsafe tar entry".into());
            }
            if kind.is_file() {
                modes.insert(path, entry.header().mode()? & 0o777);
            }
            if !entry.unpack_in(staging.path())? {
                return Err("unsafe tar path".into());
            }
        }
    }
    let mut roots = fs::read_dir(staging.path())?.collect::<Result<Vec<_>, _>>()?;
    if roots.len() != 1 || !roots[0].file_type()?.is_dir() {
        return Err("expected one package root".into());
    }
    let root = roots.remove(0).path();
    let manifest_path = root.join("PACKAGE.json");
    let mut manifest: serde_json::Value = serde_json::from_slice(&fs::read(&manifest_path)?)?;
    let suffix = if zip { ".exe" } else { "" };
    for required in [
        "resources/d2r-import/mapping.toml".to_string(),
        "resources/d2r-import/env_mapping.toml".to_string(),
        format!("resources/d2r-import/bin/d2rtool{suffix}"),
    ] {
        if !root.join(required).is_file() {
            return Err("package is missing a required converter recipe or native helper".into());
        }
    }
    for (name, expected) in manifest["files_sha256"]
        .as_object()
        .ok_or("package has no file checksums")?
    {
        let path = Path::new(name);
        if !safe(path) || expected.as_str() != Some(digest_file(&root.join(path))?.as_str()) {
            return Err(format!("package checksum mismatch: {name}").into());
        }
    }
    let relative = format!("resources/d2r-import/bin/d2r-convert{suffix}");
    let dest = root.join(&relative);
    fs::create_dir_all(dest.parent().unwrap())?;
    fs::copy(converter, &dest)?;
    let converter_hash = digest_file(&dest)?;
    manifest["files_sha256"][&relative] = serde_json::Value::String(converter_hash);
    manifest["native_rust_converter"] = serde_json::Value::Bool(true);
    if let Some(required) = manifest
        .get_mut("required_resources")
        .and_then(serde_json::Value::as_array_mut)
    {
        if !required
            .iter()
            .any(|value| value.as_str() == Some(relative.as_str()))
        {
            required.push(serde_json::Value::String(relative.clone()));
        }
    }
    let mut data = serde_json::to_vec_pretty(&manifest)?;
    data.push(b'\n');
    fs::write(manifest_path, data)?;
    let mut files = Vec::new();
    collect(&root, &root, &mut files)?;
    files.sort();
    let root_name = root
        .file_name()
        .ok_or("invalid package name")?
        .to_string_lossy()
        .into_owned();
    let mut temp = tempfile::NamedTempFile::new_in(parent)?;
    if zip {
        let mut writer = ZipWriter::new(temp.as_file_mut());
        for path in &files {
            let rel = path.to_string_lossy().replace('\\', "/");
            let mode = if rel == relative {
                0o755
            } else {
                *modes
                    .get(&PathBuf::from(&root_name).join(path))
                    .unwrap_or(&0o644)
            };
            let options = SimpleFileOptions::default()
                .compression_method(zip::CompressionMethod::Deflated)
                .unix_permissions(mode);
            writer.start_file(format!("{root_name}/{rel}"), options)?;
            io::copy(&mut fs::File::open(root.join(path))?, &mut writer)?;
        }
        writer.finish()?;
    } else {
        let encoder = GzBuilder::new()
            .mtime(0)
            .write(temp.as_file_mut(), Compression::default());
        let mut writer = tar::Builder::new(encoder);
        for path in &files {
            let rel = path.to_string_lossy().replace('\\', "/");
            let mode = if rel == relative {
                0o755
            } else {
                *modes
                    .get(&PathBuf::from(&root_name).join(path))
                    .unwrap_or(&0o644)
            };
            let mut file = fs::File::open(root.join(path))?;
            let mut header = tar::Header::new_gnu();
            header.set_size(file.metadata()?.len());
            header.set_mode(mode);
            header.set_uid(0);
            header.set_gid(0);
            header.set_mtime(0);
            header.set_cksum();
            writer.append_data(&mut header, format!("{root_name}/{rel}"), &mut file)?;
        }
        writer.into_inner()?.finish()?;
    }
    temp.as_file_mut().flush()?;
    temp.as_file().sync_all()?;
    let mut hash = Sha256::new();
    let mut file = fs::File::open(temp.path())?;
    let mut block = [0; 65536];
    loop {
        let n = file.read(&mut block)?;
        if n == 0 {
            break;
        }
        hash.update(&block[..n]);
    }
    let checksum = format!(
        "{:x}  {}\n",
        hash.finalize(),
        archive.file_name().unwrap().to_string_lossy()
    );
    temp.persist(archive).map_err(|e| e.error)?;
    fs::write(format!("{}.sha256", archive.display()), checksum)?;
    Ok(())
}
fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() != 2 {
        eprintln!("usage: d2r-packager ARCHIVE CONVERTER");
        std::process::exit(2);
    }
    if let Err(e) = run(Path::new(&args[0]), Path::new(&args[1])) {
        eprintln!("{e}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn adds_converter_to_both_archive_formats_and_updates_manifest_and_checksum() {
        for is_zip in [false, true] {
            let temp = tempfile::tempdir().unwrap();
            let archive = temp.path().join(if is_zip {
                "launcher.zip"
            } else {
                "launcher.tar.gz"
            });
            let converter = temp.path().join("converter");
            fs::write(&converter, b"test-converter").unwrap();
            let manifest = br#"{"required_resources":["existing"],"files_sha256":{}}"#;
            let helper = format!(
                "package/resources/d2r-import/bin/d2rtool{}",
                if is_zip { ".exe" } else { "" }
            );
            let files = [
                ("package/PACKAGE.json", manifest.as_slice()),
                (
                    "package/resources/d2r-import/mapping.toml",
                    b"recipe".as_slice(),
                ),
                (
                    "package/resources/d2r-import/env_mapping.toml",
                    b"recipe".as_slice(),
                ),
                (helper.as_str(), b"helper".as_slice()),
                ("package/launcher-bin", b"binary".as_slice()),
            ];
            if is_zip {
                let mut w = ZipWriter::new(fs::File::create(&archive).unwrap());
                for (path, bytes) in files {
                    w.start_file(
                        path,
                        SimpleFileOptions::default().unix_permissions(
                            if path.ends_with("launcher-bin") {
                                0o755
                            } else {
                                0o644
                            },
                        ),
                    )
                    .unwrap();
                    w.write_all(bytes).unwrap();
                }
                w.finish().unwrap();
            } else {
                let gz = GzBuilder::new()
                    .mtime(0)
                    .write(fs::File::create(&archive).unwrap(), Compression::default());
                let mut w = tar::Builder::new(gz);
                for (path, bytes) in files {
                    let mut h = tar::Header::new_gnu();
                    h.set_size(bytes.len() as u64);
                    h.set_mode(if path.ends_with("launcher-bin") {
                        0o755
                    } else {
                        0o644
                    });
                    h.set_cksum();
                    w.append_data(&mut h, path, bytes).unwrap();
                }
                w.into_inner().unwrap().finish().unwrap();
            }
            run(&archive, &converter).unwrap();
            let mut found = std::collections::BTreeMap::new();
            let mut mode = 0;
            let mut existing_mode = 0;
            if is_zip {
                let mut z = ZipArchive::new(fs::File::open(&archive).unwrap()).unwrap();
                for i in 0..z.len() {
                    let mut e = z.by_index(i).unwrap();
                    let mut b = Vec::new();
                    e.read_to_end(&mut b).unwrap();
                    if e.name().contains("d2r-convert") {
                        mode = e.unix_mode().unwrap();
                    }
                    if e.name().ends_with("launcher-bin") {
                        existing_mode = e.unix_mode().unwrap();
                    }
                    found.insert(e.name().to_owned(), b);
                }
            } else {
                let mut a = tar::Archive::new(GzDecoder::new(fs::File::open(&archive).unwrap()));
                for e in a.entries().unwrap() {
                    let mut e = e.unwrap();
                    let p = e.path().unwrap().to_string_lossy().into_owned();
                    if p.contains("d2r-convert") {
                        mode = e.header().mode().unwrap();
                    }
                    if p.ends_with("launcher-bin") {
                        existing_mode = e.header().mode().unwrap();
                    }
                    let mut b = Vec::new();
                    e.read_to_end(&mut b).unwrap();
                    found.insert(p, b);
                }
            }
            let rel = format!(
                "resources/d2r-import/bin/d2r-convert{}",
                if is_zip { ".exe" } else { "" }
            );
            assert_eq!(found[&format!("package/{rel}")], b"test-converter");
            assert_eq!(mode & 0o777, 0o755);
            assert_eq!(existing_mode & 0o777, 0o755);
            let manifest: serde_json::Value =
                serde_json::from_slice(&found["package/PACKAGE.json"]).unwrap();
            assert_eq!(manifest["native_rust_converter"], true);
            assert!(manifest["required_resources"]
                .as_array()
                .unwrap()
                .iter()
                .any(|v| v.as_str() == Some(rel.as_str())));
            assert_eq!(
                manifest["files_sha256"][&rel],
                format!("{:x}", Sha256::digest(b"test-converter"))
            );
            assert!(fs::read_to_string(format!("{}.sha256", archive.display()))
                .unwrap()
                .starts_with(&format!(
                    "{:x}",
                    Sha256::digest(fs::read(&archive).unwrap())
                )));
        }
    }
}
