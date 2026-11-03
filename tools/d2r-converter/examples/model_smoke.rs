//! Owned-data smoke: model_smoke HELPER SKELETON MODEL OUTPUT.glb [ANIMATIONS]
use d2r_converter::{
    granny::Granny,
    model::{self, Part},
};
use std::{env, fs, path::Path, process::Command};
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = env::args_os().collect();
    if !(5..=6).contains(&args.len()) {
        return Err("usage: model_smoke HELPER SKELETON MODEL OUTPUT.glb [ANIMATIONS]".into());
    }
    let tmp = env::temp_dir().join(format!("d2r-rust-smoke-{}", std::process::id()));
    fs::create_dir_all(&tmp)?;
    let load =
        |input: &Path, output: &str| -> Result<serde_json::Value, Box<dyn std::error::Error>> {
            let dest = tmp.join(output);
            let status = Command::new(&args[1])
                .arg("-")
                .arg("gr2flat")
                .arg(input)
                .arg(&dest)
                .status()?;
            if !status.success() {
                return Err("gr2flat failed".into());
            }
            let bytes = fs::read(dest)?;
            Ok(Granny::new(&bytes)?.root()?)
        };
    let skeleton = load(Path::new(&args[2]), "skeleton.grfl")?;
    let model = load(Path::new(&args[3]), "model.grfl")?;
    let animation_bytes = if args.len() == 6 {
        let dest = tmp.join("animations.grfl");
        if !Command::new(&args[1])
            .arg("-")
            .arg("gr2flat")
            .arg(&args[5])
            .arg(&dest)
            .status()?
            .success()
        {
            return Err("gr2flat failed".into());
        }
        fs::read(dest)?
    } else {
        Vec::new()
    };
    let animations: Box<dyn Iterator<Item = Result<serde_json::Value, String>>> = if args.len() == 6
    {
        Box::new(Granny::new(&animation_bytes)?.animations()?)
    } else {
        Box::new(std::iter::empty())
    };
    let result = model::build_stream(
        "smoke",
        &skeleton,
        &[Part {
            part: "torso",
            variant: "",
            root: &model,
        }],
        animations,
        [1.; 3],
        |_, _| Ok(None),
    )?;
    fs::write(&args[4], result.glb)?;
    println!(
        "{} bones, {} meshes, {} clips",
        result.bones,
        result.meshes,
        result.clips.len()
    );
    fs::remove_dir_all(tmp)?;
    Ok(())
}
