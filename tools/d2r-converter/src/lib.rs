//! Portable D2R conversion primitives shared by future native and browser importers.
//! Conversion operates on the player's owned data through platform-specific access adapters.

pub mod environment;
pub mod granny;
pub mod import_model;
pub mod mapping;
#[cfg(not(target_arch = "wasm32"))]
pub mod storage;
pub mod texture;
pub mod ui;
pub mod unit;
pub mod variants;

pub mod gltf;

pub mod curves;

pub mod model;

pub mod static_model;

pub mod vfx;

pub mod attachments;

pub mod cathedral;

pub mod sockets;

pub mod height_terrain;
