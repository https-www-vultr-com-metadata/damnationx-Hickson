//! Geometry-derived cottage facade lights and measured doorway openings.
use serde_json::{json, Value};
type Mat = [[f64; 4]; 4];
type V = [f64; 3];
fn ident() -> Mat {
    [
        [1., 0., 0., 0.],
        [0., 1., 0., 0.],
        [0., 0., 1., 0.],
        [0., 0., 0., 1.],
    ]
}
fn multiply(a: Mat, b: Mat) -> Mat {
    let mut r = [[0.; 4]; 4];
    for i in 0..4 {
        for j in 0..4 {
            for k in 0..4 {
                r[i][j] += a[i][k] * b[k][j];
            }
        }
    }
    r
}
fn transform(m: Mat, p: V) -> V {
    [0, 1, 2].map(|i| m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3])
}
fn vec3(v: &Value, default: V) -> V {
    [0, 1, 2].map(|i| v.get(i).and_then(Value::as_f64).unwrap_or(default[i]))
}
fn local(n: &Value) -> Result<Mat, String> {
    if let Some(a) = n["matrix"].as_array() {
        if a.len() != 16 {
            return Err("invalid node matrix".into());
        }
        let mut m = ident();
        for i in 0..4 {
            for j in 0..4 {
                m[i][j] = a[j * 4 + i].as_f64().ok_or("invalid node matrix")?;
            }
        }
        return Ok(m);
    }
    let t = vec3(&n["translation"], [0.; 3]);
    let s = vec3(&n["scale"], [1.; 3]);
    let q = &n["rotation"];
    let x = q[0].as_f64().unwrap_or(0.);
    let y = q[1].as_f64().unwrap_or(0.);
    let z = q[2].as_f64().unwrap_or(0.);
    let w = q[3].as_f64().unwrap_or(1.);
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
    let mut m = ident();
    for i in 0..3 {
        for j in 0..3 {
            m[i][j] = r[i][j] * s[j];
        }
        m[i][3] = t[i];
    }
    Ok(m)
}
fn document(data: &[u8]) -> Result<(Value, &[u8]), String> {
    if data.get(..4) != Some(b"glTF") || data.get(16..20) != Some(b"JSON") {
        return Err("invalid GLB".into());
    }
    let n =
        u32::from_le_bytes(data.get(12..16).ok_or("truncated GLB")?.try_into().unwrap()) as usize;
    let end = 20usize.checked_add(n).ok_or("GLB overflow")?;
    let doc = serde_json::from_slice(data.get(20..end).ok_or("truncated GLB")?)
        .map_err(|e| format!("invalid GLB JSON: {e}"))?;
    if data.get(end + 4..end + 8) != Some(b"BIN\0") {
        return Err("missing GLB binary chunk".into());
    }
    Ok((doc, data.get(end + 8..).ok_or("truncated GLB")?))
}
fn array<'a>(v: &'a Value, key: &str) -> Result<&'a Vec<Value>, String> {
    v.get(key)
        .and_then(Value::as_array)
        .ok_or_else(|| format!("missing {key}"))
}
fn roots(doc: &Value) -> Result<Vec<usize>, String> {
    let scene = doc["scene"].as_u64().unwrap_or(0) as usize;
    array(&doc["scenes"][scene], "nodes")?
        .iter()
        .map(|v| {
            v.as_u64()
                .map(|v| v as usize)
                .ok_or_else(|| "invalid root node".into())
        })
        .collect()
}
fn visit<F: FnMut(&Value, Mat, bool) -> Result<(), String>>(
    doc: &Value,
    index: usize,
    parent: Mat,
    facade: bool,
    depth: usize,
    f: &mut F,
) -> Result<(), String> {
    if depth > 64 {
        return Err("GLB hierarchy exceeds socket traversal limit".into());
    }
    let n = doc["nodes"].get(index).ok_or("invalid node index")?;
    let world = multiply(parent, local(n)?);
    let name = n["name"].as_str().unwrap_or("");
    let facade = facade || name.contains("_front") || name.contains("cottages_cottage");
    f(n, world, facade)?;
    for child in n["children"].as_array().into_iter().flatten() {
        visit(
            doc,
            child.as_u64().ok_or("invalid child")? as usize,
            world,
            facade,
            depth + 1,
            f,
        )?;
    }
    Ok(())
}
fn primitives<'a>(doc: &'a Value, node: &Value) -> Result<Vec<&'a Value>, String> {
    let Some(i) = node["mesh"].as_u64() else {
        return Ok(Vec::new());
    };
    Ok(array(&doc["meshes"][i as usize], "primitives")?
        .iter()
        .collect())
}
/// Return doorway, window, and debris estimates from transformed front-wall bounds.
pub fn facade_sockets(data: &[u8]) -> Result<Vec<Value>, String> {
    let (doc, _) = document(data)?;
    let mut lo = [f64::INFINITY; 3];
    let mut hi = [f64::NEG_INFINITY; 3];
    let mut found = false;
    let mut f = |node: &Value, world: Mat, facade: bool| -> Result<(), String> {
        if !facade {
            return Ok(());
        }
        for p in primitives(&doc, node)? {
            let index = p["attributes"]["POSITION"]
                .as_u64()
                .ok_or("missing position")? as usize;
            let acc = &doc["accessors"][index];
            let a = vec3(&acc["min"], [f64::NAN; 3]);
            let b = vec3(&acc["max"], [f64::NAN; 3]);
            for x in [a[0], b[0]] {
                for y in [a[1], b[1]] {
                    for z in [a[2], b[2]] {
                        let v = transform(world, [x, y, z]);
                        if v.iter().any(|x| !x.is_finite()) {
                            return Err("invalid facade bounds".into());
                        }
                        for i in 0..3 {
                            lo[i] = lo[i].min(v[i]);
                            hi[i] = hi[i].max(v[i]);
                        }
                        found = true;
                    }
                }
            }
        }
        Ok(())
    };
    for root in roots(&doc)? {
        visit(&doc, root, ident(), false, 0, &mut f)?;
    }
    if !found {
        return Ok(Vec::new());
    }
    let mid = [0, 1, 2].map(|i| (lo[i] + hi[i]) / 2.);
    let h = (hi[1] - 0.35).clamp(1., 1.9);
    Ok(vec![
        json!({"name":"doorway_spill","at":[mid[0]-(hi[0]-lo[0])*0.19,h,hi[2]+0.18],"direction":[0,0,1],"purpose":"doorway","power_lumens":60000,"radius_tiles":4.5,"occlusion":"door_reveal","position_source":"exported_front_wall_bounds"}),
        json!({"name":"window_spill","at":[hi[0]+0.18,h,mid[2]+(hi[2]-lo[2])*0.19],"direction":[1,0,0],"purpose":"window","power_lumens":28000,"radius_tiles":4.5,"occlusion":"window_reveal","position_source":"exported_front_wall_bounds"}),
        json!({"name":"wall_base_debris","at":[hi[0],0.,hi[2]],"direction":[0,0,1],"purpose":"debris","position_source":"exported_front_wall_bounds"}),
    ])
}
fn accessor(doc: &Value, bin: &[u8], i: usize) -> Result<Vec<Vec<f64>>, String> {
    let a = doc["accessors"].get(i).ok_or("invalid accessor")?;
    let view = &doc["bufferViews"][a["bufferView"].as_u64().ok_or("invalid view")? as usize];
    let component = a["componentType"].as_u64().ok_or("missing component")?;
    let size = match component {
        5126 | 5125 => 4,
        5123 => 2,
        5121 => 1,
        _ => return Err("unsupported accessor component".into()),
    };
    let lanes = match a["type"].as_str() {
        Some("VEC3") => 3,
        Some("SCALAR") => 1,
        _ => return Err("unsupported accessor type".into()),
    };
    let count = a["count"].as_u64().ok_or("missing count")? as usize;
    let stride = view["byteStride"].as_u64().unwrap_or((size * lanes) as u64) as usize;
    let offset = view["byteOffset"].as_u64().unwrap_or(0) as usize
        + a["byteOffset"].as_u64().unwrap_or(0) as usize;
    let mut out = Vec::with_capacity(count);
    for row in 0..count {
        let mut v = Vec::with_capacity(lanes);
        for lane in 0..lanes {
            let p = offset
                .checked_add(row.checked_mul(stride).ok_or("accessor overflow")?)
                .and_then(|x| x.checked_add(lane * size))
                .ok_or("accessor overflow")?;
            let bytes = bin.get(p..p + size).ok_or("truncated accessor")?;
            v.push(match component {
                5126 => f32::from_le_bytes(bytes.try_into().unwrap()) as f64,
                5125 => u32::from_le_bytes(bytes.try_into().unwrap()) as f64,
                5123 => u16::from_le_bytes(bytes.try_into().unwrap()) as f64,
                _ => bytes[0] as f64,
            });
        }
        out.push(v);
    }
    Ok(out)
}
fn dot(a: V, b: V) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
/// Search projected wall triangles for a bounded doorway at two low heights.
pub fn aperture_socket(
    data: &[u8],
    bounds_min: V,
    bounds_max: V,
    sockets: &[Value],
) -> Result<Option<Value>, String> {
    let (doc, bin) = document(data)?;
    let socket = sockets.iter().find(|s| s["purpose"] == "doorway");
    let mut normal = socket
        .map(|s| vec3(&s["direction"], [0., 0., 1.]))
        .unwrap_or([0., 0., 1.]);
    normal[1] = 0.;
    let len = dot(normal, normal).sqrt().max(1e-8);
    normal = normal.map(|x| x / len);
    let tangent = [normal[2], 0., -normal[0]];
    let corners = [
        [bounds_min[0], 0., bounds_min[2]],
        [bounds_min[0], 0., bounds_max[2]],
        [bounds_max[0], 0., bounds_min[2]],
        [bounds_max[0], 0., bounds_max[2]],
    ];
    let at = socket.map(|s| vec3(&s["at"], [0.; 3]));
    let plane = at.map(|a| dot(a, normal) - 0.18).unwrap_or_else(|| {
        corners
            .iter()
            .map(|x| dot(*x, normal))
            .fold(f64::NEG_INFINITY, f64::max)
    });
    let low = corners
        .iter()
        .map(|x| dot(*x, tangent))
        .fold(f64::INFINITY, f64::min)
        + 0.3;
    let high = corners
        .iter()
        .map(|x| dot(*x, tangent))
        .fold(f64::NEG_INFINITY, f64::max)
        - 0.3;
    let hint = at.map(|a| dot(a, tangent)).unwrap_or(0.);
    let mut triangles: Vec<[[f64; 2]; 3]> = Vec::new();
    let mut f = |node: &Value, world: Mat, _: bool| -> Result<(), String> {
        for prim in primitives(&doc, node)? {
            if prim["mode"].as_u64().unwrap_or(4) != 4 {
                continue;
            }
            let pos = accessor(
                &doc,
                bin,
                prim["attributes"]["POSITION"]
                    .as_u64()
                    .ok_or("missing positions")? as usize,
            )?;
            let indices = if let Some(i) = prim["indices"].as_u64() {
                accessor(&doc, bin, i as usize)?
                    .iter()
                    .map(|v| v[0] as usize)
                    .collect::<Vec<_>>()
            } else {
                (0..pos.len()).collect()
            };
            for ids in indices.chunks_exact(3) {
                let mut uv = [[0.; 2]; 3];
                let mut good = true;
                for j in 0..3 {
                    let p = pos.get(ids[j]).ok_or("index outside positions")?;
                    if p.len() != 3 {
                        return Err("position is not vec3".into());
                    }
                    let v = transform(world, [p[0], p[1], p[2]]);
                    let dist = dot(v, normal);
                    if v.iter().any(|x| !x.is_finite())
                        || dist <= plane - 0.75
                        || dist >= plane + 0.25
                    {
                        good = false;
                        break;
                    }
                    uv[j] = [dot(v, tangent), v[1]];
                }
                if good {
                    triangles.push(uv);
                }
            }
        }
        Ok(())
    };
    for root in roots(&doc)? {
        visit(&doc, root, ident(), false, 0, &mut f)?;
    }
    if triangles.is_empty() {
        return Ok(None);
    }
    let covered = |u: f64, h: f64| {
        triangles.iter().any(|tri| {
            let a = tri[0];
            let f = [tri[1][0] - a[0], tri[1][1] - a[1]];
            let s = [tri[2][0] - a[0], tri[2][1] - a[1]];
            let d = f[0] * s[1] - f[1] * s[0];
            if d.abs() <= 0.00001 {
                return false;
            }
            let delta = [u - a[0], h - a[1]];
            let x = (delta[0] * s[1] - delta[1] * s[0]) / d;
            let y = (f[0] * delta[1] - f[1] * delta[0]) / d;
            x >= -0.0001 && y >= -0.0001 && x + y <= 1.0001
        })
    };
    let count = (((high - low) / 0.05).ceil() as usize).min(1200);
    let mut intervals = Vec::new();
    let mut start = None;
    for step in 0..=count {
        let u = (low + step as f64 * 0.05).min(high);
        let open = step < count && !covered(u, 0.3) && !covered(u, 1.25);
        if open && start.is_none() {
            start = Some(u);
        } else if !open {
            if let Some(a) = start.take() {
                if (0.75..=3.6).contains(&(u - a)) && a > low + 0.15 && u < high - 0.15 {
                    intervals.push((a, u));
                }
            }
        }
    }
    let Some((left, right)) = intervals.into_iter().min_by(|a, b| {
        (((a.0 + a.1) / 2. - hint).abs()).total_cmp(&(((b.0 + b.1) / 2. - hint).abs()))
    }) else {
        return Ok(None);
    };
    let center = (left + right) / 2.;
    let mut height = 1.3;
    while height < 5.8 && !covered(center, height) {
        height += 0.05;
    }
    let width = (right - left - 0.08).max(0.65);
    let at = [
        tangent[0] * center + normal[0] * plane,
        0.02,
        tangent[2] * center + normal[2] * plane,
    ];
    Ok(Some(
        json!({"name":"native_door_aperture","purpose":"aperture","at":at,"direction":normal,"width_m":width,"height_m":(height-0.05).max(1.3),"height_capped":height>=5.8,"depth_m":(width*0.65).clamp(0.65,1.4),"depth_policy":"authored_interior_proxy","position_source":"transformed_mesh_coverage_0.30_1.25m_step0.05","power_lumens":0,"occlusion":"measured_door_opening"}),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gltf::{Gltf, FLOAT, UINT};
    #[test]
    fn facade_bounds_and_measured_opening_follow_transforms() {
        let mut g = Gltf::new("test");
        let mut positions = Vec::new();
        let mut indices = Vec::new();
        for (x0, x1, y0, y1) in [(-2., -0.5, 0., 3.), (0.5, 2., 0., 3.), (-0.5, 0.5, 2., 3.)] {
            let base = (positions.len() / 3) as u32;
            for p in [[x0, y0, 0.], [x1, y0, 0.], [x1, y1, 0.], [x0, y1, 0.]] {
                positions.extend(p);
            }
            indices.extend([base, base + 1, base + 2, base, base + 2, base + 3]);
        }
        let p: Vec<u8> = positions
            .iter()
            .flat_map(|x| (*x as f32).to_le_bytes())
            .collect();
        let i: Vec<u8> = indices.iter().flat_map(|x| x.to_le_bytes()).collect();
        let pa = g
            .accessor(
                &p,
                FLOAT,
                positions.len() / 3,
                3,
                false,
                Some((vec![-2., 0., 0.], vec![2., 3., 0.])),
                Some(34962),
            )
            .unwrap();
        let ia = g
            .accessor(&i, UINT, indices.len(), 1, false, None, Some(34963))
            .unwrap();
        let mesh = g
            .add(
                "meshes",
                json!({"primitives":[{"attributes":{"POSITION":pa},"indices":ia}]}),
            )
            .unwrap();
        let node = g
            .add(
                "nodes",
                json!({"name":"cottage_front","mesh":mesh,"translation":[1.,0.,2.]}),
            )
            .unwrap();
        g.doc["scenes"][0]["nodes"]
            .as_array_mut()
            .unwrap()
            .push(json!(node));
        let glb = g.finish().unwrap();
        let sockets = facade_sockets(&glb).unwrap();
        assert_eq!(sockets.len(), 3);
        assert_eq!(sockets[0]["at"][2], 2.18);
        let aperture = aperture_socket(&glb, [-1., 0., 2.], [3., 3., 2.], &sockets)
            .unwrap()
            .unwrap();
        assert_eq!(aperture["purpose"], "aperture");
        assert!(aperture["width_m"].as_f64().unwrap() > 0.8);
    }
}
