//! Platform-independent selection and placement of D2R environment sources.
use crate::{granny::Granny, import_model::AssetAccess};
use serde_json::Value;
use std::collections::BTreeSet;

const FEET: f64 = 1.0 / 3.2808399;
#[derive(Debug, Clone, PartialEq)]
pub struct Item {
    pub name: String,
    pub model: String,
    /// glTF column-major transform in metres
    pub matrix: [f64; 16],
}
fn identity() -> [f64; 16] {
    [
        1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.,
    ]
}
fn stem(path: &str) -> String {
    path.rsplit('/')
        .next()
        .unwrap_or(path)
        .strip_suffix(".model")
        .unwrap_or(path.rsplit('/').next().unwrap_or(path))
        .to_owned()
}
fn xyz(value: &Value, default: f64) -> [f64; 3] {
    ["x", "y", "z"].map(|k| value.get(k).and_then(Value::as_f64).unwrap_or(default))
}
fn array3(value: &Value, default: f64) -> [f64; 3] {
    std::array::from_fn(|i| value.get(i).and_then(Value::as_f64).unwrap_or(default))
}
fn glob(p: &str, value: &str) -> bool {
    let (p, v) = (p.as_bytes(), value.as_bytes());
    let (mut i, mut j, mut star, mut mark) = (0, 0, None, 0);
    while j < v.len() {
        if i < p.len() && (p[i] == b'?' || p[i] == v[j]) {
            i += 1;
            j += 1;
        } else if i < p.len() && p[i] == b'*' {
            star = Some(i);
            i += 1;
            mark = j;
        } else if let Some(s) = star {
            mark += 1;
            j = mark;
            i = s + 1;
        } else {
            return false;
        }
    }
    while i < p.len() && p[i] == b'*' {
        i += 1;
    }
    i == p.len()
}
fn transform(position: [f64; 3], q: [f64; 4], scale: [f64; 3]) -> [f64; 16] {
    let [x, y, z, w] = q;
    let mut m = identity();
    let r = [
        [
            1. - 2. * (y * y + z * z),
            2. * (x * y - z * w),
            2. * (x * z + y * w),
        ],
        [
            2. * (x * y + z * w),
            1. - 2. * (x * x + z * z),
            2. * (y * z - x * w),
        ],
        [
            2. * (x * z - y * w),
            2. * (y * z + x * w),
            1. - 2. * (x * x + y * y),
        ],
    ];
    for col in 0..3 {
        for row in 0..3 {
            m[col * 4 + row] = r[row][col] * scale[col];
        }
    }
    for row in 0..3 {
        m[12 + row] = position[row] * FEET;
    }
    m
}
fn bounds(
    access: &mut impl AssetAccess,
    listing: &BTreeSet<String>,
    model: &str,
) -> Result<([f64; 3], [f64; 3]), String> {
    let base = model.strip_suffix(".model").unwrap_or(model);
    let path = (0..5)
        .map(|i| format!("{base}_lod{i}.model"))
        .find(|p| listing.contains(&p.to_lowercase()))
        .unwrap_or_else(|| model.to_owned());
    let bytes = access.granny(&path)?;
    let root = Granny::new(&bytes)?.root()?;
    let mut low = [f64::INFINITY; 3];
    let mut high = [f64::NEG_INFINITY; 3];
    for mesh in root["Meshes"].as_array().into_iter().flatten() {
        if mesh["Name"].as_str().unwrap_or("").starts_with("death_") {
            continue;
        }
        let ext = &mesh["ExtendedData"];
        if ext["bbMin"].as_array().is_none() || ext["bbMax"].as_array().is_none() {
            continue;
        }
        let l = array3(&ext["bbMin"], f64::NAN);
        let h = array3(&ext["bbMax"], f64::NAN);
        for i in 0..3 {
            low[i] = low[i].min(l[i] * FEET);
            high[i] = high[i].max(h[i] * FEET);
        }
    }
    if low.iter().chain(high.iter()).any(|x| !x.is_finite()) {
        return Err(format!("{path}: model has no bounds"));
    }
    Ok((low, high))
}
/// Select all source geometry and author transforms for a model recipe.
pub fn spec_items(access: &mut impl AssetAccess, spec: &Value) -> Result<Vec<Item>, String> {
    let mut items = Vec::new();
    if let Some(model) = spec["model"].as_str() {
        items.push(Item {
            name: stem(model),
            model: model.into(),
            matrix: identity(),
        });
    }
    let listing: BTreeSet<String> = access
        .listing()?
        .into_iter()
        .map(|s| s.to_lowercase())
        .collect();
    for part in spec["parts"].as_array().into_iter().flatten() {
        let model = part["model"].as_str().ok_or("part has no model")?;
        let pos = array3(&part["pos"], 0.);
        let a = part["yaw"].as_f64().unwrap_or(0.).to_radians() / 2.;
        let scale = part["scale"].as_f64().unwrap_or(1.);
        let mut matrix = transform(pos, [0., a.sin(), 0., a.cos()], [scale; 3]);
        if part["center"]
            .as_bool()
            .or_else(|| spec["center_parts"].as_bool())
            .unwrap_or(false)
        {
            let (lo, hi) = bounds(access, &listing, model)?;
            let off = [-(lo[0] + hi[0]) / 2., -lo[1], -(lo[2] + hi[2]) / 2.];
            for row in 0..3 {
                matrix[12 + row] += (0..3)
                    .map(|col| matrix[col * 4 + row] * off[col])
                    .sum::<f64>();
            }
        }
        items.push(Item {
            name: stem(model),
            model: model.into(),
            matrix,
        });
    }
    if let Some(preset) = spec["preset"].as_str() {
        let source = access.read(preset)?;
        let doc: Value =
            serde_json::from_slice(source.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(&source))
                .map_err(|e| format!("{preset}: {e}"))?;
        let region = spec["region"]
            .as_array()
            .filter(|a| a.len() == 4)
            .ok_or("preset requires four-element region")?;
        let region: Vec<f64> = region
            .iter()
            .map(|x| x.as_f64().ok_or("invalid region"))
            .collect::<Result<_, _>>()?;
        let includes: Vec<String> = spec["include"]
            .as_array()
            .map(|a| {
                a.iter()
                    .filter_map(Value::as_str)
                    .map(|s| format!("*{}", s.to_lowercase()))
                    .collect()
            })
            .unwrap_or_else(|| vec!["*".into()]);
        let excludes: Vec<String> = spec["exclude"]
            .as_array()
            .map(|a| {
                a.iter()
                    .filter_map(Value::as_str)
                    .map(|s| format!("*{}", s.to_lowercase()))
                    .collect()
            })
            .unwrap_or_default();
        for entity in doc["entities"].as_array().into_iter().flatten() {
            let Some(components) = entity["components"].as_array() else {
                continue;
            };
            if components
                .iter()
                .any(|c| c["type"] == "TerrainDefinitionComponent")
            {
                continue;
            }
            let tr = components
                .iter()
                .find(|c| c["type"] == "TransformDefinitionComponent");
            let md = components
                .iter()
                .find(|c| c["type"] == "ModelDefinitionComponent");
            let (Some(tr), Some(md)) = (tr, md) else {
                continue;
            };
            let Some(model) = md["filename"].as_str() else {
                continue;
            };
            let p = xyz(&tr["position"], 0.);
            if p[0] < region[0] || p[0] > region[2] || p[2] < region[1] || p[2] > region[3] {
                continue;
            }
            let lower = model.to_lowercase();
            if !includes.iter().any(|g| glob(g, &lower)) || excludes.iter().any(|g| glob(g, &lower))
            {
                continue;
            }
            let q = &tr["orientation"];
            let quat = [
                q["x"].as_f64().unwrap_or(0.),
                q["y"].as_f64().unwrap_or(0.),
                q["z"].as_f64().unwrap_or(0.),
                q["w"].as_f64().unwrap_or(1.),
            ];
            items.push(Item {
                name: entity["name"].as_str().unwrap_or("model").into(),
                model: model.into(),
                matrix: transform(p, quat, xyz(&tr["scale"], 1.)),
            });
        }
    }
    if items.is_empty() {
        return Err("environment recipe selects no models".into());
    }
    Ok(items)
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    struct Fixture;
    impl AssetAccess for Fixture {
        fn listing(&mut self) -> Result<Vec<String>, String> {
            Ok(vec![])
        }
        fn read(&mut self, _: &str) -> Result<Vec<u8>, String> {
            Ok(br#"{"entities":[{"name":"roof","components":[{"type":"ModelDefinitionComponent","filename":"data/cottage/roof.model"},{"type":"TransformDefinitionComponent","position":{"x":3,"y":0,"z":4},"orientation":{"w":1},"scale":{"x":1,"y":1,"z":1}}]},{"name":"terrain","components":[{"type":"TerrainDefinitionComponent"},{"type":"ModelDefinitionComponent","filename":"data/cottage/floor.model"},{"type":"TransformDefinitionComponent"}]}]}"#.to_vec())
        }
        fn granny(&mut self, _: &str) -> Result<Vec<u8>, String> {
            Err("unused".into())
        }
    }
    #[test]
    fn manual_parts_apply_feet_and_yaw() {
        let spec = json!({"parts":[{"model":"data/a.model","pos":[10,0,0],"yaw":90}]});
        let out = spec_items(&mut Fixture, &spec).unwrap();
        assert_eq!(out[0].name, "a");
        assert!((out[0].matrix[12] - 10. * FEET).abs() < 1e-8);
        assert!((out[0].matrix[2] + 1.).abs() < 1e-8);
    }
    #[test]
    fn globs_match_full_path() {
        assert!(glob("*cottage/*", "data/cottage/a.model"));
        assert!(!glob("*cottage/*", "data/castle/a.model"));
    }
    #[test]
    fn preset_filters_terrain_and_preserves_position() {
        let spec = json!({"preset":"p.json","region":[0,0,10,10],"include":["cottage/*"]});
        let out = spec_items(&mut Fixture, &spec).unwrap();
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].name, "roof");
        assert!((out[0].matrix[12] - 3. * FEET).abs() < 1e-8);
        assert!((out[0].matrix[14] - 4. * FEET).abs() < 1e-8);
    }
}

/// Combine separately converted static GLBs into one, rebasing all glTF indices
/// and applying each source's authored placement before optional recentering.
pub fn combine(
    items: &[(Item, crate::static_model::StaticOutput)],
    recenter: bool,
) -> Result<crate::static_model::StaticOutput, String> {
    use crate::gltf::Gltf;
    use serde_json::json;
    if items.is_empty() {
        return Err("environment has no static models".into());
    }
    let mut out = Gltf::new("damnationx Rust D2R environment");
    let root = out.add("nodes", json!({"name":"root","children":[]}))?;
    out.doc["scenes"][0]["nodes"] = json!([root]);
    let mut lo = [f64::INFINITY; 3];
    let mut hi = [f64::NEG_INFINITY; 3];
    let mut triangles = 0;
    for (item, model) in items {
        let bytes = &model.glb;
        if bytes.get(..4) != Some(b"glTF") || bytes.len() < 28 {
            return Err("invalid static GLB".into());
        }
        let json_len = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
        let end = 20usize.checked_add(json_len).ok_or("GLB overflow")?;
        let mut doc: Value = serde_json::from_slice(bytes.get(20..end).ok_or("truncated GLB")?)
            .map_err(|e| e.to_string())?;
        if bytes.get(end + 4..end + 8) != Some(b"BIN\0") {
            return Err("missing GLB buffer".into());
        }
        let bin_len = u32::from_le_bytes(
            bytes
                .get(end..end + 4)
                .ok_or("truncated GLB")?
                .try_into()
                .unwrap(),
        ) as usize;
        let bin = bytes
            .get(end + 8..end + 8 + bin_len)
            .ok_or("truncated GLB buffer")?;
        while out.bin.len() % 4 != 0 {
            out.bin.push(0);
        }
        let bin_offset = out.bin.len();
        out.bin.extend_from_slice(bin);
        let keys = [
            "nodes",
            "meshes",
            "materials",
            "textures",
            "images",
            "samplers",
            "accessors",
            "bufferViews",
        ];
        let offsets: std::collections::BTreeMap<&str, usize> = keys
            .iter()
            .map(|k| (*k, out.doc[*k].as_array().unwrap().len()))
            .collect();
        let add_index = |v: &mut Value, key: &str, offset: usize| {
            if let Some(n) = v.get(key).and_then(Value::as_u64) {
                v[key] = json!(n + offset as u64);
            }
        };
        for node in doc["nodes"].as_array_mut().into_iter().flatten() {
            add_index(node, "mesh", offsets["meshes"]);
            if let Some(children) = node["children"].as_array_mut() {
                for child in children {
                    let n = child.as_u64().ok_or("invalid child")?;
                    *child = json!(n + offsets["nodes"] as u64);
                }
            }
        }
        for mesh in doc["meshes"].as_array_mut().into_iter().flatten() {
            for primitive in mesh["primitives"].as_array_mut().into_iter().flatten() {
                add_index(primitive, "indices", offsets["accessors"]);
                add_index(primitive, "material", offsets["materials"]);
                if let Some(attrs) = primitive["attributes"].as_object_mut() {
                    for value in attrs.values_mut() {
                        let n = value.as_u64().ok_or("invalid attribute")?;
                        *value = json!(n + offsets["accessors"] as u64);
                    }
                }
            }
        }
        for material in doc["materials"].as_array_mut().into_iter().flatten() {
            for key in ["normalTexture", "occlusionTexture", "emissiveTexture"] {
                if let Some(texture) = material.get_mut(key) {
                    add_index(texture, "index", offsets["textures"]);
                }
            }
            if let Some(pbr) = material.get_mut("pbrMetallicRoughness") {
                for key in ["baseColorTexture", "metallicRoughnessTexture"] {
                    if let Some(texture) = pbr.get_mut(key) {
                        add_index(texture, "index", offsets["textures"]);
                    }
                }
            }
        }
        for texture in doc["textures"].as_array_mut().into_iter().flatten() {
            add_index(texture, "source", offsets["images"]);
            add_index(texture, "sampler", offsets["samplers"]);
        }
        for image in doc["images"].as_array_mut().into_iter().flatten() {
            add_index(image, "bufferView", offsets["bufferViews"]);
        }
        for accessor in doc["accessors"].as_array_mut().into_iter().flatten() {
            add_index(accessor, "bufferView", offsets["bufferViews"]);
        }
        for view in doc["bufferViews"].as_array_mut().into_iter().flatten() {
            let start = view["byteOffset"].as_u64().unwrap_or(0);
            view["byteOffset"] = json!(start + bin_offset as u64);
        }
        let scene_nodes = doc["scenes"][0]["nodes"]
            .as_array()
            .ok_or("missing scene nodes")?
            .iter()
            .map(|x| {
                x.as_u64()
                    .ok_or("invalid scene node")
                    .map(|i| i + offsets["nodes"] as u64)
            })
            .collect::<Result<Vec<_>, _>>()?;
        for key in keys {
            if let Some(array) = doc[key].as_array_mut() {
                out.doc[key].as_array_mut().unwrap().append(array);
            }
        }
        let node = out.add(
            "nodes",
            json!({"name":item.name,"matrix":item.matrix,"children":scene_nodes}),
        )?;
        out.doc["nodes"][root]["children"]
            .as_array_mut()
            .unwrap()
            .push(json!(node));
        for x in [model.bounds_min[0], model.bounds_max[0]] {
            for y in [model.bounds_min[1], model.bounds_max[1]] {
                for z in [model.bounds_min[2], model.bounds_max[2]] {
                    let v = [x, y, z];
                    for row in 0..3 {
                        let n = item.matrix[12 + row]
                            + (0..3)
                                .map(|col| item.matrix[col * 4 + row] * v[col])
                                .sum::<f64>();
                        lo[row] = lo[row].min(n);
                        hi[row] = hi[row].max(n);
                    }
                }
            }
        }
        triangles += model.triangles;
    }
    if recenter {
        let offset = [-(lo[0] + hi[0]) / 2., -lo[1], -(lo[2] + hi[2]) / 2.];
        out.doc["nodes"][root]["translation"] = json!(offset);
        for i in 0..3 {
            lo[i] += offset[i];
            hi[i] += offset[i];
        }
    }
    deduplicate_views(&mut out)?;
    out.doc["asset"]["extras"] = json!({"units":"metres","up":"+Y","forward":"+Z","normal_convention":"d2r_directx","rt_representation":"triangles"});
    Ok(crate::static_model::StaticOutput {
        glb: out.finish()?,
        bounds_min: lo,
        bounds_max: hi,
        triangles,
    })
}

fn deduplicate_views(gltf: &mut crate::gltf::Gltf) -> Result<(), String> {
    use sha2::{Digest, Sha256};
    let old = std::mem::take(&mut gltf.bin);
    let mut compact = Vec::new();
    let mut seen: std::collections::BTreeMap<[u8; 32], Vec<(usize, usize)>> =
        std::collections::BTreeMap::new();
    for view in gltf.doc["bufferViews"]
        .as_array_mut()
        .ok_or("missing buffer views")?
    {
        let offset = usize::try_from(view["byteOffset"].as_u64().unwrap_or(0))
            .map_err(|_| "view offset overflow")?;
        let len = usize::try_from(view["byteLength"].as_u64().ok_or("missing view length")?)
            .map_err(|_| "view length overflow")?;
        let source = old
            .get(offset..offset.checked_add(len).ok_or("view overflow")?)
            .ok_or("view outside buffer")?;
        let digest: [u8; 32] = Sha256::digest(source).into();
        let prior = seen
            .get(&digest)
            .and_then(|candidates| {
                candidates
                    .iter()
                    .find(|(p, n)| *n == len && compact.get(*p..*p + *n) == Some(source))
            })
            .map(|(p, _)| *p);
        let start = if let Some(start) = prior {
            start
        } else {
            while compact.len() % 4 != 0 {
                compact.push(0);
            }
            let start = compact.len();
            compact.extend_from_slice(source);
            seen.entry(digest).or_default().push((start, len));
            start
        };
        view["byteOffset"] = serde_json::json!(start);
    }
    gltf.bin = compact;
    Ok(())
}

/// Convert and combine source parts of one environment recipe.
pub fn convert_spec(
    access: &mut impl AssetAccess,
    spec: &Value,
    lod: usize,
) -> Result<crate::static_model::StaticOutput, String> {
    Ok(convert_spec_with_sockets(access, spec, lod, 1024)?.0)
}
fn convert_spec_with_sockets(
    access: &mut impl AssetAccess,
    spec: &Value,
    lod: usize,
    texture_limit: u32,
) -> Result<(crate::static_model::StaticOutput, Vec<Value>), String> {
    let items = spec_items(access, spec)?;
    let listing: BTreeSet<_> = access
        .listing()?
        .into_iter()
        .map(|s| s.to_lowercase())
        .collect();
    let exists = |p: &str| listing.contains(&p.replace('\\', "/").to_lowercase());
    let mut converted = Vec::new();
    for item in items {
        let path = crate::unit::model_file(&item.model, lod, exists);
        if !exists(&path) {
            return Err(format!("environment model missing: {path}"));
        }
        let bytes = access.granny(&path)?;
        let root = Granny::new(&bytes)?.root()?;
        let model = crate::static_model::build(&item.name, &root, false, false, |p, u| {
            if exists(p) {
                environment_png(access, p, u, texture_limit)
            } else {
                Ok(None)
            }
        })?;
        converted.push((item, model));
    }
    let mut sockets = Vec::new();
    if let Some(architecture) = spec["architecture"].as_str() {
        if architecture != "diablo1_cathedral_v1" {
            return Err(format!("unknown architecture {architecture}"));
        }
        let authored = crate::cathedral::build(lod as u32, |p, u| {
            environment_png(access, p, u, texture_limit)
        })?;
        sockets = authored.sockets;
        converted.push((
            Item {
                name: "authored_cathedral".into(),
                model: "authored".into(),
                matrix: identity(),
            },
            crate::static_model::StaticOutput {
                glb: authored.glb,
                bounds_min: authored.bounds_min,
                bounds_max: authored.bounds_max,
                triangles: authored.triangles,
            },
        ));
    }
    let model = combine(
        &converted,
        spec["recenter"].as_str().unwrap_or("footprint") == "footprint",
    )?;
    Ok((model, sockets))
}

#[cfg(test)]
mod combine_tests {
    use super::*;
    use crate::{gltf::Gltf, static_model::StaticOutput};
    use serde_json::json;
    fn fixture() -> StaticOutput {
        let mut g = Gltf::new("fixture");
        let position = g
            .accessor(
                &[0; 36],
                crate::gltf::FLOAT,
                3,
                3,
                false,
                Some((vec![0., 0., 0.], vec![1., 1., 1.])),
                None,
            )
            .unwrap();
        let indices = g
            .accessor(&[0; 6], crate::gltf::USHORT, 3, 1, false, None, None)
            .unwrap();
        let texture = g.image_png(b"png", "asset").unwrap();
        let material = g
            .add(
                "materials",
                json!({"pbrMetallicRoughness":{"baseColorTexture":{"index":texture}}}),
            )
            .unwrap();
        let mesh=g.add("meshes",json!({"primitives":[{"attributes":{"POSITION":position},"indices":indices,"material":material}]})).unwrap();
        let node = g.add("nodes", json!({"mesh":mesh})).unwrap();
        g.doc["scenes"][0]["nodes"] = json!([node]);
        StaticOutput {
            glb: g.finish().unwrap(),
            bounds_min: [0.; 3],
            bounds_max: [1.; 3],
            triangles: 1,
        }
    }
    #[test]
    fn composite_rebases_indices_and_applies_placement() {
        let first = Item {
            name: "first".into(),
            model: "a".into(),
            matrix: identity(),
        };
        let mut matrix = identity();
        matrix[12] = 3.;
        let second = Item {
            name: "second".into(),
            model: "b".into(),
            matrix,
        };
        let output = combine(&[(first, fixture()), (second, fixture())], true).unwrap();
        assert_eq!(output.triangles, 2);
        assert_eq!(output.bounds_min, [-2., 0., -0.5]);
        assert_eq!(output.bounds_max, [2., 1., 0.5]);
        let len = u32::from_le_bytes(output.glb[12..16].try_into().unwrap()) as usize;
        let doc: Value = serde_json::from_slice(&output.glb[20..20 + len]).unwrap();
        assert_eq!(doc["meshes"][1]["primitives"][0]["indices"], 3);
        assert_eq!(doc["meshes"][1]["primitives"][0]["material"], 1);
        assert_eq!(
            doc["materials"][1]["pbrMetallicRoughness"]["baseColorTexture"]["index"],
            1
        );
        assert_eq!(
            doc["bufferViews"][0]["byteOffset"],
            doc["bufferViews"][3]["byteOffset"]
        );
    }
}

/// Convert one biome/explicit-albedo terrain material, returning its host
/// manifest entry and PNGs under `env/terrain`.
pub fn convert_material(
    access: &mut impl AssetAccess,
    spec: &Value,
    texture_limit: u32,
) -> Result<(Value, std::collections::BTreeMap<String, Vec<u8>>), String> {
    use serde_json::json;
    use sha2::{Digest, Sha256};
    let listing: BTreeSet<_> = access
        .listing()?
        .into_iter()
        .map(|s| s.to_lowercase())
        .collect();
    let albedo = if let Some(source) = spec["albedo"].as_str() {
        source.to_owned()
    } else {
        let biome = spec["biome"].as_str().ok_or("material missing biome")?;
        let layer = spec["layer"].as_str().ok_or("material missing layer")?;
        let bytes = access.read(&format!("data/hd/env/biome/{biome}.json"))?;
        let bytes = bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(&bytes);
        let json: Value = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
        json["terrainDataUltra"]["terrainLayers"]
            .as_array()
            .ok_or("biome missing terrain layers")?
            .iter()
            .find(|x| x["name"].as_str() == Some(layer))
            .and_then(|x| x["Albedo"].as_str())
            .ok_or_else(|| format!("terrain layer {layer} not found"))?
            .to_owned()
    };
    if !albedo.to_ascii_lowercase().ends_with("alb.texture") {
        return Err(format!("invalid terrain albedo: {albedo}"));
    }
    let base = &albedo[..albedo.len() - "alb.texture".len()];
    let stem = albedo
        .rsplit('/')
        .next()
        .unwrap_or(&albedo)
        .trim_end_matches(".texture")
        .to_lowercase();
    let stem = stem.strip_suffix("_alb").unwrap_or(&stem);
    let mut files = std::collections::BTreeMap::new();
    let mut entry = serde_json::Map::new();
    let mut hashes = serde_json::Map::new();
    let max = spec["tex"]
        .as_u64()
        .unwrap_or(1024)
        .min(texture_limit as u64)
        .max(1) as u32;
    for (usage, suffix) in [("albedo", "ALB"), ("normal", "NRM"), ("orm", "ORM")] {
        let source = format!("{base}{suffix}.texture");
        if !listing.contains(&source.to_lowercase()) {
            continue;
        }
        let bytes = access.read(&source)?;
        let mut image = crate::texture::decode(&bytes, 0)?;
        if usage == "albedo" {
            if let Some(specification) = spec.get("height") {
                let (height, contract) = crate::height_terrain::encode(&image, specification, max)?;
                let path = format!("env/terrain/{stem}_height.png");
                hashes.insert(
                    "height".into(),
                    json!(format!("{:x}", Sha256::digest(&height))),
                );
                entry.insert("height".into(), json!(path));
                entry.insert("height_contract".into(), contract);
                files.insert(path, height);
            }
        }
        if usage == "normal" {
            for p in image.rgba.chunks_exact_mut(4) {
                let x = f32::from(p[3]) / 127.5 - 1.;
                let y = f32::from(p[1]) / 127.5 - 1.;
                let z = (1. - x * x - y * y).max(0.).sqrt();
                p[0] = ((x * 0.5 + 0.5) * 255.).round() as u8;
                p[1] = ((y * 0.5 + 0.5) * 255.).round() as u8;
                p[2] = ((z * 0.5 + 0.5) * 255.).round() as u8;
                p[3] = 255;
            }
        } else {
            for p in image.rgba.chunks_exact_mut(4) {
                p[3] = 255;
            }
        }
        let png = encode_resized(&image, max)?;
        let target = format!("env/terrain/{stem}_{}.png", suffix.to_lowercase());
        hashes.insert(usage.into(), json!(format!("{:x}", Sha256::digest(&png))));
        entry.insert(usage.into(), json!(target));
        files.insert(target, png);
    }
    if !entry.contains_key("albedo") {
        return Err(format!("terrain albedo is missing: {albedo}"));
    }
    entry.insert("texture_sha256".into(), Value::Object(hashes));
    entry.insert(
        "repeat_m".into(),
        spec.get("repeat_m").cloned().unwrap_or(json!(4.)),
    );
    entry.insert(
        "tint".into(),
        spec.get("tint").cloned().unwrap_or(json!([1., 1., 1.])),
    );
    entry.insert(
        "roughness".into(),
        spec.get("roughness").cloned().unwrap_or(json!(0.9)),
    );
    entry.insert("source".into(), json!(albedo));
    entry.insert("normal_convention".into(), json!("d2r_directx"));
    entry.insert("channels".into(),json!({"albedo":"srgb","normal":"linear","orm":"linear: R=AO G=roughness B=metallic","height":"linear"}));
    entry.insert("coverage".into(), json!("separate_material_weight"));
    entry.insert("emission".into(), json!("none"));
    entry.insert(
        "base_color_lighting".into(),
        json!("source_authored_unverified"),
    );
    Ok((Value::Object(entry), files))
}
fn encode_resized(image: &crate::texture::Image, max: u32) -> Result<Vec<u8>, String> {
    let input = image::RgbaImage::from_raw(image.width, image.height, image.rgba.clone())
        .ok_or("invalid texture pixels")?;
    let resized = if image.width.max(image.height) > max {
        let factor = max as f64 / image.width.max(image.height) as f64;
        image::imageops::resize(
            &input,
            ((image.width as f64 * factor).floor() as u32).max(1),
            ((image.height as f64 * factor).floor() as u32).max(1),
            image::imageops::FilterType::Lanczos3,
        )
    } else {
        input
    };
    let mut bytes = Vec::new();
    {
        let mut e = png::Encoder::new(&mut bytes, resized.width(), resized.height());
        e.set_color(png::ColorType::Rgba);
        e.set_depth(png::BitDepth::Eight);
        e.set_compression(png::Compression::Default);
        e.set_adaptive_filter(png::AdaptiveFilterType::Adaptive);
        let mut w = e.write_header().map_err(|e| e.to_string())?;
        w.write_image_data(&resized).map_err(|e| e.to_string())?;
    }
    Ok(bytes)
}
fn environment_png(
    access: &mut impl AssetAccess,
    path: &str,
    usage: &str,
    max: u32,
) -> Result<Option<Vec<u8>>, String> {
    let mut image = crate::texture::decode(&access.read(path)?, 0)?;
    if usage == "normal" {
        for pixel in image.rgba.chunks_exact_mut(4) {
            let x = f32::from(pixel[3]) / 127.5 - 1.;
            let y = f32::from(pixel[1]) / 127.5 - 1.;
            let z = (1. - x * x - y * y).max(0.).sqrt();
            pixel[0] = ((x * 0.5 + 0.5) * 255.).round() as u8;
            pixel[1] = ((y * 0.5 + 0.5) * 255.).round() as u8;
            pixel[2] = ((z * 0.5 + 0.5) * 255.).round() as u8;
            pixel[3] = 255;
        }
    } else if usage == "orm" {
        for pixel in image.rgba.chunks_exact_mut(4) {
            pixel[3] = 255;
        }
    }
    Ok(Some(encode_resized(&image, max.max(1))?))
}

/// Produce a complete environment package, emitting each binary as soon as it
/// is ready. The caller must only publish emitted files after this returns
/// successfully.
pub fn build<F>(
    access: &mut impl AssetAccess,
    recipe: &str,
    game_build: &str,
    texture_limit: u32,
    mut emit: F,
) -> Result<Value, String>
where
    F: FnMut(&str, &[u8]) -> Result<(), String>,
{
    use serde_json::json;
    use sha2::{Digest, Sha256};
    let parsed: toml::Value = toml::from_str(recipe).map_err(|e| e.to_string())?;
    let recipe_value = serde_json::to_value(parsed).map_err(|e| e.to_string())?;
    let scale = recipe_value["scale"].as_f64().unwrap_or(0.75);
    if !scale.is_finite() || scale <= 0. {
        return Err("invalid environment scale".into());
    }
    let mut models = serde_json::Map::new();
    let mut materials = serde_json::Map::new();
    for (key, spec) in recipe_value["models"]
        .as_object()
        .ok_or("missing environment model recipes")?
    {
        if !key
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')
        {
            return Err("invalid model key".into());
        }
        let lod = spec["lod"].as_u64().unwrap_or(0) as usize;
        let mut lods = vec![lod];
        for n in spec["lods"].as_array().into_iter().flatten() {
            let n = n.as_u64().ok_or("invalid environment LOD")? as usize;
            if !lods.contains(&n) {
                lods.push(n);
            }
        }
        if lods.len() > 3 || lods.iter().any(|n| *n > 4) {
            return Err("invalid environment LODs".into());
        }
        let mut entries = Vec::new();
        let mut authored_sockets = Vec::new();
        let mut facade_sockets = Vec::new();
        let mut aperture_model = None;
        let mut first = None;
        for (index, n) in lods.iter().enumerate() {
            let max = spec["tex"]
                .as_u64()
                .unwrap_or(1024)
                .min(texture_limit as u64)
                .max(1) as u32;
            let (model, sockets) = convert_spec_with_sockets(access, spec, *n, max)?;
            if index == 0 {
                authored_sockets = sockets;
            }
            if model
                .bounds_min
                .iter()
                .zip(model.bounds_max)
                .any(|(lo, hi)| !lo.is_finite() || !hi.is_finite() || *lo >= hi)
            {
                return Err(format!("{key} has invalid bounds"));
            }
            let name = if index == 0 {
                format!("env/models/{key}.glb")
            } else {
                format!("env/models/{key}_lod{n}.glb")
            };
            let digest = format!("{:x}", Sha256::digest(&model.glb));
            if index == 0 && (key.starts_with("cottage") || key == "cathedral") {
                if key.starts_with("cottage") {
                    facade_sockets = crate::sockets::facade_sockets(&model.glb)?;
                }
                aperture_model = Some(model.glb.clone());
            }
            emit(&name, &model.glb)?;
            entries.push(json!({"glb":name,"triangles":model.triangles,"sha256":digest,"distance_tiles":index as f64*14.}));
            if first.is_none() {
                first = Some((model.bounds_min, model.bounds_max, digest, name));
            }
        }
        let (lo, hi, hash, path) = first.ok_or("missing LOD")?;
        let pivot = spec["recenter"].as_str().unwrap_or("footprint");
        if !["footprint", "none"].contains(&pivot) {
            return Err("invalid pivot policy".into());
        }
        let mut sockets = Vec::new();
        let mut names = BTreeSet::new();
        for socket in spec["sockets"].as_array().into_iter().flatten() {
            let name = socket["name"].as_str().ok_or("socket has no name")?;
            if !names.insert(name.to_owned()) {
                return Err("duplicate socket".into());
            }
            let mut result = socket.as_object().ok_or("invalid socket")?.clone();
            result.entry("direction").or_insert(json!([0, 0, 1]));
            result.entry("purpose").or_insert(json!("debris"));
            sockets.push(Value::Object(result));
        }
        sockets.extend(authored_sockets);
        if key.starts_with("cottage") && sockets.is_empty() {
            sockets.extend(facade_sockets);
        }
        if let Some(glb) = aperture_model {
            sockets.retain(|s| s["purpose"] != "aperture");
            if let Some(mut aperture) = crate::sockets::aperture_socket(&glb, lo, hi, &sockets)? {
                aperture["source_glb_sha256"] = json!(hash);
                sockets.push(aperture);
            }
        }
        let contract = json!({"units":"metres","source_units":"feet","source_to_metres":FEET,"pivot":if pivot=="footprint"{"ground_footprint"}else{"authored"},"up":"+Y","forward":"+Z","tile_x":"+X","tile_y":"+Z","footprint":[lo[0],lo[2],hi[0],hi[2]],"sockets":sockets,"lods":entries,"rt_representation":"triangles","normal_convention":"d2r_directx","collision":"core_tiles_authoritative","tiles_per_metre":scale});
        models.insert(
            key.clone(),
            json!({"glb":path,"bounds_min":lo,"bounds_max":hi,"sha256":hash,"contract":contract}),
        );
    }
    for (key, spec) in recipe_value["materials"]
        .as_object()
        .ok_or("missing material recipes")?
    {
        let (entry, files) = convert_material(access, spec, texture_limit)?;
        for (name, bytes) in files {
            emit(&name, &bytes)?;
        }
        materials.insert(key.clone(), entry);
    }
    let levels = recipe_value.get("levels").cloned().unwrap_or(json!({}));
    for (level, value) in levels.as_object().ok_or("invalid levels")? {
        for layer in value["ground"].as_array().into_iter().flatten() {
            let name = layer["material"]
                .as_str()
                .ok_or("ground material missing")?;
            if !materials.contains_key(name) {
                return Err(format!("{level}: missing material {name}"));
            }
        }
        if let Some(name) = value["walls"].as_str() {
            if !materials.contains_key(name) {
                return Err(format!("{level}: missing material {name}"));
            }
        }
        let mut refs = Vec::new();
        for item in value["place"].as_array().into_iter().flatten() {
            if let Some(s) = item["model"].as_str() {
                refs.push(s);
            }
        }
        for card in value["cards"].as_array().into_iter().flatten() {
            refs.extend(
                card["models"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .filter_map(Value::as_str),
            );
        }
        if let Some(s) = value["bridge"]["model"].as_str() {
            refs.push(s);
        }
        for name in refs {
            if !models.contains_key(name) {
                return Err(format!("{level}: missing model {name}"));
            }
        }
    }
    Ok(
        json!({"version":2,"scale":scale,"models":models,"materials":materials,"levels":levels,"export_revision":2,"mapping_sha256":format!("{:x}",Sha256::digest(recipe.as_bytes())),"game_build":game_build,"source_policy":"player_owned_cache_only"}),
    )
}
