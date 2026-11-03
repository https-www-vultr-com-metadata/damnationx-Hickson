//! Owned-data focused environment check: environment_spec_smoke GAME CACHE HELPER RECIPE KEY OUTPUT
use d2r_converter::{environment, storage::NativeStorage};
use std::{env, fs};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let a: Vec<_> = env::args_os().collect();
    if a.len() != 7 {
        return Err("usage: environment_spec_smoke GAME CACHE HELPER RECIPE KEY OUTPUT".into());
    }
    let mut s = NativeStorage::new(&a[1], &a[2], &a[3])?;
    let recipe: toml::Value = toml::from_str(&fs::read_to_string(&a[4])?)?;
    let v = serde_json::to_value(recipe)?;
    let key = a[5].to_str().ok_or("bad key")?;
    let model = environment::convert_spec(&mut s, &v["models"][key], 0)?;
    println!(
        "{} triangles, bounds {:?}..{:?}",
        model.triangles, model.bounds_min, model.bounds_max
    );
    fs::write(&a[6], model.glb)?;
    Ok(())
}
