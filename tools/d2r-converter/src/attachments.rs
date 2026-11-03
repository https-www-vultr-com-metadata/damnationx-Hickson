//! Resolve native unit and recipe equipment attachments into rigid mesh models.
use crate::{granny::Granny, import_model::AssetAccess, model, unit};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

pub struct OwnedAttachment {
    pub name: String,
    pub bone: String,
    pub root: Value,
    pub translation: [f64; 3],
    pub rotation: [f64; 4],
    pub scale: [f64; 3],
}
impl OwnedAttachment {
    pub fn as_model(&self) -> model::Attachment<'_> {
        model::Attachment {
            name: &self.name,
            bone: &self.bone,
            root: &self.root,
            translation: self.translation,
            rotation: self.rotation,
            scale: self.scale,
        }
    }
}
fn mul(a: [f64; 4], b: [f64; 4]) -> [f64; 4] {
    let [ax, ay, az, aw] = a;
    let [bx, by, bz, bw] = b;
    [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]
}
fn quat(degrees: [f64; 3]) -> [f64; 4] {
    let [x, y, z] = degrees.map(|v| v.to_radians() / 2.);
    mul(
        [0., 0., z.sin(), z.cos()],
        mul([0., y.sin(), 0., y.cos()], [x.sin(), 0., 0., x.cos()]),
    )
}
/// Resolve parts' native attachments and recipe `key -> slot:item-unit` entries.
/// Missing items or bones are skipped in the same way as the original converter.
pub fn load(
    access: &mut impl AssetAccess,
    unit: &unit::Unit,
    parts: &[unit::Part],
    skeleton: &Value,
    listing: &BTreeSet<String>,
    extra: &BTreeMap<String, String>,
) -> Result<Vec<OwnedAttachment>, String> {
    let exists = |p: &str| listing.contains(&p.replace('\\', "/").to_lowercase());
    let names: BTreeSet<_> = skeleton["Skeletons"][0]["Bones"]
        .as_array()
        .ok_or("missing bones")?
        .iter()
        .filter_map(|b| b["Name"].as_str().map(str::to_owned))
        .collect();
    let upm = skeleton["ArtToolInfo"]["UnitsPerMeter"]
        .as_f64()
        .filter(|x| *x > 0.)
        .unwrap_or(1.);
    let mut requests: Vec<(String, unit::Attachment, String)> = parts
        .iter()
        .filter_map(|p| {
            p.attachment
                .clone()
                .map(|a| (p.part.clone(), a, String::new()))
        })
        .collect();
    for (key, spec) in extra {
        let (slot, item) = spec
            .split_once(':')
            .ok_or_else(|| format!("invalid attachment {spec}"))?;
        let slot_name = match slot {
            "shield" => "shield",
            "lhand" => "lefthand",
            "rhand" => "righthand",
            "head" => "head",
            _ => slot,
        };
        let (translation, rotation, scale) = unit
            .slot_transforms
            .get(slot_name)
            .copied()
            .unwrap_or(([0.; 3], [0.; 3], [1.; 3]));
        requests.push((
            slot.into(),
            unit::Attachment {
                item: item.into(),
                bone: String::new(),
                translation,
                rotation,
                scale,
            },
            format!("attach:{key}"),
        ));
    }
    let mut output = Vec::new();
    for (slot, att, explicit_name) in requests {
        if !exists(&att.item) {
            continue;
        }
        let choices: Vec<&str> = if !att.bone.is_empty() {
            vec![att.bone.as_str()]
        } else if slot == "shield" {
            vec!["shield_attach", "L_weapon_attach"]
        } else if slot == "head" {
            vec!["head_attach", "C_head", "head"]
        } else if slot.starts_with("lhand") {
            vec!["L_weapon_attach"]
        } else {
            vec!["R_weapon_attach"]
        };
        let Some(bone) = choices.into_iter().find(|s| names.contains(*s)) else {
            continue;
        };
        let item = unit::load_unit(&att.item, |p| access.read(p), exists, false)?;
        let holder = if explicit_name.is_empty() {
            format!(
                "attach:{}",
                att.item
                    .rsplit('/')
                    .next()
                    .unwrap_or(&att.item)
                    .trim_end_matches(".json")
            )
        } else {
            explicit_name
        };
        for part in item.parts {
            let Some(path) = part
                .model
                .map(|p| unit::model_file(&p, 0, exists))
                .filter(|p| exists(p))
            else {
                continue;
            };
            let data = access.granny(&path)?;
            let root = Granny::new(&data)?.root()?;
            output.push(OwnedAttachment {
                name: holder.clone(),
                bone: bone.into(),
                root,
                translation: att.translation.map(|x| x / upm),
                rotation: quat(att.rotation),
                scale: att.scale,
            });
        }
    }
    Ok(output)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn quaternion_uses_xyz_rotation_order() {
        let q = quat([0., 0., 180.]);
        assert!(q[2].abs() > 0.999);
        assert!(q[3].abs() < 1e-9);
    }
}
