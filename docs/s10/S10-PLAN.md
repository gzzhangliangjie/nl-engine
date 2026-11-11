# S10 详细开发路线图：屏幕探针一跳 GI

> 对应总路线 `docs/ROADMAP.md` 的 S10。预计 4 个里程碑、业余时间约 2 周。**P5 最难段的开始。**
> Lumen Screen Probe：在屏幕可见点上稀疏布探针，每探针存八面体辐照度图（每 texel 一条方向射线），final gather 逐像素插值。先做一跳（直接天光+直光面反弹一次），世界缓存 S11 再补多跳。

---

## M10.0 Cascade 布点（2 天）

> **M10.0 拍板记录（2026-11-11 用户拍板「尽量按照 UE 的思路来」）**：
> - **Q10-1 velocity 写入形态 = 独立 velocity prepass pass**（UE VelocityDepthStencil pass / bevy prepass 口径）：单独一条小 pass 画一遍几何，双帧矩阵 + `unjittered` 剥离 jitter 写 `velocity_`（RG16_FLOAT，T0 已建），天空像素补写 0；不碰 6 组按 hdrInfo 重建的 PSO/framebuffer；compute 主导模式（cull/dag/hiz viz）与探针布点均只对 raster 场景模式生效，viz 模式不开 velocity prepass。方案 B（主 pass 附加 MRT）否决。
> - **Q10-2 探针 cascade 数据形态 = 每级一张 R32G32B32A32 atlas 小纹理 + 上一帧同规格 ping-pong**（UE 以 atlas 纹理存 probe 位置/深度的口径）：4 级（16×16→2×2）各存 `worldPos.xyz+depth`，共 8 张微纹理；**法线不落盘**，探针处法线用 `gbuffer_common.slangh` 即取即算（守 Q4 冻结面）。方案 B（StructuredBuffer）、C（不持久化，重投影落空）否决。
> - **口径备注**：UE 屏幕探针实为单级均匀网格（DownsampleFactor=16）+ 自适应加密，屏幕空间无 cascade；4 级 cascade 是本项目自设计（自检目标「近密远疏」），GI-RESEARCH-NOTES §1 已确认，自适应加密列 S14 可选升级。

- [ ] 4 级 cascade：逐级分辨率减半、间距加倍（16×16 起步，参数进 ImGui）
- [ ] 探针位置：屏幕像素反投影 → 与深度求交 → 沿法线偏移；重投影上一帧探针（velocity 由 **T0 里程碑**落地，直接读 T0 的 `RG16_FLOAT` velocity 纹理，不再走 M12 简化版占位；写入形态按 Q10-1 独立 prepass 落地）
- [ ] 探针参数表（级数/分辨率/间距）集中头文件

**自检**：探针位置可视化（画小点）覆盖屏幕、近密远疏。tag：`s10-place`。

## M10.1 八面体图 + 探针追踪（2~3 天）

> **M10.1 拍板记录（2026-11-11 用户拍板「按推荐的来」，三处参考实读：UE LumenScreenProbeGather.usf/.cpp + bevy initial_path.wesl + Carrot 无屏幕探针追踪对应物）**：
> - **Q12-1 探针射线预算 = 每探针每八面体 texel 1 条方向射线（8×8=64 条/探针）**——UE 实锤口径（线程数 = 探针数 × TracingOctahedronResolution，默认 8）；PLAN 旧字面「每探针 1 条」与八面体图矛盾（1 条填不满图），勘误。备选（1 条+时间轮转多帧填图）否决——M10.1 就要历史混合且收敛慢。预算 340 探针 × 64 ≈ 2.2 万 trace/帧 + 命中点 shadow ray 同量级，M9.2 实跑 3900 万/帧无压力。
> - **Q12-2 八面体分辨率 = trace 与存储同分辨率 8×8 + 1 texel border（10×10 存储 texel/探针）**；UE 分两步（trace 8×8 → integrate 16×16）是为 importance sampling filtering，第一步不需要。备选 16×16（射线 ×4 且无滤波意义）否决。
> - **Q12-3 天空函数 = 解析渐变天穹**（zenith/horizon 两色 + 方向余弦混合，bevy atmosphere 简化版，落 slang 头 S10/S12 复用）：探针 miss 与 M10.2 天光共用；**渲染背景暂不替换**（仍 clearColor，M10.3 可选切换）。
> - **已定项（复用既有拍板）**：命中点直光 = M9.1 口径（albedo × 主灯 shadow ray，每 texel 射线命中后再发 1 条阴影射线）；命中点 albedo = 统一模型色——如实降级备案：TLAS 的 BLAS 是 M9.1 S3 fallback 104-tri 粗代理，primitive→细材质查不了（M7.2 GPUMaterial 表挂真网格簇）；探针辐照度 atlas = 每级一张 RGBA16F atlas（每探针 10×10 格），**M10.1 不做时间混合**（单份不 ping-pong，降噪归 M10.3/S14）；射线走 M8 RayQuery compute + M9.x TLAS 惯例；`--mode probe-viz` 复用 M10.0 dots 管线（点色改按「探针→眼睛」方向采样八面体辐照度，probe-place 保持级别色点保形）。

- [ ] 八面体映射：方向→八面体 uv（**8×8 texel + 1 texel border，按 Q12-2**），集中一个 slang 头文件（S12 复用）
- [ ] 探针 pass：每探针每 texel 1 条方向射线（**按 Q12-1，64 条/探针**），命中 → 查命中点直光辐照度（M9.1 口径 shadow ray）写入八面体图；未命中 → 天空渐变色（**按 Q12-3**）
- [ ] `--mode probe-viz`：直接显示探针辐照度颜色（点色 = 八面体图按视线方向采样）

**自检**：探针颜色方向合理（朝天亮、朝暗处黑），天光渐变正确。tag：`s10-probe`。

⚠️ 常见坑（2026-11-09 精读补强，详 `docs/s10/GI-RESEARCH-NOTES.md` §1）：
- 八面体映射的**边界 texel 卷绕**（seam）——UE 的做法是**core + 1 texel border ring**（分辨率 +2），边界 texel 在生成 mip 前就 `OctahedralMapWrapBorder` 回绕到对侧，mip 链在带边框图上逐级做；采样时 seam 无缝。我们的八面体 slang 头按此布局
- 探针射线命中点取直光辐照度时，先用「命中点 albedo×直光」的简化 irradiance，别急着上递归
- 探针射线 tMax 别设太小：bevy 注释实锤「截断处 miss ≠ 真 miss，截太小会漏天光」

## M10.2 Final Gather（2 天）

- [ ] 全屏 pass：每像素找最近 4 个同级探针 → 查八面体图按方向插值 → 权重（距离+法线一致性）加权
  - 📋 预研证据（2026-11-09）：UE 的具体公式 = **双线性 corner 权重 × 平面深度权重**——后者是探针世界位置到「像素法线+位置」定义平面的距离的 `exp2(k·(d/depth)²)`，且带一组指数更弱的 FallbackWeights 降级链路。即「距离+法线一致性」的具体化，按 UE 口径实现（对齐官方）
- [ ] 与直光合成输出

**自检**：画面出现天光间接照明；角落不再死黑。tag：`s10-gather`。

## M10.3 参考图对比验收（1~2 天）

- [ ] `--mode probe` 完整链路 + ImGui 调 cascade 参数
- [ ] **与 S10 自立的 GI 参考图并排对比**（读盘同屏显示）。**Q3 拍板勘误**：S9 参考图只算直光一跳，不含间接光，仅可作**直光分量**对照，不能当 GI 尺子

**自检（S10 验收）**：天光与一次弹射可见；与 **S10 自立 GI 参考图**误差趋势一致（亮暗分布对，允许噪声差）；探针辐照度方向合理。tag：`s10-done`，截图 `docs/screenshots/s10.png`（画面+参考图并排）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M10.0 布点 | 2 天 | `s10-place` |
| M10.1 八面体+追踪 | 2~3 天 | `s10-probe` |
| M10.2 Final gather | 2 天 | `s10-gather` |
| M10.3 对比验收 | 1~2 天 | `s10-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| final gather 与探针布局 | `Carrot/.../lighting/gi/read-screen-probes.slang`；UE `LumenScreenProbeGather*`（你已拉源码） |
| prepare 与初始路径 | `bevy_solari/src/realtime/prepare.rs` + `initial_path.wesl` |
| 概念 | kb/index.html#lum（02 章屏幕探针/八面体映射/final gather 节）；Carrot GI 长文 |

## 底线

- 探针数量上限、射线预算全部常量化——S11 世界缓存会在同一帧里加更多射线，预算爆了先砍探针 texel 再砍数量
- 一跳不干净（趋势都错）就别进 S11——世界缓存会放大而不是修复错误
