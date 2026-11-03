//! Decode D2R unit definitions and select the model parts to convert.
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Debug, PartialEq)]
pub struct Attachment {
    pub item: String,
    pub bone: String,
    pub translation: [f64; 3],
    pub rotation: [f64; 3],
    pub scale: [f64; 3],
}
#[derive(Clone, Debug, PartialEq)]
pub struct Part {
    pub part: String,
    pub variant: String,
    pub model: Option<String>,
    pub attachment: Option<Attachment>,
}
#[derive(Clone, Debug, PartialEq)]
pub struct Unit {
    pub name: String,
    pub path: String,
    pub skeleton: Option<String>,
    pub parts: Vec<Part>,
    pub animations: BTreeMap<String, String>,
    pub combined: Option<String>,
    pub root_rotation: Option<[f64; 4]>,
    pub root_scale: [f64; 3],
    pub slot_transforms: BTreeMap<String, ([f64; 3], [f64; 3], [f64; 3])>,
    pub skipped: Vec<String>,
}
fn text(value: &Value, key: &str) -> String {
    value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_owned()
}
fn xyz(value: &Value, fallback: f64) -> [f64; 3] {
    ["x", "y", "z"].map(|key| value.get(key).and_then(Value::as_f64).unwrap_or(fallback))
}
fn parse_json(bytes: &[u8]) -> Result<Value, String> {
    let bytes = bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(bytes);
    serde_json::from_slice(bytes).map_err(|e| format!("Invalid unit JSON: {e}"))
}

/// Parse one unit with caller-provided archive access; works with browser and native storage.
pub fn load_unit(
    path: &str,
    mut read: impl FnMut(&str) -> Result<Vec<u8>, String>,
    exists: impl Fn(&str) -> bool,
    physics: bool,
) -> Result<Unit, String> {
    let data = parse_json(&read(path)?)?;
    let base = path.strip_suffix(".json").unwrap_or(path);
    let entities = data
        .get("entities")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    let components = |entity: &Value| {
        entity
            .get("components")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default()
    };
    let has_parts = entities.iter().any(|e| {
        components(e)
            .iter()
            .any(|c| text(c, "type") == "UnitPartComponent")
    });
    let mut unit = Unit {
        name: data
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or_else(|| base.rsplit('/').next().unwrap_or(base))
            .into(),
        path: path.into(),
        skeleton: None,
        parts: vec![],
        animations: BTreeMap::new(),
        combined: None,
        root_rotation: None,
        root_scale: [1.0; 3],
        slot_transforms: BTreeMap::new(),
        skipped: vec![],
    };
    let mut state_machine = None;
    for entity in entities {
        let components = components(&entity);
        let types: BTreeSet<String> = components.iter().map(|c| text(c, "type")).collect();
        let mut part = String::new();
        let mut variant = String::new();
        let mut model = None;
        let mut attachment = None;
        for c in &components {
            match text(c, "type").as_str() {
                "SkeletonDefinitionComponent" => {
                    let v = text(c, "filename");
                    if !v.is_empty() {
                        unit.skeleton = Some(v);
                    }
                }
                "UnitRootComponent" => {
                    let sm = text(c, "state_machine_filename");
                    if !sm.is_empty() {
                        state_machine = Some(sm);
                    }
                    if let Some(r) = c.get("rotationOverride").filter(|v| v.is_object()) {
                        unit.root_rotation = Some(
                            ["x", "y", "z", "w"]
                                .map(|k| r.get(k).and_then(Value::as_f64).unwrap_or(0.0)),
                        );
                    }
                }
                "TransformDefinitionComponent" if text(&entity, "name") == "entity_root" => {
                    unit.root_scale = xyz(&c["scale"], 1.0)
                }
                "DefaultAttachmentTransformComponent" => {
                    if let Some(transforms) = c.get("defaultTransforms").and_then(Value::as_object)
                    {
                        for transform in transforms.values() {
                            let name = text(transform, "name");
                            let slot = name.rsplit('_').next().unwrap_or("").to_lowercase();
                            unit.slot_transforms.insert(
                                slot,
                                (
                                    xyz(&transform["translation"], 0.0),
                                    xyz(&transform["rotation"], 0.0),
                                    xyz(&transform["scale"], 1.0),
                                ),
                            );
                        }
                    }
                }
                "UnitPartComponent" => {
                    part = text(c, "part");
                    variant = text(c, "variant");
                }
                "ModelDefinitionComponent" => {
                    let v = text(c, "filename");
                    if !v.is_empty() {
                        model = Some(v);
                    }
                }
                "UnitAttachmentComponent" => {
                    let item = text(c, "filename");
                    if !item.is_empty() {
                        let tr = &c["attachmentTransform"];
                        attachment = Some(Attachment {
                            item,
                            bone: text(c, "overrideBoneName"),
                            translation: xyz(&tr["translation"], 0.0),
                            rotation: xyz(&tr["rotation"], 0.0),
                            scale: xyz(&tr["scale"], 1.0),
                        });
                    }
                }
                _ => (),
            }
        }
        if types.contains("PhysicsBodyDefinitionComponent")
            && !types.contains("UnitPartComponent")
            && has_parts
            && !physics
        {
            if let Some(model) = model {
                unit.skipped.push(format!(
                    "{}: physics piece {}",
                    text(&entity, "name"),
                    model.rsplit('/').next().unwrap_or(&model)
                ));
            }
            continue;
        }
        if !part.is_empty() && model.is_none() {
            for candidate in [
                format!("{base}/{part}_{variant}.model"),
                format!("{base}/{part}.model"),
                format!("{base}/model/{part}_{variant}.model"),
                format!("{base}/model/{part}.model"),
            ] {
                if exists(&candidate.replace(".model", "_lod0.model")) || exists(&candidate) {
                    model = Some(candidate);
                    break;
                }
            }
        }
        if model.is_some() || attachment.is_some() {
            if part.is_empty() {
                part = text(&entity, "name");
                part = part.strip_prefix("entity_").unwrap_or(&part).to_owned();
            }
            unit.parts.push(Part {
                part,
                variant,
                model,
                attachment,
            });
        }
    }
    if let Some(sm) = state_machine.filter(|p| exists(p)) {
        let machine = parse_json(&read(&sm)?)?;
        for anim in machine
            .get("animations")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            let name = text(anim, "name");
            let filename = text(anim, "filename");
            unit.animations.insert(name, filename.clone());
            if unit.combined.is_none() {
                let directory = filename.rsplit_once('/').map(|(dir, _)| dir).unwrap_or("");
                let combined = if directory.is_empty() {
                    "combined.animations".into()
                } else {
                    format!("{directory}/combined.animations")
                };
                if exists(&combined) {
                    unit.combined = Some(combined);
                }
            }
        }
    }
    if unit.combined.is_none() {
        let combined = format!("{base}/animation/combined.animations");
        if exists(&combined) {
            unit.combined = Some(combined);
        }
    }
    Ok(unit)
}

pub fn choose_parts(
    unit: &Unit,
    overrides: &BTreeMap<String, String>,
    all_variants: bool,
) -> Result<Vec<Part>, String> {
    if all_variants {
        return Ok(unit
            .parts
            .iter()
            .filter(|p| overrides.get(&p.part).map(String::as_str) != Some("none"))
            .cloned()
            .collect());
    }
    // Preserve source order of part groups, like the original converter.
    let mut names = Vec::new();
    for part in &unit.parts {
        if !names.contains(&part.part) {
            names.push(part.part.clone());
        }
    }
    let mut selected = Vec::new();
    for name in names {
        let options: Vec<&Part> = unit.parts.iter().filter(|p| p.part == name).collect();
        let want = overrides.get(&name).map(String::as_str).unwrap_or("");
        if want == "none" {
            continue;
        }
        let chosen = if !want.is_empty() {
            options
                .iter()
                .find(|p| p.variant == want)
                .copied()
                .ok_or_else(|| format!("part {name}: no variant {want}"))?
        } else {
            ["lit", "med", "hvy"]
                .iter()
                .find_map(|v| options.iter().find(|p| p.variant == *v).copied())
                .unwrap_or_else(|| *options.iter().min_by_key(|p| &p.variant).unwrap())
        };
        selected.push(chosen.clone());
    }
    Ok(selected)
}

pub fn model_file(model: &str, lod: usize, exists: impl Fn(&str) -> bool) -> String {
    let base = model.strip_suffix(".model").unwrap_or(model);
    for lod in [lod, 0] {
        let candidate = format!("{base}_lod{lod}.model");
        if exists(&candidate) {
            return candidate;
        }
    }
    model.into()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn resolves_parts_skeleton_and_state_machine() {
        let unit = br#"{"name":"hero","entities":[{"name":"entity_root","components":[{"type":"UnitRootComponent","state_machine_filename":"data/hero/state.json"},{"type":"SkeletonDefinitionComponent","filename":"data/hero/hero.skeleton"}]},{"name":"body","components":[{"type":"UnitPartComponent","part":"torso","variant":"hvy"}]},{"name":"body","components":[{"type":"UnitPartComponent","part":"torso","variant":"lit"}]}]}"#;
        let state =
            br#"{"animations":[{"name":"walk","filename":"data/hero/animation/walk.animation"}]}"#;
        let files = BTreeMap::from([
            ("data/hero.json", unit.as_slice()),
            ("data/hero/state.json", state.as_slice()),
        ]);
        let exists = |p: &str| {
            files.contains_key(p)
                || p == "data/hero/torso_lit_lod0.model"
                || p == "data/hero/torso_hvy.model"
                || p == "data/hero/animation/combined.animations"
        };
        let u = load_unit(
            "data/hero.json",
            |p| files.get(p).map(|v| v.to_vec()).ok_or_else(|| p.into()),
            exists,
            false,
        )
        .unwrap();
        assert_eq!(u.skeleton.as_deref(), Some("data/hero/hero.skeleton"));
        assert_eq!(
            u.combined.as_deref(),
            Some("data/hero/animation/combined.animations")
        );
        let selected = choose_parts(&u, &BTreeMap::new(), false).unwrap();
        assert_eq!(selected[0].variant, "lit");
        assert_eq!(
            model_file(selected[0].model.as_ref().unwrap(), 1, exists),
            "data/hero/torso_lit_lod0.model"
        );
        assert!(
            choose_parts(&u, &BTreeMap::from([("torso".into(), "bad".into())]), false).is_err()
        );
    }
}
