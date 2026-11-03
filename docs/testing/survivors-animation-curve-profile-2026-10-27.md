# Survivors D2R animation-curve experiment (October 27, 2026)

A discarded Rust asset-reader prototype removed a node property from every clip
only when its sampled float32 values matched the node's bind transform in all
clips. It retained reset tracks, clip duration, skeleton JSON, and the original
binary chunk. It operated only on monster GLBs loaded from the installed D2R
mapping and did not write to or alter any imported game asset. Tests checked
clip resets, duration, unknown metadata and original payload preservation.

For three locally installed models the prototype removed 1,792 of 4,704
channels from fallen_club, 1,380 of 3,600 from zombie, and 1,507 of 3,762 from
goatman. Standalone debug-build preparation took 32–44 ms per model. During an
actual run Bevy requested each sampled GLB twice concurrently, producing two
preparation operations per model; logged per-read times ranged from 27 to 63 ms.
This was additional cold-load work and could contribute to spawn hitches.

A single-machine, offscreen, scripted Warrior A/B used the same 3440×1440 debug
binary and graphics settings, with the reader disabled or enabled at startup.
The existing Survivors detail budget remained active. The following values are
medians of the individual two-second profile windows with 33–37 live monsters;
they are medians of per-window percentiles, not percentiles over all frames.

| Reader | Windows | Median window p50 | Median window p95 |
| --- | ---: | ---: | ---: |
| Disabled | 7 | 16.94 ms | 21.32 ms |
| Enabled | 2 | 17.11 ms | 20.30 ms |

The runs did not have identical enemy spawns and the enabled run supplied only
two windows at this density. The sampled frame time also often sat near 16.7 ms.
The measurements do not establish a useful steady-state improvement. Together
with the extra cold-load cost, they do not justify shipping the reader or raising
the detailed-model budget. It was removed after the experiment. Future work
should measure animation evaluation directly and avoid duplicate conversion,
then repeat controlled crowd, clip-transition, cold-load and visual comparisons.
