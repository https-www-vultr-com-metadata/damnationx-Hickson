# D2R crowd animation budget investigation — 2026-10-27

An experimental, presentation-only budget evaluated ordinary phase-driven D2R
monster walk poses at up to 20 Hz during crowded Survivors runs. Combat, hit,
death, special, idle, and unique-monster animation remained at normal cadence.
It is **not enabled or included in the runtime**: the measurement below did not
establish a repeatable benefit or establish stability.

The experiment used the same debug host binary for both runs, with 48 animated
`fallen_club.glb` stress models near the player and independent walk phases
and facings. The debug fixture allowed synthetic actors to count toward budget
activation, without adding them to simulation or picking. The run was offscreen
at 3440 × 1440 with a 1280 × 720 UI and the same scripted Survivors start.
The host was naturally paced near 60 frames per second early in both runs.
The control disabled the budget by environment flag; the other run enabled it.

Diagnostics confirmed the enabled run was exercising the budget: at one sample
60 walks were eligible and 57 were skipped that frame. Before the enabled run
stalled, frame-time windows at roughly 21–25 live game monsters reported median
18.6–19.3 ms enabled versus 19.4–19.8 ms disabled. Around 27–28 live monsters,
the enabled median was 20.5 ms versus 19.4 ms disabled. Those windows are noisy,
and differing live enemy states mean they cannot establish an improvement.
The disabled run completed; the enabled run stopped advancing at 27 monsters.

The macOS thread sample showed the main thread and render workers waiting in
Metal command-buffer allocation (`MTLCommandQueue.commandBuffer`, via wgpu
encoder finishing and a dispatch semaphore). It does not identify a cause or
prove that the animation experiment caused the stall. The benchmark process
remained in uninterruptible sleep after SIGTERM and remained listed in an exit
state after SIGKILL. Further GPU repetitions were stopped. On this machine the
raw sample and run logs are `/tmp/dx-walk-budget-hang.sample`,
`/tmp/dx-walk-budget-off2.log`, and `/tmp/dx-walk-budget-on2.log`.

Bevy's stock skin batching on storage-buffer-capable hardware can batch compatible
instances sharing mesh and material, with each instance retaining its own skin
index. This is capability evidence, not a measured draw-call count for this
scene. Stock skin matrices are world-space, so simply sharing joint entities
across actors would place them at the same world location. A future shared-pose
implementation needs its own transform handling and measured GPU validation.
