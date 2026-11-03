//! Storage-independent model conversion orchestration for native and browser.
use crate::{granny::Granny, model, texture, unit};
use std::collections::{BTreeMap, BTreeSet};

pub trait AssetAccess {
    fn listing(&mut self) -> Result<Vec<String>, String>;
    fn read(&mut self, path: &str) -> Result<Vec<u8>, String>;
    fn granny(&mut self, path: &str) -> Result<Vec<u8>, String>;
    fn prefetch(&mut self, _paths: &[String]) -> Result<(), String> {
        Ok(())
    }
}
#[cfg(not(target_arch = "wasm32"))]
impl AssetAccess for crate::storage::NativeStorage {
    fn listing(&mut self) -> Result<Vec<String>, String> {
        Ok(crate::storage::NativeStorage::listing(self)?
            .keys()
            .cloned()
            .collect())
    }
    fn read(&mut self, path: &str) -> Result<Vec<u8>, String> {
        crate::storage::NativeStorage::read(self, path)
    }
    fn granny(&mut self, path: &str) -> Result<Vec<u8>, String> {
        crate::storage::NativeStorage::granny(self, path)
    }
    fn prefetch(&mut self, paths: &[String]) -> Result<(), String> {
        crate::storage::NativeStorage::fetch(self, paths)
    }
}
fn root(access: &mut impl AssetAccess, path: &str) -> Result<serde_json::Value, String> {
    let bytes = access.granny(path)?;
    Granny::new(&bytes)?.root()
}
pub fn png(
    access: &mut impl AssetAccess,
    path: &str,
    usage: &str,
) -> Result<Option<Vec<u8>>, String> {
    png_with_dye(access, path, usage, &BTreeMap::new(), 0)
}
fn png_with_dye(
    access: &mut impl AssetAccess,
    path: &str,
    usage: &str,
    dyes: &BTreeMap<String, String>,
    limit: u32,
) -> Result<Option<Vec<u8>>, String> {
    let mut image = texture::decode(&access.read(path)?, 0)?;
    if usage == "albedo" {
        if let Some((_, color)) = dyes
            .iter()
            .find(|(needle, _)| path.to_lowercase().contains(&needle.to_lowercase()))
        {
            let color = color.strip_prefix('#').ok_or("Invalid dye color")?;
            if color.len() != 6 || !color.is_ascii() {
                return Err("Invalid dye color".into());
            }
            let parse = |range| {
                u8::from_str_radix(&color[range], 16).map_err(|_| "Invalid dye color".to_string())
            };
            image = crate::variants::dye(&image, [parse(0..2)?, parse(2..4)?, parse(4..6)?])?;
        }
    }
    if usage == "normal" {
        for pixel in image.rgba.chunks_exact_mut(4) {
            let x = f32::from(pixel[3]) / 127.5 - 1.0;
            let y = f32::from(pixel[1]) / 127.5 - 1.0;
            let z = (1.0 - x * x - y * y).max(0.0).sqrt();
            pixel[0] = ((x * 0.5 + 0.5) * 255.0).round() as u8;
            pixel[1] = ((y * 0.5 + 0.5) * 255.0).round() as u8;
            pixel[2] = ((z * 0.5 + 0.5) * 255.0).round() as u8;
            pixel[3] = 255;
        }
    }
    if usage == "orm" {
        for pixel in image.rgba.chunks_exact_mut(4) {
            pixel[3] = 255;
        }
    }
    image = texture::resize(image, limit)?;
    let mut bytes = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut bytes, image.width, image.height);
        encoder.set_compression(png::Compression::Default);
        encoder.set_adaptive_filter(png::AdaptiveFilterType::Adaptive);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().map_err(|e| e.to_string())?;
        writer
            .write_image_data(&image.rgba)
            .map_err(|e| e.to_string())?;
    }
    Ok(Some(bytes))
}

/// Convert a single unit; caller owns the outputs and decides where to save them.
pub fn convert(
    access: &mut impl AssetAccess,
    path: &str,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
) -> Result<model::ModelOutput, String> {
    convert_with_clips(access, path, overrides, all_variants, &[])
}

pub fn convert_with_clips(
    access: &mut impl AssetAccess,
    path: &str,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
    clips: &[String],
) -> Result<model::ModelOutput, String> {
    convert_with_dyes(
        access,
        path,
        overrides,
        all_variants,
        clips,
        &BTreeMap::new(),
    )
}
pub fn convert_with_dyes(
    access: &mut impl AssetAccess,
    path: &str,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
    clips: &[String],
    dyes: &BTreeMap<String, String>,
) -> Result<model::ModelOutput, String> {
    convert_recipe(
        access,
        path,
        overrides,
        all_variants,
        clips,
        dyes,
        &BTreeMap::new(),
    )
}
pub fn convert_recipe(
    access: &mut impl AssetAccess,
    path: &str,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
    clips: &[String],
    dyes: &BTreeMap<String, String>,
    recipe_attachments: &BTreeMap<String, String>,
) -> Result<model::ModelOutput, String> {
    convert_recipe_with_limit(
        access,
        path,
        overrides,
        all_variants,
        clips,
        dyes,
        recipe_attachments,
        0,
    )
}
pub fn convert_recipe_with_limit(
    access: &mut impl AssetAccess,
    path: &str,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
    clips: &[String],
    dyes: &BTreeMap<String, String>,
    recipe_attachments: &BTreeMap<String, String>,
    texture_limit: u32,
) -> Result<model::ModelOutput, String> {
    convert_recipe_with_sources(
        access,
        path,
        overrides,
        all_variants,
        clips,
        dyes,
        recipe_attachments,
        &[],
        texture_limit,
    )
}

/// As `convert_recipe_with_limit`, also taking clips from other units' `combined.animations`
/// files. Some D2R units keep their clips in another unit's folder: the Act 1 Rogue hireling's
/// state machine plays Flavie's (`npc/navi`) animations; its own file holds only `run`. The
/// unit's own clips win over a source's clip of the same name; absent sources are skipped.
#[allow(clippy::too_many_arguments)]
pub fn convert_recipe_with_sources(
    access: &mut impl AssetAccess,
    path: &str,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
    clips: &[String],
    dyes: &BTreeMap<String, String>,
    recipe_attachments: &BTreeMap<String, String>,
    animation_sources: &[String],
    texture_limit: u32,
) -> Result<model::ModelOutput, String> {
    let listing: BTreeSet<String> = access
        .listing()?
        .into_iter()
        .map(|p| p.to_lowercase())
        .collect();
    let exists = |path: &str| listing.contains(&path.replace('\\', "/").to_lowercase());
    let unit = unit::load_unit(path, |p| access.read(p), exists, false)?;
    let parts = unit::choose_parts(&unit, overrides, all_variants)?;
    let model_paths: Vec<_> = parts
        .iter()
        .filter_map(|p| p.model.as_ref().map(|m| unit::model_file(m, 0, exists)))
        .filter(|p| exists(p))
        .collect();
    let skeleton_path = unit
        .skeleton
        .as_ref()
        .filter(|p| exists(p))
        .or_else(|| model_paths.first())
        .ok_or("No skeleton or model found for unit")?;
    let mut needed = model_paths.clone();
    needed.push(skeleton_path.clone());
    let mut combined: Vec<String> = unit.combined.iter().cloned().collect();
    for source in animation_sources {
        let source = source.replace('\\', "/");
        if exists(&source) && !combined.iter().any(|c| c.eq_ignore_ascii_case(&source)) {
            combined.push(source);
        }
    }
    needed.extend(combined.iter().cloned());
    access.prefetch(&needed)?;
    let skeleton = root(access, skeleton_path)?;
    let mut decoded = Vec::new();
    for part in &parts {
        if let Some(path) = part
            .model
            .as_ref()
            .map(|p| unit::model_file(p, 0, exists))
            .filter(|p| exists(p))
        {
            decoded.push((
                part.part.clone(),
                part.variant.clone(),
                root(access, &path)?,
            ));
        }
    }
    let refs: Vec<_> = decoded
        .iter()
        .map(|(part, variant, root)| model::Part {
            part,
            variant,
            root,
        })
        .collect();
    let attachments = crate::attachments::load(
        access,
        &unit,
        &parts,
        &skeleton,
        &listing,
        recipe_attachments,
    )?;
    let attachment_refs: Vec<_> = attachments.iter().map(|a| a.as_model()).collect();
    let mut texture_paths = BTreeSet::new();
    for root in decoded
        .iter()
        .map(|(_, _, root)| root)
        .chain(attachments.iter().map(|a| &a.root))
    {
        for mesh in root["Meshes"].as_array().into_iter().flatten() {
            for binding in mesh["MaterialBindings"].as_array().into_iter().flatten() {
                for map in binding["Material"]["Maps"].as_array().into_iter().flatten() {
                    if ["AlbedoTexture", "NormalTexture", "ORMTexture"]
                        .contains(&map["Usage"].as_str().unwrap_or(""))
                    {
                        if let Some(path) = map["Map"]["Texture"]["FromFileName"]
                            .as_str()
                            .filter(|p| exists(p))
                        {
                            texture_paths.insert(path.to_owned());
                        }
                    }
                }
            }
        }
    }
    access.prefetch(&texture_paths.into_iter().collect::<Vec<_>>())?;
    let animation_bytes = combined
        .iter()
        .map(|p| access.granny(p))
        .collect::<Result<Vec<_>, _>>()?;
    let mut sources = Vec::new();
    for bytes in &animation_bytes {
        sources.push(Granny::new(bytes)?.animations()?);
    }
    // Only a model with several sources drops repeated names, so single-source output is unchanged.
    let dedupe = sources.len() > 1;
    let mut seen = BTreeSet::new();
    let animations: Box<dyn Iterator<Item = Result<serde_json::Value, String>> + '_> =
        Box::new(sources.into_iter().flatten().filter(move |a| {
            let Ok(value) = a else { return true };
            let name = value
                .get("Name")
                .and_then(serde_json::Value::as_str)
                .unwrap_or("");
            (clips.is_empty()
                || clips
                    .iter()
                    .any(|pattern| crate::mapping::glob(pattern, name)))
                && (!dedupe || seen.insert(name.to_owned()))
        }));
    model::build_stream_attached(
        &unit.name,
        &skeleton,
        &refs,
        &attachment_refs,
        animations,
        unit.root_scale,
        |path, usage| {
            // Some units reference optional textures absent in an install. Keep
            // the material usable without that map, as the legacy importer did.
            if !exists(path) {
                return Ok(None);
            }
            png_with_dye(access, path, usage, dyes, texture_limit)
        },
    )
}
