#!/usr/bin/env python3
"""Embed external-uri base color textures into a glb as a bufferView.

【M-W3.0 2026-11-14】kenney car-kit 的 cinevva 单文件 glb 引用外部
`Textures/colormap.png`（原包分离结构），而引擎 mesh_io/tinygltf 不加载
外部文件（image.image 空 -> "not 8-bit decoded RGBA-capable data"）。本工具
把 colormap.png 追加进 BIN chunk 并改写 images 为嵌入式 bufferView——资产端
一次性适配，引擎零改动（Q-W3A 拍板 A1 精神：装配层消化资产形态差异）。

colormap.png 来源：Kenney Car Kit 3.1 官方包（CC0），经 GitHub sz-games
镜像 games/infinite-road/models/Textures/colormap.png 获取（512x512 8-bit
RGBA PNG，与 vendored glb 的 UV 采样对齐——同包同图，逐字节原样嵌入）。

用法:
    python tools/embed_glb_textures.py <glb> <png> [<glb> <png> ...]
"""
import json
import struct
import sys
from pathlib import Path


def embed(glb_path: Path, png_path: Path) -> None:
    data = glb_path.read_bytes()
    magic, version, total = struct.unpack_from("<4sII", data, 0)
    assert magic == b"glTF" and version == 2, "not a glb v2"
    jlen, jtype = struct.unpack_from("<II", data, 12)
    assert jtype == 0x4E4F534A, "first chunk is not JSON"
    js = json.loads(data[20:20 + jlen])
    blen, btype = struct.unpack_from("<II", data, 20 + jlen)
    assert btype == 0x004E4942, "second chunk is not BIN"
    bin_data = bytearray(data[20 + jlen + 8:20 + jlen + 8 + blen])

    images = js.get("images", [])
    if not images:
        print("  %s: no images, skipped" % glb_path.name)
        return
    png = png_path.read_bytes()
    for img in images:
        uri = img.get("uri")
        if uri is None:
            continue  # already embedded
        # pad BIN to 4-byte alignment, append the png, register a bufferView
        while len(bin_data) % 4:
            bin_data.append(0)
        bv_index = len(js.get("bufferViews", []))
        js.setdefault("bufferViews", []).append({
            "buffer": 0,
            "byteOffset": len(bin_data),
            "byteLength": len(png),
        })
        bin_data += png
        del img["uri"]
        img["bufferView"] = bv_index
        img["mimeType"] = "image/png"

    js["buffers"][0]["byteLength"] = len(bin_data)
    new_json = json.dumps(js, separators=(",", ":")).encode("utf-8")
    json_pad = (-len(new_json)) % 4
    new_json += b" " * json_pad
    bin_pad = (-len(bin_data)) % 4
    bin_data += b"\x00" * bin_pad

    header_len = 12
    out_len = (header_len + 8 + len(new_json) + 8 + len(bin_data))
    out = b"glTF" + struct.pack("<II", 2, out_len)
    out += struct.pack("<I", len(new_json)) + b"JSON" + new_json
    out += struct.pack("<I", len(bin_data)) + b"BIN\0" + bytes(bin_data)
    glb_path.write_bytes(out)
    print("  %s: %d image(s) embedded (+%d B, total %d B)"
          % (glb_path.name, len(images), len(png), out_len))


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if not argv or len(argv) % 2:
        print(__doc__)
        return 2
    for i in range(0, len(argv), 2):
        embed(Path(argv[i]), Path(argv[i + 1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
