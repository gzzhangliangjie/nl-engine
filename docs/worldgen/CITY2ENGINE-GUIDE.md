# CITY2ENGINE-GUIDE · M-W1.0 引擎交付驱动步归档（worldgen 轨道）

> 工具：`scripts/city2engine.py`（Python stdlib 零依赖，照 `scripts/obj2glb.py` 先例）。
> 计划：`W1-PLAN.md`；方案矩阵与拍板：`W1-OPTIONS.md` §5。
> 展示台：`example/cases.json` 卡 `w1-m0`（第 125 张，`kind:"tool"`，exe=`{python}` token）。

## 1. 链路

```
example/w0-m3/blocks/block_NN.obj   ──obj2glb.py(in-process)──▶  assets/models/city_block_<seed>_<size>_NN.glb
                                    ──nanite-tools cluster─────▶  assets/meshlets/...NN.nmesh
                                    ──nanite-tools lod─────────▶  assets/meshlets/...NN.lodN.nmesh
                                    ──nanite-tools dag─────────▶  assets/meshlets/...NN.dag.json
example/w0-m3/city.scene            ──纯文本重写(仅 meshFile)──▶  example/w1-m0/city.scene.engine
全部产物                            ──真实字节驱动────────────▶  example/w1-m0/city_delivery.png
```

- 双轨（Q-W1A）：city-tools C++ 本体与 w0 产物（OBJ/roadnet/PNG）一字不动；`city.scene.engine` 是唯一派生场景，差异仅 meshFile 值。
- 扁平命名（Q-W1B）：`city_block_<seed>_<size>_NN.glb` 天然过 viewer/renderer 白名单（纯 ASCII 文件名 + `.glb`），两处消费代码零改动。seed/size 维度编码进文件名防不同参数集撞车。
- nanite 套件是资产层收口（W1-B 判据）：scene 渲染消费 glb 顶点展开直渲（不经过 .nmesh）；.nmesh/LOD/DAG 由 nanite-tools 链产出供 cluster/cull 路径与后续 milestone 消费。

## 2. 确定性契约

- **同 seed + 同输入 → 同路径双跑逐字节一致**（146 文件聚合 sha256 门禁，run.bat 段 5 机检）。
- nanite 调用统一 `cwd=repo root` + **repo 相对路径**：`.dag.json` 内嵌 `prefix` 字段是输出前缀本身，绝对路径会让它随机器/cwd 漂移；相对路径约束后稳定。跨工具树（DX12 vs VULKAN nanite-tools）交叉验证过逐字节一致。
- `city.scene.engine` 用正则重写 `"mesh": "blocks/block_NN.obj"` → glb 名，读写都 `newline=""` 保真换行——除 mesh 值外与冻结 scene 逐字节一致。
- 证据图 `city_delivery.png`：block 网格（列=x 行=z，从 Transform 世界位置反推）× glb 字节数归一化亮度，stdlib PNG 写出器（zlib+struct），双跑确定性。

## 3. 白名单镜像与机检

`w1_check.py`（example/w1-m0/）三模式：`anchors`（36 glb 白名单 + 36×3 meshlet 套件 + engine 引用闭合）、`json`（engine 是合法 E1 World JSON、Transform 完整、mesh 全扁 glb）、`hash`（聚合 sha256）。run.bat 七段：selftest / drive / anchors / json / 同路径确定性 / usage / negative。

## 4. exe_for `{python}` token（2026-11-13）

Python 驱动工具卡没有 per-backend exe：`tools/showcase/server.py exe_for` 新增 `"{python}"` → `sys.executable`。**卡的每个用例 args 必须前置 `{root}/scripts/city2engine.py`**（exe 是解释器本体，漏脚本路径 = python 把第一个 token 当脚本名 exit 2，usage 用例更是裸 REPL 超时）。bat 垫片方案否决（cmd GBK 输出 + cwd 漂移），`.py` 直跑否决（WinError 193）。

## 5. city.scene 截断 bug（M-W1.0 实锤修复，2026-11-13）

`city-tools/main.cpp serializeCityScene` 的 `char buf[192]` 装不下单实体文本（实需 ~280B）：snprintf 截断在第 191 字符（恰在 `"position": [-13.75` 后），36 实体全伤、city.scene 是非法 JSON。**截断是确定性的 → w0 时代 fc 逐字节段与 findstr 锚全绿、无人发现**；viewer（M-W1.1 `--scene`）才是第一个 JSON 解析消费者。

- 修复：`buf[192]`→`buf[512]` + selftest 72→74（rotation 尾段计数 == 36 / 花括号配平，两条 JSON 结构硬闸）。
- 重拍：w0-m2/m3 仅 city.scene 变化（OBJ/PNG/roadnet 逐字节不动），`--only w0-m2 --only w0-m3` 8/8 bytes ALL GREEN。
- 用户拍板：违反 Q-W1A 冻结面走「bug 非基线语义」条款（AGENTS.md 冻结基线重开须单独拍板），2026-11-13 拍板「现在修」。
- 教训：**冻结产物若从未被解析消费，结构有效性断言缺位会让「确定性 + 锚全绿」掩护真 bug**——工具产物凡有 JSON/结构化格式，验收必须含一次真实解析。
