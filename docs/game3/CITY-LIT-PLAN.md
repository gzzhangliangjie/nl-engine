# M-CITY-LIT PLAN — 白天城市 + 探针 GI 接进 ECS 城市链

> 轨道：game（feat/game，wt `E:/UE_STUDY_2026/nl-engine-game`）。
> 背景：M-GAME1 收官后用户看基线图拍板「画面太挫——白天+光照+GI（手写 Lumen）必须用上」
> （两次点名「谁允许你不用 Lumen」「这项目就是为了还原 nanite 和 lumen 做的」）。
> M-GAME2（AI 车体物理化）按原 PLAN 顺延，GAME2-PLAN.md 保留待复工。

## 1. 拍板记录（2026-11-17 深夜，用户连续拍板）

1. 三候选（A 车体物理化/B 尾灯/C 手感）→ 视觉包优先（AskUserQuestion 拍板）
2. 「弄个白天搞点光」→ 白天口径（AskUserQuestion 选项④被用户直接点名）
3. 「Lumen 你得用上」×2 → **探针 GI 必须接进城市链**（不再询问，直接立项）
4. 「地面完全没有」→ 地面板必须第一时间补上

## 2. 三处证据（实读）

1. **ROADMAP S10~S12** = 本仓手写 Lumen：屏幕探针一跳 GI（probe_trace/gather +
   octahedral irradiance atlas + world_cache bounce + ReSTIR/filter），验收
   「天光与一次弹射可见」已达成（viewer --mode probe 链）。
2. **ECS scene 链（M-E1b）**：pscene 直光 = lights 数组（g_lights，方向灯带
   shadow 槽），ambient = g_lights[0].y；天气链（M-WEA1）只调制 ambient/天空/雾；
   **brightness carrier = light.color**（Lo_dir × 1/ΣI 消掉 intensity，E1b 口径
   「ambient 0.55 / 灯 color ×2.8」）。
3. **TLAS 现状**：Scene::initTlas 依赖 BLAS（fallback 资产/cluster BLAS）；
   ECS 城市场景无自身 BLAS → 城市无 TLAS → 探针 trace 无从打城市几何。
   batch 几何（vb/ib/instances）全在 sceneBatches_，BLAS 可直接吃。

## 3. 两阶段设计

### 第一阶段（已完成，commit `f59f6fd5`）：白天链

- game.exe CLI：`--day`（正午 t=300，kDayLength=600s）/`--weather-*`/`--sky-bg`/`--ground`
- 天气链全字段进 FrameParams（viewer M-WEA1 同款，固定 t 确定性）
- 太阳方向灯注入 lights 首位（M-E1b.4 阴影槽 0，color×8 增益对齐 E1b 口径）
- 地面板：同共享盒 mesh 压板（bbox+60% margin，顶面 y=0）
- 已实证：白天取景双后端 PNG max diff=1（0 像素>8）；game-m0/m1 回归 12/12 bytes ALL GREEN

### 第二阶段（本轮开工）：探针 GI 接进 ECS 城市链

1. **ECS batch 几何 → BLAS**：每 batch 一个 BLAS（吃 batch.vb/ib，PreferFastTrace）；
   城市 = 盒 mesh batch（150 实例）+ 塔/车/灯 mesh 各一 BLAS。initScene 内 additive。
2. **TLAS**：城市实例数（~170）级 TLAS，每帧 rebuild（M8.1 UE 口径沿用）。
3. **探针布点**：城市 AABB 水平网格（间距 ~24m）+ 高度 2 层（y=2/25）；
   S10 trace 每 probe 1 条余弦射线 → irradiance atlas（现成 shader/binding 面）。
4. **pscene 采样**：ambient 项 = 现常量 → 探针 irradiance 三线性采样（octahedral
   atlas）+ 保留常量 ambient 兜底（探针 miss/未收敛帧）。
5. **确定性**：探针布点/trace 均为纯函数+固定帧序；game 基线双跑逐字节。
6. CLI：`--gi 1` 显式激活（缺省 0 = 白天直光口径保形，第二阶段基线分文件）。

## 4. 验收判据

1. 第二阶段基线（--gi 1）：pscene 收到探针 irradiance——楼立面有间接光层次
   （背光面不再是纯黑/纯 ambient）；双后端 PNG 视觉逐像素等价（max diff≤2
   口径，PCF 同款浮点微差；收口时量化报告）。
2. `--gi 0`（缺省）：第一阶段白天基线 + game-m0/m1 基线 bytes 全保形。
3. phys-tools/selftest：探针 trace 确定性（同帧序双跑 irradiance atlas 逐位）。
4. run.bat + cases.json game-m2-lit 卡（白天+GI 取景）。
5. ctest 双后端全绿；viewer 既有 probe 链基线零回归（probe 链 shader 不动）。

## 5. 坑清单（本里程碑实测沉淀）

1. **ECS 灯亮度载体是 color**：intensity 被 1/ΣI 归一化消掉；E1b 口径 color×2.8。
   太阳灯 color×8 才是白天亮度。
2. **正午 elev=90° 顶光立面物理全黑**：取 t=200（elev 45°）。
3. **kDayLength=600s**（不是 86400）：正午 t=300。
4. **CG2 city.scene.engine 无地面实体**：game 层地面板注入（数据层冻结不动）。
5. **阴影视锥扇形 artifact**（大俯瞰中央亮斑）：疑似 lightViewProj AABB/深度
   精度，收口前查（挂账）。
6. 白天链双后端 max diff=1：PCF/雾浮点微差，收口定「视觉逐像素等价」口径。
