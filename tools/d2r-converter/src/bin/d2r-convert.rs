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

fn run() -> Result<(), String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if !((args.len() == 5 && ["convert", "mapping-all", "environment"].contains(&args[0].as_str()))
        || (args.len() == 6 && args[0] == "mapping")
        || (args.len() == 4 && ["ui", "portal"].contains(&args[0].as_str())))
    {
        return Err("Usage: d2r-convert convert GAME_DIR CACHE_DIR D2RTOOL UNIT_JSON | mapping GAME_DIR CACHE_DIR D2RTOOL MAPPING_TOML MODEL_KEY | mapping-all GAME_DIR CACHE_DIR D2RTOOL MAPPING_TOML | environment GAME_DIR CACHE_DIR D2RTOOL ENV_TOML | ui|portal GAME_DIR CACHE_DIR D2RTOOL".into());
    }
    let cache = PathBuf::from(&args[2]);
    let mut storage = NativeStorage::new(&args[1], &cache, &args[3])?;
    if args[0] == "environment" {
        let recipe = std::fs::read_to_string(&args[4]).map_err(|e| e.to_string())?;
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
                std::fs::create_dir_all(target.parent().unwrap()).map_err(|e| e.to_string())?;
                std::fs::write(target, bytes).map_err(|e| e.to_string())
            },
        )?;
        let dest = dir.join("env/env.resolved.json");
        std::fs::create_dir_all(dest.parent().unwrap()).map_err(|e| e.to_string())?;
        std::fs::write(
            dest,
            serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
        return Ok(());
    }
    if args[0] == "mapping-all" {
        let recipe = std::fs::read_to_string(&args[4]).map_err(|e| e.to_string())?;
        let specs = mapping::plan(&recipe)?;
        let directory = cache.join("glb");
        std::fs::create_dir_all(&directory).map_err(|e| e.to_string())?;
        if directory.join("mapping.resolved.json").exists() {
            return Err(
                "Refusing to overwrite an existing converted mapping; select a new working cache"
                    .into(),
            );
        }
        let identity = format!(
            "{:x}",
            Sha256::digest(format!("{}\n{}", storage.build_version()?, recipe).as_bytes())
        );
        let identity_path = cache.join(".rust-mapping-identity");
        let resume = match std::fs::read_to_string(&identity_path) {
            Ok(previous) if previous != identity => {
                let has_models = std::fs::read_dir(&directory)
                    .map_err(|e| e.to_string())?
                    .filter_map(Result::ok)
                    .any(|entry| {
                        entry.path().is_file()
                            && matches!(
                                entry.path().extension().and_then(|e| e.to_str()),
                                Some("glb" | "json")
                            )
                    });
                if has_models {
                    return Err("Mapping work belongs to another recipe or game build; select a new working cache".into());
                }
                std::fs::write(&identity_path, identity).map_err(|e| e.to_string())?;
                false
            }
            Ok(_) => true,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                std::fs::write(identity_path, identity).map_err(|e| e.to_string())?;
                false
            }
            Err(e) => return Err(e.to_string()),
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
                std::fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
                std::fs::write(path, bytes).map_err(|e| e.to_string())?;
            }
            std::fs::write(directory.join(format!("{}.glb", spec.key)), result.glb)
                .map_err(|e| e.to_string())?;
            std::fs::write(
                directory.join(format!("{}.json", spec.key)),
                serde_json::to_vec_pretty(&result.sidecar).map_err(|e| e.to_string())?,
            )
            .map_err(|e| e.to_string())?;
            metadata.insert(spec.key.clone(), result.sidecar);
        }
        let (resolved, problems) =
            mapping::resolve(&recipe, &directory.to_string_lossy(), &metadata)?;
        for problem in &problems {
            eprintln!("mapping: {problem}");
        }
        std::fs::write(
            directory.join("mapping.resolved.json"),
            serde_json::to_vec_pretty(&resolved).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
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
            std::fs::create_dir_all(destination.parent().unwrap()).map_err(|e| e.to_string())?;
            std::fs::write(destination, bytes).map_err(|e| e.to_string())?;
        }
        return Ok(());
    }
    let (path, stem, parts, all_variants, clips, dyes, attachments, variant_names, sources) =
        if args[0] == "mapping" {
            let recipe = std::fs::read_to_string(&args[4]).map_err(|e| e.to_string())?;
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
    std::fs::create_dir_all(destination.parent().unwrap()).map_err(|e| e.to_string())?;
    std::fs::write(&destination, &output.glb).map_err(|e| e.to_string())?;
    for (name, bytes) in baked.files {
        let target = cache.join("glb").join(name);
        std::fs::create_dir_all(target.parent().unwrap()).map_err(|e| e.to_string())?;
        std::fs::write(target, bytes).map_err(|e| e.to_string())?;
    }
    let sidecar = serde_json::json!({"source":path,"meshes":output.meshes,"bones":output.bones,"clips":output.clips,"variants":baked.variants});
    std::fs::write(
        destination.with_extension("json"),
        serde_json::to_vec_pretty(&sidecar).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    eprintln!(
        "Converted {path}: {} bones, {} meshes",
        output.bones, output.meshes
    );
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
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
