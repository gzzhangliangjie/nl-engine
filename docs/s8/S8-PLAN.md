# S8 详细开发路线图：BLAS/TLAS + Ray Query 最小闭环

> 对应总路线 `docs/ROADMAP.md` 的 S8。预计 3 个里程碑、业余时间约 1.5 周。
> Lumen 的光追地基：用 S3 的 fallback mesh 建 BLAS（每 mesh 一个，预建）、TLAS 每帧重建；compute 里发调试射线验证整条链路通。**这一步做完，后面所有 GI 才有「射线」可用。**

---

## M8.0 BLAS 预建（2 天）

- [ ] NVRHI AS 接口：`IBottomLevelAS` / buildBLAS 相关 API（照 Donut 的 AS 封装读用法）
- [ ] 每个 mesh 一个 BLAS，用 fallback mesh 几何，离线/启动时一次性预建
- [ ] AS 构建走专用 compute 队列（可用的话），构建耗时计时进 HUD

**自检**：BLAS 构建成功且无验证层输出；记录构建耗时。tag：`s8-blas`。

⚠️ 常见坑：
- AS 构建需要 scratch/临时 buffer，大小查询 API 别跳过——容量猜错是最常见的创建失败原因
- fallback mesh 的顶点格式与 BLAS 输入格式要严格对齐（AABB 精度影响命中结果）

## M8.1 TLAS 每帧重建（1~2 天）

- [ ] `ITopLevelAS`：每帧写实例（transform + BLAS 引用），build 快速重建路径
- [ ] 先支持静态场景（单实例也行），动态 transform 下帧生效验证

**自检**：移动一个实例的 transform，下帧射线结果跟随变化。tag：`s8-tlas`。

## M8.2 Ray Query 调试射线闭环（2 天）

- [ ] compute 里 Slang `RayQuery` 对象（SPIR-V/DXIL 都支持 ray query，无需完整 RT pipeline）：每像素从相机发 1 条射线
- [ ] 命中距离 → 灰度可视化；命中 instanceId/triangleId → 伪彩色（两档调试）
- [ ] `--mode rt` 接入模式分发

**自检（S8 验收）**：射线可视化与场景**深度吻合**（边缘、轮廓都对）；HUD 显示 AS 构建/更新耗时。tag：`s8-done`，截图 `docs/screenshots/s8.png`（深度图 vs 命中距离图对比）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M8.0 BLAS 预建 | 2 天 | `s8-blas` |
| M8.1 TLAS 每帧 | 1~2 天 | `s8-tlas` |
| M8.2 调试射线闭环 | 2 天 | `s8-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| AS 管理+聚合的工业级写法 | `Carrot/.../render/raytracing/AccelerationStructure.cpp`、`RaytracingScene.cpp`（本项目最值钱的对照文件之一） |
| 官方用法 | NVRHI / Donut 光追示例；nvpro ray tracing samples |
| 簇式几何加速 BVH（卡壳超 3 天再翻） | NVIDIA **RTXMG**（RTX Mega Geometry，MIT）——S13 的官方参考，先收藏 |
| 概念 | kb/index.html#bridge（03 章 BLAS/TLAS 接口层） |

## 底线

- AS 构建全部异步（专用队列 + fence），主渲染队列**禁止等待**——S13 的延迟回读会感谢今天这个纪律
- Ray query 的 SDF hit Shader Record（hit group 表）S8 不需要——本步只用 closest hit 信息，Slang RayQuery 走查询式，简单一半
