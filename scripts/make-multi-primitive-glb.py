#!/usr/bin/env python3
"""Make a multi-primitive GLB from an existing single-mesh GLB (P8 M-J2 test asset).

Copies mesh primitives N times (same accessors, distinct primitive entries), so
`nanite-tools cluster` sees multiple submeshes and exercises its parallel path.
Only the JSON chunk changes; the BIN chunk is untouched.

Usage:
  python scripts/make-multi-primitive-glb.py <in.glb> <out.glb> <copies>
"""

import json
import struct
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    src, dst, copies = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])

    raw = src.read_bytes()
    magic, version, length = struct.unpack_from("<III", raw, 0)
    assert magic == 0x46546C67, "not a GLB"
    json_len, json_type = struct.unpack_from("<II", raw, 12)
    assert json_type == 0x4E4F534A, "first chunk is not JSON"
    doc = json.loads(raw[20 : 20 + json_len].decode("utf-8"))

    for mesh in doc.get("meshes", []):
        base = list(mesh["primitives"])
        for _ in range(copies - 1):
            prim = dict(base[0])
            mesh["primitives"].append(prim)

    json_bytes = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    bin_bytes = raw[20 + json_len :]
    bin_len = struct.unpack_from("<I", bin_bytes, 0)[0]
    bin_chunk = bin_bytes[8 : 8 + bin_len]
    bin_chunk += b"\x00" * ((4 - len(bin_chunk) % 4) % 4)

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_chunk)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A)
    out += json_bytes
    out += struct.pack("<II", len(bin_chunk), 0x004E4942)
    out += bin_chunk
    dst.write_bytes(bytes(out))
    print(f"wrote {dst} ({copies} primitives per mesh, {total} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
