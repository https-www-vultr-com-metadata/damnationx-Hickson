//! Compare decoded RGBA pixels from two PNGs.
use std::{env, fs};
fn decode(p: &std::ffi::OsStr) -> Result<(u32, u32, Vec<u8>), Box<dyn std::error::Error>> {
    let data = fs::read(p)?;
    let mut d = png::Decoder::new(data.as_slice());
    d.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut r = d.read_info()?;
    let mut b = vec![0; r.output_buffer_size()];
    let info = r.next_frame(&mut b)?;
    let mut rgba = Vec::new();
    match info.color_type {
        png::ColorType::Rgba => rgba.extend(&b[..info.buffer_size()]),
        png::ColorType::Rgb => {
            for x in b[..info.buffer_size()].chunks_exact(3) {
                rgba.extend([x[0], x[1], x[2], 255]);
            }
        }
        png::ColorType::Grayscale => {
            for &x in &b[..info.buffer_size()] {
                rgba.extend([x, x, x, 255]);
            }
        }
        png::ColorType::GrayscaleAlpha => {
            for x in b[..info.buffer_size()].chunks_exact(2) {
                rgba.extend([x[0], x[0], x[0], x[1]]);
            }
        }
        _ => return Err("unsupported PNG".into()),
    }
    Ok((info.width, info.height, rgba))
}
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let a: Vec<_> = env::args_os().collect();
    let x = decode(&a[1])?;
    let y = decode(&a[2])?;
    if (x.0, x.1) != (y.0, y.1) {
        return Err("dimensions differ".into());
    }
    let mut max = 0;
    let mut sum = 0u64;
    let mut changed = 0usize;
    for (a, b) in x.2.iter().zip(&y.2) {
        let d = a.abs_diff(*b);
        max = max.max(d);
        sum += u64::from(d);
        changed += usize::from(d != 0);
    }
    println!(
        "{}x{}, changed channels {}, max {}, mean {}",
        x.0,
        x.1,
        changed,
        max,
        sum as f64 / x.2.len() as f64
    );
    Ok(())
}
