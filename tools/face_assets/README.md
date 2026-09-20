# Character face assets

The picture Enco shows on her LCD. `firmware/main/face_assets.c` is **generated** — edit the art in
`source/` and re-run the builder, never the `.c` by hand:

```sh
python3 tools/face_assets/build_face_assets.py firmware/main
```

No third-party Python packages are needed (macOS `sips` does the image decoding; everything else is
implemented in `face_lib.py`).

## What gets generated

| Symbol | Size | Flash |
|---|---|---|
| `enco_face_base` | 240x320 | 38,464 B |
| `enco_face_eyes_half` / `enco_face_eyes_shut` | 120x59 | 3,604 B each |
| `enco_face_mouth_small` / `enco_face_mouth_wide` | 48x33 | 856 B each |

Total ≈ 47 KB.

## Why 16 colours

A 240x320 RGB565 bitmap is 153,600 bytes, which is most of the app partition's free space. The same
image as a 16-colour indexed (I4) bitmap is 38,464 bytes, and flat cel-shaded art quantises to 16
colours with almost no visible loss.

LVGL cannot draw I4 out of the box on this board: its built-in decoder expands the whole image to
ARGB8888 in RAM first (307,200 bytes, on a device with ~30 KB of free heap). `firmware/main/lv_i4_decoder.c`
registers a decoder that uses LVGL's streaming draw path to convert one scanline at a time instead.

## Why overlay sprites

Animating by swapping five full-screen images would cost five times the flash *and* repaint the
whole screen on every frame. Instead the base portrait is drawn once and small opaque sprites are
composited over the eyes and mouth. They are crops of the *same* quantised image sharing the *same*
palette, so there is no seam where they meet the base.

The sprite rectangles are derived automatically: each variant is diffed against the base and the
rectangle is the bounding box of the changed pixels. A 3x3 erosion first removes the scattered
single-pixel noise that JPEG compression and re-quantisation introduce, otherwise the box would
balloon to cover the entire search window.

## Adding or changing a frame

1. Generate/draw a new full portrait that is **pixel-aligned with `source/portrait_base.jpg`** —
   same framing, same head position, only the one feature changed.
2. Check the alignment before trusting it:
   ```sh
   python3 tools/face_assets/align_check.py tools/face_assets/build/state.bin \
       tools/face_assets/source/portrait_new.jpg tools/face_assets/build/new
   ```
   The reported offset should be `dx=0 dy=0` and the mismatch outside the region of interest should
   be a couple of percent. Inspect `new_diff.png`: the red pixels must be clustered on the feature
   you changed, not spread over the image.
3. Add it to `VARIANTS` in `build_face_assets.py`, re-run the builder, and check the composite
   previews written to `build/`.

`zoom_face.py` renders a magnified crop with a 10-pixel coordinate grid, which is how the search
windows in `VARIANTS` were chosen:

```sh
python3 tools/face_assets/zoom_face.py tools/face_assets/build/state.bin 60 112 50 36 12 /tmp/eye.png
```

## Source art

`source/reference_character_sheet.png` is the original reference. The portraits were generated from
it rather than cropped out of it: the face in the sheet is only about 90x120 pixels, and upscaling
that to fill a 240x320 screen looks mushy. The prompts asked for flat cel shading, crisp dark
outlines, few colour regions and a solid dark background, precisely because the result has to
survive being reduced to 16 colours.
