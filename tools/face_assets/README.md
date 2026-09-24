# Character face assets

The picture Enco shows on her LCD. `firmware/main/face_assets.{c,h}` are **generated** — edit the
art in `source/` and re-run the builder, never the `.c` by hand:

```sh
python3 tools/face_assets/build_face_assets.py firmware/main
```

No third-party Python packages are needed (macOS `sips` does the image decoding; everything else is
implemented in `face_lib.py`). A full build takes a few seconds.

## What gets generated

All images are native RGB565 C arrays. LVGL's built-in decoder draws them in place from flash, so
they cost no RAM.

| Symbol | Size | Flash |
|---|---|---|
| `enco_face_base` | 240x320 | 153,600 B |
| `enco_face_eyes_half` / `_shut` | ~116x30 | ~7 KB each |
| `enco_face_mouth_small` / `_wide` | ~72x38 | ~5.5 KB each |
| `enco_face_bangs_*` (4 sway levels) | 112x55 | ~12 KB each |
| `enco_face_locks_l_*` / `locks_r_*` (4 each) | 80x116 | ~18.5 KB each |
| `enco_face_thumb` (camera HUD) | 82x86 | 14 KB |

Total ≈ 390 KB. The builder prints the exact sprite rectangles and total.

## Why RGB565 (and not 16 colours any more)

The first character was pixel art quantised to a 16-colour palette (I4) to save flash. On the
current, anti-aliased illustration that same quantisation turns every soft edge into stair-steps
and bands her skin, so the art is now kept at full panel colour depth. The app partition has more
than 1 MB free, which comfortably covers it.

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
