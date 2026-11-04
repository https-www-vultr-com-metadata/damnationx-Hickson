//! Native development entry point for the in-progress Rust converter.
use d2r_converter::{
    import_model, import_model::AssetAccess, mapping, storage::NativeStorage, ui, variants, vfx,
};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

fn resumable_model(directory: &Path, spec: &mapping::ModelSpec) -> Option<serde_json::Value> {
    use std::io::Read;
    let sidecar: serde_json::Value =
        serde_json::from_slice(&std::fs::read(directory.join(format!("{}.json", spec.key))).ok()?)
            .ok()?;
    let mut header = [0u8; 12];
    let mut glb = std::fs::File::open(directory.join(format!("{}.glb", spec.key))).ok()?;
    glb.read_exact(&mut header).ok()?;
    if &header[0..4] != b"glTF"
        || u32::from_le_bytes(header[4..8].try_into().ok()?) != 2
        || u64::from(u32::from_le_bytes(header[8..12].try_into().ok()?))
            != glb.metadata().ok()?.len()
    {
        return None;
    }
    let variants = sidecar.get("variants")?.as_object()?;
    for variant in &spec.variants {
        for path in variants.get(variant)?.as_object()?.values() {
            let path = path.as_str()?;
            if path.split('/').any(|part| part == ".." || part.is_empty())
                || !directory.join(path).is_file()
            {
                return None;
            }
        }
    }
    Some(sidecar)
}

fn read_recipe(path: &str) -> Result<String, String> {
    std::fs::read_to_string(path).map_err(|e| format!("Cannot read recipe {path}: {e}"))
}

fn create_dir(path: &Path) -> Result<(), String> {
    std::fs::create_dir_all(path)
        .map_err(|e| format!("Cannot create folder {}: {e}", path.display()))
}

/// Writes one converted file, creating its folder. Errors name the file, so a
/// full disk or a blocked folder is reported as such rather than a bare OS error.
fn write_output(path: &Path, bytes: impl AsRef<[u8]>) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        create_dir(parent)?;
    }
    std::fs::write(path, bytes).map_err(|e| format!("Cannot write {}: {e}", path.display()))
}

fn json(value: &serde_json::Value) -> Result<Vec<u8>, String> {
    serde_json::to_vec_pretty(value).map_err(|e| e.to_string())
}

/// Arguments as UTF-8. A path Windows cannot express in UTF-8 is reported
/// instead of panicking (which would exit with 101 and no explanation).
fn arguments() -> Result<Vec<String>, String> {
    std::env::args_os()
        .skip(1)
        .enumerate()
        .map(|(index, arg)| {
            arg.into_string().map_err(|arg| {
                format!(
                    "Argument {} is not valid Unicode: {}",
                    index + 1,
                    arg.to_string_lossy()
                )
            })
        })
        .collect()
}

/// The single line the launcher shows: every failure exits 1 after printing it.
fn error_line(command: Option<&str>, error: &str) -> String {
    let text = error.split_whitespace().collect::<Vec<_>>().join(" ");
    match command {
        Some(command) => format!("d2r-convert: error: {command}: {text}"),
        None => format!("d2r-convert: error: {text}"),
    }
}

fn run(args: &[String]) -> Result<(), String> {
    if !((args.len() == 5 && ["convert", "mapping-all", "environment"].contains(&args[0].as_str()))
        || (args.len() == 6 && args[0] == "mapping")
        || (args.len() == 4 && ["ui", "portal"].contains(&args[0].as_str())))
    {
        return Err("Unrecognised arguments. Usage: d2r-convert convert GAME_DIR CACHE_DIR D2RTOOL UNIT_JSON | mapping GAME_DIR CACHE_DIR D2RTOOL MAPPING_TOML MODEL_KEY | mapping-all GAME_DIR CACHE_DIR D2RTOOL MAPPING_TOML | environment GAME_DIR CACHE_DIR D2RTOOL ENV_TOML | ui|portal GAME_DIR CACHE_DIR D2RTOOL".into());
    }
    let cache = PathBuf::from(&args[2]);
    let mut storage = NativeStorage::new(&args[1], &cache, &args[3])?;
    if args[0] == "environment" {
        let recipe = read_recipe(&args[4])?;
        let build = storage.build_version()?;
        let dir = cache.join("glb");
        let manifest = d2r_converter::environment::build(
            &mut storage,
            &recipe,
            &build,
            1024,
            |path, bytes| {
                let relative = Path::new(path);
                if relative
                    .components()
                    .any(|c| !matches!(c, std::path::Component::Normal(_)))
                {
                    return Err(format!("Invalid output path {path}"));
                }
                let target = dir.join(relative);
                write_output(&target, bytes)
            },
        )?;
        let dest = dir.join("env/env.resolved.json");
        write_output(&dest, json(&manifest)?)?;
        return Ok(());
    }
    if args[0] == "mapping-all" {
        let recipe = read_recipe(&args[4])?;
        let specs = mapping::plan(&recipe)?;
        let directory = cache.join("glb");
        create_dir(&directory)?;
        if directory.join("mapping.resolved.json").exists() {
            return Err(
                "Refusing to overwrite an existing converted mapping; clear the D2R import in the launcher and import again"
                    .into(),
            );
        }
        let identity = format!(
            "{:x}",
            Sha256::digest(
                format!(
                    "{}\n{}\n{}",
                    storage.build_version()?,
                    d2r_converter::texture::OUTPUT_FORMAT,
                    recipe
                )
                .as_bytes(),
            )
        );
        let identity_path = cache.join(".rust-mapping-identity");
        let resume = match std::fs::read_to_string(&identity_path) {
            Ok(previous) if previous != identity => {
                let has_models = std::fs::read_dir(&directory)
                    .map_err(|e| format!("Cannot list {}: {e}", directory.display()))?
                    .filter_map(Result::ok)
                    .any(|entry| {
                        entry.path().is_file()
                            && matches!(
                                entry.path().extension().and_then(|e| e.to_str()),
                                Some("glb" | "json")
                            )
                    });
                if has_models {
                    return Err("Mapping work in the cache belongs to another recipe or game build; clear the D2R import in the launcher and import again".into());
                }
                write_output(&identity_path, identity.as_bytes())?;
                false
            }
            Ok(_) => true,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                write_output(&identity_path, identity.as_bytes())?;
                false
            }
            Err(e) => return Err(format!("Cannot read {}: {e}", identity_path.display())),
        };
        let mut metadata = BTreeMap::new();
        for (index, spec) in specs.iter().enumerate() {
            if !spec
                .key
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')
            {
                return Err(format!("Unsafe model key {}", spec.key));
            }
            eprintln!("[{}/{}] Converting {}", index + 1, specs.len(), spec.key);
            if let Some(sidecar) = resume.then(|| resumable_model(&directory, spec)).flatten() {
                eprintln!("Reusing complete model {}", spec.key);
                metadata.insert(spec.key.clone(), sidecar);
                continue;
            }
            let result = mapping::convert_spec(&mut storage, spec)
                .map_err(|e| format!("{}: {e}", spec.key))?;
            for (name, bytes) in result.files {
                let path = directory.join(name);
                write_output(&path, bytes)?;
            }
            write_output(&directory.join(format!("{}.glb", spec.key)), result.glb)?;
            write_output(
                &directory.join(format!("{}.json", spec.key)),
                json(&result.sidecar)?,
            )?;
            metadata.insert(spec.key.clone(), result.sidecar);
        }
        let (resolved, problems) =
            mapping::resolve(&recipe, &directory.to_string_lossy(), &metadata)?;
        for problem in &problems {
            eprintln!("mapping: {problem}");
        }
        write_output(&directory.join("mapping.resolved.json"), json(&resolved)?)?;
        return Ok(());
    }
    if args[0] == "ui" || args[0] == "portal" {
        let build = storage.build_version()?;
        let files = if args[0] == "ui" {
            ui::extract_hud(&mut storage, &build)?
                .into_iter()
                .map(|(p, b)| (format!("ui/{p}"), b))
                .collect()
        } else {
            vfx::build_town_portal(&mut storage, &build)?.files
        };
        for (path, bytes) in files {
            let destination = cache.join("glb").join(path);
            write_output(&destination, bytes)?;
        }
        return Ok(());
    }
    let (path, stem, parts, all_variants, clips, dyes, attachments, variant_names, sources) =
        if args[0] == "mapping" {
            let recipe = read_recipe(&args[4])?;
            let spec = mapping::plan(&recipe)?
                .into_iter()
                .find(|s| s.key == args[5])
                .ok_or("Mapping key is not referenced by the recipe")?;
            let path = mapping::unit_path(&spec.unit, &AssetAccess::listing(&mut storage)?)?;
            (
                path,
                spec.key,
                spec.parts,
                spec.all_variants,
                spec.clips,
                spec.dye,
                spec.attachments,
                spec.variants,
                spec.animations,
            )
        } else {
            let path = args[4].clone();
            let stem = Path::new(&path)
                .file_stem()
                .and_then(|s| s.to_str())
                .ok_or("Invalid unit filename")?
                .to_owned();
            (
                path,
                stem,
                BTreeMap::new(),
                false,
                Vec::new(),
                BTreeMap::new(),
                BTreeMap::new(),
                BTreeSet::new(),
                Vec::new(),
            )
        };
    let output = import_model::convert_recipe_with_sources(
        &mut storage,
        &path,
        &parts,
        all_variants,
        &clips,
        &dyes,
        &attachments,
        &sources,
        1024,
    )?;
    let baked = variants::bake_with_limit(
        &mut storage,
        &path,
        &stem,
        &parts,
        all_variants,
        &variant_names,
        1024,
    )?;
    let destination = cache.join("glb").join(format!("{stem}.glb"));
    write_output(&destination, &output.glb)?;
    for (name, bytes) in baked.files {
        let target = cache.join("glb").join(name);
        write_output(&target, bytes)?;
    }
    let sidecar = serde_json::json!({"source":path,"meshes":output.meshes,"bones":output.bones,"clips":output.clips,"variants":baked.variants});
    write_output(&destination.with_extension("json"), json(&sidecar)?)?;
    eprintln!(
        "Converted {path}: {} bones, {} meshes",
        output.bones, output.meshes
    );
    Ok(())
}

fn main() {
    let line = match arguments() {
        Err(error) => error_line(None, &error),
        Ok(args) => match run(&args) {
            Ok(()) => return,
            Err(error) => error_line(args.first().map(String::as_str), &error),
        },
    };
    eprintln!("{line}");
    std::process::exit(1);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn errors_print_as_one_prefixed_line() {
        assert_eq!(
            error_line(Some("mapping-all"), "hero: Cannot write x:\n  disk full"),
            "d2r-convert: error: mapping-all: hero: Cannot write x: disk full"
        );
        assert_eq!(error_line(None, "bad"), "d2r-convert: error: bad");
    }
    #[test]
    fn resume_requires_complete_glb_and_every_variant_file() {
        let temp = std::env::temp_dir().join(format!(
            "d2r-resume-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&temp).unwrap();
        let spec = mapping::ModelSpec {
            key: "hero".into(),
            unit: "hero".into(),
            parts: BTreeMap::new(),
            clips: vec![],
            all_variants: false,
            variants: BTreeSet::from(["red".into()]),
            attachments: BTreeMap::new(),
            dye: BTreeMap::new(),
            animations: Vec::new(),
        };
        let sidecar = serde_json::json!({"variants":{"red":{"material":"variants/red.png"}}});
        std::fs::write(
            temp.join("hero.json"),
            serde_json::to_vec(&sidecar).unwrap(),
        )
        .unwrap();
        std::fs::write(
            temp.join("hero.glb"),
            [b'g', b'l', b'T', b'F', 2, 0, 0, 0, 12, 0, 0, 0],
        )
        .unwrap();
        assert!(resumable_model(&temp, &spec).is_none());
        std::fs::create_dir_all(temp.join("variants")).unwrap();
        std::fs::write(temp.join("variants/red.png"), b"png").unwrap();
        assert!(resumable_model(&temp, &spec).is_some());
        std::fs::write(temp.join("hero.glb"), b"truncated").unwrap();
        assert!(resumable_model(&temp, &spec).is_none());
        std::fs::remove_dir_all(temp).unwrap();
    }
}
