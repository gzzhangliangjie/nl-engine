# S5 详细开发路线图：compute 遍历 DAG，按屏幕误差选 cut

> 对应总路线 `docs/ROADMAP.md` 的 S5。预计 3 个里程碑、业余时间约 1.5 周。
> Nanite 的心脏：不再「按层画」，而是从 DAG 根出发遍历——一个簇够粗（屏幕误差 ≤ 阈值）就画它，否则下钻子簇。相机拉远时三角数**连续**下降就是它在工作。

---

## M5.0 误差投影：世界 → 屏幕像素（1 天）

- [x] 公式（2026-11-08 拍板修正）：bevy/zeux 口径 `screenPx = worldError / max(d, near) × projYScale × 0.5 × H`，`d = |球心−eye| − 球半径`（保守取球最近点）。~~原文「distance 用簇球心到相机平面距离」与 M3.2 已存档的 bevy 公式冲突，弃用~~——与 S3 error chain 同源；与 UE `GetProjectedEdgeScales` 切线投影的差异记 `docs/s5/UE-DIFFS.md` D1
- [x] 单元测试：`core/src/lod_error.cpp` `lodErrorSelfTest()`（45° fov 手算逐例对比，进 viewer `--selftest`）
- [x] 阈值参数进 ImGui（`cut thr px` 滑条 0.25~4.0，默认 1.0；HUD 加 `s5 err` 实时行）

**自检（2026-11-08 实测通过）**：dag-top + HUD 三机位（dist 2.0/4.2/8.0）屏幕误差 1567.05 → 352.15 → 149.84 px 单调下降，与 Python 按资产真实 maxError=1.1375、联合球 R=1.2661、eye 公式离线手算**逐位一致**；数值复核 + 26 张 HUD 基线重拍 + 3 条关键用例回归 6/6 bytes。tag：`s5-proj`（详见 MILESTONES M5.0 纪要）。

## M5.1 compute DAG 遍历（2~3 天，本步核心）

- [x] 遍历策略起步用**简单版**：compute 每线程持一个显式栈（局部数组），从根组出发：簇屏幕误差 ≤ 阈值 → 写 indirect 命令；否则把子簇压栈（`renderer/shaders/dag.slang`；成员 CSR 拍平——lod0 实测成员方向不连续，不能断言区间；D3a 与 bevy 全局队列的差异记 UE-DIFFS）
- [x] 栈溢出保护：超出深度直接画当前簇（宁粗勿漏）；组重复下钻用 `dagGroupExpanded_` + `InterlockedExchange` 声标志去重（同组多个同误差父簇全失败会重复压成员，实测 54923 emits 放大）
- [x] 数据布局：DAG 组/子引用做成 GPU 可索引的扁平数组（S3 的 v2 格式在此兑现）：全层拼接 vertexPool/localIndices/clusterMeta + 逐簇 error/level/genGroup SSBO + 组记录 float4×2；DagConstants 独立 volatile CB 48B，不动 SceneConstants 160B 契约

**自检（2026-11-08 实测通过）**：`--mode dag` 选层谱单调（dist 20，thr 4/8/16/32/64 → 230/106/14/7/3 簇）；**全细节等价**：`--cut-thr 0.001` 与 `--mode cull --frustum off` 同机位逐字节相等（md5 f0b5db63，双后端各自成立且 dag 图跨后端也逐字节相等）；视觉验收 thr=8/16 无洞无裂缝；回归 s4.0/s5.0/s3.2/s4.1 + 新卡 s5.1 全 bytes。tag：`s5-traverse`（踩坑见 MILESTONES M5.1 纪要）。

⚠️ 常见坑：
- **洞**的来源几乎都是：父簇被画时其子簇区域本应被父覆盖、但父简化时边界没对齐——回到 S2 检查边界锁，别在遍历侧修
- 遍历里读 DAG 引用注意共享子簇只画一次（父替换语义：选父则子不画；兄弟组共享子簇时按引用计数去重）

## M5.2 时滞防抖 + HUD（1~2 天）

- [ ] 误差阈值加滞回（hysteresis）：上帧选中的簇，本帧误差只要 < 阈值×1.1 就不换——抑制边界抖动
- [ ] HUD 实时显示当前总三角数（GPU 原子计数 + 延迟回读，复用 S4）

**自检（S5 验收）**：相机拉远三角数**连续**下降（不是跳变）；固定视角数字稳定不抖。tag：`s5-done`，录屏或连拍 `docs/screenshots/s5_*.png`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M5.0 误差投影 | 1 天 | `s5-proj` |
| M5.1 DAG 遍历 | 2~3 天 | `s5-traverse` |
| M5.2 防抖+HUD | 1~2 天 | `s5-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| cut 选择精确数学 | UE 源码 `NaniteCullRaster.cpp`（你已拉取；只看 cut 判定和误差投影） |
| 简化遍历策略对照 | `bevy/.../meshlet/cull_bvh.wesl`（bevy 的 BVH 版遍历，结构可借鉴） |
| 公式与防抖 | kb/index.html#algo（04 章 §4.1 LOD 选择）；Karis《Nanite: A Deep Dive》 |

## 底线

- 遍历 compute 的输出接口 = S4 的 indirect buffer，**不另起炉灶**——S6 的第二遍剔除直接复用
- 出现洞先查 S2 边界锁、再查遍历语义，顺序别反（80% 是前者）
