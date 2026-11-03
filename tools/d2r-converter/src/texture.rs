//! Decoder for the `<DE(` D2R texture container and its supported BC formats.

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Image {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

/// Resize to a bounded maximum edge while preserving aspect ratio. Zero disables resizing.
pub fn resize(image: Image, limit: u32) -> Result<Image, String> {
    if limit == 0 || image.width.max(image.height) <= limit {
        return Ok(image);
    }
    let width = ((u64::from(image.width) * u64::from(limit)
        / u64::from(image.width.max(image.height))) as u32)
        .max(1);
    let height = ((u64::from(image.height) * u64::from(limit)
        / u64::from(image.width.max(image.height))) as u32)
        .max(1);
    let src = image::RgbaImage::from_raw(image.width, image.height, image.rgba)
        .ok_or("Invalid image dimensions")?;
    let resized =
        image::imageops::resize(&src, width, height, image::imageops::FilterType::Lanczos3);
    Ok(Image {
        width,
        height,
        rgba: resized.into_raw(),
    })
}

fn u32_at(data: &[u8], offset: usize) -> Result<u32, String> {
    let bytes = data
        .get(offset..offset + 4)
        .ok_or("truncated texture header")?;
    Ok(u32::from_le_bytes(bytes.try_into().unwrap()))
}

fn alpha(block: &[u8]) -> [u8; 16] {
    let a = block[0] as u32;
    let b = block[1] as u32;
    let mut palette = [0u8; 8];
    palette[0] = a as u8;
    palette[1] = b as u8;
    if a > b {
        for i in 1..=6 {
            palette[i + 1] = (((7 - i as u32) * a + i as u32 * b) / 7) as u8;
        }
    } else {
        for i in 1..=4 {
            palette[i + 1] = (((5 - i as u32) * a + i as u32 * b) / 5) as u8;
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    let mut bits = 0u64;
    for (i, byte) in block[2..8].iter().enumerate() {
        bits |= (*byte as u64) << (8 * i);
    }
    std::array::from_fn(|i| palette[((bits >> (i * 3)) & 7) as usize])
}

fn color(block: &[u8], transparent: bool) -> [[u8; 4]; 16] {
    let a = u16::from_le_bytes([block[0], block[1]]);
    let b = u16::from_le_bytes([block[2], block[3]]);
    let rgb = |v: u16| -> [u8; 4] {
        let r = ((v >> 11) & 31) as u8;
        let g = ((v >> 5) & 63) as u8;
        let b = (v & 31) as u8;
        [
            (r << 3) | (r >> 2),
            (g << 2) | (g >> 4),
            (b << 3) | (b >> 2),
            255,
        ]
    };
    let mut p = [rgb(a), rgb(b), [0; 4], [0; 4]];
    let three = transparent && a <= b;
    for c in 0..3 {
        if three {
            p[2][c] = ((p[0][c] as u16 + p[1][c] as u16) / 2) as u8;
        } else {
            p[2][c] = ((2 * p[0][c] as u16 + p[1][c] as u16) / 3) as u8;
            p[3][c] = ((p[0][c] as u16 + 2 * p[1][c] as u16) / 3) as u8;
        }
    }
    p[2][3] = 255;
    if !three {
        p[3][3] = 255;
    }
    let bits = u32::from_le_bytes(block[4..8].try_into().unwrap());
    std::array::from_fn(|i| p[((bits >> (i * 2)) & 3) as usize])
}

/// Decode one mip level to tightly packed RGBA8. Reject malformed lengths and offsets.
pub fn decode(data: &[u8], mip: usize) -> Result<Image, String> {
    if data.get(..4) != Some(b"<DE(") {
        return Err("not a D2R texture".into());
    }
    let fmt = u16::from_le_bytes(
        data.get(4..6)
            .ok_or("truncated texture header")?
            .try_into()
            .unwrap(),
    );
    let mips = *data.get(7).ok_or("truncated texture header")? as usize;
    if mip >= mips {
        return Err("mip index out of range".into());
    }
    let width = u32_at(data, 8)?.checked_shr(mip as u32).unwrap_or(0).max(1);
    let height = u32_at(data, 12)?
        .checked_shr(mip as u32)
        .unwrap_or(0)
        .max(1);
    let field = 36usize
        .checked_add(mip.checked_mul(8).ok_or("invalid mip")?)
        .ok_or("invalid mip")?;
    let size = u32_at(data, field)? as usize;
    let offset = (field + 4)
        .checked_add(u32_at(data, field + 4)? as usize)
        .ok_or("invalid mip offset")?;
    let end = offset.checked_add(size).ok_or("invalid mip size")?;
    let payload = data.get(offset..end).ok_or("truncated mip")?;
    let block_size = match fmt {
        31 => 0,
        57 | 58 | 63 => 8,
        61 | 62 | 65 => 16,
        _ => return Err(format!("unknown D2R texture format {fmt}")),
    };
    let pixels = (width as usize)
        .checked_mul(height as usize)
        .ok_or("image too large")?;
    let output_len = pixels.checked_mul(4).ok_or("image too large")?;
    let bw = (width as usize).div_ceil(4);
    let bh = (height as usize).div_ceil(4);
    let expected = if block_size == 0 {
        output_len
    } else {
        bw.checked_mul(bh)
            .and_then(|n| n.checked_mul(block_size))
            .ok_or("image too large")?
    };
    if size != expected {
        return Err(format!(
            "mip {mip}: {size} bytes, expected {expected} for format {fmt}"
        ));
    }
    if block_size == 0 {
        return Ok(Image {
            width,
            height,
            rgba: payload.to_vec(),
        });
    }
    let mut rgba = vec![0; output_len];
    for by in 0..bh {
        for bx in 0..bw {
            let block = &payload[(by * bw + bx) * block_size..][..block_size];
            let colors = match fmt {
                57 | 58 => color(block, true),
                61 | 62 => {
                    let mut c = color(&block[8..], false);
                    let a = alpha(block);
                    for i in 0..16 {
                        c[i][3] = a[i];
                    }
                    c
                }
                63 => {
                    let a = alpha(block);
                    std::array::from_fn(|i| [a[i], a[i], a[i], 255])
                }
                65 => {
                    let r = alpha(block);
                    let g = alpha(&block[8..]);
                    std::array::from_fn(|i| [r[i], g[i], 0, 255])
                }
                _ => unreachable!(),
            };
            for y in 0..4 {
                for x in 0..4 {
                    let px = bx * 4 + x;
                    let py = by * 4 + y;
                    if px < width as usize && py < height as usize {
                        let dst = (py * width as usize + px) * 4;
                        rgba[dst..dst + 4].copy_from_slice(&colors[y * 4 + x]);
                    }
                }
            }
        }
    }
    Ok(Image {
        width,
        height,
        rgba,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn resize_limits_both_axes_and_zero_keeps_native_size() {
        let src = Image {
            width: 2,
            height: 8,
            rgba: [20, 30, 40, 255].repeat(16),
        };
        assert_eq!(resize(src.clone(), 0).unwrap(), src);
        let scaled = resize(src, 4).unwrap();
        assert_eq!((scaled.width, scaled.height), (1, 4));
        assert_eq!(scaled.rgba.len(), 16);
    }
    fn texture(fmt: u16, width: u32, height: u32, payloads: &[&[u8]]) -> Vec<u8> {
        let mut data = vec![0; 36 + 8 * payloads.len()];
        data[..4].copy_from_slice(b"<DE(");
        data[4..6].copy_from_slice(&fmt.to_le_bytes());
        data[6] = 2;
        data[7] = payloads.len() as u8;
        data[8..12].copy_from_slice(&width.to_le_bytes());
        data[12..16].copy_from_slice(&height.to_le_bytes());
        for (i, payload) in payloads.iter().enumerate() {
            let field = 36 + 8 * i;
            data[field..field + 4].copy_from_slice(&(payload.len() as u32).to_le_bytes());
            let relative = (data.len() - field - 4) as u32;
            data[field + 4..field + 8].copy_from_slice(&relative.to_le_bytes());
            data.extend_from_slice(payload);
        }
        data
    }
    #[test]
    fn raw_mips_preserve_rgba_and_relative_offsets() {
        let big = [1, 2, 3, 4].repeat(4);
        let small = [21, 22, 23, 24];
        let data = texture(31, 2, 2, &[&big, &small]);
        assert_eq!(
            decode(&data, 1).unwrap(),
            Image {
                width: 1,
                height: 1,
                rgba: small.to_vec()
            }
        );
        assert!(decode(&data, 2).is_err());
    }
    #[test]
    fn bc4_and_bc5_channels() {
        let block = [200, 0, 0, 0, 0, 0, 0, 0];
        let bc4 = decode(&texture(63, 4, 4, &[&block]), 0).unwrap();
        assert!(bc4.rgba.chunks_exact(4).all(|p| p == [200, 200, 200, 255]));
        let mut bc5 = [0; 16];
        bc5[0] = 64;
        bc5[8] = 192;
        let image = decode(&texture(65, 3, 2, &[&bc5]), 0).unwrap();
        assert!(image.rgba.chunks_exact(4).all(|p| p == [64, 192, 0, 255]));
    }
    #[test]
    fn bc1_transparency_and_bc3_alpha() {
        let bc1 = [0, 0, 255, 255, 255, 255, 255, 255];
        assert!(decode(&texture(57, 4, 4, &[&bc1]), 0)
            .unwrap()
            .rgba
            .chunks_exact(4)
            .all(|p| p[3] == 0));
        let mut bc3 = [0u8; 16];
        bc3[0] = 123;
        bc3[8] = 0;
        bc3[9] = 248;
        assert!(decode(&texture(61, 4, 4, &[&bc3]), 0)
            .unwrap()
            .rgba
            .chunks_exact(4)
            .all(|p| p == [255, 0, 0, 123]));
    }
    #[test]
    fn malformed_input_is_rejected() {
        assert!(decode(b"<DE(", 0).is_err());
        assert!(decode(&texture(31, 4, 4, &[&[0; 63]]), 0).is_err());
        assert!(decode(&texture(63, 4, 4, &[&[0; 7]]), 0).is_err());
        assert!(decode(&texture(999, 4, 4, &[&[0; 8]]), 0).is_err());
    }
}
