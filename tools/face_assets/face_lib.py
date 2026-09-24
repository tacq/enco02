#!/usr/bin/env python3
"""Shared helpers for the ENCO-02 face asset pipeline.

Quantisation is cached to a small binary blob and reused by every downstream tool.  No third-party
deps: PIL is unavailable and this machine's binary authorization policy blocks installing new
tooling, so macOS `sips` does the decode/resize and the BMP is parsed by hand.
"""
import json
import os
import struct
import subprocess
import zlib

N_COLORS = 16


# ---------------------------------------------------------------- image loading

def load_rgb(path, w, h):
    """Decode + resize via sips, then parse the BMP into a flat list of (r,g,b)."""
    tmp = "/tmp/_enco_asset.bmp"
    subprocess.run(["sips", "-s", "format", "bmp", "-z", str(h), str(w), path, "--out", tmp],
                   check=True, capture_output=True)
    d = open(tmp, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    bw = struct.unpack_from("<i", d, 18)[0]
    bh = struct.unpack_from("<i", d, 22)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    top_down = bh < 0
    bh = abs(bh)
    assert bpp in (24, 32), f"unexpected bpp {bpp}"
    nb = bpp // 8
    row_bytes = ((bw * nb + 3) // 4) * 4

    px = [None] * (bw * bh)
    for y in range(bh):
        src_y = y if top_down else (bh - 1 - y)
        base = off + src_y * row_bytes
        row = y * bw
        for x in range(bw):
            i = base + x * nb
            px[row + x] = (d[i + 2], d[i + 1], d[i])
    os.remove(tmp)
    return px, bw, bh


# ---------------------------------------------------------------- quantisation

def median_cut(pixels, n):
    boxes = [list(set(pixels))]
    while len(boxes) < n:
        target, ch_best, spread_best = None, 0, -1
        for b in boxes:
            if len(b) < 2:
                continue
            for ch in range(3):
                vals = [p[ch] for p in b]
                spread = max(vals) - min(vals)
                if spread > spread_best:
                    spread_best, ch_best, target = spread, ch, b
        if target is None:
            break
        target.sort(key=lambda p: p[ch_best])
        mid = len(target) // 2
        boxes.remove(target)
        boxes.append(target[:mid])
        boxes.append(target[mid:])

    palette = []
    for b in boxes:
        if b:
            palette.append(tuple(sum(p[i] for p in b) // len(b) for i in range(3)))
    while len(palette) < n:
        palette.append((0, 0, 0))
    return palette[:n]


def nearest(c, palette):
    best, bestd = 0, 1 << 30
    for k, p in enumerate(palette):
        dr, dg, db = c[0] - p[0], c[1] - p[1], c[2] - p[2]
        d = 2 * dr * dr + 4 * dg * dg + 3 * db * db
        if d < bestd:
            best, bestd = k, d
    return best


def build_index_map(pixels, palette):
    cache, out = {}, []
    for p in pixels:
        i = cache.get(p)
        if i is None:
            cache[p] = i = nearest(p, palette)
        out.append(i)
    return out


# ---------------------------------------------------------------- cache

def save_state(path, palette, indices, w, h):
    meta = {"w": w, "h": h, "palette": [list(c) for c in palette]}
    with open(path, "wb") as f:
        blob = json.dumps(meta).encode()
        f.write(struct.pack("<I", len(blob)))
        f.write(blob)
        f.write(bytes(indices))


def load_state(path):
    with open(path, "rb") as f:
        n = struct.unpack("<I", f.read(4))[0]
        meta = json.loads(f.read(n))
        indices = list(f.read())
    palette = [tuple(c) for c in meta["palette"]]
    return palette, indices, meta["w"], meta["h"]


# ---------------------------------------------------------------- png output

def write_png_rgb(path, rgb_rows, w, h):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw += bytes(rgb_rows[y * w + x])

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def write_png_indexed(path, indices, palette, w, h, scale=1, grid=0, origin=(0, 0)):
    """Upscale + optional coordinate grid, for eyeballing regions."""
    ow, oh = w * scale, h * scale
    out = [None] * (ow * oh)
    for y in range(h):
        for x in range(w):
            c = palette[indices[y * w + x]]
            for dy in range(scale):
                row = (y * scale + dy) * ow
                for dx in range(scale):
                    out[row + x * scale + dx] = c
    if grid:
        ox, oy = origin
        for y in range(h):
            if (y + oy) % grid == 0:
                for x in range(ow):
                    out[(y * scale) * ow + x] = (255, 0, 0)
        for x in range(w):
            if (x + ox) % grid == 0:
                for yy in range(oh):
                    out[yy * ow + x * scale] = (255, 0, 0)
    write_png_rgb(path, out, ow, oh)


def crop(indices, w, h, x0, y0, cw, ch):
    return [indices[(y0 + y) * w + (x0 + x)] for y in range(ch) for x in range(cw)]


# ---------------------------------------------------------------- change detection

def change_mask(base, var, w, h, win, neighbours=9):
    """Pixels inside `win` where `var` differs from `base`, minus requantisation speckle."""
    wx, wy, ww, wh = win
    d = [[1 if base[y * w + x] != var[y * w + x] else 0 for x in range(wx, wx + ww)]
         for y in range(wy, wy + wh)]
    out = bytearray(w * h)
    for y in range(1, wh - 1):
        for x in range(1, ww - 1):
            if d[y][x] and sum(d[y + dy][x + dx] for dy in (-1, 0, 1)
                               for dx in (-1, 0, 1)) >= neighbours:
                out[(wy + y) * w + (wx + x)] = 1
    return out


def blobs(mask, w, h):
    """8-connected components of `mask`, largest first, as (area, [pixel indices])."""
    seen = bytearray(w * h)
    found = []
    for i in range(w * h):
        if not mask[i] or seen[i]:
            continue
        stack, cells = [i], []
        seen[i] = 1
        while stack:
            j = stack.pop()
            cells.append(j)
            x, y = j % w, j // w
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1),
                           (x - 1, y - 1), (x + 1, y - 1), (x - 1, y + 1), (x + 1, y + 1)):
                if 0 <= nx < w and 0 <= ny < h:
                    k = ny * w + nx
                    if mask[k] and not seen[k]:
                        seen[k] = 1
                        stack.append(k)
        found.append((len(cells), cells))
    found.sort(key=lambda c: -c[0])
    return found


def keep_main_blobs(mask, w, h, frac=4):
    """Drop components smaller than 1/`frac` of the largest one."""
    found = blobs(mask, w, h)
    if not found:
        return mask, []
    cutoff = found[0][0] / frac
    out = bytearray(w * h)
    kept = []
    for area, cells in found:
        if area < cutoff:
            continue
        kept.append(area)
        for i in cells:
            out[i] = 1
    return out, kept


def dilate(mask, w, h, n):
    for _ in range(n):
        grown = bytearray(mask)
        for i in range(w * h):
            if not mask[i]:
                continue
            x, y = i % w, i // w
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if 0 <= nx < w and 0 <= ny < h:
                    grown[ny * w + nx] = 1
        mask = grown
    return mask


def bbox(mask, w, h):
    xs = [i % w for i in range(w * h) if mask[i]]
    ys = [i // w for i in range(w * h) if mask[i]]
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


# ---------------------------------------------------------------- lvgl output

def lvgl_i4_bytes(indices, palette, w, h):
    stride = (w + 1) // 2
    body = bytearray()
    for (r, g, b) in palette:
        body += bytes((b, g, r, 0xFF))
    for y in range(h):
        for x in range(0, w, 2):
            hi = indices[y * w + x] & 0x0F
            lo = indices[y * w + x + 1] & 0x0F if x + 1 < w else 0
            body.append((hi << 4) | lo)
    return bytes(body), stride


def emit_c(fh, name, indices, palette, w, h):
    body, stride = lvgl_i4_bytes(indices, palette, w, h)
    fh.write(f"\n// {name}: {w}x{h} I4, {len(body)} bytes "
             f"({N_COLORS * 4} palette + {stride * h} bitmap)\n")
    fh.write(f"static const LV_ATTRIBUTE_LARGE_CONST uint8_t {name}_map[] "
             "__attribute__((aligned(4))) = {\n")
    for i in range(0, len(body), 16):
        fh.write("    " + "".join(f"0x{b:02x}," for b in body[i:i + 16]) + "\n")
    fh.write("};\n\n")
    fh.write(f"const lv_image_dsc_t {name} = {{\n")
    fh.write("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
    fh.write("    .header.cf = LV_COLOR_FORMAT_I4,\n")
    fh.write("    .header.flags = 0,\n")
    fh.write(f"    .header.w = {w},\n")
    fh.write(f"    .header.h = {h},\n")
    fh.write(f"    .header.stride = {stride},\n")
    fh.write(f"    .data_size = sizeof({name}_map),\n")
    fh.write(f"    .data = {name}_map,\n")
    fh.write("};\n")
    return len(body)


def to565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def from565(v):
    """What the panel actually shows for a 565 value, expanded back to 8 bits per channel."""
    r5, g6, b5 = v >> 11, (v >> 5) & 0x3F, v & 0x1F
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def emit_c_rgb565(fh, name, rgb, w, h):
    """Emit `rgb` (flat list of (r,g,b)) as a native little-endian RGB565 lv_image_dsc_t.

    LVGL's built-in bin decoder hands a variable RGB565 image straight to the renderer - the
    pixels are read from flash in place, nothing is copied into RAM.
    """
    body = bytearray()
    for c in rgb:
        v = to565(c)
        body += bytes((v & 0xFF, v >> 8))
    fh.write(f"\n// {name}: {w}x{h} RGB565, {len(body)} bytes\n")
    fh.write(f"static const LV_ATTRIBUTE_LARGE_CONST uint8_t {name}_map[] "
             "__attribute__((aligned(4))) = {\n")
    for i in range(0, len(body), 16):
        fh.write("    " + "".join(f"0x{b:02x}," for b in body[i:i + 16]) + "\n")
    fh.write("};\n\n")
    fh.write(f"const lv_image_dsc_t {name} = {{\n")
    fh.write("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
    fh.write("    .header.cf = LV_COLOR_FORMAT_RGB565,\n")
    fh.write("    .header.flags = 0,\n")
    fh.write(f"    .header.w = {w},\n")
    fh.write(f"    .header.h = {h},\n")
    fh.write(f"    .header.stride = {w * 2},\n")
    fh.write(f"    .data_size = sizeof({name}_map),\n")
    fh.write(f"    .data = {name}_map,\n")
    fh.write("};\n")
    return len(body)
