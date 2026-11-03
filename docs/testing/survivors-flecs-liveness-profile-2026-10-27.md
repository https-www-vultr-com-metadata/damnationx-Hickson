# Flecs entity liveness bookkeeping (October 27, 2026)

An existing active-crowd CPU sample included the Flecs mirror's entity writes
and readback, in addition to substantial renderer work. Each write pass used a
new hash set to remember which game IDs were still present. The mirror now
marks entries in its existing game-ID map and clears the marks when pruning.
It still writes every authoritative component field, including after an
outside mutation, and removes missing entities as before. Restore rebuilds the
map with clear marks. Focused tests cover recording and non-recording modes,
outside component mutation, removal and ID reuse, resync, and duplicate IDs.

An optimized standalone Rust comparison on the same machine measured only the
two liveness bookkeeping algorithms, with a stable map and no Flecs operations,
rendering, or game simulation. From the repository root compile
`tools/bench_flecs_entity_liveness.rs` with
`rustc -O tools/bench_flecs_entity_liveness.rs -o /tmp/dx_entity_liveness_bench`,
then run `/tmp/dx_entity_liveness_bench`. Each of five rounds performed 10,000
passes for each algorithm.

| Entity IDs | Old total per round | New total per round |
| ---: | ---: | ---: |
| 64 | 7.6–7.9 ms | 2.7–2.8 ms |
| 256 | 30.4–31.3 ms | 10.8–11.2 ms |
| 1,024 | 122.7–130.0 ms | 44.6–47.3 ms |

At 256 IDs this is approximately two microseconds saved per bookkeeping pass.
This is a synthetic, stable-membership microbenchmark; it does not establish a
whole-frame improvement, an FPS gain, or a simulation bottleneck. A normal
in-game frame and churn-heavy workloads should be profiled separately.
