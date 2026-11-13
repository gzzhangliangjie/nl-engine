"""Unity .mat 权威材质解析 -> glTF PBR 数据。

解析层用 vendored 成熟库 unityparser 4.0.0（socialpoint-labs/unity-yaml-parser，
Apache-2.0）+ PyYAML 6.0.2 纯 Python 版（yaml/pyyaml，MIT），二者均在
scripts/vendor/ 下，由 fbx2glb.py 注入 sys.path。不手写 YAML 解析。

映射层照抄 KhronosGroup/UnityGLTF 的 Runtime/Scripts/SceneExporter/
ExporterMaterials.cs（MIT，本地参考副本 vendor-ref/ExporterMaterials.cs），
只实现我们 .mat 资产实际出现的分支（legacy Standard 系）：

* alphaMode：_Mode 1 -> MASK(_Cutoff)；2/3 -> BLEND；0 -> OPAQUE
  （对应 ExporterMaterial 的 RenderType/keywords 分支，.mat 里 legacy
  Standard 的 _Mode 就是 RenderType 的序列化形态）；
* emission：m_ShaderKeywords 含 _EMISSION -> _EmissionColor/_EmissionMap；
* normal：_NORMALMAP 关键字 + _BumpMap -> normalTexture（_BumpScale）；
* PBR 主链（ExporterMaterials.cs ExportPBRMetallicRoughness 的 YAML 面）：
  baseColorFactor = _Color；baseColorTexture = _MainTex；
  metallic = _Metallic（_METALLICGLOSSMAP 关键字 + _MetallicGlossMap 贴图
  时改走 metallicRoughnessTexture、metallic 因子置 1）；
  roughness = 1 - _Glossiness（有 _MetallicGlossMap 时取 _GlossMapScale）；
* occlusion：_OcclusionMap（_OcclusionStrength）；
* 贴图 tiling/offset != 单位 -> KHR_texture_transform
  （UnityGLTF ExportTextureTransform 的 Y 翻转口径：offset' = (x, 1-y-scale)）；
* shader 名：m_Shader 内置 guid (0000000000000000f000000000000000) 按已知
  fileID 取名（46=Standard），自定义 guid 经 guid->路径查 .shader 文件头。
"""

import os
import re
import sys

# bpy.ops.wm.read_factory_settings() 会重置 sys.path（实测），vendor 路径
# 不能只在 fbx2glb.py 头部注入一次，每次真正 import 前都要确保。
_VENDOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor")


def _ensure_vendor():
    if _VENDOR not in sys.path:
        sys.path.insert(0, _VENDOR)

# ---------------------------------------------------------------- guid 数据库

def build_guid_db(root):
    """扫全工程 *.meta 建 guid -> 资产路径（不含 .meta 后缀）。
    与 Unity 资产数据库同口径，无启发式。"""
    db = {}
    for dirpath, _dirs, names in os.walk(root):
        if os.path.normpath(dirpath).lower().find(os.path.normpath("Library")) >= 0:
            continue  # Library 缓存不是资产
        for n in names:
            if not n.endswith(".meta"):
                continue
            p = os.path.join(dirpath, n)
            try:
                with open(p, encoding="utf-8", errors="ignore") as fh:
                    head = fh.read(512)
            except OSError:
                continue
            m = re.search(r"guid:\s*([0-9a-f]{32})", head)
            if m:
                # 只剥 ".meta"，保留资产真实扩展名（.tga/.psd/...）
                db[m.group(1)] = p[:-5]
    return db


PREF_EXTS = (".png", ".jpg", ".jpeg", ".tga")


def resolve_embeddable(path):
    """guid 命中的贴图若非可嵌入格式（如 .psd），找同目录同 stem 的
    png/jpg/tga 兄弟文件；都没有则返回 None（走 FBX 自带引用兜底）。"""
    if not path:
        return None
    ext = os.path.splitext(path)[1].lower()
    if ext in PREF_EXTS and os.path.isfile(path):
        return path
    stem = os.path.splitext(path)[0]
    for e in PREF_EXTS:
        cand = stem + e
        if os.path.isfile(cand):
            return cand
    return None

# ---------------------------------------------------------------- .mat 解析

BUILTIN_SHADER_GUID = "0000000000000000f000000000000000"
# unity_builtin_extra 的 fileID -> shader 名。社区长期整理表（vicobill
# Unity 开发笔记），与 Unity 官方 URP shader 映射文档交叉印证一致
# （46=Standard、10703=Mobile/Diffuse、10704=Mobile/Bumped Diffuse、
# 10708=Mobile/Unlit）。缺失的 fileID 归为 unresolved 由审计上报。
BUILTIN_SHADER_FILEIDS = {
    1: "legacy/diffuse fast", 2: "Legacy/Bumped Diffuse",
    3: "legacy/specular", 4: "Legacy/Bumped Specular",
    5: "legacy/diffuse detail", 6: "legacy/vertexlit", 7: "legacy/Diffuse",
    8: "legacy/parallax diffuse", 9: "legacy/parallax specular",
    10: "legacy/self-illumin/diffuse", 11: "legacy/self-illumin/bumped diffuse",
    12: "legacy/self-illumin/specular", 13: "legacy/self-illumin/bumped specular",
    14: "legacy/self-illumin/vertexlit", 15: "legacy/self-illumin/parallax diffuse",
    16: "legacy/self-illumin/parallax specular",
    20: "legacy/reflective/diffuse", 21: "legacy/reflective/bumped diffuse",
    22: "legacy/reflective/specular", 23: "legacy/reflective/bumped specular",
    24: "legacy/reflective/vertexlit", 25: "legacy/reflective/bumped unlit",
    26: "legacy/reflective/bumped vertexlit", 27: "legacy/reflective/parallax diffuse",
    28: "legacy/reflective/parallax specular",
    30: "legacy/transparent/diffuse", 31: "legacy/transparent/bumped diffuse",
    32: "legacy/transparent/specular", 33: "legacy/transparent/bumped specular",
    34: "legacy/transparent/vertexlit", 35: "legacy/transparent/parallax diffuse",
    36: "legacy/transparent/parallax specular",
    40: "legacy/lightmapped/vertexlit", 41: "legacy/lightmapped/diffuse",
    42: "legacy/lightmapped/bumped diffuse", 43: "legacy/lightmapped/specular",
    44: "legacy/lightmapped/bumped specular",
    45: "Standard (Specular Setup)", 46: "Standard", 47: "Autodesk Interactive",
    50: "legacy/transparent/cutout/vertexlit",
    51: "legacy/transparent/cutout/diffuse",
    52: "legacy/transparent/cutout/bumped diffuse",
    53: "legacy/transparent/cutout/specular",
    54: "legacy/transparent/cutout/bumped specular",
    100: "Legacy/Decal", 101: "FX/Flare", 103: "skybox/cubemap",
    104: "skybox/6-sided", 106: "skybox/procedural", 108: "skybox/panoramic",
    200: "Legacy/Particles/Additive", 201: "Legacy/Particles/Additive (Soft)",
    203: "Legacy/Particles/Alpha Blended", 205: "Legacy/Particles/Multiply",
    206: "Legacy/Particles/Multiply (Double)", 207: "Legacy/Particles/Alpha Blended Premultiply",
    208: "Legacy/Particles/VertexLit Blended", 209: "Legacy/Particles/Anim Alpha Blended",
    210: "Particles/Standard Surface", 211: "Particles/Standard Unlit",
    10101: "GUI/Text Shader", 10505: "Nature/Terrain/Diffuse",
    10509: "Nature/Tree Soft Occlusion Bark", 10511: "Nature/Tree Soft Occlusion Leaves",
    10600: "Nature/Tree Creator Bark", 10601: "Nature/Tree Creator Leaves",
    10602: "Nature/Terrain/Specular", 10606: "Nature/Tree Creator Leaves Fast",
    10623: "Nature/Terrain/Standard",
    10700: "Mobile/Skybox", 10701: "Mobile/VertexLit", 10703: "Mobile/Diffuse",
    10704: "Mobile/Bumped Diffuse", 10705: "Mobile/Bumped Specular",
    10706: "Mobile/Bumped Specular (1 Directional)", 10707: "Mobile/VertexLit (1 Directional)",
    10708: "Mobile/Unlit (Supports Lightmap)",
    10720: "Mobile/Particles/Additive", 10721: "Mobile/Particles/Alpha Blended",
    10722: "Mobile/Particles/VertexLit Blended", 10723: "Mobile/Particles/Multiply",
    10750: "Unlit/Transparent", 10751: "Unlit/Transparent Cutout",
    10752: "Unlit/Texture", 10753: "Sprites/Default", 10755: "Unlit/Color",
    10757: "Sprites/Mask", 10760: "UI/Unlit/Transparent", 10761: "UI/Unlit/Detail",
    10762: "UI/Unlit/Text", 10763: "UI/Unlit/Text Detail",
    10764: "UI/Lit/Transparent", 10765: "UI/Lit/Bumped", 10766: "UI/Lit/Detail",
    10767: "UI/Lit/Refraction", 10768: "UI/Lit/Refraction Detail",
    10770: "UI/Default", 10782: "UI/Default Font", 10783: "UI/DefaultETC1",
    10800: "Sprites/Diffuse",
    14000: "Nature/SpeedTree", 14001: "Nature/SpeedTree Billboard",
    14002: "Nature/SpeedTree8",
    15300: "VR/SpatialMapping/Occlusion", 15301: "VR/SpatialMapping/Wireframe",
    15401: "AR/TangoARRenderer",
}


def _flat(seq):
    """新版格式（Unity 5.x）：m_TexEnvs/m_Floats/m_Colors 是
    [ {key: value}, ... ] 列表，摊平成 dict。"""
    out = {}
    for kv in seq:
        for k, v in kv.items():
            out[k] = v
    return out


def _normalize_entries(container):
    """m_SavedProperties 各槽位容器 -> {name: value}。兼容两代序列化格式：
    * 新版（Unity 5.x，serializedVersion 3+）：[{name: value}, ...]
    * 旧版（Unity 4.x 及更早）：{data: [{first: {name}, second: value}, ...]}
      （重复 `data:` 键由 vendored unityparser 的 DupSeq 收集，不丢项）
    旧版 second 可为标量（m_Floats）或映射（m_TexEnvs/m_Colors）。"""
    from unityparser.constructor import DupSeq   # vendor 注入后可用
    if container is None:
        return {}
    if isinstance(container, dict):
        seq = container.get("data")
        out = {}
        if isinstance(seq, DupSeq):
            items = seq
        elif seq is None:
            return {}
        else:  # 单条 data（理论不出现，兜底）
            items = [seq]
        for item in items:
            if not isinstance(item, dict):
                continue
            first, second = item.get("first"), item.get("second")
            name = first.get("name") if isinstance(first, dict) else first
            if name is not None:
                out[name] = second
        return out
    return _flat(container)


def _tex_guid(texprop):
    """m_Texture: {fileID, guid, type} | {fileID: 0}（空）-> guid or None"""
    if isinstance(texprop, dict) and texprop.get("guid"):
        return texprop["guid"]
    return None


def _shader_name(m_shader, guid_db):
    guid = m_shader.get("guid") if isinstance(m_shader, dict) else None
    if guid == BUILTIN_SHADER_GUID:
        return BUILTIN_SHADER_FILEIDS.get(m_shader.get("fileID", 0))
    path = guid_db.get(guid)
    if path and os.path.isfile(path):
        try:
            with open(path, encoding="utf-8", errors="ignore") as fh:
                m = re.search(r'Shader\s+"([^"]+)"', fh.read(4096))
            if m:
                return m.group(1)
        except OSError:
            pass
    return None


def load_mat(path, guid_db):
    """解析一个 .mat -> 归一化 dict（全部键可缺省）。新旧 Unity 序列化格式
    均兼容（见 _normalize_entries）。"""
    _ensure_vendor()
    from unityparser import UnityDocument
    doc = UnityDocument.load_yaml(path, try_preserve_types=True)
    m = doc.entries[0]                 # 取第一个文档；!u!1002 EditorExtensionImpl 等附加条目忽略
    sp = m.m_SavedProperties
    tex = _normalize_entries(sp["m_TexEnvs"])
    flo = _normalize_entries(sp["m_Floats"])
    col = _normalize_entries(sp["m_Colors"])

    def col4(entry):
        c = col.get(entry)
        if c is None:
            return None
        return [c["r"], c["g"], c["b"], c["a"]]

    def texinfo(entry):
        t = tex.get(entry)
        if t is None:
            return None
        guid = _tex_guid(t.get("m_Texture"))
        return {"guid": guid,
                "path": resolve_embeddable(guid_db.get(guid)),
                "scale": [t["m_Scale"]["x"], t["m_Scale"]["y"]],
                "offset": [t["m_Offset"]["x"], t["m_Offset"]["y"]]}

    kw_field = getattr(m, "m_ShaderKeywords", None)
    if kw_field is None:
        keywords = set()
    elif isinstance(kw_field, (list, tuple)):
        keywords = {str(k) for k in kw_field if k}      # 旧格式：YAML 列表
    else:
        keywords = set(str(kw_field).split())           # 新格式：空格分隔字符串
    return {
        "name": m.m_Name,
        "shader": _shader_name(m.m_Shader, guid_db),
        "keywords": keywords,
        "color": col4("_Color"),
        "emissionColor": col4("_EmissionColor"),
        "metallic": flo.get("_Metallic"),
        "glossiness": flo.get("_Glossiness"),
        "glossMapScale": flo.get("_GlossMapScale"),
        "cutoff": flo.get("_Cutoff"),
        "mode": flo.get("_Mode"),
        "bumpScale": flo.get("_BumpScale"),
        "occlusionStrength": flo.get("_OcclusionStrength"),
        "mainTex": texinfo("_MainTex"),
        "bumpMap": texinfo("_BumpMap"),
        "emissionMap": texinfo("_EmissionMap"),
        "occlusionMap": texinfo("_OcclusionMap"),
        "metallicGlossMap": texinfo("_MetallicGlossMap"),
    }


def load_all_mats(root, guid_db):
    mats = {}
    for dirpath, _dirs, names in os.walk(root):
        if os.path.normpath(dirpath).lower().find(os.path.normpath("Library")) >= 0:
            continue
        for n in names:
            if not n.endswith(".mat"):
                continue
            p = os.path.join(dirpath, n)
            try:
                um = load_mat(p, guid_db)
            except Exception as e:          # 单个坏文件不拖垮整批
                mats["!error:" + n] = {"name": n, "error": str(e)}
                continue
            mats[os.path.splitext(n)[0]] = um
    return mats


def find_mat_for(fbx_mat_name, mats):
    """Unity ModelImporter materialSearch=1（Project 按名搜索）口径的候选链：
    精确名 -> X_Albedo -> XAlbedo（实测覆盖 Body->BodyAlbedo、Misc->Misc_Albedo）。"""
    for cand in (fbx_mat_name, fbx_mat_name + "_Albedo", fbx_mat_name + "Albedo"):
        if cand in mats:
            return cand
    return None

# ---------------------------------------------------------------- glTF 映射
# 以下为 ExporterMaterials.cs 分支的 YAML 面实现。返回
# {"pbr": {...}, "alphaMode": .., "alphaCutoff": .., "normalTexture": ..,
#  "occlusionTexture": .., "emissiveFactor": .., "emissiveTexture": ..,
#  "transform": <slot, scale, offset> | None, "unlit": bool}

def umat_to_gltf(um, guid_db):
    if um is None or um.get("error"):
        return None
    kw = um.get("keywords") or set()
    out = {"pbr": {}, "unlit": False}

    # --- unlit 分支（IsUnlit: shader 名含 unlit）：KHR unlit，引擎按普通
    # PBR 吃也无碍（baseColor 照给），只标记。
    shader = (um.get("shader") or "")
    out["unlit"] = "unlit" in shader.lower()

    # --- alpha（legacy Standard _Mode == RenderType 的序列化形态）
    mode = um.get("mode")
    if mode == 1:
        out["alphaMode"] = "MASK"
        out["alphaCutoff"] = um.get("cutoff", 0.5)
    elif mode in (2, 3):
        out["alphaMode"] = "BLEND"
    else:
        out["alphaMode"] = "OPAQUE"

    # --- PBR 主链
    pbr = out["pbr"]
    if um.get("color") is not None:
        r, g, b, a = um["color"]
        # Windows 等纯色玻璃：_Mode3 + alpha<1 -> 保 alpha（引擎侧混合未接，
        # 转 GLB 仍按 spec 记录，后处理/引擎接入各取所需）
        pbr["baseColorFactor"] = [r, g, b, a if out["alphaMode"] != "OPAQUE" else 1.0]
    main = um.get("mainTex")
    if main and main.get("path"):
        pbr["baseColorTexture"] = main["path"]
        sc, off = main["scale"], main["offset"]
        if sc != [1.0, 1.0] or off != [0.0, 0.0]:
            # UnityGLTF ExportTextureTransform Y 翻转口径
            out["transform"] = {"slot": "baseColor",
                                "scale": sc,
                                "offset": [off[0], 1.0 - off[1] - sc[1]]}

    has_mg_map = bool(um.get("metallicGlossMap") and um["metallicGlossMap"].get("path"))
    if has_mg_map and "_METALLICGLOSSMAP" in kw:
        pbr["metallicRoughnessTexture"] = um["metallicGlossMap"]["path"]
        # UnityGLTF 口径：MG 贴图在场 → 因子恒 1（纹理即权威）。注意 glTF 因子
        # 与纹理是相乘关系——旧写法 roughnessFactor=1-_GlossMapScale 在 scale=1
        # 时会把整张贴图的 roughness 乘成 0。_GlossMapScale（smoothness 缩放）
        # 烘进 swizzle（G = 1 - A*scale，见 fbx2glb.apply_unity_material）。
        pbr["metallicFactor"] = 1.0
        pbr["roughnessFactor"] = 1.0
        smooth = um.get("glossMapScale", um.get("glossiness"))
        out["mgSmoothScale"] = smooth if smooth is not None else 1.0
    else:
        if um.get("metallic") is not None:
            pbr["metallicFactor"] = um["metallic"]
        if um.get("glossiness") is not None:
            pbr["roughnessFactor"] = 1.0 - um["glossiness"]

    # --- normal（_NORMALMAP 关键字 + _BumpMap，照 ExporterMaterials L239）
    if "_NORMALMAP" in kw and um.get("bumpMap") and um["bumpMap"].get("path"):
        out["normalTexture"] = {"path": um["bumpMap"]["path"],
                                "scale": um.get("bumpScale") or 1.0}

    # --- emission（_EMISSION 关键字，照 ExporterMaterials L147）
    if "_EMISSION" in kw:
        ec = um.get("emissionColor")
        if ec and any(abs(v) > 1e-4 for v in ec[:3]):
            out["emissiveFactor"] = [min(max(v, 0.0), 1.0) for v in ec[:3]]
        em = um.get("emissionMap")
        if em and em.get("path"):
            out["emissiveTexture"] = em["path"]

    # --- occlusion
    occ = um.get("occlusionMap")
    if occ and occ.get("path"):
        out["occlusionTexture"] = {"path": occ["path"],
                                   "strength": um.get("occlusionStrength", 1.0)}
    return out
