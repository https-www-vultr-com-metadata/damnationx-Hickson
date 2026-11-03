# Survivors GPU crowd gameplay probe — October 29, 2026

A scratch-save release run on macOS at 3440×1440 enabled the experimental GPU
crowd option with the locally imported D2R models and HD pack. The scripted
Survivors run reached dense combat and exited normally, but every reported
`active_actors` sample was zero. This is **not** a performance comparison:
the new presentation was never observed in gameplay. Its log is
`/tmp/dx-gpu-on-20261029.log` on the test machine.

A subsequent run added readiness counters. Pose buffers, materials, main and
prepass pipelines became available, but probes remained pending because most
shadow-view cache entries were absent. The render world retains shadow-view
entities that are not in Bevy's current shadow render phases. The readiness
probe now skips views absent from those phases and explicitly disabled lights;
unknown lights remain pending. Windows testing below exercised the revised gate.
`/tmp/dx-gpu-diagnostic-20261029.log` records the failing run.

The integration also recreated probes whenever a clip or view signature
changed, so a changing fight could prevent warmup. Probes now persist per
model and texture appearance; view changes advance the generation in place,
forcing fresh readiness without discarding the warmed entity. Review found a
separate deferred-spawn bug that removed GPU actors before their first frame;
the integration now retains the newly queued child until it is queryable.

On Windows with the locally imported D2R assets, a debug offscreen Survivors
run at 2560×1080 subsequently showed actual GPU replacements. The first
diagnostic run exposed frequent fallback during ordinary clip changes and
light-budget updates. The handoff now keeps prepared geometry across supported
clip changes: `present_actors` updates the new pose in the same update before
extraction. The view signature is sampled after the light budget. Readiness
also checks the actual camera view specialization, material bind groups, pose
buffers, and active shadow pipelines, while allowing the camera's global IBL
and retaining fallback for spatial light probes.

These changes do **not** make the feature ready by default. In separately
seeded, stationary god-mode debug runs with zero sprite promotions and the
same adaptive model budget, a no-screenshot synchronized profile at 38–45
monsters measured approximately 172–196 ms per frame and 57–60 ms post-submit
GPU wait with the crowd path, versus 78–84 ms and 28–33 ms respectively at
40–45 monsters with the normal path. GPU synchronization affects timings, the
runs are not deterministic replays, and the numbers are not release-build
benchmarks. The instrumented custom CPU systems each averaged under 1 ms in
the sampled window. Reducing the indexed probe geometry to a degenerate
triangle did not produce a measurable improvement and was removed; probes
continue to check the real geometry. The renderer is still opt-in.
`DAMNATIONX_GPU_CROWD_SPRITE_LIMIT` defaults to zero and permits an explicit
experimental limit up to 96; increasing it adds full 3D actors beyond the
normal model budget and can sharply worsen frame times. Set
`DAMNATIONX_GPU_CROWD_PROFILE=1` for sampled CPU timings of the experimental
systems; `--rt-perf` separately measures GPU waits and serializes CPU/GPU.

A discarded experiment precomputed the 16 fractions already encoded in each
pose tag. A Windows stress run with 48 phased skeleton models used 18,798,592
bytes for this palette versus 1,174,912 bytes for the compact form. At roughly
48–52 live monsters it measured 39–57 ms GPU wait and 133–170 ms frame mean,
versus 30–37 ms and 93–103 ms around 48–53 monsters for the compact palette.
These independent runs have different gameplay activity, so they cannot prove
the exact cost, but show no evidence of a win commensurate with 16× memory.
The preinterpolation experiment was removed.

Six full prewarm probes were then added to a diagnostic-only Windows run with
48 independently animated copies of the same skeleton. Around 45–48 live
game monsters, the instrumented run measured roughly 272–322 ms/frame and
85–93 ms synchronized GPU wait, compared with 93–103 ms and 30–37 ms around
48–53 monsters in an independently seeded 48-skeleton run without the extra
probes. Different live gameplay prevents a controlled causal estimate, but
the result makes prewarm geometry and view submission a strong lead.

The opt-in gameplay path now tests replacing a duplicate probe with an already
visible same-appearance GPU actor only after that actor independently passes
the readiness gate. If the representative is lost or becomes unready, the
probe is restored with a fresh generation. Current camera and shadow-view
visibility and pipeline readiness are required; absent visibility data is
Pending. In one Windows run with no-cull representatives, telemetry showed four to six probes parked and
11–22 active GPU actors, but frame times at 39–44 monsters remained around
194–200 ms, similar to the earlier GPU run. This is not evidence of an overall
performance win, and the asynchronous render-frame race remains unresolved.

A normally culled representative variant was tested with candidate-specific
view checks. It initially parked probes, but later repeatedly fell back to zero
active actors; at 42–44 monsters it measured roughly 646–911 ms/frame and
98–134 ms synchronized GPU wait in that run. The runs are independently
seeded, but this visible churn and regression did not justify the change, so
the normally culled variant was reverted.

Nsight Systems 2023.3.3 Vulkan traces were also attempted on Windows. The
initial raw trace failed conversion due to duplicate thread/process metadata;
disabling CPU context-switch tracing yielded reports, but conversion still
reported out-of-range identifiers and no GPU range markers were available.
In those incomplete traces, buffer-creation calls divided by recorded queue
submissions were approximately 237 with the GPU path and 134 with native
rendering. This is a lead for investigating transient uploads, not a measured
per-frame or per-pass attribution: the runs and workload differ, trace
instrumentation changes timing, and the captures reported conversion errors.

A main-world readiness result still comes from an earlier render frame. A view,
shadow phase, or device change can occur before the next observation; pipeline
cache readiness also does not establish successful drawing. Native actors must
remain the fallback, and the new path remains experimental. GPU materials do
not yet reproduce the native actor's custom physical/rim pass, and GPU picking
uses a coarse game-entity box rather than exact animated geometry. Live visual,
targeting and performance comparisons remain required before default use.
