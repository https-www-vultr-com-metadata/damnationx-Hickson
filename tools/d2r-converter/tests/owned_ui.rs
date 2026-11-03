//! Optional qualification against locally extracted owned data; no game assets are bundled.
use d2r_converter::ui::decode_sprite;
use std::path::PathBuf;

#[test]
fn decode_owned_hud_atlas() {
    let Some(root) = std::env::var_os("D2R_RAW_DIR") else {
        eprintln!("D2R_RAW_DIR not set; owned-data check skipped");
        return;
    };
    let path = PathBuf::from(root).join("data/hd/global/ui/panel/hud_02/belt_uparrow.sprite");
    let data = std::fs::read(&path).expect("owned sprite must be present");
    let sprite = decode_sprite(&data).expect("owned HUD sprite must decode");
    assert_eq!(sprite.image.rgba, data[40..]);
    assert!(!sprite.frames.is_empty());
    assert_eq!(
        sprite.frames.last().unwrap().rect[0] + sprite.frames.last().unwrap().rect[2],
        sprite.image.width
    );
}

#[test]
fn extract_owned_base_hud() {
    let (Some(game), Some(cache), Some(helper)) = (
        std::env::var_os("D2R_GAME_DIR"),
        std::env::var_os("D2R_CACHE_DIR"),
        std::env::var_os("D2R_HELPER"),
    ) else {
        eprintln!(
            "D2R_GAME_DIR, D2R_CACHE_DIR and D2R_HELPER not set; owned HUD extraction skipped"
        );
        return;
    };
    let mut storage = d2r_converter::storage::NativeStorage::new(game, cache, helper).unwrap();
    let result = d2r_converter::ui::extract_base_hud(&mut storage).unwrap();
    for key in [
        "panel_background",
        "health_fill",
        "mana_fill",
        "belt_background",
    ] {
        let name = result.images[key]["file"].as_str().unwrap();
        assert!(result.files[name].starts_with(b"\x89PNG\r\n\x1a\n"));
    }
}

#[test]
fn resolve_owned_panel_layouts() {
    let Some(root) = std::env::var_os("D2R_RAW_DIR") else {
        return;
    };
    let dir = PathBuf::from(root).join("data/global/ui/layouts");
    let read = |name: &str| {
        d2r_converter::ui::read_layout(&std::fs::read(dir.join(name)).unwrap()).unwrap()
    };
    let profile = read("_profilehd.json");
    for (name, kind) in [
        ("playerinventoryoriginallayouthd.json", "inventory"),
        ("vendorpanellayouthd.json", "shop"),
    ] {
        let panel = d2r_converter::ui::panel_geometry(&read(name), &profile, kind).unwrap();
        assert_eq!(panel["grid_count"][0].as_u64(), Some(10));
        if kind == "inventory" {
            assert_eq!(panel["equipment_rects"].as_array().unwrap().len(), 7);
        }
    }
}

#[test]
fn extract_owned_hud_manifest() {
    let (Some(game), Some(cache), Some(helper)) = (
        std::env::var_os("D2R_GAME_DIR"),
        std::env::var_os("D2R_CACHE_DIR"),
        std::env::var_os("D2R_HELPER"),
    ) else {
        return;
    };
    let mut storage = d2r_converter::storage::NativeStorage::new(game, cache, helper).unwrap();
    let files = d2r_converter::ui::extract_hud(&mut storage, "owned-test").unwrap();
    let manifest: serde_json::Value = serde_json::from_slice(&files["ui.resolved.json"]).unwrap();
    for key in [
        "panel_background",
        "health_fill",
        "mana_fill",
        "belt_background",
        "inventory_d1_background",
        "skill_attack",
        "skill_firebolt",
    ] {
        let filename = manifest["images"][key]["file"].as_str().unwrap();
        assert!(files[filename].starts_with(b"\x89PNG\r\n\x1a\n"));
    }
    assert_eq!(
        manifest["panels"]["inventory"]["equipment_rects"]
            .as_array()
            .unwrap()
            .len(),
        7
    );
}

#[test]
fn compare_owned_hud_inventory_to_previous_output() {
    let (Some(baseline), Some(candidate)) = (
        std::env::var_os("D2R_UI_BASELINE"),
        std::env::var_os("D2R_UI_CANDIDATE"),
    ) else {
        return;
    };
    let decode = |path: std::ffi::OsString| {
        let file = std::fs::File::open(path).unwrap();
        let mut reader = png::Decoder::new(file).read_info().unwrap();
        let mut bytes = vec![0; reader.output_buffer_size()];
        let info = reader.next_frame(&mut bytes).unwrap();
        bytes.truncate(info.buffer_size());
        (info.width, info.height, info.color_type, bytes)
    };
    let a = decode(baseline);
    let b = decode(candidate);
    assert_eq!((a.0, a.1, a.2), (b.0, b.1, b.2));
    let mean =
        a.3.iter()
            .zip(&b.3)
            .map(|(x, y)| u64::from(x.abs_diff(*y)))
            .sum::<u64>() as f64
            / a.3.len() as f64;
    eprintln!("Inventory image mean channel difference: {mean:.4}/255");
    assert!(mean < 2.0);
}
