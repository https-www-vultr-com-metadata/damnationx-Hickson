//! Explicitly contracted terrain height maps derived from albedo alpha.
use crate::texture::Image;
use serde_json::{json, Value};

/// Returns a grayscale PNG with the engine's white-recessed polarity and its
/// contract. Unverified alpha is never silently treated as height.
pub fn encode(source: &Image, height_spec: &Value, max: u32) -> Result<(Vec<u8>, Value), String> {
    if height_spec["source"].as_str() != Some("albedo_alpha") {
        return Err("height requires albedo_alpha source".into());
    }
    let polarity = height_spec["source_polarity"]
        .as_str()
        .ok_or("height requires explicit source polarity")?;
    if polarity != "white_raised" && polarity != "white_recessed" {
        return Err("height source polarity must be white_raised or white_recessed".into());
    }
    let depth = height_spec["depth_scale_m"]
        .as_f64()
        .filter(|v| v.is_finite() && *v > 0. && *v <= 0.04)
        .ok_or("height depth_scale_m must be in (0, 0.04] metres")?;
    if max == 0 {
        return Err("height max edge must be positive".into());
    }
    let count = (source.width as usize)
        .checked_mul(source.height as usize)
        .ok_or("height dimensions overflow")?;
    if source.rgba.len() != count.checked_mul(4).ok_or("height dimensions overflow")? {
        return Err("invalid height source image".into());
    }
    let alpha: Vec<u8> = source
        .rgba
        .chunks_exact(4)
        .map(|p| {
            if polarity == "white_raised" {
                255 - p[3]
            } else {
                p[3]
            }
        })
        .collect();
    let input = image::GrayImage::from_raw(source.width, source.height, alpha)
        .ok_or("invalid height source image")?;
    let max = max.min(512);
    let resized = if source.width.max(source.height) > max {
        let factor = max as f64 / source.width.max(source.height) as f64;
        image::imageops::resize(
            &input,
            ((source.width as f64 * factor).floor() as u32).max(1),
            ((source.height as f64 * factor).floor() as u32).max(1),
            image::imageops::FilterType::Lanczos3,
        )
    } else {
        input
    };
    let mut png = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut png, resized.width(), resized.height());
        encoder.set_compression(png::Compression::Default);
        encoder.set_adaptive_filter(png::AdaptiveFilterType::Adaptive);
        encoder.set_color(png::ColorType::Grayscale);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().map_err(|e| e.to_string())?;
        writer
            .write_image_data(&resized)
            .map_err(|e| e.to_string())?;
    }
    let contract = json!({"validated":height_spec["validated"].as_bool().unwrap_or(false),"polarity":"white_recessed","depth_scale_m":depth,"source":"albedo_alpha","source_polarity":polarity,"primary_policy":"verified_single_layer_raster_opt_in","layered_policy":"ordinary_pbr","rt_policy":"ordinary_pbr"});
    Ok((png, contract))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn validates_and_preserves_height_polarity() {
        let image = Image {
            width: 2,
            height: 1,
            rgba: vec![1, 2, 3, 0, 4, 5, 6, 200],
        };
        let spec = json!({"source":"albedo_alpha","source_polarity":"white_raised","depth_scale_m":0.03,"validated":true});
        let (png, contract) = encode(&image, &spec, 512).unwrap();
        let decoder = png::Decoder::new(png.as_slice());
        let mut reader = decoder.read_info().unwrap();
        let mut buffer = vec![0; reader.output_buffer_size()];
        let info = reader.next_frame(&mut buffer).unwrap();
        assert_eq!(&buffer[..info.buffer_size()], &[255, 55]);
        assert_eq!(contract["polarity"], "white_recessed");
        assert_eq!(contract["validated"], true);
        assert!(encode(
            &image,
            &json!({"source":"albedo_alpha","source_polarity":"white_raised","depth_scale_m":0.05}),
            512
        )
        .is_err());
    }
}
