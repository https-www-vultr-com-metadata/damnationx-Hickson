# macOS Survivors clustered-light validation — 2026-10-27

The user's terminal reported a Bevy/wgpu validation failure in an ordinary
Survivors run: the GPU light clustering z-slice list attempted to grow from
1,024 to 1,073,741,824 entries, requesting a 12,884,901,888-byte buffer on a
device with a 4,294,967,295-byte limit. Subsequent invalid bind-group and queue
submission errors ended the host. The underlying reason for the reported count
has not been established. A scratch run of the old binary did not reproduce
the intermittent error.

The macOS host now selects Bevy's CPU light-cluster assignment after Bevy PBR
installs device defaults. It does not disable point lights or shadow rendering;
other operating systems retain their existing default. A plugin-finish ordering
unit test checks that the GPU clustering option is cleared without altering the
other clustering limits. A macOS sample during the updated run showed Bevy's
`assign_objects_to_clusters` on the CPU.

An offscreen release host was run at 1280×720 with the user's installed D2R
models/environment and HD pack, with a scratch save/config, muted dummy audio,
`DAMNATIONX_HOST_GOD=1`, profiling and scripted draft choices. It completed
1:43 of active combat with 115 kills; profiler windows reported up to 32 active
point lights. The final image at `/tmp/dx-cluster-cpu-combat-20261027.png`
shows colored fire/lightning illumination and enemy shadows. The process exited
normally without the original validation failure. This is one run, not proof
that the underlying GPU issue is fixed or that all lighting scenes match.

A separate old-binary GPU-clustering control also completed without the error.
At 30–40 reported monsters, the median of two-second window p50/p95 values was
16.69/20.89 ms for CPU clustering (46 windows), versus 16.68/18.68 ms for GPU
clustering (5 windows). These runs had different spawns, equipment and timing,
may include brief draft overlays, and were often paced near 60 Hz. Their frame
percentiles do not isolate clustering cost or establish an FPS gain/regression.
Logs and thread sample are `/tmp/dx-cluster-cpu-active.log`,
`/tmp/dx-cluster-gpu-active.log`, and `/tmp/dx-cluster-cpu-active.sample`.

On October 29 a second offscreen scratch run used the existing release binary
containing this workaround, with D2R models, D2R environment, D2R UI, the HD
pack, muted audio, autoplay and scripted draft selections. The process exited
normally with no validation errors after the scripted 115-second interval.
Its final capture, `/tmp/dx-cluster-oct29-end.png`, shows 1:37 of Survivors
game time, 99 kills and colored lightning illumination. This validates the
existing binary, not a fresh build containing later crowd-renderer changes;
like the earlier run, it does not establish why the intermittent GPU counter
was invalid or prove that every possible lighting scene is unaffected.
