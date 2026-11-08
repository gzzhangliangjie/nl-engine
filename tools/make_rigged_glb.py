#!/usr/bin/env python3
"""生成最小 rigged GLB（M-A4b 资产）：3 骨骼链 + 单个 sway 旋转动画。

结构（全部数值确定性，供 anim-tools selftest skeleton 手算断言）：
    hip   (translation [0,1,0],  parent=-1)
      spine (translation [0,0.5,0], parent=hip)
        head  (translation [0,0.5,0], parent=spine)

动画 "sway"（duration 2.0s，LINEAR，绕 Z 轴，关键帧 [0, 0.5, 1.0, 1.5, 2.0]）：
    hip :   0deg, +30deg,   0deg, -30deg,   0deg
    spine:  0deg,   0deg, +30deg,   0deg, -30deg   （相位右移一格）
    head :  0deg,   0deg,   0deg, +30deg,   0deg

锚点（selftest 用）：t=0.5 时 hip=+30deg、spine=0、head=0；
    spineWorld = (-0.25, 1.4330127, 0)；headWorld = (-0.5, 1.8660254, 0)。

纯标准库（struct + json + zlib），无网络无第三方。
"""

import json
import math
import struct
import sys
import zlib

DEG = math.radians


def rot_z(deg):
    """绕 Z 轴旋转的四元数 (x, y, z, w)——glTF 顺序。"""
    r = DEG(deg)
    return (0.0, 0.0, math.sin(r / 2.0), math.cos(r / 2.0))


TIMES = [0.0, 0.5, 1.0, 1.5, 2.0]
HIP = [0.0, 30.0, 0.0, -30.0, 0.0]
SPINE = [0.0, 0.0, 30.0, 0.0, -30.0]
HEAD = [0.0, 0.0, 0.0, 30.0, 0.0]


def build_gltf():
    binary = bytearray()
    views = []      # (byteOffset, byteLength) -> accessor 逐个建 view
    accessors = []

    def add_f32(data, components):
        blob = struct.pack(f"<{len(data)}f", *data)
        view = {
            "buffer": 0,
            "byteOffset": len(binary),
            "byteLength": len(blob),
        }
        binary.extend(blob)
        # 4 字节对齐（GLB 规范）
        if len(binary) % 4:
            binary.extend(b"\x00" * (4 - len(binary) % 4))
        view_index = len(views)
        views.append(view)
        accessor = {
            "bufferView": view_index,
            "componentType": 5126,  # FLOAT
            "count": len(data) // components,
            "type": {1: "SCALAR", 3: "VEC3", 4: "VEC4"}[components],
        }
        accessors.append(accessor)
        return len(accessors) - 1

    t_acc = add_f32(TIMES, 1)
    hip_acc = add_f32([v for d in HIP for v in rot_z(d)], 4)
    spine_acc = add_f32([v for d in SPINE for v in rot_z(d)], 4)
    head_acc = add_f32([v for d in HEAD for v in rot_z(d)], 4)

    def sampler(output):
        return {"input": t_acc, "interpolation": "LINEAR", "output": output}

    def channel(node, path, sampler_index):
        return {
            "sampler": sampler_index,
            "target": {"node": node, "path": path},
        }

    gltf = {
        "asset": {"version": "2.0", "generator": "nl-engine anim-tools fixture"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "hip", "translation": [0.0, 1.0, 0.0], "rotation": rot_z(0), "children": [1]},
            {"name": "spine", "translation": [0.0, 0.5, 0.0], "rotation": rot_z(0), "children": [2]},
            {"name": "head", "translation": [0.0, 0.5, 0.0], "rotation": rot_z(0)},
        ],
        "animations": [
            {
                "name": "sway",
                "samplers": [sampler(hip_acc), sampler(spine_acc), sampler(head_acc)],
                "channels": [
                    channel(0, "rotation", 0),
                    channel(1, "rotation", 1),
                    channel(2, "rotation", 2),
                ],
            }
        ],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    return gltf, bytes(binary)


def pack_glb(gltf, binary):
    json_blob = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    if len(json_blob) % 4:
        json_blob += b" " * (4 - len(json_blob) % 4)
    total = 12 + 8 + len(json_blob) + 8 + len(binary)
    out = struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<I4s", len(json_blob), b"JSON") + json_blob
    out += struct.pack("<I4s", len(binary), b"BIN\x00") + binary
    return out


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "assets/models/RiggedChain.glb"
    gltf, binary = build_gltf()
    blob = pack_glb(gltf, binary)
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"rigged GLB written: {out_path} ({len(blob)} bytes, magic=glTF, bones=3, clips=1)")


if __name__ == "__main__":
    main()
