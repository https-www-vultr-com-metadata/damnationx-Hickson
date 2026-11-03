//! Export the owned town portal's texture fields and static particle geometry.
//! PopcornFX simulation is not decoded; the game animates these assets itself.
use crate::{granny::Granny, import_model::AssetAccess, static_model, texture};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

const UNIT: &str = "data/hd/objects/vfx_only/town_portal.json";
const HOST_TEXTURE: &str = "data/hd/vfx/textures/gradient/t_fx_portal_gradient_03.texture";
pub struct PortalOutput {
    pub files: BTreeMap<String, Vec<u8>>,
    pub manifest: Value,
}
fn sha(data: &[u8]) -> String {
    format!("{:x}", Sha256::digest(data))
}
fn lower(v: &str) -> String {
    v.replace('\\', "/").to_lowercase()
}
fn name(path: &str) -> Result<&str, String> {
    path.rsplit('/')
        .next()
        .and_then(|x| x.rsplit_once('.').map(|p| p.0))
        .ok_or_else(|| format!("invalid path {path}"))
}
/// Recover ASCII resource names embedded in the native particle blob.
pub fn embedded_resources(data: &[u8]) -> Vec<String> {
    let d: Vec<u8> = data
        .iter()
        .map(u8::to_ascii_lowercase)
        .map(|b| if b == b'\\' { b'/' } else { b })
        .collect();
    let mut found = BTreeSet::new();
    for (start, _) in d.windows(8).enumerate().filter(|(_, w)| *w == b"data/hd/") {
        let rest = &d[start..];
        let mut end = 0;
        for suffix in [
            b".texture".as_slice(),
            b".model",
            b".particles",
            b".json",
            b".skeleton",
            b".animation",
        ] {
            if let Some(i) = rest.windows(suffix.len()).position(|w| w == suffix) {
                let candidate = i + suffix.len();
                if rest[..candidate]
                    .iter()
                    .all(|b| b.is_ascii_alphanumeric() || b"_./-".contains(b))
                    && (end == 0 || candidate < end)
                {
                    end = candidate;
                }
            }
        }
        if end > 0 {
            found.insert(String::from_utf8_lossy(&rest[..end]).into_owned());
        }
    }
    found.into_iter().collect()
}
fn encode_png(image: &texture::Image) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut bytes, image.width, image.height);
        enc.set_compression(png::Compression::Default);
        enc.set_adaptive_filter(png::AdaptiveFilterType::Adaptive);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        let mut writer = enc.write_header().map_err(|e| e.to_string())?;
        writer
            .write_image_data(&image.rgba)
            .map_err(|e| e.to_string())?;
    }
    Ok(bytes)
}
fn resolve(path: &str, listing: &BTreeSet<String>) -> Option<String> {
    let path = lower(path);
    if listing.contains(&path) {
        return Some(path);
    }
    let lod = path
        .strip_suffix(".model")
        .map(|p| format!("{p}_lod0.model"));
    lod.filter(|p| listing.contains(p))
}
/// Paths in `files` are relative to the shared `glb/` cache directory.
pub fn build_town_portal(
    access: &mut impl AssetAccess,
    game_build: &str,
) -> Result<PortalOutput, String> {
    let listing: BTreeSet<_> = access.listing()?.into_iter().map(|p| lower(&p)).collect();
    let unit_bytes = access.read(UNIT)?;
    let unit: Value = serde_json::from_slice(
        unit_bytes
            .strip_prefix(&[0xef, 0xbb, 0xbf])
            .unwrap_or(&unit_bytes),
    )
    .map_err(|e| e.to_string())?;
    let particle = lower(
        unit["dependencies"]["particles"][0]["path"]
            .as_str()
            .ok_or("portal unit has no particle dependency")?,
    );
    let data = access.read(&particle)?;
    let mut requested: BTreeSet<_> = embedded_resources(&data).into_iter().collect();
    requested.insert(HOST_TEXTURE.into());
    let resolved = requested
        .into_iter()
        .map(|req| {
            let source = resolve(&req, &listing)
                .ok_or_else(|| format!("missing portal particle resource {req}"))?;
            Ok((req, source))
        })
        .collect::<Result<Vec<_>, String>>()?;
    access.prefetch(
        &resolved
            .iter()
            .map(|(_, source)| source.clone())
            .collect::<Vec<_>>(),
    )?;
    let mut files = BTreeMap::new();
    let mut textures = serde_json::Map::new();
    let mut meshes = serde_json::Map::new();
    let mut hashes = serde_json::Map::new();
    hashes.insert(UNIT.into(), json!(sha(&unit_bytes)));
    hashes.insert(particle.clone(), json!(sha(&data)));
    for (req, source) in resolved {
        let bytes = access.read(&source)?;
        hashes.insert(source.clone(), json!(sha(&bytes)));
        let key = name(&source)?.to_owned();
        if source.ends_with(".texture") {
            let image = texture::decode(&bytes, 0)?;
            let format = u16::from_le_bytes(
                bytes
                    .get(4..6)
                    .ok_or("truncated texture")?
                    .try_into()
                    .unwrap(),
            );
            let png = encode_png(&image)?;
            let path = format!("vfx/town_portal/textures/{key}.png");
            if textures.contains_key(&key) {
                return Err(format!("ambiguous texture basename {key}"));
            }
            textures.insert(key,json!({"source":source,"png":path,"sha256":sha(&png),"width":image.width,"height":image.height,"source_format":format,"decode":match format {31=>"RGBA8",57|58=>"BC1",61|62=>"BC3",63=>"BC4",65=>"BC5",_=>"unknown"},"export_channels":"RGBA8","sample_channels":match format {63=>"R",65=>"RG",_=>"RGBA"},"srgb":format==58||format==62,"native_resolution":true}));
            files.insert(path, png);
        } else if source.ends_with(".model") {
            let flat = access.granny(&source)?;
            let model = Granny::new(&flat)?.root()?;
            let out = static_model::build(&key, &model, false, true, |_, _| Ok(None))?;
            let path = format!("vfx/town_portal/meshes/{key}.glb");
            meshes.insert(key,json!({"source":source,"requested":req,"glb":path,"triangles":out.triangles,"bounds_min_m":out.bounds_min,"bounds_max_m":out.bounds_max,"sha256":sha(&out.glb),"native_vertex_color_preserved":true,"uv_v_flipped_from_maya":true}));
            files.insert(path, out.glb);
        }
    }
    let mut dependencies = Vec::new();
    if let Some(obj) = unit["dependencies"].as_object() {
        for (category, records) in obj {
            for record in records.as_array().into_iter().flatten() {
                if let Some(path) = record["path"].as_str() {
                    dependencies.push((category.clone(), lower(path)));
                }
            }
        }
    }
    for state in unit["dependencies"]["json"]
        .as_array()
        .into_iter()
        .flatten()
    {
        if let Some(path) = state["path"].as_str() {
            let path = lower(path);
            if listing.contains(&path) {
                let bytes = access.read(&path)?;
                hashes.insert(path, json!(sha(&bytes)));
                let v: Value = serde_json::from_slice(
                    bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(&bytes),
                )
                .map_err(|e| e.to_string())?;
                for anim in v["animations"].as_array().into_iter().flatten() {
                    if let Some(p) = anim["filename"].as_str() {
                        dependencies.push(("animations".into(), lower(p)));
                    }
                }
            }
        }
    }
    let stale: Vec<_> = dependencies
        .into_iter()
        .filter(|(_, p)| resolve(p, &listing).is_none())
        .map(|(category, requested)| json!({"category":category,"requested":requested}))
        .collect();
    let recipes = json!({
        "vfx.rs": sha(include_bytes!("vfx.rs")),
        "texture.rs": sha(include_bytes!("texture.rs")),
        "static_model.rs": sha(include_bytes!("static_model.rs")),
        "model.rs": sha(include_bytes!("model.rs")),
        "gltf.rs": sha(include_bytes!("gltf.rs")),
        "granny.rs": sha(include_bytes!("granny.rs")),
    });
    let manifest = json!({"schema_version":1,"game_build":game_build,"unit":UNIT,"particle":particle,"particle_playback":"not_decoded; genuine source fields and geometry for host-authored animation","mesh_units":"metres","mesh_up":"+Y","mesh_pivot":"native_authored_center","stale_unit_references":stale,"textures":textures,"meshes":meshes,"source_sha256":hashes,"import_recipe_sha256":recipes});
    files.insert(
        "vfx/town_portal/portal.resolved.json".into(),
        serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?,
    );
    Ok(PortalOutput { files, manifest })
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn resources_stop_at_suffix_and_deduplicate() {
        assert_eq!(
            embedded_resources(
                b"junkDATA\\HD\\vfx\\a.texture\x04data/hd/vfx/B.modeldata/hd/vfx/a.texture"
            ),
            vec!["data/hd/vfx/a.texture", "data/hd/vfx/b.model"]
        );
    }
}
