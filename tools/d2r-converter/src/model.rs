//! Assemble decoded Granny skeletons, meshes and animation tracks into glTF.
use crate::{
    curves::{self, Kind},
    gltf::{Gltf, FLOAT, UINT, USHORT},
};
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};

pub struct Part<'a> {
    pub part: &'a str,
    pub variant: &'a str,
    pub root: &'a Value,
}
/// Rigid equipment model parented to a skeleton bone, with transforms in metres.
pub struct Attachment<'a> {
    pub name: &'a str,
    pub bone: &'a str,
    pub root: &'a Value,
    pub translation: [f64; 3],
    pub rotation: [f64; 4],
    pub scale: [f64; 3],
}
pub struct ModelOutput {
    pub glb: Vec<u8>,
    pub bones: usize,
    pub meshes: usize,
    pub clips: Vec<String>,
}
fn arr<'a>(v: &'a Value, key: &str) -> Result<&'a Vec<Value>, String> {
    v.get(key)
        .and_then(Value::as_array)
        .ok_or_else(|| format!("missing {key} array"))
}
fn name<'a>(v: &'a Value, key: &str) -> Result<&'a str, String> {
    v.get(key)
        .and_then(Value::as_str)
        .ok_or_else(|| format!("missing {key} string"))
}
fn number(v: &Value) -> Result<f64, String> {
    v.as_f64().ok_or_else(|| "expected number".into())
}
fn flag(v: &Value) -> bool {
    v.as_bool()
        .unwrap_or_else(|| v.as_f64().is_some_and(|value| value != 0.))
}
fn vector(v: &Value) -> Result<Vec<f64>, String> {
    v.as_array()
        .ok_or("expected vector")?
        .iter()
        .map(number)
        .collect()
}
/// Ground dressing baked into an object model: D2R's alpha-blended terrain decals (the sand and grunge
/// under the Act 2 `chest_3`). Without D2R's blend shader they import as opaque quads, and their desert
/// textures never match a Diablo floor, so they are left out.
fn is_ground_dressing(material: &Value) -> bool {
    material["Maps"].as_array().into_iter().flatten().any(|map| {
        map["Usage"].as_str() == Some("AlbedoTexture")
            && map["Map"]["Texture"]["FromFileName"].as_str().is_some_and(|path| {
                let path = path.replace('\\', "/").to_ascii_lowercase();
                path.contains("env/texture/global/decal/") || path.contains("env/texture/terrain/")
            })
    })
}

fn f32bytes(values: &[f64]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|x| (*x as f32).to_le_bytes())
        .collect()
}
fn accessor_f32(
    g: &mut Gltf,
    rows: &[Vec<f64>],
    components: usize,
    bounds: bool,
    target: Option<u32>,
) -> Result<usize, String> {
    if rows.is_empty()
        || rows
            .iter()
            .any(|v| v.len() != components || v.iter().any(|x| !x.is_finite()))
    {
        return Err("invalid float accessor".into());
    }
    let mut min = vec![f64::INFINITY; components];
    let mut max = vec![f64::NEG_INFINITY; components];
    let mut vals = Vec::with_capacity(rows.len() * components);
    for row in rows {
        for (i, &x) in row.iter().enumerate() {
            // The accessor payload stores f32; its declared bounds must match
            // those rounded values, not the pre-conversion f64 intermediates.
            let encoded = x as f32 as f64;
            min[i] = min[i].min(encoded);
            max[i] = max[i].max(encoded);
            vals.push(x);
        }
    }
    g.accessor(
        &f32bytes(&vals),
        FLOAT,
        rows.len(),
        components,
        false,
        if bounds { Some((min, max)) } else { None },
        target,
    )
}
fn children(g: &mut Gltf, parent: usize, child: usize) {
    let n = &mut g.doc["nodes"][parent];
    if n.get("children").is_none() {
        n["children"] = json!([]);
    }
    n["children"].as_array_mut().unwrap().push(json!(child));
}
fn norm(q: &mut [f64]) {
    let l = q.iter().map(|x| x * x).sum::<f64>().sqrt();
    if l > 1e-8 {
        for x in q {
            *x /= l;
        }
    }
}
fn field_vec(v: &Value, key: &str, n: usize) -> Result<Vec<f64>, String> {
    let a = vector(v.get(key).ok_or_else(|| format!("missing {key}"))?)?;
    if a.len() != n {
        return Err(format!("invalid {key} vector"));
    }
    Ok(a)
}
fn scalar_array(v: &Value) -> Result<Vec<u32>, String> {
    v.as_array()
        .ok_or("missing indices")?
        .iter()
        .map(|v| {
            let n = if let Some(obj) = v.as_object() {
                obj.values().next().ok_or("empty index")?
            } else {
                v
            };
            n.as_i64()
                .map(|x| x as u32)
                .or_else(|| n.as_u64().map(|x| x as u32))
                .ok_or_else(|| "invalid index".into())
        })
        .collect()
}

/// Converts decoded roots. Textures are requested as already encoded PNG bytes.
/// The caller supplies skeleton and model roots read from GRFL, and optionally the combined animation root.
pub fn build<F>(
    unit: &str,
    skeleton: &Value,
    parts: &[Part<'_>],
    animations: Option<&Value>,
    root_scale: [f64; 3],
    texture_png: F,
) -> Result<ModelOutput, String>
where
    F: FnMut(&str, &str) -> Result<Option<Vec<u8>>, String>,
{
    let animations = match animations {
        Some(root) => arr(root, "Animations")?
            .iter()
            .cloned()
            .map(Ok)
            .collect::<Vec<_>>(),
        None => Vec::new(),
    };
    build_stream(unit, skeleton, parts, animations, root_scale, texture_png)
}

/// Like `build`, but consumes one decoded animation at a time to bound memory.
pub fn build_stream<F, I>(
    unit: &str,
    skeleton: &Value,
    parts: &[Part<'_>],
    animations: I,
    root_scale: [f64; 3],
    texture_png: F,
) -> Result<ModelOutput, String>
where
    F: FnMut(&str, &str) -> Result<Option<Vec<u8>>, String>,
    I: IntoIterator<Item = Result<Value, String>>,
{
    build_stream_attached(
        unit,
        skeleton,
        parts,
        &[],
        animations,
        root_scale,
        texture_png,
    )
}

pub fn build_stream_attached<F, I>(
    unit: &str,
    skeleton: &Value,
    parts: &[Part<'_>],
    attachments: &[Attachment<'_>],
    animations: I,
    root_scale: [f64; 3],
    mut texture_png: F,
) -> Result<ModelOutput, String>
where
    F: FnMut(&str, &str) -> Result<Option<Vec<u8>>, String>,
    I: IntoIterator<Item = Result<Value, String>>,
{
    let skel = arr(skeleton, "Skeletons")?.first().ok_or("no skeleton")?;
    let bones = arr(skel, "Bones")?;
    if bones.is_empty() {
        return Err("empty skeleton".into());
    }
    let upm = skeleton
        .get("ArtToolInfo")
        .and_then(|x| x.get("UnitsPerMeter"))
        .and_then(Value::as_f64)
        .filter(|v| *v > 0.)
        .unwrap_or(1.);
    let scale = 1. / upm;
    let mut g = Gltf::new("damnationx Rust D2R converter");
    let root = g.add(
        "nodes",
        json!({"name":unit,"scale":root_scale,"children":[]}),
    )?;
    g.doc["scenes"][0]["nodes"]
        .as_array_mut()
        .unwrap()
        .push(json!(root));
    let mut nodes = Vec::new();
    let mut bone_index = HashMap::new();
    let mut ibms = Vec::new();
    for (i, b) in bones.iter().enumerate() {
        let bone_name = name(b, "Name")?;
        bone_index.insert(bone_name.to_owned(), i);
        let tr = b.get("Transform").ok_or("missing bone transform")?;
        let flags = tr["flags"].as_u64().unwrap_or(0);
        let mut node = json!({"name":bone_name});
        if flags & 1 != 0 {
            let mut v = field_vec(tr, "position", 3)?;
            for x in &mut v {
                *x *= scale;
            }
            node["translation"] = json!(v);
        }
        if flags & 2 != 0 {
            let mut q = field_vec(tr, "orientation", 4)?;
            norm(&mut q);
            node["rotation"] = json!(q);
        }
        if flags & 4 != 0 {
            let matrix = tr["scale_shear"].as_array().ok_or("invalid scale shear")?;
            if matrix.len() != 3 {
                return Err("invalid scale shear".into());
            }
            let mut s = Vec::new();
            for (i, row) in matrix.iter().enumerate() {
                s.push(number(row.get(i).ok_or("invalid scale shear")?)?);
            }
            node["scale"] = json!(s);
        }
        nodes.push(g.add("nodes", node)?);
        let mut m = field_vec(b, "InverseWorldTransform", 16)?;
        for v in &mut m[12..15] {
            *v *= scale;
        }
        ibms.push(m);
    }
    for (i, b) in bones.iter().enumerate() {
        let parent = b["ParentIndex"].as_i64().unwrap_or(-1);
        if parent >= 0 {
            let p = usize::try_from(parent).map_err(|_| "invalid parent")?;
            if p >= bones.len() || p == i {
                return Err("invalid bone parent".into());
            }
            children(&mut g, nodes[p], nodes[i]);
        } else {
            children(&mut g, root, nodes[i]);
        }
    }
    let ibm = accessor_f32(&mut g, &ibms, 16, false, None)?;
    let skin=g.add("skins",json!({"name":name(skel,"Name")?,"joints":nodes,"inverseBindMatrices":ibm,"skeleton":nodes[0]}))?;
    let mut count = 0;
    let mut materials = HashMap::new();
    let mut textures = HashMap::new();
    let mut holders = HashMap::new();
    for (part_name, variant, part_root, attachment) in parts
        .iter()
        .map(|p| (p.part, p.variant, p.root, None))
        .chain(attachments.iter().map(|a| (a.name, "", a.root, Some(a))))
    {
        let holder = if let Some(a) = attachment {
            let node = if let Some(node) = holders.get(a.name) {
                *node
            } else {
                let bi = *bone_index
                    .get(a.bone)
                    .ok_or_else(|| format!("attachment bone {} missing", a.bone))?;
                let node = g.add("nodes", json!({"name":a.name,"translation":a.translation,"rotation":a.rotation,"scale":a.scale,"children":[]}))?;
                children(&mut g, nodes[bi], node);
                holders.insert(a.name, node);
                node
            };
            Some(node)
        } else {
            None
        };
        for mesh in arr(part_root, "Meshes")? {
            let mesh_name = name(mesh, "Name")?;
            if mesh_name.starts_with("death_")
                || mesh["ExtendedData"]["meshRenderLayer"].as_i64() == Some(16)
            {
                continue;
            }
            let verts = arr(&mesh["PrimaryVertexData"], "Vertices")?;
            if verts.is_empty() {
                continue;
            }
            let mut pos = Vec::new();
            let mut normals = Vec::new();
            let mut tangents = Vec::new();
            let mut colors = Vec::new();
            let mut uv = Vec::new();
            let mut joints = Vec::new();
            let mut weights = Vec::new();
            let has_normal = verts[0].get("Normal").is_some();
            let has_tangent = verts[0]["Tangent"].as_array().is_some_and(|a| a.len() == 4);
            let has_color = verts[0]["DiffuseColor0"]
                .as_array()
                .is_some_and(|a| a.len() == 4);
            let has_uv = verts[0].get("TextureCoordinates0").is_some();
            let bindings = if attachment.is_some() {
                Vec::new()
            } else {
                mesh["BoneBindings"].as_array().cloned().unwrap_or_default()
            };
            for v in verts {
                let mut p = vector(&v["Position"])?;
                if p.len() < 3 {
                    return Err("position too short".into());
                }
                p.truncate(3);
                let vertex_scale = if v["Position"][0].is_i64() {
                    mesh["ExtendedData"]["VertexScale"].as_f64().unwrap_or(1.) / 32767.
                } else {
                    1.
                };
                for x in &mut p {
                    *x *= vertex_scale * scale;
                }
                pos.push(p);
                if has_normal {
                    let mut n = vector(&v["Normal"])?;
                    n.truncate(3);
                    if n.len() != 3 {
                        return Err("normal too short".into());
                    }
                    norm(&mut n);
                    if n.iter().all(|x| *x == 0.) {
                        n = vec![0., 1., 0.];
                    }
                    normals.push(n);
                }
                if has_tangent {
                    let tangent = vector(&v["Tangent"])?;
                    if tangent.len() != 4 {
                        return Err("invalid tangent".into());
                    }
                    let mut xyz = tangent[..3].to_vec();
                    norm(&mut xyz);
                    if xyz.iter().all(|x| *x == 0.) {
                        let n = normals.last().cloned().unwrap_or_else(|| vec![0., 1., 0.]);
                        let axis = if n[0].abs() < 0.9 {
                            [1., 0., 0.]
                        } else {
                            [0., 0., 1.]
                        };
                        xyz = vec![
                            n[1] * axis[2] - n[2] * axis[1],
                            n[2] * axis[0] - n[0] * axis[2],
                            n[0] * axis[1] - n[1] * axis[0],
                        ];
                        norm(&mut xyz);
                    }
                    xyz.push(if tangent[3] < 0. { -1. } else { 1. });
                    tangents.push(xyz);
                }
                if has_uv {
                    let mut t = vector(&v["TextureCoordinates0"])?;
                    if t.len() != 2 {
                        return Err("invalid UV".into());
                    }
                    t[1] = 1. - t[1];
                    uv.push(t);
                }
                if has_color {
                    let color = v["DiffuseColor0"]
                        .as_array()
                        .ok_or("invalid vertex color")?;
                    if color.len() != 4 {
                        return Err("invalid vertex color".into());
                    }
                    for component in color {
                        colors.push(
                            u8::try_from(component.as_u64().ok_or("invalid vertex color")?)
                                .map_err(|_| "invalid vertex color")?,
                        );
                    }
                }
                if !bindings.is_empty() {
                    let mut js = vec![0u16; 4];
                    let mut ws = vec![0.; 4];
                    if let (Some(indices), Some(w)) =
                        (v["BoneIndices"].as_array(), v["BoneWeights"].as_array())
                    {
                        for i in 0..indices.len().min(w.len()).min(4) {
                            let local = indices[i].as_u64().unwrap_or(0) as usize;
                            let bn = bindings
                                .get(local)
                                .and_then(|x| x["BoneName"].as_str())
                                .unwrap_or("");
                            js[i] = *bone_index.get(bn).unwrap_or(&0) as u16;
                            ws[i] = number(&w[i])?;
                        }
                    } else {
                        let bn = bindings[0]["BoneName"].as_str().unwrap_or("");
                        js[0] = *bone_index.get(bn).unwrap_or(&0) as u16;
                        ws[0] = 1.;
                    }
                    let total = ws.iter().sum::<f64>();
                    if total <= 0. {
                        ws[0] = 1.;
                        js[0] = 0;
                    } else {
                        for x in &mut ws {
                            *x /= total;
                        }
                    }
                    for i in 0..4 {
                        if ws[i] == 0. {
                            js[i] = 0;
                        }
                    }
                    joints.push(js);
                    weights.push(ws);
                }
            }
            let mut attrs = json!({"POSITION":accessor_f32(&mut g,&pos,3,true,Some(34962))?});
            if has_normal {
                attrs["NORMAL"] = json!(accessor_f32(&mut g, &normals, 3, false, Some(34962))?);
            }
            if has_tangent {
                attrs["TANGENT"] = json!(accessor_f32(&mut g, &tangents, 4, false, Some(34962))?);
            }
            if has_uv {
                attrs["TEXCOORD_0"] = json!(accessor_f32(&mut g, &uv, 2, false, Some(34962))?);
            }
            if has_color {
                attrs["COLOR_0"] = json!(g.accessor(
                    &colors,
                    crate::gltf::UBYTE,
                    verts.len(),
                    4,
                    true,
                    None,
                    Some(34962)
                )?);
            }
            if !joints.is_empty() {
                let bytes: Vec<u8> = joints
                    .iter()
                    .flat_map(|r| r.iter().flat_map(|v| v.to_le_bytes()))
                    .collect();
                attrs["JOINTS_0"] =
                    json!(g.accessor(&bytes, USHORT, joints.len(), 4, false, None, Some(34962))?);
                attrs["WEIGHTS_0"] = json!(accessor_f32(&mut g, &weights, 4, false, Some(34962))?);
            }
            let topo = &mesh["PrimaryTopology"];
            let idx32 = scalar_array(&topo["Indices"])?;
            let indices = if idx32.is_empty() {
                scalar_array(&topo["Indices16"])?
                    .into_iter()
                    .map(|x| x & 65535)
                    .collect()
            } else {
                idx32
            };
            let groups = topo["Groups"].as_array().cloned().unwrap_or_default();
            let groups = if groups.is_empty() {
                vec![json!({"MaterialIndex":0,"TriFirst":0,"TriCount":indices.len()/3})]
            } else {
                groups
            };
            let mats = mesh["MaterialBindings"]
                .as_array()
                .cloned()
                .unwrap_or_default();
            let mut primitives = Vec::new();
            for group in groups {
                let start = group["TriFirst"].as_u64().unwrap_or(0) as usize;
                let n = group["TriCount"].as_u64().unwrap_or(0) as usize;
                let start = start.checked_mul(3).ok_or("index overflow")?;
                let end = start
                    .checked_add(n.checked_mul(3).ok_or("index overflow")?)
                    .ok_or("index overflow")?;
                let tri = indices.get(start..end).ok_or("invalid triangle range")?;
                if tri.is_empty() {
                    continue;
                }
                if tri.iter().any(|i| *i as usize >= verts.len()) {
                    return Err("triangle index outside vertex buffer".into());
                }
                let group_material = mats
                    .get(group["MaterialIndex"].as_u64().unwrap_or(0) as usize)
                    .and_then(|x| x.get("Material"));
                if group_material.is_some_and(is_ground_dressing) {
                    continue;
                }
                let bytes: Vec<u8> = tri.iter().flat_map(|i| i.to_le_bytes()).collect();
                let acc = g.accessor(&bytes, UINT, tri.len(), 1, false, None, Some(34963))?;
                let mut prim = json!({"attributes":attrs,"indices":acc});
                let mi = group["MaterialIndex"].as_u64().unwrap_or(0) as usize;
                if let Some(mat) = mats
                    .get(mi)
                    .and_then(|x| x.get("Material"))
                    .filter(|x| x.is_object())
                {
                    let mat_name = name(mat, "Name")?.to_owned();
                    let id = if let Some(&id) = materials.get(&mat_name) {
                        id
                    } else {
                        let mut out = json!({"name":mat_name,"pbrMetallicRoughness":{"metallicFactor":0.,"roughnessFactor":1.}});
                        for map in mat["Maps"].as_array().into_iter().flatten() {
                            let usage = map["Usage"].as_str().unwrap_or("");
                            let path = map["Map"]["Texture"]["FromFileName"].as_str();
                            if let Some(path) = path {
                                let kind = match usage {
                                    "AlbedoTexture" => "albedo",
                                    "NormalTexture" => "normal",
                                    "ORMTexture" => "orm",
                                    _ => continue,
                                };
                                let key = (path.to_lowercase(), kind.to_owned());
                                let tex = if let Some(t) = textures.get(&key) {
                                    *t
                                } else {
                                    let t = match texture_png(path, kind)? {
                                        Some(bytes) => Some(g.image(&bytes, path)?),
                                        None => None,
                                    };
                                    textures.insert(key, t);
                                    t
                                };
                                if let Some(t) = tex {
                                    match kind {
                                        "albedo" => {
                                            out["pbrMetallicRoughness"]["baseColorTexture"] =
                                                json!({"index":t})
                                        }
                                        "normal" => out["normalTexture"] = json!({"index":t}),
                                        "orm" => {
                                            out["pbrMetallicRoughness"]
                                                ["metallicRoughnessTexture"] = json!({"index":t});
                                            out["occlusionTexture"] = json!({"index":t});
                                        }
                                        _ => {}
                                    }
                                }
                            }
                        }
                        if out["pbrMetallicRoughness"]
                            .get("baseColorTexture")
                            .is_some()
                            && out["pbrMetallicRoughness"]
                                .get("metallicRoughnessTexture")
                                .is_some()
                        {
                            out["pbrMetallicRoughness"]["metallicFactor"] = json!(1.0);
                        }
                        if flag(&mat["ExtendedData"]["IsDoubleSided"]) {
                            out["doubleSided"] = json!(true);
                        }
                        if flag(&mat["ExtendedData"]["PrimaryAlphaThresholdEnabled"]) {
                            out["alphaMode"] = json!("MASK");
                            out["alphaCutoff"] = json!(mat["ExtendedData"]
                                ["PrimaryAlphaThreshold"]
                                .as_f64()
                                .unwrap_or(0.5));
                        }
                        let id = g.add("materials", out)?;
                        materials.insert(mat_name, id);
                        id
                    };
                    prim["material"] = json!(id);
                }
                primitives.push(prim);
            }
            if !primitives.is_empty() {
                let mi = g.add("meshes", json!({"name":mesh_name,"primitives":primitives}))?;
                let n = if let Some(parent) = holder {
                    let n = g.add("nodes", json!({"name":mesh_name,"mesh":mi}))?;
                    children(&mut g, parent, n);
                    n
                } else {
                    let n = g.add("nodes",json!({"name":format!("{}_{}:{}",part_name,variant,mesh_name),"mesh":mi,"skin":skin}))?;
                    g.doc["scenes"][0]["nodes"]
                        .as_array_mut()
                        .unwrap()
                        .push(json!(n));
                    n
                };
                let _ = n;
                count += 1;
            }
        }
    }
    if count == 0 {
        return Err("no converted meshes".into());
    }
    let mut clips = Vec::new();
    for anim in animations {
        let anim = anim?;
        let duration = anim["Duration"].as_f64().unwrap_or(0.).max(0.);
        let mut samplers = Vec::new();
        let mut channels = Vec::new();
        let mut done = HashSet::new();
        for group in arr(&anim, "TrackGroups")? {
            let group_name = group["Name"].as_str().unwrap_or("");
            if group_name != "root"
                && !parts
                    .iter()
                    .any(|p| group_name == format!("group_{}_{}", p.part, p.variant))
            {
                continue;
            }
            for tr in arr(group, "TransformTracks")? {
                let Some(&bi) = tr["Name"].as_str().and_then(|x| bone_index.get(x)) else {
                    continue;
                };
                if !done.insert(bi) {
                    continue;
                }
                for (field, path, kind) in [
                    ("PositionCurve", "translation", Kind::Position),
                    ("OrientationCurve", "rotation", Kind::Orientation),
                    ("ScaleShearCurve", "scale", Kind::Scale),
                ] {
                    let Some(c) = curves::decode(&tr[field]["CurveData"], kind)? else {
                        continue;
                    };
                    let mut times = Vec::new();
                    let mut vals: Vec<Vec<f64>> = Vec::new();
                    for (time, mut value) in c.times.into_iter().zip(c.values) {
                        let time = time.clamp(0., duration);
                        if times.last().is_some_and(|last: &f64| time - *last <= 1e-6) {
                            continue;
                        }
                        if kind == Kind::Position {
                            for v in &mut value {
                                *v *= scale;
                            }
                            if ["root", "translate", "anim"].contains(
                                &bones[bi]["Name"]
                                    .as_str()
                                    .unwrap_or("")
                                    .to_lowercase()
                                    .as_str(),
                            ) && !vals.is_empty()
                            {
                                value[0] = vals[0][0];
                                value[2] = vals[0][2];
                            }
                        }
                        if kind == Kind::Orientation {
                            norm(&mut value);
                            if let Some(prev) = vals.last() {
                                let dot: f64 = value.iter().zip(prev).map(|(a, b)| a * b).sum();
                                if dot < 0. {
                                    for v in &mut value {
                                        *v = -*v;
                                    }
                                }
                            }
                        }
                        times.push(time);
                        vals.push(value);
                    }
                    if times.is_empty() {
                        continue;
                    }
                    let input = accessor_f32(
                        &mut g,
                        &times.iter().map(|x| vec![*x]).collect::<Vec<_>>(),
                        1,
                        true,
                        None,
                    )?;
                    let output = accessor_f32(
                        &mut g,
                        &vals,
                        if kind == Kind::Orientation { 4 } else { 3 },
                        false,
                        None,
                    )?;
                    let si = samplers.len();
                    samplers.push(json!({"input":input,"output":output,"interpolation":"LINEAR"}));
                    channels.push(json!({"sampler":si,"target":{"node":nodes[bi],"path":path}}));
                }
            }
        }
        if !channels.is_empty() {
            let nm = name(&anim, "Name")?.to_owned();
            g.add(
                "animations",
                json!({"name":nm,"samplers":samplers,"channels":channels}),
            )?;
            clips.push(nm);
        }
    }
    Ok(ModelOutput {
        glb: g.finish()?,
        bones: bones.len(),
        meshes: count,
        clips,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn granny_integer_flags_are_truthy() {
        assert!(flag(&json!(1)));
        assert!(flag(&json!(true)));
        assert!(!flag(&json!(0)));
        assert!(!flag(&Value::Null));
    }
    #[test]
    fn builds_skinned_model_and_animation() {
        let skeleton = json!({"ArtToolInfo":{"UnitsPerMeter":2},"Skeletons":[{"Name":"rig","Bones":[{"Name":"root","ParentIndex":-1,"Transform":{"flags":0},"InverseWorldTransform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}]}]});
        let model = json!({"Meshes":[{"Name":"body","PrimaryVertexData":{"Vertices":[{"Position":[0.0,0.0,0.0]},{"Position":[1.0,0.0,0.0]},{"Position":[0.0,1.0,0.0]}]},"BoneBindings":[{"BoneName":"root"}],"PrimaryTopology":{"Indices":[],"Indices16":[{"Index":0},{"Index":1},{"Index":2}],"Groups":[]},"MaterialBindings":[]}]});
        let animations = json!({"Animations":[{"Name":"idle","Duration":1,"TrackGroups":[{"Name":"root","TransformTracks":[{"Name":"root","PositionCurve":{"CurveData":{"CurveDataHeader_D3Constant32f":{},"Controls":[1,2,3]}},"OrientationCurve":{"CurveData":null},"ScaleShearCurve":{"CurveData":null}}]}]}]});
        let output = build(
            "hero",
            &skeleton,
            &[Part {
                part: "body",
                variant: "lit",
                root: &model,
            }],
            Some(&animations),
            [1.; 3],
            |_, _| Ok(None),
        )
        .unwrap();
        assert_eq!(
            (output.bones, output.meshes, output.clips),
            (1, 1, vec!["idle".to_owned()])
        );
        let n = u32::from_le_bytes(output.glb[12..16].try_into().unwrap()) as usize;
        let doc: Value = serde_json::from_slice(&output.glb[20..20 + n]).unwrap();
        assert_eq!(
            doc["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"].is_number(),
            true
        );
        assert_eq!(
            doc["animations"][0]["channels"][0]["target"]["path"],
            "translation"
        );
    }
    #[test]
    fn parents_rigid_equipment_under_bone_without_skinning() {
        let skeleton = json!({"ArtToolInfo":{"UnitsPerMeter":1},"Skeletons":[{"Name":"rig","Bones":[{"Name":"R_weapon_attach","ParentIndex":-1,"Transform":{"flags":0},"InverseWorldTransform":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}]}]});
        let mesh = json!({"Meshes":[{"Name":"body","PrimaryVertexData":{"Vertices":[{"Position":[0.0,0.0,0.0]},{"Position":[1.0,0.0,0.0]},{"Position":[0.0,1.0,0.0]}]},"BoneBindings":[],"PrimaryTopology":{"Indices":[],"Indices16":[{"Index":0},{"Index":1},{"Index":2}],"Groups":[]},"MaterialBindings":[]}]});
        let attachment = Attachment {
            name: "attach:sword",
            bone: "R_weapon_attach",
            root: &mesh,
            translation: [1., 2., 3.],
            rotation: [0., 0., 0., 1.],
            scale: [1.; 3],
        };
        let output = build_stream_attached(
            "hero",
            &skeleton,
            &[Part {
                part: "body",
                variant: "",
                root: &mesh,
            }],
            &[attachment],
            std::iter::empty(),
            [1.; 3],
            |_, _| Ok(None),
        )
        .unwrap();
        let n = u32::from_le_bytes(output.glb[12..16].try_into().unwrap()) as usize;
        let doc: Value = serde_json::from_slice(&output.glb[20..20 + n]).unwrap();
        let nodes = doc["nodes"].as_array().unwrap();
        let (holder_id, holder) = nodes
            .iter()
            .enumerate()
            .find(|(_, n)| n["name"] == "attach:sword")
            .unwrap();
        let bone = nodes
            .iter()
            .find(|n| n["name"] == "R_weapon_attach")
            .unwrap();
        assert!(bone["children"]
            .as_array()
            .unwrap()
            .contains(&json!(holder_id)));
        let mesh_node = &nodes[holder["children"][0].as_u64().unwrap() as usize];
        assert!(mesh_node.get("skin").is_none());
    }

    #[test]
    fn terrain_decals_baked_into_objects_are_ground_dressing() {
        let material = |path: &str| json!({"Maps":[{"Usage":"AlbedoTexture","Map":{"Texture":{"FromFileName":path}}}]});
        assert!(is_ground_dressing(&material("data/hd/env/texture/global/decal/act2/town/decal_sand_fade01_ALB.texture")));
        assert!(is_ground_dressing(&material("Data\\HD\\env\\texture\\terrain\\act2\\town\\act2_town_sand_base01_alb.texture")));
        assert!(!is_ground_dressing(&material("data/hd/objects/chests/chest_3/texture/chest_3_alb.texture")));
        assert!(!is_ground_dressing(&material("data/hd/objects/traps/floor_trap/texture/floor_trap_alb.texture")));
    }
}
