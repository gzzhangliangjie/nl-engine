#!/usr/bin/env python3
"""全工程 .mat 审计：解析 + 逐槽位验证，问题逐个归因，不允许静默容错。

  python scripts/mat_audit.py <AssetRoot> [--report out.json]

检查项（每个 .mat）：
* 解析成功（新旧两代序列化格式）；
* m_Shader 可归因：内置 guid(fileID 已知) / 工程内 .shader 文件 / unresolved；
* 消费槽位（_MainTex/_BumpMap/_EmissionMap/_OcclusionMap/_MetallicGlossMap）
  的贴图 guid：未登记 / 文件缺失 / 不可嵌入且无同 stem 兄弟 / 正常；
* m_SavedProperties 缺失等结构异常。

exit 0 = 零错误；否则 exit 1 并列出全部问题。
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import unity_mat

CONSUMED_SLOTS = ("_MainTex", "_BumpMap", "_EmissionMap",
                  "_OcclusionMap", "_MetallicGlossMap")


def audit(root, report_path=None):
    guid_db = unity_mat.build_guid_db(root)
    mat_files = []
    for dirpath, _dirs, names in os.walk(root):
        if "Library" in dirpath:
            continue
        for n in names:
            if n.endswith(".mat"):
                mat_files.append(os.path.join(dirpath, n))
    mat_files.sort()

    stats = {"total": len(mat_files), "guidDb": len(guid_db),
             "formats": {"new": 0, "legacy": 0},
             "shaders": {"resolved": 0, "unresolved": 0},
             "textures": {"ok": 0, "missing": 0, "nonEmbeddable": 0},
             "errors": [], "warnings": []}

    for p in mat_files:
        rel = os.path.relpath(p, root)
        entry = {"file": rel, "issues": []}
        try:
            # 复用 load_mat 的同时取原始形态判断格式代际
            unity_mat._ensure_vendor()
            from unityparser import UnityDocument
            doc = UnityDocument.load_yaml(p, try_preserve_types=True)
            m = doc.entries[0]
            sp = m.m_SavedProperties
            entry["format"] = ("legacy" if isinstance(sp["m_TexEnvs"], dict)
                               else "new")
        except Exception as e:
            entry["issues"].append(f"parse-error: {type(e).__name__}: {e}")
            stats["errors"].append(entry)
            continue

        if entry["format"] == "legacy":
            stats["formats"]["legacy"] += 1
        else:
            stats["formats"]["new"] += 1

        um = unity_mat.load_mat(p, guid_db)

        # shader 归因：内置 guid(fileID 表) / 工程内 .shader 文件 / unresolved
        entry["shader"] = um.get("shader") or "unresolved"
        if um.get("shader"):
            stats["shaders"]["resolved"] += 1
        else:
            entry["issues"].append("shader-unresolved")
            stats["shaders"]["unresolved"] += 1

        # 消费槽位贴图归因
        for slot in CONSUMED_SLOTS:
            info = um.get(_slot_field(slot))
            if info is None:
                continue                                    # 材质没有该槽位
            guid = info.get("guid")
            if not guid:
                continue                                    # fileID 0，未绑定
            if not info.get("path"):
                db_path = guid_db.get(guid)
                if db_path is None:
                    entry["issues"].append(f"{slot}: guid-not-in-db {guid}")
                    stats["textures"]["missing"] += 1
                else:
                    entry["issues"].append(
                        f"{slot}: non-embeddable-no-sibling {db_path}")
                    stats["textures"]["nonEmbeddable"] += 1
            else:
                stats["textures"]["ok"] += 1

        if "m_SavedProperties" not in dir(m) and not hasattr(m, "m_SavedProperties"):
            entry["issues"].append("no-m_SavedProperties")
        if entry["issues"]:
            # errors = 转换器侧失败（解析/结构）；asset-defect = 资产包自身
            # 缺陷（悬空 guid / 无法归因 shader），如实列出并归因，不算我们失败。
            if any(i.startswith("parse-error") or i == "no-m_SavedProperties"
                   for i in entry["issues"]):
                stats["errors"].append(entry)
            else:
                stats["warnings"].append(entry)
        else:
            stats.setdefault("clean", []).append(rel)

    line = json.dumps(stats, ensure_ascii=False, indent=1)
    if report_path:
        with open(report_path, "w", encoding="utf-8") as fh:
            fh.write(line + "\n")
    print(line)
    return 0 if not stats["errors"] else 1


def _slot_field(slot):
    return {"_MainTex": "mainTex", "_BumpMap": "bumpMap",
            "_EmissionMap": "emissionMap", "_OcclusionMap": "occlusionMap",
            "_MetallicGlossMap": "metallicGlossMap"}[slot]


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        sys.exit(2)
    root = argv[0]
    report = None
    if "--report" in argv:
        report = argv[argv.index("--report") + 1]
    sys.exit(audit(root, report))


main()
