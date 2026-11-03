//! Owned-data cathedral smoke: cathedral_smoke GAME CACHE HELPER OUTPUT
use d2r_converter::{cathedral, import_model, storage::NativeStorage};
use std::{env, fs};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let a: Vec<_> = env::args_os().collect();
    if a.len() != 5 {
        return Err("usage: cathedral_smoke GAME CACHE HELPER OUTPUT".into());
    }
    let mut storage = NativeStorage::new(&a[1], &a[2], &a[3])?;
    let out = cathedral::build(1, |path, usage| {
        import_model::png(&mut storage, path, usage)
    })?;
    fs::write(&a[4], out.glb)?;
    println!(
        "{} triangles, {} sockets, bounds {:?}..{:?}",
        out.triangles,
        out.sockets.len(),
        out.bounds_min,
        out.bounds_max
    );
    Ok(())
}
