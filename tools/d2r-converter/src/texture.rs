//! Decoder for the `<DE(` D2R texture container and its BC formats, and KTX2 output that keeps them compressed.

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

/// BC4 palette for endpoints `a` and `b`; also the alpha palette of BC3 and each channel of BC5.
fn bc4_palette(a: u8, b: u8) -> [u8; 8] {
    let (a, b) = (a as u32, b as u32);
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
    palette
}

fn alpha(block: &[u8]) -> [u8; 16] {
    let palette = bc4_palette(block[0], block[1]);
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

/// Block encodings found in D2R textures, plus BC5 for converted normal maps.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Block {
    /// BC1 with one-bit alpha.
    Bc1,
    /// BC3: BC4 alpha followed by a BC1 colour block.
    Bc3,
    /// One BC4 channel, decoded as grey.
    Bc4,
    /// Two BC4 channels, red then green.
    Bc5,
}

impl Block {
    pub fn bytes(self) -> usize {
        match self {
            Block::Bc1 | Block::Bc4 => 8,
            Block::Bc3 | Block::Bc5 => 16,
        }
    }

    fn decode(self, block: &[u8]) -> [[u8; 4]; 16] {
        match self {
            Block::Bc1 => color(block, true),
            Block::Bc3 => {
                let mut c = color(&block[8..], false);
                let a = alpha(block);
                for i in 0..16 {
                    c[i][3] = a[i];
                }
                c
            }
            Block::Bc4 => {
                let a = alpha(block);
                std::array::from_fn(|i| [a[i], a[i], a[i], 255])
            }
            Block::Bc5 => {
                let r = alpha(block);
                let g = alpha(&block[8..]);
                std::array::from_fn(|i| [r[i], g[i], 0, 255])
            }
        }
    }
}

/// Bytes of one `width` x `height` level in `block` encoding.
fn level_bytes(block: Block, width: u32, height: u32) -> Option<usize> {
    (width as usize)
        .div_ceil(4)
        .checked_mul((height as usize).div_ceil(4))?
        .checked_mul(block.bytes())
}

fn decode_blocks(block: Block, width: u32, height: u32, payload: &[u8]) -> Result<Image, String> {
    let output_len = (width as usize)
        .checked_mul(height as usize)
        .and_then(|n| n.checked_mul(4))
        .ok_or("image too large")?;
    if Some(payload.len()) != level_bytes(block, width, height) {
        return Err("block data does not match the image size".into());
    }
    let bw = (width as usize).div_ceil(4);
    let bh = (height as usize).div_ceil(4);
    let mut rgba = vec![0; output_len];
    for by in 0..bh {
        for bx in 0..bw {
            let colors = block.decode(&payload[(by * bw + bx) * block.bytes()..][..block.bytes()]);
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

/// A parsed `<DE(` texture: its format code, mip count and level-0 size.
struct Texture<'a> {
    data: &'a [u8],
    format: u16,
    mips: usize,
    width: u32,
    height: u32,
}

impl<'a> Texture<'a> {
    fn parse(data: &'a [u8]) -> Result<Self, String> {
        if data.get(..4) != Some(b"<DE(") {
            return Err("not a D2R texture".into());
        }
        let format = u16::from_le_bytes(
            data.get(4..6)
                .ok_or("truncated texture header")?
                .try_into()
                .unwrap(),
        );
        let mips = *data.get(7).ok_or("truncated texture header")? as usize;
        Ok(Self {
            data,
            format,
            mips,
            width: u32_at(data, 8)?,
            height: u32_at(data, 12)?,
        })
    }

    /// `None` for raw RGBA (format 31).
    fn block(&self) -> Result<Option<Block>, String> {
        Ok(match self.format {
            31 => None,
            57 | 58 => Some(Block::Bc1),
            61 | 62 => Some(Block::Bc3),
            63 => Some(Block::Bc4),
            65 => Some(Block::Bc5),
            fmt => return Err(format!("unknown D2R texture format {fmt}")),
        })
    }

    fn size(&self, mip: usize) -> (u32, u32) {
        let shrink = |v: u32| v.checked_shr(mip as u32).unwrap_or(0).max(1);
        (shrink(self.width), shrink(self.height))
    }

    /// The payload of one level, checked against its size and format.
    fn level(&self, mip: usize) -> Result<&'a [u8], String> {
        if mip >= self.mips {
            return Err("mip index out of range".into());
        }
        let data = self.data;
        let field = 36usize
            .checked_add(mip.checked_mul(8).ok_or("invalid mip")?)
            .ok_or("invalid mip")?;
        let size = u32_at(data, field)? as usize;
        let offset = (field + 4)
            .checked_add(u32_at(data, field + 4)? as usize)
            .ok_or("invalid mip offset")?;
        let end = offset.checked_add(size).ok_or("invalid mip size")?;
        let payload = data.get(offset..end).ok_or("truncated mip")?;
        let (width, height) = self.size(mip);
        let expected = match self.block()? {
            None => (width as usize)
                .checked_mul(height as usize)
                .and_then(|n| n.checked_mul(4)),
            Some(block) => level_bytes(block, width, height),
        }
        .ok_or("image too large")?;
        if size != expected {
            return Err(format!(
                "mip {mip}: {size} bytes, expected {expected} for format {}",
                self.format
            ));
        }
        Ok(payload)
    }
}

/// Decode one mip level to tightly packed RGBA8. Reject malformed lengths and offsets.
pub fn decode(data: &[u8], mip: usize) -> Result<Image, String> {
    let texture = Texture::parse(data)?;
    let payload = texture.level(mip)?;
    let (width, height) = texture.size(mip);
    match texture.block()? {
        None => Ok(Image {
            width,
            height,
            rgba: payload.to_vec(),
        }),
        Some(block) => decode_blocks(block, width, height, payload),
    }
}

/// Encode 16 values as a BC4 block. D2R normal maps keep Y in the green of a BC1 colour block, whose four
/// levels are evenly spaced thirds; a BC4 range stretched to 7/6 of theirs has those thirds among its sevenths,
/// so such blocks convert without loss. Plain min/max is the fallback for anything else.
fn bc4_encode(values: &[u8; 16]) -> [u8; 8] {
    let lo = *values.iter().min().unwrap();
    let hi = *values.iter().max().unwrap();
    let mut out = [0u8; 8];
    if lo == hi {
        out[0] = hi;
        out[1] = lo;
        return out;
    }
    let span = (hi - lo) as u32;
    let stretch = (span * 7 + 3) / 6;
    let mut candidates = vec![(hi, lo)];
    if lo as u32 + stretch <= 255 {
        candidates.push(((lo as u32 + stretch) as u8, lo));
    }
    if hi as u32 >= stretch {
        candidates.push((hi, (hi as u32 - stretch) as u8));
    }
    if candidates.len() == 1 {
        // Wide blocks (rare in normal maps): the six-level mode, whose fixed 0 and 255 cover the extremes, and
        // endpoints near the extremes.
        candidates.push(((lo as u32 + span / 3) as u8, (lo as u32 + (2 * span + 1) / 3) as u8));
        for da in -6i32..=6 {
            for db in -6i32..=6 {
                let (a, b) = (hi as i32 + da, lo as i32 + db);
                if (0..=255).contains(&a) && (0..=255).contains(&b) && a > b {
                    candidates.push((a as u8, b as u8));
                }
            }
        }
    }
    let mut best: Option<(u32, [u8; 8])> = None;
    for (a, b) in candidates {
        let palette = bc4_palette(a, b);
        let mut bits = 0u64;
        let mut error = 0u32;
        for (i, v) in values.iter().enumerate() {
            let (index, e) = palette
                .iter()
                .enumerate()
                .map(|(index, p)| (index, (*p as i32 - *v as i32).unsigned_abs().pow(2)))
                .min_by_key(|(_, e)| *e)
                .unwrap();
            error += e;
            bits |= (index as u64) << (3 * i);
        }
        if best.is_none_or(|(e, _)| error < e) {
            let mut block = [0u8; 8];
            block[0] = a;
            block[1] = b;
            block[2..8].copy_from_slice(&bits.to_le_bytes()[..6]);
            best = Some((error, block));
        }
    }
    best.unwrap().1
}

/// Squared length of the tangent-space XY a normal texel encodes.
fn normal_xy_squared(x: u8, y: u8) -> f32 {
    let (x, y) = (x as f32 / 127.5 - 1., y as f32 / 127.5 - 1.);
    x * x + y * y
}

/// A D2R normal block (BC3 with X in alpha and Y in green) as BC5 with X in red and Y in green, the
/// two-channel layout Bevy reconstructs Z from. The alpha block is BC4 already and is copied unchanged.
///
/// Bevy computes Z as `sqrt(1 - x² - y²)` without a clamp, so a texel outside the unit disk shades as NaN.
/// About 0.5% of D2R's normal texels are (the PNG path clamped Z to 0). Blocks with such texels have XY pulled
/// just inside the disk, keeping their direction, and both channels re-encoded.
fn bc3_normal_to_bc5(block: &[u8]) -> [u8; 16] {
    let x = alpha(block);
    let colors = color(&block[8..], false);
    let y: [u8; 16] = std::array::from_fn(|i| colors[i][1]);
    let mut out = [0u8; 16];
    out[..8].copy_from_slice(&block[..8]);
    out[8..].copy_from_slice(&bc4_encode(&y));
    let inside = |out: &[u8; 16]| {
        let (x, y) = (alpha(out), alpha(&out[8..]));
        (0..16).all(|i| normal_xy_squared(x[i], y[i]) <= 0.998)
    };
    if inside(&out) {
        return out;
    }
    for radius in [0.99f32, 0.97, 0.94, 0.9, 0.8] {
        let mut xs = x;
        let mut ys = y;
        for i in 0..16 {
            let length = normal_xy_squared(x[i], y[i]).sqrt();
            if length > radius {
                let scale = radius / length;
                let pull = |v: u8| (((v as f32 / 127.5 - 1.) * scale + 1.) * 127.5).round().clamp(0., 255.) as u8;
                (xs[i], ys[i]) = (pull(x[i]), pull(y[i]));
            }
        }
        out[..8].copy_from_slice(&bc4_encode(&xs));
        out[8..].copy_from_slice(&bc4_encode(&ys));
        if inside(&out) {
            break;
        }
    }
    out
}

/// Identifies how model textures are written, so resumable conversions never mix GLBs from an older format
/// (RGBA PNG before GPU-compressed KTX2) with new ones.
pub const OUTPUT_FORMAT: &str = "model-textures:ktx2-bc-1";

const KTX2_MAGIC: [u8; 12] = [
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
];

/// True for a KTX2 container.
pub fn is_ktx2(bytes: &[u8]) -> bool {
    bytes.starts_with(&KTX2_MAGIC)
}

/// Writes a single-image KTX2 texture holding `levels` (largest first) of `block` data. The Vulkan format is
/// the sRGB variant when `srgb`; Bevy's glTF loader picks sRGB or linear from the material slot either way.
pub fn ktx2(block: Block, srgb: bool, width: u32, height: u32, levels: &[Vec<u8>]) -> Vec<u8> {
    // vkFormat, DFD colour model and the DFD samples (channel id, bit offset) of each block format.
    let (vk_format, model, samples): (u32, u8, &[(u8, u16)]) = match (block, srgb) {
        (Block::Bc1, false) => (133, 128, &[(1, 0)]),
        (Block::Bc1, true) => (134, 128, &[(1, 0)]),
        (Block::Bc3, false) => (137, 130, &[(15, 0), (0, 64)]),
        (Block::Bc3, true) => (138, 130, &[(15, 0), (0, 64)]),
        (Block::Bc4, _) => (139, 131, &[(0, 0)]),
        (Block::Bc5, _) => (141, 132, &[(0, 0), (1, 64)]),
    };
    let srgb = srgb && matches!(block, Block::Bc1 | Block::Bc3);
    let mut dfd_block = Vec::new();
    let block_size = (24 + 16 * samples.len()) as u16;
    dfd_block.extend_from_slice(&0u32.to_le_bytes()); // Khronos vendor, basic descriptor
    dfd_block.extend_from_slice(&2u16.to_le_bytes()); // version
    dfd_block.extend_from_slice(&block_size.to_le_bytes());
    dfd_block.extend_from_slice(&[model, 1, if srgb { 2 } else { 1 }, 0]); // BT.709, sRGB/linear, straight alpha
    dfd_block.extend_from_slice(&[3, 3, 0, 0]); // 4x4x1x1 texels
    let mut planes = [0u8; 8];
    planes[0] = block.bytes() as u8;
    dfd_block.extend_from_slice(&planes);
    for (channel, offset) in samples {
        let linear_alpha = srgb && *channel == 15;
        dfd_block.extend_from_slice(&offset.to_le_bytes());
        dfd_block.push(63); // 64 bits
        dfd_block.push(channel | if linear_alpha { 0x10 } else { 0 });
        dfd_block.extend_from_slice(&[0; 4]);
        dfd_block.extend_from_slice(&0u32.to_le_bytes());
        dfd_block.extend_from_slice(&u32::MAX.to_le_bytes());
    }
    let level_count = levels.len();
    let dfd_offset = 80 + 24 * level_count;
    let dfd_length = 4 + dfd_block.len();
    let mut out = Vec::with_capacity(dfd_offset + dfd_length + levels.iter().map(Vec::len).sum::<usize>() + 16 * level_count);
    let header = [
        vk_format,
        1, // typeSize for block-compressed data
        width,
        height,
        0, // depth
        0, // layers
        1, // faces
        level_count as u32,
        0, // no supercompression
        dfd_offset as u32,
        dfd_length as u32,
        0, // no key/value data
        0,
    ];
    out.extend_from_slice(&KTX2_MAGIC);
    for value in header {
        out.extend_from_slice(&value.to_le_bytes());
    }
    out.extend_from_slice(&[0; 16]); // no supercompression global data
    let index = out.len();
    out.resize(dfd_offset, 0);
    out.extend_from_slice(&(dfd_length as u32).to_le_bytes());
    out.extend_from_slice(&dfd_block);
    // Levels are stored smallest first, each aligned to the block size.
    let mut offsets = vec![0usize; level_count];
    for (level, bytes) in levels.iter().enumerate().rev() {
        while out.len() % block.bytes() != 0 {
            out.push(0);
        }
        offsets[level] = out.len();
        out.extend_from_slice(bytes);
    }
    for (level, bytes) in levels.iter().enumerate() {
        let entry = index + 24 * level;
        out[entry..entry + 8].copy_from_slice(&(offsets[level] as u64).to_le_bytes());
        out[entry + 8..entry + 16].copy_from_slice(&(bytes.len() as u64).to_le_bytes());
        out[entry + 16..entry + 24].copy_from_slice(&(bytes.len() as u64).to_le_bytes());
    }
    out
}

/// Decode level 0 of a KTX2 texture written by [`ktx2`].
pub fn decode_ktx2(bytes: &[u8]) -> Result<Image, String> {
    if !is_ktx2(bytes) {
        return Err("not a KTX2 texture".into());
    }
    let block = match u32_at(bytes, 12)? {
        133 | 134 => Block::Bc1,
        137 | 138 => Block::Bc3,
        139 => Block::Bc4,
        141 => Block::Bc5,
        other => return Err(format!("unsupported KTX2 format {other}")),
    };
    let (width, height) = (u32_at(bytes, 20)?, u32_at(bytes, 24)?);
    let u64_at = |offset: usize| -> Result<usize, String> {
        let v = u64::from_le_bytes(
            bytes
                .get(offset..offset + 8)
                .ok_or("truncated KTX2 level index")?
                .try_into()
                .unwrap(),
        );
        usize::try_from(v).map_err(|_| "KTX2 level out of range".to_string())
    };
    let (offset, length) = (u64_at(80)?, u64_at(88)?);
    let payload = bytes
        .get(offset..offset.checked_add(length).ok_or("KTX2 level overflow")?)
        .ok_or("truncated KTX2 level")?;
    decode_blocks(block, width, height, payload)
}

/// GPU-ready KTX2 for a material slot (`albedo`, `normal` or `orm`) made from D2R's own block data and mip
/// chain without re-encoding, or `None` when the texture has to take the decoded RGBA path: raw or BC4
/// sources, normal maps that are not BC3, and sizes that are not whole blocks. Top levels are dropped until
/// the long edge fits `limit` (0 keeps the imported size).
///
/// Normal maps become BC5 (X red, Y green; see [`bc3_normal_to_bc5`]). Albedo and ORM blocks are copied; the
/// RGBA path forced ORM alpha to 255, but no material reads ORM alpha.
pub fn gpu_texture(data: &[u8], usage: &str, limit: u32) -> Result<Option<Vec<u8>>, String> {
    let texture = Texture::parse(data)?;
    let Some(source) = texture.block()? else {
        return Ok(None);
    };
    let block = match (usage, source) {
        ("albedo" | "orm", Block::Bc1 | Block::Bc3) => source,
        ("orm", Block::Bc5) => Block::Bc5,
        ("normal", Block::Bc3) => Block::Bc5,
        _ => return Ok(None),
    };
    let Some(first) = (0..texture.mips).find(|&mip| {
        let (w, h) = texture.size(mip);
        limit == 0 || w.max(h) <= limit
    }) else {
        return Ok(None);
    };
    let (width, height) = texture.size(first);
    if width % 4 != 0 || height % 4 != 0 {
        return Ok(None);
    }
    let mut levels = Vec::with_capacity(texture.mips - first);
    for mip in first..texture.mips {
        let payload = texture.level(mip)?;
        levels.push(if source == block {
            payload.to_vec()
        } else {
            payload
                .chunks_exact(source.bytes())
                .flat_map(bc3_normal_to_bc5)
                .collect()
        });
    }
    Ok(Some(ktx2(block, usage == "albedo", width, height, &levels)))
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

    /// Deterministic pseudo-random bytes.
    fn noise(seed: u32, len: usize) -> Vec<u8> {
        let mut state = seed.wrapping_mul(2654435761).max(1);
        (0..len)
            .map(|_| {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                (state >> 8) as u8
            })
            .collect()
    }

    /// A chain of `mips` random levels from `size` x `size` in `block` encoding.
    fn chain(block: Block, size: u32, mips: usize) -> Vec<Vec<u8>> {
        (0..mips)
            .map(|mip| {
                let edge = (size >> mip).max(1);
                noise(mip as u32 + 7, level_bytes(block, edge, edge).unwrap())
            })
            .collect()
    }

    #[test]
    fn ktx2_files_parse_with_the_ktx2_crate_and_describe_their_format() {
        use ktx2::{dfd, Format};
        for (block, srgb, format) in [
            (Block::Bc1, false, Format::BC1_RGBA_UNORM_BLOCK),
            (Block::Bc1, true, Format::BC1_RGBA_SRGB_BLOCK),
            (Block::Bc3, false, Format::BC3_UNORM_BLOCK),
            (Block::Bc3, true, Format::BC3_SRGB_BLOCK),
            (Block::Bc4, false, Format::BC4_UNORM_BLOCK),
            (Block::Bc5, false, Format::BC5_UNORM_BLOCK),
        ] {
            let levels = chain(block, 16, 5);
            let bytes = ktx2(block, srgb, 16, 16, &levels);
            let reader = ktx2::Reader::new(&bytes[..]).unwrap();
            let header = reader.header();
            assert_eq!(header.format, Some(format));
            assert_eq!((header.pixel_width, header.pixel_height, header.level_count), (16, 16, 5));
            assert_eq!(header.supercompression_scheme, None);
            let stored: Vec<&[u8]> = reader.levels().map(|level| level.data).collect();
            assert_eq!(stored, levels.iter().map(Vec::as_slice).collect::<Vec<_>>());
            let (expected, type_size) = dfd::Basic::from_format(format).unwrap();
            assert_eq!(header.type_size, type_size);
            assert_eq!(reader.dfd_blocks(), &[dfd::Block::Basic(expected)], "{format:?}");
            let top = decode_ktx2(&bytes).unwrap();
            assert_eq!(top, decode_blocks(block, 16, 16, &levels[0]).unwrap());
        }
    }

    #[test]
    fn bc5_normals_keep_x_exactly_and_y_within_a_level() {
        let (mut worst, mut worst_wide) = (0, 0);
        for seed in 0..2000 {
            let mut bc3 = noise(seed, 16);
            if seed % 2 == 0 {
                // A typical normal block: X and Y near the centre (eight-level alpha, green-only colours).
                bc3[0] = 129 + bc3[0] % 60;
                bc3[1] = 128 - bc3[1] % 60;
                let g0 = (32 + bc3[8] as u16 % 14) << 5;
                let g1 = (18 + bc3[10] as u16 % 14) << 5;
                bc3[8..10].copy_from_slice(&g0.to_le_bytes());
                bc3[10..12].copy_from_slice(&g1.to_le_bytes());
            }
            let source = Block::Bc3.decode(&bc3);
            let converted = Block::Bc5.decode(&bc3_normal_to_bc5(&bc3));
            // Every output texel is a valid two-channel normal.
            assert!(converted.iter().all(|c| normal_xy_squared(c[0], c[1]) <= 0.998));
            if source.iter().any(|s| normal_xy_squared(s[3], s[1]) > 0.99) {
                // Blocks with texels outside (or at the edge of) the unit disk are re-encoded.
                continue;
            }
            let greens = source.map(|s| s[1] as u32);
            let (lo, hi) = (*greens.iter().min().unwrap(), *greens.iter().max().unwrap());
            let fits = lo + ((hi - lo) * 7 + 3) / 6 <= 255 || hi >= ((hi - lo) * 7 + 3) / 6;
            for (s, c) in source.iter().zip(converted) {
                assert_eq!(c[0], s[3]);
                let error = (c[1] as i32 - s[1] as i32).abs();
                if fits {
                    worst = worst.max(error);
                } else {
                    worst_wide = worst_wide.max(error);
                }
            }
        }
        assert!(worst <= 1, "green differs by {worst}");
        // Blocks spanning nearly the whole range cannot be exact; they stay close.
        assert!(worst_wide <= 24, "wide green differs by {worst_wide}");
    }

    #[test]
    fn gpu_textures_keep_blocks_drop_levels_and_fall_back_when_needed() {
        let bc1 = chain(Block::Bc1, 64, 7);
        let refs: Vec<&[u8]> = bc1.iter().map(Vec::as_slice).collect();
        let source = texture(57, 64, 64, &refs);
        // Full size: every level, bytes unchanged.
        let full = gpu_texture(&source, "albedo", 0).unwrap().unwrap();
        let reader = ktx2::Reader::new(&full[..]).unwrap();
        assert_eq!(reader.header().format, Some(ktx2::Format::BC1_RGBA_SRGB_BLOCK));
        assert_eq!(reader.levels().map(|l| l.data.to_vec()).collect::<Vec<_>>(), bc1);
        // A 16 px limit starts at level 2.
        let small = gpu_texture(&source, "orm", 16).unwrap().unwrap();
        let reader = ktx2::Reader::new(&small[..]).unwrap();
        assert_eq!(reader.header().format, Some(ktx2::Format::BC1_RGBA_UNORM_BLOCK));
        assert_eq!((reader.header().pixel_width, reader.header().level_count), (16, 5));
        assert_eq!(reader.levels().next().unwrap().data, &bc1[2][..]);
        assert_eq!(decode_ktx2(&small).unwrap(), decode(&source, 2).unwrap());
        // BC3 normals become BC5.
        let bc3 = chain(Block::Bc3, 16, 3);
        let refs: Vec<&[u8]> = bc3.iter().map(Vec::as_slice).collect();
        let normal = gpu_texture(&texture(61, 16, 16, &refs), "normal", 0).unwrap().unwrap();
        assert_eq!(ktx2::Reader::new(&normal[..]).unwrap().header().format, Some(ktx2::Format::BC5_UNORM_BLOCK));
        // Sources the GPU path cannot carry unchanged.
        let raw = [1u8, 2, 3, 4].repeat(16 * 16);
        assert_eq!(gpu_texture(&texture(31, 16, 16, &[&raw]), "albedo", 0).unwrap(), None);
        let bc4 = noise(1, 128);
        assert_eq!(gpu_texture(&texture(63, 16, 32, &[&bc4]), "orm", 0).unwrap(), None);
        assert_eq!(gpu_texture(&texture(57, 16, 16, &[&bc1[2]]), "normal", 0).unwrap(), None);
        let odd = noise(2, level_bytes(Block::Bc1, 6, 8).unwrap());
        assert_eq!(gpu_texture(&texture(57, 6, 8, &[&odd]), "albedo", 0).unwrap(), None);
        assert!(gpu_texture(&texture(57, 64, 64, &refs[..1]), "albedo", 0).is_err());
    }
}
