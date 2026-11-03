//! Owned-data material height smoke: height_smoke GAME CACHE HELPER
use d2r_converter::{environment, storage::NativeStorage};
use serde_json::json;
use std::env;
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let a: Vec<_> = env::args_os().collect();
    if a.len() != 4 {
        return Err("usage: height_smoke GAME CACHE HELPER".into());
    }
    let mut s = NativeStorage::new(&a[1], &a[2], &a[3])?;
    let spec = json!({"albedo":"data/hd/env/texture/act1/monastry/act1_monastry_architecture/act1_monastry_architecture_basic_wall01_c_alb.texture","height":{"source":"albedo_alpha","source_polarity":"white_raised","depth_scale_m":0.03,"validated":true}});
    let (entry, files) = environment::convert_material(&mut s, &spec, 1024)?;
    let path = entry["height"].as_str().ok_or("missing height")?;
    let mut reader = png::Decoder::new(files[path].as_slice()).read_info()?;
    let mut data = vec![0; reader.output_buffer_size()];
    let info = reader.next_frame(&mut data)?;
    if info.color_type != png::ColorType::Grayscale || info.width > 512 || info.height > 512 {
        return Err("invalid height PNG".into());
    }
    println!(
        "{}x{} grayscale, contract {}",
        info.width, info.height, entry["height_contract"]
    );
    Ok(())
}
