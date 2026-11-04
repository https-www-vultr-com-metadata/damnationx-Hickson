//! Compact glTF 2.0 binary builder, used by desktop and browser conversion.
use serde_json::{json, Map, Value};

pub const FLOAT: u32 = 5126;
pub const UBYTE: u32 = 5121;
pub const USHORT: u32 = 5123;
pub const UINT: u32 = 5125;

pub struct Gltf {
    pub doc: Map<String, Value>,
    pub bin: Vec<u8>,
}
impl Gltf {
    pub fn new(generator: &str) -> Self {
        let mut doc = json!({"asset":{"version":"2.0","generator":generator},"scenes":[{"nodes":[]}],"scene":0}).as_object().unwrap().clone();
        for key in [
            "nodes",
            "meshes",
            "materials",
            "textures",
            "images",
            "samplers",
            "accessors",
            "bufferViews",
            "buffers",
            "skins",
            "animations",
        ] {
            doc.insert(key.into(), json!([]));
        }
        Self {
            doc,
            bin: Vec::new(),
        }
    }
    pub fn add(&mut self, key: &str, value: Value) -> Result<usize, String> {
        let array = self
            .doc
            .get_mut(key)
            .and_then(Value::as_array_mut)
            .ok_or_else(|| format!("missing glTF array {key}"))?;
        let index = array.len();
        array.push(value);
        Ok(index)
    }
    pub fn view(&mut self, bytes: &[u8], target: Option<u32>) -> Result<usize, String> {
        while self.bin.len() % 4 != 0 {
            self.bin.push(0);
        }
        let mut view = json!({"buffer":0,"byteOffset":self.bin.len(),"byteLength":bytes.len()});
        if let Some(target) = target {
            view["target"] = json!(target);
        }
        self.bin.extend_from_slice(bytes);
        self.add("bufferViews", view)
    }
    /// Bytes must be tightly packed, little-endian, and match the supplied shape.
    pub fn accessor(
        &mut self,
        bytes: &[u8],
        component: u32,
        count: usize,
        components: usize,
        normalized: bool,
        bounds: Option<(Vec<f64>, Vec<f64>)>,
        target: Option<u32>,
    ) -> Result<usize, String> {
        let component_size = match component {
            UBYTE => 1,
            USHORT => 2,
            UINT | FLOAT => 4,
            _ => return Err("unsupported glTF component type".into()),
        };
        let typ = match components {
            1 => "SCALAR",
            2 => "VEC2",
            3 => "VEC3",
            4 => "VEC4",
            16 => "MAT4",
            _ => return Err("unsupported glTF accessor shape".into()),
        };
        let expected = count
            .checked_mul(components)
            .and_then(|v| v.checked_mul(component_size))
            .ok_or("accessor length overflow")?;
        if bytes.len() != expected {
            return Err("accessor byte length does not match shape".into());
        }
        let view = self.view(bytes, target)?;
        let mut acc = json!({"bufferView":view,"componentType":component,"count":count,"type":typ});
        if normalized {
            acc["normalized"] = json!(true);
        }
        if let Some((min, max)) = bounds {
            if min.len() != components || max.len() != components {
                return Err("invalid accessor bounds".into());
            }
            acc["min"] = json!(min);
            acc["max"] = json!(max);
        }
        self.add("accessors", acc)
    }
    /// Embeds an encoded image (PNG, or KTX2 from [`crate::texture::gpu_texture`]) and adds a texture for it.
    pub fn image(&mut self, bytes: &[u8], name: &str) -> Result<usize, String> {
        let mime = if crate::texture::is_ktx2(bytes) {
            "image/ktx2"
        } else {
            "image/png"
        };
        let view = self.view(bytes, None)?;
        let source = self.add(
            "images",
            json!({"bufferView":view,"mimeType":mime,"name":name}),
        )?;
        if self.doc["samplers"].as_array().unwrap().is_empty() {
            self.add(
                "samplers",
                json!({"magFilter":9729,"minFilter":9987,"wrapS":10497,"wrapT":10497}),
            )?;
        }
        self.add("textures", json!({"sampler":0,"source":source}))
    }
    pub fn finish(mut self) -> Result<Vec<u8>, String> {
        self.doc
            .retain(|_, v| !v.as_array().is_some_and(Vec::is_empty));
        self.doc
            .insert("buffers".into(), json!([{"byteLength":self.bin.len()}]));
        let mut js = serde_json::to_vec(&self.doc).map_err(|e| e.to_string())?;
        while js.len() % 4 != 0 {
            js.push(b' ');
        }
        while self.bin.len() % 4 != 0 {
            self.bin.push(0);
        }
        let total = 12usize
            .checked_add(8)
            .and_then(|n| n.checked_add(js.len()))
            .and_then(|n| n.checked_add(8))
            .and_then(|n| n.checked_add(self.bin.len()))
            .ok_or("GLB too large")?;
        let total = u32::try_from(total).map_err(|_| "GLB exceeds 4 GiB")?;
        let mut out = Vec::with_capacity(total as usize);
        out.extend_from_slice(b"glTF");
        out.extend_from_slice(&2u32.to_le_bytes());
        out.extend_from_slice(&total.to_le_bytes());
        out.extend_from_slice(&(js.len() as u32).to_le_bytes());
        out.extend_from_slice(b"JSON");
        out.extend(js);
        out.extend_from_slice(&(self.bin.len() as u32).to_le_bytes());
        out.extend_from_slice(b"BIN\0");
        out.extend(self.bin);
        Ok(out)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn writes_aligned_valid_glb() {
        let mut g = Gltf::new("test");
        assert_eq!(
            g.accessor(&[1, 2, 3], UBYTE, 3, 1, false, None, None)
                .unwrap(),
            0
        );
        g.image(&[9, 8], "image").unwrap();
        let bytes = g.finish().unwrap();
        assert_eq!(&bytes[..4], b"glTF");
        assert_eq!(
            u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize,
            bytes.len()
        );
        let len = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
        let doc: Value = serde_json::from_slice(&bytes[20..20 + len]).unwrap();
        assert_eq!(doc["bufferViews"][1]["byteOffset"], 4);
        assert_eq!(doc["buffers"][0]["byteLength"], 6);
        assert_eq!(&bytes[20 + len + 8..], &[1, 2, 3, 0, 9, 8, 0, 0]);
    }
    #[test]
    fn rejects_bad_accessor_lengths() {
        assert!(Gltf::new("test")
            .accessor(&[0; 3], FLOAT, 1, 1, false, None, None)
            .is_err());
    }
}
