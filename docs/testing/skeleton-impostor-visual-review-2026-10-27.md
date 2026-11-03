# Skeleton impostor visual review — October 27, 2026

An independent, read-only review compared a private Windows-rendered
`skeleton_axe.glb` impostor pilot with independently captured ordinary Bevy GLB
scene renders of the same source. Both used eight 256-pixel cells per row,
eight orientation rows, an orthographic 3.75-unit extent, yaw 45°, pitch 30°,
model scale 0.73, and the same reported fixed lighting. The source GLB SHA-256
was `04bf85acce9744e4c283bd990640a33b71c1de83a6ba6cfad06ffac0a23883eb`.

The comparison covered all 64 matched sampled poses for each of walk, attack,
hit and death; the atlas-only neutral and skill sheets were also inspected.
For the walk loop, ordered samples around the join were inspected as a strip
(frames 5, 6, 7, 0, 1, 2). The atlas and source have closely matching pose,
silhouette, apparent scale, framing and lighting in the inspected samples. No
gross missing pose, weapon, or obvious image-space origin displacement was
found. Small differences in individual pixels are present; this is not a
pixel-identical claim. The axe disappears at the beginning of death in both
the atlas and the ordinary source render; source-channel inspection indicates
that the clip itself scales the weapon attachment down.

Alpha bounds were measured at a threshold of one on each sampled cell. No
rendered alpha reached a cell edge: the narrowest margin was 15 pixels on
skill. The first neutral cell's visible alpha occupied 62 × 85 pixels of its
256 × 256 cell. Thus the nominal cell size does not guarantee that much
character detail. The previous 4.5-unit bake occupied 51 × 71 pixels for the
same sample. These figures exclude game rendering, texture filtering and
display scaling.

This is an **uncertain runtime assessment**, not an acceptance of the in-game
effect. The stills have no visible ground plane, and cannot validate grounding,
floor contact or actual ground-pivot placement. No browser or game playback,
normal-speed motion, crowd lighting, draw ordering, runtime facing selection,
LOD switching, or hit/death event synchronization was observed. The eight
samples per clip cannot prove subframe smoothness or transition continuity;
neutral and skill did not have independent source-reference sheets. Skill
starts in a low pose and should be checked in its actual intended transition.
The PNGs are private local evidence and are not checked into the repository.
