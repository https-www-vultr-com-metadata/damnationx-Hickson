use std::{collections::{HashMap, HashSet}, hint::black_box, time::Instant};

fn old(ids: &[u32], map: &mut HashMap<u32, u64>) {
    let mut seen = HashSet::with_capacity(ids.len());
    for &id in ids {
        seen.insert(id);
        black_box(map.entry(id).or_insert(id as u64));
    }
    map.retain(|id, _| seen.contains(id));
}
fn new(ids: &[u32], map: &mut HashMap<u32, (u64, bool)>) {
    for &id in ids {
        let entry = map.entry(id).or_insert((id as u64, false));
        entry.1 = true;
        black_box(entry.0);
    }
    map.retain(|_, value| { let found = value.1; value.1 = false; found });
}
fn main() {
    for size in [64u32, 256, 1024] {
        let ids: Vec<_> = (0..size).collect();
        let mut a = HashMap::new(); let mut b = HashMap::new();
        old(&ids, &mut a); new(&ids, &mut b);
        for round in 0..5 {
            let start = Instant::now();
            for _ in 0..10000 { old(black_box(&ids), &mut a); }
            let old_ms = start.elapsed().as_secs_f64() * 1000.0;
            let start = Instant::now();
            for _ in 0..10000 { new(black_box(&ids), &mut b); }
            let new_ms = start.elapsed().as_secs_f64() * 1000.0;
            println!("n={size} round={round} old_ms={old_ms:.3} new_ms={new_ms:.3}");
        }
    }
}
