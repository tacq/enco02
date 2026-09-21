#!/usr/bin/env python3
"""Regenerate firmware/main/face_assets.{c,h} from the source portraits.

    python3 tools/face_assets/build_face_assets.py firmware/main

Emits one full-screen 240x320 I4 base image plus small I4 overlay sprites for the blink, mouth,
and hair-breeze frames.  Every image shares the base's 16-colour palette, which is what lets the
overlays composite onto the base without a seam.

Fifteen of those sixteen colours are quantised from the portrait; the last is reserved for the
emitter on the sci-fi accessories, which accessories.py cuts out of the approved concept render
and composites into every frame.  Because the same pixels land in all nine frames, the overlay
sprites never see them change and their rectangles are unaffected.

Intermediate artefacts (the quantised palette + index map, and composite previews) land in
tools/face_assets/build/ for align_check.py and zoom_face.py to use.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import accessories as A
import face_lib as F

SOURCE = os.path.join(HERE, "source")
BUILD = os.path.join(HERE, "build")

W, H = 240, 320

# source file, window to search for changes in (x, y, w, h), sprite group, C symbol
VARIANTS = [
    ("portrait_eyes_half.jpg",   (40, 100, 160, 60), "eyes",  "enco_face_eyes_half"),
    ("portrait_eyes_shut.jpg",   (40, 100, 160, 60), "eyes",  "enco_face_eyes_shut"),
    ("portrait_mouth_small.jpg", (70, 145, 100, 60), "mouth", "enco_face_mouth_small"),
    ("portrait_mouth_wide.jpg",  (70, 145, 100, 60), "mouth", "enco_face_mouth_wide"),
]

# Regions for the hair-breeze sprites: front bangs across the forehead, plus left and right side
# locks / bob tips.  Splitting into three small rectangles avoids covering the middle of her face
# (saving both flash and SPI redraw pixels) and keeps the hair sprites clear of the eyes and mouth.
HAIR_REGIONS = [
    ("bangs",   (70, 66, 96, 42)),
    ("locks_l", (10, 104, 74, 108)),
    ("locks_r", (156, 104, 74, 108)),
]

# Sway magnitudes, as a fraction of the full displacement computed in warp_hair().  Two extremes on
# their own made the breeze look like a flip-book: the hair jumped the whole 2-3px between frames.
# The intermediate pair halves each visible step, which is what actually reads as "smooth" - the
# firmware ramps -2 -> -1 -> 0 -> +1 -> +2 instead of toggling.  Each extra level costs ~10KB of
# flash (one crop per region), which is why there are four and not eight.
HAIR_LEVELS = [
    ("left",  -1.00),
    ("lhalf", -0.45),
    ("rhalf", +0.45),
    ("right", +1.00),
]

PAD = 3


def is_skin(rgb):
    r, g, b = rgb
    return r > 155 and (r - b) > 22


def warp_hair(src_rgb, w, h, direction):
    """Sway the front bangs and loose side locks in `direction` (-1.0 left .. +1.0 right) while
    leaving her face, eyes, eyebrows, chin, neck and shoulders untouched.
    """
    out = list(src_rgb)

    # 1. Front bangs hanging over her forehead (y = 66..108, x = 70..166).
    for y in range(66, 108):
        t = math.sin(math.pi * (y - 66) / (108 - 66))
        shift = direction * 2.3 * (t ** 1.2)
        for x in range(70, 166):
            ex = min((x - 70) / 14.0, (166 - x) / 14.0, 1.0)
            dx = int(round(shift * ex))
            if dx == 0:
                continue
            dst = src_rgb[y * w + x]
            sx = max(0, min(w - 1, x - dx))
            cand = src_rgb[y * w + sx]
            if not is_skin(dst) or not is_skin(cand):
                if y >= 102 and is_skin(dst):
                    continue
                out[y * w + x] = cand

    # 2. Side locks & bob tips from temple to collar (y = 104..212), strictly outside the eyes and
    #    mouth/cheek/neck silhouette.
    for y in range(104, 212):
        if y < 192:
            wy = ((y - 104) / (192 - 104)) ** 1.25
        else:
            wy = max(0.0, (212 - y) / (212 - 192))
        shift = direction * 3.2 * wy
        if abs(shift) < 0.4:
            continue

        if y < 144:
            inner_half = 58  # outside the eyes (which sit within x = 68..171)
        elif y < 165:
            inner_half = 48  # outside cheeks
        elif y < 192:
            inner_half = 42 - (y - 165) * 0.5
        else:
            inner_half = 28  # outside neck collar

        for x in range(10, w - 10):
            dist = abs(x - 120) - inner_half
            if dist <= 0:
                continue
            wx = min(1.0, dist / 10.0)
            dx = int(round(shift * wx))
            if dx == 0:
                continue
            sx = max(0, min(w - 1, x - dx))
            out[y * w + x] = src_rgb[y * w + sx]

    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(BUILD, exist_ok=True)

    base_rgb, w, h = F.load_rgb(os.path.join(SOURCE, "portrait_base.jpg"), W, H)

    # The palette has to be measured on the *clean* portrait. median_cut() repeatedly splits the
    # largest box in RGB space, and the emitter cyan sits a long way from every colour in this
    # picture, so if it can see the accessories it spends several of the sixteen slots on shades
    # of cyan and starves her face. Quantising the portrait into the remaining fifteen and
    # pinning the accent on the end costs a handful of portrait pixels a shift to a neighbouring
    # tone, which is not visible at this resolution. The accessory metal is grey, and greys the
    # portrait already owns, so it needs no slot at all.
    accents = A.accent_colors()
    palette = F.median_cut(base_rgb[::4], F.N_COLORS - len(accents)) + accents
    base = F.build_index_map(A.draw(list(base_rgb), w, h), palette)
    F.save_state(os.path.join(BUILD, "state.bin"), palette, base, w, h)

    quantised, group_mask = {}, {}
    for fname, win, group, name in VARIANTS:
        pixels, _, _ = F.load_rgb(os.path.join(SOURCE, fname), w, h)
        var = F.build_index_map(A.draw(pixels, w, h), palette)
        quantised[name] = var
        m = F.change_mask(base, var, w, h, win)
        assert any(m), f"{fname}: no change detected in {win}"
        prev = group_mask.get(group)
        group_mask[group] = m if prev is None else bytearray(a | b for a, b in zip(prev, m))

    rects = {}
    for group, m in group_mask.items():
        m, kept = F.keep_main_blobs(m, w, h)
        m = F.dilate(m, w, h, PAD)
        group_mask[group] = m
        x1, y1, x2, y2 = F.bbox(m, w, h)
        x1 &= ~1
        rects[group] = (x1, y1, ((x2 - x1 + 1) + 1) & ~1, y2 - y1 + 1)
        print(f"{group} sprite: x={x1} y={y1} {rects[group][2]}x{rects[group][3]} "
              f"from blobs {kept}")

    def sprite(name, group):
        var = quantised[name]
        px = [var[i] if group_mask[group][i] else base[i] for i in range(w * h)]
        x, y, cw, ch = rects[group]
        return F.crop(px, w, h, x, y, cw, ch), px

    # Warp first, accessories second. The warp slides pixels sideways by up to 3px, which would
    # smear the hard edges of the metal into the hair.
    hair_frames = {
        name: F.build_index_map(A.draw(warp_hair(base_rgb, w, h, amount), w, h), palette)
        for name, amount in HAIR_LEVELS
    }
    for region, (rx, ry, rw, rh) in HAIR_REGIONS:
        print(f"{region} sprite: x={rx} y={ry} {rw}x{rh}")

    total = 0
    previews = {}
    with open(os.path.join(outdir, "face_assets.c"), "w") as fh:
        fh.write("// Generated by tools/face_assets/build_face_assets.py - do not edit by hand.\n"
                 "//\n"
                 "// A full-screen 16-colour base portrait plus small overlay sprites for the blink,\n"
                 "// mouth and hair-breeze frames.  See lv_i4_decoder.c for why a custom LVGL decoder\n"
                 "// is needed to draw I4 without a full-screen RAM framebuffer.\n"
                 "\n#include \"face_assets.h\"\n")
        total += F.emit_c(fh, "enco_face_base", base, palette, w, h)
        for fname, win, group, name in VARIANTS:
            _, _, cw, ch = rects[group]
            cropped, previews[name] = sprite(name, group)
            total += F.emit_c(fh, name, cropped, palette, cw, ch)
        for region, (rx, ry, rw, rh) in HAIR_REGIONS:
            for level_name, _ in HAIR_LEVELS:
                sym = f"enco_face_{region}_{level_name}"
                cropped = F.crop(hair_frames[level_name], w, h, rx, ry, rw, rh)
                total += F.emit_c(fh, sym, cropped, palette, rw, rh)

    with open(os.path.join(outdir, "face_assets.h"), "w") as fh:
        ex, ey, _, _ = rects["eyes"]
        mx, my, _, _ = rects["mouth"]
        bx, by, _, _ = HAIR_REGIONS[0][1]
        llx, lly, _, _ = HAIR_REGIONS[1][1]
        lrx, lry, _, _ = HAIR_REGIONS[2][1]
        hair_externs = "".join(
            f"extern const lv_image_dsc_t enco_face_{region}_{level_name};\n"
            for region, _ in HAIR_REGIONS
            for level_name, _ in HAIR_LEVELS
        )
        fh.write("// Generated by tools/face_assets/build_face_assets.py - do not edit by hand.\n"
                 "#pragma once\n\n#include \"lvgl.h\"\n\n"
                 "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
                 f"#define ENCO_FACE_W {w}\n#define ENCO_FACE_H {h}\n\n"
                 "// Overlay sprites are positioned at these offsets inside the base image.\n"
                 f"#define ENCO_FACE_EYES_X {ex}\n#define ENCO_FACE_EYES_Y {ey}\n"
                 f"#define ENCO_FACE_MOUTH_X {mx}\n#define ENCO_FACE_MOUTH_Y {my}\n"
                 f"#define ENCO_FACE_BANGS_X {bx}\n#define ENCO_FACE_BANGS_Y {by}\n"
                 f"#define ENCO_FACE_LOCKS_L_X {llx}\n#define ENCO_FACE_LOCKS_L_Y {lly}\n"
                 f"#define ENCO_FACE_LOCKS_R_X {lrx}\n#define ENCO_FACE_LOCKS_R_Y {lry}\n\n"
                 "extern const lv_image_dsc_t enco_face_base;\n"
                 "extern const lv_image_dsc_t enco_face_eyes_half;\n"
                 "extern const lv_image_dsc_t enco_face_eyes_shut;\n"
                 "extern const lv_image_dsc_t enco_face_mouth_small;\n"
                 "extern const lv_image_dsc_t enco_face_mouth_wide;\n"
                 "\n// Hair sway frames, ordered from full-left to full-right.\n"
                 f"{hair_externs}\n"
                 "#ifdef __cplusplus\n}\n#endif\n")

    F.write_png_indexed(os.path.join(BUILD, "frame_base.png"), base, palette, w, h)
    for fname, win, group, name in VARIANTS:
        x, y, cw, ch = rects[group]
        comp = list(base)
        for yy in range(ch):
            for xx in range(cw):
                i = (y + yy) * w + (x + xx)
                comp[i] = previews[name][i]
        F.write_png_indexed(os.path.join(BUILD, f"frame_{name}.png"), comp, palette, w, h)
    for level_name, _ in HAIR_LEVELS:
        comp = list(base)
        for _, (rx, ry, rw, rh) in HAIR_REGIONS:
            for yy in range(rh):
                for xx in range(rw):
                    i = (ry + yy) * w + (rx + xx)
                    comp[i] = hair_frames[level_name][i]
        F.write_png_indexed(os.path.join(BUILD, f"frame_hair_{level_name}.png"), comp, palette, w, h)

    print(f"\nwrote {outdir}/face_assets.c + .h, {total:,} bytes of flash")
    print(f"previews in {BUILD}")


if __name__ == "__main__":
    main()
