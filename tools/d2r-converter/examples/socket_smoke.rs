//! Owned-export smoke: socket_smoke INPUT.glb
use d2r_converter::sockets;
use std::{env, fs};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let p = env::args_os()
        .nth(1)
        .ok_or("usage: socket_smoke INPUT.glb")?;
    let bytes = fs::read(p)?;
    let s = sockets::facade_sockets(&bytes)?;
    println!("{}", serde_json::to_string_pretty(&s)?);
    Ok(())
}
