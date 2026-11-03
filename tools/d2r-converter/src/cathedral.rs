//! Authored D1 cathedral massing, dressed with owned D2R materials.
use crate::gltf::{Gltf, FLOAT, UINT};
use serde_json::{json, Value};
use std::collections::BTreeMap;

type V = [f64; 3];
fn add(a: V, b: V) -> V {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}
fn sub(a: V, b: V) -> V {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn mul(a: V, x: f64) -> V {
    a.map(|v| v * x)
}
fn cross(a: V, b: V) -> V {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}
fn dot(a: V, b: V) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
fn length(a: V) -> f64 {
    dot(a, a).sqrt()
}
fn normalize(a: V) -> V {
    let l = length(a);
    if l > 1e-12 {
        mul(a, 1. / l)
    } else {
        [0., 1., 0.]
    }
}
fn point(p: V) -> V {
    [(p[0] - 24.5) / 0.75, p[1] / 0.75, (p[2] - 20.5) / 0.75]
}
#[derive(Default)]
struct Geometry {
    positions: Vec<f64>,
    normals: Vec<f64>,
    tangents: Vec<f64>,
    uvs: Vec<f64>,
    colors: Vec<f64>,
    indices: Vec<u32>,
}
impl Geometry {
    fn face(&mut self, mut p: Vec<V>, normal: Option<V>, uv: Option<Vec<[f64; 2]>>, shade: f64) {
        if p.len() < 3 {
            return;
        }
        let mut t = uv.unwrap_or_else(|| {
            vec![[0.07, 0.9], [0.92, 0.9], [0.92, 0.17], [0.07, 0.17]][..p.len()].to_vec()
        });
        let mut n = cross(sub(p[1], p[0]), sub(p[2], p[0]));
        if length(n) < 1e-10 {
            return;
        }
        if normal.is_some_and(|v| dot(n, v) < 0.) {
            p.reverse();
            t.reverse();
            n = mul(n, -1.);
        }
        n = normalize(n);
        let e1 = sub(p[1], p[0]);
        let e2 = sub(p[2], p[0]);
        let du = [t[1][0] - t[0][0], t[1][1] - t[0][1]];
        let dv = [t[2][0] - t[0][0], t[2][1] - t[0][1]];
        let det = du[0] * dv[1] - dv[0] * du[1];
        let tangent = if det.abs() > 1e-10 {
            mul(sub(mul(e1, dv[1]), mul(e2, du[1])), 1. / det)
        } else {
            e1
        };
        let tangent = normalize(sub(tangent, mul(n, dot(n, tangent))));
        let bitangent = if det.abs() > 1e-10 {
            mul(sub(mul(e2, du[0]), mul(e1, dv[0])), 1. / det)
        } else {
            cross(n, tangent)
        };
        let hand = if dot(cross(n, tangent), bitangent) < 0. {
            -1.
        } else {
            1.
        };
        let start = (self.positions.len() / 3) as u32;
        for (v, uv) in p.iter().zip(t.iter()) {
            self.positions.extend(v);
            self.normals.extend(n);
            self.tangents
                .extend([tangent[0], tangent[1], tangent[2], hand]);
            self.uvs.extend(uv);
            self.colors.extend([shade, shade, shade, 1.]);
        }
        for i in 1..p.len() - 1 {
            self.indices
                .extend([start, start + i as u32, start + i as u32 + 1]);
        }
    }
    fn quad(&mut self, p: [V; 4], n: Option<V>, shade: f64) {
        self.face(p.into_iter().map(point).collect(), n, None, shade)
    }
    fn cube(&mut self, a: V, b: V, shade: f64) {
        let [x, y, z] = a;
        let [xx, yy, zz] = b;
        for (p, n) in [
            (
                [[x, y, z], [x, y, zz], [x, yy, zz], [x, yy, z]],
                [-1., 0., 0.],
            ),
            (
                [[xx, y, zz], [xx, y, z], [xx, yy, z], [xx, yy, zz]],
                [1., 0., 0.],
            ),
            (
                [[xx, y, z], [x, y, z], [x, yy, z], [xx, yy, z]],
                [0., 0., -1.],
            ),
            (
                [[x, y, zz], [xx, y, zz], [xx, yy, zz], [x, yy, zz]],
                [0., 0., 1.],
            ),
            (
                [[x, yy, z], [x, yy, zz], [xx, yy, zz], [xx, yy, z]],
                [0., 1., 0.],
            ),
            (
                [[x, y, zz], [x, y, z], [xx, y, z], [xx, y, zz]],
                [0., -1., 0.],
            ),
        ] {
            self.quad(p, Some(n), shade);
        }
    }
    fn beam(&mut self, start: V, end: V, width: f64, shade: f64) {
        let along = normalize(sub(end, start));
        let side = mul(normalize(cross(along, [0., 1., 0.])), width / 2.);
        let up = mul(cross(normalize(side), along), width / 2.);
        let mut corners = Vec::new();
        for origin in [start, end] {
            for (s, u) in [(-1., -1.), (1., -1.), (1., 1.), (-1., 1.)] {
                corners.push(point(add(add(origin, mul(side, s)), mul(up, u))));
            }
        }
        for indices in [
            [0, 1, 2, 3],
            [4, 7, 6, 5],
            [0, 4, 5, 1],
            [1, 5, 6, 2],
            [2, 6, 7, 3],
            [3, 7, 4, 0],
        ] {
            self.face(
                indices.into_iter().map(|i| corners[i]).collect(),
                None,
                None,
                shade,
            );
        }
    }
}
fn wall(
    g: &mut Geometry,
    a: [f64; 2],
    b: [f64; 2],
    low: f64,
    high: f64,
    windows: &[(f64, f64, f64, f64)],
) {
    let dx = b[0] - a[0];
    let dz = b[1] - a[1];
    let len = (dx * dx + dz * dz).sqrt();
    let d = [dx / len, dz / len];
    let normal = [d[1], 0., -d[0]];
    let mut cuts = vec![0., len];
    let mut levels = vec![low, high];
    for &(c, y, w, h) in windows {
        cuts.extend([c - w / 2., c + w / 2.]);
        levels.extend([y, y + h]);
    }
    let mut v = 0.;
    while v < len {
        cuts.push(v);
        v += 1.65;
    }
    v = low;
    while v < high {
        levels.push(v);
        v += 1.45;
    }
    cuts.retain(|v| *v >= 0. && *v <= len);
    levels.retain(|v| *v >= low && *v <= high);
    cuts.sort_by(f64::total_cmp);
    levels.sort_by(f64::total_cmp);
    cuts.dedup_by(|a, b| (*a - *b).abs() < 1e-8);
    levels.dedup_by(|a, b| (*a - *b).abs() < 1e-8);
    for (i, c) in cuts.windows(2).enumerate() {
        for (row, h) in levels.windows(2).enumerate() {
            let u = (c[0] + c[1]) / 2.;
            let y = (h[0] + h[1]) / 2.;
            if windows.iter().any(|&(center, bottom, width, height)| {
                (u - center).abs() < width / 2. && y > bottom && y < bottom + height
            }) {
                continue;
            }
            let p0 = [a[0] + d[0] * c[0], a[1] + d[1] * c[0]];
            let p1 = [a[0] + d[0] * c[1], a[1] + d[1] * c[1]];
            g.quad(
                [
                    [p0[0], h[0], p0[1]],
                    [p1[0], h[0], p1[1]],
                    [p1[0], h[1], p1[1]],
                    [p0[0], h[1], p0[1]],
                ],
                Some(normal),
                0.88 + ((i * 13 + row * 7) % 8) as f64 * 0.012,
            );
        }
    }
}
fn window(
    groups: &mut BTreeMap<&'static str, Geometry>,
    sockets: &mut Vec<Value>,
    name: String,
    origin: V,
    n: V,
    width: f64,
    height: f64,
) {
    let t = [n[2], 0., -n[0]];
    let p =
        |u: f64, h: f64, d: f64| point(add(add(add(origin, mul(t, u)), mul(n, d)), [0., h, 0.]));
    let w = width / 2.;
    let h = height / 2.;
    groups.get_mut("glass").unwrap().face(
        vec![
            p(-w, -h, -0.13),
            p(w, -h, -0.13),
            p(w, h, -0.13),
            p(-w, h, -0.13),
        ],
        Some(n),
        None,
        1.,
    );
    for (l, r, b, t) in [
        (-w - 0.1, -w, -h - 0.08, h + 0.08),
        (w, w + 0.1, -h - 0.08, h + 0.08),
        (-w, w, -h - 0.1, -h),
        (-w, w, h, h + 0.1),
    ] {
        groups.get_mut("trim").unwrap().face(
            vec![p(l, b, 0.02), p(r, b, 0.02), p(r, t, 0.02), p(l, t, 0.02)],
            Some(n),
            None,
            1.,
        );
    }
    for u in [-w, w] {
        groups.get_mut("trim").unwrap().face(
            vec![
                p(u, -h, 0.02),
                p(u, h, 0.02),
                p(u, h, -0.15),
                p(u, -h, -0.15),
            ],
            None,
            None,
            1.,
        );
    }
    for y in [-h, h] {
        groups.get_mut("trim").unwrap().face(
            vec![
                p(-w, y, 0.02),
                p(-w, y, -0.15),
                p(w, y, -0.15),
                p(w, y, 0.02),
            ],
            None,
            None,
            1.,
        );
    }
    groups.get_mut("wood").unwrap().face(
        vec![
            p(-0.035, -h, 0.03),
            p(0.035, -h, 0.03),
            p(0.035, h, 0.03),
            p(-0.035, h, 0.03),
        ],
        Some(n),
        None,
        1.,
    );
    sockets.push(json!({"name":name,"purpose":"cathedral_window","at":point(add(origin,mul(n,0.1))),"direction":n}));
}
fn slope(g: &mut Geometry, start: V, across: V, along: V, coarse: bool, damage: u8) {
    let rows = (length(across) / if coarse { 0.54 } else { 0.29 }).ceil() as usize;
    let cols = (length(along) / if coarse { 0.72 } else { 0.39 }).ceil() as usize;
    let mut n = cross(across, along);
    if n[1] < 0. {
        n = mul(n, -1.);
    }
    n = normalize(n);
    let lip = mul(n, 0.017);
    for row in 0..rows {
        let u0 = row as f64 / rows as f64;
        let u1 = ((row as f64 + 1.07) / rows as f64).min(1.);
        let shift = (row % 2) as f64 * 0.5;
        for col in -1..cols as i32 {
            let v0 = ((col as f64 + shift) / cols as f64).max(0.);
            let v1 = ((col as f64 + 1. + shift) / cols as f64).min(1.);
            if v1 - v0 < 0.001 {
                continue;
            }
            let at = |u: f64, v: f64| add(add(start, mul(across, u)), mul(along, v));
            let c = at((u0 + u1) / 2., (v0 + v1) / 2.);
            if (damage == 1
                && ((19.9 < c[2] && c[2] < 22.15 && c[0] > 25.8)
                    || (15.1 < c[2] && c[2] < 16.5 && 25.1 < c[0] && c[0] < 27.1)))
                || (damage == 2 && 20. < c[2] && c[2] < 22.15)
            {
                continue;
            }
            let p = [at(u0, v0), at(u1, v0), at(u1, v1), at(u0, v1)];
            let seed = row as i32 * 173 + col * 53;
            let ox = 0.04 + seed.rem_euclid(7) as f64 * 0.115;
            let oy = 0.04 + (seed.div_euclid(7)).rem_euclid(4) as f64 * 0.085;
            let uv = vec![
                [ox, oy],
                [ox, oy + 0.075],
                [ox + 0.11, oy + 0.075],
                [ox + 0.11, oy],
            ];
            let shade = 0.83 + seed.rem_euclid(11) as f64 * 0.014;
            g.face(
                p.iter().map(|v| point(add(*v, lip))).collect(),
                Some(n),
                Some(uv.clone()),
                shade,
            );
            g.face(
                vec![
                    point(add(p[1], lip)),
                    point(p[1]),
                    point(p[2]),
                    point(add(p[2], lip)),
                ],
                None,
                Some(uv),
                shade * 0.8,
            );
        }
    }
}
fn cone(g: &mut Geometry, c: [f64; 2], radius: f64, low: f64, high: f64, coarse: bool) {
    let rows = if coarse { 8 } else { 14 };
    let seg = if coarse { 18 } else { 32 };
    for row in 0..rows {
        let r0 = radius * (1. - row as f64 / rows as f64);
        let r1 = radius * (1. - (row + 1) as f64 / rows as f64);
        let y0 = low + (high - low) * row as f64 / rows as f64;
        let y1 = low + (high - low) * (row + 1) as f64 / rows as f64;
        for i in 0..seg {
            let a = 2. * std::f64::consts::PI * i as f64 / seg as f64;
            let b = 2. * std::f64::consts::PI * (i + 1) as f64 / seg as f64;
            let mut p = vec![
                point([c[0] + r0 * a.cos(), y0, c[1] + r0 * a.sin()]),
                point([c[0] + r0 * b.cos(), y0, c[1] + r0 * b.sin()]),
                point([c[0] + r1 * b.cos(), y1, c[1] + r1 * b.sin()]),
            ];
            if r1 > 0.0001 {
                p.push(point([c[0] + r1 * a.cos(), y1, c[1] + r1 * a.sin()]));
            }
            let uv =
                vec![[0.06, 0.39], [0.21, 0.39], [0.21, 0.10], [0.06, 0.10]][..p.len()].to_vec();
            g.face(
                p,
                Some([((a + b) / 2.).cos(), 0.5, ((a + b) / 2.).sin()]),
                Some(uv),
                0.85 + (i % 7) as f64 * 0.022,
            );
        }
    }
}
fn pyramid(g: &mut Geometry, corners: [V; 4], peak: V, coarse: bool) {
    let rows = if coarse { 7 } else { 13 };
    for edge in 0..4 {
        let a = corners[edge];
        let b = corners[(edge + 1) % 4];
        let mut n = cross(sub(b, a), sub(peak, a));
        if n[1] < 0. {
            n = mul(n, -1.);
        }
        n = normalize(n);
        for row in 0..rows {
            let u0 = row as f64 / rows as f64;
            let u1 = (row + 1) as f64 / rows as f64;
            let l0 = add(mul(a, 1. - u0), mul(peak, u0));
            let r0 = add(mul(b, 1. - u0), mul(peak, u0));
            let l1 = add(mul(a, 1. - u1), mul(peak, u1));
            let r1 = add(mul(b, 1. - u1), mul(peak, u1));
            let cols = (length(sub(r0, l0)) / if coarse { 0.63 } else { 0.34 })
                .ceil()
                .max(1.) as usize;
            for col in 0..cols {
                let v0 = col as f64 / cols as f64;
                let v1 = (col + 1) as f64 / cols as f64;
                let mut p = vec![
                    add(mul(l0, 1. - v0), mul(r0, v0)),
                    add(mul(l0, 1. - v1), mul(r0, v1)),
                    add(mul(l1, 1. - v1), mul(r1, v1)),
                ];
                if row + 1 < rows {
                    p.push(add(mul(l1, 1. - v0), mul(r1, v0)));
                }
                let uv = vec![[0.06, 0.39], [0.21, 0.39], [0.21, 0.10], [0.06, 0.10]][..p.len()]
                    .to_vec();
                g.face(
                    p.into_iter()
                        .map(|p| point(add(p, mul(n, 0.012))))
                        .collect(),
                    Some(n),
                    Some(uv),
                    0.84 + (col % 7) as f64 * 0.022,
                );
            }
        }
    }
}
fn round_volume(
    groups: &mut BTreeMap<&'static str, Geometry>,
    sockets: &mut Vec<Value>,
    name: &str,
    c: [f64; 2],
    radius: f64,
    height: f64,
    coarse: bool,
) {
    let seg = if coarse { 24 } else { 40 };
    let levels = [0., 0.35, 1.25, 2.55, height - 0.22, height];
    for i in 0..seg {
        let a = 2. * std::f64::consts::PI * i as f64 / seg as f64;
        let b = 2. * std::f64::consts::PI * (i + 1) as f64 / seg as f64;
        for pair in levels.windows(2) {
            if ((a + b) / 2. - std::f64::consts::FRAC_PI_2).abs() < 0.23
                && pair[0] >= 1.25
                && pair[1] <= 2.55
            {
                continue;
            }
            groups.get_mut("stone").unwrap().quad(
                [
                    [c[0] + radius * a.cos(), pair[0], c[1] + radius * a.sin()],
                    [c[0] + radius * b.cos(), pair[0], c[1] + radius * b.sin()],
                    [c[0] + radius * b.cos(), pair[1], c[1] + radius * b.sin()],
                    [c[0] + radius * a.cos(), pair[1], c[1] + radius * a.sin()],
                ],
                Some([((a + b) / 2.).cos(), 0., ((a + b) / 2.).sin()]),
                1.,
            );
        }
    }
    window(
        groups,
        sockets,
        name.into(),
        [c[0], 1.9, c[1] + radius],
        [0., 0., 1.],
        radius * 0.39,
        1.3,
    );
    for (y, extend) in [(0.2, 0.07), (height - 0.22, 0.09), (height, 0.14)] {
        for i in 0..seg {
            let a = 2. * std::f64::consts::PI * i as f64 / seg as f64;
            let b = 2. * std::f64::consts::PI * (i + 1) as f64 / seg as f64;
            let r = radius + extend;
            groups.get_mut("trim").unwrap().quad(
                [
                    [c[0] + r * a.cos(), y, c[1] + r * a.sin()],
                    [c[0] + r * b.cos(), y, c[1] + r * b.sin()],
                    [c[0] + r * b.cos(), y + 0.1, c[1] + r * b.sin()],
                    [c[0] + r * a.cos(), y + 0.1, c[1] + r * a.sin()],
                ],
                None,
                1.,
            );
        }
    }
    cone(
        groups.get_mut("slate").unwrap(),
        c,
        radius + 0.23,
        height + 0.1,
        height + radius * 0.74,
        coarse,
    );
}
fn bytes(v: &[f64]) -> Vec<u8> {
    v.iter().flat_map(|x| (*x as f32).to_le_bytes()).collect()
}
fn accessor(g: &mut Gltf, v: &[f64], dim: usize, bounds: bool) -> Result<usize, String> {
    let count = v.len() / dim;
    let mut lo = vec![f64::INFINITY; dim];
    let mut hi = vec![f64::NEG_INFINITY; dim];
    for row in v.chunks_exact(dim) {
        for (i, x) in row.iter().enumerate() {
            lo[i] = lo[i].min(*x);
            hi[i] = hi[i].max(*x);
        }
    }
    g.accessor(
        &bytes(v),
        FLOAT,
        count,
        dim,
        false,
        if bounds { Some((lo, hi)) } else { None },
        Some(34962),
    )
}
fn material<F: FnMut(&str, &str) -> Result<Option<Vec<u8>>, String>>(
    g: &mut Gltf,
    name: &str,
    source: &str,
    tint: [f64; 3],
    blue: bool,
    texture: &mut F,
) -> Result<usize, String> {
    let mut m = json!({"name":name,"pbrMetallicRoughness":{"metallicFactor":0.,"roughnessFactor":0.96,"baseColorFactor":[tint[0],tint[1],tint[2],1.]}});
    for (usage, suffix) in [
        ("albedo", if blue { "_blue_alb" } else { "_alb" }),
        ("normal", "_nrm"),
        ("orm", "_orm"),
    ] {
        let path = format!("{source}{suffix}.texture");
        let png = texture(&path, usage)?.ok_or_else(|| format!("cathedral requires {path}"))?;
        let id = g.image_png(&png, &path)?;
        match usage {
            "albedo" => m["pbrMetallicRoughness"]["baseColorTexture"] = json!({"index":id}),
            "normal" => m["normalTexture"] = json!({"index":id,"scale":0.85}),
            _ => {
                m["pbrMetallicRoughness"]["metallicRoughnessTexture"] = json!({"index":id});
                m["occlusionTexture"] = json!({"index":id,"strength":0.75});
            }
        }
    }
    g.add("materials", m)
}
pub struct CathedralOutput {
    pub glb: Vec<u8>,
    pub bounds_min: [f64; 3],
    pub bounds_max: [f64; 3],
    pub triangles: usize,
    pub sockets: Vec<Value>,
}
/// Build the authored cathedral. The callback supplies encoded PNGs from owned textures.
pub fn build<F: FnMut(&str, &str) -> Result<Option<Vec<u8>>, String>>(
    lod: u32,
    mut texture: F,
) -> Result<CathedralOutput, String> {
    let coarse = lod >= 2;
    let mut groups: BTreeMap<_, _> = ["stone", "trim", "slate", "wood", "glass", "dark"]
        .into_iter()
        .map(|k| (k, Geometry::default()))
        .collect();
    let mut sockets = Vec::new();
    wall(
        groups.get_mut("stone").unwrap(),
        [20.9, 12.6],
        [28.15, 12.6],
        0.,
        7.65,
        &[],
    );
    let clear: Vec<_> = [15.1, 18.25, 21.4, 24.55]
        .into_iter()
        .map(|z| (z - 12.6, 6.7, 0.85, 0.42))
        .collect();
    wall(
        groups.get_mut("stone").unwrap(),
        [28.15, 12.6],
        [28.15, 25.8],
        0.,
        7.65,
        &clear,
    );
    wall(
        groups.get_mut("stone").unwrap(),
        [28.15, 25.8],
        [20.9, 25.8],
        0.,
        7.65,
        &[],
    );
    wall(
        groups.get_mut("stone").unwrap(),
        [20.9, 25.8],
        [20.9, 12.6],
        0.,
        7.65,
        &[],
    );
    for (z, n) in [(12.6, [0., 0., -1.]), (25.8, [0., 0., 1.])] {
        groups.get_mut("stone").unwrap().face(
            vec![
                point([20.9, 7.65, z]),
                point([28.15, 7.65, z]),
                point([24.525, 11.55, z]),
            ],
            Some(n),
            None,
            1.,
        );
    }
    slope(
        groups.get_mut("slate").unwrap(),
        [24.525, 11.55, 12.35],
        [-3.95, -3.9, 0.],
        [0., 0., 13.72],
        coarse,
        0,
    );
    slope(
        groups.get_mut("slate").unwrap(),
        [24.525, 11.55, 12.35],
        [3.95, -3.9, 0.],
        [0., 0., 13.72],
        coarse,
        1,
    );
    let aisle: Vec<_> = [15.2, 18.35, 21.5, 24.65]
        .into_iter()
        .map(|z| (z - 13., 2.7, 0.62, 1.3))
        .collect();
    for (a, b, holes) in [
        ([30.35, 13.], [30.35, 26.], aisle.as_slice()),
        ([28., 26.], [30.35, 26.], &[][..]),
        ([30.35, 13.], [28., 13.], &[][..]),
        ([18.8, 26.], [18.8, 13.], &[][..]),
        ([18.8, 13.], [21., 13.], &[][..]),
        ([21., 26.], [18.8, 26.], &[][..]),
    ] {
        wall(groups.get_mut("stone").unwrap(), a, b, 0., 4.55, holes);
    }
    slope(
        groups.get_mut("slate").unwrap(),
        [27.98, 7., 12.9],
        [2.62, -2.45, 0.],
        [0., 0., 13.4],
        coarse,
        2,
    );
    slope(
        groups.get_mut("slate").unwrap(),
        [20.98, 7., 12.9],
        [-2.43, -2.45, 0.],
        [0., 0., 13.4],
        coarse,
        0,
    );
    for z in [13.3, 16., 19.15, 22.3, 25.6] {
        for (lo, hi, out) in [(0., 1.25, 0.78), (1.25, 2.55, 0.61), (2.55, 4.55, 0.43)] {
            groups.get_mut("trim").unwrap().cube(
                [30.19, lo, z - 0.31],
                [30.35 + out, hi, z + 0.31],
                0.93,
            );
        }
        groups.get_mut("trim").unwrap().quad(
            [
                [30.18, 4.55, z - 0.36],
                [30.83, 3.95, z - 0.36],
                [30.83, 3.95, z + 0.36],
                [30.18, 4.55, z + 0.36],
            ],
            Some([0., 1., 0.]),
            1.,
        );
    }
    for (i, z) in [15.2, 18.35, 21.5, 24.65].into_iter().enumerate() {
        window(
            &mut groups,
            &mut sockets,
            format!("d1_aisle_window_{i}"),
            [30.35, 3.35, z],
            [1., 0., 0.],
            0.62,
            1.3,
        );
        window(
            &mut groups,
            &mut sockets,
            format!("d1_clerestory_window_{i}"),
            [28.15, 6.91, z - 0.1],
            [1., 0., 0.],
            0.85,
            0.42,
        );
    }
    let (x0, z0, x1, z1) = (19.1f64, 22.3, 22.65, 25.85);
    for (a, b, n) in [
        ([x0, z0], [x1, z0], [0., 0., -1.]),
        ([x1, z0], [x1, z1], [1., 0., 0.]),
        ([x1, z1], [x0, z1], [0., 0., 1.]),
        ([x0, z1], [x0, z0], [-1., 0., 0.]),
    ] {
        let len = ((b[0] - a[0]).powi(2) + (b[1] - a[1]).powi(2)).sqrt();
        let holes = [
            (len * 0.3, 11.35, 0.55, 1.5),
            (len * 0.7, 11.35, 0.55, 1.5),
            (len * 0.3, 14.55, 0.55, 1.5),
            (len * 0.7, 14.55, 0.55, 1.5),
        ];
        wall(groups.get_mut("stone").unwrap(), a, b, 0., 16.45, &holes);
        if n[0] > 0. || n[2] > 0. {
            for (row, y) in [12.1, 15.3].into_iter().enumerate() {
                for (column, f) in [0.3, 0.7].into_iter().enumerate() {
                    window(
                        &mut groups,
                        &mut sockets,
                        format!("d1_belfry_{}_{row}_{column}", n[0] as i32),
                        [a[0] + (b[0] - a[0]) * f, y, a[1] + (b[1] - a[1]) * f],
                        n,
                        0.55,
                        1.5,
                    );
                }
            }
        }
    }
    for y in [0.18, 5.4, 10.8, 13.65, 16.35] {
        groups.get_mut("trim").unwrap().cube(
            [x0 - 0.08, y, z0 - 0.08],
            [x1 + 0.08, y + 0.14, z1 + 0.08],
            0.93,
        );
    }
    pyramid(
        groups.get_mut("slate").unwrap(),
        [
            [x0 - 0.22, 16.55, z0 - 0.22],
            [x1 + 0.22, 16.55, z0 - 0.22],
            [x1 + 0.22, 16.55, z1 + 0.22],
            [x0 - 0.22, 16.55, z1 + 0.22],
        ],
        [(x0 + x1) / 2., 18.75, (z0 + z1) / 2.],
        coarse,
    );
    round_volume(
        &mut groups,
        &mut sockets,
        "d1_round_entry_left",
        [22.45, 26.15],
        1.95,
        4.3,
        coarse,
    );
    round_volume(
        &mut groups,
        &mut sockets,
        "d1_round_entry_right",
        [28.05, 26.95],
        1.28,
        3.05,
        coarse,
    );
    for z in [20.02, 20.66, 21.3, 21.94] {
        groups
            .get_mut("wood")
            .unwrap()
            .beam([24.53, 11.34, z], [28.32, 7.45, z], 0.12, 0.73);
        groups
            .get_mut("wood")
            .unwrap()
            .beam([27.99, 6.82, z], [30.35, 4.42, z], 0.12, 0.73);
    }
    for x in [25.4, 26.25, 27.1] {
        let roof_y = 11.55 - (x - 24.525) * 3.9 / 3.95;
        groups.get_mut("wood").unwrap().cube(
            [x, roof_y - 0.17, 15.],
            [x + 0.075, roof_y - 0.08, 16.6],
            0.7,
        );
    }
    groups
        .get_mut("dark")
        .unwrap()
        .cube([21.1, 0.01, 13.], [27.95, 0.07, 25.4], 0.65);
    for (a, b, shade) in [
        ([24.45, 11.49, 12.3], [24.6, 11.67, 26.05], 0.86),
        ([28.1, 7.56, 12.5], [28.25, 7.76, 25.9], 0.89),
        ([20.8, 7.56, 12.5], [20.96, 7.76, 25.9], 0.89),
    ] {
        groups.get_mut("trim").unwrap().cube(a, b, shade);
    }
    let mut g = Gltf::new("damnationx Rust D2R cathedral");
    let root = g.add("nodes", json!({"name":"root","children":[]}))?;
    g.doc["scenes"][0]["nodes"]
        .as_array_mut()
        .unwrap()
        .push(json!(root));
    let stone=material(&mut g,"D1 cathedral D2R aged stone","data/hd/env/texture/act1/monastry/act1_monastry_architecture/act1_monastry_architecture_basic_wall01_c",[0.77,0.81,0.88],false,&mut texture)?;
    let slate = material(
        &mut g,
        "D1 cathedral D2R blue slate",
        "data/hd/env/texture/act2/town/act2_town_shops_roof01",
        [0.75, 0.83, 1.],
        true,
        &mut texture,
    )?;
    let wood=material(&mut g,"D1 cathedral exposed D2R rafters","data/hd/env/texture/expansion/town/town_structures/structure04/expansion_town_structure04_b",[0.45,0.4,0.34],false,&mut texture)?;
    let dark=g.add("materials",json!({"name":"D1 cathedral unlit interior matter","pbrMetallicRoughness":{"baseColorFactor":[0.018,0.014,0.012,1.],"metallicFactor":0.,"roughnessFactor":1.}}))?;
    let glass=g.add("materials",json!({"name":"D1 cathedral recessed crimson glazing","pbrMetallicRoughness":{"baseColorFactor":[0.24,0.008,0.003,1.],"metallicFactor":0.,"roughnessFactor":0.48},"emissiveFactor":[1.,0.022,0.005],"extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":0.7}}}))?;
    g.doc.insert(
        "extensionsUsed".into(),
        json!(["KHR_materials_emissive_strength"]),
    );
    let mut lo = [f64::INFINITY; 3];
    let mut hi = [f64::NEG_INFINITY; 3];
    let mut triangles = 0;
    for (key, mesh) in groups {
        if mesh.indices.is_empty() {
            continue;
        }
        for p in mesh.positions.chunks_exact(3) {
            for i in 0..3 {
                lo[i] = lo[i].min(p[i]);
                hi[i] = hi[i].max(p[i]);
            }
        }
        let material = match key {
            "stone" | "trim" => stone,
            "slate" => slate,
            "wood" => wood,
            "glass" => glass,
            _ => dark,
        };
        let attrs = json!({"POSITION":accessor(&mut g,&mesh.positions,3,true)?,"NORMAL":accessor(&mut g,&mesh.normals,3,false)?,"TANGENT":accessor(&mut g,&mesh.tangents,4,false)?,"TEXCOORD_0":accessor(&mut g,&mesh.uvs,2,false)?,"COLOR_0":accessor(&mut g,&mesh.colors,4,false)?});
        let indices: Vec<u8> = mesh.indices.iter().flat_map(|i| i.to_le_bytes()).collect();
        let idx = g.accessor(
            &indices,
            UINT,
            mesh.indices.len(),
            1,
            false,
            None,
            Some(34963),
        )?;
        let mi=g.add("meshes",json!({"name":format!("D1 cathedral {key}"),"primitives":[{"attributes":attrs,"indices":idx,"material":material}]}))?;
        let node = g.add(
            "nodes",
            json!({"name":format!("D1 cathedral {key}"),"mesh":mi}),
        )?;
        g.doc["nodes"][root]["children"]
            .as_array_mut()
            .unwrap()
            .push(json!(node));
        triangles += mesh.indices.len() / 3;
    }
    Ok(CathedralOutput {
        glb: g.finish()?,
        bounds_min: lo,
        bounds_max: hi,
        triangles,
        sockets,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cathedral_has_bounded_geometry_and_light_sockets() {
        let fine = build(1, |_, _| Ok(Some(vec![137, 80, 78, 71]))).unwrap();
        let coarse = build(2, |_, _| Ok(Some(vec![137, 80, 78, 71]))).unwrap();
        assert_eq!(fine.sockets.len(), 18);
        assert!(fine.triangles > coarse.triangles);
        assert!(fine
            .bounds_min
            .iter()
            .zip(fine.bounds_max.iter())
            .all(|(a, b)| a < b));
        let n = u32::from_le_bytes(fine.glb[12..16].try_into().unwrap()) as usize;
        let doc: Value = serde_json::from_slice(&fine.glb[20..20 + n]).unwrap();
        assert!(doc["meshes"].as_array().unwrap().len() >= 5);
        assert_eq!(doc["images"].as_array().unwrap().len(), 9);
    }
}
