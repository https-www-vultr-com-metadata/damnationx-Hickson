//! Decoding of Granny animation curve variants used by D2R.
use serde_json::Value;

#[derive(Debug, Clone, PartialEq)]
pub struct Curve {
    pub times: Vec<f64>,
    pub values: Vec<Vec<f64>>,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Position,
    Orientation,
    Scale,
}

fn number(v: &Value) -> Result<f64, String> {
    v.as_f64().ok_or_else(|| "expected curve number".into())
}
fn field(v: &Value, k: &str) -> Result<f64, String> {
    number(v.get(k).ok_or_else(|| format!("missing curve field {k}"))?)
}
fn numbers(v: &Value) -> Result<Vec<f64>, String> {
    v.as_array()
        .ok_or("expected curve array")?
        .iter()
        .map(|x| {
            if let Some(o) = x.as_object() {
                number(o.values().next().ok_or("empty curve element")?)
            } else {
                number(x)
            }
        })
        .collect()
}
fn array(v: &Value, k: &str) -> Result<Vec<f64>, String> {
    numbers(v.get(k).ok_or_else(|| format!("missing curve field {k}"))?)
}
fn truncated(v: f64) -> f64 {
    f32::from_bits((v as u32 & 0xffff) << 16) as f64
}
fn knot_scale(cd: &Value) -> Result<f64, String> {
    if let Some(v) = cd.get("OneOverKnotScaleTrunc") {
        Ok(truncated(number(v)?))
    } else {
        Ok(1.0)
    }
}
fn split(kc: Vec<f64>, dim: usize, scale: f64) -> Result<(Vec<f64>, Vec<Vec<f64>>), String> {
    if dim == 0 || kc.len() % (dim + 1) != 0 || scale == 0.0 || !scale.is_finite() {
        return Err("invalid compressed curve dimensions or knot scale".into());
    }
    let n = kc.len() / (dim + 1);
    let times = kc[..n].iter().map(|x| x / scale).collect();
    Ok((
        times,
        kc[n..].chunks_exact(dim).map(|v| v.to_vec()).collect(),
    ))
}
fn diagonal(values: Vec<Vec<f64>>, kind: Kind) -> Vec<Vec<f64>> {
    if kind == Kind::Scale {
        values
            .into_iter()
            .map(|v| {
                if v.len() == 9 {
                    vec![v[0], v[4], v[8]]
                } else {
                    v
                }
            })
            .collect()
    } else {
        values
    }
}
// Granny curve dequantization table: f32-rounded literals, kept bit-exact on purpose.
#[allow(clippy::approx_constant)]
const QS: [f64; 16] = [
    1.4142135,
    0.70710677,
    0.35355338,
    0.35355338,
    0.35355338,
    0.17677669,
    0.17677669,
    0.17677669,
    -1.4142135,
    -0.70710677,
    -0.35355338,
    -0.35355338,
    -0.35355338,
    -0.17677669,
    -0.17677669,
    -0.17677669,
];
const QO: [f64; 16] = [
    -0.70710677,
    -0.35355338,
    -0.53033006,
    -0.17677669,
    0.17677669,
    -0.17677669,
    -0.088388346,
    0.0,
    0.70710677,
    0.35355338,
    0.53033006,
    0.17677669,
    -0.17677669,
    0.17677669,
    0.088388346,
    0.0,
];
fn quat(cd: &Value, bits: u32) -> Result<Curve, String> {
    let kc = array(cd, "KnotsControls")?;
    let scale = field(cd, "OneOverKnotScale")?;
    let (times, values) = split(kc, 3, scale)?;
    let selector = field(cd, "ScaleOffsetTableEntries")? as u32;
    let hi = 1u32 << bits;
    let lo = hi - 1;
    let shift = bits - 1;
    let mul = if bits == 15 {
        0.000030518509
    } else {
        0.0078740157
    };
    let indices: Vec<usize> = (0..4)
        .map(|i| ((selector >> (4 * i)) & 15) as usize)
        .collect();
    let mut out = Vec::with_capacity(values.len());
    for v in values {
        let a = v[0] as u32;
        let b = v[1] as u32;
        let c = v[2] as u32;
        let s1 = (((b & hi) >> shift) | ((c & hi) >> (shift + 1))) as usize;
        let s2 = (s1 + 1) & 3;
        let s3 = (s2 + 1) & 3;
        let s4 = (s3 + 1) & 3;
        let da = (a & lo) as f64 * QS[indices[s2]] * mul + QO[indices[s2]];
        let db = (b & lo) as f64 * QS[indices[s3]] * mul + QO[indices[s3]];
        let dc = (c & lo) as f64 * QS[indices[s4]] * mul + QO[indices[s4]];
        let dd = (1.0 - da * da - db * db - dc * dc).max(0.0).sqrt()
            * if a & hi != 0 { -1.0 } else { 1.0 };
        let mut q = vec![0.0; 4];
        q[s1] = dd;
        q[s2] = da;
        q[s3] = db;
        q[s4] = dc;
        out.push(q);
    }
    Ok(Curve { times, values: out })
}
/// Decodes a variant object. Scalar wrapper structs emitted by the GRFL reader are accepted.
pub fn decode(cd: &Value, kind: Kind) -> Result<Option<Curve>, String> {
    if cd.is_null() {
        return Ok(None);
    }
    let obj = cd.as_object().ok_or("expected curve object")?;
    let header = obj
        .keys()
        .find(|k| k.starts_with("CurveDataHeader_"))
        .ok_or("missing curve format header")?;
    let fmt = header.trim_start_matches("CurveDataHeader_");
    let result = match fmt {
        "DaIdentity" => Curve {
            times: vec![0.0],
            values: vec![match kind {
                Kind::Position => vec![0., 0., 0.],
                Kind::Orientation => vec![0., 0., 0., 1.],
                Kind::Scale => vec![1., 1., 1.],
            }],
        },
        "D3Constant32f" | "D4Constant32f" | "DaConstant32f" => Curve {
            times: vec![0.0],
            values: diagonal(vec![array(cd, "Controls")?], kind),
        },
        "DaK32fC32f" => {
            let times = array(cd, "Knots")?;
            let ctrl = array(cd, "Controls")?;
            if times.is_empty() || ctrl.len() % times.len() != 0 {
                return Err("invalid uncompressed curve".into());
            }
            let dim = ctrl.len() / times.len();
            if dim == 0 {
                return Err("empty curve controls".into());
            }
            Curve {
                times,
                values: diagonal(ctrl.chunks_exact(dim).map(|x| x.to_vec()).collect(), kind),
            }
        }
        "D4nK16uC15u" => quat(cd, 15)?,
        "D4nK8uC7u" => quat(cd, 7)?,
        "D3K16uC16u" | "D3K8uC8u" | "D9I3K16uC16u" | "D9I3K8uC8u" => {
            let (times, mut values) = split(array(cd, "KnotsControls")?, 3, knot_scale(cd)?)?;
            let scales = array(cd, "ControlScales")?;
            let offsets = array(cd, "ControlOffsets")?;
            if scales.len() != 3 || offsets.len() != 3 {
                return Err("invalid curve control transform".into());
            }
            for v in &mut values {
                for i in 0..3 {
                    v[i] = v[i] * scales[i] + offsets[i];
                }
            }
            Curve { times, values }
        }
        "D3I1K16uC16u" | "D3I1K8uC8u" | "D3I1K32fC32f" => {
            let (times, values) = split(array(cd, "KnotsControls")?, 1, knot_scale(cd)?)?;
            let scales = array(cd, "ControlScales")?;
            let offsets = array(cd, "ControlOffsets")?;
            if scales.len() != 3 || offsets.len() != 3 {
                return Err("invalid curve control transform".into());
            }
            let values = values
                .iter()
                .map(|v| (0..3).map(|i| v[0] * scales[i] + offsets[i]).collect())
                .collect();
            Curve { times, values }
        }
        "D9I1K16uC16u" | "D9I1K8uC8u" => {
            let (times, values) = split(array(cd, "KnotsControls")?, 1, knot_scale(cd)?)?;
            let scale = field(cd, "ControlScale")?;
            let offset = field(cd, "ControlOffset")?;
            Curve {
                times,
                values: values
                    .iter()
                    .map(|v| vec![v[0] * scale + offset; 3])
                    .collect(),
            }
        }
        "DaK16uC16u" | "DaK8uC8u" => {
            let so = array(cd, "ControlScaleOffsets")?;
            if so.is_empty() || so.len() % 2 != 0 {
                return Err("invalid generic curve transform".into());
            }
            let dim = so.len() / 2;
            let (times, mut values) = split(array(cd, "KnotsControls")?, dim, knot_scale(cd)?)?;
            for v in &mut values {
                for i in 0..dim {
                    v[i] = v[i] * so[i] + so[dim + i];
                }
            }
            Curve {
                times,
                values: diagonal(values, kind),
            }
        }
        _ => return Err(format!("unsupported curve format {fmt}")),
    };
    Ok(Some(result))
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn identity_and_constant() {
        assert_eq!(
            decode(&json!({"CurveDataHeader_DaIdentity":{}}), Kind::Orientation)
                .unwrap()
                .unwrap()
                .values,
            vec![vec![0., 0., 0., 1.]]
        );
        let cd = json!({"CurveDataHeader_DaConstant32f":{},"Controls":[1,0,0,0,2,0,0,0,3]});
        assert_eq!(
            decode(&cd, Kind::Scale).unwrap().unwrap().values,
            vec![vec![1., 2., 3.]]
        );
    }
    #[test]
    fn compressed_scalar_scale() {
        let cd = json!({"CurveDataHeader_D9I1K8uC8u":{},"KnotsControls":[0,2,3,4],"OneOverKnotScaleTrunc":16256,"ControlScale":2,"ControlOffset":1});
        let c = decode(&cd, Kind::Scale).unwrap().unwrap();
        assert_eq!(c.times, vec![0., 2.]);
        assert_eq!(c.values, vec![vec![7.; 3], vec![9.; 3]]);
    }
    #[test]
    fn invalid_curves_fail() {
        assert!(decode(
            &json!({"CurveDataHeader_D3K8uC8u":{},"KnotsControls":[1,2,3]}),
            Kind::Position
        )
        .is_err());
    }
}
