//! Convert one unskinned environment or particle model to a GLB.
use crate::model::{self, Part};
use serde_json::{json, Value};

/// Whether more than 1% of an embedded image (PNG or KTX2) is under half alpha.
fn alpha_coverage(png: &[u8]) -> Result<bool, String> {
    if crate::texture::is_ktx2(png) {
        let image = crate::texture::decode_ktx2(png)?;
        let count = image.rgba.len() / 4;
        let transparent = image.rgba.chunks_exact(4).filter(|p| p[3] < 128).count();
        return Ok(count > 0 && transparent * 100 > count);
    }
    let mut decoder = png::Decoder::new(png);
    decoder.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut reader = decoder.read_info().map_err(|e| e.to_string())?;
    let mut pixels = vec![0; reader.output_buffer_size()];
    let info = reader.next_frame(&mut pixels).map_err(|e| e.to_string())?;
    let (stride, alpha) = match info.color_type {
        png::ColorType::Rgba => (4, 3),
        png::ColorType::GrayscaleAlpha => (2, 1),
        _ => return Ok(false),
    };
    let pixels = &pixels[..info.buffer_size()];
    let count = pixels.len() / stride;
    let transparent = pixels
        .chunks_exact(stride)
        .filter(|p| p[alpha] < 128)
        .count();
    Ok(count > 0 && transparent * 100 > count)
}

pub struct StaticOutput {
    pub glb: Vec<u8>,
    pub bounds_min: [f64; 3],
    pub bounds_max: [f64; 3],
    pub triangles: usize,
}

/// `particle` preserves vertex color and replaces proprietary particle materials
/// with an unlit placeholder; the native simulation itself is not decoded.
pub fn build<F>(
    name: &str,
    root: &Value,
    recenter: bool,
    particle: bool,
    texture_png: F,
) -> Result<StaticOutput, String>
where
    F: FnMut(&str, &str) -> Result<Option<Vec<u8>>, String>,
{
    let upm = root["ArtToolInfo"]["UnitsPerMeter"]
        .as_f64()
        .filter(|x| *x > 0.)
        .unwrap_or(1.);
    let fake = json!({"ArtToolInfo":{"UnitsPerMeter":upm},"Skeletons":[{"Name":"static","Bones":[{"Name":"root","ParentIndex":-1,"Transform":{"flags":0},"InverseWorldTransform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}]}]});
    let result = model::build(
        name,
        &fake,
        &[Part {
            part: name,
            variant: "",
            root,
        }],
        None,
        [1.; 3],
        texture_png,
    )?;
    let data = result.glb;
    let json_len =
        u32::from_le_bytes(data.get(12..16).ok_or("truncated GLB")?.try_into().unwrap()) as usize;
    let end = 20usize.checked_add(json_len).ok_or("GLB overflow")?;
    let mut doc: Value = serde_json::from_slice(data.get(20..end).ok_or("truncated GLB JSON")?)
        .map_err(|e| e.to_string())?;
    let bin = data.get(end..).ok_or("truncated GLB buffer")?;
    if !particle {
        // Foliage and roof cards often carry coverage solely in albedo alpha.
        // Match the legacy environment importer when no explicit mask exists.
        let mut masks = Vec::new();
        for (index, material) in doc["materials"]
            .as_array()
            .into_iter()
            .flatten()
            .enumerate()
        {
            if material.get("alphaMode").is_some() {
                continue;
            }
            let Some(texture) =
                material["pbrMetallicRoughness"]["baseColorTexture"]["index"].as_u64()
            else {
                continue;
            };
            let Some(image) = doc["textures"][texture as usize]["source"].as_u64() else {
                continue;
            };
            let Some(view) = doc["images"][image as usize]["bufferView"].as_u64() else {
                continue;
            };
            let view = &doc["bufferViews"][view as usize];
            let offset = view["byteOffset"].as_u64().unwrap_or(0) as usize;
            let length = view["byteLength"].as_u64().ok_or("missing image length")? as usize;
            let start = 8usize.checked_add(offset).ok_or("image offset overflow")?;
            let end = start.checked_add(length).ok_or("image length overflow")?;
            if alpha_coverage(bin.get(start..end).ok_or("truncated embedded image")?)? {
                masks.push(index);
            }
        }
        for index in masks {
            doc["materials"][index]["alphaMode"] = json!("MASK");
            doc["materials"][index]["alphaCutoff"] = json!(0.5);
            doc["materials"][index]["doubleSided"] = json!(true);
        }
    }
    let mut lo = [f64::INFINITY; 3];
    let mut hi = [f64::NEG_INFINITY; 3];
    let mut tris = 0usize;
    for mesh in doc["meshes"].as_array_mut().ok_or("missing meshes")? {
        for primitive in mesh["primitives"]
            .as_array_mut()
            .ok_or("missing primitives")?
        {
            let p = primitive["attributes"]["POSITION"]
                .as_u64()
                .ok_or("missing position accessor")? as usize;
            let i = primitive["indices"]
                .as_u64()
                .ok_or("missing index accessor")? as usize;
            // Temporarily collect indices before mutating attributes; bounds are in accessor metadata.
            let _ = (p, i);
            primitive["attributes"]
                .as_object_mut()
                .unwrap()
                .remove("JOINTS_0");
            primitive["attributes"]
                .as_object_mut()
                .unwrap()
                .remove("WEIGHTS_0");
            if particle {
                primitive["material"] = json!(0);
            }
        }
    }
    for mesh in doc["meshes"].as_array().unwrap() {
        for primitive in mesh["primitives"].as_array().unwrap() {
            let p = primitive["attributes"]["POSITION"].as_u64().unwrap() as usize;
            let i = primitive["indices"].as_u64().unwrap() as usize;
            for axis in 0..3 {
                lo[axis] = lo[axis].min(
                    doc["accessors"][p]["min"][axis]
                        .as_f64()
                        .ok_or("missing position bounds")?,
                );
                hi[axis] = hi[axis].max(
                    doc["accessors"][p]["max"][axis]
                        .as_f64()
                        .ok_or("missing position bounds")?,
                );
            }
            tris += doc["accessors"][i]["count"]
                .as_u64()
                .ok_or("missing index count")? as usize
                / 3;
        }
    }
    if tris == 0 || lo.iter().chain(hi.iter()).any(|x| !x.is_finite()) {
        return Err("static model has no finite triangles".into());
    }
    for node in doc["nodes"].as_array_mut().ok_or("missing nodes")? {
        node.as_object_mut().ok_or("invalid node")?.remove("skin");
    }
    doc.as_object_mut().unwrap().remove("skins");
    if recenter {
        let off = [-(lo[0] + hi[0]) / 2., -lo[1], -(lo[2] + hi[2]) / 2.];
        for axis in 0..3 {
            lo[axis] += off[axis];
            hi[axis] += off[axis];
        }
        for n in doc["scenes"][0]["nodes"].as_array().unwrap().clone() {
            let i = n.as_u64().ok_or("invalid node index")? as usize;
            doc["nodes"][i]["translation"] = json!(off);
        }
    }
    if particle {
        doc["extensionsUsed"] = json!(["KHR_materials_unlit"]);
        doc["materials"] = json!([{"name":"particle_shader_required","doubleSided":true,"alphaMode":"BLEND","pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],"metallicFactor":0,"roughnessFactor":1},"extensions":{"KHR_materials_unlit":{}}}]);
    }
    doc["asset"]["extras"] = if particle {
        json!({"units":"metres","up":"+Y","pivot":"native_authored_center","uv_v_flipped_from_maya":true,"native_vertex_color_preserved":true,"particle_materials_and_simulation":"not_converted"})
    } else {
        json!({"units":"metres","up":"+Y","forward":"+Z","normal_convention":"d2r_directx","rt_representation":"triangles"})
    };
    let mut js = serde_json::to_vec(&doc).map_err(|e| e.to_string())?;
    while js.len() % 4 != 0 {
        js.push(b' ');
    }
    let total = 12usize
        .checked_add(8)
        .and_then(|x| x.checked_add(js.len()))
        .and_then(|x| x.checked_add(bin.len()))
        .ok_or("GLB too large")?;
    let total = u32::try_from(total).map_err(|_| "GLB exceeds 4 GiB")?;
    let mut out = Vec::with_capacity(total as usize);
    out.extend_from_slice(b"glTF");
    out.extend_from_slice(&2u32.to_le_bytes());
    out.extend_from_slice(&total.to_le_bytes());
    out.extend_from_slice(&(js.len() as u32).to_le_bytes());
    out.extend_from_slice(b"JSON");
    out.extend(js);
    out.extend_from_slice(bin);
    Ok(StaticOutput {
        glb: out,
        bounds_min: lo,
        bounds_max: hi,
        triangles: tris,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn png(alpha: &[u8]) -> Vec<u8> {
        let mut bytes = Vec::new();
        {
            let mut e = png::Encoder::new(&mut bytes, alpha.len() as u32, 1);
            e.set_color(png::ColorType::Rgba);
            e.set_depth(png::BitDepth::Eight);
            let pixels: Vec<u8> = alpha.iter().flat_map(|a| [255, 255, 255, *a]).collect();
            e.write_header().unwrap().write_image_data(&pixels).unwrap();
        }
        bytes
    }
    #[test]
    fn coverage_threshold_handles_opaque_and_cutout() {
        let mut opaque = vec![255; 100];
        opaque[0] = 0;
        assert!(!alpha_coverage(&png(&opaque)).unwrap());
        opaque[1] = 0;
        assert!(alpha_coverage(&png(&opaque)).unwrap());
    }
}
