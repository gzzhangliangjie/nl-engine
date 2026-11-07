#!/usr/bin/env python3
"""M3.2 visual evidence: dag-top drill gallery from real viewer screenshots.

Tiles arbitrary already-captured GPU screenshots with one-line captions. The
pixels are genuine viewer renders (--mode dag-top, deterministic pipeline);
nothing is drawn by hand.

Usage:
    python tools/dag_gallery.py \
        --tile "top L6 (104 tri):example/s3-done/dag-top-dx12.png" \
        --tile "drill g0 (top-1 parent -> 5 children):example/s3-done/dag-drill-g0-dx12.png" \
        ... --out example/s3-done/dag-drills.png
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw

from qem_preview import BG, MUTED, TEXT, font


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tile", action="append", required=True,
                    help="caption:path, repeatable; caption shown under each tile")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    tiles = []
    for spec in args.tile:
        caption, _, path = spec.rpartition(":")
        p = Path(path)
        if not p.is_file():
            raise SystemExit(f"missing screenshot: {p}")
        tiles.append((caption, Image.open(p).convert("RGB")))

    w, h = tiles[0][1].size
    pad, cap_h = 12, 26
    cols = len(tiles)
    canvas = Image.new("RGB", (cols * w + (cols + 1) * pad,
                               h + pad * 2 + cap_h), BG)
    draw = ImageDraw.Draw(canvas)
    for i, (caption, im) in enumerate(tiles):
        x = pad + i * (w + pad)
        canvas.paste(im, (x, pad))
        draw.text((x + 4, pad + h + 4), caption, fill=TEXT, font=font(16))
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out)
    print(f"wrote {out} ({canvas.size[0]}x{canvas.size[1]}, {len(tiles)} tiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
