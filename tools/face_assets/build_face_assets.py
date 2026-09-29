#!/usr/bin/env python3
"""Regenerate firmware/main/face_assets.h and face_assets_<id>.c from the source portraits.

    python3 tools/face_assets/build_face_assets.py firmware/main          # every character
    python3 tools/face_assets/build_face_assets.py firmware/main fox      # just one (header too)

Each character in CHARACTERS (k3, fox) gets its own face_assets_<id>.c exporting a single
enco_character_t (enco_char_<id>); face_assets.h declares them behind ENCO_CHAR_<ID> flags (default
1, -D ENCO_CHAR_<ID>=0 leaves one out). Per character: one full-screen 240x320 RGB565 base image,
small RGB565 overlay sprites for the blink, mouth, expression and (optional) hair-breeze frames, a
pre-scaled camera-HUD thumbnail and (optional) RGB565A8 falling-petal sprites. Petals painted into
the base are inpainted out first (erase boxes) so only the animated ones remain.

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

# The display's background colour (also the face container's bg in display.cpp). Pixels of the
# portrait's backdrop that are within JPEG noise of it are snapped to it exactly, so the backdrop
# neither speckles nor shows a seam against the strip under the image.
BG = (0x0c, 0x11, 0x21)
BG_SAMPLE = (10, 19, 37)    # what the render's backdrop actually measures
BG_TOL = 7

# Facial expressions. Each render yields an eyes sprite and a mouth sprite that reuse the blink and
# lip-sync overlays' rectangles, so showing one needs no extra LVGL objects. Order here is the
# order of the expression table in the generated file, and is the same for every character so
# display.cpp can look them up by name.
EXPRESSIONS = ["happy", "sad", "wink", "pout", "surprised", "angry", "shy", "thinking"]

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

# ---------------------------------------------------------------- characters
#
# Every character is a base portrait plus the same set of variants: two blink frames, two lip-sync
# frames and the eight EXPRESSIONS. Each gets its own generated firmware/main/face_assets_<id>.c,
# which exports a single enco_character_t (enco_char_<id>); all sprites inside it are static.
#
#   eyes_win / mouth_win - feature windows (x, y, w, h) in 240x320 space.
#       The mouth window spans the whole lower face, jawline to collar: in the talking frames the
#       jaw drops with the mouth, and clipping the window at the lips left a static chin under a
#       moving mouth. The eyes window reaches up to the eyebrows, which carry most of an expression.
#   bg_snap - snap backdrop noise to BG (only for a flat backdrop that is meant to be BG).
#   accessories - metal accessories baked into the art, kept out of the hair warp. (x, y, w, h)
#   hair - hair-breeze sprite regions, or None for no breeze. The bangs stop at the top of the eye
#       sprite (patched in at build time) so the two never overlap.
#   alpha_tweaks - per-sprite mask adjustments, in 1x coordinates (see tweak_alpha()).
#   thumb_src - the camera HUD shows her in an 84x88 panel beside the viewfinder. The shrink is
#       baked here rather than done by LVGL at runtime so the HUD costs no transform buffers.
#       thumb_src matches the panel interior's aspect ratio (82/86) and frames her head plus a hint
#       of collar.
#   erase - painted-in props to remove from the base before anything else, as ellipses inscribed in
#       (x, y, w, h) boxes. They are filled from their surroundings (see inpaint()).
#   petals - generate animated falling-petal sprites for this character.
#   petal_lanes - (x0, x1): petals fall left of x0 and right of x1, i.e. clear of her face.
#   hair_flow - optional runtime wind-in-the-hair bands (see the fox entry), or absent for none.
#   ears - optional runtime twitching-ear rigs with their ornaments (see the fox entry).
CHARACTERS = {
    "k3": {
        "title": "K3 Elegant Bob (silver hair, white armour)",
        "prefix": "k3",
        "eyes_win": (50, 96, 140, 58),
        "mouth_win": (66, 158, 108, 60),
        "bg_snap": True,
        "bg_color": 0x0c1121,
        "accessories": [
            (14, 30, 62, 80),     # left hair clip
            (168, 86, 44, 48),    # right temple device
        ],
        "hair": [
            ("bangs",   (64, 58, 112, 50)),
            ("locks_l", (8, 104, 80, 116)),
            ("locks_r", (152, 104, 80, 116)),
        ],
        # shy: the render's blush sits on her cheeks and carries on below the eye window. Copied
        # as-is it stopped dead at the window's bottom edge - a saturated pink band straight across
        # her face. The cheek skin is taken at reduced strength, and the whole change fades out
        # before the edge.
        "alpha_tweaks": {
            "expr_shy_eyes": {"skin_from_y": 132, "skin_gain": 0.7, "fade_y": (140, 152)},
        },
        "thumb_src": (14, 6, 212, 222),
        "erase": [],
        "petals": False,
        "petal_lanes": (0, 0),
    },
    "fox": {
        "title": "Fox (pink fox-girl, crimson dress)",
        "prefix": "fox",
        # Measured off a union-of-differences map of all twelve renders: eyes + brows sit in
        # x 83..160, y 100..132; lips, chin and the jaw line in x 88..157, y 150..186.
        "eyes_win": (76, 94, 92, 46),
        "mouth_win": (84, 144, 76, 48),
        # Her backdrop is a warm bokeh, not the flat navy - nothing to snap. The container fill is
        # the backdrop's darkest tone, for the few rows the portrait never covers.
        "bg_snap": False,
        "bg_color": 0x1c1719,
        "accessories": [],
        "hair": None,
        # shy: same issue as K3 - the blush runs past the eye window. Her render paints it as a
        # saturated stripe over the nose, so it is taken weaker and faded over a longer run than
        # K3's; a 9px fade still left a hard pink band.
        "alpha_tweaks": {
            "expr_shy_eyes": {"skin_from_y": 118, "skin_gain": 0.55, "fade_y": (120, 139)},
        },
        "thumb_src": (14, 0, 212, 222),
        # The three petals painted into the backdrop. Left there, they would hang motionless among
        # the animated ones. (A fourth sits on the hair at the right edge and is left alone - the
        # fill would smear the strands.)
        "erase": [
            (8, 28, 27, 29),
            (3, 157, 24, 26),
            (199, 67, 19, 24),
        ],
        "petals": True,
        "petal_lanes": (70, 172),
        # Her long side hair, blowing in the wind. No sprites: the firmware warps the rendered
        # pixels of these two bands sideways every frame (see WarpHairChunk() in display.cpp), so
        # the motion is continuous and sub-pixel instead of a handful of baked poses. Polylines are
        # (y, x) points in 240x320 space, traced off a 3x/4x grid of the base:
        #   *_edge - the outermost strands against the backdrop, where the sway is strongest;
        #   *_in   - where her hair meets face, neck or dress: the sway fades to zero there, so
        #            skin, dress and the eye/mouth sprites never move.
        #   *_body - her bare shoulders, which sit *inside* the band with hair on both sides:
        #            (y, x0, x1) rows of a shape that is held perfectly still. The sway fades out
        #            over `feather` px towards it, so hair next to a shoulder is pushed off it or
        #            settles against it rather than dragging the skin along.
        # The backdrop is dragged along for `falloff` px beyond the edge so the silhouette moves
        # without a seam. The amplitude eases in from nothing at y0 (her temples) to `amp` px at
        # full_y, like hair pinned at the crown with loose ends; `floor` keeps the upper hair
        # swaying a little too.
        "hair_flow": {
            "y0": 80, "full_y": 240, "amp": 4.0, "floor": 0.3, "falloff": 12, "feather": 8,
            "left_edge": [(80, 50), (100, 47), (120, 36), (130, 32), (150, 30), (170, 26),
                          (190, 18), (200, 10), (210, 3), (220, 0), (319, 0)],
            "left_in": [(80, 84), (100, 85), (140, 85), (160, 88), (175, 92), (190, 92),
                        (205, 88), (230, 86), (250, 86), (258, 70), (319, 68)],
            "right_in": [(80, 156), (100, 157), (140, 157), (160, 156), (175, 150), (190, 150),
                         (205, 154), (230, 158), (250, 160), (260, 176), (319, 176)],
            "right_edge": [(80, 190), (100, 196), (120, 200), (140, 202), (160, 208), (175, 215),
                           (190, 228), (200, 236), (210, 239), (319, 239)],
            # From the chest down (the sides of her bust, the drape under her arms) the body masks
            # widen right up to the dress, so down there only the outermost strands still sway.
            "left_body": [(206, 40, 44), (215, 22, 54), (230, 12, 56), (250, 12, 60), (262, 10, 70),
                          (319, 8, 70)],
            "right_body": [(208, 196, 200), (215, 188, 212), (230, 184, 218), (250, 180, 220),
                           (262, 176, 224), (319, 176, 232)],
        },
        # Her fox ears, twitching at runtime (see WarpEarsChunk() in display.cpp). Traced off a 5x
        # grid of the base, (x, y) in 240x320 space:
        #   poly  - the ear, down to where hair covers its root;
        #   base  - the root line: nothing below it moves; the ear bends over its own lowest
        #           base_fade px, as if rooted in her head;
        #   pivot - what the ear turns about; tip - its tip (the tip lags the root, bending it);
        #   out   - -1: "outward" turns the tip left (her ear on screen left), +1: right;
        #   margin - px of backdrop around the ear dragged along, so the outline moves seamlessly;
        #   deco  - ornaments on the ear: a polygon, the point it hangs from, how much it hangs
        #           plumb rather than turning with the ear (0..1), and its swing (Hz, damping);
        #   hold  - where her hair (the crown between the ears) must not be dragged along;
        #   pin   - regions held entirely (the flower pinned in her hair beside the ear).
        "ears": [
            {
                "poly": [(47, -2), (53, -2), (62, 12), (72, 23), (82, 31), (92, 37), (97, 42),
                         (92, 53), (52, 58), (47, 50)],
                "base": [(52, 58), (92, 53)],
                "pivot": (74, 52), "tip": (50, 1), "out": -1, "margin": 22, "base_fade": 14,
                "hold": [[(94, 28), (132, 28), (132, 80), (94, 80)]],
                # The red flower, its clasp and pearl strings sit in her hair, not on the ear, so
                # they stay put. (As ornaments riding the ear they turned a whole patch of the
                # picture with them - hair included - which tore the strands around them, and so
                # did the pendulums on the right ear's earring and bead chain.)
                "pin": [[(44, 58), (56, 50), (80, 50), (88, 62), (86, 84), (72, 90), (68, 97),
                         (50, 97), (44, 90), (44, 76)]],
                "deco": [],
            },
            {
                "poly": [(197, -2), (202, -2), (201, 20), (198, 40), (196, 58), (148, 52),
                         (149, 40), (155, 37), (165, 30), (175, 24), (185, 18), (191, 12)],
                "base": [(148, 52), (196, 58)],
                "pivot": (173, 54), "tip": (199, 1), "out": 1, "margin": 22, "base_fade": 14,
                "hold": [[(116, 28), (153, 28), (153, 80), (116, 80)]],
                "deco": [],
            },
        ],
    },
}


def variants_for(prefix):
    """(source file, window key, sprite group, sprite suffix) for every render of a character."""
    out = [
        (f"{prefix}_eyes_half.jpg",   "eyes_win",  "eyes",  "eyes_half"),
        (f"{prefix}_eyes_shut.jpg",   "eyes_win",  "eyes",  "eyes_shut"),
        (f"{prefix}_mouth_small.jpg", "mouth_win", "mouth", "mouth_small"),
        (f"{prefix}_mouth_wide.jpg",  "mouth_win", "mouth", "mouth_wide"),
    ]
    for name in EXPRESSIONS:
        out.append((f"{prefix}_expr_{name}.jpg", "eyes_win",  "eyes",  f"expr_{name}_eyes"))
        out.append((f"{prefix}_expr_{name}.jpg", "mouth_win", "mouth", f"expr_{name}_mouth"))
    return out


THUMB_W, THUMB_H = 82, 86      # 84x88 panel less its 1px border

# Metal accessories of the character being built; set per character in build_character().
ACCESSORY_BOXES = []


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


# Whether finish() snaps backdrop noise to BG; set per character in build_character().
BG_SNAP = True


def finish(rgb):
    """Snap backdrop noise to BG, then quantise to exactly what the panel will show."""
    out = []
    for c in rgb:
        if BG_SNAP and max(abs(c[0] - BG_SAMPLE[0]), abs(c[1] - BG_SAMPLE[1]), abs(c[2] - BG_SAMPLE[2])) <= BG_TOL:
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


# Per-sprite mask adjustments live in each character's "alpha_tweaks", in 1x coordinates.



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


def inpaint(rgb2, boxes, iters=400):
    """Fill the ellipses inscribed in `boxes` (1x coords) from their surroundings, in place.

    A Laplace fill: every masked pixel repeatedly becomes the mean of its four neighbours, with the
    unmasked border held fixed. On a soft bokeh backdrop that reads as more of the same blur, which
    is all a painted-in petal was covering.
    """
    for bx, by, bw, bh in boxes:
        x0, y0, w, h = bx * S, by * S, bw * S, bh * S
        cx, cy, rx, ry = x0 + w / 2.0, y0 + h / 2.0, w / 2.0, h / 2.0
        mask = [(xx, yy) for yy in range(max(1, y0), min(H2 - 1, y0 + h))
                for xx in range(max(1, x0), min(W2 - 1, x0 + w))
                if ((xx + 0.5 - cx) / rx) ** 2 + ((yy + 0.5 - cy) / ry) ** 2 <= 1.0]
        inside = set(yy * W2 + xx for xx, yy in mask)
        ring = [rgb2[yy * W2 + xx] for xx, yy in mask
                for nx, ny in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1))
                if ny * W2 + nx not in inside]
        start = tuple(sum(c[k] for c in ring) / len(ring) for k in range(3))
        buf = {i: start for i in inside}
        for _ in range(iters):
            nxt = {}
            for i in inside:
                acc = [0.0, 0.0, 0.0]
                for j in (i - 1, i + 1, i - W2, i + W2):
                    c = buf[j] if j in buf else rgb2[j]
                    acc[0] += c[0]
                    acc[1] += c[1]
                    acc[2] += c[2]
                nxt[i] = (acc[0] / 4, acc[1] / 4, acc[2] / 4)
            buf = nxt
        for i, c in buf.items():
            rgb2[i] = tuple(int(round(v)) for v in c)


# ---------------------------------------------------------------- falling petals
#
# Drawn procedurally rather than cut from the art: a cherry-blossom petal (a rounded blade with the
# notch at its tip), pink shading from base to tip, and a faint glow that picks up the backdrop's
# bokeh. Each size has PETAL_FRAMES frames of one tumble - the petal turns edge-on and back while
# wobbling a few degrees - so cycling them in order loops seamlessly. The firmware only ever
# translates them; there is no runtime rotation or scaling (LVGL's transform path needs buffers).
PETAL_FRAMES = 8
PETAL_SIZES = [   # (length, width, canvas) in px
    (8, 6, 12),
    (12, 9, 16),
]


def render_petal(length, width, canvas, frame):
    k = frame / PETAL_FRAMES
    turn = math.cos(math.pi * k)                         # 1 -> 0 (edge-on) -> -1 over the cycle
    squash = max(0.2, abs(turn))
    shade = 1.0 if turn >= 0 else 0.82                   # the underside is a shade darker
    ang = math.radians(-35 + 14 * math.sin(2 * math.pi * k))
    ca, sa = math.cos(ang), math.sin(ang)
    hl, hw = length / 2.0, width / 2.0 * squash
    c0, c1 = (232, 112, 150), (255, 210, 222)            # base -> tip
    ss = 4
    cov = [0.0] * (canvas * canvas)
    col = [(0, 0, 0)] * (canvas * canvas)
    for py in range(canvas):
        for px in range(canvas):
            n, acc = 0, [0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    X = px + (sx + 0.5) / ss - canvas / 2.0
                    Y = py + (sy + 0.5) / ss - canvas / 2.0
                    u = (X * ca + Y * sa) / hl
                    v = (-X * sa + Y * ca) / hw
                    if not -1.0 <= u <= 1.0:
                        continue
                    prof = math.sqrt(max(0.0, 1.0 - u * u)) * (0.72 + 0.28 * u)
                    if abs(v) > prof:
                        continue
                    if u > 0.68 and abs(v) < (u - 0.68) * 1.5:
                        continue                         # the notch
                    t = (u + 1.0) / 2.0
                    c = [c0[i] + (c1[i] - c0[i]) * t for i in range(3)]
                    if abs(v) < 0.14 and u < 0.6:
                        c = [min(255, ch + 14) for ch in c]  # faint centre vein
                    n += 1
                    for i in range(3):
                        acc[i] += c[i] * shade
            i = py * canvas + px
            cov[i] = n / (ss * ss)
            if n:
                col[i] = tuple(int(round(a / n)) for a in acc)
    # Soft glow: a blurred copy of the coverage, at low strength, around the petal.
    glow = [0.0] * (canvas * canvas)
    for py in range(canvas):
        for px in range(canvas):
            s, m = 0.0, 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    qx, qy = px + dx, py + dy
                    if 0 <= qx < canvas and 0 <= qy < canvas:
                        s += cov[qy * canvas + qx]
                    m += 1
            glow[py * canvas + px] = s / m
    out = []
    for i in range(canvas * canvas):
        a = max(cov[i] * 0.92, glow[i] * 0.30)
        c = col[i] if cov[i] > 0 else (255, 196, 212)
        out.append((c[0], c[1], c[2], int(round(a * 255))))
    return out


def petal_sprites():
    """[(name suffix, rgba, w, h)], size-major: all frames of size 0, then size 1."""
    out = []
    for si, (length, width, canvas) in enumerate(PETAL_SIZES):
        for f in range(PETAL_FRAMES):
            out.append((f"petal_{si}_{f}", render_petal(length, width, canvas, f), canvas, canvas))
    return out


def over(bg, rgba):
    r, g, b, a = rgba
    t = a / 255.0
    return tuple(int(round(bg[k] + ((r, g, b)[k] - bg[k]) * t)) for k in range(3))


# ---------------------------------------------------------------- wind in the hair
#
# The firmware animates long hair by warping already-rendered pixels (display.cpp, WarpHairChunk),
# so all that ships is geometry: per row, where each side's sway fades in and out, what must stay
# still, and how strong the sway is. 10 x int16 + 1 byte per row, ~5KB, instead of hundreds of KB
# of pose sprites.

HAIR_AMP_UNIT = 32          # amplitude table is in 1/32 px
FLOW_SPAN = 5               # int16 per side per row: p0, p1, p2, body0, body1


def polyline_at(points, y):
    """Linear interpolation of a [(y, x), ...] polyline (sorted by y), clamped at both ends."""
    if y <= points[0][0]:
        return points[0][1]
    for (y0, x0), (y1, x1) in zip(points, points[1:]):
        if y <= y1:
            return x0 + (x1 - x0) * (y - y0) / (y1 - y0)
    return points[-1][1]


def body_at(rows, y):
    """(x0, x1) of a [(y, x0, x1), ...] shape at row y, or (1, 0) (empty) outside it."""
    if not rows or y < rows[0][0] or y > rows[-1][0]:
        return 1, 0
    for (ya, a0, a1), (yb, b0, b1) in zip(rows, rows[1:]):
        if y <= yb:
            t = (y - ya) / (yb - ya)
            return round(a0 + (b0 - a0) * t), round(a1 + (b1 - a1) * t)
    return rows[-1][1], rows[-1][2]


def hair_flow_table(spec):
    """-> {y0, feather, amps, spans}: per portrait row from y0 down, the sway amplitude (1/32 px)
    and, per side, (p0, p1, p2, body0, body1) in portrait x. The sway is 0 at p0 and p2, strongest
    at p1, and fades to 0 over `feather` px towards the still [body0, body1] (empty if body0 >
    body1). Left side: p0 = backdrop end, p1 = outer strands, p2 = face; right side mirrored."""
    y0, full_y, amp, fall = spec["y0"], spec["full_y"], spec["amp"], spec["falloff"]
    floor = spec.get("floor", 0.0)
    amps, spans = [], []
    for y in range(y0, H):
        t = min(1.0, max(0.0, (y - y0) / (full_y - y0)))
        ease = min(1.0, (y - y0) / 12)   # no hard shear line where the band starts
        a = amp * (floor + (1 - floor) * t * t * (3 - 2 * t)) * ease
        amps.append(min(255, round(a * HAIR_AMP_UNIT)))
        le = round(polyline_at(spec["left_edge"], y))
        li = round(polyline_at(spec["left_in"], y))
        ri = round(polyline_at(spec["right_in"], y))
        re = round(polyline_at(spec["right_edge"], y))
        # The warp is only a proper (fold-free) mapping while the band is comfortably wider than
        # the sway; the traced lines keep >30px, this just guards future edits.
        assert li - le >= 3 * amp and re - ri >= 3 * amp, f"hair band too thin at y={y}"
        lb = body_at(spec.get("left_body"), y)
        rb = body_at(spec.get("right_body"), y)
        spans.append((le - fall, le, li) + lb + (ri, re, re + fall) + rb)
    return {"y0": y0, "feather": spec["feather"], "amps": amps, "spans": spans}


def flow_weight(x, side, feather):
    """Sway weight 0..1 of portrait column x for one side's (p0, p1, p2, b0, b1) - the firmware's
    WarpSpan() computes exactly this."""
    p0, p1, p2, b0, b1 = side
    if not p0 < x < p2:
        return 0.0
    w = (x - p0) / (p1 - p0) if x <= p1 else (p2 - x) / (p2 - p1)
    if b0 <= b1:
        dist = b0 - x if x < b0 else (x - b1 if x > b1 else 0)
        w *= min(dist, feather) / feather
    return w


def hair_flow_warp(rgb, table, u_left, u_right):
    """Python twin of the firmware's per-row warp, for previews. u_* are outward displacements in
    px at full amplitude (negative = blown inward)."""
    out = list(rgb)
    fe = table["feather"]
    for r, sp in enumerate(table["spans"]):
        y = table["y0"] + r
        a = table["amps"][r] / HAIR_AMP_UNIT
        row = rgb[y * W:(y + 1) * W]
        left, right = sp[:FLOW_SPAN], sp[FLOW_SPAN:]
        for x in range(W):
            if left[0] < x < left[2]:
                s = x + a * u_left * flow_weight(x, left, fe)
            elif right[0] < x < right[2]:
                s = x - a * u_right * flow_weight(x, right, fe)
            else:
                continue
            i = math.floor(s)
            f = s - i
            c0 = row[min(W - 1, max(0, i))]
            c1 = row[min(W - 1, max(0, i + 1))]
            out[y * W + x] = tuple(round(p + (q - p) * f) for p, q in zip(c0, c1))
    return out


# ---------------------------------------------------------------- ear twitch
#
# Each ear is a bone rotating about its root (display.cpp WarpEarsChunk). Nothing but weights
# ships: per pixel of the ear's box, how much it follows the ear (1 on the ear, easing to 0 over
# `margin` px of backdrop around it and over the `base_fade` px of ear above its root - nothing
# below the root moves, since that is her hair and head), and which ornament
# it belongs to, if any. Ornaments ride on the ear but have their own angle - a pendulum for things
# that dangle - so a bead string swings back to plumb after a flick instead of turning rigidly.
# The firmware resamples the base portrait in flash through these, so it can move pixels across
# rows (a true rotation), which the row-by-row hair warp cannot.

EAR_DECO_FEATHER = 4        # px over which an ornament's own swing fades into its surroundings
EAR_DECO_MAX = 4            # ornament index lives in the top 2 bits of the deco map
# The margin drags the backdrop around an ear along with it, but part of that margin is her own
# hair (the crown between the ears, the locks beside them). Dragged, those strands bend and kink
# where the drag fades out - the "broken" hair. So hair beside the ear stays put: inside the
# ear's `hold` polygons, any pixel brighter than the backdrop, more than EAR_HOLD_GAP px outside
# the ear's outline and not part of an ornament is held, and the ear's pull fades in over
# EAR_HOLD_FEATHER px away from held pixels (all through backdrop).
EAR_HOLD_LUM = 100          # mean RGB above this is hair / skin, not the dark backdrop
EAR_HOLD_GAP = 2.5          # px outside the outline still counted as the ear's own fur
EAR_HOLD_FEATHER = 6        # px over which the pull fades in away from held hair


def _inside(poly, x, y):
    c = False
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (x1 - x0) * (y - y0) / (y1 - y0):
            c = not c
    return c


def _poly_dist(poly, x, y):
    """0 inside `poly`, else the distance to its outline."""
    if _inside(poly, x, y):
        return 0.0
    best = 1e9
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        dx, dy = x1 - x0, y1 - y0
        t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy)))
        best = min(best, math.hypot(x - x0 - t * dx, y - y0 - t * dy))
    return best


def _fall(t):
    """1 at t <= 0 easing to 0 at t >= 1 (cosine: no slope kink at either end)."""
    return 1.0 if t <= 0 else (0.0 if t >= 1 else 0.5 + 0.5 * math.cos(math.pi * t))


def ear_rig(spec, rgb=None):
    """-> dict with the ear's box, bone geometry, ornaments and per-pixel maps (see enco_ear_t).
    rgb (the base portrait) is used to keep her hair around the ear still; None: no hold."""
    poly, margin, bfade = spec["poly"], spec["margin"], spec["base_fade"]
    (bx0, by0), (bx1, by1) = spec["base"]
    tx, ty = spec["tip"]
    # Unit normal of the root line, pointing away from the tip: "below the root".
    nx, ny = -(by1 - by0), bx1 - bx0
    n = math.hypot(nx, ny)
    nx, ny = nx / n, ny / n
    if (tx - bx0) * nx + (ty - by0) * ny > 0:
        nx, ny = -nx, -ny
    decos = spec.get("deco", [])
    assert len(decos) <= EAR_DECO_MAX
    wmap, dmap = {}, {}
    pts = [(x, y, margin) for x, y in poly]
    pts += [(x, y, EAR_DECO_FEATHER) for dc in decos for x, y in dc["poly"]]
    sx0 = max(0, int(min(x - m for x, _, m in pts)) - 1)
    sx1 = min(W - 1, int(max(x + m for x, _, m in pts)) + 1)
    sy0 = max(0, int(min(y - m for _, y, m in pts)) - 1)
    sy1 = min(H - 1, int(max(y + m for _, y, m in pts)) + 1)
    dist = {}
    for y in range(sy0, sy1 + 1):
        for x in range(sx0, sx1 + 1):
            dist[(x, y)] = _poly_dist(poly, x, y)
    held = set()
    holds = spec.get("hold", [])
    if rgb is not None and holds:
        for (x, y), d in dist.items():
            if (d > EAR_HOLD_GAP and sum(rgb[y * W + x]) > 3 * EAR_HOLD_LUM
                    and any(_inside(hp, x, y) for hp in holds)
                    and all(_poly_dist(dc["poly"], x, y) >= EAR_DECO_FEATHER for dc in decos)):
                held.add((x, y))
    for (x, y) in dist:
        if any(_inside(pp, x, y) for pp in spec.get("pin", [])):
            held.add((x, y))
    hf = EAR_HOLD_FEATHER
    offs = [(i, j, math.hypot(i, j)) for j in range(-hf, hf + 1) for i in range(-hf, hf + 1)
            if math.hypot(i, j) < hf]
    for y in range(sy0, sy1 + 1):
        for x in range(sx0, sx1 + 1):
            d = dist[(x, y)]
            if d >= margin:
                w = 0.0
            else:
                below = (x - bx0) * nx + (y - by0) * ny
                w = _fall(d / margin) * _fall((below + bfade) / bfade)
                if w > 0 and held:
                    near = min((r for i, j, r in offs if (x + i, y + j) in held), default=hf)
                    w *= 1.0 - _fall(near / hf)
            best, bi = 0.0, 0
            for i, dc in enumerate(decos):
                dd = _poly_dist(dc["poly"], x, y)
                wd = _fall(dd / EAR_DECO_FEATHER)
                if wd > best:
                    best, bi = wd, i
            wv, dv = round(w * 255), round(best * 63)
            if wv or dv:
                wmap[(x, y)] = wv
                dmap[(x, y)] = (bi << 6) | dv if dv else 0
    xs = [p[0] for p in wmap]
    ys = [p[1] for p in wmap]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    bw, bh = x1 - x0 + 1, y1 - y0 + 1
    assert bw <= 255 and bh <= 255, "ear box too big for uint8"
    weight = [wmap.get((x0 + i % bw, y0 + i // bw), 0) for i in range(bw * bh)]
    deco_map = [dmap.get((x0 + i % bw, y0 + i // bw), 0) for i in range(bw * bh)]
    return {"box": (x0, y0, bw, bh), "pivot": spec["pivot"], "tip": spec["tip"], "out": spec["out"],
            "deco": decos, "weight": weight, "deco_map": deco_map}


def _rot(vx, vy, a):
    c, s = math.cos(a), math.sin(a)
    return c * vx - s * vy, s * vx + c * vy


def ear_warp(rgb, rigs, out_base, out_tip, deco_angles):
    """Python twin of the firmware's ear warp, for previews. out_* are outward angles in radians
    (base and tip of the bone, same for both ears); deco_angles[e][i] are the ornaments' absolute
    screen angles."""
    out = list(rgb)

    def sample(sx, sy):
        sx = min(W - 1.0, max(0.0, sx))
        sy = min(H - 1.0, max(0.0, sy))
        ix, iy = min(W - 2, int(sx)), min(H - 2, int(sy))
        fx, fy = sx - ix, sy - iy
        p = [rgb[(iy + j) * W + ix + i] for j in (0, 1) for i in (0, 1)]
        return tuple(round((p[0][k] * (1 - fx) + p[1][k] * fx) * (1 - fy) +
                           (p[2][k] * (1 - fx) + p[3][k] * fx) * fy) for k in range(3))

    for e, rig in enumerate(rigs):
        bx, by, bw, bh = rig["box"]
        cx, cy = rig["pivot"]
        tx, ty = rig["tip"]
        ln = math.hypot(tx - cx, ty - cy)
        ux, uy = (tx - cx) / ln, (ty - cy) / ln
        sb, st = rig["out"] * out_base, rig["out"] * out_tip
        dparams = []
        for i, dc in enumerate(rig["deco"]):
            ax, ay = dc["anchor"]
            s = max(0.0, min(1.0, ((ax - cx) * ux + (ay - cy) * uy) / ln))
            th = sb + (st - sb) * s
            rx, ry = _rot(ax - cx, ay - cy, th)
            dparams.append((ax, ay, cx + rx, cy + ry, deco_angles[e][i]))
        for j in range(bh):
            for i in range(bw):
                wv, dv = rig["weight"][j * bw + i], rig["deco_map"][j * bw + i]
                if not wv and not dv & 63:
                    continue
                x, y = bx + i, by + j
                s = max(0.0, min(1.0, ((x - cx) * ux + (y - cy) * uy) / ln))
                th = sb + (st - sb) * s
                ex, ey = _rot(x - cx, y - cy, -th)
                we, wd = wv / 255, (dv & 63) / 63
                dx = (1 - wd) * we * (cx + ex - x)
                dy = (1 - wd) * we * (cy + ey - y)
                if wd:
                    ax, ay, fx, fy, phi = dparams[dv >> 6]
                    qx, qy = _rot(x - fx, y - fy, -phi)
                    dx += wd * (ax + qx - x)
                    dy += wd * (ay + qy - y)
                out[y * W + x] = sample(x + dx, y + dy)
    return out


# ---------------------------------------------------------------- build + emit

def build_character(cid, ch, outdir):
    global BG_SNAP, ACCESSORY_BOXES
    BG_SNAP = ch["bg_snap"]
    ACCESSORY_BOXES = list(ch["accessories"])
    prefix = ch["prefix"]
    sym = f"enco_{prefix}"
    build = os.path.join(BUILD, cid)
    os.makedirs(build, exist_ok=True)
    print(f"=== {cid}: {ch['title']}")

    base2, _, _ = F.load_rgb(os.path.join(SOURCE, f"{prefix}_base.jpg"), W2, H2)
    if ch["erase"]:
        inpaint(base2, ch["erase"])
    base_lin = half(base2)
    base = finish(base_lin)

    variants = variants_for(prefix)
    frames, groups, loaded = {}, {}, {}
    for fname, win_key, group, name in variants:
        if fname not in loaded:
            loaded[fname] = F.load_rgb(os.path.join(SOURCE, fname), W2, H2)[0]
        var2 = loaded[fname]
        win2 = tuple(v * S for v in ch[win_key])
        dx, dy, err = align(base2, var2, win2)
        var2 = shifted(var2, dx, dy)
        a, kept = feature_alpha(base2, var2, win2)
        if name in ch["alpha_tweaks"]:
            a = tweak_alpha(a, base2, win2, ch["alpha_tweaks"][name])
        frames[name] = finish(half(blend(base2, var2, a)))
        groups.setdefault(group, []).append(name)
        print(f"{sym}_{name}: offset ({dx:+d},{dy:+d}) @2x, ring residual {err:.1f}, blobs {kept}")

    rects = {g: rect_of_changes(base, [frames[n] for n in names]) for g, names in groups.items()}
    for g, r in rects.items():
        print(f"{g} sprite: x={r[0]} y={r[1]} {r[2]}x{r[3]}")

    hair_regions, hair_frames = [], {}
    if ch["hair"]:
        # Bangs end where the eye sprite starts, so a blink never cuts across swaying hair.
        bx, by, bw, _ = ch["hair"][0][1]
        bangs = (bx, by, bw, rects["eyes"][1] - by)
        hair_regions = [("bangs", bangs)] + ch["hair"][1:]
        locks = ch["hair"][1][1]
        locks_y = (locks[1], locks[1] + locks[3])
        keepout = [rects["eyes"], rects["mouth"]]
        hair_frames = {name: finish(warp_hair(base, W, H, amt, bangs, locks_y, keepout=keepout))
                       for name, amt in HAIR_LEVELS}
        for region, (rx, ry, rw, rh) in hair_regions:
            print(f"{region} sprite: x={rx} y={ry} {rw}x{rh}")

    thumb = finish(box_downscale(base_lin, W, H, ch["thumb_src"], THUMB_W, THUMB_H))
    petals = petal_sprites() if ch["petals"] else []
    flow = hair_flow_table(ch["hair_flow"]) if ch.get("hair_flow") else None
    ears = [ear_rig(e, base) for e in ch.get("ears", [])]

    total = 0
    guard = f"ENCO_CHAR_{prefix.upper()}"
    path = os.path.join(outdir, f"face_assets_{cid}.c")
    with open(path, "w") as fh:
        fh.write("// Generated by tools/face_assets/build_face_assets.py - do not edit by hand.\n"
                 "//\n"
                 f"// Character \"{cid}\": {ch['title']}.\n"
                 "// A full-screen RGB565 base portrait plus small opaque overlay sprites for the\n"
                 "// blink, mouth, expression and hair-breeze frames. All of it is drawn straight\n"
                 "// from flash. Only enco_char_" + cid + " is exported.\n"
                 "\n#include \"face_assets.h\"\n"
                 f"\n#if {guard}\n")
        total += F.emit_c_rgb565(fh, f"{sym}_base", base, W, H, static=True)
        for _, _, group, name in variants:
            x, y, cw, chh = rects[group]
            total += F.emit_c_rgb565(fh, f"{sym}_{name}", F.crop(frames[name], W, H, x, y, cw, chh),
                                     cw, chh, static=True)
        for region, (rx, ry, rw, rh) in hair_regions:
            for level_name, _ in HAIR_LEVELS:
                total += F.emit_c_rgb565(fh, f"{sym}_{region}_{level_name}",
                                         F.crop(hair_frames[level_name], W, H, rx, ry, rw, rh),
                                         rw, rh, static=True)
        total += F.emit_c_rgb565(fh, f"{sym}_thumb", thumb, THUMB_W, THUMB_H, static=True)
        for name, rgba, pw, ph in petals:
            total += F.emit_c_rgb565a8(fh, f"{sym}_{name}", rgba, pw, ph, static=True)

        fh.write(f"\nstatic const enco_face_expr_t {sym}_exprs[] = {{\n")
        for name in EXPRESSIONS:
            fh.write(f"    {{\"{name}\", &{sym}_expr_{name}_eyes, &{sym}_expr_{name}_mouth}},\n")
        fh.write("};\n")
        for region, _ in hair_regions:
            lv = {n: f"&{sym}_{region}_{n}" for n, _ in HAIR_LEVELS}
            fh.write(f"\nstatic const lv_image_dsc_t* const {sym}_{region}[5] = {{\n"
                     f"    {lv['left']}, {lv['lhalf']}, NULL, {lv['rhalf']}, {lv['right']},\n}};\n")
        if petals:
            fh.write(f"\nstatic const lv_image_dsc_t* const {sym}_petals[{len(petals)}] = {{\n")
            for name, _, _, _ in petals:
                fh.write(f"    &{sym}_{name},\n")
            fh.write("};\n")
        if flow:
            fy0, famps, fspans = flow["y0"], flow["amps"], flow["spans"]
            fh.write(f"\n// Wind in the hair: {len(famps)} rows from y={fy0}, amplitude in 1/{HAIR_AMP_UNIT} px\n"
                     f"static const uint8_t {sym}_flow_amp[{len(famps)}] = {{\n")
            for i in range(0, len(famps), 20):
                fh.write("    " + "".join(f"{v}," for v in famps[i:i + 20]) + "\n")
            fh.write(f"}};\n\n// per row, left then right: p0, p1, p2, body0, body1\n"
                     f"static const int16_t {sym}_flow_spans[{len(fspans) * 2 * FLOW_SPAN}] = {{\n")
            for s in fspans:
                fh.write("    " + ", ".join(str(v) for v in s) + ",\n")
            fh.write(f"}};\n\nstatic const enco_hair_flow_t {sym}_flow = {{\n"
                     f"    .y0 = {fy0},\n    .rows = {len(famps)},\n    .amp_unit = {HAIR_AMP_UNIT},\n"
                     f"    .feather = {flow['feather']},\n"
                     f"    .amp = {sym}_flow_amp,\n    .spans = {sym}_flow_spans,\n}};\n")
            total += len(famps) * (1 + 4 * FLOW_SPAN)
        for e, rig in enumerate(ears):
            bx, by, bw, bh = rig["box"]
            for key, what in (("weight", "follows the ear, 0..255"),
                              ("deco_map", "ornament index << 6 | its weight 0..63")):
                data = rig[key]
                fh.write(f"\n// ear {e} {key}: {bw}x{bh} at ({bx},{by}), how much each pixel {what}\n"
                         f"static const uint8_t {sym}_ear{e}_{key}[{len(data)}] = {{\n")
                for i in range(0, len(data), 24):
                    fh.write("    " + "".join(f"{v}," for v in data[i:i + 24]) + "\n")
                fh.write("};\n")
                total += len(data)
        if ears:
            fh.write(f"\nstatic const enco_ear_t {sym}_ears[{len(ears)}] = {{\n")
            for e, rig in enumerate(ears):
                bx, by, bw, bh = rig["box"]
                decos = "".join(
                    f"{{{d['anchor'][0]}, {d['anchor'][1]}, {round(d['hang'] * 255)}, "
                    f"{round(d['hz'] * 10)}, {round(d['zeta'] * 100)}}}, " for d in rig["deco"])
                fh.write(f"    {{.box_x = {bx}, .box_y = {by}, .box_w = {bw}, .box_h = {bh},\n"
                         f"     .pivot_x = {rig['pivot'][0]}, .pivot_y = {rig['pivot'][1]},"
                         f" .tip_x = {rig['tip'][0]}, .tip_y = {rig['tip'][1]},\n"
                         f"     .out_sign = {rig['out']}, .deco_count = {len(rig['deco'])},"
                         f" .deco = {{{decos}}},\n"
                         f"     .weight = {sym}_ear{e}_weight, .deco_map = {sym}_ear{e}_deco_map}},\n")
            fh.write("};\n")

        ex, ey, _, _ = rects["eyes"]
        mx, my, _, _ = rects["mouth"]
        hr = dict(hair_regions)
        hpos = lambda r: hr[r][:2] if r in hr else (0, 0)
        hptr = lambda r: f"{sym}_{r}" if r in hr else "NULL"
        lane_l, lane_r = ch["petal_lanes"]
        fh.write(f"\nconst enco_character_t enco_char_{cid} = {{\n"
                 f"    .id = \"{cid}\",\n"
                 f"    .base = &{sym}_base,\n"
                 f"    .thumb = &{sym}_thumb,\n"
                 f"    .bg_color = 0x{ch['bg_color']:06x},\n"
                 f"    .eyes_x = {ex}, .eyes_y = {ey},\n"
                 f"    .mouth_x = {mx}, .mouth_y = {my},\n"
                 f"    .eyes_half = &{sym}_eyes_half,\n"
                 f"    .eyes_shut = &{sym}_eyes_shut,\n"
                 f"    .mouth_small = &{sym}_mouth_small,\n"
                 f"    .mouth_wide = &{sym}_mouth_wide,\n"
                 f"    .exprs = {sym}_exprs,\n"
                 f"    .expr_count = {len(EXPRESSIONS)},\n"
                 f"    .bangs_x = {hpos('bangs')[0]}, .bangs_y = {hpos('bangs')[1]},\n"
                 f"    .locks_l_x = {hpos('locks_l')[0]}, .locks_l_y = {hpos('locks_l')[1]},\n"
                 f"    .locks_r_x = {hpos('locks_r')[0]}, .locks_r_y = {hpos('locks_r')[1]},\n"
                 f"    .bangs = {hptr('bangs')},\n"
                 f"    .locks_l = {hptr('locks_l')},\n"
                 f"    .locks_r = {hptr('locks_r')},\n"
                 f"    .petals = {sym + '_petals' if petals else 'NULL'},\n"
                 f"    .petal_frames = {PETAL_FRAMES if petals else 0},\n"
                 f"    .petal_sizes = {len(PETAL_SIZES) if petals else 0},\n"
                 f"    .petal_lane_l = {lane_l}, .petal_lane_r = {lane_r},\n"
                 f"    .hair_flow = {'&' + sym + '_flow' if flow else 'NULL'},\n"
                 f"    .ears = {sym + '_ears' if ears else 'NULL'},\n"
                 f"    .ear_count = {len(ears)},\n"
                 "};\n"
                 f"\n#endif  // {guard}\n")

    # Previews: exactly what the panel shows (base + sprites pasted at their rectangles).
    F.write_png_rgb(os.path.join(build, "frame_base.png"), base, W, H)
    F.write_png_rgb(os.path.join(build, "frame_thumb.png"), thumb, THUMB_W, THUMB_H)
    for _, _, group, name in variants:
        F.write_png_rgb(os.path.join(build, f"frame_{name}.png"), paste(base, frames[name], rects[group]), W, H)
    for name in EXPRESSIONS:
        comp = paste(base, frames[f"expr_{name}_eyes"], rects["eyes"])
        comp = paste(comp, frames[f"expr_{name}_mouth"], rects["mouth"])
        F.write_png_rgb(os.path.join(build, f"frame_expr_{name}.png"), comp, W, H)
    for level_name, _ in HAIR_LEVELS if hair_regions else ():
        comp = base
        for _, r in hair_regions:
            comp = paste(comp, hair_frames[level_name], r)
        F.write_png_rgb(os.path.join(build, f"frame_hair_{level_name}.png"), comp, W, H)
    if flow:
        # The sway's extremes (strongest outward gust, blown inward), an exaggerated 3x gust that
        # makes anything moving that should not obvious, and where it acts: red is full strength,
        # fading to nothing at the backdrop, her face, dress and shoulders.
        F.write_png_rgb(os.path.join(build, "frame_flow_out.png"), hair_flow_warp(base, flow, 1.25, 1.25), W, H)
        F.write_png_rgb(os.path.join(build, "frame_flow_in.png"), hair_flow_warp(base, flow, -0.5, -0.5), W, H)
        F.write_png_rgb(os.path.join(build, "frame_flow_out3x.png"), hair_flow_warp(base, flow, 3.0, 3.0), W, H)
        fy0, famps, fspans = flow["y0"], flow["amps"], flow["spans"]
        bands = [tuple(v // 2 for v in c) for c in base]
        for r, sp in enumerate(fspans):
            k = famps[r] / max(famps)
            for x in range(W):
                w = max(flow_weight(x, sp[:FLOW_SPAN], flow["feather"]),
                        flow_weight(x, sp[FLOW_SPAN:], flow["feather"]))
                if w > 0:
                    c = bands[(fy0 + r) * W + x]
                    bands[(fy0 + r) * W + x] = (min(255, c[0] + round(200 * w * k)), c[1], c[2])
        F.write_png_rgb(os.path.join(build, "frame_flow_bands.png"), bands, W, H)
    if ears:
        # Both ears flicked outward (tip lagging behind), perked inward, and where the rig acts:
        # red = follows the ear, green = an ornament's own swing.
        deg = math.pi / 180
        swing = [[r["out"] * (1 - d["hang"]) * 12 * deg + r["out"] * d["hang"] * -6 * deg
                  for d in r["deco"]] for r in ears]
        F.write_png_rgb(os.path.join(build, "frame_ears_out.png"),
                        ear_warp(base, ears, 12 * deg, 16 * deg, swing), W, H)
        F.write_png_rgb(os.path.join(build, "frame_ears_in.png"),
                        ear_warp(base, ears, -7 * deg, -9 * deg, [[0.0] * len(r["deco"]) for r in ears]), W, H)
        emap = [tuple(v // 2 for v in c) for c in base]
        for r in ears:
            bx, by, bw, bh = r["box"]
            for j in range(bh):
                for i in range(bw):
                    wv, dv = r["weight"][j * bw + i], r["deco_map"][j * bw + i]
                    c = emap[(by + j) * W + bx + i]
                    emap[(by + j) * W + bx + i] = (min(255, c[0] + wv * 200 // 255),
                                                   min(255, c[1] + (dv & 63) * 200 // 63), c[2])
        F.write_png_rgb(os.path.join(build, "frame_ears_map.png"), emap, W, H)
    if petals:
        # Every petal frame, 4x, over the backdrop colour, in one strip per size.
        sc = 4
        cw = max(p[2] for p in petals)
        sw, sh = cw * PETAL_FRAMES * sc, cw * len(PETAL_SIZES) * sc
        strip = [(40, 30, 34)] * (sw * sh)
        for idx, (_, rgba, pw, ph) in enumerate(petals):
            ox, oy = (idx % PETAL_FRAMES) * cw * sc, (idx // PETAL_FRAMES) * cw * sc
            for y in range(ph * sc):
                for x in range(pw * sc):
                    strip[(oy + y) * sw + ox + x] = over((40, 30, 34), rgba[(y // sc) * pw + x // sc])
        F.write_png_rgb(os.path.join(build, "petals.png"), strip, sw, sh)

    print(f"wrote {path}, {total:,} bytes of flash; previews in {build}\n")
    return total


HEADER = """\
// Generated by tools/face_assets/build_face_assets.py - do not edit by hand.
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which characters are compiled in. Each one costs roughly 0.3-0.4 MB of flash; override with
// -D ENCO_CHAR_<ID>=0 to leave one out (its art and face_assets_<id>.c stay in the tree).
%(guards)s
#define ENCO_FACE_W %(w)d
#define ENCO_FACE_H %(h)d

// Pre-scaled bust for the camera HUD's side panel, baked at build time.
#define ENCO_FACE_THUMB_W %(tw)d
#define ENCO_FACE_THUMB_H %(th)d

// A named facial expression: an eyes sprite and a mouth sprite. Eye sprites sit at the
// character's eyes_x/eyes_y, mouth sprites at mouth_x/mouth_y.
typedef struct {
  const char* name;
  const lv_image_dsc_t* eyes;
  const lv_image_dsc_t* mouth;
} enco_face_expr_t;

// Wind-in-the-hair geometry. Row r covers portrait row y0 + r. spans[r * 10 ..] holds, in portrait
// x, the left side then the right side as (p0, p1, p2, body0, body1): the sideways sway is zero at
// p0 and p2 (backdrop / face, neck, dress), strongest at p1 (the outer strands), linear in between,
// and fades to zero over `feather` px towards [body0, body1] - a bare shoulder that must stay put
// (none when body0 > body1). amp[r] / amp_unit is the row's sway amplitude in px.
typedef struct {
  int16_t y0;
  uint16_t rows;
  uint8_t amp_unit;
  uint8_t feather;
  const uint8_t* amp;
  const int16_t* spans;
} enco_hair_flow_t;

// An ornament riding on an ear (a flower, an earring, a bead string). It is carried along with the
// ear's root but turns about `x, y` by an angle of its own: a damped spring that pulls it towards
// the ear's angle, or - by `hang` / 255 - towards hanging plumb. hz10 / 10 is the swing frequency,
// zeta100 / 100 the damping ratio.
typedef struct {
  int16_t x, y;
  uint8_t hang;
  uint8_t hz10;
  uint8_t zeta100;
} enco_ear_deco_t;

// One ear: a bone turning about its root (pivot) towards its tip. weight[] (box_w x box_h, from
// box_x, box_y) is how much each pixel follows the ear, 0..255; deco_map[] is (ornament << 6) |
// how much it follows that ornament instead, 0..63. out_sign turns "outward" (positive) into a
// screen rotation: -1 for her left-on-screen ear (tip goes left), +1 for the other.
typedef struct {
  int16_t box_x, box_y;
  uint8_t box_w, box_h;
  int16_t pivot_x, pivot_y;
  int16_t tip_x, tip_y;
  int8_t out_sign;
  uint8_t deco_count;
  enco_ear_deco_t deco[4];
  const uint8_t* weight;
  const uint8_t* deco_map;
} enco_ear_t;

// Everything display.cpp needs to draw and animate one character.
typedef struct {
  const char* id;
  const lv_image_dsc_t* base;   // full-screen ENCO_FACE_W x ENCO_FACE_H portrait
  const lv_image_dsc_t* thumb;  // ENCO_FACE_THUMB_W x ENCO_FACE_THUMB_H camera-HUD bust
  uint32_t bg_color;            // face container fill, matching the portrait's backdrop

  // Blink, lip-sync and expression sprites (all opaque, positioned at the offsets below).
  int16_t eyes_x, eyes_y;
  int16_t mouth_x, mouth_y;
  const lv_image_dsc_t* eyes_half;
  const lv_image_dsc_t* eyes_shut;
  const lv_image_dsc_t* mouth_small;
  const lv_image_dsc_t* mouth_wide;
  const enco_face_expr_t* exprs;
  uint8_t expr_count;

  // Hair breeze. Each table holds 5 frames indexed by level + 2 (level -2 full-left .. +2
  // full-right); [2] is NULL because the base portrait already shows the hair at rest. NULL tables
  // mean the character has no breeze.
  int16_t bangs_x, bangs_y;
  int16_t locks_l_x, locks_l_y;
  int16_t locks_r_x, locks_r_y;
  const lv_image_dsc_t* const* bangs;
  const lv_image_dsc_t* const* locks_l;
  const lv_image_dsc_t* const* locks_r;

  // Falling petals: petal_sizes x petal_frames RGB565A8 sprites, size-major, one tumble cycle per
  // size. They fall left of petal_lane_l and right of petal_lane_r, clear of her face. NULL: none.
  const lv_image_dsc_t* const* petals;
  uint8_t petal_frames;
  uint8_t petal_sizes;
  int16_t petal_lane_l, petal_lane_r;

  // Wind in the long hair, warped at runtime (display.cpp WarpHairChunk). NULL: no flow.
  const enco_hair_flow_t* hair_flow;

  // Twitching ears, warped at runtime (display.cpp WarpEarsChunk). NULL / 0: none.
  const enco_ear_t* ears;
  uint8_t ear_count;
} enco_character_t;

%(externs)s
#ifdef __cplusplus
}
#endif
"""


def write_header(outdir):
    guards = "".join(f"#ifndef ENCO_CHAR_{c['prefix'].upper()}\n#define ENCO_CHAR_{c['prefix'].upper()} 1\n#endif\n"
                     for c in CHARACTERS.values())
    externs = "".join(f"#if ENCO_CHAR_{c['prefix'].upper()}\n"
                      f"extern const enco_character_t enco_char_{cid};  // {c['title']}\n#endif\n"
                      for cid, c in CHARACTERS.items())
    with open(os.path.join(outdir, "face_assets.h"), "w") as fh:
        fh.write(HEADER % {"guards": guards, "w": W, "h": H, "tw": THUMB_W, "th": THUMB_H,
                           "externs": externs})


def main():
    args = sys.argv[1:]
    if not args or len(args) > 2:
        sys.exit(__doc__)
    outdir = args[0]
    only = args[1:] or list(CHARACTERS)
    for cid in only:
        if cid not in CHARACTERS:
            sys.exit(f"unknown character {cid!r}; have {', '.join(CHARACTERS)}")
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(BUILD, exist_ok=True)

    write_header(outdir)
    total = 0
    for cid in only:
        total += build_character(cid, CHARACTERS[cid], outdir)
    print(f"wrote {outdir}/face_assets.h + face_assets_{{{','.join(only)}}}.c, {total:,} bytes of flash")


if __name__ == "__main__":
    main()
