# S8 详细开发路线图：BLAS/TLAS + Ray Query 最小闭环

> 对应总路线 `docs/ROADMAP.md` 的 S8。预计 3 个里程碑、业余时间约 1.5 周。
> Lumen 的光追地基：用 S3 的 fallback mesh 建 BLAS（每 mesh 一个，预建）、TLAS 每帧重建；compute 里发调试射线验证整条链路通。**这一步做完，后面所有 GI 才有「射线」可用。**
>
> **2026-11-09 预研轮**：三处参考已按对照总表全部读过（Carrot `raytracing/AccelerationStructure.cpp/.h` + `RaytracingScene.cpp/.h` 全文、bevy `bevy_solari/src/scene/blas.rs`、UE `RayTracingScene.cpp` / `RayTracingGeometry.cpp` / `RayTracingGeometryManager.cpp` / `NaniteRayTracing.cpp` 定向调研）+ **vendored NVRHI 头文件 RT 接口面逐签名核查**，结论固化在本文件，开工前不再重复调研。

---

## 预研结论（2026-11-09，三处对照 + NVRHI 接口核查）

### 1. NVRHI RT 接口签名核查（vendored `thirdparty/nvrhi/include/nvrhi/nvrhi.h`）

⚠️ **版本口径修正**：头文件版权头是 2014-2021，但 API 面含 cluster ops / OMM / LSS / CoopVec / RTXMU——是**比「2021 代」新得多的 fork**。旧文档/MEMORY 里「NVRHI 2021 代」的说法按此修正；所有签名以 vendored 头文件为准。

S8 用到的签名（行号为 nvrhi.h 实测）：

| API | 签名/要点 |
|---|---|
| 创建 AS | `IDevice::createAccelStruct(const rt::AccelStructDesc&) → rt::AccelStructHandle`（3928 行）。`AccelStructDesc`：BLAS 用 `addBottomLevelGeometry(GeometryDesc)`，TLAS 用 `setTopLevelMaxInstances(n)`；`buildFlags`、`debugName`、`trackLiveness` |
| BLAS 几何 | `rt::GeometryDesc` → `GeometryTriangles`：`IBuffer*` 顶点/索引 + format + offset + count + **vertexStride（支持交错顶点布局）**；GeometryType 有 Triangles/AABBs/Spheres/Lss；OMM 字段可选不填 |
| 构建 BLAS | `ICommandList::buildBottomLevelAccelStruct(as, GeometryDesc*, numGeometries, AccelStructBuildFlags)`（3642 行）。**必须传与 createAccelStruct 相同的 geometry 描述符**；更新时几何/图元数不得变且需 AllowUpdate |
| 构建 TLAS | `buildTopLevelAccelStruct(as, InstanceDesc*, numInstances, flags)`（3670 行，CPU 数组）或 `buildTopLevelAccelStructFromBuffer(as, IBuffer*, offset, numInstances, flags)`（3688 行，GPU 侧填充实例——UE/bevy 同款路径） |
| 实例描述 | `rt::InstanceDesc` 64B（1783 行 static_assert）：`instanceID`、`instanceContributionToHitGroupIndex`、`instanceMask`、`transform(AffineTransform)`、`flags`、`setBLAS(ptr)` 或 `blasDeviceAddress` |
| 构建旗标 | `AccelStructBuildFlags`：`AllowUpdate / AllowCompaction / PreferFastTrace / PreferFastBuild / MinimizeMemory / PerformUpdate / AllowEmptyInstances`（1786 行） |
| 尺寸查询 | 只有 **TLAS** 有 `IDevice::queryTopLevelAccelStructPrebuildInfo(desc, instanceCount, outInfo)` → `{resultBytes, scratchBytes, updateScratchBytes}`（3849 行）；**BLAS 尺寸由 createAccelStruct 内部按 geometry 描述符自算**，用户不查 |
| scratch | TLAS 构建的 scratch 由**命令列表内建 scratch manager 自动分配**（3665 行注释明说）；BLAS 构建同理但注释未明说，开工时以 Donut AS 示例跑通为准 |
| 压缩 | `copyRaytracingAccelerationStructure`（clone，3656 行）；`compactBottomLevelAccelStructs`（3648 行）**只在 NVRHI 编译带 RTXMU 时生效，否则 no-op**——compaction 不得作为 S8 依赖 |
| 绑定 | `BindingSetItem::RayTracingAccelStruct(slot, as)`（2363 行）；资源类型 `RayTracingAccelStruct` |
| Ray query | **只需把 TLAS 绑进 BindingSet 即可在 compute 里用 Slang `RayQuery`，不需要 RayTracingPipeline/ShaderTable/setRayTracingState**——S8 最小闭环用不到这些（它们留给完整 RT pipeline） |
| 附赠（S13 备用） | `executeMultiIndirectClusterOperation(rt::cluster::OperationDesc)`（CLAS build/BLAS build，3676 行）——**DX12-only，走 NVAPI，Vulkan 不支持**；对照 RTXMG 时注意两后端不对称 |

⚠️ 与本项目约定的交叉检查点：
- AS 相关资源也要进 §2.7 单一 BindingSet（TLAS 是普通 binding slot，不额外开 set）。
- AS 构建（build BLAS/TLAS）是 command list 上的操作，非 dispatch——放 compute 命令列表，与 S6 多 pass 队列的关系开工时核。

### 2. Carrot 口径（`RaytracingScene.cpp`，1182 行全文）

- **BLAS = mesh 列表聚合**：一个 BLAS 可含多 geometry（每 mesh 一个 `GeometryTriangles`，transform 按 geometry 给 device address；全 identity 时共享同一个 identity 矩阵 buffer 地址，省上传，207 行/71-74 行）。
- **构建流水线**（`buildBottomLevels` 456-914 行）：① parallelFor 逐 BLAS 查 `getAccelerationStructureBuildSizesKHR`（需传 primitive counts），scratch 按 `minAccelerationStructureScratchOffsetAlignment` 对齐后累计；② multiAllocate 全部 AS 存储 + scratch 大 buffer；③ BLAS 分 **bucket**（`BLASBuildBucketCount`），每 bucket 一条 cmd buffer 并行录制，一次 compute submit；④ **timeline semaphore**（blasBuildSemaphore → TLAS 等它 → 渲染前 `addWaitSemaphoreBeforeRendering`）串全链——印证本项目「主渲染队列禁止 CPU 等待」的底线，Carrot 全靠 GPU timeline 信号。
- **静态/动态旗标**：静态 = `PreferFastTrace | AllowCompaction`（compaction TODO 未启用，729-730 行硬 `false`）；动态 = `PreferFastBuild | AllowUpdate`。
- **TLAS 策略**：实例数变更/BLAS 新建/实例 dirty → 本帧 **update 模式**（`AllowUpdate` + `PerformUpdate` 语义，srcTLAS 复用）；否则冷却计数（11/20 帧一档）决定 rebuild 还是 update。TLAS 在独立 compute cmd buffer，实例上传 stageUpload + 二值 semaphore，build 完 signal timeline semaphore。
- **伴生逻辑数据**：`SceneDescription::Geometry`（VB/IB/transform 地址 + materialIndex）与 `SceneDescription::Instance`（firstGeometryIndex）两个 buffer 与 AS 平行上传——shader 命中后靠它们反解材质，这正是 S9+ 命中回调需要的结构。

### 3. UE 口径（5.8.4 定向调研）

- **TLAS 每帧无条件全量 rebuild，不存在 update/refit 路径**（`FRayTracingScene::Build`，RayTracingScene.cpp 506-635 行；`DispatchRayTracingWorldUpdates` 每帧先 Update 再 Build）。可调的只有 `r.RayTracing.Scene.BuildMode`（FastTrace/FastBuild）。实例管理靠「持久缓存段 + 每帧瞬时段」两段数组 + per-view 可见位图。
- **BLAS = 每 StaticMesh LOD 一个**（Section 只是其中的 Segment）；主流路径**离线烘焙序列化 BVH**，加载时反序列化免构建；运行时构建走优先级队列（Immediate/High/Normal/Low）+ 每帧图元预算 + 提权防饥饿（RayTracingGeometryManager.cpp 550/1793 行）。
- **异步构建**：`r.RayTracing.AsyncBuild` 把整组 AS pass 放 async compute；同步不靠显式 fence，靠 RDG 资源 access 标签（BVHWrite/BVHRead）自动插 barrier；Nanite BLAS 缓冲作为 TLAS 构建的显式依赖集。
- **compaction**：引擎层只按 0.5 比例估算驻留预算，真实压缩在离线数据或 RHI 层（Vulkan query）——与 Carrot 一样没启用运行时压缩。
- **Nanite fallback 进 RT**（与我们 S8 路线同构）：stream-out 抽可见 LOD fallback 几何 → CPU readback → 组装常规 triangle BLAS，每帧限流（`MaxBlasBuildsPerFrame`）；`bAllowCompaction=false`。

### 4. bevy solari 口径（`scene/blas.rs`）

- BLAS 每 mesh asset 一个 × **opaque/non-opaque 两份**（geometry flags OPAQUE 分离）；mesh extract 变更时重建，`PREFER_FAST_TRACE | ALLOW_COMPACTION`。
- 每帧 compaction 预算 40 万顶点；**删除延迟 2 次 TLAS build**（TLAS 双缓冲 current+previous 供时域效应用）；`on_submitted_work_done` 延迟释放。
- 兼容性门槛：TriangleList + 固定 attribute 集 + indexed 才建 BLAS（358-371 行）。

### 5. 三处口径的取舍（拍板点 D-S8-1）

| 议题 | UE | Carrot | bevy | 建议 |
|---|---|---|---|---|
| TLAS rebuild vs update | **每帧全量 rebuild** | update 优先 + 冷却 | 双缓冲 TLAS + 每帧建 | **每帧全量 rebuild**（UE 口径）：正确性最省心，viewer 实例数少开销可忽略；update 路径（AllowUpdate + PerformUpdate）留作升级项登记 UE-DIFFS |
| BLAS 预建时机 | 离线烘焙为主 | 注册即入队、按需建 | mesh extract 时建 | **启动/资产加载时一次性预建**（原计划保留），构建耗时进 HUD |
| 压缩 | 不做运行时 | TODO 未启用 | 每帧预算做 | **S8 不做**（NVRHI 无 RTXMU 时 API 也是 no-op）；S13 按需再看 |
| 实例填充 | GPU compute 填实例 buffer | CPU 组装上传 | CPU 组装 | **CPU 组装上传**（InstanceDesc 数组 + buildTopLevelAccelStruct）——S8 实例数个位数，不值得上 GPU 填充；FromBuffer 变体留给 S10+ |

---

## M8.0 BLAS 预建（2 天）

- [ ] NVRHI AS 接口落地（预研 §1 签名表为准）：`rt::AccelStructDesc.addBottomLevelGeometry(GeometryTriangles)` + `createAccelStruct` + `buildBottomLevelAccelStruct`；flags = `PreferFastTrace`（静态场景）
- [ ] 每个 mesh 一个 BLAS，用 fallback mesh 几何；`GeometryTriangles.vertexStride` 吃 `.nmesh` fallback 交错顶点布局，`vertexFormat` 声明 position 的 R32G32B32——**开工时先核 fallback mesh 顶点 buffer 是否需要拆 position-only buffer**（不用拆则零拷贝直指原 buffer）
- [ ] AS 构建走 compute 命令列表，构建耗时计时进 HUD；构建后 BLAS device address 供 TLAS 实例引用

**自检**：BLAS 构建成功且无验证层输出（NVRHI validation 开着）；记录构建耗时。tag：`s8-blas`。

⚠️ 常见坑（预研更新）：
- ~~容量猜错是最常见创建失败原因~~（修正：NVRHI 里 BLAS 尺寸 createAccelStruct 内部自算，**没有用户侧 scratch 容量查询**）；真正的坑是 build 时 geometry 描述符与 create 时不一致
- `buildBottomLevelAccelStruct` 后到 TLAS 引用/射线访问之间的屏障由 NVRHI 内建管理，但仍需保证 AS 构建与后续使用在同一 command list 顺序内（跨队列再议）
- fallback mesh 顶点格式与 BLAS 输入格式要严格对齐（AABB 精度影响命中结果）

## M8.1 TLAS 每帧重建（1~2 天）

- [ ] `rt::AccelStructDesc.setTopLevelMaxInstances(场景实例上限)`；每帧填 `InstanceDesc` 数组（transform + setBLAS）→ `buildTopLevelAccelStruct` 全量 rebuild（拍板点 D-S8-1 建议）
- [ ] 先支持静态场景（单实例也行），动态 transform 下帧生效验证
- [ ] TLAS 绑进 §2.7 单一 BindingSet（`RayTracingAccelStruct` slot）

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
| NVRHI API 签名/语义 | **vendored `thirdparty/nvrhi/include/nvrhi/nvrhi.h`**（本文件预研 §1 速查表；行号可直接跳） |
| AS 管理+聚合的工业级写法 | `Carrot/.../render/raytracing/RaytracingScene.cpp`（bucket 并行构建 + timeline semaphore 全链）；`AccelerationStructure.cpp` 只是薄封装 |
| TLAS/BLAS 官方口径 | UE `RayTracingScene.cpp::FRayTracingScene::Build`（每帧 rebuild）、`RayTracingGeometryManager.cpp::ProcessBuildRequests`（优先级队列）；bevy `bevy_solari/src/scene/blas.rs` |
| cluster ops / RTXMG | NVRHI `executeMultiIndirectClusterOperation`（DX12-only/NVAPI）；NVIDIA RTXMG（S13 官方参考，卡壳超 3 天再翻） |
| 概念 | kb/index.html#bridge（03 章 BLAS/TLAS 接口层） |

## 底线

- AS 构建全部异步（专用命令列表），主渲染队列**禁止 CPU 等待**——同步靠 NVRHI 队列提交顺序/timeline 语义，S13 的延迟回读会感谢今天这个纪律
- Ray query 不需要 ShaderTable / hit group / RayTracingPipeline——Slang RayQuery 走查询式，S8 全程零 RT pipeline
- compaction 不进 S8 依赖（RTXMU 缺失时 API no-op）；TLAS update 路径是升级项不是本期目标
