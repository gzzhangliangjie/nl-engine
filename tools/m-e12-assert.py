# -*- coding: utf-8 -*-
# M-E12 机械验收断言段：编辑 undo-redo（工具段 = m-e9-probe.py 已验证 32/32 原版）。
#   ① rename undo/redo 对称（栈深联动读回）
#   ② setProp undo/redo（Transform.position 旧/新值读回，写后回读真值载荷）
#   ③ addEntity undo/redo（重建 id 变化 + 组件保形）
#   ④ removeEntity 子树 undo/redo + 级联 id 重映射机械实证（重映射后再 undo/redo 无 103）
#   ⑤ 空栈 error 102（双向）
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "DX12", "viewer.exe")

def tree(sock):
    tr = result_of(req(sock, "scene.getTree")) or {}
    return tr, {n.get("id"): n for n in (tr.get("nodes") or []) if isinstance(n, dict)}

print("---- M-E12: undo-redo on --preview ----")
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT)); srv.listen(1)
proc = subprocess.Popen([EXE, "--preview", "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("announce received", ann is not None)
    time.sleep(0.5)

    tr, byId = tree(conn)
    check("baseline count=2", tr.get("count") == 2, str(tr.get("count")))
    pm = next((n for n in byId.values() if n.get("name") == "preview-model"), None)
    check("preview-model present", pm is not None)
    pmId = pm.get("id")

    # --- ① rename undo/redo 对称 ---
    r = req(conn, "scene.rename", {"id": pmId, "name": "renamed-A"})
    check("rename ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.undo")
    check("undo ok + depths(0,1)",
          ok_of(r) and (result_of(r) or {}).get("undoDepth") == 0 and
          (result_of(r) or {}).get("redoDepth") == 1, str(result_of(r))[:120])
    _, byId = tree(conn)
    check("undo 读回原名 preview-model", byId.get(pmId, {}).get("name") == "preview-model",
          str(byId.get(pmId, {}).get("name")))
    r = req(conn, "scene.redo")
    check("redo ok", ok_of(r), str(err_of(r))[:100])
    _, byId = tree(conn)
    check("redo 读回 renamed-A", byId.get(pmId, {}).get("name") == "renamed-A",
          str(byId.get(pmId, {}).get("name")))
    r = req(conn, "scene.undo")
    check("second undo ok", ok_of(r), str(err_of(r))[:100])
    _, byId = tree(conn)
    check("second undo 读回 preview-model", byId.get(pmId, {}).get("name") == "preview-model")

    # --- ② setProp undo/redo（Transform.position）---
    r = req(conn, "scene.setProp", {"id": pmId, "component": "Transform",
                                    "name": "position", "value": [1.5, 2.0, 3.25]})
    check("setProp position ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.getProps", {"id": pmId})
    pos = next((p for p in ((result_of(r) or {}).get("props") or [])
                if p.get("name") == "position"), {})
    check("setProp 后读回 [1.5,2,3.25]",
          [round(float(x), 3) for x in (pos.get("value") or [])] == [1.5, 2.0, 3.25],
          str(pos)[:120])
    r = req(conn, "scene.undo")
    check("setProp undo ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.getProps", {"id": pmId})
    pos = next((p for p in ((result_of(r) or {}).get("props") or [])
                if p.get("name") == "position"), {})
    check("undo 后 position 回旧值(非 1.5 系)",
          [round(float(x), 3) for x in (pos.get("value") or [])] != [1.5, 2.0, 3.25],
          str(pos)[:120])
    r = req(conn, "scene.redo")
    check("setProp redo ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.getProps", {"id": pmId})
    pos = next((p for p in ((result_of(r) or {}).get("props") or [])
                if p.get("name") == "position"), {})
    check("redo 后 position 回 [1.5,2,3.25]",
          [round(float(x), 3) for x in (pos.get("value") or [])] == [1.5, 2.0, 3.25],
          str(pos)[:120])
    req(conn, "scene.undo")   # 复原

    # --- ③ addEntity undo/redo（重建 id 变化 + 组件保形）---
    r = req(conn, "scene.addEntity", {"name": "u-lamp", "kind": "light",
                                      "lightType": 1, "intensity": 3.5})
    check("addEntity ok", ok_of(r), str(err_of(r))[:100])
    lampId = (result_of(r) or {}).get("id")
    r = req(conn, "scene.undo")
    check("add undo ok", ok_of(r), str(err_of(r))[:100])
    tr, byId = tree(conn)
    check("undo 后树复原（无 u-lamp）",
          tr.get("count") == 2 and lampId not in byId, "count=%s" % tr.get("count"))
    r = req(conn, "scene.redo")
    check("add redo ok", ok_of(r), str(err_of(r))[:100])
    tr, byId = tree(conn)
    lamp2 = next((n for n in byId.values() if n.get("name") == "u-lamp"), None)
    check("redo 后树含 u-lamp（重建新 id）",
          tr.get("count") == 3 and lamp2 is not None and lamp2.get("id") != lampId,
          "count=%s lampId=%s newId=%s" % (tr.get("count"), lampId,
                                           lamp2 and lamp2.get("id")))
    check("重建灯组件保形 kind=light lightType=1 intensity≈3.5",
          lamp2 is not None and lamp2.get("kind") == "light" and
          lamp2.get("lightType") == 1 and
          abs(float(lamp2.get("intensity") or 0) - 3.5) < 1e-3, str(lamp2)[:140])
    newLampId = lamp2.get("id") if lamp2 else 0

    # --- ④ removeEntity 子树 undo/redo + 级联重映射 ---
    r = req(conn, "scene.addEntity", {"name": "u-child", "kind": "model",
                                      "mesh": "ParkingLot.glb", "parent": newLampId})
    check("add child under rebuilt lamp ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.removeEntity", {"id": newLampId})
    check("remove subtree ok", ok_of(r), str(err_of(r))[:100])
    tr, _ = tree(conn)
    check("remove 后 count=2", tr.get("count") == 2, str(tr.get("count")))
    r = req(conn, "scene.undo")
    check("remove undo (rebuild) ok", ok_of(r), str(err_of(r))[:100])
    tr, byId = tree(conn)
    lamp3 = next((n for n in byId.values() if n.get("name") == "u-lamp"), None)
    child3 = next((n for n in byId.values() if n.get("name") == "u-child"), None)
    check("undo 后子树重建（lamp+child 齐）",
          tr.get("count") == 4 and lamp3 is not None and child3 is not None,
          "count=%s" % tr.get("count"))
    check("重建拓扑保形 child.parent=重建 lamp",
          child3 is not None and child3.get("parent") == (lamp3 or {}).get("id"),
          "child.parent=%s lamp=%s" % (child3 and child3.get("parent"),
                                       lamp3 and lamp3.get("id")))
    # 级联重映射机械实证：栈顶 redo cmd 目标 id 已被 remapStacks 修正为重建 id，
    # redo→undo 循环一轮无 103（引用失效会在 execRedo 炸出）
    r = req(conn, "scene.redo")
    check("redo (remapped id) ok", ok_of(r), str(err_of(r))[:100])
    tr, _ = tree(conn)
    check("redo 后 count=2", tr.get("count") == 2, str(tr.get("count")))
    r = req(conn, "scene.undo")
    check("undo after redo (remapped) ok", ok_of(r), str(err_of(r))[:100])
    tr, _ = tree(conn)
    check("undo 后 count=4（重映射链闭环）", tr.get("count") == 4, str(tr.get("count")))

    # --- ⑤ 空栈 error 102 ---
    for _ in range(20):
        r = req(conn, "scene.undo")
        if not ok_of(r):
            break
    check("undo 空栈 error 102", not ok_of(r) and err_of(r).get("code") == 102,
          str(err_of(r))[:100])
    for _ in range(40):
        r = req(conn, "scene.redo")
        if not ok_of(r):
            break
    check("redo 空栈 error 102", not ok_of(r) and err_of(r).get("code") == 102,
          str(err_of(r))[:100])
    conn.close()
finally:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

print("")
if fails:
    print("FAILED: %d -> %s" % (len(fails), fails))
    sys.exit(1)
print("ALL PASS (m-e12 undo-redo)")
sys.exit(0)
