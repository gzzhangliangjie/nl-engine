# S15 详细开发路线图：Virtual Shadow Maps（虚拟阴影贴图）

> 对应总路线 `docs/ROADMAP.md` 的新增研究轨道 S15。**插入位置：S9 之后（阴影阶段 rasterized 对照路线，合并原 `shadow-map` 穿插项）**。预计 3 个里程碑、业余时间 1.5~2 周。
> 目标：用「clipmap 级联 + 物理页池 + 页表」替代传统 shadow map / S9 的光追阴影射线，得到随距离无缝、可复用 Nanite 簇渲染路径、能与 GPU-driven 共存的阴影系统。

---

## 三处参考口径（2026-11-08 按铁律定向读）

- **UE5（唯一权威，只读不抄）**：`Engine/Source/Runtime/Renderer/Private/VirtualShadowMaps/`
  - `VirtualShadowMapArray.cpp` —— `FVirtualShadowMapArray`、`PhysicalPagePool`、`MaxPhysicalPages`（页池上限）、clipmap 级数、投影数据打包。
  - `VirtualShadowMapClipmap.cpp` —— 级联 clipmap 布局（按距相机分 N 级）。
  - `VirtualShadowMapProjection.cpp` —— OnePassProjection / 投影采样（世界坐标→页坐标→采样物理页深度比较）。
  - `VirtualShadowMapCacheManager.cpp` —— 页请求/分配/缓存/回收（跨帧复用有效页）。
- **bevy**：meshlet 管线仅有基础 shadow，无 VSM 等价实现（待开工按铁律在 `bevy_pbr/src/meshlet/` 二次确认）。
- **Carrot**：聚焦几何+GI，无 VSM 实现（同上待确认）。

> 本模块以 UE 为唯一 read-only 对照；bevy/Carrot 仅作「是否需要对齐」的否决检查。UE 代码一行不进 repo（EULA）。

## 前置（已具备，依赖 S4+S7/S9 在 S9 已全部就绪 → 插入 S9 之后，无需等 Lumen）
- **S4 硬件光栅 + GPU 视锥剔除 + indirect draw**：VSM 的阴影深度渲染直接复用 S4 的簇渲染/剔除路径，只把渲染目标从屏幕换成物理页池。
- **S7 vis buffer / S9 GBuffer**：最终着色采样 VSM 投影结果。
- VSM 与 S9 光追阴影射线是「阴影的两条平行路线」，互为验收对照，不互相替代。

## M15.0 单级 VSM 框架：物理页池 + 投影采样（2~3 天）
- [ ] 建立物理页池（Physical Page Pool）：一张大 depth 图按 PageSize 切页，`MaxPhysicalPages` 上限（对位 UE `r.Shadow.Virtual.MaxPhysicalPages`）。
- [ ] 单光源 clipmap 一级：把该光源视锥覆盖区渲进若干物理页（先整页分配，不做动态请求）。
- [ ] 投影采样 shader：世界坐标 → 光源 clipmap 页坐标 → 采样物理页深度比较。
- [ ] 先把阴影画到屏幕，替代 S9 射线阴影做对照。

**自检**：单方向光阴影形状/软硬与 S9 参考图方向一致；关掉 VSM 退回 S9 路线时画面退化可见。tag：`s15-vsm`。

## M15.1 级联 clipmap + 页请求/分配/回收（3~4 天）
- [ ] 多级 clipmap：按距相机距离分 N 级（对位 UE `VirtualShadowMapClipmap`），近处高分辨率、远处低分辨率。
- [ ] 每帧页请求 → 分配物理页 → 渲染 → 未命中/失效页回收（对位 UE `VirtualShadowMapCacheManager`：缓存有效页跨帧复用）。
- [ ] HUD 显示物理页占用 / 各级分辨率。

**自检**：大场景近处清晰、远处不糊不抖；相机前后移动时各级分辨率过渡平滑无突跳。tag：`s15-clipmap`。

## M15.2 Nanite/簇 间接阴影 + 去闪烁（2~3 天）
- [ ] 阴影深度渲染复用 S4 的 GPU-driven 间接绘制（簇级剔除/indirect），不另写一遍提交。
- [ ] temporal / rejection 去闪烁：阴影页跨帧稳定，边缘不闪（参考 UE `ShadingRejection` 思路，但本模块只做阴影页稳定性，不搬 TSR 整套路）。
- [ ] 双后端：DX12 / Vulkan 同图，分档 bytes/near。

**自检**：阴影边缘静止稳定、移动无 ghosting；与 S9 参考图并排误差趋势一致；`regression.py` 全绿；kStage bump 后重拍 m0.9/s2.0 HUD 基线（M3.2 固化④）。tag：`s15-done`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M15.0 单级页池+投影 | 2~3 天 | `s15-vsm` |
| M15.1 级联+页管理 | 3~4 天 | `s15-clipmap` |
| M15.2 间接阴影+去闪烁 | 2~3 天 | `s15-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 物理页池/页表结构 | UE `VirtualShadowMaps/VirtualShadowMapArray.cpp`（`FVirtualShadowMapArray::Initialize` 页池分配、`MaxPhysicalPages`） |
| 级联 clipmap 布局 | UE `VirtualShadowMaps/VirtualShadowMapClipmap.cpp` |
| 投影采样/OnePassProjection | UE `VirtualShadowMaps/VirtualShadowMapProjection.cpp` |
| 页缓存/请求管理 | UE `VirtualShadowMaps/VirtualShadowMapCacheManager.cpp` |

## 底线
- VSM 是「阴影路线」不是「渲染路线」：绝不碰 vis buffer / GBuffer 主路径，只新增阴影深度目标 + 投影采样；S9 光追阴影作为对照保留。
- 物理页池是核心资源，slot 级同步表必须在 `createTargets()` 初始化（沿用渲染约定，防首帧 139 段错误）。
- 每次 kStage bump 必须重拍 m0.9 gate-hud 与 s2.0 lod-hud 两对基线（M3.2 固化④）。
- UE 代码一行不进 repo；参考结构与边界处理思路。
