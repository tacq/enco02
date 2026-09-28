# Character face assets

The picture Enco shows on her LCD. `firmware/main/face_assets.h` and `face_assets_<id>.c` are
**generated** — edit the art in `source/` and re-run the builder, never the `.c` by hand:

```sh
python3 tools/face_assets/build_face_assets.py firmware/main        # all characters
python3 tools/face_assets/build_face_assets.py firmware/main fox    # one character
```

No third-party Python packages are needed (macOS `sips` does the image decoding; everything else is
implemented in `face_lib.py`). A full build takes ~15 s.

## Characters

| Id | Source | File | Flash | Extras |
|---|---|---|---|---|
| `fox` (default) | `source/fox_*.jpg` | `face_assets_fox.c` | ~330 KB | falling cherry petals |
| `k3` | `source/k3_*.jpg` | `face_assets_k3.c` | ~596 KB | hair breeze |

Both are compiled in and switchable at runtime: say "换角色", or the model calls
`self.screen.set_mode` with `fox` / `k3` / `char` (next). The choice is saved in NVS
(`display/character`). To leave one out of the build, add `-D ENCO_CHAR_K3=0` (or `ENCO_CHAR_FOX=0`)
to the build flags — its art and `.c` stay in the tree.

Each file exports one `enco_character_t` (`enco_char_<id>`): base, blink/mouth frames, 9
expressions, optional hair-breeze tables, camera thumbnail and optional petal sprites. The display
code only ever goes through that struct.

## What gets generated (per character)

All opaque images are native RGB565 C arrays, petals are RGB565A8. LVGL's built-in decoder draws
them in place from flash, so they cost no RAM.

| Part | Size |
|---|---|
| base portrait | 240x320, 153,600 B |
| eyes half / shut, mouth small / wide | small overlays, a few KB each |
| 9 expressions (eyes+mouth region) | small overlays |
| hair breeze (k3 only): bangs + locks, 4 sway levels | ~12-18 KB each |
| camera HUD thumbnail | 82x86, 14 KB |
| petals (fox only): 2 sizes x 8 tumble frames | 12x12 / 16x16 RGB565A8 |

The builder prints the exact sprite rectangles and totals.

## Falling petals

Petals painted into the fox's base art are erased (Laplace inpaint of `erase` boxes) so they don't
sit frozen behind the animated ones. The firmware draws 6 procedural petals with one LVGL object
and a draw callback: they drift down with a sway in the side lanes (`petal_lanes`) clear of her
face, tumble through the 8 frames, and respawn at the top forever. Only the petals' old/new areas
are invalidated each 80 ms tick.

## Why RGB565 (and not 16 colours any more)

The first character was pixel art quantised to a 16-colour palette (I4) to save flash. On the
current, anti-aliased illustration that same quantisation turns every soft edge into stair-steps
and bands her skin, so the art is now kept at full panel colour depth. With both characters the
firmware is 3,817,994 B of the 4,063,232 B (0x3E0000) app partition, ~240 KB free — a third character
would need one of them built out (`ENCO_CHAR_*=0`).

## Why overlay sprites

Animating by swapping full-screen images would cost many times the flash *and* repaint the whole
screen on every frame. Instead the base portrait is drawn once and small opaque sprites are
composited over the eyes, mouth and hair. Outside the changed feature each sprite is bit-identical
to the base, so there is no seam.

## How a variant becomes a sprite

Each variant in `source/` is a separate render of `k3_base.jpg` with one feature changed. Renders
drift slightly and carry their own JPEG noise, so the builder:

1. works at 2x (480x640);
2. searches ±6 px for the translation that best matches the base in a ring *around* the feature
   window, and applies it (it prints the offset and residual - expect `(+0,+0)` and a residual of
   a few units);
3. keeps only the pixels that genuinely changed inside the window, grows that mask and feathers its
   edge, and blends the variant into the base through it;
4. downsamples to 240x320 and converts to RGB565.

The sprite rectangle is then the bounding box of whatever differs from the base.

The hair-breeze frames are generated from the base by a fractional horizontal warp of the bangs and
side locks that stays outside the face silhouette and away from the metal accessories
(`ACCESSORY_BOXES`).

## Adding or changing a frame

1. Generate a new full portrait from `source/k3_base.jpg` with **only the one feature changed**:
   same framing, same head position.
2. Add it to `VARIANTS` in `build_face_assets.py` with a window that encloses the feature.
3. Re-run the builder. Check the printed offset and residual, then look at the panel-exact
   previews in `build/` (`frame_*.png`).

## Source art

`source/k3_base.jpg` is the approved "K3 Elegant Bob" portrait with the silver hair clip and the
temple device painted in. `source/k3_{eyes_half,eyes_shut,mouth_small,mouth_wide}.jpg` are its
animation variants. `source/reference_character_sheet.png` is the original character reference.
