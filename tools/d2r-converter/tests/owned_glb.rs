//! Optional integrity validation for locally generated and legacy owned GLBs.
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
fn validate(path: &std::path::Path) -> (BTreeMap<String, BTreeSet<String>>, usize) {
    let data = std::fs::read(path).unwrap();
    assert_eq!(&data[..4], b"glTF");
    assert_eq!(
        u32::from_le_bytes(data[8..12].try_into().unwrap()) as usize,
        data.len()
    );
    let len = u32::from_le_bytes(data[12..16].try_into().unwrap()) as usize;
    let doc: Value = serde_json::from_slice(&data[20..20 + len]).unwrap();
    assert_eq!(&data[24 + len..28 + len], b"BIN\0");
    let binary = &data[28 + len..];
    let views = doc["bufferViews"].as_array().unwrap();
    let images = doc["images"].as_array().unwrap();
    let mut hashes = Vec::new();
    let mut by_name: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    for image in images {
        let view = &views[image["bufferView"].as_u64().unwrap() as usize];
        let off = view["byteOffset"].as_u64().unwrap_or(0) as usize;
        let size = view["byteLength"].as_u64().unwrap() as usize;
        let bytes = binary
            .get(off..off + size)
            .expect("image bufferView outside GLB");
        let decoder = png::Decoder::new(bytes);
        let mut reader = decoder.read_info().expect("invalid PNG");
        let mut pixels = vec![0; reader.output_buffer_size()];
        let info = reader.next_frame(&mut pixels).expect("PNG decode failed");
        pixels.truncate(info.buffer_size());
        if info.color_type == png::ColorType::Rgb {
            pixels = pixels
                .chunks_exact(3)
                .flat_map(|p| [p[0], p[1], p[2], 255])
                .collect();
        }
        let hash = format!(
            "{}x{}:{:x}",
            info.width,
            info.height,
            Sha256::digest(&pixels)
        );
        by_name
            .entry(
                image["name"]
                    .as_str()
                    .unwrap_or("")
                    .rsplit('/')
                    .next()
                    .unwrap_or("")
                    .trim_end_matches(".texture")
                    .to_lowercase(),
            )
            .or_default()
            .insert(hash.clone());
        hashes.push(hash);
    }
    let textures = doc["textures"].as_array().unwrap();
    for texture in textures {
        assert!((texture["source"].as_u64().unwrap() as usize) < images.len());
    }
    let materials = doc["materials"].as_array().unwrap();
    for material in materials {
        for field in [
            &material["pbrMetallicRoughness"]["baseColorTexture"],
            &material["pbrMetallicRoughness"]["metallicRoughnessTexture"],
            &material["normalTexture"],
            &material["occlusionTexture"],
            &material["emissiveTexture"],
        ] {
            if let Some(index) = field["index"].as_u64() {
                assert!((index as usize) < textures.len());
            }
        }
    }
    for mesh in doc["meshes"].as_array().unwrap() {
        for primitive in mesh["primitives"].as_array().unwrap() {
            if let Some(index) = primitive["material"].as_u64() {
                assert!((index as usize) < materials.len());
            }
        }
    }
    (by_name, hashes.len())
}
#[test]
fn validate_owned_glb_and_compare_legacy_images() {
    let (Some(new), Some(old)) = (
        std::env::var_os("D2R_GLB_NEW"),
        std::env::var_os("D2R_GLB_OLD"),
    ) else {
        return;
    };
    let (new, new_count) = validate(std::path::Path::new(&new));
    let (old, old_count) = validate(std::path::Path::new(&old));
    let names = new.keys().filter(|n| old.contains_key(*n)).count();
    let equal = new
        .iter()
        .filter(|(n, v)| old.get(*n).is_some_and(|old| old == *v))
        .count();
    eprintln!("validated {new_count} new and {old_count} legacy PNG references; {equal}/{names} shared names decode identically");
    for (name, hashes) in &new {
        if let Some(previous) = old.get(name) {
            if previous != hashes {
                eprintln!("different {name}: new {hashes:?}; old {previous:?}");
            }
        }
    }
}
