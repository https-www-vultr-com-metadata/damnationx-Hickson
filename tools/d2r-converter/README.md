# D2R Rust converter

This crate provides the native and browser-compatible D2R asset conversion
pipeline. It reads the player's installation through the existing CASC/Granny
helper and produces models, animations, environment art, HUD and portal files
in a user-selected cache. The Rust converter does not require a Python runtime.
The separate browser adapter lives in `tools/d2r-browser-rust`.

The command-line binary is also useful for qualification; run it with no
arguments for the supported commands. Run the synthetic tests with:

```sh
cargo test --manifest-path tools/d2r-converter/Cargo.toml --locked --all-targets
```
The tests in `tests/owned_ui.rs` can additionally exercise an owned local game
and its existing cache when the `D2R_GAME_DIR`, `D2R_CACHE_DIR`, `D2R_HELPER`,
and `D2R_RAW_DIR` environment variables are set. Optional examples compare GLB
and PNG output with previously converted owned assets. Game assets and outputs
must remain outside this repository.
