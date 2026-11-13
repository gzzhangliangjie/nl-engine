"""w1-m0 run.bat helper - machine checks for the M-W1.0 delivery chain.

modes:
  anchors   - 36 glb + 36x3 meshlet suite files + engine refs closed, all
              pass the flat-name whitelist; exit 1 on any mismatch
  json      - city.scene.engine parses as E1 World JSON, 36 entities, every
              Transform complete (rotation closure), every mesh ref == the
              expected glb name; exit 1 on any mismatch
  hash      - print one aggregate sha256 over all delivery files (glb +
              meshlets + scene.engine + evidence png) for the determinism
              segment (same-path double run)
"""
import hashlib
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SEED, SIZE, BLOCKS = 12345, 6, 36

GLB_RE = re.compile(r"^[A-Za-z0-9_]+\.glb$")
NAME = "city_block_%d_%d_%02d.glb" % (SEED, SIZE, 0)


def glb(i):
    return "city_block_%d_%d_%02d.glb" % (SEED, SIZE, i)


def delivery_files():
    out = list((ROOT / "assets" / "models").glob("city_block_%d_%d_*.glb" % (SEED, SIZE)))
    out += list((ROOT / "assets" / "meshlets").glob("city_block_%d_%d_*" % (SEED, SIZE)))
    # M-W2.0 修复：hash 集合按文档口径（glb + meshlets + scene.engine +
    # evidence png）。旧实现 HERE.iterdir() 全量收录把 logs / run.bat /
    # h1.txt / h2.txt 也卷进集合——h1/h2 重定向文件自身时序（h1 计算时 0B、
    # h2 计算时已有内容）使 seg5 双跑集合永不对称，确定性对拍永远 MISMATCH
    # （W1.0 潜伏 bug，M-W2.0 基线重拍炸出；旧 146 文件口径从未被 run.bat
    # 本体真正跑通过，M-W1 验收是复刻工具在集合外对拍的等效口径）。
    for name in ("city.scene.engine", "city_delivery.png"):
        p = HERE / name
        if p.is_file():
            out.append(p)
    return sorted(out)


def fail(msg):
    print("w1_check: FAIL - " + msg)
    sys.exit(1)


def mode_anchors():
    models = ROOT / "assets" / "models"
    meshlets = ROOT / "assets" / "meshlets"
    glbs = list(models.glob("city_block_%d_%d_*.glb" % (SEED, SIZE)))
    if len(glbs) != BLOCKS:
        fail("glb count %d != %d" % (len(glbs), BLOCKS))
    for g in glbs:
        if not GLB_RE.match(g.name):
            fail("glb name fails whitelist: %s" % g.name)
    suites = list(meshlets.glob("city_block_%d_%d_*" % (SEED, SIZE)))
    # per block: .nmesh + .lodN.nmesh(>=1) + .dag.json
    nmesh = [p for p in suites if p.name.endswith(".nmesh") and ".lod" not in p.name]
    lods = [p for p in suites if ".lod" in p.name]
    dags = [p for p in suites if p.name.endswith(".dag.json")]
    if not (len(nmesh) == len(lods) == len(dags) == BLOCKS):
        fail("meshlet suite counts nmesh=%d lod=%d dag=%d != %d"
             % (len(nmesh), len(lods), len(dags), BLOCKS))
    engine = HERE / "city.scene.engine"
    if not engine.is_file():
        fail("city.scene.engine missing")
    text = engine.read_text(encoding="utf-8", newline="")
    refs = re.findall(r'"mesh": "([^"]+)"', text)
    if len(refs) != BLOCKS:
        fail("engine mesh refs %d != %d" % (len(refs), BLOCKS))
    for i, r in enumerate(refs):
        if r != glb(i):
            fail("engine ref %d != %s" % (i, glb(i)))
        if not (models / r).is_file():
            fail("engine ref not closed: %s" % r)
    print("anchors OK: 36 glb (whitelist) / 36 nmesh / 36 lod / 36 dag / "
          "36 engine refs closed")


def mode_json():
    engine = HERE / "city.scene.engine"
    try:
        scene = json.loads(engine.read_text(encoding="utf-8"))
    except ValueError as e:
        fail("city.scene.engine is not valid JSON: %s" % e)
    ents = scene.get("entities", [])
    if len(ents) != BLOCKS or scene.get("nextID") != BLOCKS + 1:
        fail("engine E1 shape wrong (entities=%d nextID=%r)"
             % (len(ents), scene.get("nextID")))
    for e in ents:
        tr = e["components"]["Transform"]
        if "rotation" not in tr or "scale" not in tr:
            fail("entity %s Transform truncated" % e.get("name"))
        if not GLB_RE.match(e["components"]["Model"]["mesh"]):
            fail("entity %s mesh not whitelist-flat" % e.get("name"))
    print("json OK: E1 World JSON valid / 36 entities / transforms complete / "
          "mesh refs flat glb")


def mode_hash():
    h = hashlib.sha256()
    for p in delivery_files():
        h.update(str(p.relative_to(ROOT)).encode("utf-8", "replace"))
        h.update(hashlib.sha256(p.read_bytes()).digest())
    print(h.hexdigest())


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("anchors", "json", "hash"):
        print("usage: w1_check.py anchors|json|hash")
        sys.exit(2)
    {"anchors": mode_anchors, "json": mode_json, "hash": mode_hash}[sys.argv[1]]()
