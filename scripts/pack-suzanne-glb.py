#!/usr/bin/env python3
"""Pack Khronos glTF-Sample-Models Suzanne (glTF + bin + 2 PNGs) into one GLB.

Source files (downloaded from gh-proxy mirror, zero vendor SDK / zero runtime network):
  https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/Suzanne/glTF
  Suzanne.gltf, Suzanne.bin, Suzanne_BaseColor.png, Suzanne_MetallicRoughness.png

Usage:
  python scripts/pack-suzanne-glb.py <src-dir> <output.glb>

The GLB keeps the original geometry/UV/baseColor intact; images become bufferViews
inside the single BIN chunk, so the runtime only needs one path and no CWD assumptions.
"""

import json
import struct
import sys
from pathlib import Path


def pad4(data: bytes, fill: bytes) -> bytes:
    return data + fill * ((4 - len(data) % 4) % 4)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    doc = json.loads((src / "Suzanne.gltf").read_text(encoding="utf-8"))
    geo = bytearray((src / "Suzanne.bin").read_bytes())

    # The one glTF buffer becomes the GLB BIN chunk (drop its external uri).
    doc["buffers"] = [{"byteLength": len(geo)}]

    for image in doc.get("images", []):
        uri = image.pop("uri")
        pixels = (src / uri).read_bytes()
        while len(geo) % 4:
            geo.append(0)
        offset = len(geo)
        view_index = len(doc["bufferViews"])
        doc["bufferViews"].append(
            {"buffer": 0, "byteOffset": offset, "byteLength": len(pixels)}
        )
        geo.extend(pixels)
        image["bufferView"] = view_index
        image["mimeType"] = "image/png"

    bin_bytes = pad4(bytes(geo), b"\x00")
    doc["buffers"][0]["byteLength"] = len(bin_bytes)
    json_bytes = pad4(
        json.dumps(doc, separators=(",", ":"), ensure_ascii=False).encode("utf-8"), b" "
    )

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))       # 'glTF', v2
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))  # 'JSON'
        f.write(json_bytes)
        f.write(struct.pack("<II", len(bin_bytes), 0x004E4942))   # 'BIN\0'
        f.write(bin_bytes)

    print(f"wrote {out} ({total} bytes): {len(doc['images'])} images, "
          f"{len(doc['bufferViews'])} bufferViews, "
          f"{doc['accessors'][0]['count']} indices")
    return 0


if __name__ == "__main__":
    sys.exit(main())
