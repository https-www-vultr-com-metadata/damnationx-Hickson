//! Owned-data portal smoke: portal_smoke GAME CACHE HELPER OUTPUT
use d2r_converter::{storage::NativeStorage, vfx};
use std::{env, fs, path::Path};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let a: Vec<_> = env::args_os().collect();
    if a.len() != 5 {
        return Err("usage: portal_smoke GAME CACHE HELPER OUTPUT".into());
    }
    let mut s = NativeStorage::new(&a[1], &a[2], &a[3])?;
    let output = vfx::build_town_portal(&mut s, "owned-smoke")?;
    for (path, bytes) in &output.files {
        let dest = Path::new(&a[4]).join(path);
        fs::create_dir_all(dest.parent().unwrap())?;
        fs::write(dest, bytes)?;
    }
    println!(
        "{} output files, {} meshes, {} textures",
        output.files.len(),
        output.manifest["meshes"].as_object().unwrap().len(),
        output.manifest["textures"].as_object().unwrap().len()
    );
    Ok(())
}
