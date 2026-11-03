//! Compare owned exported models: glb_compare LEGACY.glb RUST.glb
//! Compares mesh attributes and animation channels by names; epsilon 1e-4.
use serde_json::Value;
use std::{env, fs};
struct Glb {
    doc: Value,
    binary: Vec<u8>,
}
impl Glb {
    fn read(path: &std::ffi::OsStr) -> Result<Self, String> {
        let b = fs::read(path).map_err(|e| e.to_string())?;
        if b.get(..4) != Some(b"glTF") || b.get(16..20) != Some(b"JSON") {
            return Err("invalid GLB".into());
        }
        let n =
            u32::from_le_bytes(b.get(12..16).ok_or("truncated GLB")?.try_into().unwrap()) as usize;
        let end = 20usize.checked_add(n).ok_or("GLB overflow")?;
        let doc = serde_json::from_slice(b.get(20..end).ok_or("truncated JSON")?)
            .map_err(|e| e.to_string())?;
        if b.get(end + 4..end + 8) != Some(b"BIN\0") {
            return Err("missing BIN".into());
        }
        Ok(Self {
            doc,
            binary: b.get(end + 8..).ok_or("truncated BIN")?.to_vec(),
        })
    }
    fn values(&self, i: usize) -> Result<Vec<f64>, String> {
        let a = self.doc["accessors"].get(i).ok_or("accessor missing")?;
        let view =
            &self.doc["bufferViews"][a["bufferView"].as_u64().ok_or("view missing")? as usize];
        let component = a["componentType"].as_u64().ok_or("component missing")?;
        let size = match component {
            5121 => 1,
            5123 => 2,
            5125 | 5126 => 4,
            _ => return Err("unsupported component".into()),
        };
        let lanes = match a["type"].as_str() {
            Some("SCALAR") => 1,
            Some("VEC2") => 2,
            Some("VEC3") => 3,
            Some("VEC4") => 4,
            Some("MAT4") => 16,
            _ => return Err("unsupported shape".into()),
        };
        let count = a["count"].as_u64().ok_or("count missing")? as usize;
        let stride = view["byteStride"].as_u64().unwrap_or((size * lanes) as u64) as usize;
        let offset = view["byteOffset"].as_u64().unwrap_or(0) as usize
            + a["byteOffset"].as_u64().unwrap_or(0) as usize;
        let normalized = a["normalized"].as_bool().unwrap_or(false);
        let mut out = Vec::with_capacity(count * lanes);
        for row in 0..count {
            for lane in 0..lanes {
                let p = offset + row * stride + lane * size;
                let b = self.binary.get(p..p + size).ok_or("truncated accessor")?;
                let mut x = match component {
                    5126 => f32::from_le_bytes(b.try_into().unwrap()) as f64,
                    5125 => u32::from_le_bytes(b.try_into().unwrap()) as f64,
                    5123 => u16::from_le_bytes(b.try_into().unwrap()) as f64,
                    _ => b[0] as f64,
                };
                if normalized {
                    x /= if component == 5121 { 255. } else { 65535. };
                }
                out.push(x);
            }
        }
        Ok(out)
    }
    fn node_name(&self, i: &Value) -> Option<&str> {
        self.doc["nodes"].get(i.as_u64()? as usize)?["name"].as_str()
    }
}
fn compare(
    a: &Glb,
    ai: &Value,
    b: &Glb,
    bi: &Value,
    tolerance: f64,
) -> Result<Option<f64>, String> {
    let x = a.values(ai.as_u64().ok_or("invalid accessor")? as usize)?;
    let y = b.values(bi.as_u64().ok_or("invalid accessor")? as usize)?;
    if x.len() != y.len() {
        return Ok(Some(f64::INFINITY));
    }
    let max = x
        .iter()
        .zip(&y)
        .fold(0f64, |m, (x, y)| m.max((x - y).abs()));
    Ok((max > tolerance).then_some(max))
}
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = env::args_os().collect();
    if args.len() != 3 {
        return Err("usage: glb_compare LEGACY.glb RUST.glb".into());
    }
    let a = Glb::read(&args[1])?;
    let b = Glb::read(&args[2])?;
    let mut checked = 0;
    let mut differences = Vec::new();
    for mesh in a.doc["meshes"].as_array().into_iter().flatten() {
        let name = mesh["name"].as_str().unwrap_or("");
        let Some(other) = b.doc["meshes"]
            .as_array()
            .and_then(|v| v.iter().find(|m| m["name"] == name))
        else {
            differences.push(format!("missing mesh {name}"));
            continue;
        };
        if mesh["primitives"].as_array().map(Vec::len)
            != other["primitives"].as_array().map(Vec::len)
        {
            differences.push(format!("{name}: primitive count differs"));
        }
        for (x, y) in mesh["primitives"]
            .as_array()
            .into_iter()
            .flatten()
            .zip(other["primitives"].as_array().into_iter().flatten())
        {
            if let (Some(ai), Some(bi)) = (x.get("indices"), y.get("indices")) {
                if let Some(delta) = compare(&a, ai, &b, bi, 0.)? {
                    differences.push(format!("{name} indices: max delta {delta}"));
                }
                checked += 1;
            } else if x.get("indices").is_some() != y.get("indices").is_some() {
                differences.push(format!("{name}: index presence differs"));
            }
            for key in [
                "POSITION",
                "NORMAL",
                "TANGENT",
                "TEXCOORD_0",
                "COLOR_0",
                "WEIGHTS_0",
            ] {
                let (ai, bi) = (x["attributes"].get(key), y["attributes"].get(key));
                if ai.is_some() != bi.is_some() {
                    differences.push(format!("{name} {key}: attribute presence differs"));
                }
                let (Some(ai), Some(bi)) = (ai, bi) else {
                    continue;
                };
                if let Some(delta) = compare(&a, ai, &b, bi, 1e-4)? {
                    differences.push(format!("{name} {key}: max delta {delta}"));
                }
                checked += 1;
            }
            if let (Some(aj), Some(bj), Some(aw)) = (
                x["attributes"].get("JOINTS_0"),
                y["attributes"].get("JOINTS_0"),
                x["attributes"].get("WEIGHTS_0"),
            ) {
                let joints_a = a.values(aj.as_u64().ok_or("invalid joints")? as usize)?;
                let joints_b = b.values(bj.as_u64().ok_or("invalid joints")? as usize)?;
                let weights = a.values(aw.as_u64().ok_or("invalid weights")? as usize)?;
                if joints_a.len() != joints_b.len()
                    || joints_a.len() != weights.len()
                    || joints_a
                        .iter()
                        .zip(&joints_b)
                        .zip(&weights)
                        .any(|((a, b), w)| *w > 0. && a != b)
                {
                    differences.push(format!("{name}: weighted joint indices differ"));
                }
                checked += 1;
            } else if x["attributes"].get("JOINTS_0").is_some()
                != y["attributes"].get("JOINTS_0").is_some()
            {
                differences.push(format!("{name}: joint attribute presence differs"));
            }
        }
    }
    for anim in a.doc["animations"].as_array().into_iter().flatten() {
        let name = anim["name"].as_str().unwrap_or("");
        let Some(other) = b.doc["animations"]
            .as_array()
            .and_then(|v| v.iter().find(|m| m["name"] == name))
        else {
            differences.push(format!("missing animation {name}"));
            continue;
        };
        for channel in anim["channels"].as_array().into_iter().flatten() {
            let node = a.node_name(&channel["target"]["node"]);
            let path = &channel["target"]["path"];
            let Some(target) = other["channels"].as_array().and_then(|v| {
                v.iter().find(|x| {
                    b.node_name(&x["target"]["node"]) == node && x["target"]["path"] == *path
                })
            }) else {
                differences.push(format!("missing channel {name} {node:?} {path}"));
                continue;
            };
            let sam =
                &anim["samplers"][channel["sampler"].as_u64().ok_or("sampler missing")? as usize];
            let tgt =
                &other["samplers"][target["sampler"].as_u64().ok_or("sampler missing")? as usize];
            for key in ["input", "output"] {
                if let Some(delta) = compare(&a, &sam[key], &b, &tgt[key], 1e-4)? {
                    differences.push(format!("{name} {node:?} {path} {key}: max delta {delta}"));
                }
                checked += 1;
            }
        }
    }
    println!(
        "Compared {checked} attribute and animation accessors; {} differences > 1e-4",
        differences.len()
    );
    for line in differences.iter().take(30) {
        println!("{line}");
    }
    if !differences.is_empty() {
        return Err("GLB parity differences found".into());
    }
    Ok(())
}
