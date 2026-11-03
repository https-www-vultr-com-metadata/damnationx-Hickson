//! Resolve the shared mapping recipe against converted model sidecars.
use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Debug, Clone)]
pub struct ModelSpec {
    pub key: String,
    pub unit: String,
    pub parts: BTreeMap<String, String>,
    pub clips: Vec<String>,
    pub all_variants: bool,
    pub variants: BTreeSet<String>,
    pub attachments: BTreeMap<String, String>,
    pub dye: BTreeMap<String, String>,
    /// Other units' `combined.animations` files whose clips the model also takes (recipe `animations`).
    pub animations: Vec<String>,
}

/// A mapped model and its variant PNGs, ready for native or browser storage.
pub struct ConvertedModel {
    pub glb: Vec<u8>,
    pub sidecar: Value,
    pub files: BTreeMap<String, Vec<u8>>,
}
pub fn convert_spec(
    access: &mut impl crate::import_model::AssetAccess,
    spec: &ModelSpec,
) -> Result<ConvertedModel, String> {
    convert_spec_with_limit(access, spec, 1024)
}
pub fn convert_spec_with_limit(
    access: &mut impl crate::import_model::AssetAccess,
    spec: &ModelSpec,
    texture_limit: u32,
) -> Result<ConvertedModel, String> {
    let unit = unit_path(&spec.unit, &access.listing()?)?;
    let result = crate::import_model::convert_recipe_with_sources(
        access,
        &unit,
        &spec.parts,
        spec.all_variants,
        &spec.clips,
        &spec.dye,
        &spec.attachments,
        &spec.animations,
        texture_limit,
    )?;
    let baked = crate::variants::bake_with_limit(
        access,
        &unit,
        &spec.key,
        &spec.parts,
        spec.all_variants,
        &spec.variants,
        texture_limit,
    )?;
    let sidecar = json!({"source":unit,"meshes":result.meshes,"bones":result.bones,"clips":result.clips,"variants":baked.variants});
    Ok(ConvertedModel {
        glb: result.glb,
        sidecar,
        files: baked.files,
    })
}

/// Build the set of model conversions referenced by the mapping recipe.
pub fn plan(recipe: &str) -> Result<Vec<ModelSpec>, String> {
    let recipe: toml::Value =
        toml::from_str(recipe).map_err(|e| format!("Invalid mapping recipe: {e}"))?;
    let mut wanted: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    for kind in ["monster", "player", "towner", "object", "item", "unique"] {
        for entry in recipe
            .get(kind)
            .and_then(toml::Value::as_array)
            .into_iter()
            .flatten()
        {
            let mut keys: Vec<&str> = entry
                .get("model")
                .and_then(toml::Value::as_str)
                .into_iter()
                .collect();
            keys.extend(
                entry
                    .get("options")
                    .and_then(toml::Value::as_array)
                    .into_iter()
                    .flatten()
                    .filter_map(toml::Value::as_str),
            );
            for key in keys.into_iter().filter(|k| !k.is_empty()) {
                let variants = wanted.entry(key.to_owned()).or_default();
                if let Some(variant) = entry.get("variant").and_then(toml::Value::as_str) {
                    variants.insert(variant.into());
                }
            }
        }
    }
    let models = recipe
        .get("models")
        .and_then(toml::Value::as_table)
        .ok_or("Missing models table")?;
    let mut result = Vec::new();
    for (key, mut variants) in wanted {
        let spec = models
            .get(&key)
            .ok_or_else(|| format!("No model specification for {key}"))?;
        let unit = spec
            .get("unit")
            .and_then(toml::Value::as_str)
            .ok_or_else(|| format!("Model {key} has no unit"))?
            .to_owned();
        let strings = |field: &str| -> BTreeMap<String, String> {
            spec.get(field)
                .and_then(toml::Value::as_table)
                .into_iter()
                .flatten()
                .filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_owned())))
                .collect()
        };
        let list = |field: &str| -> Vec<String> {
            spec.get(field)
                .and_then(toml::Value::as_array)
                .into_iter()
                .flatten()
                .filter_map(toml::Value::as_str)
                .map(str::to_owned)
                .collect()
        };
        let clips = list("clips");
        variants.extend(
            spec.get("variants")
                .and_then(toml::Value::as_array)
                .into_iter()
                .flatten()
                .filter_map(toml::Value::as_str)
                .map(str::to_owned),
        );
        result.push(ModelSpec {
            key,
            unit,
            parts: strings("parts"),
            clips,
            all_variants: spec
                .get("all_variants")
                .and_then(toml::Value::as_bool)
                .unwrap_or(false),
            variants,
            attachments: strings("attach"),
            dye: strings("dye"),
            animations: list("animations"),
        });
    }
    Ok(result)
}

/// Find the unit definition, including nested object definitions, in an archive listing.
pub fn unit_path(unit: &str, listing: &[String]) -> Result<String, String> {
    let unit = unit.to_lowercase();
    if unit.ends_with(".json") && listing.iter().any(|p| p.eq_ignore_ascii_case(&unit)) {
        return Ok(unit);
    }
    let roots = [
        "data/hd/character/enemy/",
        "data/hd/character/player/",
        "data/hd/character/npc/",
        "data/hd/objects/",
        "data/hd/items/misc/gold/",
        "data/hd/items/misc/potion/",
    ];
    let hits: Vec<_> = listing
        .iter()
        .filter(|p| {
            roots.iter().any(|root| {
                p.strip_prefix(root).is_some_and(|rest| {
                    let depth = rest.bytes().filter(|b| *b == b'/').count();
                    depth == usize::from(*root == "data/hd/objects/")
                        && rest.ends_with(".json")
                        && !["_variant.json", "_state_machine.json", "objects.json"]
                            .iter()
                            .any(|suffix| rest.ends_with(suffix))
                        && rest.rsplit('/').next().unwrap_or("").strip_suffix(".json")
                            == Some(unit.as_str())
                })
            })
        })
        .collect();
    match hits.as_slice() {
        [p] => Ok((*p).clone()),
        [] => Err(format!("No unit named {unit}")),
        _ => Err(format!("Unit {unit} is ambiguous: {hits:?}")),
    }
}

pub(crate) fn glob(pattern: &str, value: &str) -> bool {
    // Recipe patterns use '*' and '?'; iterative backtracking avoids recursion.
    let (p, v) = (pattern.as_bytes(), value.as_bytes());
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

pub fn resolve_clip(
    spec: &str,
    clips: &[String],
    weapon_class: Option<&str>,
    unit: &str,
) -> Option<String> {
    for alt in spec.split('|') {
        let alt = alt.replace("{wc}", weapon_class.unwrap_or("hth"));
        if alt.contains('*') {
            let exact = alt.replacen('*', unit, 1);
            if clips.contains(&exact) {
                return Some(exact);
            }
            if let Some(hit) = clips.iter().filter(|c| glob(&alt, c)).min() {
                return Some(hit.clone());
            }
        } else if clips.contains(&alt) {
            return Some(alt);
        }
    }
    None
}

fn as_map(value: Option<&Value>) -> Map<String, Value> {
    value
        .and_then(Value::as_object)
        .cloned()
        .unwrap_or_default()
}

fn resolve_model(
    key: &str,
    kind: &str,
    anims: &Map<String, Value>,
    mapping: &Value,
    metadata: &BTreeMap<String, Value>,
    problems: &mut BTreeSet<String>,
) -> Map<String, Value> {
    let mut output = Map::new();
    output.insert("model".into(), json!(key));
    output.insert("glb".into(), Value::Null);
    output.insert("clips".into(), json!({}));
    if key.is_empty() {
        return output;
    }
    let Some(info) = metadata.get(key) else {
        problems.insert(format!("{key}: not converted"));
        return output;
    };
    output.insert("glb".into(), json!(format!("{key}.glb")));
    let source = info.get("source").and_then(Value::as_str).unwrap_or("");
    let unit = source
        .rsplit('/')
        .next()
        .unwrap_or("")
        .strip_suffix(".json")
        .unwrap_or("");
    let clips: Vec<String> = info
        .get("clips")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .map(str::to_owned)
        .collect();
    let classes: Vec<Option<String>> = if kind == "player" {
        let classes: BTreeSet<String> = as_map(mapping.get("player_weapon_class"))
            .values()
            .filter_map(Value::as_str)
            .map(str::to_owned)
            .collect();
        classes.into_iter().map(Some).collect()
    } else {
        vec![None]
    };
    // A model's own `anims` (clip names of a unit unlike the kind's usual set) win over the entry's.
    let recipe_model = mapping.get("models").and_then(|m| m.get(key));
    let mut anims = anims.clone();
    anims.extend(as_map(recipe_model.and_then(|m| m.get("anims"))));
    if kind == "player" {
        // Heroes converted with every armour tier show the tier of the D1 armour class and the held
        // D1 weapon; a single-outfit model (a hireling) keeps its own parts and weapon.
        let tiers = recipe_model
            .and_then(|m| m.get("all_variants"))
            .and_then(Value::as_bool)
            .unwrap_or(false);
        output.insert("gear".into(), json!(tiers));
    }
    let mut resolved = Map::new();
    for (anim, spec) in &anims {
        let Some(spec) = spec.as_str() else {
            continue;
        };
        for class in &classes {
            let clip = resolve_clip(spec, &clips, class.as_deref(), unit);
            let name = class
                .as_ref()
                .map_or_else(|| anim.clone(), |class| format!("{anim}@{class}"));
            if clip.is_none()
                && !clips.is_empty()
                && !["town_stand", "town_walk", "talk"].contains(&anim.as_str())
            {
                problems.insert(format!("{key}: no clip for {kind} anim '{anim}' ({spec})"));
            }
            resolved.insert(name, json!(clip));
        }
    }
    output.insert("clips".into(), Value::Object(resolved));
    if clips.is_empty() {
        output.insert("static".into(), json!(true));
    }
    output.insert(
        "_variants".into(),
        info.get("variants").cloned().unwrap_or_else(|| json!({})),
    );
    output
}

/// Return the resolved host manifest and diagnostics from already converted model sidecars.
/// Inputs and output are in memory so the same resolver works in native and browser builds.
pub fn resolve(
    recipe: &str,
    glb_dir: &str,
    metadata: &BTreeMap<String, Value>,
) -> Result<(Value, Vec<String>), String> {
    let recipe: toml::Value =
        toml::from_str(recipe).map_err(|e| format!("Invalid mapping recipe: {e}"))?;
    let recipe = serde_json::to_value(recipe).map_err(|e| e.to_string())?;
    let mut entries = Vec::new();
    let mut problems = BTreeSet::new();
    for (kind, id) in [
        ("player", 0),
        ("monster", 1),
        ("towner", 2),
        ("object", 3),
        ("item", 4),
        ("unique", 1),
    ] {
        let mut anims = as_map(
            recipe
                .get("anims")
                .and_then(|a| a.get(if kind == "unique" { "monster" } else { kind })),
        );
        for entry in recipe
            .get(kind)
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            let Some(e) = entry.as_object() else {
                continue;
            };
            let mut entry_anims = anims.clone();
            entry_anims.extend(as_map(e.get("anims")));
            let key = e.get("model").and_then(Value::as_str).unwrap_or("");
            let mut result = Map::new();
            result.insert("kind".into(), json!(id));
            result.insert(
                "type_id".into(),
                e.get("type_id").cloned().unwrap_or(json!(0)),
            );
            result.insert(
                "id".into(),
                e.get("id")
                    .or_else(|| e.get("name"))
                    .cloned()
                    .unwrap_or(json!("")),
            );
            result.insert(
                "quality".into(),
                e.get("quality").cloned().unwrap_or(Value::Null),
            );
            if let Some(unique) = e.get("unique").filter(|_| kind == "unique") {
                result.insert("unique".into(), unique.clone());
            }
            let mut base = resolve_model(key, kind, &entry_anims, &recipe, metadata, &mut problems);
            let variants = base.remove("_variants").unwrap_or(json!({}));
            result.extend(base);
            if let Some(scale) = e.get("scale") {
                result.insert("scale".into(), scale.clone());
            }
            if let Some(variant) = e.get("variant").and_then(Value::as_str) {
                result.insert("variant".into(), json!(variant));
                if !result["glb"].is_null() && variants.get(variant).is_none() {
                    problems.insert(format!("{key}: variant {variant} not baked"));
                }
                result.insert(
                    "textures".into(),
                    variants.get(variant).cloned().unwrap_or(json!({})),
                );
            }
            if let Some(options) = e.get("options").and_then(Value::as_array) {
                let mut resolved_options = Vec::new();
                for option in options {
                    let Some(key) = option.as_str() else {
                        continue;
                    };
                    let mut model =
                        resolve_model(key, kind, &entry_anims, &recipe, metadata, &mut problems);
                    model.remove("_variants");
                    let label = recipe
                        .get("models")
                        .and_then(|m| m.get(key))
                        .and_then(|m| m.get("label"))
                        .cloned()
                        .unwrap_or(json!(key));
                    model.insert("label".into(), label);
                    resolved_options.push(Value::Object(model));
                }
                result.insert("options".into(), Value::Array(resolved_options));
            }
            entries.push(Value::Object(result));
        }
        anims.clear();
    }
    Ok((
        json!({"version":2, "glb_dir":glb_dir, "entries":entries}),
        problems.into_iter().collect(),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn plans_requested_models_and_resolves_nested_unit_paths() {
        let recipe = r#"
[models.zombie]
unit = "zombie1"
parts = { head = "lit" }
clips = ["walk*"]
[models.hero]
unit = "amazon"
[[monster]]
model = "zombie"
quality = "close"
variant = "red"
[[player]]
model = "hero"
options = ["hero"]
quality = "close"
"#;
        let specs = plan(recipe).unwrap();
        assert_eq!(specs.len(), 2);
        let zombie = specs.iter().find(|s| s.key == "zombie").unwrap();
        assert_eq!(zombie.parts["head"], "lit");
        assert!(zombie.variants.contains("red"));
        assert_eq!(
            unit_path(
                "chest_small",
                &["data/hd/objects/chests/chest_small.json".into()]
            )
            .unwrap(),
            "data/hd/objects/chests/chest_small.json"
        );
    }
    #[test]
    fn wildcard_resolution_prefers_unit_then_sorted_fallback() {
        let clips = vec!["z_opened".into(), "a_opened".into(), "door_opened".into()];
        assert_eq!(
            resolve_clip("*_opened", &clips, None, "door"),
            Some("door_opened".into())
        );
        assert_eq!(
            resolve_clip("*_opened", &clips, None, "missing"),
            Some("a_opened".into())
        );
        assert_eq!(
            resolve_clip("{wc}_walk|hth_walk", &["hth_walk".into()], Some("1hs"), ""),
            Some("hth_walk".into())
        );
    }
    #[test]
    fn builds_model_variants_and_options() {
        let recipe = r#"
[anims.player]
walk = "{wc}_walk|hth_walk"
[player_weapon_class]
0 = "hth"
[models.hero]
label = "Hero"
[[player]]
id = "warrior"
quality = "close"
model = "hero"
variant = "red"
options = ["hero"]
"#;
        let metadata = BTreeMap::from([(
            "hero".into(),
            json!({"source":"data/player/hero.json", "clips":["hth_walk"], "variants":{"red":{"albedo":"red.png"}}}),
        )]);
        let (value, problems) = resolve(recipe, "cache/glb", &metadata).unwrap();
        assert!(problems.is_empty());
        let entry = &value["entries"][0];
        assert_eq!(entry["clips"]["walk@hth"], "hth_walk");
        assert_eq!(entry["textures"]["albedo"], "red.png");
        assert_eq!(entry["options"][0]["label"], "Hero");
    }
    #[test]
    fn hireling_option_uses_its_own_clip_names_and_sources_without_gear_tiers() {
        let recipe = r#"
[anims.player]
walk = "{wc}_run|hth_run"
attack = "{wc}_attack1|hth_attack1"
[player_weapon_class]
0 = "hth"
4 = "bow"
[models.hero]
unit = "amazon"
all_variants = true
[models.hire]
unit = "roguehire"
label = "Rogue Hireling"
animations = ["data/hd/character/npc/navi/animation/combined.animations"]
anims = { walk = "run|walk", attack = "attack1" }
[[player]]
type_id = 1
model = "hero"
options = ["hero", "hire"]
"#;
        let hire = plan(recipe)
            .unwrap()
            .into_iter()
            .find(|s| s.key == "hire")
            .unwrap();
        assert_eq!(
            hire.animations,
            ["data/hd/character/npc/navi/animation/combined.animations"]
        );
        let metadata = BTreeMap::from([
            (
                "hero".into(),
                json!({"source":"data/player/amazon.json", "clips":["bow_run","hth_run","bow_attack1","hth_attack1"]}),
            ),
            (
                "hire".into(),
                json!({"source":"data/enemy/roguehire.json", "clips":["run","walk","attack1"]}),
            ),
        ]);
        let (value, problems) = resolve(recipe, "glb", &metadata).unwrap();
        assert!(problems.is_empty(), "{problems:?}");
        let options = &value["entries"][0]["options"];
        assert_eq!(options[0]["clips"]["walk@bow"], "bow_run");
        assert_eq!(options[0]["gear"], true);
        assert_eq!(options[1]["label"], "Rogue Hireling");
        assert_eq!(options[1]["clips"]["walk@bow"], "run");
        assert_eq!(options[1]["clips"]["attack@hth"], "attack1");
        assert_eq!(options[1]["gear"], false);
    }
}
