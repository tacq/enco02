#!/usr/bin/env python3
"""Regenerate firmware/main/face_assets.{c,h} from the source portraits.

    python3 tools/face_assets/build_face_assets.py firmware/main

Emits one full-screen 240x320 RGB565 base image plus small RGB565 overlay sprites for the blink,
mouth and hair-breeze frames, and a pre-scaled thumbnail for the camera HUD.

Why RGB565 and not the old 16-colour I4
---------------------------------------
The previous art was quantised to a 16-entry palette with no dithering. Every soft edge - hair
strands, eyelashes, the jawline - collapsed into hard stair-steps and her skin posterised into
bands. RGB565 keeps the anti-aliasing of the source illustration. It costs ~4x the flash per pixel
(the whole set is ~0.4 MB of a partition with >1 MB free) and no RAM: LVGL's built-in decoder draws
a variable RGB565 image straight out of flash.

How the overlays line up
------------------------
Each variant (eyes half/shut, mouth small/wide) is a separate full render of the same portrait with
one feature changed, so it can drift by a pixel or two and its JPEG noise differs everywhere. Rather
than trusting it wholesale, the builder
  1. works at 2x (480x640) for precision,
  2. finds the translation that best matches the base *around* the feature and applies it,
  3. takes only the pixels that genuinely changed inside the feature window, grows that mask and
     feathers its edge, and blends the variant into the base through it,
  4. downsamples to 240x320.
Outside the feather the composite *is* the base, bit for bit, so the sprite rectangle is simply
the bounding box of the pixels that differ from the base after conversion to RGB565.

The hair clip and temple device are painted into every source render, so they need no compositing
of their own; the hair warp just keeps its hands off them (ACCESSORY_BOXES).

Previews land in tools/face_assets/build/.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import face_lib as F

SOURCE = os.path.join(HERE, "source")
BUILD = os.path.join(HERE, "build")

W, H = 240, 320
S = 2                       # working supersample factor
W2, H2 = W * S, H * S

BASE_SRC = "k3_base.jpg"

# The display's background colour (also the face container's bg in display.cpp). Pixels of the
# portrait's backdrop that are within JPEG noise of it are snapped to it exactly, so the backdrop
# neither speckles nor shows a seam against the strip under the image.
BG = (0x0c, 0x11, 0x21)
BG_SAMPLE = (10, 19, 37)    # what the render's backdrop actually measures
BG_TOL = 7

# source file, feature window (x, y, w, h) in 240x320 space, sprite group, C symbol
# The mouth window spans the whole lower face, jawline to collar: in the talking frames the jaw drops
# with the mouth, and clipping the window at the lips left a static chin under a moving mouth.
# The eyes window reaches up to the eyebrows, which carry most of an expression.
EYES_WIN = (50, 96, 140, 58)
MOUTH_WIN = (66, 158, 108, 60)

# Facial expressions. Each render yields an eyes sprite and a mouth sprite that reuse the blink and
# lip-sync overlays' rectangles, so showing one needs no extra LVGL objects. Order here is the
# order of the ENCO_EXPR_* table in the generated header.
EXPRESSIONS = ["happy", "sad", "wink", "pout", "surprised", "angry", "shy", "thinking"]

VARIANTS = [
    ("k3_eyes_half.jpg",   EYES_WIN,  "eyes",  "enco_face_eyes_half"),
    ("k3_eyes_shut.jpg",   EYES_WIN,  "eyes",  "enco_face_eyes_shut"),
    ("k3_mouth_small.jpg", MOUTH_WIN, "mouth", "enco_face_mouth_small"),
    ("k3_mouth_wide.jpg",  MOUTH_WIN, "mouth", "enco_face_mouth_wide"),
] + [
    entry
    for name in EXPRESSIONS
    for entry in (
        (f"k3_expr_{name}.jpg", EYES_WIN,  "eyes",  f"enco_face_expr_{name}_eyes"),
        (f"k3_expr_{name}.jpg", MOUTH_WIN, "mouth", f"enco_face_expr_{name}_mouth"),
    )
]

# Metal accessories baked into the art: left hair clip, right temple device. (x, y, w, h)
ACCESSORY_BOXES = [
    (14, 30, 62, 80),
    (168, 86, 44, 48),
]

# Hair-breeze sprite regions: front bangs, left and right side locks / bob tips. The bangs stop at
# the top of the eye sprite (patched in at build time) so the two never overlap.
HAIR_REGIONS = [
    ("bangs",   (64, 58, 112, 50)),
    ("locks_l", (8, 104, 80, 116)),
    ("locks_r", (152, 104, 80, 116)),
]

# Sway magnitudes, as a fraction of the full displacement computed in warp_hair().  Two extremes on
# their own made the breeze look like a flip-book: the hair jumped the whole 2-3px between frames.
# The intermediate pair halves each visible step, which is what actually reads as "smooth" - the
# firmware ramps -2 -> -1 -> 0 -> +1 -> +2 instead of toggling.
HAIR_LEVELS = [
    ("left",  -1.00),
    ("lhalf", -0.45),
    ("rhalf", +0.45),
    ("right", +1.00),
]

# ---------------------------------------------------------------- camera-view thumbnail
#
# The camera HUD shows her in an 84x88 panel beside the viewfinder. The shrink is baked here rather
# than done by LVGL at runtime so the HUD costs no transform buffers. THUMB_SRC matches the panel
# interior's aspect ratio (82/86) and frames her head plus a hint of collar.
THUMB_SRC = (14, 6, 212, 222)  # x, y, w, h in the 240x320 portrait
THUMB_W, THUMB_H = 82, 86      # 84x88 panel less its 1px border


def box_downscale(rgb, w, h, crop, out_w, out_h):
    """Area-average `crop` (x, y, w, h) of `rgb` down to out_w x out_h."""
    cx, cy, cw, ch = crop
    out = []
    for oy in range(out_h):
        sy0 = cy + oy * ch // out_h
        sy1 = max(sy0 + 1, cy + (oy + 1) * ch // out_h)
        for ox in range(out_w):
            sx0 = cx + ox * cw // out_w
            sx1 = max(sx0 + 1, cx + (ox + 1) * cw // out_w)
            r = g = b = n = 0
            for sy in range(sy0, sy1):
                if sy < 0 or sy >= h:
                    continue
                row = sy * w
                for sx in range(sx0, sx1):
                    if sx < 0 or sx >= w:
                        continue
                    pr, pg, pb = rgb[row + sx]
                    r += pr
                    g += pg
                    b += pb
                    n += 1
            out.append(((r + n // 2) // n, (g + n // 2) // n, (b + n // 2) // n) if n else (0, 0, 0))
    return out


def half(rgb2):
    return box_downscale(rgb2, W2, H2, (0, 0, W2, H2), W, H)


def finish(rgb):
    """Snap backdrop noise to BG, then quantise to exactly what the panel will show."""
    out = []
    for c in rgb:
        if max(abs(c[0] - BG_SAMPLE[0]), abs(c[1] - BG_SAMPLE[1]), abs(c[2] - BG_SAMPLE[2])) <= BG_TOL:
            c = BG
        out.append(F.from565(F.to565(c)))
    return out


def luma(c):
    return (c[0] * 3 + c[1] * 6 + c[2]) // 10


def align(base2, var2, win2, search=6, ring=28):
    """Translation (dx, dy) of var2 that best matches base2 in a ring around win2."""
    x, y, w, h = win2
    rx0, ry0 = max(0, x - ring), max(0, y - ring)
    rx1, ry1 = min(W2, x + w + ring), min(H2, y + h + ring)
    samples = [(sx, sy) for sy in range(ry0, ry1, 2) for sx in range(rx0, rx1, 2)
               if not (x <= sx < x + w and y <= sy < y + h)]
    bl = [luma(base2[sy * W2 + sx]) for sx, sy in samples]
    vl = [luma(c) for c in var2]
    best = None
    for dy in range(-search, search + 1):
        for dx in range(-search, search + 1):
            err = 0
            for (sx, sy), b in zip(samples, bl):
                vx, vy = sx + dx, sy + dy
                if 0 <= vx < W2 and 0 <= vy < H2:
                    err += abs(vl[vy * W2 + vx] - b)
                else:
                    err += 255
            if best is None or err < best[0]:
                best = (err, dx, dy)
    return best[1], best[2], best[0] / len(samples)


def shifted(var2, dx, dy):
    out = []
    for y in range(H2):
        vy = min(H2 - 1, max(0, y + dy))
        row = vy * W2
        for x in range(W2):
            out.append(var2[row + min(W2 - 1, max(0, x + dx))])
    return out


def feature_alpha(base2, var2, win2, thresh=48, grow=5, feather=5):
    """0..1 blend weights: where var2 genuinely differs from base2 inside win2, grown and feathered.

    The feather is kept inside the window, so the composite equals the base everywhere outside it.
    """
    x, y, w, h = win2
    diff = bytearray(W2 * H2)
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            i = yy * W2 + xx
            b, v = base2[i], var2[i]
            if abs(b[0] - v[0]) + abs(b[1] - v[1]) + abs(b[2] - v[2]) > thresh:
                diff[i] = 1
    # Drop JPEG speckle: keep pixels with most of their 3x3 neighbourhood also changed.
    clean = bytearray(W2 * H2)
    for yy in range(y + 1, y + h - 1):
        for xx in range(x + 1, x + w - 1):
            i = yy * W2 + xx
            if diff[i] and sum(diff[i + dy * W2 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) >= 6:
                clean[i] = 1
    clean, kept = F.keep_main_blobs(clean, W2, H2, frac=10)
    mask = F.dilate(clean, W2, H2, grow)
    # Feather: repeated 3x3 box blur of the mask, restricted to the window with a zero border.
    a = [0.0] * (W2 * H2)
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            a[yy * W2 + xx] = float(mask[yy * W2 + xx])
    for _ in range(feather):
        nxt = [0.0] * (W2 * H2)
        for yy in range(y + 1, y + h - 1):
            for xx in range(x + 1, x + w - 1):
                i = yy * W2 + xx
                s = 0.0
                for dy in (-W2, 0, W2):
                    s += a[i + dy - 1] + a[i + dy] + a[i + dy + 1]
                nxt[i] = s / 9.0
        a = nxt
    # Re-harden the core so the changed feature itself is taken 100% from the variant.
    for i in range(W2 * H2):
        if clean[i]:
            a[i] = 1.0
    return a, kept


def blend(base2, var2, a):
    out = list(base2)
    for i, t in enumerate(a):
        if t > 0.0:
            b, v = base2[i], var2[i]
            out[i] = tuple(int(round(b[k] + (v[k] - b[k]) * t)) for k in range(3))
    return out


# Per-sprite mask adjustments, in 1x coordinates.
#
# shy: the render's blush sits on her cheeks and carries on below the eye window. Copied as-is it
# stopped dead at the window's bottom edge - a saturated pink band straight across her face. The
# cheek skin is taken at reduced strength, and the whole change fades out before the edge.
ALPHA_TWEAKS = {
    "enco_face_expr_shy_eyes": {"skin_from_y": 132, "skin_gain": 0.7, "fade_y": (140, 152)},
}


def tweak_alpha(a, base2, win2, tw):
    x, y, w, h = win2
    skin_y = tw["skin_from_y"] * S
    f0, f1 = (v * S for v in tw["fade_y"])
    gain = tw["skin_gain"]
    for yy in range(y, y + h):
        fade = 1.0 if yy <= f0 else max(0.0, 1.0 - (yy - f0) / (f1 - f0))
        fade = fade * fade * (3.0 - 2.0 * fade)  # smoothstep: no visible start or end of the ramp
        for xx in range(x, x + w):
            i = yy * W2 + xx
            t = a[i]
            if t <= 0.0:
                continue
            if is_skin(base2[i]):
                if yy >= skin_y:
                    t *= gain
                t *= fade
            a[i] = t
    return a


def is_skin(rgb):
    r, g, b = rgb
    return r > 200 and (r - b) > 12


def in_accessory(x, y):
    return any(ax <= x < ax + aw and ay <= y < ay + ah for ax, ay, aw, ah in ACCESSORY_BOXES)


def keepout_weight(x, y, rects, ramp=8.0):
    """0 inside any of `rects`, rising to 1 at `ramp` px away from all of them."""
    wgt = 1.0
    for rx, ry, rw, rh in rects:
        dx = max(rx - x, 0, x - (rx + rw - 1))
        dy = max(ry - y, 0, y - (ry + rh - 1))
        wgt = min(wgt, min(1.0, math.hypot(dx, dy) / ramp))
    return wgt


def warp_hair(src, w, h, direction, bangs, locks_y, keepout=()):
    """Sway the front bangs and loose side locks in `direction` (-1.0 left .. +1.0 right) while
    leaving her face, eyes, eyebrows, chin, neck, shoulders and the metal accessories untouched.

    Shifts are fractional and linearly interpolated. Whole-pixel shifts were fine on the old pixel
    art, but on anti-aliased strands they step from row to row and comb the hair's outline.

    `keepout` are the rectangles of opaque sprites that sit above the hair in z-order (the mouth).
    The sway fades out approaching them, so their static copy of the hair meets the moving hair
    without a seam.
    """
    out = list(src)

    def pull(x, y, dx):
        dx *= keepout_weight(x, y, keepout)
        if abs(dx) < 0.05:
            return None
        fx = x - dx
        x0 = int(math.floor(fx))
        t = fx - x0
        x0c, x1c = max(0, min(w - 1, x0)), max(0, min(w - 1, x0 + 1))
        if in_accessory(x, y) or in_accessory(x0c, y) or in_accessory(x1c, y):
            return None
        a, b = src[y * w + x0c], src[y * w + x1c]
        return tuple(int(round(a[k] + (b[k] - a[k]) * t)) for k in range(3))

    # 1. Front bangs hanging over her forehead.
    bx, by, bw, bh = bangs
    for y in range(by, by + bh):
        t = math.sin(math.pi * (y - by) / bh)
        shift = direction * 2.3 * (t ** 1.2)
        for x in range(bx, bx + bw):
            ex = min((x - bx) / 14.0, (bx + bw - x) / 14.0, 1.0)
            dx = shift * ex
            if abs(dx) < 0.05:
                continue
            dst = src[y * w + x]
            cand = pull(x, y, dx)
            if cand is None:
                continue
            if is_skin(dst) and is_skin(cand):
                continue
            if y >= by + bh - 8 and is_skin(dst):
                continue  # never paint hair over the brow line
            out[y * w + x] = cand

    # 2. Side locks & bob tips from temple to collar, strictly outside the face silhouette.
    y0, y1 = locks_y
    for y in range(y0, y1):
        if y < 196:
            wy = ((y - y0) / (196 - y0)) ** 1.25
        else:
            wy = max(0.0, (y1 - y) / (y1 - 196))
        shift = direction * 3.2 * wy
        if abs(shift) < 0.05:
            continue

        # Half-width of her face (plus margin) about x=120, measured off the K3 render.
        if y < 150:
            inner_half = 58          # outside the eyes (x = 71..169)
        elif y < 175:
            inner_half = 52          # outside the cheeks
        elif y < 205:
            inner_half = 48 - (y - 175) * 0.55
        else:
            inner_half = 32          # outside the neck / collar

        for x in range(4, w - 4):
            dist = abs(x - 120) - inner_half
            if dist <= 0:
                continue
            wx = min(1.0, dist / 10.0)
            dx = shift * wx
            if abs(dx) < 0.05:
                continue
            cand = pull(x, y, dx)
            if cand is not None:
                out[y * w + x] = cand

    return out


def rect_of_changes(base, frames, pad=1):
    xs, ys = [], []
    for fr in frames:
        for i in range(W * H):
            if fr[i] != base[i]:
                xs.append(i % W)
                ys.append(i // W)
    assert xs, "variant produced no visible change"
    x1, y1 = max(0, min(xs) - pad) & ~1, max(0, min(ys) - pad)
    x2, y2 = min(W - 1, max(xs) + pad), min(H - 1, max(ys) + pad)
    return x1, y1, ((x2 - x1 + 1) + 1) & ~1, y2 - y1 + 1


def paste(base, frame, rect):
    x, y, w, h = rect
    out = list(base)
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            out[yy * W + xx] = frame[yy * W + xx]
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(BUILD, exist_ok=True)

    base2, _, _ = F.load_rgb(os.path.join(SOURCE, BASE_SRC), W2, H2)
    base_lin = half(base2)
    base = finish(base_lin)

    frames, groups, loaded = {}, {}, {}
    for fname, win, group, name in VARIANTS:
        if fname not in loaded:
            loaded[fname] = F.load_rgb(os.path.join(SOURCE, fname), W2, H2)[0]
        var2 = loaded[fname]
        win2 = tuple(v * S for v in win)
        dx, dy, err = align(base2, var2, win2)
        var2 = shifted(var2, dx, dy)
        a, kept = feature_alpha(base2, var2, win2)
        if name in ALPHA_TWEAKS:
            a = tweak_alpha(a, base2, win2, ALPHA_TWEAKS[name])
        frames[name] = finish(half(blend(base2, var2, a)))
        groups.setdefault(group, []).append(name)
        print(f"{name}: offset ({dx:+d},{dy:+d}) @2x, ring residual {err:.1f}, blobs {kept}")

    rects = {g: rect_of_changes(base, [frames[n] for n in names]) for g, names in groups.items()}
    for g, r in rects.items():
        print(f"{g} sprite: x={r[0]} y={r[1]} {r[2]}x{r[3]}")

    # Bangs end where the eye sprite starts, so a blink never cuts across swaying hair.
    bx, by, bw, _ = HAIR_REGIONS[0][1]
    bangs = (bx, by, bw, rects["eyes"][1] - by)
    hair_regions = [("bangs", bangs)] + HAIR_REGIONS[1:]
    locks_y = (HAIR_REGIONS[1][1][1], HAIR_REGIONS[1][1][1] + HAIR_REGIONS[1][1][3])
    keepout = [rects["eyes"], rects["mouth"]]
    hair_frames = {name: finish(warp_hair(base, W, H, amt, bangs, locks_y, keepout=keepout))
                   for name, amt in HAIR_LEVELS}
    for region, (rx, ry, rw, rh) in hair_regions:
        print(f"{region} sprite: x={rx} y={ry} {rw}x{rh}")

    thumb = finish(box_downscale(base_lin, W, H, THUMB_SRC, THUMB_W, THUMB_H))

    total = 0
    with open(os.path.join(outdir, "face_assets.c"), "w") as fh:
        fh.write("// Generated by tools/face_assets/build_face_assets.py - do not edit by hand.\n"
                 "//\n"
                 "// A full-screen RGB565 base portrait plus small opaque overlay sprites for the\n"
                 "// blink, mouth, expression and hair-breeze frames. All of it is drawn straight\n"
                 "// from flash.\n"
                 "\n#include \"face_assets.h\"\n")
        total += F.emit_c_rgb565(fh, "enco_face_base", base, W, H)
        for _, _, group, name in VARIANTS:
            x, y, cw, ch = rects[group]
            total += F.emit_c_rgb565(fh, name, F.crop(frames[name], W, H, x, y, cw, ch), cw, ch)
        for region, (rx, ry, rw, rh) in hair_regions:
            for level_name, _ in HAIR_LEVELS:
                sym = f"enco_face_{region}_{level_name}"
                total += F.emit_c_rgb565(fh, sym, F.crop(hair_frames[level_name], W, H, rx, ry, rw, rh),
                                         rw, rh)
        total += F.emit_c_rgb565(fh, "enco_face_thumb", thumb, THUMB_W, THUMB_H)
        fh.write("\nconst enco_face_expr_t enco_face_exprs[ENCO_FACE_EXPR_COUNT] = {\n")
        for name in EXPRESSIONS:
            fh.write(f"    {{\"{name}\", &enco_face_expr_{name}_eyes, &enco_face_expr_{name}_mouth}},\n")
        fh.write("};\n")

    with open(os.path.join(outdir, "face_assets.h"), "w") as fh:
        ex, ey, _, _ = rects["eyes"]
        mx, my, _, _ = rects["mouth"]
        (bx, by, _, _), (llx, lly, _, _), (lrx, lry, _, _) = (r for _, r in hair_regions)
        sprite_externs = "".join(f"extern const lv_image_dsc_t {name};\n" for _, _, _, name in VARIANTS)
        hair_externs = "".join(
            f"extern const lv_image_dsc_t enco_face_{region}_{level_name};\n"
            for region, _ in hair_regions
            for level_name, _ in HAIR_LEVELS
        )
        fh.write("// Generated by tools/face_assets/build_face_assets.py - do not edit by hand.\n"
                 "#pragma once\n\n#include \"lvgl.h\"\n\n"
                 "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
                 f"#define ENCO_FACE_W {W}\n#define ENCO_FACE_H {H}\n\n"
                 "// Overlay sprites are positioned at these offsets inside the base image.\n"
                 f"#define ENCO_FACE_EYES_X {ex}\n#define ENCO_FACE_EYES_Y {ey}\n"
                 f"#define ENCO_FACE_MOUTH_X {mx}\n#define ENCO_FACE_MOUTH_Y {my}\n"
                 f"#define ENCO_FACE_BANGS_X {bx}\n#define ENCO_FACE_BANGS_Y {by}\n"
                 f"#define ENCO_FACE_LOCKS_L_X {llx}\n#define ENCO_FACE_LOCKS_L_Y {lly}\n"
                 f"#define ENCO_FACE_LOCKS_R_X {lrx}\n#define ENCO_FACE_LOCKS_R_Y {lry}\n\n"
                 "// Pre-scaled bust for the camera HUD's side panel, baked at build time.\n"
                 f"#define ENCO_FACE_THUMB_W {THUMB_W}\n#define ENCO_FACE_THUMB_H {THUMB_H}\n\n"
                 "extern const lv_image_dsc_t enco_face_base;\n"
                 "extern const lv_image_dsc_t enco_face_thumb;\n\n"
                 "// Blink, lip-sync and expression sprites. Eye sprites sit at ENCO_FACE_EYES_*,\n"
                 "// mouth sprites at ENCO_FACE_MOUTH_*.\n"
                 f"{sprite_externs}"
                 "\n// Hair sway frames, ordered from full-left to full-right.\n"
                 f"{hair_externs}\n"
                 "// Named facial expressions: an eyes sprite and a mouth sprite each.\n"
                 "typedef struct {\n"
                 "  const char* name;\n"
                 "  const lv_image_dsc_t* eyes;\n"
                 "  const lv_image_dsc_t* mouth;\n"
                 "} enco_face_expr_t;\n\n"
                 f"#define ENCO_FACE_EXPR_COUNT {len(EXPRESSIONS)}\n"
                 "extern const enco_face_expr_t enco_face_exprs[ENCO_FACE_EXPR_COUNT];\n\n"
                 "#ifdef __cplusplus\n}\n#endif\n")

    # Previews: exactly what the panel shows (base + sprites pasted at their rectangles).
    F.write_png_rgb(os.path.join(BUILD, "frame_base.png"), base, W, H)
    F.write_png_rgb(os.path.join(BUILD, "frame_thumb.png"), thumb, THUMB_W, THUMB_H)
    for _, _, group, name in VARIANTS:
        F.write_png_rgb(os.path.join(BUILD, f"frame_{name}.png"), paste(base, frames[name], rects[group]), W, H)
    for name in EXPRESSIONS:
        comp = paste(base, frames[f"enco_face_expr_{name}_eyes"], rects["eyes"])
        comp = paste(comp, frames[f"enco_face_expr_{name}_mouth"], rects["mouth"])
        F.write_png_rgb(os.path.join(BUILD, f"frame_expr_{name}.png"), comp, W, H)
    for level_name, _ in HAIR_LEVELS:
        comp = base
        for _, r in hair_regions:
            comp = paste(comp, hair_frames[level_name], r)
        F.write_png_rgb(os.path.join(BUILD, f"frame_hair_{level_name}.png"), comp, W, H)

    print(f"\nwrote {outdir}/face_assets.c + .h, {total:,} bytes of flash")
    print(f"previews in {BUILD}")


if __name__ == "__main__":
    main()
