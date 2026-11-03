//! D2R albedo color variants and hero dye baking.
use crate::texture::Image;
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

fn hsv(rgb: [f32; 3]) -> [f32; 3] {
    let max = rgb[0].max(rgb[1]).max(rgb[2]);
    let min = rgb[0].min(rgb[1]).min(rgb[2]);
    let d = max - min;
    let h = if d <= 1e-6 {
        0.0
    } else if max == rgb[0] {
        ((rgb[1] - rgb[2]) / d / 6.0).rem_euclid(1.0)
    } else if max == rgb[1] {
        ((2.0 + (rgb[2] - rgb[0]) / d) / 6.0).rem_euclid(1.0)
    } else {
        ((4.0 + (rgb[0] - rgb[1]) / d) / 6.0).rem_euclid(1.0)
    };
    [h, if max > 1e-6 { d / max } else { 0.0 }, max]
}
fn rgb(hsv: [f32; 3]) -> [f32; 3] {
    let x = hsv[0].rem_euclid(1.0) * 6.0;
    let i = x.floor() as usize;
    let f = x - i as f32;
    let [_, s, v] = hsv;
    let p = v * (1.0 - s);
    let q = v * (1.0 - s * f);
    let t = v * (1.0 - s * (1.0 - f));
    match i % 6 {
        0 => [v, t, p],
        1 => [q, v, p],
        2 => [p, v, t],
        3 => [p, q, v],
        4 => [t, p, v],
        _ => [v, p, q],
    }
}
fn vec4(value: &Value, key: &str, defaults: [f32; 4]) -> [f32; 4] {
    ["x", "y", "z", "w"].map(|k| {
        value
            .get(key)
            .and_then(|v| v.get(k))
            .and_then(Value::as_f64)
            .map(|v| v as f32)
            .unwrap_or(
                defaults[match k {
                    "x" => 0,
                    "y" => 1,
                    "z" => 2,
                    _ => 3,
                }],
            )
    })
}
fn lerp(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t
}
fn mask_channel(mask: &Image, x: u32, y: u32, width: u32, height: u32, channel: usize) -> f32 {
    let fx = ((x as f32 + 0.5) * mask.width as f32 / width as f32 - 0.5)
        .clamp(0.0, (mask.width - 1) as f32);
    let fy = ((y as f32 + 0.5) * mask.height as f32 / height as f32 - 0.5)
        .clamp(0.0, (mask.height - 1) as f32);
    let (x0, y0) = (fx.floor() as u32, fy.floor() as u32);
    let (x1, y1) = ((x0 + 1).min(mask.width - 1), (y0 + 1).min(mask.height - 1));
    let at =
        |x: u32, y: u32| mask.rgba[((y * mask.width + x) * 4) as usize + channel] as f32 / 255.0;
    lerp(
        lerp(at(x0, y0), at(x1, y0), fx - x0 as f32),
        lerp(at(x0, y1), at(x1, y1), fx - x0 as f32),
        fy - y0 as f32,
    )
}
fn valid(image: &Image) -> bool {
    image.width > 0
        && image.height > 0
        && (image.width as usize)
            .checked_mul(image.height as usize)
            .and_then(|n| n.checked_mul(4))
            == Some(image.rgba.len())
}

/// Apply up to three color transforms weighted by the RGB channels of the KTINT mask.
pub fn apply(albedo: &Image, mask: &Image, transforms: &[Value]) -> Result<Image, String> {
    if !valid(albedo) || !valid(mask) {
        return Err("Invalid image dimensions".into());
    }
    let mut result = Image {
        width: albedo.width,
        height: albedo.height,
        rgba: albedo.rgba.clone(),
    };
    for y in 0..albedo.height {
        for x in 0..albedo.width {
            let i = ((y * albedo.width + x) * 4) as usize;
            let mut color = [0, 1, 2].map(|c| f32::from(albedo.rgba[i + c]) / 255.0);
            for (ch, transform) in transforms.iter().take(3).enumerate() {
                let weight = mask_channel(mask, x, y, albedo.width, albedo.height, ch);
                if weight < 1e-3 {
                    continue;
                }
                let adjustment = vec4(transform, "colorAdjustment", [0., 0., 0., 1.]);
                let tint = vec4(transform, "colorTint", [0., 0., 0., 0.]);
                let [h, s, v] = hsv(color);
                let s = (s + adjustment[1] * if adjustment[1] > 0.0 { 1.0 - s } else { s })
                    .clamp(0., 1.);
                let v = (v * (1.0 + 0.5 * adjustment[2])).clamp(0., 1.);
                let adjusted = rgb([(h + adjustment[0]).rem_euclid(1.0), s, v]);
                let mut out =
                    [0, 1, 2].map(|c| lerp(color[c], adjusted[c], adjustment[3].clamp(0., 1.)));
                if tint[3] > 1e-3 {
                    let luma = out[0] * 0.2126 + out[1] * 0.7152 + out[2] * 0.0722;
                    out = [0, 1, 2].map(|c| lerp(out[c], tint[c] * luma * 1.6, tint[3] * 0.5));
                }
                color = [0, 1, 2].map(|c| lerp(color[c], out[c].clamp(0., 1.), weight));
            }
            for c in 0..3 {
                result.rgba[i + c] = (color[c].clamp(0., 1.) * 255.0 + 0.5) as u8;
            }
        }
    }
    Ok(result)
}

/// Replace saturated green/teal or magenta dye placeholders with the chosen color.
pub fn dye(image: &Image, target: [u8; 3]) -> Result<Image, String> {
    if !valid(image) {
        return Err("Invalid image dimensions".into());
    }
    let band = |h: f32, lo: f32, hi: f32| {
        ((h - lo) / 12.0).clamp(0., 1.) * ((hi - h) / 12.0).clamp(0., 1.)
    };
    let mut masks = Vec::with_capacity(image.rgba.len() / 4);
    let (mut weight, mut luminance) = (0., 0.);
    for p in image.rgba.chunks_exact(4) {
        let color = [0, 1, 2].map(|c| f32::from(p[c]) / 255.0);
        let [h, s, v] = hsv(color);
        let deg = h * 360.;
        let m = (band(deg, 80., 200.) * ((s - 0.28) / 0.15).clamp(0., 1.)
            + band(deg, 275., 330.) * ((s - 0.4) / 0.15).clamp(0., 1.))
        .clamp(0., 1.)
            * ((v - 0.08) / 0.08).clamp(0., 1.);
        let l = color[0] * 0.2126 + color[1] * 0.7152 + color[2] * 0.0722;
        masks.push((m, l));
        weight += m;
        luminance += l * m;
    }
    let mut result = Image {
        width: image.width,
        height: image.height,
        rgba: image.rgba.clone(),
    };
    if weight < 16.0 {
        return Ok(result);
    }
    let reference = (luminance / weight).max(1e-3);
    for (p, (m, l)) in result.rgba.chunks_exact_mut(4).zip(masks) {
        let shade = (l / reference).clamp(0., 2.5);
        for c in 0..3 {
            let color = f32::from(p[c]) / 255.;
            let dyed = (f32::from(target[c]) / 255. * shade).clamp(0., 1.);
            p[c] = (lerp(color, dyed, m) * 255. + 0.5) as u8;
        }
    }
    Ok(result)
}

/// Variant PNG paths and material-to-path mappings for a converted unit.
pub struct BakedVariants {
    pub files: BTreeMap<String, Vec<u8>>,
    pub variants: BTreeMap<String, BTreeMap<String, String>>,
}

/// Bake variants from the same selected model parts used by the base converter.
pub fn bake(
    access: &mut impl crate::import_model::AssetAccess,
    unit_path: &str,
    model_key: &str,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
    names: &BTreeSet<String>,
) -> Result<BakedVariants, String> {
    bake_with_limit(
        access,
        unit_path,
        model_key,
        overrides,
        all_variants,
        names,
        0,
    )
}
pub fn bake_with_limit(
    access: &mut impl crate::import_model::AssetAccess,
    unit_path: &str,
    model_key: &str,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
    names: &BTreeSet<String>,
    texture_limit: u32,
) -> Result<BakedVariants, String> {
    use sha2::{Digest, Sha256};
    let mut output = BakedVariants {
        files: BTreeMap::new(),
        variants: BTreeMap::new(),
    };
    if names.is_empty() {
        return Ok(output);
    }
    let listing: BTreeSet<String> = access
        .listing()?
        .into_iter()
        .map(|p| p.to_lowercase())
        .collect();
    let exists = |p: &str| listing.contains(&p.replace('\\', "/").to_lowercase());
    let base = unit_path.strip_suffix(".json").unwrap_or(unit_path);
    let name = base.rsplit('/').next().unwrap_or(base);
    let table_path = [
        format!("{base}/{name}_variant.json"),
        format!("{base}_variant.json"),
    ]
    .into_iter()
    .find(|p| exists(p))
    .ok_or_else(|| format!("Variant table unavailable for {unit_path}"))?;
    let data = access.read(&table_path)?;
    let table: Value = serde_json::from_slice(data.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&data))
        .map_err(|e| e.to_string())?;
    let transforms: BTreeMap<String, Vec<Value>> = table["entries"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|e| {
            Some((
                e["name"].as_str()?.to_owned(),
                e["transforms"].as_array().cloned().unwrap_or_default(),
            ))
        })
        .collect();
    let unit = crate::unit::load_unit(unit_path, |p| access.read(p), exists, false)?;
    let parts = crate::unit::choose_parts(&unit, overrides, all_variants)?;
    let mut materials = BTreeMap::new();
    for part in parts {
        let Some(path) = part
            .model
            .as_ref()
            .map(|p| crate::unit::model_file(p, 0, exists))
            .filter(|p| exists(p))
        else {
            continue;
        };
        let raw = access.granny(&path)?;
        let root = crate::granny::Granny::new(&raw)?.root()?;
        for mesh in root["Meshes"].as_array().into_iter().flatten() {
            if mesh["Name"].as_str().unwrap_or("").starts_with("death_")
                || mesh["ExtendedData"]["meshRenderLayer"].as_i64() == Some(16)
            {
                continue;
            }
            for binding in mesh["MaterialBindings"].as_array().into_iter().flatten() {
                let material = &binding["Material"];
                let Some(name) = material["Name"].as_str() else {
                    continue;
                };
                let mut albedo = None;
                let mut mask = None;
                for map in material["Maps"].as_array().into_iter().flatten() {
                    match (
                        map["Usage"].as_str(),
                        map["Map"]["Texture"]["FromFileName"].as_str(),
                    ) {
                        (Some("AlbedoTexture"), Some(p)) => albedo = Some(p.to_owned()),
                        (Some("MaskTexture"), Some(p)) if p.to_lowercase().contains("ktint") => {
                            mask = Some(p.to_owned())
                        }
                        _ => (),
                    }
                }
                if let (Some(a), Some(m)) = (albedo, mask) {
                    materials.insert(name.to_owned(), (a, m));
                }
            }
        }
    }
    let paths: Vec<String> = materials
        .values()
        .flat_map(|(a, m)| [a.clone(), m.clone()])
        .collect();
    access.prefetch(&paths)?;
    // All path components are ASCII, portable, and collision-resistant; material keys remain native names.
    let component = |s: &str| format!("{:x}", Sha256::digest(s.as_bytes()))[..24].to_owned();
    for variant in names {
        let transforms = transforms
            .get(variant)
            .ok_or_else(|| format!("Variant {variant} missing in {table_path}"))?;
        let identity = transforms.iter().all(|t| {
            let a = vec4(t, "colorAdjustment", [0., 0., 0., 1.]);
            let c = vec4(t, "colorTint", [0., 0., 0., 0.]);
            a[..3].iter().all(|v| v.abs() <= 1e-3) && c[3] <= 1e-3
        });
        let mut mapping = BTreeMap::new();
        if !identity {
            for (material, (albedo, mask)) in &materials {
                let a = crate::texture::resize(
                    crate::texture::decode(&access.read(albedo)?, 0)?,
                    texture_limit,
                )?;
                let m = crate::texture::decode(&access.read(mask)?, 0)?;
                let image = apply(&a, &m, transforms)?;
                let mut bytes = Vec::new();
                {
                    let mut encoder = png::Encoder::new(&mut bytes, image.width, image.height);
                    encoder.set_compression(png::Compression::Default);
                    encoder.set_adaptive_filter(png::AdaptiveFilterType::Adaptive);
                    encoder.set_color(png::ColorType::Rgba);
                    encoder.set_depth(png::BitDepth::Eight);
                    encoder
                        .write_header()
                        .map_err(|e| e.to_string())?
                        .write_image_data(&image.rgba)
                        .map_err(|e| e.to_string())?;
                }
                let path = format!(
                    "variants/{}/{}/{}.png",
                    component(model_key),
                    component(variant),
                    component(material)
                );
                mapping.insert(material.clone(), path.clone());
                output.files.insert(path, bytes);
            }
        }
        output.variants.insert(variant.clone(), mapping);
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn identity_preserves_pixels_and_mask_zero_is_noop() {
        let a = Image {
            width: 1,
            height: 1,
            rgba: vec![50, 100, 200, 77],
        };
        let m = Image {
            width: 1,
            height: 1,
            rgba: vec![255, 0, 0, 255],
        };
        assert_eq!(apply(&a, &m, &[serde_json::json!({})]).unwrap(), a);
        let m = Image {
            width: 1,
            height: 1,
            rgba: vec![0, 0, 0, 255],
        };
        assert_eq!(
            apply(&a, &m, &[serde_json::json!({"colorAdjustment":{"x":0.5}})]).unwrap(),
            a
        );
    }
    #[test]
    fn hue_shift_and_dye_preserve_alpha() {
        let a = Image {
            width: 4,
            height: 4,
            rgba: [0, 255, 0, 90].repeat(16),
        };
        let m = Image {
            width: 1,
            height: 1,
            rgba: vec![255, 0, 0, 255],
        };
        let b = apply(&a, &m, &[serde_json::json!({"colorAdjustment":{"x":0.5}})]).unwrap();
        assert_eq!(&b.rgba[..4], &[255, 0, 255, 90]);
        let c = dye(&a, [255, 0, 0]).unwrap();
        assert!(c.rgba[0] > 200 && c.rgba[1] < 5);
        assert_eq!(c.rgba[3], 90);
    }
}
