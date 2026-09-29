#!/usr/bin/env python3
"""Exports the face assets the firmware is built with, for the browser previewer (index.html).

Reads firmware/main/face_assets_*.c - the exact bytes that get flashed - plus the animation tuning
tables in firmware/main/display.cpp (ear scripts, mood poses, ear spring constants), and writes
data/<character>.js and data/manifest.js next to this file. index.html loads those with plain
<script> tags, so it opens straight from disk (file://): no server, no network, nothing to install.

    python3 tools/face_preview/export.py          # then open tools/face_preview/index.html

Run it again after rebuilding the assets (tools/face_assets/build_face_assets.py) or retuning the
tables in display.cpp.
"""

import base64
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
MAIN = os.path.join(ROOT, "firmware", "main")
OUT = os.path.join(HERE, "data")


def b64(data):
    return base64.b64encode(bytes(data)).decode("ascii")


def parse_assets(path):
    src = open(path, encoding="utf-8").read()
    cid = re.search(r'const enco_character_t enco_char_(\w+) = \{', src).group(1)

    # Image pixel data (hex bytes) and their descriptors.
    maps = {m.group(1): bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
            for m in re.finditer(r"uint8_t (\w+)_map\[\][^=]*=\s*\{(.*?)\};", src, re.S)}
    images = {}
    for m in re.finditer(r"const lv_image_dsc_t (\w+) = \{(.*?)\};", src, re.S):
        name, body = m.group(1), m.group(2)
        f = dict(re.findall(r"\.header\.(\w+) = (\w+)", body))
        images[name] = {"w": int(f["w"]), "h": int(f["h"]),
                        "cf": "rgb565a8" if "A8" in f["cf"] else "rgb565",
                        "data": b64(maps[name])}

    # Plain numeric tables (ear maps, hair-flow tables).
    arrays = {}
    for m in re.finditer(r"static const (u?int(?:8|16)_t) (\w+)\[\d+\] = \{(.*?)\};", src, re.S):
        arrays[m.group(2)] = [int(v) for v in re.findall(r"-?\d+", m.group(3))]

    def ref(v):
        v = v.strip().lstrip("&")
        return None if v == "NULL" else v

    def image_ref(v):
        r = ref(v)
        return None if r is None else images[r]

    # Sprite tables: expressions, petals, hair-breeze levels.
    exprs = [{"name": n, "eyes": images[e], "mouth": images[mo]}
             for n, e, mo in re.findall(r'\{"(\w+)", &(\w+), &(\w+)\}', src)]
    tables = {m.group(1): [ref(v) for v in m.group(2).split(",") if v.strip()]
              for m in re.finditer(r"static const lv_image_dsc_t\* const (\w+)\[\d+\] = \{(.*?)\};", src, re.S)}

    chr_body = re.search(r"const enco_character_t enco_char_\w+ = \{(.*?)\n\};", src, re.S).group(1)
    ch = dict((k, v.strip()) for k, v in re.findall(r"\.(\w+) = ([^,\n]+)", chr_body))
    num = lambda k: int(ch[k], 0)

    def sprite_table(k):
        t = ref(ch.get(k, "NULL"))
        return None if t is None else [None if n is None else images[n] for n in tables[t]]

    flow = None
    if ref(ch.get("hair_flow", "NULL")):
        body = re.search(r"static const enco_hair_flow_t \w+ = \{(.*?)\};", src, re.S).group(1)
        f = dict((k, v.strip()) for k, v in re.findall(r"\.(\w+) = ([^,\n]+)", body))
        flow = {"y0": int(f["y0"]), "rows": int(f["rows"]), "amp_unit": int(f["amp_unit"]),
                "feather": int(f["feather"]), "amp": arrays[f["amp"]], "spans": arrays[f["spans"]]}

    ears = []
    if ref(ch.get("ears", "NULL")):
        body = re.search(r"static const enco_ear_t \w+\[\d+\] = \{(.*?)\n\};", src, re.S).group(1)
        for e in re.findall(r"\{(\.box_x.*?deco_map = \w+)\}", body, re.S):
            f = dict((k, v.strip()) for k, v in re.findall(r"\.(\w+) = (-?\w+)", e))
            deco = [list(map(int, d)) for d in
                    re.findall(r"\{(-?\d+), (-?\d+), (\d+), (\d+), (\d+)\}", e.split(".deco =")[1])]
            ears.append({k: int(f[k]) for k in ("box_x", "box_y", "box_w", "box_h", "pivot_x", "pivot_y",
                                               "tip_x", "tip_y", "out_sign")}
                        | {"deco": [dict(zip(("x", "y", "hang", "hz10", "zeta100"), d)) for d in deco],
                           "weight": b64(arrays[f["weight"]]), "deco_map": b64(arrays[f["deco_map"]])})

    petals = sprite_table("petals")
    return cid, {
        "id": cid,
        "bg_color": num("bg_color"),
        "base": image_ref(ch["base"]),
        "eyes_x": num("eyes_x"), "eyes_y": num("eyes_y"),
        "mouth_x": num("mouth_x"), "mouth_y": num("mouth_y"),
        "eyes_half": image_ref(ch["eyes_half"]), "eyes_shut": image_ref(ch["eyes_shut"]),
        "mouth_small": image_ref(ch["mouth_small"]), "mouth_wide": image_ref(ch["mouth_wide"]),
        "exprs": exprs,
        "bangs_x": num("bangs_x"), "bangs_y": num("bangs_y"),
        "locks_l_x": num("locks_l_x"), "locks_l_y": num("locks_l_y"),
        "locks_r_x": num("locks_r_x"), "locks_r_y": num("locks_r_y"),
        "bangs": sprite_table("bangs"), "locks_l": sprite_table("locks_l"), "locks_r": sprite_table("locks_r"),
        "petals": petals, "petal_frames": num("petal_frames"), "petal_sizes": num("petal_sizes"),
        "petal_lane_l": num("petal_lane_l"), "petal_lane_r": num("petal_lane_r"),
        "hair_flow": flow,
        "ears": ears,
    }


def parse_display():
    """The ear tuning tables, so the preview moves exactly like the firmware does."""
    src = open(os.path.join(MAIN, "display.cpp"), encoding="utf-8").read()
    scripts = {}
    for m in re.finditer(r"static const EarStep (kEar\w+)\[\] = \{(.*?)\};", src, re.S):
        scripts[m.group(1)] = [list(map(int, s)) for s in
                               re.findall(r"\{(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\}", m.group(2))]
    poses = {n: [int(a), int(b), int(t)] for n, a, b, t in
             re.findall(r'\{"(\w+)",\s*(-?\d+),\s*(-?\d+),\s*(\d+)\}',
                        re.search(r"kEarPoses\[\] = \{(.*?)\};", src, re.S).group(1))}
    consts = {k: float(v) for k, v in re.findall(r"(kEar\w+) = ([\d.]+)f", src)}
    return {"ear_scripts": scripts, "ear_poses": poses, "ear_consts": consts}


def main():
    os.makedirs(OUT, exist_ok=True)
    ids = []
    for path in sorted(glob.glob(os.path.join(MAIN, "face_assets_*.c"))):
        cid, data = parse_assets(path)
        with open(os.path.join(OUT, f"{cid}.js"), "w") as fh:
            fh.write("// Generated by tools/face_preview/export.py from " + os.path.basename(path) + "\n"
                     "window.ENCO_CHARS = window.ENCO_CHARS || {};\n"
                     f"window.ENCO_CHARS[{json.dumps(cid)}] = " + json.dumps(data, separators=(",", ":")) + ";\n")
        ids.append(cid)
        print(f"{cid}: {len(data['exprs'])} expressions, {len(data['ears'])} ears, "
              f"hair flow {'yes' if data['hair_flow'] else 'no'}, petals {'yes' if data['petals'] else 'no'}")
    # Same order as kCharacters in display.cpp: fox first (the default), then k3.
    order = {"fox": 0, "k3": 1}
    ids.sort(key=lambda c: order.get(c, 9))
    with open(os.path.join(OUT, "manifest.js"), "w") as fh:
        fh.write("// Generated by tools/face_preview/export.py\n"
                 "window.ENCO_MANIFEST = " + json.dumps({"characters": ids} | parse_display(), indent=1) + ";\n")
    print(f"wrote {OUT}; open {os.path.join(HERE, 'index.html')}")


if __name__ == "__main__":
    sys.exit(main())
