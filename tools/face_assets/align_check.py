#!/usr/bin/env python3
"""Check whether a generated variant lines up with the cached base image.

Quantises the variant against the base's palette, searches a small translation window for the
offset that best matches the base *outside* the region of interest, and reports the residual plus
a visual diff.  If the residual is high the variant is unusable as an overlay.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import face_lib as F

state, variant, out_prefix = sys.argv[1:4]
roi = [int(v) for v in sys.argv[4:8]] if len(sys.argv) > 7 else None

palette, base, w, h = F.load_state(state)
pixels, vw, vh = F.load_rgb(variant, w, h)
var = F.build_index_map(pixels, palette)


def in_roi(x, y):
    return roi and roi[0] <= x < roi[0] + roi[2] and roi[1] <= y < roi[1] + roi[3]


best = None
for dy in range(-8, 9):
    for dx in range(-8, 9):
        bad = tot = 0
        for y in range(0, h, 3):
            sy = y + dy
            if not (0 <= sy < h):
                continue
            for x in range(0, w, 3):
                sx = x + dx
                if not (0 <= sx < w) or in_roi(x, y):
                    continue
                tot += 1
                if base[y * w + x] != var[sy * w + sx]:
                    bad += 1
        if tot and (best is None or bad / tot < best[0]):
            best = (bad / tot, dx, dy)

res, dx, dy = best
print(f"best offset dx={dx} dy={dy}, mismatch outside ROI = {res * 100:.1f}%")

# Visual diff at the best offset: grey = same, red = differs.
vis = []
for y in range(h):
    for x in range(w):
        sy, sx = y + dy, x + dx
        v = var[sy * w + sx] if 0 <= sy < h and 0 <= sx < w else -1
        if v == base[y * w + x]:
            r, g, b = palette[v]
            vis.append(((r + 160) // 2, (g + 160) // 2, (b + 160) // 2))
        else:
            vis.append((255, 0, 0))
F.write_png_rgb(out_prefix + "_diff.png", vis, w, h)
F.write_png_indexed(out_prefix + "_quant.png", var, palette, w, h, scale=2)
print("wrote", out_prefix + "_diff.png", out_prefix + "_quant.png")
