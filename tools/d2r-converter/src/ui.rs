//! Decoding and lossless repacking of D2R's SpA1 UI sprite atlases.
use crate::texture::Image;

#[derive(Debug, Clone, PartialEq)]
pub struct Frame {
    pub index: usize,
    pub rect: [u32; 4],
    pub uv: [f64; 4],
}

#[derive(Debug, Clone, PartialEq)]
pub struct Sprite {
    pub image: Image,
    pub logical_frame_width: u16,
    pub origin_raw: u32,
    pub origin: [i16; 2],
    pub header_unknown: [u32; 2],
    pub frames: Vec<Frame>,
}

fn u16_at(data: &[u8], offset: usize) -> Result<u16, String> {
    Ok(u16::from_le_bytes(
        data.get(offset..offset + 2)
            .ok_or("truncated sprite")?
            .try_into()
            .unwrap(),
    ))
}
fn u32_at(data: &[u8], offset: usize) -> Result<u32, String> {
    Ok(u32::from_le_bytes(
        data.get(offset..offset + 4)
            .ok_or("truncated sprite")?
            .try_into()
            .unwrap(),
    ))
}

/// Decode an SpA1 RGBA8 horizontal atlas, preserving logical frame width and origin.
pub fn decode_sprite(data: &[u8]) -> Result<Sprite, String> {
    if data.len() < 40 || &data[..4] != b"SpA1" {
        return Err("not a complete SpA1 sprite".into());
    }
    let format = u16_at(data, 4)?;
    let logical_frame_width = u16_at(data, 6)?;
    let width = u32_at(data, 8)?;
    let height = u32_at(data, 12)?;
    let origin_raw = u32_at(data, 16)?;
    let count = u32_at(data, 20)?;
    let header_unknown = [u32_at(data, 24)?, u32_at(data, 28)?];
    let size = u32_at(data, 32)? as usize;
    let channels = u32_at(data, 36)?;
    if format != 31 || channels != 4 {
        return Err(format!(
            "unsupported SpA1 format {format}, channels {channels}"
        ));
    }
    if width == 0 || height == 0 || count == 0 || count > width || logical_frame_width == 0 {
        return Err("invalid SpA1 dimensions/frame count".into());
    }
    let expected = (width as usize)
        .checked_mul(height as usize)
        .and_then(|v| v.checked_mul(4))
        .ok_or("sprite dimensions overflow")?;
    if size != expected || data.len().checked_sub(40) != Some(size) {
        return Err("SpA1 RGBA payload size mismatch".into());
    }
    let frames = (0..count)
        .map(|index| {
            let left = (u64::from(index) * u64::from(width) / u64::from(count)) as u32;
            let right = (u64::from(index + 1) * u64::from(width) / u64::from(count)) as u32;
            Frame {
                index: index as usize,
                rect: [left, 0, right - left, height],
                uv: [
                    index as f64 / count as f64,
                    0.,
                    (index + 1) as f64 / count as f64,
                    1.,
                ],
            }
        })
        .collect();
    Ok(Sprite {
        image: Image {
            width,
            height,
            rgba: data[40..].to_vec(),
        },
        logical_frame_width,
        origin_raw,
        origin: [origin_raw as u16 as i16, (origin_raw >> 16) as u16 as i16],
        header_unknown,
        frames,
    })
}

/// Repack horizontal cells without resampling, retaining their source order.
pub fn pack_atlas(
    sprite: &Sprite,
    max_edge: u32,
    preferred_columns: u32,
) -> Result<Sprite, String> {
    if sprite.image.width.max(sprite.image.height) <= max_edge {
        return Ok(sprite.clone());
    }
    let count = u32::try_from(sprite.frames.len()).map_err(|_| "too many frames")?;
    if count == 0 {
        return Err("oversized image has no frames".into());
    }
    let cell_width = sprite.frames.iter().map(|f| f.rect[2]).max().unwrap();
    let cell_height = sprite.frames.iter().map(|f| f.rect[3]).max().unwrap();
    if cell_width == 0 || cell_height == 0 {
        return Err("empty frame".into());
    }
    let max_columns = count.min(max_edge / cell_width);
    let max_rows = max_edge / cell_height;
    if max_columns == 0
        || max_rows == 0
        || u64::from(max_columns) * u64::from(max_rows) < u64::from(count)
    {
        return Err("atlas cannot fit losslessly".into());
    }
    let columns = count
        .div_ceil(max_rows)
        .max(preferred_columns.min(max_columns));
    let rows = count.div_ceil(columns);
    let width = columns * cell_width;
    let height = rows * cell_height;
    let len = (width as usize)
        .checked_mul(height as usize)
        .and_then(|x| x.checked_mul(4))
        .ok_or("atlas too large")?;
    let source_len = (sprite.image.width as usize)
        .checked_mul(sprite.image.height as usize)
        .and_then(|x| x.checked_mul(4))
        .ok_or("source too large")?;
    if sprite.image.rgba.len() != source_len {
        return Err("invalid source image".into());
    }
    let mut image = Image {
        width,
        height,
        rgba: vec![0; len],
    };
    let mut frames = Vec::with_capacity(count as usize);
    for (i, frame) in sprite.frames.iter().enumerate() {
        let [sx, sy, w, h] = frame.rect;
        if frame.index != i
            || w == 0
            || h == 0
            || sx.checked_add(w).is_none_or(|x| x > sprite.image.width)
            || sy.checked_add(h).is_none_or(|y| y > sprite.image.height)
        {
            return Err("frame outside source image".into());
        }
        let x = i as u32 % columns * cell_width;
        let y = i as u32 / columns * cell_height;
        for row in 0..h {
            let src = (((sy + row) as usize * sprite.image.width as usize) + sx as usize) * 4;
            let dst = (((y + row) as usize * width as usize) + x as usize) * 4;
            image.rgba[dst..dst + w as usize * 4]
                .copy_from_slice(&sprite.image.rgba[src..src + w as usize * 4]);
        }
        frames.push(Frame {
            index: i,
            rect: [x, y, w, h],
            uv: [
                x as f64 / width as f64,
                y as f64 / height as f64,
                (x + w) as f64 / width as f64,
                (y + h) as f64 / height as f64,
            ],
        });
    }
    Ok(Sprite {
        image,
        frames,
        ..sprite.clone()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> Vec<u8> {
        let mut data = vec![0; 40];
        data[..4].copy_from_slice(b"SpA1");
        data[4..6].copy_from_slice(&31u16.to_le_bytes());
        data[6..8].copy_from_slice(&2u16.to_le_bytes());
        for (offset, value) in [
            (8, 6u32),
            (12, 1),
            (16, 0xfffe0001),
            (20, 3),
            (32, 24),
            (36, 4),
        ] {
            data[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        }
        data.extend(0..24);
        data
    }
    #[test]
    fn decode_and_repack_preserves_pixels() {
        let sprite = decode_sprite(&fixture()).unwrap();
        assert_eq!(sprite.origin, [1, -2]);
        let packed = pack_atlas(&sprite, 4, 2).unwrap();
        assert_eq!((packed.image.width, packed.image.height), (4, 2));
        assert_eq!(packed.frames[2].rect, [0, 1, 2, 1]);
        assert_eq!(&packed.image.rgba[16..24], &sprite.image.rgba[16..24]);
    }
    #[test]
    fn reject_bad_headers_and_insufficient_space() {
        let mut data = fixture();
        data.pop();
        assert!(decode_sprite(&data).is_err());
        assert!(pack_atlas(&decode_sprite(&fixture()).unwrap(), 2, 1).is_err());
    }
}

/// Parse D2R layout JSON, which permits comments and trailing commas.
pub fn read_layout(data: &[u8]) -> Result<serde_json::Value, String> {
    let data = data.strip_prefix(b"\xef\xbb\xbf").unwrap_or(data);
    let text = std::str::from_utf8(data).map_err(|e| e.to_string())?;
    let bytes = text.as_bytes();
    let mut clean = Vec::with_capacity(bytes.len());
    let mut i = 0;
    let mut in_string = false;
    while i < bytes.len() {
        if in_string {
            clean.push(bytes[i]);
            if bytes[i] == b'\\' && i + 1 < bytes.len() {
                i += 1;
                clean.push(bytes[i]);
            } else if bytes[i] == b'"' {
                in_string = false;
            }
            i += 1;
        } else if bytes[i] == b'"' {
            in_string = true;
            clean.push(bytes[i]);
            i += 1;
        } else if bytes.get(i..i + 2) == Some(b"//") {
            i += 2;
            while i < bytes.len() && !matches!(bytes[i], b'\n' | b'\r') {
                i += 1;
            }
        } else if bytes.get(i..i + 2) == Some(b"/*") {
            i += 2;
            while i + 1 < bytes.len() && &bytes[i..i + 2] != b"*/" {
                i += 1;
            }
            if i + 1 >= bytes.len() {
                return Err("unterminated layout comment".into());
            }
            i += 2;
        } else {
            clean.push(bytes[i]);
            i += 1;
        }
    }
    let mut out = Vec::with_capacity(clean.len());
    let mut i = 0;
    let mut in_string = false;
    while i < clean.len() {
        let c = clean[i];
        if in_string {
            out.push(c);
            if c == b'\\' && i + 1 < clean.len() {
                i += 1;
                out.push(clean[i]);
            } else if c == b'"' {
                in_string = false;
            }
        } else if c == b'"' {
            in_string = true;
            out.push(c);
        } else if c == b',' {
            let mut j = i + 1;
            while j < clean.len() && clean[j].is_ascii_whitespace() {
                j += 1;
            }
            if !matches!(clean.get(j), Some(b'}' | b']')) {
                out.push(c);
            }
        } else {
            out.push(c);
        }
        i += 1;
    }
    serde_json::from_slice(&out).map_err(|e| e.to_string())
}

#[cfg(test)]
mod layout_tests {
    use super::*;
    #[test]
    fn comments_and_trailing_commas_do_not_damage_strings() {
        let value = read_layout(
            br#"{"url":"http://x/*y*/", "escaped":"\\\"//", /* hi */ "list":[1,2,], // line
        }"#,
        )
        .unwrap();
        assert_eq!(value["url"], "http://x/*y*/");
        assert_eq!(value["list"], serde_json::json!([1, 2]));
        assert!(read_layout(b"{/*").is_err());
    }
}

/// Find native sprite references and their widget paths in a D2R layout.
pub fn layout_images(
    layout: &serde_json::Value,
) -> std::collections::BTreeMap<String, Vec<serde_json::Value>> {
    fn visit(
        node: &serde_json::Value,
        parents: &[String],
        out: &mut std::collections::BTreeMap<String, Vec<serde_json::Value>>,
    ) {
        let Some(object) = node.as_object() else {
            return;
        };
        let name = object.get("name").and_then(|x| x.as_str()).unwrap_or("?");
        let mut path = parents.to_vec();
        path.push(name.to_owned());
        if let Some(fields) = object.get("fields") {
            if let Some(filename) = fields
                .get("filename")
                .and_then(|x| x.as_str())
                .filter(|x| !x.starts_with('$'))
            {
                let normalized = filename.replace('\\', "/");
                let source = format!(
                    "data/hd/global/ui/{}.sprite",
                    normalized.trim_start_matches('/')
                )
                .to_lowercase();
                out.entry(source)
                    .or_default()
                    .push(serde_json::json!({"widget": path.join("/"), "fields": fields}));
            }
        }
        if let Some(children) = object.get("children").and_then(|x| x.as_array()) {
            for child in children {
                visit(child, &path, out);
            }
        }
    }
    let mut out = std::collections::BTreeMap::new();
    visit(layout, &[], &mut out);
    out
}

/// Resolve HUD globe rectangles relative to the overlay, in panel pixels.
pub fn hud_geometry(layout: &serde_json::Value) -> Result<serde_json::Value, String> {
    let children = layout
        .get("children")
        .and_then(|v| v.as_array())
        .ok_or("HUD layout missing children")?;
    let child = |name: &str| {
        children
            .iter()
            .find(|v| v.get("name").and_then(|v| v.as_str()) == Some(name))
            .ok_or_else(|| format!("HUD layout missing {name}"))
    };
    let rect = |node: &serde_json::Value| {
        node.get("fields")
            .and_then(|v| v.get("rect"))
            .cloned()
            .ok_or("HUD layout missing rectangle".to_string())
    };
    let overlay = rect(child("Overlay")?)?;
    let number = |v: &serde_json::Value, key: &str| -> Result<f64, String> {
        v.get(key)
            .and_then(|v| v.as_f64())
            .filter(|v| v.is_finite())
            .ok_or_else(|| format!("invalid HUD rectangle {key}"))
    };
    let ox = overlay.get("x").and_then(|v| v.as_f64()).unwrap_or(0.0);
    let oy = overlay.get("y").and_then(|v| v.as_f64()).unwrap_or(0.0);
    let mut out = serde_json::Map::new();
    for (attribute, name) in [("health", "HealthBall"), ("mana", "ManaBall")] {
        let r = rect(child(name)?)?;
        out.insert(
            format!("{attribute}_fill_rect"),
            serde_json::json!([
                number(&r, "x")? - ox,
                number(&r, "y")? - oy,
                number(&r, "width")?,
                number(&r, "height")?
            ]),
        );
    }
    Ok(out.into())
}

#[cfg(test)]
mod geometry_tests {
    use super::*;
    #[test]
    fn finds_nested_sprites_and_resolves_hud_rects() {
        let layout = serde_json::json!({"name":"root","children":[
            {"name":"Overlay","fields":{"rect":{"x":10,"y":20}}},
            {"name":"HealthBall","fields":{"rect":{"x":11,"y":22,"width":3,"height":4},"filename":"Panel\\Foo"}},
            {"name":"ManaBall","fields":{"rect":{"x":15,"y":26,"width":7,"height":8},"filename":"$variable"}}
        ]});
        assert_eq!(
            layout_images(&layout)["data/hd/global/ui/panel/foo.sprite"][0]["widget"],
            "root/HealthBall"
        );
        assert_eq!(
            hud_geometry(&layout).unwrap()["health_fill_rect"],
            serde_json::json!([1.0, 2.0, 3.0, 4.0])
        );
    }
}

/// Result of extracting the base HUD assets. Panel adaptation and skill table
/// mapping are separate stages and must be completed before release integration.
pub struct HudAssets {
    pub files: std::collections::BTreeMap<String, Vec<u8>>,
    pub images: serde_json::Map<String, serde_json::Value>,
    pub missing: Vec<String>,
}

fn encode_png(image: &Image) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut bytes, image.width, image.height);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().map_err(|e| e.to_string())?;
        writer
            .write_image_data(&image.rgba)
            .map_err(|e| e.to_string())?;
    }
    Ok(bytes)
}

/// Extract the base HUD sprites from an owned installation through the common
/// native/browser storage interface. This does not write or publish a manifest.
pub fn extract_base_hud(
    access: &mut impl crate::import_model::AssetAccess,
) -> Result<HudAssets, String> {
    const HUD: &str = "panel/hud_02/";
    let mut assets: Vec<(&str, String)> = vec![
        ("panel_background", format!("{HUD}front_panel")),
        (
            "health_fill",
            format!("{HUD}healthmanaanimation/healthidle/4k/globe_health_man_idle"),
        ),
        (
            "orb_edge",
            format!("{HUD}healthmanaanimation/hotpoint/frameset1/globe_hot_point"),
        ),
        ("experience_bar", format!("{HUD}experience_bar")),
        ("stamina_bar", format!("{HUD}stamina_bar")),
        ("belt_background", format!("{HUD}belt_popped_row")),
        ("belt_cap", format!("{HUD}belt_popped_cap")),
        ("belt_arrow", format!("{HUD}belt_uparrow")),
    ];
    for (key, value) in [
        ("inventory_background", "panel/inventory/background"),
        (
            "inventory_classic_background",
            "panel/inventory/classic_background",
        ),
        ("shop_background", "panel/vendors/vendorshop_bg"),
        ("shop_repair_background", "panel/vendors/vendorforge_bg"),
        ("panel_close", "panel/closebtn_4x"),
        (
            "character_background",
            "panel/character_sheet/character_background",
        ),
        (
            "spellbook_background",
            "spells/skill_trees/panel_skilltreebg",
        ),
        ("potion_health", "items/misc/potion/healing_potion"),
        ("potion_mana", "items/misc/potion/mana_potion"),
        ("potion_rejuvenation", "items/misc/potion/rejuv_potion"),
        (
            "potion_full_rejuvenation",
            "items/misc/potion/full_rejuv_potion",
        ),
        ("scroll_identify", "items/misc/scroll/identify_scroll"),
        ("scroll_town_portal", "items/misc/scroll/town_portal_scroll"),
        ("book_identify", "items/misc/book/identify_book"),
        ("book_town_portal", "items/misc/book/town_portal_book"),
        ("cursor", "cursor/ohand"),
        ("skill_icons", "spells/submenu/skillicon"),
        ("skill_icons_sorceress", "spells/sorceress/soskillicon"),
        ("skill_icons_paladin", "spells/paladin/paskillicon"),
        ("options_background", "panel/options/frontendoptionsbg"),
        ("options_ingame_background", "pausemenu/settingspanel"),
        (
            "options_controller_background",
            "controller/panel/options/panel_options_bg",
        ),
        ("options_tabs", "controller/panel/stash/v2/stashtabs"),
        (
            "options_checkbox",
            "lobby/creategame/creategame_advancedcheckbox",
        ),
        ("options_divider", "pausemenu/divider"),
        ("options_slider_track", "pausemenu/horizontalscroll"),
        ("options_slider_thumb", "pausemenu/horizontalindicator"),
        ("options_scroll_track", "pausemenu/verticalscroll"),
        ("options_scroll_thumb", "pausemenu/verticalindicator"),
        ("options_arrow_left", "pausemenu/leftarrow"),
        ("options_arrow_right", "pausemenu/rightarrow"),
        ("options_button", "frontend/hd/final/frontend_buttonmed"),
        (
            "options_button_small",
            "frontend/hd/final/frontend_buttonsmall",
        ),
        ("options_pause_button", "pausemenu/pausebutton"),
        ("options_dropdown_left", "panel/dropdownbutton_left"),
        ("options_dropdown_center", "panel/dropdownbutton_center"),
        ("options_dropdown_right", "panel/dropdownbutton_right"),
        ("options_dropdown_list_left", "panel/dropdowntile_left"),
        ("options_dropdown_list_center", "panel/dropdowntile_center"),
        ("options_dropdown_list_right", "panel/dropdowntile_right"),
    ] {
        assets.push((key, value.to_owned()));
    }
    for (key, name) in [
        ("button_character", "character"),
        ("button_inventory", "inventory"),
        ("button_skills", "skilltree"),
        ("button_automap", "automap"),
        ("button_messages", "messages"),
        ("button_quests", "questlog"),
        ("button_menu", "pausemenu"),
        ("button_sprint", "sprint"),
    ] {
        assets.push((key, format!("{HUD}{name}")));
    }
    let listing: std::collections::BTreeSet<_> = access
        .listing()?
        .into_iter()
        .map(|s| s.to_lowercase())
        .collect();
    let mut result = HudAssets {
        files: Default::default(),
        images: Default::default(),
        missing: Vec::new(),
    };
    for (key, relative) in assets {
        let path = format!("data/hd/global/ui/{relative}.sprite").to_lowercase();
        if !listing.contains(&path) {
            result.missing.push(key.to_owned());
            continue;
        }
        let data = access.read(&path)?;
        let (image, frames) = if data.starts_with(b"<DE(") {
            (crate::texture::decode(&data, 0)?, Vec::new())
        } else {
            let sprite = pack_atlas(&decode_sprite(&data)?, 4096, 8)?;
            (sprite.image, sprite.frames)
        };
        let filename = format!("{key}.png");
        result.files.insert(filename.clone(), encode_png(&image)?);
        let frames: Vec<_> = frames
            .iter()
            .map(|frame| serde_json::json!({"index":frame.index,"rect":frame.rect,"uv":frame.uv}))
            .collect();
        result.images.insert(key.to_owned(), serde_json::json!({"file":filename,"width":image.width,"height":image.height,"source":path,"frames":frames}));
        if key == "health_fill" {
            // Rotating HSV hue by two thirds of a turn is the RGB channel
            // permutation (G, B, R), retaining each pixel's alpha and shading.
            let mut mana = image.clone();
            for pixel in mana.rgba.chunks_exact_mut(4) {
                let [r, g, b, _] = [pixel[0], pixel[1], pixel[2], pixel[3]];
                pixel[0] = g;
                pixel[1] = b;
                pixel[2] = r;
            }
            result
                .files
                .insert("mana_fill.png".into(), encode_png(&mana)?);
            let mut description = result.images[key].clone();
            description["file"] = "mana_fill.png".into();
            result.images.insert("mana_fill".into(), description);
        }
    }
    for required in ["panel_background", "health_fill", "belt_background"] {
        if !result.images.contains_key(required) {
            return Err(format!("required HUD sprite {required} missing"));
        }
    }
    Ok(result)
}

/// Resolve desktop inventory or vendor layout variables and validate its geometry.
pub fn panel_geometry(
    layout: &serde_json::Value,
    profile: &serde_json::Value,
    kind: &str,
) -> Result<serde_json::Value, String> {
    use serde_json::{json, Value};
    fn resolve(v: &Value, profile: &Value, stack: &mut Vec<String>) -> Result<Value, String> {
        if let Some(s) = v.as_str().and_then(|s| s.strip_prefix('$')) {
            if stack.iter().any(|x| x == s) {
                return Err(format!("cyclic panel variable {s}"));
            }
            let next = profile
                .get(s)
                .ok_or_else(|| format!("unresolved panel variable {s}"))?;
            stack.push(s.to_owned());
            let out = resolve(next, profile, stack);
            stack.pop();
            out
        } else if let Some(obj) = v.as_object() {
            let mut out = serde_json::Map::new();
            for (k, v) in obj {
                out.insert(k.clone(), resolve(v, profile, stack)?);
            }
            Ok(out.into())
        } else {
            Ok(v.clone())
        }
    }
    fn resolved(v: &Value, profile: &Value) -> Result<Value, String> {
        resolve(v, profile, &mut Vec::new())
    }
    fn num(v: &Value, key: &str) -> Result<f64, String> {
        v.get(key)
            .and_then(Value::as_f64)
            .filter(|n| n.is_finite())
            .ok_or_else(|| format!("invalid panel field {key}"))
    }
    fn rect(v: &Value, profile: &Value) -> Result<[f64; 4], String> {
        let v = resolved(v, profile)?;
        let mut r = [0.; 4];
        for (i, k) in ["x", "y", "width", "height"].iter().enumerate() {
            if v.get(k).is_some() {
                r[i] = num(&v, k)?;
            }
        }
        Ok(r)
    }
    let expected = match kind {
        "inventory" => [10., 4.],
        "shop" => [10., 10.],
        _ => return Err(format!("unknown panel {kind}")),
    };
    let children = layout
        .get("children")
        .and_then(Value::as_array)
        .ok_or("panel children missing")?;
    let fields = |name: &str| -> Result<&Value, String> {
        children
            .iter()
            .find(|v| v.get("name").and_then(Value::as_str) == Some(name))
            .and_then(|v| v.get("fields"))
            .ok_or_else(|| format!("panel missing {name}"))
    };
    let panel = rect(
        layout
            .get("fields")
            .and_then(|v| v.get("rect"))
            .ok_or("panel rect missing")?,
        profile,
    )?;
    if panel[2] <= 0. || panel[3] <= 0. {
        return Err("panel dimensions must be positive".into());
    }
    let grid = fields("grid")?;
    let count = resolved(grid.get("cellCount").ok_or("grid count missing")?, profile)?;
    let cell = resolved(grid.get("cellSize").ok_or("grid size missing")?, profile)?;
    if [num(&count, "x")?, num(&count, "y")?] != expected
        || num(&cell, "x")? <= 0.
        || num(&cell, "y")? <= 0.
    {
        return Err(format!("unsupported {kind} grid"));
    }
    let mut grid_rect = rect(grid.get("rect").ok_or("grid rect missing")?, profile)?;
    grid_rect[2] = expected[0] * num(&cell, "x")?;
    grid_rect[3] = expected[1] * num(&cell, "y")?;
    let field_rect = |name: &str| -> Result<[f64; 4], String> {
        rect(fields(name)?.get("rect").ok_or("rect missing")?, profile)
    };
    let title = field_rect("title")?;
    let gold = field_rect("gold_amount")?;
    let close = field_rect("close")?;
    let equipment = [
        "slot_head",
        "slot_right_hand",
        "slot_left_hand",
        "slot_neck",
        "slot_right_arm",
        "slot_left_arm",
        "slot_torso",
    ];
    let mut equipment_rects = Vec::new();
    let mut disabled_rects = Vec::new();
    if kind == "inventory" {
        for key in equipment {
            equipment_rects.push(field_rect(key)?);
        }
        for key in ["slot_belt", "slot_feet", "slot_gloves"] {
            if fields(key).is_ok() {
                disabled_rects.push(field_rect(key)?);
            }
        }
    }
    for r in std::iter::once(&grid_rect)
        .chain([&title, &gold])
        .chain(equipment_rects.iter())
    {
        if r[0] < 0.
            || r[1] < 0.
            || r[2] <= 0.
            || r[3] <= 0.
            || r[0] + r[2] > panel[2]
            || r[1] + r[3] > panel[3]
        {
            return Err(format!("{kind} content outside native panel"));
        }
    }
    let mut result = json!({"size":[panel[2],panel[3]],"grid_rect":grid_rect,"cell_size":[num(&cell,"x")?,num(&cell,"y")?],"grid_count":[expected[0] as u32,expected[1] as u32],"title_rect":title,"gold_rect":gold,"close_origin":[close[0],close[1]],"background":if kind=="inventory" {"inventory_classic_background"} else {"shop_background"}});
    if kind == "inventory" {
        result["equipment_rects"] = json!(equipment_rects);
        result["equipment_order"] = json!(equipment);
        result["disabled_d2_slots"] = json!(["slot_belt", "slot_feet", "slot_gloves"]);
        result["disabled_slot_rects"] = json!(disabled_rects);
    } else {
        result["content_rect"] = json!(grid_rect);
        result["disabled_d2_actions"] =
            json!(["tabs", "button_repair_all", "button_refresh", "StashWidget"]);
    }
    Ok(result)
}

fn decode_png(bytes: &[u8]) -> Result<Image, String> {
    let mut decoder = png::Decoder::new(bytes);
    decoder.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut reader = decoder.read_info().map_err(|e| e.to_string())?;
    let mut pixels = vec![0; reader.output_buffer_size()];
    let info = reader.next_frame(&mut pixels).map_err(|e| e.to_string())?;
    if info.color_type != png::ColorType::Rgba || info.bit_depth != png::BitDepth::Eight {
        return Err("expected RGBA8 PNG".into());
    }
    pixels.truncate(info.buffer_size());
    Ok(Image {
        width: info.width,
        height: info.height,
        rgba: pixels,
    })
}
fn crop_image(image: &Image, rect: [u32; 4]) -> Result<Image, String> {
    let [x, y, w, h] = rect;
    if w == 0
        || h == 0
        || x.checked_add(w).is_none_or(|v| v > image.width)
        || y.checked_add(h).is_none_or(|v| v > image.height)
    {
        return Err("crop outside image".into());
    }
    let mut out = Image {
        width: w,
        height: h,
        rgba: vec![0; w as usize * h as usize * 4],
    };
    for row in 0..h {
        let src = ((y + row) as usize * image.width as usize + x as usize) * 4;
        let dst = row as usize * w as usize * 4;
        out.rgba[dst..dst + w as usize * 4].copy_from_slice(&image.rgba[src..src + w as usize * 4]);
    }
    Ok(out)
}

/// Construct an inventory background with Diablo II-only equipment sockets
/// replaced by nearby native stone, leaving the original image untouched.
pub fn inventory_background(image: &Image, geometry: &serde_json::Value) -> Result<Image, String> {
    fn array(v: &serde_json::Value) -> Result<[f64; 4], String> {
        let a = v.as_array().ok_or("invalid rectangle")?;
        if a.len() != 4 {
            return Err("invalid rectangle".into());
        }
        let mut out = [0.; 4];
        for i in 0..4 {
            out[i] = a[i]
                .as_f64()
                .filter(|n| n.is_finite())
                .ok_or("invalid rectangle")?;
        }
        Ok(out)
    }
    let size = geometry["size"]
        .as_array()
        .ok_or("missing inventory size")?;
    if size.len() != 2
        || size[0].as_f64() != Some(image.width as f64)
        || size[1].as_f64() != Some(image.height as f64)
    {
        return Err("inventory background size mismatch".into());
    }
    let equipment = geometry["equipment_rects"]
        .as_array()
        .ok_or("missing equipment")?;
    let removed = geometry["disabled_slot_rects"]
        .as_array()
        .ok_or("missing disabled slots")?;
    if equipment.len() != 7 || removed.len() != 3 {
        return Err("unsupported inventory geometry".into());
    }
    let weapon = array(&equipment[4])?;
    let torso = array(&equipment[6])?;
    let left = (weapon[0] + weapon[2] + 20.) as u32;
    let right = (torso[0] - 20.) as u32;
    let top = weapon[1].max(torso[1]) as u32;
    let bottom = (weapon[1] + weapon[3]).min(torso[1] + torso[3]) as u32;
    if right <= left || bottom <= top {
        return Err("no native stone region".into());
    }
    let stone = crop_image(image, [left, top, right - left, bottom - top])?;
    let mut out = image.clone();
    for value in removed {
        let [x, y, w, h] = array(value)?;
        let x = (x as i64 - 14).max(0) as u32;
        let y = (y as i64 - 14).max(0) as u32;
        let w = w as u32 + 28;
        let h = h as u32 + 28;
        if x.checked_add(w).is_none_or(|v| v > image.width)
            || y.checked_add(h).is_none_or(|v| v > image.height)
        {
            return Err("socket outside inventory".into());
        }
        // Bilinear sampling preserves the native texture while filling each socket.
        for dy in 0..h {
            for dx in 0..w {
                let sx = ((dx as f64 + 0.5) * stone.width as f64 / w as f64 - 0.5)
                    .clamp(0., (stone.width - 1) as f64);
                let sy = ((dy as f64 + 0.5) * stone.height as f64 / h as f64 - 0.5)
                    .clamp(0., (stone.height - 1) as f64);
                let x0 = sx.floor() as u32;
                let y0 = sy.floor() as u32;
                let x1 = (x0 + 1).min(stone.width - 1);
                let y1 = (y0 + 1).min(stone.height - 1);
                let fx = sx - x0 as f64;
                let fy = sy - y0 as f64;
                let dst = (((y + dy) * image.width + x + dx) * 4) as usize;
                for c in 0..4 {
                    let p = |xx: u32, yy: u32| {
                        stone.rgba[((yy * stone.width + xx) * 4) as usize + c] as f64
                    };
                    out.rgba[dst + c] = ((p(x0, y0) * (1. - fx) + p(x1, y0) * fx) * (1. - fy)
                        + (p(x0, y1) * (1. - fx) + p(x1, y1) * fx) * fy)
                        .round()
                        .clamp(0., 255.) as u8;
                }
            }
        }
    }
    Ok(out)
}

/// Build the HUD manifest and files consumed by the game, using owned data.
pub fn extract_hud(
    access: &mut impl crate::import_model::AssetAccess,
    build: &str,
) -> Result<std::collections::BTreeMap<String, Vec<u8>>, String> {
    use serde_json::json;
    let listing: std::collections::BTreeSet<_> = access
        .listing()?
        .into_iter()
        .map(|s| s.to_lowercase())
        .collect();
    let mut base = extract_base_hud(access)?;
    let fire_source = "data/hd/vfx/textures/pyro/t_fx_fire_plate_med_04.texture";
    if listing.contains(fire_source) {
        let image = crate::texture::decode(&access.read(fire_source)?, 0)?;
        base.files
            .insert("firebolt_plate.png".into(), encode_png(&image)?);
        base.images.insert("firebolt_plate".into(), json!({"file":"firebolt_plate.png", "width":image.width, "height":image.height, "source":fire_source}));
    }
    let mut icons = vec![
        ("button_background", "skill_icons", 0usize),
        ("skill_sword", "skill_icons", 26usize),
    ];
    let skill_table = "data/global/excel/skilldesc.txt";
    if listing.contains(skill_table) {
        let table = access.read(skill_table)?;
        let text = std::str::from_utf8(&table)
            .map_err(|e| e.to_string())?
            .trim_start_matches('\u{feff}');
        let mut lines = text.lines();
        if let Some(header) = lines.next() {
            let fields: Vec<_> = header.split('\t').collect();
            if let (Some(name_col), Some(icon_col)) = (
                fields.iter().position(|v| *v == "skilldesc"),
                fields.iter().position(|v| *v == "IconCel"),
            ) {
                for line in lines {
                    let row: Vec<_> = line.split('\t').collect();
                    let Some(name) = row.get(name_col) else {
                        continue;
                    };
                    let Some(index) = row
                        .get(icon_col)
                        .and_then(|v| v.trim().parse::<usize>().ok())
                    else {
                        continue;
                    };
                    let key = match *name {
                        "attack" => Some(("skill_attack", "skill_icons")),
                        "scroll of identify" => Some(("skill_identify", "skill_icons")),
                        "townportal" => Some(("skill_town_portal", "skill_icons")),
                        "fire bolt" => Some(("skill_firebolt", "skill_icons_sorceress")),
                        "fire ball" => Some(("skill_fireball", "skill_icons_sorceress")),
                        "lightning" => Some(("skill_lightning", "skill_icons_sorceress")),
                        "prayer" => Some(("skill_prayer", "skill_icons_paladin")),
                        _ => None,
                    };
                    if let Some((key, atlas)) = key {
                        icons.push((key, atlas, index));
                    }
                }
            }
        }
    }
    for (key, atlas, index) in icons {
        let Some(entry) = base.images.get(atlas) else {
            continue;
        };
        let Some(frame) = entry["frames"].as_array().and_then(|f| f.get(index)) else {
            continue;
        };
        let rect: [u32; 4] =
            serde_json::from_value(frame["rect"].clone()).map_err(|e| e.to_string())?;
        let filename = entry["file"].as_str().ok_or("missing atlas filename")?;
        let image = decode_png(base.files.get(filename).ok_or("missing atlas image")?)?;
        let tile = crop_image(&image, rect)?;
        let filename = format!("{key}.png");
        base.files.insert(filename.clone(), encode_png(&tile)?);
        base.images.insert(key.into(), json!({"file":filename,"width":tile.width,"height":tile.height,"source_frame":index,"source_atlas":atlas}));
    }
    let layout_path = "data/global/ui/layouts/hudpanelhd.json";
    let layout = read_layout(&access.read(layout_path)?)?;
    let composition = hud_geometry(&layout)?;
    let mut panels = serde_json::Map::new();
    let profile_path = "data/global/ui/layouts/_profilehd.json";
    if listing.contains(profile_path) {
        let profile = read_layout(&access.read(profile_path)?)?;
        for (kind, path) in [
            (
                "inventory",
                "data/global/ui/layouts/playerinventoryoriginallayouthd.json",
            ),
            ("shop", "data/global/ui/layouts/vendorpanellayouthd.json"),
        ] {
            if !listing.contains(path) {
                continue;
            }
            let native = read_layout(&access.read(path)?)?;
            let mut geometry = panel_geometry(&native, &profile, kind)?;
            if kind == "inventory" {
                if let Some(entry) = base.images.get("inventory_classic_background") {
                    let filename = entry["file"].as_str().ok_or("missing inventory filename")?;
                    let image =
                        decode_png(base.files.get(filename).ok_or("missing inventory png")?)?;
                    let adapted = inventory_background(&image, &geometry)?;
                    base.files
                        .insert("inventory_d1_background.png".into(), encode_png(&adapted)?);
                    base.images.insert("inventory_d1_background".into(),json!({"file":"inventory_d1_background.png","width":adapted.width,"height":adapted.height}));
                }
            }
            geometry["source"] = json!(path);
            geometry["profile_source"] = json!(profile_path);
            geometry["native_layout"] = native;
            geometry["requires_state_bridge"] = json!(true);
            panels.insert(kind.into(), geometry);
        }
    }
    let mut fonts = serde_json::Map::new();
    for (key, name) in [
        ("exocet", "exocetblizzardot-medium.otf"),
        ("formal", "formal436bt.ttf"),
    ] {
        let path = format!("data/hd/ui/fonts/{name}");
        if listing.contains(&path) {
            let data = access.read(&path)?;
            if ![b"OTTO".as_slice(), b"\0\x01\0\0", b"true", b"ttcf"]
                .iter()
                .any(|magic| data.starts_with(magic))
            {
                return Err(format!("invalid font {path}"));
            }
            base.files.insert(name.into(), data);
            fonts.insert(key.into(), json!({"file":name,"source":path}));
        }
    }
    let manifest = json!({"version":1,"build":build,"images":base.images,"fonts":fonts,"missing":base.missing,"layout_source":layout_path,"hud_layout":layout,"composition":composition,"panels":panels});
    let mut bytes = serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?;
    bytes.push(b'\n');
    base.files.insert("ui.resolved.json".into(), bytes);
    Ok(base.files)
}
