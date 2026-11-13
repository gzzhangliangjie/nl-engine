#!/usr/bin/env python3
"""FBX -> self-contained GLB via headless Blender (P9 V1 asset-ingest warm-up).

Run by Blender itself (NOT plain python):

  blender --background --python scripts/fbx2glb.py -- <in.fbx> <out.glb> [--report out.json]

Why Blender
-----------
The mesh entry point of nl-engine is glTF 2.0 only (tinygltf). Source FBX files
are binary Kaydara; Blender 5.2 is already installed locally and its FBX
importer + glTF exporter handle binary FBX, Principled PBR maps and texture
embedding with zero new repository dependencies. This script is tooling only --
nothing here is vendored or linked into the engine.

What it does
------------
* fresh empty scene, imports one FBX
* reports object / triangle / material counts and which image files each
  material references (the measured constraint for the kMaxVisMaterials=4
  budget is per-asset; composition goes through ECS scene JSON placement)
* exports one GLB with textures embedded (export_image_format='AUTO')

Exit code 0 on success; the JSON report on stdout's last line is the contract
consumed by the batch evidence script.
"""

import json
import os
import struct
import sys

import bpy


def clean_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_fbx(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    return [o for o in bpy.data.objects if o not in before]


def mesh_stats(objects):
    tris = 0
    meshes = []
    for ob in objects:
        if ob.type != "MESH":
            continue
        me = ob.data
        tris += len(me.loop_triangles) if me.loop_triangles else sum(
            max(len(p.vertices) - 2, 0) for p in me.polygons)
        meshes.append(ob)
    return meshes, tris


def material_report(meshes):
    report = []
    seen = set()
    for ob in meshes:
        for slot in ob.material_slots:
            mat = slot.material
            if mat is None or mat.name in seen:
                continue
            seen.add(mat.name)
            images = []
            if mat.use_nodes:
                for node in mat.node_tree.nodes:
                    if node.type == "TEX_IMAGE" and node.image:
                        images.append(os.path.basename(node.image.filepath)
                                      or node.image.name)
            report.append({"material": mat.name, "images": sorted(set(images))})
    return report


def bbox(objects):
    lo = [1e30] * 3
    hi = [-1e30] * 3
    any_vert = False
    for ob in objects:
        if ob.type != "MESH":
            continue
        for corner in ob.bound_box:
            world = ob.matrix_world @ __import__("mathutils").Vector(corner)
            any_vert = True
            for i in range(3):
                lo[i] = min(lo[i], world[i])
                hi[i] = max(hi[i], world[i])
    if not any_vert:
        return None
    return {"min": lo, "max": hi,
            "size": [hi[i] - lo[i] for i in range(3)]}


IMAGE_EXTS = (".png", ".jpg", ".jpeg")

# Preference order when resolving a referenced texture to a shipped file.
PREF_EXTS = (".png", ".jpg", ".jpeg", ".tga")


def retarget_images(maps_dir):
    """FBX materials usually reference dead absolute paths (.psd on the
    author's machine). Re-point every image at a shipped file under
    ``maps_dir`` matched by basename stem (exact, then prefix), preferring
    embeddable formats (png/jpg/tga). Returns a list of retarget notes."""
    if not maps_dir:
        return []
    files = []
    for root, _dirs, names in os.walk(maps_dir):
        for n in names:
            ext = os.path.splitext(n)[1].lower()
            if ext in PREF_EXTS:
                files.append(os.path.join(root, n))
    by_stem = {}
    for f in files:
        by_stem.setdefault(os.path.splitext(os.path.basename(f))[0].lower(), []).append(f)
    notes = []
    for img in bpy.data.images:
        raw = img.filepath_raw or ""
        stem = os.path.splitext(os.path.basename(raw))[0].lower()
        if not stem or raw.lower().endswith(IMAGE_EXTS):
            if raw:  # already an embeddable format
                continue
        cand = by_stem.get(stem)
        if not cand:  # fuzzy: shipped file starts with the referenced stem
            pref = sorted(f for s, fs in by_stem.items()
                          if s.startswith(stem) for f in fs)
            cand = pref
        if not cand:
            notes.append({"ref": os.path.basename(raw), "resolved": None})
            continue
        order = {ext: i for i, ext in enumerate(PREF_EXTS)}
        best = sorted(cand, key=lambda f: order.get(os.path.splitext(f)[1].lower(), 99))[0]
        img.filepath = best
        try:
            img.reload()
        except Exception:
            pass
        notes.append({"ref": os.path.basename(raw),
                      "resolved": os.path.basename(best)})
    return notes


def order_materials_textured_first(meshes):
    """Engine convention (scene.cpp M0.4/M7.2): primitive 0 must carry a base
    colour texture. Blender writes glTF primitives in material-slot order, so
    per mesh, move slots whose material has an image in front, remapping
    polygon material_index accordingly. Returns count of meshes reordered."""
    moved = 0
    for ob in meshes:
        me = ob.data
        mats = list(me.materials)
        if len(mats) < 2 or any(m is None for m in mats):
            continue
        has_img = [bool(m.use_nodes and any(
            n.type == "TEX_IMAGE" and n.image for n in m.node_tree.nodes))
            for m in mats]
        if has_img[0] or not any(has_img):
            continue
        order = sorted(range(len(mats)), key=lambda i: (not has_img[i], i))
        remap = {old: new for new, old in enumerate(order)}
        me.materials.clear()
        for old in order:
            me.materials.append(mats[old])
        for p in me.polygons:
            p.material_index = remap[p.material_index]
        moved += 1
    return moved


def reorder_glb_nodes_textured_first(path):
    """Post-export GLB rewrite: within every scene/nodes children array, stably
    move nodes carrying a textured mesh to the front. The engine's
    loadGltfScene visits nodes depth-first (mesh_io.cpp visitNode) and requires
    primitive 0 to have a base colour texture, so traversal order decides prim0.
    JSON-only rewrite: node indices and the BIN chunk stay untouched."""
    with open(path, "rb") as fh:
        data = fh.read()
    magic, version, _length = struct.unpack("<III", data[:12])
    jlen, jtype = struct.unpack("<II", data[12:20])
    j = json.loads(data[20:20 + jlen])
    blen, btype = struct.unpack("<II", data[20 + jlen:28 + jlen])
    binchunk = data[28 + jlen:28 + jlen + blen]

    textured_meshes = set()
    for mi, mesh in enumerate(j.get("meshes", [])):
        for prim in mesh.get("primitives", []):
            mid = prim.get("material")
            if mid is not None and mid < len(j.get("materials", [])):
                pbr = j["materials"][mid].get("pbrMetallicRoughness", {})
                if pbr.get("baseColorTexture") is not None:
                    textured_meshes.add(mi)
                    break

    moved = 0

    def textured_first(order):
        nonlocal moved
        key = [0 if j["nodes"][ni].get("mesh") in textured_meshes else 1
               for ni in order]
        if key != sorted(key):
            moved += 1
        return [ni for _, ni in sorted(zip(key, order), key=lambda t: t[0])]
    # stable: sorted() keeps original relative order inside each class

    for sc in j.get("scenes", []):
        sc["nodes"] = textured_first(sc.get("nodes", []))
    for nd in j.get("nodes", []):
        if "children" in nd:
            nd["children"] = textured_first(nd["children"])

    outj = json.dumps(j, separators=(",", ":")).encode()
    outj += b" " * ((4 - len(outj) % 4) % 4)
    total = 12 + 8 + len(outj) + 8 + len(binchunk)
    with open(path, "wb") as fh:
        fh.write(struct.pack("<III", magic, version, total))
        fh.write(struct.pack("<I", len(outj)) + struct.pack("<I", 0x4E4F534A))
        fh.write(outj)
        fh.write(struct.pack("<I", blen) + struct.pack("<I", btype) + binchunk)
    return moved, len(textured_meshes)


def prune_objects(name_keys, report):
    """Delete objects whose name starts with one of ``name_keys``
    (case-insensitive; e.g. collision proxies '..._Col', duplicate 'LOD1'
    subtrees). Recurses into children of removed empties."""
    keys = [k.lower() for k in name_keys]
    if not keys:
        return 0
    doomed = []
    for ob in list(bpy.data.objects):
        nm = ob.name.lower()
        if any(nm.startswith(k) for k in keys):
            doomed.append(ob)
    count = 0
    for ob in doomed:
        nm = ob.name  # capture before removal (RNA dies with the object)

        def kill(o):
            nonlocal count
            for c in list(o.children):
                kill(c)
            bpy.data.objects.remove(o, do_unlink=True)
            count += 1
        kill(ob)
        report.append(nm)
    return count


def split_by_material(src, outdir, stem, maps_dir, prune_keys, results):
    """ECS scene mode loads only primitive 0 of each glb (M-E1b.0), so scene
    composition needs ONE primitive per glb. Re-import the FBX once per
    material, keep only meshes using it, clear+bake transforms, join into a
    single mesh object, export <stem>_<material>.glb."""
    import mathutils

    # one pass to enumerate materials
    clean_scene()
    import_fbx(src)
    prune_objects(prune_keys, [])
    mat_names = []
    for ob in bpy.data.objects:
        if ob.type != "MESH":
            continue
        for slot in ob.material_slots:
            if slot.material and slot.material.name not in mat_names:
                mat_names.append(slot.material.name)

    os.makedirs(outdir, exist_ok=True)
    for mat_name in mat_names:
        clean_scene()
        import_fbx(src)
        pruned = []
        prune_objects(prune_keys, pruned)
        retarget_images(maps_dir)

        keep = []
        for ob in list(bpy.data.objects):
            if ob.type != "MESH":
                continue
            slots = [s.material.name if s.material else None
                     for s in ob.material_slots]
            if mat_name not in slots:
                bpy.data.objects.remove(ob, do_unlink=True)
                continue
            # keep only faces whose slot is the target material; drop the rest
            import bmesh
            me = ob.data
            bm = bmesh.new()
            bm.from_mesh(me)
            bm.faces.ensure_lookup_table()
            doomed = [f for f in bm.faces
                      if slots[f.material_index] != mat_name]
            if doomed:
                bmesh.ops.delete(bm, geom=doomed, context="FACES")
            bm.to_mesh(me)
            bm.free()
            me.materials.clear()
            me.materials.append(bpy.data.materials[mat_name])
            for p in me.polygons:
                p.material_index = 0
            keep.append(ob)
        if not keep:
            continue

        bpy.ops.object.select_all(action="DESELECT")
        for ob in keep:
            ob.select_set(True)
        bpy.context.view_layer.objects.active = keep[0]
        bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        if len(keep) > 1:
            bpy.ops.object.join()

        safe = "".join(c if c.isalnum() else "_" for c in mat_name)
        dst = os.path.join(outdir, f"{stem}_{safe}.glb")
        bpy.ops.export_scene.gltf(
            filepath=dst, export_format="GLB",
            export_image_format="AUTO", export_yup=True, export_apply=True)
        reorder_glb_nodes_textured_first(dst)

        meshes, tris = mesh_stats([o for o in bpy.data.objects
                                   if o.type == "MESH"])
        bounds = bbox([o for o in bpy.data.objects if o.type == "MESH"])
        results.append({"part": os.path.basename(dst), "material": mat_name,
                        "triangles": tris, "bbox": bounds,
                        "outBytes": os.path.getsize(dst)})


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 2:
        print("usage: blender --background --python scripts/fbx2glb.py -- "
              "<in.fbx> <out.glb> [--report out.json]")
        sys.exit(2)
    src, dst = argv[0], argv[1]
    report_path = None
    maps_dir = None
    if "--report" in argv:
        report_path = argv[argv.index("--report") + 1]
    if "--maps" in argv:
        maps_dir = argv[argv.index("--maps") + 1]
    prune_keys = []
    if "--prune" in argv:
        prune_keys = argv[argv.index("--prune") + 1].split(",")
    split_dir = None
    if "--split-materials" in argv:
        split_dir = argv[argv.index("--split-materials") + 1]
    lod0_only = "--lod0" in argv        # 只留 LOD0 / 无 LOD 后缀件（Y-up 习惯车模）
    up_fix = "--up-fix" in argv         # Blender 空间绕 X +90°：Y-up 数据落回 Z-up
    mat_remap = {}
    if "--mat-remap" in argv:
        for pair in argv[argv.index("--mat-remap") + 1].split(","):
            frm, _, to = pair.partition("=")
            if frm and to:
                mat_remap[frm] = to
    # 材质颜色覆写："Name=r,g,b[,a]"（0~1）。FBX 只带作者机器的原始材质色，
    # Unity .mat 里的 _Color 覆写带不出来（如 Windows 应为黑 0,0,0，FBX 是灰
    # 0.561），按 Unity 权威色在此补写。
    mat_tint = {}
    if "--mat-tint" in argv:
        for pair in argv[argv.index("--mat-tint") + 1].split(","):
            name, _, vals = pair.partition("=")
            v = [float(x) for x in vals.split(":")]
            if name and len(v) >= 3:
                mat_tint[name] = v + [1.0] * (4 - len(v))

    if split_dir:
        results = []
        split_by_material(src, split_dir, os.path.splitext(os.path.basename(dst))[0],
                          maps_dir, prune_keys, results)
        line = json.dumps({"src": src, "splitDir": split_dir,
                           "parts": results}, ensure_ascii=True)
        if report_path:
            with open(report_path, "w", encoding="utf-8") as fh:
                fh.write(line + "\n")
        print("FBX2GLB_REPORT " + line)
        return

    clean_scene()
    objects = import_fbx(src)
    pruned = []
    prune_objects(prune_keys, pruned)
    # --lod0：源文件每部件带 LOD0/LOD2/LOD3 三层同位重叠网格（如 CivilianVehicle05，
    # 无 LOD1），全部导出会三角数翻倍 + 自遮挡。子串匹配以兼容 Blender 的 .001 后缀。
    lod0_pruned = 0
    if lod0_only:
        doomed = [ob for ob in bpy.data.objects
                  if any(s in ob.name for s in ("LOD1", "LOD2", "LOD3"))]
        lod0_pruned = len(doomed)
        for ob in doomed:
            bpy.data.objects.remove(ob, do_unlink=True)
    up_fixed = 0
    if up_fix:
        # 源 FBX 按 Y-up 习惯存（Y=车高、Z=车长），Blender 导入不做轴向转换、
        # glTF 导出再做 Z-up→Y-up，结果车长落到 glTF Y（竖立）。绕 X +90° 后
        # Blender 空间恢复「Z=高、Y=长」，导出后 glTF Y=高、Z=长，车平放。
        # 转对象 world matrix（含 empty 父）而非 mesh data：轮子等部件靠
        # object transform 摆位、data 在原点，只转 data 会漏掉它们的位移。
        # 转对象 matrix_world 会踩 FBX 导入的 unit-scale/父子分解坑（实测 ±90 尺度
        # 爆炸）。改走烘平：先把每块 mesh data 烘到 Blender 世界坐标、清空变换，
        # 再对烘平后的 data 绕 X +90°——全程无矩阵分解，轮子等 object 摆位件
        # 与车体一起走同一旋转。
        from mathutils import Matrix, Vector
        rot = Matrix.Rotation(__import__("math").radians(90.0), 4, (1.0, 0.0, 0.0))
        ident = Matrix.Identity(4)
        for ob in list(bpy.data.objects):
            if ob.type != "MESH":
                continue
            ob.data.transform(ob.matrix_world.copy())
            ob.data.transform(rot)
            ob.parent = None
            ob.matrix_world = ident
            up_fixed += 1
    mat_remapped = 0
    if mat_remap:
        # 源 FBX 部件丢材质槽（如 4 个轮子 3 个落到 "06 - Default"），按名重指。
        for ob in bpy.data.objects:
            for slot in ob.material_slots:
                if slot.material and slot.material.name in mat_remap:
                    tgt = bpy.data.materials.get(mat_remap[slot.material.name])
                    if tgt:
                        slot.material = tgt
                        mat_remapped += 1
    # 空材质槽压缩：FBX 常带无多边形的空槽（如轮子首槽 "06 - Default"），
    # Blender glTF 导出器在这种网格上不重映射 polygon.material_index，导致
    # 整个 prim 落到错误材质（实测：3 个轮子的轮毂面全导到空灰材质）。
    # 注意：materials.clear() 会把所有 polygon.material_index 归零（实测），
    # 所以必须先算好新索引、clear+append 完材质后再写回。
    slots_compacted = 0
    for ob in bpy.data.objects:
        if ob.type != "MESH" or not ob.material_slots:
            continue
        used = sorted({p.material_index for p in ob.data.polygons})
        if len(used) == len(ob.material_slots) and used == list(range(len(used))):
            continue
        remap = {old: new for new, old in enumerate(used)}
        newidx = [remap[p.material_index] for p in ob.data.polygons]  # clear 前算
        keep = [ob.material_slots[i].material for i in used]
        ob.data.materials.clear()
        for m in keep:
            ob.data.materials.append(m)
        for p, ni in zip(ob.data.polygons, newidx):                   # clear 后写
            p.material_index = ni
        slots_compacted += 1

    mat_tinted = 0
    if mat_tint:
        # 只覆写未连贴图的 Base Color 直连值；连贴图的材质 tint 属于乘色，
        # 语义不同，不允许在这里覆盖。
        for mat in bpy.data.materials:
            if mat.name not in mat_tint or not mat.use_nodes:
                continue
            bsdf = [n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"]
            if not bsdf or bsdf[0].inputs["Base Color"].is_linked:
                continue
            bsdf[0].inputs["Base Color"].default_value = mat_tint[mat.name]
            mat_tinted += 1

    objects = [o for o in bpy.data.objects]
    meshes, tris = mesh_stats(objects)
    retarget = retarget_images(maps_dir)
    reordered = order_materials_textured_first(meshes)
    mats = material_report(meshes)
    bounds = bbox(objects)

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=dst,
        export_format="GLB",
        export_image_format="AUTO",   # embed textures into the GLB
        export_yup=True,
        export_apply=True,            # apply modifiers / triangulate
    )
    nodes_moved, textured_count = reorder_glb_nodes_textured_first(dst)

    result = {
        "src": src,
        "dst": dst,
        "objects": len(objects),
        "meshes": len(meshes),
        "triangles": tris,
        "materials": len(mats),
        "materialDetail": mats,
        "retargetedTextures": retarget,
        "prunedObjects": pruned,
        "lod0Pruned": lod0_pruned,
        "upFixRotated": up_fixed,
        "emptySlotsCompacted": slots_compacted,
        "matRemapped": mat_remapped,
        "matTinted": mat_tinted,
        "meshesReorderedTexturedFirst": reordered,
        "glbNodesTexturedFirst": {"arraysReordered": nodes_moved,
                                  "texturedMeshes": textured_count},
        "bbox": bounds,
        "outBytes": os.path.getsize(dst),
    }
    line = json.dumps(result, ensure_ascii=True)
    if report_path:
        with open(report_path, "w", encoding="utf-8") as fh:
            fh.write(line + "\n")
    print("FBX2GLB_REPORT " + line)


main()
