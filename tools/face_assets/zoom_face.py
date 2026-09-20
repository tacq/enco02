#!/usr/bin/env python3
"""Zoom into a region of the cached quantised face, with a coordinate grid, to pick geometry."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import face_lib as F

state, x0, y0, cw, ch, scale, out = sys.argv[1:8]
x0, y0, cw, ch, scale = int(x0), int(y0), int(cw), int(ch), int(scale)
palette, indices, w, h = F.load_state(state)
sub = F.crop(indices, w, h, x0, y0, cw, ch)
F.write_png_indexed(out, sub, palette, cw, ch, scale=scale, grid=10, origin=(x0, y0))
print(f"{out}: source ({x0},{y0}) {cw}x{ch} at {scale}x, red grid every 10 source px")
