#!/usr/bin/env python3
"""FBX -> self-contained GLB via headless Blender (P9 V1 asset-ingest).

Run by Blender itself (NOT plain python):

  blender --background --python scripts/fbx2glb.py -- <in.fbx> <out.glb>
          [--report out.json] [--unity <AssetRoot>] [--up-fix]
          [--prune Name1,Name2] [--split-materials <dir>]

Material authority
------------------
Unity 资产包是材质权威：FBX 里贴图引用是作者机器死路径、_Color 是导出残值。
--unity 给定资产根后：

* .mat 解析走 vendored unityparser（scripts/vendor/，Apache-2.0），不手写；
* 属性 -> glTF PBR 映射照抄 KhronosGroup/UnityGLTF ExporterMaterials.cs
  （本地副本 vendor-ref/ExporterMaterials.cs），实现见 scripts/unity_mat.py；
* 贴图经 .meta guid 还原为真实文件路径；
* .mat 匹配 FBX 材质名按 Unity ModelImporter materialSearch=1 口径
  （工程内按名搜索）：精确名 -> X_Albedo -> XAlbedo。

Exit code 0 on success; the JSON report on stdout's last line is the contract
consumed by the batch evidence script.
"""

import json
import os
import re
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
for _v in ("vendor", _HERE):
    if _v not in sys.path:
        sys.path.insert(0, _v)

import bpy

import unity_mat


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
    """兜底：FBX 材质自带贴图引用（.mat 未覆盖的槽位）常是死路径，按 stem
    在 maps_dir 内重定向。.mat guid 绑定走 unity_mat，这里只处理剩余项。"""
    if not maps_dir:
        return []
    files = []
    for root, _dirs, names in os.walk(maps_dir):
        if "Library" in root:
            continue
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


def patch_glb_basecolor_factors(path, factors):
    """Blender glTF 导出器在 Base Color 连贴图时不写 baseColorFactor（按其
    实现默认白色）。glTF spec 里 factor 与 texture 是相乘关系，.mat 的
    _Color 是权威乘色 -> 导出后 JSON 回写。仅对有 baseColorTexture 的材质
    写入，按材质名精确匹配。"""
    if not factors:
        return 0
    with open(path, "rb") as fh:
        data = fh.read()
    magic, version, _length = struct.unpack("<III", data[:12])
    jlen, jtype = struct.unpack("<II", data[12:20])
    j = json.loads(data[20:20 + jlen])
    blen, btype = struct.unpack("<II", data[20 + jlen:28 + jlen])
    binchunk = data[28 + jlen:28 + jlen + blen]

    written = []
    for mat in j.get("materials", []):
        pbr = mat.setdefault("pbrMetallicRoughness", {})
        if pbr.get("baseColorTexture") is not None and mat.get("name") in factors:
            pbr["baseColorFactor"] = factors[mat["name"]]
            written.append(mat["name"])

    outj = json.dumps(j, separators=(",", ":")).encode()
    outj += b" " * ((4 - len(outj) % 4) % 4)
    total = 12 + 8 + len(outj) + 8 + len(binchunk)
    with open(path, "wb") as fh:
        fh.write(struct.pack("<III", magic, version, total))
        fh.write(struct.pack("<I", len(outj)) + struct.pack("<I", 0x4E4F534A))
        fh.write(outj)
        fh.write(struct.pack("<I", blen) + struct.pack("<I", btype) + binchunk)
    return len(written)


def prune_objects(name_keys, report):
    """Delete objects whose name starts with one of ``name_keys``
    (case-insensitive; e.g. collision proxies, driver 'Civil01')."""
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


def _set_blend_mode(mat, alpha_mode):
    """Blender 5.x：blend_method 已迁往 surface_render_method；两个都按存在
    性尝试，glTF 导出器据此写 alphaMode。引擎现阶段不消费 alphaMode，
    记录为 spec 正确性，不算行为变更。"""
    try:
        if alpha_mode == "BLEND":
            mat.blend_method = "BLEND"
            mat.surface_render_method = "DITHERED"
        elif alpha_mode == "MASK":
            mat.blend_method = "CLIP"
    except AttributeError:
        pass


def apply_unity_material(mat, g, factors):
    """unity_mat.umat_to_gltf 的产物落到 Blender Principled 节点。
    factors：连贴图且乘色非白 -> 导出后 patch_glb_basecolor_factors 回写。"""
    if not mat.use_nodes:
        mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    pbr = g["pbr"]
    note = {"color": False, "tex": False, "pbr": False, "normal": False,
            "emission": False, "glbFactor": False}

    tex_path = pbr.get("baseColorTexture")
    if tex_path and os.path.isfile(tex_path):
        img = bpy.data.images.load(tex_path, check_existing=True)
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = img
        nt.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
        note["tex"] = True
    f = pbr.get("baseColorFactor")
    if f:
        r, g_, b, a = f
        if note["tex"]:
            if any(abs(v - 1.0) > 0.01 for v in (r, g_, b)):
                factors[mat.name] = [r, g_, b, a]
                note["glbFactor"] = True
        else:
            bsdf.inputs["Base Color"].default_value = (r, g_, b, 1.0)
            note["color"] = True

    if "metallicFactor" in pbr and not bsdf.inputs["Metallic"].is_linked:
        bsdf.inputs["Metallic"].default_value = pbr["metallicFactor"]
        note["pbr"] = True
    if "roughnessFactor" in pbr and not bsdf.inputs["Roughness"].is_linked:
        bsdf.inputs["Roughness"].default_value = pbr["roughnessFactor"]
        note["pbr"] = True

    nrm = g.get("normalTexture")
    if nrm and os.path.isfile(nrm["path"]):
        img = bpy.data.images.load(nrm["path"], check_existing=True)
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = img
        nmap = nt.nodes.new("ShaderNodeNormalMap")
        nmap.inputs["Strength"].default_value = nrm.get("scale", 1.0)
        nt.links.new(node.outputs["Color"], nmap.inputs["Color"])
        nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        note["normal"] = True

    emf = g.get("emissiveFactor")
    if emf:
        bsdf.inputs["Emission Color"].default_value = (emf[0], emf[1], emf[2], 1.0)
        bsdf.inputs["Emission Strength"].default_value = 1.0
        note["emission"] = True
    if g.get("emissiveTexture") and os.path.isfile(g["emissiveTexture"]):
        img = bpy.data.images.load(g["emissiveTexture"], check_existing=True)
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = img
        if not bsdf.inputs["Emission Color"].is_linked:
            nt.links.new(node.outputs["Color"], bsdf.inputs["Emission Color"])
        note["emission"] = True

    _set_blend_mode(mat, g.get("alphaMode", "OPAQUE"))
    return note


def split_by_material(src, outdir, stem, unity_dir, prune_keys, results):
    """Legacy path: one glb per material (--split-materials). Re-import the FBX
    once per material, keep only meshes using it, clear+bake transforms, join
    into a single mesh object, export <stem>_<material>.glb."""
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
              "<in.fbx> <out.glb> [--report out.json] [--unity <AssetRoot>] "
              "[--up-fix] [--prune N1,N2] [--split-materials <dir>]")
        sys.exit(2)
    src, dst = argv[0], argv[1]
    report_path = None
    if "--report" in argv:
        report_path = argv[argv.index("--report") + 1]
    # --unity <dir>：Unity 工程资产根（材质权威）。.mat 解析 + guid 贴图绑定
    # + materialSearch=1 按名匹配；FBX 只是残缺几何容器。
    unity_dir = None
    if "--unity" in argv:
        unity_dir = argv[argv.index("--unity") + 1]
    prune_keys = []                        # --prune：点名剪除（司机模型等）
    if "--prune" in argv:
        prune_keys = argv[argv.index("--prune") + 1].split(",")
    split_dir = None
    if "--split-materials" in argv:
        split_dir = argv[argv.index("--split-materials") + 1]
    up_fix = "--up-fix" in argv         # Blender 空间绕 X +90°：Y-up 数据落回 Z-up

    if split_dir:
        results = []
        split_by_material(src, split_dir, os.path.splitext(os.path.basename(dst))[0],
                          unity_dir, prune_keys, results)
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
    # 自动剪除（Unity 导出惯例）：
    #   *_Col / *Collision      碰撞代理（引擎不需要）
    #   *_LOD[1-9]*             LOD>0 层（每部件 2~4 层同位重叠网格）
    doomed = [ob for ob in bpy.data.objects
              if ob.type == "MESH" and re.search(r"(_Col$|_Collision$|_LOD[1-9])", ob.name)]
    auto_pruned = len(doomed)
    for ob in doomed:
        bpy.data.objects.remove(ob, do_unlink=True)
    up_fixed = 0
    if up_fix:
        # 源 FBX 按 Y-up 习惯存（Y=车高、Z=车长），Blender 导入不做轴向转换、
        # glTF 导出再做 Z-up→Y-up，结果车长落到 glTF Y（竖立）。走烘平：先把
        # 每块 mesh data 烘到 Blender 世界坐标、清空变换，再对烘平后的 data
        # 绕 X +90°——全程无矩阵分解，轮子等 object 摆位件与车体同转。
        from mathutils import Matrix
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

    # ---- Unity 权威材质（unityparser + UnityGLTF ExporterMaterials 映射链）----
    unity_stats = {"applied": 0, "matched": 0, "unmatchedFBX": [],
                   "matErrors": [], "unlit": [], "notes": {}}
    glb_factors = {}
    if unity_dir:
        guid_db = unity_mat.build_guid_db(unity_dir)
        all_mats = unity_mat.load_all_mats(unity_dir, guid_db)
        for name, um in all_mats.items():
            if name.startswith("!error:"):
                unity_stats["matErrors"].append(
                    {"file": name[7:], "error": um.get("error")})
        for mat in bpy.data.materials:
            if not mat.use_nodes:
                mat.use_nodes = True
            key = unity_mat.find_mat_for(mat.name, all_mats)
            if key is None:
                unity_stats["unmatchedFBX"].append(mat.name)
                continue
            g = unity_mat.umat_to_gltf(all_mats[key], guid_db)
            if g is None:
                continue
            unity_stats["matched"] += 1
            if g.get("unlit"):
                unity_stats["unlit"].append(mat.name)
            note = apply_unity_material(mat, g, glb_factors)
            if any(note.values()):
                unity_stats["applied"] += 1
                unity_stats["notes"][mat.name] = {"mat": key, **note}

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

    objects = [o for o in bpy.data.objects]
    meshes, tris = mesh_stats(objects)
    retarget = retarget_images(unity_dir)
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
    factors_written = patch_glb_basecolor_factors(dst, glb_factors)

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
        "autoPruned": auto_pruned,
        "upFixRotated": up_fixed,
        "emptySlotsCompacted": slots_compacted,
        "unityMaterial": unity_stats,
        "glbBaseColorFactors": {"requested": sorted(glb_factors.keys()),
                                "written": factors_written},
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
