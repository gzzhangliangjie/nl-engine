#!/usr/bin/env python3
"""Make a multi-material GLB from an existing single-material GLB (M7.2 test asset).

Clones the single primitive N times as N separate meshes/nodes with distinct
materials: per-copy baseColor/MR texture entries (referencing the same image
bufferViews, so the BIN chunk is untouched) plus distinct baseColorFactor /
metallicFactor / roughnessFactor. Nodes get X translations so the copies sit
side by side (the engine bakes node TRS into vertices at load).

`nanite-tools cluster -o` sees N submeshes and writes one multi-submesh .nmesh
('NSUB' tail section, material id = submesh index).

Usage:
  python scripts/make-multi-material-glb.py <in.glb> <out.glb> <copies>
"""

import json
import struct
import sys
from pathlib import Path

# 每份拷贝的材质差异：baseColorFactor 调色 + PBR 因子档位，保证 3 材质在
# mode 0（文本化）与 mode 5（PBR 直光）下都肉眼可辨。
COPY_STYLES = [
    {"tint": [1.00, 0.25, 0.20, 1.0], "metallic": 1.0, "roughness": 0.15},  # 抛光金属红
    {"tint": [0.25, 1.00, 0.35, 1.0], "metallic": 0.0, "roughness": 0.85},  # 哑光粗糙绿
    {"tint": [0.30, 0.55, 1.00, 1.0], "metallic": 0.5, "roughness": 0.45},  # 半金属蓝
]
COPY_SPACING_X = 2.6  # Suzanne 包围盒 ~2.3 宽，2.6 间距无重叠


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    src, dst, copies = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])
    if copies < 1 or copies > len(COPY_STYLES):
        print(f"copies must be 1..{len(COPY_STYLES)}")
        return 2

    raw = src.read_bytes()
    magic, version, length = struct.unpack_from("<III", raw, 0)
    assert magic == 0x46546C67, "not a GLB"
    json_len, json_type = struct.unpack_from("<II", raw, 12)
    assert json_type == 0x4E4F534A, "first chunk is not JSON"
    doc = json.loads(raw[20 : 20 + json_len].decode("utf-8"))

    assert len(doc.get("scenes", [])) == 1, "expect single scene"
    assert len(doc.get("meshes", [])) == 1, "expect single mesh"
    base_mesh = doc["meshes"][0]
    assert len(base_mesh["primitives"]) == 1, "expect single primitive"
    base_prim = base_mesh["primitives"][0]
    assert len(doc.get("materials", [])) == 1, "expect single material"
    pbr = doc["materials"][0]["pbrMetallicRoughness"]
    base_bc_img = doc["textures"][pbr["baseColorTexture"]["index"]]["source"]
    base_mr_img = doc["textures"][pbr["metallicRoughnessTexture"]["index"]]["source"]
    # 清空前先抓住两张原图的 bufferView 号（拷贝的 image 条目复用它们，BIN 不动）
    bc_bv = doc["images"][base_bc_img]["bufferView"]
    mr_bv = doc["images"][base_mr_img]["bufferView"]

    doc["meshes"] = []
    doc["materials"] = []
    doc["textures"] = []
    doc["images"] = []
    doc["nodes"] = []
    scene_nodes = []

    for c in range(copies):
        style = COPY_STYLES[c]
        # 贴图：每份拷贝自己的 texture/image 条目 → 材质间接表里贴图槽号真实互异
        bc_image = len(doc["images"])
        doc["images"].append({"bufferView": bc_bv, "mimeType": "image/png"})
        mr_image = len(doc["images"])
        doc["images"].append({"bufferView": mr_bv, "mimeType": "image/png"})
        bc_tex = len(doc["textures"])
        doc["textures"].append({"sampler": 0, "source": bc_image})
        mr_tex = len(doc["textures"])
        doc["textures"].append({"sampler": 0, "source": mr_image})

        doc["materials"].append({
            "name": f"mat{c}",
            "pbrMetallicRoughness": {
                "baseColorFactor": style["tint"],
                "baseColorTexture": {"index": bc_tex},
                "metallicFactor": style["metallic"],
                "roughnessFactor": style["roughness"],
                "metallicRoughnessTexture": {"index": mr_tex},
            },
        })

        doc["meshes"].append({
            "name": f"Suzanne#{c}",
            "primitives": [dict(base_prim, material=c)],
        })
        node = {"mesh": c, "name": f"Suzanne#{c}",
                "translation": [c * COPY_SPACING_X - (copies - 1) * COPY_SPACING_X / 2, 0, 0]}
        scene_nodes.append(len(doc["nodes"]))
        doc["nodes"].append(node)

    doc["scenes"][0]["nodes"] = scene_nodes

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
    print(f"wrote {dst} ({copies} meshes/materials, {total} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
