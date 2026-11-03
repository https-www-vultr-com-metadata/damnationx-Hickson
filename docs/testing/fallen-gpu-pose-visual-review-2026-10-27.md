# Fallen GPU pose visual review — October 27, 2026

An independent reviewer examined four private paired captures of
`fallen_club.glb`: walk, attack, hit and death. Each 4096 × 2048 image contains
eight orientations and eight sampled phases, with an ordinary Bevy GLB scene
and the proposed GPU pose path side by side at each phase. The source SHA-256
was `0c8ac5a9ac763370425bcf894664e65a9c47b71af632841d28ed3a75052c40a3`;
no texture variant was applied.

Both paths were paused and explicitly positioned at matching phases. The native
scene sought to `duration × frame / denominator × 0.999`, with denominator 8
for walk and 7 for one-shot clips; the GPU path used the corresponding
normalized pose, sampled from 30 Hz baked matrices with a four-bit
interpolation fraction. Captures used an orthographic 3.75-unit extent per
256-pixel cell, yaw 45°, pitch 30°, focus y = 0.75, scale 0.73, and 45°
orientation increments. Both sides used the same reported ambient and
directional light, with shadows and MSAA off, on an RTX 3080 Ti using Vulkan.

The sampled poses, body and clothing silhouettes, visible weapon, apparent
scale and image-space position are close across the four sheets. No gross
missing pose, weapon or mesh was observed. At attack orientation row 4,
frame 2 (zero-based), the raised forearm and weapon angle differ slightly in
the enlarged pair. Other small edge and pixel differences are visible; no
claim of pixel identity or exact continuous animation is made. The visible
difference is an observation, not a verified diagnosis of its cause.

This assessment is limited to the sampled, paused **forward-rendered** poses
of one model. No normal-speed playback, in-game state transitions, event
timing, floor or pivot contact, culling, picking, shadow pass, prepass,
dynamic lighting, or automatic fallback was observed. Eight samples per
clip do not validate motion between samples or a seamless loop, and these
captures do not establish GPU draw-call counts or frame-rate improvement.
The private images are local evidence and are not checked into Git.
