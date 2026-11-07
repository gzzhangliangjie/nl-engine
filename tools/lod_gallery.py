#!/usr/bin/env python3
"""M2.3 visual evidence: gallery of the *real* viewer screenshots, one per LOD level.

Each tile is a genuine GPU render (viewer --mode lod --lod N, deterministic
screenshot pipeline, both backends byte-stable) pasted side by side with the
level number, triangle count and parent maxError read back from the .nmesh v2
payload. No mockups: the pixels come from the viewer, the labels from the
binary assets.

Usage:
    python tools/lod_gallery.py --prefix assets/meshlets/Suzanne \
        --pattern "example/s2-done/lod-l{n}-dx12.png" --out example/s2-done/lod-layers.png
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw

from qem_preview import BG, MUTED, TEXT, font


def read_level_meta(path: Path):
    """Return (level, tri_count, max_error) from a v1/v2 .nmesh file."""
    blob = path.read_bytes()
    magic, version, name_len, meshlet_count, vertex_count, tri_count = \
        struct.unpack_from("<IHHIII", blob, 0)
    if magic != 0x48534D4E or version not in (1, 2):
        raise SystemExit(f"not a v1/v2 .nmesh: {path}")
    level, max_error = 0, 0.0
    if version == 2:
        cur = 20 + name_len + meshlet_count * 52 + vertex_count * 32 + tri_count * 3
        mg, _payload, _mv, lvl, err, _locked, _r = struct.unpack_from("<IIHHfII", blob, cur)
        if mg != 0x444F4C4E:
            raise SystemExit(f"v2 meta section missing in {path}")
        level, max_error = int(lvl), float(err)
    return level, int(tri_count), max_error


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", required=True, help=".nmesh prefix, e.g. assets/meshlets/Suzanne")
    ap.add_argument("--pattern", required=True, help="screenshot pattern with {n}, e.g. example/s2-done/lod-l{n}-dx12.png")
    ap.add_argument("--out", required=True)
    ap.add_argument("--levels", default="", help="comma list; default = auto-detect from prefix")
    args = ap.parse_args()

    prefix = Path(args.prefix)
    levels = [int(x) for x in args.levels.split(",") if x.strip()]
    if not levels:
        n = 0
        while prefix.with_name(prefix.name + f".lod{n}.nmesh").is_file():
            levels.append(n)
            n += 1
    if not levels:
        raise SystemExit(f"no layered files for prefix {prefix}")

    tiles = []
    for lv in levels:
        shot = Path(args.pattern.format(n=lv))
        if not shot.is_file():
            raise SystemExit(f"missing screenshot {shot}")
        meta_p = Path(str(prefix) + f".lod{lv}.nmesh")
        _level, tris, err = read_level_meta(meta_p)
        img = Image.open(shot).convert("RGB")
        tiles.append((img, lv, tris, err))

    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    tw, th = tiles[0][0].size
    gap, cap = 16, 40
    top = 64
    W = cols * tw + (cols + 1) * gap
    H = top + rows * (th + cap) + (rows + 1) * gap
    canvas = Image.new("RGB", (W, H), BG)
    draw = ImageDraw.Draw(canvas)
    draw.text((gap + 4, 18), f"nl-engine M2.3  viewer --mode lod N  (Suzanne, one camera, {len(tiles)} layers)",
              fill=TEXT, font=font(22))

    for i, (img, lv, tris, err) in enumerate(tiles):
        r, c = divmod(i, cols)
        x = gap + c * (tw + gap)
        y = top + gap + r * (th + cap + gap)
        draw.text((x + 2, y - 2), f"L{lv}   {tris} tri   maxErr {err:.4f} (world)",
                  fill=MUTED, font=font(16))
        canvas.paste(img, (x, y + cap - 8))
    canvas.save(args.out)
    print(f"wrote {args.out} ({len(tiles)} tiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
