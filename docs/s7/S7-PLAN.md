# S7 详细开发路线图：Visibility Buffer + 材质解析 + PBR 直光

> 对应总路线 `docs/ROADMAP.md` 的 S7。预计 3 个里程碑、业余时间 1~1.5 周。
> 告别 GBuffer：光栅 pass 只写一个 64 位值（簇 id + 三角 id + barycentrics），材质在第二 pass 反解。**64 位原子是本步的硬前提**（S0 验过 feature），本步是它的兑现时刻。

---

## M7.0 vis buffer 打包写入（1~2 天）

- [ ] 打包格式（64 bit）：`clusterId(24) | triangleId(24) | depth/id 校验(16)` ——或按 kb/index.html#nanite 01 章的设计，先定稿再动手
- [ ] 光栅 pass 输出改写 vis buffer：`atomicMin` 按深度编码打包值（解决三角形排序）；barycentrics 另存一张 R16G16 pass（或 pack 进同一 atomic 的变体方案）
- [ ] `--mode visbuf` 可视化：clusterId→颜色 / triangleId→颜色 两档

**自检**：visbuf 调试图簇/三角颜色分明、边缘干净无撕裂；遮挡关系正确（atomicMin 生效）。tag：`s7-visbuf`。

⚠️ 常见坑：
- 位打包/解包用位运算宏集中定义——打包错一位，排查半天
- atomicMin 比较的是**打包后的整数**，直接拿 float 深度比没用——按 IEEE 位图可整型比较的技巧（depth 越小整数越小）来设计布局

## M7.1 材质 pass：vertex pulling 反解（2 天）

- [ ] 全屏三角形 pass：读 vis buffer → clusterId 查簇→三角 id 查索引→ **vertex pulling**（fetch 三个顶点）→ 重构插值（pos/normal/uv）
- [ ] 材质 ID 间接表：簇 → 材质实例 → 纹理索引，bindless 或大 bindless 数组
- [ ] PBR 直光（1~2 盏方向光）+ 深度测试用重建深度

**自检**：单材质模型与 M0.4 的传统画法**逐像素一致**（先保证单材质对，再上多材质）。tag：`s7-pulling`。

⚠️ 常见坑：
- 重构插值别忘了 perspective-correct（用屏幕空间 barycentrics 配 w）
- vertex pulling 的 buffer 绑定用 SRV 逐顶点 fetch，别复制顶点到新布局

## M7.2 多材质 + 对照验收（1 天）

- [ ] 多材质模型（≥3 种材质/贴图）跑通间接表
- [ ] 传统 GBuffer 直光路径保留为对照模式

**自检（S7 验收）**：vis buffer 路径 vs 传统直画 GBuffer 对照组**逐像素一致**（模式开关对比，差异截图为证）。tag：`s7-done`，截图 `docs/screenshots/s7.png`（visbuf 调试图 + 一致性对比）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M7.0 vis buffer 打包 | 1~2 天 | `s7-visbuf` |
| M7.1 反解+PBR | 2 天 | `s7-pulling` |
| M7.2 多材质+验收 | 1 天 | `s7-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 64 位打包/atomicMin 设计 | kb/index.html#nanite（01 章可见性缓冲节，含位布局） |
| 工程实现对照 | `Carrot/.../render/VisibilityBuffer.cpp`；`bevy/.../meshlet/visibility_buffer_hardware_raster.wesl`、`visibility_buffer_resolve.wesl`、`material_shade_nodes.rs` |
| 概念与坑 | Carrot《Recreating Nanite: A basic material pass》 |

## 底线

- 从本步起**所有着色走 vis buffer**，传统路径仅作对照保留——两条并存会越养越肥
- 材质间接表的查找函数集中一个头文件：S9/S10/S13 都要复用
