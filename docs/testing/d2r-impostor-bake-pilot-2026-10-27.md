# D2R impostor bake pilot — 2026-10-27

This is a test of the local Rust baker and catalog. A separate, explicitly
opt-in experimental runtime can use validated atlases for Survivors monsters
already using the crowd-budget sprite fallback. It is not enabled by default,
and the pilot alone does not demonstrate a frame-rate improvement. No D2R
model or derived atlas is committed or included in a release.

## Workload and results

A private copy of the user's imported `skeleton_axe.glb` was tested on Windows
with an RTX 3080 Ti (Vulkan, driver 610.88). The executable was a temporary
standalone wrapper around the same Rust baker, with Bevy's unrelated audio
plugin omitted. Eight frames were rendered at each of eight facing directions
for the mapped neutral, walk, attack1, gethit, death, and skill1 clips. The
baker uses a single orthographic camera and a common ground pivot across clips.

A first capture was blank; a bounded warm-up retry recovered on the next run. The
baker now retries the same frozen pose at 750 ms intervals, up to 12 times,
and rejects a persistently blank frame. The initial 3.0-unit camera extent
failed the four-pixel edge guard at skill1 facing 0, frame 0. A fresh batch
retried every clip at 3.75 units and passed; `--resume` then verified and
skipped the completed pack. An earlier 4.5-unit test was also successful but
left a neutral skeleton only 51×71 pixels inside its 256×256 cell. The tighter
3.75-unit result measured 62×85 pixels for the same neutral pose, and its
smallest transparent edge margin across the sampled frames was 15 pixels.
In-game quality and filtering at actual
camera scales still need review.

All six 256-pixel-cell atlases in the pilot occupied about 1.7 MiB as PNGs;
loading them all as uncompressed RGBA would occupy 96 MiB. Actual cache size
and memory residency depend on the import and selected clips. The user's full
mapping planned 41 distinct GLBs, 114 appearances, 663 clip/mode atlases,
and 42,432 frames: about 10.36 GiB decoded if all were resident at once.
Do not load the entire catalog into GPU memory at once.

The source GLB scales its right-hand weapon attachment to approximately 0.01
at the first death key, so the axe visibly disappears at the start of death;
neutral and attack retain normal attachment scale. The bake reproduced that
source data. Independent matched-phase 3D comparison is recorded in
`skeleton-impostor-visual-review-2026-10-27.md`; in-game validation remains
outstanding. Other unverified concerns include game lighting and shadow parity,
picking alignment, animation transitions, camera/facing alignment, atlas
residency, and performance across varied enemies. The experiment keeps the
fallback visible while a card is queued outside the camera frustum and checks
the exact mesh, image, material and specialized forward and requested prepass
pipelines for active views. View and device changes invalidate prior readiness,
and the render-world report expires after one second without a heartbeat. This
does not prove a draw succeeded, and handoffs have not been validated in game.
Set the diagnostics flag to log the number shown, queued and resident with
`DAMNATIONX_IMPOSTOR_DIAGNOSTICS=1`. Admission bounds
the selected RGBA8 set to four atlases and
64 MiB, not the peak memory during decoding, uploads, or eviction; the selection
is held for one second to reduce churn. On hover the experiment temporarily
restores the regular sprite rather than drawing a solid rectangular outline.
No broad cache bake or automatic integration
should be treated as production-ready on the basis of this one pilot.

On the Mac, the renderer later stalled while allocating a Metal command buffer,
after another GPU stress test also stalled there. This prevented final local
visual validation; a successful Windows pilot does not establish Mac stability.

On October 29, a release-build offscreen Mac Survivors run used a private
clone of the imported asset directory with only the skeleton pilot atlas. It
exited normally after 1:45 of game time without render validation errors.
Two-second diagnostics reported up to five skeleton cards shown, confirming
that this path can actually activate in the game. Some samples also showed
queued candidates or a normal fallback; they do not demonstrate seamless
animation transitions, picking, or an FPS gain. The final capture and log are
`/tmp/dx-impostor-live-end.png` and `/tmp/dx-impostor-live.log`.

A second Oct 29 run instrumented presentation changes. The initial exact-frame
pipeline gate recorded more than 160 card/fallback transitions in roughly ten
seconds with only one to three cards shown. This is excessive churn, not proof
of seamless animation. A subsequent pipeline-class cache was implemented to
reuse the same compiled specialization across an atlas's UV-only frame
materials while checking each prepared GPU material; it still needs a new
in-game run and timestamped unobstructed visual comparison. A later Mac run
with the class cache and actual render view-key invalidation exited normally,
but recorded 62 show transitions and 61 fallbacks with at most one shown at
the final diagnostic. Some screenshots were unobstructed, but a later group
was covered by a level-up overlay. These totals combine possible deaths,
highlighting, darkness, clip changes and readiness churn, so they do not
establish smooth handoff. A follow-up build orders readiness after the light
budget and logs the cause of each fallback. In one run, 64 fallbacks were
attributed to material or pipeline preparation and nine to removed billboards;
some repeatedly affected the same actor every 50–130 ms. A later build kept a
ready frame from the same atlas for up to 500 ms while the next prepared, then
restored the ordinary sprite on timeout. A second run recorded nine preparation
fallbacks, eight unselected-atlas fallbacks and seven removed billboards. Spawn
timing was not controlled, so these runs are not a controlled performance or
visual comparison. A further run admitted other clips from active appearances
within the four-atlas/64 MiB decoded budget; it recorded two preparation
fallbacks and seven removed billboards, with no atlas-loading or unselected
fallbacks logged. The prior admission-only run recorded nine loading, eight
preparation, ten removal and one unlit fallback. These runs had different
enemy timing. Three unpaused nine-frame combat sequences at 100 ms intervals
are in `/tmp/dx-impostor-prefetch-{35,40,45}-{1..9}.png`; independent review
could not identify individual atlas actors or establish handoff continuity
amid overlapping combatants. Isolated path-marked animation and performance
comparisons remain necessary before enabling the path by default.

An isolated, test-only offscreen scene exercised the actual impostor sync and
readiness systems with a single actor. The v3 capture exposed fallback at the
sampled clip and facing changes; after prewarming and per-image GPU readiness,
the v4 run captured 150 scripted states, including walking, facing changes and
a one-shot attack. The formerly failing sampled boundaries at frames 45 and
90 displayed the atlas. A forced highlight used the diagnostic fallback at
frames 120–130 and recovered at 131. The fixture reached four resident
atlases, 67,108,864 decoded image bytes and 256 frame/facing materials. Its
markers identify the main-world decision at capture request; screenshot
callbacks serialize these states and do not prove that every intervening GPU
frame was free of flicker. The private captures and receipts are under
`/tmp/dx-impostor-isolated-20261029-v4/`.

The current experimental prewarm creates at most 32 frame/facing materials
per update. Four resident 32-frame atlases can eventually create 1,024
material/bind-group variants. Their CPU and GPU costs are separate from the
64 MiB decoded-image selection budget; driver allocation, transient staging
peaks and frame-time impact have not yet been measured. A shared atlas
material with per-instance UV selection is a candidate for reducing that
overhead, but is not implemented or validated here.

A bounded Windows pilot on the first mapped variant (`arach.glb`) ran for
several minutes without producing a frame and was stopped when the ComfyUI
queue became active. A no-variant single-frame control of the same model
completed in about five seconds. The baker previously dropped each in-flight
variant texture handle whenever it was not yet ready. After retaining those
handles, the same variant completed all six clips, eight facings and eight
frames at 256 pixels in about 48 seconds, including three framing retries. A
resume verified the resulting pack. A separate one-second timeout terminated
the next test child, removed its staging directory and recorded failure.
The batch driver's configurable per-attempt timeout defaults to 900 seconds;
an uninterruptible driver operation can still delay process termination.

An independent inspection found all 384 variant cells nonempty with no
clipped edges. The idle loop lasts four seconds, however, so its eight frames
provide only two poses per second. A separate 32-frame, 128-pixel idle bake
completed. Both idle atlases decode to 16 MiB; the latter samples eight poses
per second but loses visible fine leg and abdomen detail at the same display
size. All 256 cells were nonempty with at least 31 pixels of margin. Cadence
and filtering still need in-game comparison against the original model.
Neither single-appearance test demonstrates that the full catalog is ready.

## Reproduce

After importing the user's own D2R models, inspect the workload, then run the
local baker when GPU resources are available. On macOS with the default import
location, using the packaged host executable or a locally built host:

```sh
D2R_DIR="$HOME/Library/Application Support/damnationx/d2r-models/glb"
damnationx_host --bake-d2r-impostors --all --plan --d2r-dir "$D2R_DIR"
damnationx_host --bake-d2r-impostors --all --d2r-dir "$D2R_DIR" --out "$D2R_DIR/impostors"
```

A batch stages each appearance privately and never replaces an existing pack.
If interrupted, repeat the second command with `--resume` and identical frame,
size, focus, and initial-extent settings. Matching packs are verified and
skipped. Clipped appearances retry all clips with a common extent multiplied
by 1.25, for at most seven attempts; errors remain errors after that bound.
The plan estimates decoded memory, not PNG size or expected bake duration.
An experimental `--adaptive-fps 8 --size 128` chooses each clip's frame count
from its duration, capped by `--frames` (32 by default in adaptive mode).
It renders a maximum-width grid and saves compact per-clip atlases, so the
peak bake cost is still that of the maximum grid. Adaptive settings are part
of the resume marker and cannot be mixed into a fixed-frame cache. A bounded
Windows GPU bake with `--frames 16` produced a 16-frame neutral loop and a
6-frame attack, with expected 2048×1024 and 768×1024 atlas dimensions at 128px
per cell. A matching adaptive cache was verified and a changed FPS setting
rejected during batch resume in a separate scratch directory. This confirms
generation, compaction and resume for two clips; in-game visual transitions
still need validation.
