# Survivors D2R render profile (October 26, 2026)

The same scripted Warrior hunt was run offscreen with D2R imports at native output resolution, raster rendering, `--profile --rt-perf`, and the user's imported HD pack. Measurements below are two-second live-combat windows before the unattended Warrior died. GPU synchronization was enabled for these comparisons; `gpu_wait_ms` is the explicit render-graph wait, **not** total GPU frame time. The scene inventory counts visible mesh instances and triangles in CPU assets, not actual draw calls or resident VRAM.

| Configuration | Resolution | Live monsters | Frame p50 / p95 | Core tick p50 | Explicit GPU wait p50 |
| --- | ---: | ---: | ---: | ---: | ---: |
| All imported monster GLBs, 4× MSAA | 2560×1080 | 37–40 | 67–77 / 106–118 ms | 3.8–3.9 ms | 9–10 ms |
| Adaptive monster detail, 4× MSAA | 2560×1080 | 35–40 | 31–32 / 47–59 ms | 1.6–2.0 ms | 7–8 ms |
| Adaptive detail, SMAA | 2560×1080 | 33–40 | 20–22 / 23–25 ms | 1.6–2.0 ms | 6–7 ms |
| Adaptive detail, SMAA | 3440×1440 | 36–39 | 23 / 25 ms | 2.8–2.9 ms | 8–9 ms |

At 2560×1080, the original 39-monster scene had about 821 visible mesh instances, 2.17 million triangles and 1.14 GB of decoded image assets. The adaptive scene near 40 monsters had about 450–550 mesh instances and 0.9–1.2 million triangles. The DevilutionX simulation tick, snapshot and Flecs checkpoint each accounted for a small fraction of the original 67–77 ms frame, so more simulation threads would not address the dominant cost. The large gap between frame duration and explicit GPU wait indicates substantial host/render preparation and other unmeasured rendering costs; it does not prove the GPU was idle.

The adaptive rule applies only to D2R monster models in active Survivors hunts. Sparse fights retain every imported model. At 24 active visible monsters it begins reducing detail, reaches 12 nearest fully detailed actors by 32 monsters at resolutions of at least two million pixels, and restores full detail below 20. At most two actors switch presentation per update. Others use their native animated sprite art; non-D2R model providers, the hero, and other modes are unaffected. A 22-unit entry and 26-unit retention radius limits distant detail switches. At high resolution, Survivors also uses SMAA when the saved raster look requests 4× MSAA, reverting to that saved setting after the hunt. This removes the expensive multisample target without changing an explicit FXAA/SMAA/TAA choice.

Visual captures: `/tmp/damnationx-surv-2560-alive.png` and `/tmp/damnationx-surv-3440-alive.png` show the Warrior alive at 00:30 with nearby D2R models and farther animated sprites. The captures also show an empty black HUD rectangle and some top-text overlap that existed independently of this render change.

These figures are from one macOS machine and offscreen scripted runs. They demonstrate a large improvement at the tested densities, but do not guarantee 60 FPS at 3440×1440 or in every act. The baseline and final hunts also straddled separate Survivors hit-reaction changes, so the most direct render evidence is the visible-mesh and triangle reduction plus the same-binary MSAA-versus-SMAA A/B. The final frame-time numbers should be treated as measured examples, not a universal benchmark.

## Default GPU crowd (November 2, 2026)

Scenario: offscreen 2560×1440, raster, `--render-mode 3d --profile`, a Survivors bot Warrior (`--bot-survivors --bot-solo`, core built with `DAMNATION_S1_BRIDGE=ON`) standing still with `DAMNATIONX_HOST_GOD=1` and a script that claims each level-up gift, so the swarm closes around the hero. Same machine, same assets, baseline = the parent commit.

| Measurement | Baseline | GPU crowd default |
| --- | ---: | ---: |
| Real-time run (60 Hz offscreen pacing), crowd ≥ 40 live monsters: mean / p95 frame | 25.6 / 40.5 ms (39 fps), avg 66 monsters | 16.7 / 21.1 ms (60 fps, capped), avg 90 monsters |
| Fixed 60 Hz game clock, `DAMNATIONX_OFFSCREEN_UNCAPPED=1`, wall ms per frame, 0–19 monsters | 8.2 ms | 6.0 ms |
| same, 40–59 monsters | 11.2–13.4 ms | 8.9–9.6 ms |
| same, 60–99 monsters | 11.7 ms | 9.3–9.7 ms |
| same, 180–199 monsters | (not reached) | 13.3 ms |

In the baseline only the 12–20 nearest monsters were 3D; the rest were sprites. With the GPU crowd every D2R monster in the run is a shared-pose instanced model. Sampling (`sample`) showed the render thread, not the simulation, as the limit: per-view, per-bin instance-buffer uploads (each `queue.write_buffer` allocates a Metal staging buffer), command encoding and submission. The largest reductions came from fewer shadow views (the unlit dungeon sun no longer renders three cascades; a dense swarm keeps one local cube; crowd parts do not cast shadows), from no longer re-preparing per-actor character edge-light materials on every flicker or for hidden actors, and from keeping admitted GPU actors on the crowd path through transient view changes instead of churning between representations. The offscreen numbers vary by ±15 % between runs on this shared machine; the monster population is not deterministic between runs, so results are bucketed by live-monster count.
