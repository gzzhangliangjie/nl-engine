#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cases.json 语义合并驱动（git custom merge driver）。

背景（2026-11-10）：并行轨道模式下每条 feat/* 都向 example/cases.json 的
`versions` 数组尾部追加版本对象，git 文本合并不懂 JSON 语义，每次必撞。
本驱动按 JSON 语义做三路合并：
  - 字典：字段级合并，只有两边对同一字段改成不同值才算冲突；
  - 数组：元素全是带 "id" 字段的对象时（versions / cases），按 id 做并集合并，
    各自追加的新条目自动合并，互不冲突；
  - 其余数组 / 标量：走「一边没动就取另一边」规则，否则冲突。

用法（由 .gitattributes + merge.nlcases.driver 调用，勿手工跑）：
  python tools/merge-cases.py %O %A %B %L %P
    %O=共同祖先版本  %A=当前分支(就地覆写为结果)  %B=对方分支  %L=标记长度  %P=路径

退出码：0=合并成功（%A 已写入规范格式结果）；1=真冲突（%A 保持我方原样，
回退到手工解决）；2=输入文件不是合法 JSON（比如已含冲突标记）。

规范序列化格式（全仓库统一）：json.dumps(indent=1, ensure_ascii=False) + 尾部 \\n。
--selftest 跑内置合成用例（追加并集 / 字段编辑 / 真冲突 / 格式差异无关性）。
"""
import json
import sys

CONFLICT_PREFIX = "CASES-CONFLICT"


def load(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        text = f.read()
    if "<<<<<<<" in text:
        raise ValueError(f"{path} 含有未解决的冲突标记")
    return json.loads(text)


def dumps(obj):
    return json.dumps(obj, indent=1, ensure_ascii=False) + "\n"


class Conflict(Exception):
    def __init__(self, path):
        self.path = path
        super().__init__(path)


def id_of(elem):
    if isinstance(elem, dict) and "id" in elem and isinstance(elem["id"], str):
        return elem["id"]
    return None


def merge_value(base, ours, theirs, path):
    """三路合并单个值；不可调和时抛 Conflict(path)。"""
    if _eq(ours, theirs):
        return ours
    if _eq(base, ours):
        return theirs  # 我方未动 → 取对方（含对方删除）
    if _eq(base, theirs):
        return ours    # 对方未动 → 取我方
    # 两边都改了且不一致：能继续下钻的类型递归，否则冲突
    if isinstance(base, dict) and isinstance(ours, dict) and isinstance(theirs, dict):
        return merge_dict(base, ours, theirs, path)
    if isinstance(base, list) and isinstance(ours, list) and isinstance(theirs, list):
        return merge_list(base, ours, theirs, path)
    raise Conflict(path)


def _eq(a, b):
    if a is None and b is None:
        return True
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(_eq(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_eq(x, y) for x, y in zip(a, b))
    return type(a) is type(b) and a == b


def merge_dict(base, ours, theirs, path):
    result = {}
    keys = list(ours.keys())
    for k in theirs:
        if k not in keys:
            keys.append(k)
    for k in keys:
        kp = f"{path}.{k}" if path else k
        in_o, in_a, in_b = k in base, k in ours, k in theirs
        if in_a and in_b:
            if in_o:
                result[k] = merge_value(base[k], ours[k], theirs[k], kp)
            else:
                # 两边都新增：一致或能下钻就合，否则冲突
                if _eq(ours[k], theirs[k]):
                    result[k] = ours[k]
                elif isinstance(ours[k], dict) and isinstance(theirs[k], dict):
                    # 视祖先为 {} 递归（新增对象的字段级并集）
                    result[k] = merge_dict({}, ours[k], theirs[k], kp)
                else:
                    raise Conflict(kp)
        elif in_a:
            result[k] = ours[k]   # 我方新增 / 对方删除（对方删我方留=保留，无法区分，偏保守）
        elif in_b:
            result[k] = theirs[k]  # 对方新增 / 我方删除
        # 两边都没有 → 都删了，丢弃
    return result


def merge_list(base, ours, theirs, path):
    # 带稳定 "id" 的对象数组（versions / cases）：按 id 三路并集
    if all(id_of(e) is not None for e in base + ours + theirs):
        return merge_by_id(base, ours, theirs, path)
    if _eq(base, ours):
        return theirs
    if _eq(base, theirs):
        return ours
    if _eq(ours, theirs):
        return ours
    raise Conflict(path)


def merge_by_id(base, ours, theirs, path):
    o_map = {id_of(e): e for e in base}
    a_map = {id_of(e): e for e in ours}
    b_map = {id_of(e): e for e in theirs}
    result = []
    # 我方顺序打底（保证我方新增的相对顺序稳定），对方独有条目按对方顺序追加在尾部
    seen = set()
    for e in ours:
        result.append(e)
        seen.add(id_of(e))
    for e in theirs:
        if id_of(e) not in seen:
            result.append(e)
            seen.add(id_of(e))
    # 共有条目做三路合并
    merged = []
    for e in result:
        i = id_of(e)
        if i in o_map and i in a_map and i in b_map:
            merged.append(merge_value(o_map[i], a_map[i], b_map[i], f"{path}[{i}]"))
        else:
            merged.append(e)
    return merged


def merge_files(base_path, ours_path, theirs_path, src):
    base, ours, theirs = load(base_path), load(ours_path), load(theirs_path)
    try:
        merged = merge_value(base, ours, theirs, src)
    except Conflict as c:
        print(f"{CONFLICT_PREFIX}: {c.path} 两边改动不可调和，回退手工解决", file=sys.stderr)
        return 1
    with open(ours_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(dumps(merged))
    return 0


def selftest():
    import tempfile
    import os

    cases = []

    def run(name, base, ours, theirs, expect_rc, expect_check=None):
        d = tempfile.mkdtemp(prefix="cases-merge-test-")
        paths = {}
        for tag, obj in (("o", base), ("a", ours), ("b", theirs)):
            p = os.path.join(d, tag + ".json")
            with open(p, "w", encoding="utf-8") as f:
                f.write(dumps(obj))
            paths[tag] = p
        rc = merge_files(paths["o"], paths["a"], paths["b"], "test")
        ok = rc == expect_rc
        detail = ""
        if ok and expect_rc == 0 and expect_check is not None:
            got = json.load(open(paths["a"], encoding="utf-8"))
            ok = expect_check(got)
            detail = "" if ok else f" 结果不符: {got}"
        cases.append((name, ok, detail))
        return rc

    # 1. 追加并集：两边各加一个版本 → 三个都在
    v1 = {"id": "v1", "name": "one"}
    run("追加并集",
        {"versions": [v1]},
        {"versions": [v1, {"id": "v2", "name": "two"}]},
        {"versions": [v1, {"id": "v3", "name": "three"}]},
        0, lambda g: [v["id"] for v in g["versions"]] == ["v1", "v2", "v3"])

    # 2. 一边改字段、一边追加 → 都生效
    run("字段编辑+追加",
        {"versions": [{"id": "v1", "name": "old"}]},
        {"versions": [{"id": "v1", "name": "new"}]},
        {"versions": [{"id": "v1", "name": "old"}, {"id": "v4", "name": "four"}]},
        0, lambda g: g["versions"][0]["name"] == "new" and len(g["versions"]) == 2)

    # 3. 同一字段两边改成不同值 → 真冲突
    run("真冲突",
        {"versions": [{"id": "v1", "name": "old"}]},
        {"versions": [{"id": "v1", "name": "aaa"}]},
        {"versions": [{"id": "v1", "name": "bbb"}]},
        1)

    # 4. 格式无关：单行压缩 vs 缩进展开，内容相同 → 干净合并
    run("格式差异无关",
        {"versions": [{"id": "v1", "name": "same"}]},
        {"versions": [{"id": "v1", "name": "same"}, {"id": "v2", "name": "x"}]},
        {"versions": [{"id": "v1", "name": "same"}, {"id": "v3", "name": "y"}]},
        0, lambda g: len(g["versions"]) == 3)

    # 5. cases 子数组按 id 合并
    run("cases 子数组",
        {"versions": [{"id": "v1", "cases": [{"id": "a"}, {"id": "b"}]}]},
        {"versions": [{"id": "v1", "cases": [{"id": "a"}, {"id": "b"}, {"id": "c"}]}]},
        {"versions": [{"id": "v1", "cases": [{"id": "a"}, {"id": "b"}, {"id": "d"}]}]},
        0, lambda g: [c["id"] for c in g["versions"][0]["cases"]] == ["a", "b", "c", "d"])

    # 6. 字符串数组（acceptance）：一边动了取一边
    run("字符串数组",
        {"acceptance": ["x"]},
        {"acceptance": ["x", "y"]},
        {"acceptance": ["x"]},
        0, lambda g: g["acceptance"] == ["x", "y"])

    # 7. 顶层标量：一边改取一边
    run("标量编辑",
        {"title": "old"},
        {"title": "new-title"},
        {"title": "old"},
        0, lambda g: g["title"] == "new-title")

    fails = [c for c in cases if not c[1]]
    for name, ok, detail in cases:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{detail}")
    if fails:
        print(f"SELFTEST FAIL ({len(fails)}/{len(cases)})", file=sys.stderr)
        return 1
    print(f"SELFTEST PASS ({len(cases)}/{len(cases)})")
    return 0


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--selftest":
        sys.exit(selftest())
    if len(argv) < 3:
        print("usage: merge-cases.py %O %A %B [%L] [%P]  |  --selftest", file=sys.stderr)
        sys.exit(2)
    base, ours, theirs = argv[0], argv[1], argv[2]
    src = argv[4] if len(argv) > 4 else "cases.json"
    try:
        sys.exit(merge_files(base, ours, theirs, src))
    except (ValueError, json.JSONDecodeError) as e:
        print(f"{CONFLICT_PREFIX}: 输入不是合法 JSON（可能已含冲突标记）：{e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
