# P8 并发地基：Job / Task 并行系统（A 类并行轨道，提前立项）

> 对应总路线 `docs/ROADMAP.md` 的 **A 类并行轨道**（引擎化 / 非渲染，不进 `kStage`）。本计划把 P8 引擎化的「并发地基」前置为独立可验收轨道——它**现在就能开**（纯底层、零 GPU 依赖），第一个真实消费者是**离线管线**（`nanite-tools` / `core` 离线部分）。
> 性质：S*-PLAN 风格里程碑草案。立项前已按铁律读三处参考（bevy_tasks / UE TaskGraph / Carrot）+ 开发历史（MILESTONES / ROADMAP / MEMORY / S-PLAN 格式样张 `s15/S15-PLAN.md`）。

---

## 三处参考口径（2026-11-08 按铁律定向读）

- **bevy_tasks（最贴，主参考）**：`bevy/crates/bevy_tasks/src/`
  - `TaskPool` + `TaskPoolBuilder`（num_threads / stack_size / thread_name / on_thread_spawn·destroy 生命周期回调）—— 固定大小线程池。
  - `pool.scope(|s| s.spawn(async {...}))` —— **scoped fork-join**，生命周期受限借用，返回 `Vec<T>`，天然适合「一批任务并行 + 汇合」。支持嵌套 spawn。
  - `ParallelSlice` / `ParallelSliceMut` / `ParallelIterator` —— **slice 数据并行原语**，正是把 cluster / simplify / dag 的循环并行化的直接工具。
  - `AsyncComputeTaskPool` / `ComputeTaskPool` / `IoTaskPool` —— 按工作类型分 named pool（compute vs IO），对应我们「计算并行 vs 资产流式预取」分流。
  - 模型：**work-stealing executor**，对任务数 / 核数不敏感。
- **UE TaskGraph（工业终态，长线对标）**：`Engine/Source/Runtime/Core/Public/Async/`
  - `FTaskGraphInterface` + `ENamedThreads`（GameThread / RenderThread / HiPri / Background / AnyThread）—— **命名线程亲和 + 优先级**。
  - `TGraphTask<T>::CreateTask(Prerequisites).ConstructAndDispatchWhenReady`（TaskGraphInterfaces.h:671/1135）—— **显式前置依赖 DAG**（不是简单 fork-join）。
  - `ParallelFor(Num, Body, MinBatchSize, Flags)`（ParallelFor.h:481/526/543）—— 数据并行循环，含 `ForceSingleThread` 调试开关。
  - 结论：UE 比 bevy 多了「命名线程亲和 + 前置依赖图」。我们当前没有独立 game/render 线程，所以**先采纳 bevy 的 scope 模型**；等 P9 V4 需要独立 IO / 流式线程时，再向 UE 的 named-thread + prerequisite 模型长。
- **Carrot（几乎无，仅否决检查）**：grep 命中 `render/AsyncResource.hpp`（异步资源加载）+ 散落 `std::thread`（network / physics）。**无专职 TaskPool / TaskGraph** —— 印证我们不该现在就照 UE 堆命名线程，bevy 的轻量 scope 模型更合适；Carrot 仅作「不要过度设计」的反向证据。

> 三处一致结论：任务系统的核心原语是两类 —— **(a) 数据并行循环**（bevy `ParallelIterator` / UE `ParallelFor`）与 **(b) 任务图 / fork-join**（bevy `scope` / UE `TGraphTask` 依赖 DAG）。本计划先落地 (a)+(b) 的最小交集；命名线程亲和留作长线扩展点。

---

## 前置（已具备 / 需新建）

- **已具备**：`core/` 模块组织 + C++ 标准库（线程 / 原子 / 互斥）；离线管线 `nanite-tools`（`cluster` / `simplify` / `dag` / `stat`）+ `core/grouping.cpp`（唯一碰 METIS 的文件）。
- **需新建**：`core/jobs/`（线程池 + scope + parallel_for 三件套），**不动 `renderer/`、零图形符号**。
- **不碰 `kStage` / 不进渲染回归总闸**（A 类轨道铁律）。

---

## 里程碑

### M-J0 线程池 + parallel_for 最小内核（2~3 天）
- [x] `core/jobs/ThreadPool`：固定 N 线程（默认 `std::thread::hardware_concurrency()`），任务队列 + 窃取；`parallel_for(count, lambda)` 把 `[0,count)` 切块分给各线程。
- [x] 正确性自测：parallel_for 求和 vs 单线程求和，N 次随机规模无差；重复运行无 data race（ThreadSanitizer 或原子计数断言）。
- [x] 负向：count=0 安全返回；parallel_for 内抛异常不拖垮线程池。
- **自检**：单测通过；`nanite-tools` 现有用例全绿（纯新增库，未改业务逻辑）。tag：`job-pool`

> **落地纪要（2026-11-08）**：接口为 `parallelFor(count, body(begin,end), minBatch)`（chunk 形式，UE MinBatchSize + bevy ParallelSlice 两家同构）；驱动 = 共享竞争队列 + **主线程参与执行**（bevy scope block_on / UE 主线程帮忙模式）；**未做 per-thread deque 窃取**（任务粒度大锁竞争可忽略，Carrot 反向证据），批数下限 = 线程数×4 保障负载均衡。边界写死：不可嵌套 parallelFor（M-J1 scope 落地嵌套）、多线程不可并发调同一实例。自测 = `nanite-tools selftest jobs` 七项（含异常重抛 + 池复用），ctest 17/17。台账见 `docs/MILESTONES.md`「P8 M-J0」。

### M-J1 scoped fork-join + 结果回收（2 天）
- [x] `pool.scope(|s| { s.spawn(...); })` 返回 `Vec<T>`，生命周期受限借用（仿 bevy）；`spawn` 可嵌套。
- [x] 与 parallel_for 共用同一底层池，不另起线程。
- **自检**：嵌套 spawn 结果值 / 序正确；scope 结束前所有任务 join（无泄漏、无悬空引用）。tag：`job-scope`

> **落地纪要（2026-11-08）**：`ThreadPool::scope<T>(fn)` 同步 fork-join；worker 语义重构为任务闭包自含完成计账（parallelFor 用 Impl.pending、scope 用 Scope::State.pending，锁顺序恒 st.mu→im.mu）。C++ 无借用检查，靠 scope 同步语义保证 State 裸指针无悬空（join 前任务全完成）。边界：spawn 可嵌套但任务不得阻塞等待其它任务（无前置依赖图，UE TGraphTask 长线模型）。自测扩至 13 项。tag `job-scope`（commit 4e92cf7）。

### M-J2 接入离线管线第一个消费者：cluster / stat 并行化（2~3 天）
- [x] 定位 `nanite-tools cluster` 与 `stat` 的数据并行热循环（按 mesh / 按 meshlet / 按三角形独立）。
- [x] 用 `parallel_for` 包裹，输出写各自 slot（无共享可变状态）。
- [x] **确定性验收**：同资产、同参数下，并行结果与单线程参考**逐字节一致**（离线资产是确定性产物，硬判据）；附耗时对比表（1 线程 vs N 线程）。
- **自检**：`nanite-tools cluster model.gltf` 并行输出 `.nmesh` 与单线程版 MD5 一致；日志打印 wall-clock 加速比。tag：`job-cluster`

> **落地纪要（2026-11-08）**：**定位结论**——贪心切簇（buildMeshlets）mesh 内串行依赖不可并行（算法本性，三处参考同口径）；真实并行点 = **多子网格 per-primitive 切簇**（cluster 命令）+ stat 无热循环（bbox 循环 O(V) 微秒级，并行无收益，保持串行——「定位」的结果可以是没有）。实现：`computeClusterResult`（verify+统计，无 I/O）从 `printClusterResult` 拆出；多子网格走 `scope` 并行 slot 输出、打印保序串行；单子网格保持串行路径（并行开销>收益，零回归）。确定性证据：selftest 合成 8 primitive 并行 vs 串行 BuiltMeshlets 四数组+summary 逐字段逐字节一致；命令行证据 `cluster Suzanne-multi.glb`（scripts/make-multi-primitive-glb.py 复制 primitives 生成，同 accessors 文件几乎不变大）。耗时：串行 4x 440ms vs 并行 194ms（约 2.3x，进程级含加载）。计划原文「-o 并行输出 .nmesh MD5 一致」按 -o 单子网格落盘边界不适用（M1.2 既有边界），确定性由 selftest 断言承担。

### M-J3 simplify / dag 并行化（含依赖边界处理）（3~4 天）
- [ ] `simplify`：METIS 分组天然并行（按组）+ 每组 QEM 简化并行；`grouping.cpp` 的 METIS 调用梳理为可分发单元。
- [ ] `dag`：逐组误差自底向上**并行**（per-group），**合并阶段有依赖**（父 = max 子）必须保留顺序合并，不能并行化合并链——明确标注「哪里能并行、哪里必须串行」。
- [ ] 同 M-J2 确定性验收：`.nmesh` v1/v2 产物与单线程参考 MD5 一致。
- **自检**：simplify / dag 并行产物与单线程 MD5 一致；合并链串行部分正确（DAG 遍历从根到叶不变）；`regression.py` 文本用例全绿（工具版交代，不进 bytes/near 渲染比对）。tag：`job-offline`

### M-J4（可选，长线）命名线程亲和 + IO / 流式池（P9 V4 预留，不排期）
- [ ] 仅设计接口：`IoTaskPool`（资产预取 / 解压，喂 P9 V4 residency 加载）+ 命名线程亲和（对标 UE `ENamedThreads`）。
- [ ] **不在本轨道当前范围**：等 P9 V4 真实需要独立流式线程时再落，避免提前引入复杂度。
- **自检**：仅接口桩 + 设计说明；无功能验收。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M-J0 池 + parallel_for | 2~3 天 | `job-pool` |
| M-J1 scope fork-join | 2 天 | `job-scope` |
| M-J2 cluster / stat 并行 | 2~3 天 | `job-cluster` |
| M-J3 simplify / dag 并行 | 3~4 天 | `job-offline` |
| M-J4 命名线程（可选） | 不排期 | — |

---

## 验收纪律（A 类轨道专属）

- **不进 `kStage` / 不进 bytes-near 渲染回归**：纯 CPU 库，无 GPU。验收走**工具版交代**：展示台 `kind:"tool"` 用例（点运行实跑 + 输出展开）+ 一张真实输出证据图（如「并行 vs 单线程耗时对比表」截图 / MD5 一致报告）+ 负向用例（count=0 / 异常路径 exit 码）。
- **确定性是硬判据**：离线资产是确定性产物，并行化**不允许**改变输出——任何 MD5 不一致即 FAIL，必须修（data race 或归并顺序问题）。
- 每里程碑本地提交 + 打 tag；`docs/MILESTONES.md` 四栏写实。

---

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| scope 生命周期 / work-stealing 模型 | bevy `bevy_tasks/src/task_pool.rs`（`pool.scope` + `Scope::spawn`）、`executor.rs` |
| slice 数据并行原语 | bevy `bevy_tasks/src/slice.rs` + `iter/` |
| 命名线程 + 前置依赖图（长线） | UE `TaskGraphInterfaces.h`（`ENamedThreads` / `TGraphTask::CreateTask(Prerequisites)`）、`ParallelFor.h` |
| 不要过度设计的反向证据 | Carrot 仅 `AsyncResource.hpp` + 散落 `std::thread`，无 TaskGraph |

---

## 底线

- Job 系统只放 `core/jobs/`，**图形 API / NVRHI / ImGui 符号零触及**（沿用 `core/` 纪律）。
- 不引入第三方线程库（标准库 + 自写池足够，与「构建链不依赖厂商 SDK / 重依赖」一致）；bevy / UE 只学结构与边界处理，一行不抄。
- 并行化离线管线时**先保确定性、再谈加速比**；任何为提速而牺牲产物确定性的改法都禁止。
- 不动 S0~S14 编号；本轨道与渲染主线零冲突，可随时插做。
