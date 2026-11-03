//! WebAssembly bindings for portable D2R conversion primitives.
//! The full import orchestration is still under development.
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;

struct BrowserAssets(JsValue, Option<Vec<String>>);

impl BrowserAssets {
    fn method(&self, name: &str) -> Result<js_sys::Function, String> {
        js_sys::Reflect::get(&self.0, &JsValue::from_str(name))
            .map_err(|e| format!("cannot access {name}: {e:?}"))?
            .dyn_into()
            .map_err(|_| format!("missing browser asset method {name}"))
    }
    fn read(&self, path: &str, flatten: bool) -> Result<Vec<u8>, String> {
        let value = self
            .method("read")?
            .call2(
                &self.0,
                &JsValue::from_str(path),
                &JsValue::from_bool(flatten),
            )
            .map_err(|e| {
                e.as_string()
                    .unwrap_or_else(|| format!("asset read failed: {e:?}"))
            })?;
        if !value.is_instance_of::<js_sys::Uint8Array>() {
            return Err("browser asset read did not return bytes".into());
        }
        Ok(js_sys::Uint8Array::new(&value).to_vec())
    }
}
impl d2r_converter::import_model::AssetAccess for BrowserAssets {
    fn listing(&mut self) -> Result<Vec<String>, String> {
        if let Some(listing) = &self.1 {
            return Ok(listing.clone());
        }
        let value = self.method("listing")?.call0(&self.0).map_err(|e| {
            e.as_string()
                .unwrap_or_else(|| format!("asset listing failed: {e:?}"))
        })?;
        let listing = value
            .as_string()
            .ok_or("browser asset listing is not text")?;
        let paths = listing
            .lines()
            .filter_map(|line| line.split_once('\t'))
            .filter_map(|(_, name)| name.strip_prefix("data:"))
            .map(|name| name.replace('\\', "/").to_lowercase())
            .collect::<Vec<_>>();
        self.1 = Some(paths.clone());
        Ok(paths)
    }
    fn read(&mut self, path: &str) -> Result<Vec<u8>, String> {
        BrowserAssets::read(self, path, false)
    }
    fn granny(&mut self, path: &str) -> Result<Vec<u8>, String> {
        BrowserAssets::read(self, path, true)
    }
}

#[wasm_bindgen]
pub struct ConvertedModel {
    glb: Vec<u8>,
    metadata: String,
}
#[wasm_bindgen]
impl ConvertedModel {
    #[wasm_bindgen(getter)]
    pub fn glb(&self) -> Vec<u8> {
        self.glb.clone()
    }
    #[wasm_bindgen(getter)]
    pub fn metadata(&self) -> String {
        self.metadata.clone()
    }
}

/// Convert one explicitly named unit through the synchronous local CASC bridge.
/// Category selection and manifest orchestration are handled by the worker.
#[wasm_bindgen]
pub fn convert_model(
    bridge: JsValue,
    path: &str,
    overrides_json: &str,
    all_variants: bool,
) -> Result<ConvertedModel, JsValue> {
    let overrides: std::collections::BTreeMap<String, String> =
        serde_json::from_str(overrides_json).map_err(|e| JsValue::from_str(&e.to_string()))?;
    let model = d2r_converter::import_model::convert(
        &mut BrowserAssets(bridge, None),
        path,
        &overrides,
        all_variants,
    )
    .map_err(|e| JsValue::from_str(&e))?;
    let metadata = serde_json::json!({ "source": path, "bones": model.bones, "meshes": model.meshes, "clips": model.clips }).to_string();
    Ok(ConvertedModel {
        glb: model.glb,
        metadata,
    })
}

/// Resolve environment source models and transforms from a recipe and local archive.
#[wasm_bindgen]
pub fn plan_environment(bridge: JsValue, spec_json: &str) -> Result<String, JsValue> {
    let spec: serde_json::Value =
        serde_json::from_str(spec_json).map_err(|e| JsValue::from_str(&e.to_string()))?;
    let items = d2r_converter::environment::spec_items(&mut BrowserAssets(bridge, None), &spec)
        .map_err(|e| JsValue::from_str(&e))?;
    let result: Vec<_> = items
        .into_iter()
        .map(|i| serde_json::json!({"name":i.name,"model":i.model,"matrix":i.matrix}))
        .collect();
    Ok(serde_json::Value::Array(result).to_string())
}

/// Convert one non-authored environment recipe. Full environment manifests,
/// materials, socket contracts and authored architecture are still pending.
#[wasm_bindgen]
pub fn convert_environment(
    bridge: JsValue,
    spec_json: &str,
    lod: usize,
) -> Result<ConvertedModel, JsValue> {
    let spec: serde_json::Value =
        serde_json::from_str(spec_json).map_err(|e| JsValue::from_str(&e.to_string()))?;
    let model =
        d2r_converter::environment::convert_spec(&mut BrowserAssets(bridge, None), &spec, lod)
            .map_err(|e| JsValue::from_str(&e))?;
    let metadata = serde_json::json!({"bounds_min":model.bounds_min,"bounds_max":model.bounds_max,"triangles":model.triangles}).to_string();
    Ok(ConvertedModel {
        glb: model.glb,
        metadata,
    })
}

fn files_to_js(
    files: std::collections::BTreeMap<String, Vec<u8>>,
    prefix: &str,
) -> Result<JsValue, JsValue> {
    let object = js_sys::Object::new();
    for (name, bytes) in files {
        let array = js_sys::Uint8Array::from(bytes.as_slice());
        js_sys::Reflect::set(
            &object,
            &JsValue::from_str(&format!("{prefix}{name}")),
            &array,
        )?;
    }
    Ok(object.into())
}

#[wasm_bindgen]
pub fn convert_environment_material(
    bridge: JsValue,
    spec_json: &str,
    texture_limit: u32,
) -> Result<JsValue, JsValue> {
    let spec: serde_json::Value =
        serde_json::from_str(spec_json).map_err(|e| JsValue::from_str(&e.to_string()))?;
    let (manifest, files) = d2r_converter::environment::convert_material(
        &mut BrowserAssets(bridge, None),
        &spec,
        texture_limit,
    )
    .map_err(|e| JsValue::from_str(&e))?;
    let result = js_sys::Object::new();
    js_sys::Reflect::set(
        &result,
        &JsValue::from_str("manifest"),
        &JsValue::from_str(&manifest.to_string()),
    )?;
    js_sys::Reflect::set(
        &result,
        &JsValue::from_str("files"),
        &files_to_js(files, "")?,
    )?;
    Ok(result.into())
}

#[wasm_bindgen]
pub fn convert_hud(bridge: JsValue, game_build: &str) -> Result<JsValue, JsValue> {
    let files = d2r_converter::ui::extract_hud(&mut BrowserAssets(bridge, None), game_build)
        .map_err(|e| JsValue::from_str(&e))?;
    files_to_js(files, "ui/")
}

#[wasm_bindgen]
pub fn convert_portal(bridge: JsValue, game_build: &str) -> Result<JsValue, JsValue> {
    let result =
        d2r_converter::vfx::build_town_portal(&mut BrowserAssets(bridge, None), game_build)
            .map_err(|e| JsValue::from_str(&e))?;
    files_to_js(result.files, "")
}

/// Parse flattened Granny data from the local CASC bridge into JSON.
#[wasm_bindgen]
pub fn parse_granny(bytes: &[u8]) -> Result<String, JsValue> {
    let mut granny =
        d2r_converter::granny::Granny::new(bytes).map_err(|e| JsValue::from_str(&e))?;
    let root = granny.root().map_err(|e| JsValue::from_str(&e))?;
    serde_json::to_string(&root).map_err(|e| JsValue::from_str(&e.to_string()))
}

#[wasm_bindgen]
pub struct DecodedTexture {
    width: u32,
    height: u32,
    rgba: Vec<u8>,
}

#[wasm_bindgen]
impl DecodedTexture {
    #[wasm_bindgen(getter)]
    pub fn width(&self) -> u32 {
        self.width
    }
    #[wasm_bindgen(getter)]
    pub fn height(&self) -> u32 {
        self.height
    }
    /// A copy is returned so callers can release WASM allocations before posting it.
    #[wasm_bindgen(getter)]
    pub fn rgba(&self) -> Vec<u8> {
        self.rgba.clone()
    }
}

#[wasm_bindgen]
pub fn decode_texture(bytes: &[u8], mip: usize) -> Result<DecodedTexture, JsValue> {
    let image = d2r_converter::texture::decode(bytes, mip).map_err(|e| JsValue::from_str(&e))?;
    Ok(DecodedTexture {
        width: image.width,
        height: image.height,
        rgba: image.rgba,
    })
}

#[wasm_bindgen]
pub struct DecodedSprite {
    image: DecodedTexture,
    metadata: String,
}

#[wasm_bindgen]
impl DecodedSprite {
    #[wasm_bindgen(getter)]
    pub fn width(&self) -> u32 {
        self.image.width
    }
    #[wasm_bindgen(getter)]
    pub fn height(&self) -> u32 {
        self.image.height
    }
    #[wasm_bindgen(getter)]
    pub fn rgba(&self) -> Vec<u8> {
        self.image.rgba.clone()
    }
    #[wasm_bindgen(getter)]
    pub fn metadata(&self) -> String {
        self.metadata.clone()
    }
}

/// Decode and losslessly repack a sprite. Zero max_edge means no repacking.
#[wasm_bindgen]
pub fn decode_sprite(bytes: &[u8], max_edge: u32, columns: u32) -> Result<DecodedSprite, JsValue> {
    let sprite = d2r_converter::ui::decode_sprite(bytes).map_err(|e| JsValue::from_str(&e))?;
    let sprite = if max_edge == 0 {
        sprite
    } else {
        d2r_converter::ui::pack_atlas(&sprite, max_edge, columns)
            .map_err(|e| JsValue::from_str(&e))?
    };
    let frames: Vec<_> = sprite
        .frames
        .iter()
        .map(|f| {
            serde_json::json!({
                "index": f.index, "rect": f.rect, "uv": f.uv,
            })
        })
        .collect();
    let metadata = serde_json::json!({
        "logical_frame_width": sprite.logical_frame_width,
        "origin_raw": sprite.origin_raw, "origin": sprite.origin,
        "header_unknown": sprite.header_unknown, "frames": frames,
    })
    .to_string();
    Ok(DecodedSprite {
        image: DecodedTexture {
            width: sprite.image.width,
            height: sprite.image.height,
            rgba: sprite.image.rgba,
        },
        metadata,
    })
}

/// Parse a source-only TOML recipe for browser orchestration.
#[wasm_bindgen]
pub fn recipe_json(recipe: &str) -> Result<String, JsValue> {
    let value: toml::Value =
        toml::from_str(recipe).map_err(|e| JsValue::from_str(&e.to_string()))?;
    serde_json::to_string(&value).map_err(|e| JsValue::from_str(&e.to_string()))
}

#[wasm_bindgen]
pub fn convert_mapping_spec(
    bridge: JsValue,
    recipe: &str,
    key: &str,
    texture_limit: u32,
) -> Result<JsValue, JsValue> {
    let plan = d2r_converter::mapping::plan(recipe).map_err(|e| JsValue::from_str(&e))?;
    let spec = plan
        .iter()
        .find(|s| s.key == key)
        .ok_or_else(|| JsValue::from_str("unknown model mapping"))?;
    let output = d2r_converter::mapping::convert_spec_with_limit(
        &mut BrowserAssets(bridge, None),
        spec,
        texture_limit,
    )
    .map_err(|e| JsValue::from_str(&e))?;
    let result = js_sys::Object::new();
    js_sys::Reflect::set(
        &result,
        &JsValue::from_str("glb"),
        &js_sys::Uint8Array::from(output.glb.as_slice()),
    )?;
    js_sys::Reflect::set(
        &result,
        &JsValue::from_str("sidecar"),
        &JsValue::from_str(&output.sidecar.to_string()),
    )?;
    js_sys::Reflect::set(
        &result,
        &JsValue::from_str("files"),
        &files_to_js(output.files, "")?,
    )?;
    Ok(result.into())
}

#[wasm_bindgen]
pub fn resolve_mapping(recipe: &str, sidecars_json: &str) -> Result<String, JsValue> {
    let sidecars: std::collections::BTreeMap<String, serde_json::Value> =
        serde_json::from_str(sidecars_json).map_err(|e| JsValue::from_str(&e.to_string()))?;
    let (manifest, _problems) = d2r_converter::mapping::resolve(recipe, "glb", &sidecars)
        .map_err(|e| JsValue::from_str(&e))?;
    Ok(manifest.to_string())
}

/// Emit environment files through a callback, keeping publication atomic in JS.
#[wasm_bindgen]
pub fn convert_environment_category(
    bridge: JsValue,
    recipe: &str,
    game_build: &str,
    texture_limit: u32,
    emit: js_sys::Function,
) -> Result<String, JsValue> {
    let output = d2r_converter::environment::build(
        &mut BrowserAssets(bridge, None),
        recipe,
        game_build,
        texture_limit,
        |path, bytes| {
            let result = emit.call2(
                &JsValue::NULL,
                &JsValue::from_str(path),
                &js_sys::Uint8Array::from(bytes),
            );
            result.map(|_| ()).map_err(|e| {
                e.as_string()
                    .unwrap_or_else(|| format!("could not emit environment file: {e:?}"))
            })
        },
    )
    .map_err(|e| JsValue::from_str(&e))?;
    Ok(output.to_string())
}
