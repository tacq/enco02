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
