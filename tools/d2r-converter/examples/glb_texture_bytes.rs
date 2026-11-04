//! Video memory the textures embedded in GLBs need, as the host uploads them: RGBA8 PNGs with a full mip
//! chain, KTX2 images with the levels they carry. Duplicate images (the host aliases them) count once in the
//! "unique" totals.
//!
//! `cargo run --release --example glb_texture_bytes -- DIR_OR_GLB...`
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

#[derive(Default)]
struct Totals {
    images: usize,
    gpu: u64,
    /// The same images decoded to RGBA8 with a full chain, as the previous PNG imports were uploaded.
    rgba: u64,
    file: u64,
    unique_images: usize,
    unique_gpu: u64,
    png: usize,
    ktx2: usize,
}

fn rgba_chain(mut w: u64, mut h: u64) -> u64 {
    let mut total = 0;
    loop {
        total += w * h * 4;
        if w == 1 && h == 1 {
            return total;
        }
        (w, h) = ((w / 2).max(1), (h / 2).max(1));
    }
}

fn glbs(path: &Path, out: &mut Vec<PathBuf>) {
    if path.is_dir() {
        let mut entries: Vec<_> = std::fs::read_dir(path).unwrap().flatten().map(|e| e.path()).collect();
        entries.sort();
        for entry in entries {
            glbs(&entry, out);
        }
    } else if path.extension().is_some_and(|e| e.eq_ignore_ascii_case("glb")) {
        out.push(path.to_path_buf());
    }
}

fn main() {
    let mut files = Vec::new();
    for arg in std::env::args().skip(1) {
        glbs(Path::new(&arg), &mut files);
    }
    let mut totals = Totals::default();
    let mut seen = BTreeSet::new();
    for file in &files {
        let data = std::fs::read(file).unwrap();
        let json_len = u32::from_le_bytes(data[12..16].try_into().unwrap()) as usize;
        let doc: serde_json::Value = serde_json::from_slice(&data[20..20 + json_len]).unwrap();
        let bin = &data[28 + json_len..];
        for image in doc["images"].as_array().into_iter().flatten() {
            let view = &doc["bufferViews"][image["bufferView"].as_u64().unwrap() as usize];
            let offset = view["byteOffset"].as_u64().unwrap_or(0) as usize;
            let bytes = &bin[offset..offset + view["byteLength"].as_u64().unwrap() as usize];
            let (gpu, rgba) = if d2r_converter::texture::is_ktx2(bytes) {
                totals.ktx2 += 1;
                let w = u32::from_le_bytes(bytes[20..24].try_into().unwrap()) as u64;
                let h = u32::from_le_bytes(bytes[24..28].try_into().unwrap()) as u64;
                let levels = u32::from_le_bytes(bytes[40..44].try_into().unwrap()) as usize;
                let gpu = (0..levels)
                    .map(|level| u64::from_le_bytes(bytes[80 + 24 * level + 8..80 + 24 * level + 16].try_into().unwrap()))
                    .sum::<u64>();
                (gpu, rgba_chain(w, h))
            } else {
                totals.png += 1;
                let w = u32::from_be_bytes(bytes[16..20].try_into().unwrap()) as u64;
                let h = u32::from_be_bytes(bytes[20..24].try_into().unwrap()) as u64;
                (rgba_chain(w, h), rgba_chain(w, h))
            };
            totals.rgba += rgba;
            totals.images += 1;
            totals.gpu += gpu;
            totals.file += bytes.len() as u64;
            if seen.insert(<[u8; 32]>::from(Sha256::digest(bytes))) {
                totals.unique_images += 1;
                totals.unique_gpu += gpu;
            }
        }
    }
    let mib = |b: u64| b as f64 / 1048576.0;
    println!(
        "{} GLBs, {} images ({} PNG, {} KTX2): {:.1} MiB GPU ({:.1} MiB as RGBA8), {:.1} MiB in files; {} unique images: {:.1} MiB GPU",
        files.len(),
        totals.images,
        totals.png,
        totals.ktx2,
        mib(totals.gpu),
        mib(totals.rgba),
        mib(totals.file),
        totals.unique_images,
        mib(totals.unique_gpu)
    );
}
