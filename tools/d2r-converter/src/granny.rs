//! Safe reader for the self-describing GRFL representation emitted by d2rtool.
use serde_json::{json, Map, Number, Value};
use std::collections::{HashMap, HashSet};

const BASE: u64 = 0x1_0000_0000;
const TYPE_SIZE: u64 = 44;
const MAX_ITEMS: usize = 10_000_000;

#[derive(Clone, Debug)]
struct Member {
    kind: u32,
    name: String,
    reference: u64,
    count: i32,
}

pub struct Granny<'a> {
    blob: &'a [u8],
    root_type: u64,
    root_object: u64,
    types: HashMap<u64, Vec<Member>>,
    active: HashSet<(u64, u64)>,
}
/// Iterates animation objects one at a time, keeping peak memory bounded even
/// for the many hundreds of tracks in a hero's combined.animations file.
pub struct Animations<'a> {
    granny: Granny<'a>,
    pointers: Vec<(u64, u64)>,
    next: usize,
}
impl Iterator for Animations<'_> {
    type Item = Result<Value, String>;
    fn next(&mut self) -> Option<Self::Item> {
        let &(ty, ptr) = self.pointers.get(self.next)?;
        self.next += 1;
        Some(self.granny.object(ty, ptr, 0))
    }
}
impl<'a> Granny<'a> {
    pub fn new(data: &'a [u8]) -> Result<Self, String> {
        if data.get(..4) != Some(b"GRFL") || read_u32(data, 4)? != 1 {
            return Err("not a GRFL version 1 image".into());
        }
        let sections = read_u32(data, 8)? as usize;
        let len = read_u32(data, 12)? as usize;
        let off = 32usize
            .checked_add(sections.checked_mul(8).ok_or("section overflow")?)
            .ok_or("section overflow")?;
        let end = off.checked_add(len).ok_or("blob overflow")?;
        let blob = data.get(off..end).ok_or("truncated GRFL blob")?;
        Ok(Self {
            blob,
            root_type: read_u64(data, 16)?,
            root_object: read_u64(data, 24)?,
            types: HashMap::new(),
            active: HashSet::new(),
        })
    }
    fn range(&self, ptr: u64, len: usize) -> Result<&'a [u8], String> {
        let off = usize::try_from(ptr.checked_sub(BASE).ok_or("pointer below GRFL base")?)
            .map_err(|_| "pointer too large")?;
        let end = off.checked_add(len).ok_or("pointer overflow")?;
        self.blob
            .get(off..end)
            .ok_or_else(|| format!("out of bounds GRFL pointer {ptr:#x}"))
    }
    fn u32(&self, p: u64) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.range(p, 4)?.try_into().unwrap()))
    }
    fn u64(&self, p: u64) -> Result<u64, String> {
        Ok(u64::from_le_bytes(self.range(p, 8)?.try_into().unwrap()))
    }
    fn str(&self, p: u64) -> Result<Value, String> {
        if p == 0 {
            return Ok(Value::Null);
        }
        let off = usize::try_from(p.checked_sub(BASE).ok_or("invalid string pointer")?)
            .map_err(|_| "invalid string pointer")?;
        let s = self.blob.get(off..).ok_or("invalid string pointer")?;
        let end = s
            .iter()
            .take(4096)
            .position(|b| *b == 0)
            .ok_or("unterminated GRFL string")?;
        Ok(Value::String(
            String::from_utf8_lossy(&s[..end]).into_owned(),
        ))
    }
    fn members(&mut self, ptr: u64) -> Result<Vec<Member>, String> {
        if let Some(v) = self.types.get(&ptr) {
            return Ok(v.clone());
        }
        let mut out = Vec::new();
        let mut p = ptr;
        loop {
            if out.len() > 4096 {
                return Err("GRFL type has too many members".into());
            }
            let kind = self.u32(p)?;
            if kind == 0 {
                break;
            }
            let name = self
                .str(self.u64(p + 4)?)?
                .as_str()
                .unwrap_or("")
                .to_owned();
            out.push(Member {
                kind,
                name,
                reference: self.u64(p + 12)?,
                count: self.u32(p + 20)? as i32,
            });
            p = p.checked_add(TYPE_SIZE).ok_or("type pointer overflow")?;
        }
        self.types.insert(ptr, out.clone());
        Ok(out)
    }
    fn member_size(&mut self, m: &Member, depth: usize) -> Result<usize, String> {
        if depth > 128 {
            return Err("recursive GRFL type".into());
        }
        let unit = match m.kind {
            1 => self.struct_size(m.reference, depth + 1)?,
            2 | 8 | 22 => 8,
            3 | 4 => 12,
            5 => 16,
            7 => 20,
            9 => 68,
            10 | 19 | 20 => 4,
            11..=14 => 1,
            15..=18 | 21 => 2,
            n => return Err(format!("unknown Granny member kind {n}")),
        };
        unit.checked_mul(m.count.max(1) as usize)
            .ok_or_else(|| "member size overflow".into())
    }
    fn struct_size(&mut self, t: u64, depth: usize) -> Result<usize, String> {
        if depth > 128 {
            return Err("recursive GRFL type".into());
        }
        let mut size = 0usize;
        for m in self.members(t)? {
            size = size
                .checked_add(self.member_size(&m, depth + 1)?)
                .ok_or("struct size overflow")?;
        }
        Ok(size)
    }
    pub fn root(&mut self) -> Result<Value, String> {
        self.object(self.root_type, self.root_object, 0)
    }
    pub fn animations(mut self) -> Result<Animations<'a>, String> {
        let mut p = self.root_object;
        let mut pointers = None;
        for m in self.members(self.root_type)? {
            let size = self.member_size(&m, 0)?;
            if m.name == "Animations" {
                if m.kind != 3 && m.kind != 4 {
                    return Err("unsupported animations container".into());
                }
                let n = self.u32(p)? as i32;
                let ptr = self.u64(p + 4)?;
                let n = n.max(0) as usize;
                if n > MAX_ITEMS {
                    return Err("too many animations".into());
                }
                let step = if m.kind == 4 {
                    8
                } else {
                    self.struct_size(m.reference, 0)?
                };
                if n > 0 {
                    self.range(ptr, step.checked_mul(n).ok_or("animation array overflow")?)?;
                }
                let mut v = Vec::with_capacity(n);
                for i in 0..n {
                    let target = if m.kind == 4 {
                        self.u64(ptr + (i * step) as u64)?
                    } else {
                        ptr + (i * step) as u64
                    };
                    if target != 0 {
                        v.push((m.reference, target));
                    }
                }
                pointers = Some(v);
                break;
            }
            p = p.checked_add(size as u64).ok_or("root pointer overflow")?;
        }
        Ok(Animations {
            granny: self,
            pointers: pointers.ok_or("missing Animations field")?,
            next: 0,
        })
    }
    fn object(&mut self, ty: u64, ptr: u64, depth: usize) -> Result<Value, String> {
        if depth > 128 {
            return Err("GRFL object nesting too deep".into());
        }
        if !self.active.insert((ty, ptr)) {
            return Err("cyclic GRFL object reference".into());
        }
        let result = (|| {
            let mut map = Map::new();
            let mut p = ptr;
            for m in self.members(ty)? {
                let size = self.member_size(&m, 0)?;
                let value = self.member(&m, p, depth + 1)?;
                map.insert(m.name.clone(), value);
                p = p
                    .checked_add(size as u64)
                    .ok_or("object pointer overflow")?;
            }
            Ok(Value::Object(map))
        })();
        self.active.remove(&(ty, ptr));
        result
    }
    fn array(&mut self, ty: u64, ptr: u64, count: i32, depth: usize) -> Result<Value, String> {
        if count <= 0 || ptr == 0 {
            return Ok(json!([]));
        }
        let count = count as usize;
        if count > MAX_ITEMS {
            return Err("GRFL array too large".into());
        }
        let step = self.struct_size(ty, 0)?;
        let mut out = Vec::with_capacity(count);
        self.range(ptr, step.checked_mul(count).ok_or("array size overflow")?)?;
        for i in 0..count {
            out.push(self.object(ty, ptr + (step * i) as u64, depth + 1)?);
        }
        Ok(Value::Array(out))
    }
    fn member(&mut self, m: &Member, p: u64, depth: usize) -> Result<Value, String> {
        if depth > 128 {
            return Err("GRFL nesting too deep".into());
        }
        let n = m.count.max(1) as usize;
        if m.count > 1 {
            let mut single = m.clone();
            single.count = 0;
            let step = self.member_size(&single, 0)?;
            self.range(p, step.checked_mul(n).ok_or("member array overflow")?)?;
            let mut values = Vec::with_capacity(n);
            for i in 0..n {
                values.push(self.member(&single, p + (step * i) as u64, depth + 1)?);
            }
            return Ok(Value::Array(values));
        }
        let f = |v: f64| {
            Number::from_f64(v)
                .map(Value::Number)
                .ok_or_else(|| "non-finite GRFL value".to_string())
        };
        match m.kind {
            1 => self.object(m.reference, p, depth + 1),
            2 => {
                let q = self.u64(p)?;
                if q == 0 {
                    Ok(Value::Null)
                } else {
                    self.object(m.reference, q, depth + 1)
                }
            }
            3 => {
                let n = self.u32(p)? as i32;
                let q = self.u64(p + 4)?;
                self.array(m.reference, q, n, depth + 1)
            }
            4 => {
                let n = self.u32(p)? as i32;
                let q = self.u64(p + 4)?;
                if n <= 0 {
                    return Ok(json!([]));
                }
                let n = n as usize;
                if n > MAX_ITEMS {
                    return Err("GRFL reference array too large".into());
                }
                self.range(q, n.checked_mul(8).ok_or("array overflow")?)?;
                let mut a = Vec::with_capacity(n);
                for i in 0..n {
                    let v = self.u64(q + (i * 8) as u64)?;
                    a.push(if v == 0 {
                        Value::Null
                    } else {
                        self.object(m.reference, v, depth + 1)?
                    });
                }
                Ok(Value::Array(a))
            }
            5 => {
                let ty = self.u64(p)?;
                let q = self.u64(p + 8)?;
                if ty == 0 || q == 0 {
                    Ok(Value::Null)
                } else {
                    self.object(ty, q, depth + 1)
                }
            }
            7 => {
                let ty = self.u64(p)?;
                let n = self.u32(p + 8)? as i32;
                let q = self.u64(p + 12)?;
                if ty == 0 {
                    Ok(json!([]))
                } else {
                    self.array(ty, q, n, depth + 1)
                }
            }
            8 => self.str(self.u64(p)?),
            9 => {
                let flags = self.u32(p)?;
                let mut vals = Vec::with_capacity(16);
                for i in 0..16 {
                    vals.push(f(f32::from_bits(self.u32(p + 4 + i * 4)?) as f64)?);
                }
                Ok(
                    json!({"flags":flags,"position":&vals[0..3],"orientation":&vals[3..7],"scale_shear":[&vals[7..10],&vals[10..13],&vals[13..16]]}),
                )
            }
            10 => f(f32::from_bits(self.u32(p)?) as f64),
            11 | 13 => Ok(json!(self.range(p, 1)?[0] as i8)),
            12 | 14 => Ok(json!(self.range(p, 1)?[0])),
            15 | 17 => Ok(json!(i16::from_le_bytes(
                self.range(p, 2)?.try_into().unwrap()
            ))),
            16 | 18 => Ok(json!(u16::from_le_bytes(
                self.range(p, 2)?.try_into().unwrap()
            ))),
            19 => Ok(json!(self.u32(p)? as i32)),
            20 => Ok(json!(self.u32(p)?)),
            21 => {
                let bits = u16::from_le_bytes(self.range(p, 2)?.try_into().unwrap());
                f(half_to_f32(bits) as f64)
            }
            22 => Ok(Value::Null),
            n => Err(format!("unknown Granny member kind {n}")),
        }
    }
}
fn read_u32(data: &[u8], p: usize) -> Result<u32, String> {
    Ok(u32::from_le_bytes(
        data.get(p..p + 4)
            .ok_or("truncated GRFL header")?
            .try_into()
            .unwrap(),
    ))
}
fn read_u64(data: &[u8], p: usize) -> Result<u64, String> {
    Ok(u64::from_le_bytes(
        data.get(p..p + 8)
            .ok_or("truncated GRFL header")?
            .try_into()
            .unwrap(),
    ))
}
fn half_to_f32(v: u16) -> f32 {
    let sign = ((v as u32) & 0x8000) << 16;
    let exp = ((v as u32) >> 10) & 31;
    let frac = (v as u32) & 1023;
    if exp == 0 {
        let n = (frac as f32) * 2f32.powi(-24);
        if sign != 0 {
            -n
        } else {
            n
        }
    } else if exp == 31 {
        f32::from_bits(sign | 0x7f80_0000 | frac << 13)
    } else {
        f32::from_bits(sign | ((exp + 112) << 23) | frac << 13)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn put32(b: &mut [u8], o: usize, n: u32) {
        b[o..o + 4].copy_from_slice(&n.to_le_bytes());
    }
    fn put64(b: &mut [u8], o: usize, n: u64) {
        b[o..o + 8].copy_from_slice(&n.to_le_bytes());
    }
    fn fixture() -> Vec<u8> {
        let mut blob = vec![0; 512];
        // One inline array and one string in the root, followed by the end marker.
        put32(&mut blob, 0, 10);
        put64(&mut blob, 4, BASE + 200);
        put32(&mut blob, 20, 3);
        put32(&mut blob, 44, 8);
        put64(&mut blob, 48, BASE + 204);
        blob[200..204].copy_from_slice(b"xyz\0");
        blob[204..209].copy_from_slice(b"name\0");
        put32(&mut blob, 256, 1f32.to_bits());
        put32(&mut blob, 260, 2f32.to_bits());
        put32(&mut blob, 264, 3f32.to_bits());
        put64(&mut blob, 268, BASE + 280);
        blob[280..286].copy_from_slice(b"hello\0");
        let mut data = vec![0; 32];
        data[..4].copy_from_slice(b"GRFL");
        put32(&mut data, 4, 1);
        put32(&mut data, 12, blob.len() as u32);
        put64(&mut data, 16, BASE);
        put64(&mut data, 24, BASE + 256);
        data.extend(blob);
        data
    }
    #[test]
    fn reads_root_members_and_arrays() {
        assert_eq!(
            Granny::new(&fixture()).unwrap().root().unwrap(),
            json!({"xyz":[1.0,2.0,3.0],"name":"hello"})
        );
    }
    #[test]
    fn rejects_truncation_and_invalid_pointers() {
        let mut data = fixture();
        data.truncate(35);
        assert!(Granny::new(&data).is_err());
        let mut data = fixture();
        put64(&mut data, 24, 1);
        assert!(Granny::new(&data).unwrap().root().is_err());
    }
    #[test]
    fn decodes_half_precision() {
        assert_eq!(half_to_f32(0x3c00), 1.0);
        assert_eq!(half_to_f32(0xc000), -2.0);
        assert_eq!(half_to_f32(1), 2f32.powi(-24));
    }
}
