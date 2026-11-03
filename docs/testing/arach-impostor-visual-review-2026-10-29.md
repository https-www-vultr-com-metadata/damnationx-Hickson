# Arach impostor atlas visual review — October 29, 2026

An independent read-only review examined the private `arach.glb` level-0
variant's six 256-pixel-cell atlases: neutral, walk, two attacks, hit and death.
Each contains eight ordered samples in each of eight rendered orientations.
Every one of the 384 cells had nontransparent pixels. Measured alpha bounds
showed no sampled cell-edge contact; the nearest visible pixel was seven pixels
from an edge in the second orientation's third death sample. Other sheets had
at least 43 pixels of margin. This does not check poses between samples or prove
that filtering cannot bleed into an adjacent cell.

Neutral silhouettes occupied 101–127 × 64–95 pixels within each 256-pixel cell;
walk silhouettes occupied 106–127 × 72–98 pixels. An enlarged ordered walk
sequence covering samples 5, 6, 7, 0, 1 and 2 at two opposing views showed no
obvious silhouette jump at the loop join. Motion was subtle in these stills;
the sampled sheets do not establish normal-speed fluidity, ground contact,
direction mapping, combat transitions or correspondence with the source model.

A separate 32-frame neutral atlas used 128-pixel cells with the same framing.
All 256 cells contained pixels with at least 31 pixels of margin, and the
silhouettes measured 50–64 × 32–48 pixels. At equal displayed size, a
matched-phase still comparison showed coarser edges and less fine leg and
abdomen detail than the eight-frame 256-pixel atlas. The two atlases contain
the same number of pixels (about 16 MiB decoded RGBA); their compressed PNGs
were about 939 KiB and 828 KiB respectively. Thirty-two frames across the
four-second neutral clip provide eight sampled poses per second at source
speed versus two for eight frames. No playback was observed, so that sampling
difference alone does not establish that either result looks smooth in-game.

The source GLB, atlases and review strip remain private local test data. A
matched-time 3D reference, game-scale playback, lighting, transitions and
contact remain unverified; this review is not production acceptance.
