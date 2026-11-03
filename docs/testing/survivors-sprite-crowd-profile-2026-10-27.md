# Survivors sprite crowd profile (October 27, 2026)

This investigation isolates the cost of fallback sprite material updates in the
Bevy host. It does not simulate 256 additional enemies, and it does not measure
combat, pathfinding, or real enemy density.

In a debug build, `DAMNATIONX_SPRITE_STRESS_COUNT=256` adds 256 non-pickable
visual-only sprite cards around the player. Each card references an actual
visible monster's animated sprite and goes through `apply_sprite_art`, the same
path as an ordinary fallback. The cards do not enter the native simulation or
entity index. The harness is disabled when the variable is absent and is not
compiled into release builds.

On an Apple M4 Max at 3440×1440, two idle offscreen runs used the same scripted
Warrior hunt, 256 synthetic cards, D2R imports, and unsynchronized `--profile`.
The baseline was the prior per-entity material updates; the second binary used
shared immutable frame materials and avoided redundant transform writes. Both
used the same debug stress harness. The experimental animation reader was
disabled in both runs and is not part of this change. The
screenshots at native tick 606/607 show the player alive. Monster behavior was
not deterministic, so these windows are comparable samples, not identical
frame-by-frame replay.

| Render workload | Real monsters in pre-capture windows | Median of window p50 | Median of window p95 |
| --- | --- | ---: | ---: |
| Prior per-entity materials + 256 synthetic cards | 29, 32, 35 | 18.17 ms | 19.97 ms |
| Shared frame materials + 256 synthetic cards | 29, 31, 33 | 17.35 ms | 19.68 ms |

The ~0.8 ms p50 difference is modest and noisy. It is not evidence of a large
FPS improvement, and it does not distinguish GPU time from render preparation.
`/tmp/damnationx-sprite-stress-base2.log` and
`/tmp/damnationx-sprite-stress-optimized.log` contain the full local runs;
`/tmp/damnationx-sprite-ab-base2.png` and
`/tmp/damnationx-sprite-ab-optimized.png` show the synthetic scenes.

The ignored Rust test `synthetic_sprite_crowd_cost` exercises 256 and 1024 ECS
actors cycling through 16 repeated frames. Three idle, CPU-only runs measured
median main-world update times of 50.4 versus 119.0 µs for 256 actors and 128.2
versus 396.4 µs for 1024 actors (per-entity updates versus shared lookup). The
shared path used 16 materials rather than 256 or 1024. This synthetic test does
not run the render world or GPU: material sharing raises its CPU lookup cost
while reducing distinct material assets and avoiding repeated asset mutation.
Run it with:

```sh
cargo test -p damnationx_host --lib synthetic_sprite_crowd_cost -- --ignored --nocapture
```

The render harness can be used with `--offscreen --window 3440x1440 --ui-size
3440x1440 --fps off --profile` and a normal scripted Survivors hunt.

## HD sprite sheet preparation

CPU samples during an ordinary D2R hunt also showed substantial work preparing
HD sprite sheets in `downsample` and `bleed_into_transparent`. The loader used
to request sheets for every actor reported by the core, even when a 3D model
was drawing that actor. It now requests only sheets needed by visible fallback
cards, shares an image when path and logical dimensions match, and limits
background preparation to two sheets at a time. Requests use a FIFO and stale
requests are removed; already-running work is retained until completion. A
classic frame remains available while an HD sheet waits or loads.

The pixel preparation routines were also rewritten. Exact reference tests cover
opaque, sparse, partially transparent, odd-sized, one-pixel-wide and edge cases.
On a synthetic sparse 1024×1024 sheet in a debug Rust build, median downsample
plus bleed time fell from 98.39 to 48.67 ms over three idle repetitions. On the
installed 3952×858 `monster_zombie-bluered_attack_e.png`, it fell from 302.68 to
224.91 ms; all output bytes matched. These are CPU preparation timings only;
PNG decoding and GPU upload are excluded. Run the optional installed-data check
with a local sheet path:

```sh
DAMNATIONX_HD_BENCH_SHEET=/path/to/sheet.png cargo test -p damnationx_host --lib installed_hd_sheet_preparation_cost -- --ignored --nocapture
```

A second pair of cold-process offscreen runs used a 3440×1440 output and the
default 1280×720 UI, with the same scripted Warrior hunt and D2R imports. OS
filesystem caches were warm and the encounters were not deterministic. In the
pre-capture windows with 24–34 real monsters, the median window p50 was 17.08 ms
before versus 17.16 ms after; median window p95 was 20.78 versus 18.53 ms. The
samples suggest a lower tail in this run, rather than a measured median FPS win.
The asset inventory near the 30-second screenshot was ~1.27 GB before and
~0.82 GB after; these are decoded image asset estimates, not measured VRAM.
Both captures show a living player.

The new loader reported no requested HD sheets while the sparse scene used only
D2R models; the first visible fallback sheet was ready by 24 monsters. Later, a
queue of five dropped to zero within the next two-second logging interval while
at most two sheets were loading and the ready count rose to 20. This verifies
progress for that encounter; it is not a guarantee for every pack or scene. The
local evidence is `/tmp/damnationx-hd-art-base.log`,
`/tmp/damnationx-hd-art-final.log`, and the corresponding `.png` captures.

For context, a macOS sample at 3440×1440 UI found 901 `CaptureUiLayer` leaf
samples over ten seconds; at the default 1280×720 UI it found 89 over eight
seconds. These are sampling counts, not timed function durations. The native UI
scan was therefore left unchanged; the default-size sample instead showed
`downsample` and `bleed_into_transparent` among the busiest active leaves.

A combined run of the final loader with 256 synthetic near-player cards and
29–35 live monsters produced window p50 values of 17.39, 17.06 and 17.33 ms
(p95 21.22, 19.31 and 18.75 ms) before the living-player capture at tick 607.
In the corresponding interval, 21 distinct HD sheets were demanded, ready
sheets rose from 64 to 73, and no sheets were queued. Later a single queued
request drained by the next logging interval while no more than two jobs ran.
The capture `/tmp/damnationx-hd-art-stress.png` and
`/tmp/damnationx-hd-art-stress.log` document this synthetic render workload.
