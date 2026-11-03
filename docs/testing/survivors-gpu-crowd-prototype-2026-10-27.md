# GPU crowd prototype: Windows rendering probe

This document records the standalone experiment as it stood on October 27;
its measurements are not an in-game benchmark or a claim of production
readiness. Since then an explicitly opt-in gameplay handoff was added, with
the original representation retained as fallback. The current Windows
gameplay measurements, representative-probe experiment, and remaining
limitations are documented in
[the October 29 gameplay probe](survivors-gpu-gameplay-probe-2026-10-29.md).
Neither the synthetic prototype nor the gameplay path is enabled by default.

On 2026-10-27, a temporary Rust/Bevy 0.20.0 wrapper rendered the locally imported `fallen_club.glb` on Windows 11, an Intel i7-8700K, and an NVIDIA RTX 3080 Ti using Vulkan (driver 610.88). The 1600×900 window used `AutoNoVsync`, the same camera, light, 8×8 positions at 0.75-unit spacing, model, walk clip, and 64 independently staggered phases in both runs. Both runs used unoptimized development builds. Samples were collected from Bevy frame delta for 15 seconds after an eight-second warm-up, with a screenshot taken during the measured window. They include the wrapper's application/render frame work and do not isolate GPU time, actual draw count, or the game's simulation and HUD.

| Presentation | Samples | Frame time p50 | Frame time p95 |
| --- | ---: | ---: | ---: |
| Ordinary Bevy glTF scenes and animation players | 121 | 123.45 ms | 141.63 ms |
| Shared baked pose buffer and custom GPU vertex shader | 331 | 44.62 ms | 46.19 ms |

The screenshot pair from this particular run is `/tmp/dx-crowd-bench-native.png` and `/tmp/dx-crowd-bench-gpu.png`; model positions match but screenshot phases were not synchronized. They demonstrate rendering, not animation fidelity. The GPU run baked 16 clips into 99 matrix slots per frame, occupying 2,192,256 bytes, and logged 64 actors without a reported rendering validation error. The wrapper, source GLB, and screenshots were temporary local/private test data; the imported model is not included in the repository.

The macOS stress experiment initially exposed a Bevy skin bind-group mismatch. The revised prototype remaps skin vertex attributes to avoid that layout and rendered on Windows, but macOS Metal could not be retested: subsequent independent programs stalled in the driver's command-buffer allocation. A successful Windows render does not establish macOS compatibility.

The prototype uses bounded per-model and aggregate pose memory, conservative baked animation bounds, an optional test-only initial phase, and rejection of unsupported source/clip data. A `Ready` registry entry means CPU baking and numeric device-limit checks succeeded; it does **not** guarantee shader specialization, material preparation or device pipeline compilation succeeded. The later opt-in gameplay integration adds a separate render-readiness gate and fallback, but picking and hover/outline continuity, material/appearance transitions, off-screen and shadow correctness, and in-game performance still need validation. Baked 30 Hz matrix interpolation is approximate; matched-phase walk, attack, hit, death and transition comparisons across facings are required before asserting animation equivalence.

## Matched-phase visual review and readiness probe

An independent review compared native and GPU renderings of the same Fallen model (`fallen_club.glb`, SHA-256 `0c8ac5a9ac763370425bcf894664e65a9c47b71af632841d28ed3a75052c40a3`) at eight paused phases and eight facings for walk, attack, hit, and death. The temporary 4096×2048 comparison sheets alternate the native and GPU result for each phase: `/tmp/dx-fallen-gpu-compare/compare-walk.png`, `compare-attack1.png`, `compare-gethit.png`, and `compare-death.png`. Walk samples use duration × frame/8 × 0.999; the one-shot samples use duration × frame/7 × 0.999. The reviewer observed broadly matching poses and weapons and one small visible raised-arm/weapon-angle difference on an attack sample. This does not test animation transitions or prove equivalence for other models.

A separate opt-in, **advisory** render-world probe forces clipped-away GPU primitives through the active render queues while the normal scene stays visible. On the Windows test machine, the good shader progressed Pending → Ready after 13 primitives, one camera, its depth/normal/motion prepasses, and four directional shadow views had pipeline entries and prepared resources; activating the probe then displayed the model. With an intentionally invalid shader in the temporary test wrapper, the same probe progressed Pending → Failed and the wrapper retained the ordinary source model. This is one controlled failure case, not a production fallback guarantee.

After those Windows runs, the probe was tightened to verify expected primitives, material identity and bind groups, and the current render views. The later gameplay test exercised it on Windows; see the linked October 29 report for the outcome. It does not prove that a draw reached the framebuffer, and its result arrives from an earlier render frame. Actual draw/readback validation and broader in-game testing are still required before enabling it by default.
