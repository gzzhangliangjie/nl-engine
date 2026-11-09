# P8 并发地基：Job / Task 并行系统（A 类并行轨道，提前立项）

> 对应总路线 `docs/ROADMAP.md` 的 **A 类并行轨道**（引擎化 / 非渲染，不进 `kStage`）。本计划把 P8 引擎化的「并发地基」前置为独立可验收轨道——它**现在就能开**（纯底层、零 GPU 依赖），第一个真实消费者是**离线管线**（`nanite-tools` / `core` 离线部分）。
> 性质：S*-PLAN 风格里程碑草案。立项前已按铁律读三处参考（bevy_tasks / UE TaskGraph / Carrot）+ 开发历史（MILESTONES / ROADMAP / MEMORY / S-PLAN 格式样张 `s15/S15-PLAN.md`）。
> **本文是 job 轨道唯一权威计划文档（2026-11-09 归并）**：原 `docs/p8/JOB-REUSE-REVIEW.md`（复用面 Review）已整段并入本文 §复用面盘点 / §M-J5，不再保留副本；其它文档中的待接消费者描述保持原样，本文 §消费者登记表只做指针索引，不复制正文，避免多处维护漂移。
> **2026-11-09 选型变更**：用户拍板「关联系统完全自己写意义不大，核心诉求是 nanite/lumen 手写」「复杂度一上来很多自己写的就不够用了」「成熟库必须开源且无使用风险」→ M-J5a 的显式依赖 DAG **不自研**，改 vendored **Taskflow v3.11.0（MIT / header-only / C++17）**；`AGENTS.md`「自研边界与第三方库选型闸」为通例，本文原「不引第三方线程库」底线作废。详见 §M-J5a 选型对照与 `thirdparty/taskflow/VENDOR.md`。

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
- [x] `simplify`：METIS 分组天然并行（按组）+ 每组 QEM 简化并行；`grouping.cpp` 的 METIS 调用梳理为可分发单元。
- [x] `dag`：逐组误差自底向上**并行**（per-group），**合并阶段有依赖**（父 = max 子）必须保留顺序合并，不能并行化合并链——明确标注「哪里能并行、哪里必须串行」。
- [x] 同 M-J2 确定性验收：`.nmesh` v1/v2 产物与单线程参考 MD5 一致。
- **自检**：simplify / dag 并行产物与单线程 MD5 一致；合并链串行部分正确（DAG 遍历从根到叶不变）；`regression.py` 文本用例全绿（工具版交代，不进 bytes/near 渲染比对）。tag：`job-offline`

> **落地纪要（2026-11-08）**：**并行边界（写死）**——逐组 QEM 简化、按组重切（buildMeshlets）、组误差 = max(成员簇, 简化)（UE :1441 口径，即 DAG 误差树的自底向上 per-group 段）并行；**组间 soup 前缀拼接（vertBase/triBase 前缀和）与层级推进（L0→LN）串行**——「合并链必须保序」落在此处。并行安全性来自既有设计红利：组间边界锁 → 非边界点单组独占、折叠不越组界；QEM「dst 位置不动、无新位置」→ pointPositions 全程只读。cmdDag 是纯校验/导出（误差在 lod 层已写盘），validateDagChain 保持串行。METIS 调用维持单点（grouping.cpp 唯一 METIS 文件不动，分组本身是全局分区问题不按组可分）。实现：simplifyGroups 步骤 4 两阶段化（并行算组 + 串行前缀）、rebuildMeshletsPerGroup 两阶段化；LodOptions 加 `threads`（0=硬件默认、1=单线程参考，UE ForceSingleThread 同款）、lod 命令加 `--threads`。**确定性三方 MD5**：lod Suzanne 7 层并行 == 改造前基线 == threads=1 逐字节一致；dag 56 groups OK；IcoSphereDense 5 层 threads=1 vs 32 全 MATCH。加速比 2.87x（4.448s vs 1.550s）。selftest jobs 扩至 15 项（+lod 链双池逐字节断言）。

### M-J4（可选，长线）命名线程亲和 + IO / 流式池（P9 V4 预留，不排期）
- [ ] 仅设计接口：`IoTaskPool`（资产预取 / 解压，喂 P9 V4 residency 加载）+ 命名线程亲和（对标 UE `ENamedThreads`）。
- [ ] **不在本轨道当前范围**：等 P9 V4 真实需要独立流式线程时再落，避免提前引入复杂度。
- **自检**：仅接口桩 + 设计说明；无功能验收。

> **能力边界（2026-11-09 复核，写死）**：M-J0~J3 交付的是「离线确定性批处理专用线程池」——只覆盖三处参考共识里的 (a) 数据并行 + (b) 同步 fork-join；缺 (c) 异步 lane / 跨帧长任务、(d) 前置依赖图。池目前由消费者各自局部构造（`ThreadPool pool(...)`），无进程级常驻实例；因此**渲染运行时帧管线接入不在能力范围内**，需先完成 M-J5b 升池。
> **(d) 的实现方式已改（2026-11-09 用户拍板）**：不再自研「依赖计数 + 就绪队列」，改用 vendored Taskflow 显式 DAG（M-J5a）；`nl/core/jobgraph.hpp` 只做薄封装与确定性收口。理由：关联系统复杂度上来自写不够用，且手写预算要留给 nanite / lumen。原 M-J4 留白中的「依赖图」部分就此关闭，M-J4 只剩命名线程 / IO 分池（仍不排期）。

### M-J5（已拍板开工）扩大复用面：从离线批处理件到进程级调度底座

> **状态（2026-11-09）**：用户「开始」批准重启 CLOSED 轨道；`wt_pool claim --track job-system --milestone M-J5a` 已登记，worktree `E:/UE_STUDY_2026/nl-engine-job-system` 已从 master `6a718fb` 重建（junction 共享 + skip-worktree 齐）。**M-J5a 已验收、收口中**；M-J5b/c 尚未开工。

- **目标**：把池从「每次调用 new 一个的局部工具」升成进程级常驻调度底座，并把已用别的方案 / 仍串行的真实消费者接回来，让「并发地基」名副其实。
- **亲眼可见的用例**（A 类轨道工具版交代）：① 材质图 4 路 slangc **fan-in DAG** 并行的 wall-clock 对比图 + 逐字节一致报告；② `jobgraph` selftest（菱形拓扑 / 成环拒绝 / 多次执行确定性）；③ `anim-tools` 批量帧 dump 并行 vs 单线程逐字节一致报告；④ 负向用例（非法图 / 异常路径 exit 码）。
- **验收判据**：
  - **A 档 / M-J5a**：产物 / 输出与单线程参考**逐字节一致**（硬判据，任何不一致即 FAIL）；附耗时对比表；不进 `kStage`、不进渲染回归。
  - **B 档 / M-J5b**：常驻池 + Compute/IO 分池 + 兼容层；合成依赖图 selftest 全绿、原 26 项 jobs selftest 不退。
  - **确定性契约**：物理 M-P0「逐样本位一致」若因换并行需放宽为「同线程数配置下逐样本位一致」，**单独拍板，不默认改**。M-J5a 不碰物理，此契约一个字不动。
- **范围分档**：A = #1/#3/#5/#7（零架构障碍）；B = #2 Jolt 适配 + #4 ECS system 调度；C = #8~#12 保留现状；#6 为已在案合流点。详见 §复用面盘点。
- **放行**：拍板齐全 → 三处参考对照 + 候选库核实 → 登记占坑 → 落码；tag 在验收后打。

#### 拍板记录（2026-11-09，四项全部关闭）

| # | 议题 | 结论 |
|---|---|---|
| 1 | 编号与轨道重启 | **成立**。M-J5 编号保留，`job-system` 从 CLOSED 重启为 ACTIVE，已 claim M-J5a 并重建 worktree |
| 2 | 范围 | **A + B，但拆三步**：M-J5a（DAG 内核 + A 档真实消费者）/ M-J5b（常驻化 + 分池）/ M-J5c（Jolt / ECS 接入，等各自轨道腾手）。用户追加「纯并行有点弱」→ 依赖图从 M-J5b **提前到 M-J5a** |
| 3 | M-P0 确定性契约 | **不改**。Jolt 适配延后到 M-J5c，本轮绕开，契约维持「逐样本位一致」 |
| 4 | DAG 实现方式 | **不自研，vendored Taskflow v3.11.0**（用户「关联系统完全自己写意义不大，核心诉求 nanite/lumen 手写」+「复杂度一上来很多自己写的就不够用了」+「必须开源且无使用风险」） |

#### 步序（重排后）

| 步 | 内容 | 状态 |
|---|---|---|
| **M-J5a** | ① vendored Taskflow 3.11.0 + `nl/core/jobgraph.hpp` 薄封装（环检测 / 登记序归并）；② 抽共用 `recompileGraph` 消除两份 4 路 slangc 实现；③ 4 编译 job → 1 汇总 job 的 fan-in DAG 接到真实消费者；④ selftest + 证据图 + 展示台卡 | **已验收，收口中**（2026-11-09） |
| M-J5b | 进程级常驻 `Scheduler` + Compute/IO 分池 + 老 `ThreadPool` 兼容层 | **已验收，收口中**（2026-11-09） |
| M-J5c-prep | 动态依赖计数 job + Barrier（Jolt JobSystem 同构桩，**Jolt 无关**，接口先行解 physics/ECS 互等死锁） | **已验收，收口中**（2026-11-09，用户拍板「直接在这个 wt 做」） |
| M-J5c | physics 侧 Jolt 适配器：**已验收，收口中**（2026-11-09，tag `phys-j5c`，feat/physics wt 内做——job-system wt 已拆、用户拍板「算 Physics 的，做第二层否则没意义」）。`PoolJobSystem`（继承 `JobSystemWithBarrier`）+ QueueJob→ThreadPool::post（AddRef 投递/Execute+Release 成对）；`DropConfig/StackConfig.jobsThreads`（0=基线路径默认，>=1=适配器）。两层验收全绿：**N=1 与 SingleThreaded 数据行逐字节一致（硬证明）、N=4 run-to-run 逐字节一致、N=4 与串行逐字节一致（岛间无共享累加实测坐实）**；`simulate --jobs 1` 全文件与 M-P0 drop.tsv 逐字节一致。详见 §M-J5c 收口纪要。ECS system 调度接入（走 jobgraph）仍待 ECS 轨道 |

### M-J5a 实施计划（2026-11-09，落码依据）

**选型对照（按 `AGENTS.md` 决策前置铁律，只读与本轮模块对应的部分）**

| 来源 | 证据 | 结论 |
|---|---|---|
| UE5 | `TaskGraphInterfaces.h:54`、`TGraphTask::CreateTask(Prerequisites)`（:671/1135） | 显式前置依赖 DAG，依赖没齐的任务不调度；`ENamedThreads` 是命名线程，与依赖图两件事，不绑在一起做 |
| Carrot | `TaskScheduler.h:83-88/50/128`（`TaskDescription::dependency` / `Async::Counter` / `wait(Counter&)`） | 依赖计数 + fiber yield 让出执行；我们无协程，改用成熟库的 work-stealing，不照抄 fiber |
| bevy | `usages.rs:57-75` 三池分流（Compute / AsyncCompute / IO） | 无显式 DAG；分池归 M-J5b，不与依赖图耦合 |
| Taskflow | vendored `core/task.hpp:511/533`（`precede/succeed`）、`core/executor.hpp:91/123`（`Executor(N)` / `run().wait()`）、`taskflow.hpp:53`（`TF_VERSION 301100`） | DAG 原生、work-stealing、环检测需自己兜；**MIT / header-only / C++17**，风险核查见 `VENDOR.md` |
| 候选否决 | enkiTS（无静态图表达力弱）、oneTBB flow_graph（带 TBB 运行时，违反「零构建期新依赖」）、Taskflow v4.x（要求 C++20） | 取 v3 末版 3.11.0 |

**改动清单（只在 feat/job-system worktree）**

1. `thirdparty/taskflow/`（52 头 + LICENSE + VENDOR.md，排除 `cuda/`）；`CMakeLists.txt` 加 `INTERFACE` 目标并挂 `nl_core PUBLIC`。
2. 新增 `core/include/nl/core/jobgraph.hpp`：`Graph::add(name, fn)` 返回登记序 id、`Graph::precede(from, to)` 建边、`Graph::run(threads, &err)`；Kahn 拓扑**拒环**（静态图成环会挂死，库不检测）；任务只写自己槽位，结果**按 id 归并**不按完成序；`tf::` 类型不泄漏到业务代码。
3. `core/src/material_graph/compile.cpp` 抽共用 `recompileGraph(g, stem, report)`：4 路 `std::system("slangc ...")` 改 4 个编译 job + 1 个汇总 job 的 fan-in DAG；`ok &=` / `report +=` 共享写改按登记序收集后再串行归并。编辑器 `compileCheck`（`_mg_ed_*`）与工具 `cmdCheck`/`cmdWatch`（`_mg_hot_*`）都改调共用入口，**两份重复实现消失**。
4. `nanite-tools selftest jobgraph` 新套件：菱形 A→{B,C}→D 拓扑顺序、成环拒绝（负向 exit 非 0）、同一 DAG 多次执行逐字节一致、fan-in 汇总 == 串行版；原 `selftest jobs` 26 项不退。
5. 证据：`example/m-j5a/` 真实输出（wall-clock 串行 vs DAG、逐字节比对报告、负向用例）、Pillow 证据图、展示台 `kind:tool` 卡 + `cases.json` + `server.py --check`。

**验收（A 类工具版交代）**：① 共用入口的输出与改造前串行版**逐字节一致**；② DAG 拓扑 / 拒环 / 确定性 selftest 全绿；③ 原 26 项 jobs selftest 不退；④ 4 路编译产物（保留版）MD5 与串行一致；⑤ 负向用例 exit 非 0；⑥ 双后端构建能编过（`nl_core` 全链）。不进 `kStage`、不进渲染回归。

**落码前已知坑**：`std::system` 并发拉 cmd.exe 需实测（不同进程、dxcompiler.dll 无共享状态，理论安全）；`report` 必须按 id 归并；`tf::Executor` 析构自带 `wait_for_all`，不要额外套 `cv.wait` 干等。

#### 落地纪要（2026-11-09，M-J5a 验收通过）

- **交付**：`thirdparty/taskflow/`（52 头 + LICENSE + VENDOR.md，排除 `cuda/`）、`core/include/nl/core/jobgraph.hpp` + `core/src/jobgraph.cpp`（`tf::` 只在此 TU）、`CMakeLists.txt` INTERFACE 目标挂 `nl_core PUBLIC`；`compile.hpp/cpp` 抽唯一 `recompileGraph()`（支持 `threads=1` 串行参考 / `keepOutputs` / `verbose`）；`material-graph-tools` 删掉第二份实现并新增 `compilepar` 对拍命令；`nanite-tools selftest jobgraph` 新套件 9 项；`scripts/make-j5a-evidence.py` + `example/m-j5a/`（4 日志 + 4 PNG + run.bat）+ cases.json 两张 tool 卡。
- **验收实测（DX12 / VULKAN 双后端各自跑过）**：`jobgraph selftest: PASS (9 cases)`；老 `jobs selftest: PASS`（26 项不退）；`compilepar : PASS`（serial 0.932s vs DAG 0.299s = **3.12x**；report bytes EQUAL 66 vs 66；lane flags EQUAL；ps.spv/vs.spv/ps.dxil/vs.dxil FNV64 4/4 MATCH）；`check` genslang OK；负向 `selftest unknown-suite` exit 1；`server.py --check` 退出 0（stage=pbr-direct cases=5 baselines=8）。
- **踩坑（固化）**：
  1. **include root**：vendored 目录 `thirdparty/taskflow/` 内部还有一层 `taskflow/`，首次把 include root 设成 `.../taskflow` 导致 `<taskflow/taskflow.hpp>` C1083。上游布局要求 root = 仓库根，本项目对应 `${THIRDPARTY}`。
  2. **Taskflow 静态图不做环检测**：成环会挂死；`Graph::topoOrder` 用 Kahn（同层按登记序）拒环，负向用例才成立。
  3. **异常不在单 task 上**：v3.11.0 把异常挂在 topology（`core/executor.hpp:1863/1871`），`tf::Task::has_exception_ptr()` 不保证置位；改为封装层 `try/catch` 写 `errors[id]`，按登记序取第一个异常，兼得确定性。
  4. **旧代码整理要连调用点一起扫**：删掉工具侧 `findSlangc/findDxc/q/runSlangc` 与第二份 `recompileGraph` 后，`cmdWatch` 残留两处 `findSlangc()` 调用编译报错；同时清掉旧 `s2` 变量遮蔽（C4456）。
  5. **旧默认路径失效**：core 的 `findSlangc()` 指向已拆除的 `nl-engine-material-graph` worktree，改指主树（各 worktree 的 `thirdparty/slang/dist` 是共享 junction）。
  6. **`std::system` 并发实测干净**：4 个独立 cmd.exe 各写各的输出文件；逐路命令回显也收集到各 lane，由汇总 job 按登记序打印，保证并行不改输出。
  7. **版本由 C++ 标准定死**：项目 `CMAKE_CXX_STANDARD 17`，Taskflow v4 起要 C++20 → 取 v3 末版 3.11.0（`TF_VERSION 301100`），不追最新。

#### 落地纪要（2026-11-09，M-J5b 验收通过）

- **交付**：`core/include/nl/core/scheduler.hpp` + `core/src/scheduler.cpp`（进程级常驻 `Scheduler`，组合 compute/io 两个既有 `jobs::ThreadPool`，复用已验证的 `parallelFor` / `scope` 内核，零确定性风险）；`CMakeLists.txt` 仅加 `scheduler.cpp` 一行；`nanite-tools/main.cpp` 加 `selftest scheduler` 新套件（11 检查）+ help/派发；**老 `ThreadPool` 一行不动**，M-J0~J3 全部消费者零改动；`example/m-j5b/`（3 真实日志 + 3 证据图 + run.bat）、`cases.json` 新增 `m-j5b` 卡（3 用例）、`scripts/make-j5b-evidence.py`。
- **验收实测（DX12 / VULKAN 双后端各自跑过）**：`scheduler selftest: PASS (11 cases)`（常驻同一性 / compute·io 规模对齐 / compute parallelFor 求和一致 / scope 保序 / compute 与 io 物理分池 / io 阻塞不拖垮 compute / 常驻多轮确定性 / 老 ThreadPool 兼容层仍可用 / configure 后 no-op 负向）；老 `jobs selftest: PASS`（**25 项**不退）；负向 `selftest unknown-suite` exit 1；`server.py --check` 退出 0（stage=S7.0 cases=4 baselines=6）。
- **设计取舍**：不新写池，直接组合两个已验证 `ThreadPool` —— 进程级常驻化 + 分池的同时复用既有并行内核，**零确定性风险**，且天然满足「老 `ThreadPool` 兼容层一行不动」。compute（CPU 密集）与 io（阻塞 IO / 资产预取，P9 V4 residency 预备位）物理分池，避免 io 阻塞拖垮计算帧。常驻规模以 `configure()` 在首次 `instance()` 前冻结、之后 no-op，防污染帧时间与确定性判据；底座与进程同生命周期不析构。
- **踩坑（固化）**：
  1. **构建纪律复犯（用户点名）**：Git Bash `export INCLUDE/LIB` 到不了 `cl.exe`（C1083）、临时 `scripts/j5b.bat` 走 `cmd //c` 被 MSYS 转义截坏 `BACKEND` 变量（`'2.bat' 不是命令`）——两处都踩中已固体化规范「构建统一走 `scripts/build-m0.2.bat`、一律 PowerShell 原生 API 不再 Git Bash export / 不再 `cmd //c`」。最终用 PowerShell 原生设置 `$env:INCLUDE/LIB/PATH` 后直接调 cmake 增量构建成功，并清理临时脚本。
  2. **cases.json 整文件重写陷阱**：`json.dump(indent=1)` 把原手工维护的 2-space 对象 / 3-space 键缩进整文件重排（223+/124− 噪音 diff）；改用精确字符串插入（匹配 `m-j5a-mg` → `m-sc0` 边界的原生缩进），diff 收敛到 66+/2−。
  3. **证据图高度**：jobs 25 项初版画布高 520 裁掉末 6 项，提到 720 后完整（ViewImage 复核确认）。
  4. **jobs selftest 实测量 = 25 项**（计划文档笼统写「26 项」），以实测为准，计划文档口径待同步。

#### M-J5c 收口纪要（2026-11-09，feat/physics wt，tag `phys-j5c`）

- **背景**：job-system wt 已拆（merge `fb2b4f4` 回 master，feat/physics 已整体合并 master 拿到 M-J5a/b/c-prep 成果），用户拍板「算 Physics 的，直接做第二层（真并行）否则没意义」→ physics 轨道占坑 M-J5c，两层一起交付。
- **实现**（`core/src/physics/World.cpp`）：`PoolJobSystem final : JPH::JobSystemWithBarrier`——依赖计数留在 `JPH::Job` 内部（RemoveDependencyAndQueue→QueueJob 回调），适配器只做三件事：`CreateJob`（new Job + deps==0 即入队，ThreadPool 同款次序）、`QueueJob`（AddRef 投递 + 闭包 Execute/Release 成对，Jolt 合约「入队须自持引用」）、`FreeJob`（delete）；Barrier 由 Jolt 基类兜底（Wait 帮跑=合约 c），maxBarriers=8（PhysicsSystem::Update 用 1 个）。
- **执行底座偏差（如实记录）**：用**专用 `ThreadPool(N)` 实例**而非 Scheduler 单例——Scheduler::configure 冻结使「同进程跑 N=1/N=4 两档对拍」不可行；生产接入仍推荐 Scheduler compute 池（构造 PoolJobSystem 时传 `Scheduler::instance().compute()` 即可，一行）。
- **接口面**：`DropConfig/StackConfig` 各加 `int jobsThreads = 0`（0=JobSystemSingleThreaded 基线路径，M-P0~P4 基线一字不动；>=1=适配器+专用池）；validate 拒 [0,64] 越界。TSV 格式**零改动**（jobs 是运行期旋钮不入 provenance 头）→ 五基线字节稳定。
- **验收结果（最优）**：check-jobs 矩阵（drop 240 帧 + stack 3 案例 600 帧 × {ST, N=1, N=4×2}）6 条判定全 IDENTICAL——①N=1 adapter vs SingleThreaded 逐字节（适配器正确性硬证明）；②N=4 run-to-run 逐字节（并行确定性）；③N=4 vs 串行逐字节（Jolt 岛间无共享累加、并行调度不碰浮点结果，实测坐实而非引用文档）。另 `simulate --jobs 1` 输出**全文件**（含头）与归档 M-P0 drop.tsv `fc /b` 一致。
- **工具**：phys-tools `check-jobs [--threads N] [--frames F]`（ASCII-only 报告）；`simulate`/`stack` 加 `--jobs`；selftest M-J5c 段 2 条断言；ctest `p-j5c-check`/`p-j5c-negative`（物理+j5c 共 15/15 双后端）。
- **踩坑**：stackTSV 按 case 索引 rs[0..2]，jobsMatrix 初版传单元素 vector 越界段错误——改整批传入；check-jobs 无参数按默认值运行（不报 usage），usage 负向用例须用 `--threads 0`。
- **与 M-P0 确定性契约的关系**：契约**未动**——默认路径仍 SingleThreaded；并行路径的确定性是**新增实测结论**（不再是「Jolt 文档声称」），且以逐字节标准验收。

#### 落地纪要（2026-11-09，M-J5c-prep 验收通过）

- **动机（用户拍板）**：M-J5c 被 physics/ECS 两轨道阻塞，但两边互相等会死锁；用户拍板「接口先行」+「直接在这个 wt 做」。把 M-J4 留白 (d) 的**动态依赖图**正身打在本轨道（Taskflow 静态 DAG 覆盖不了 Jolt 的运行中生长图），原 M-J5c 收窄为「纯绑定」。
- **交付**：`core/include/nl/core/jobsync.hpp` + `core/src/jobsync.cpp`（`JobHandle` 引用计数句柄 + `JobSync` create/launch/link/wait + `Barrier` 动态 AddJob/帮跑 wait）；`ThreadPool` 仅加 `post()`/`runOne()` 两个公开原语（与 parallelFor/scope 共用 `Impl.queue`，worker 循环零改动）；`nanite-tools` 加 `selftest jobsync` 套件（20 检查）+ help/派发；`example/m-j5c-prep/`（5 真实日志 + 3 证据图 + run.bat）、`cases.json` 新增 `m-j5c-prep` 卡（外科式插入 65+/0−）、`scripts/make-j5c-prep-evidence.py`。
- **合约读证（AGENTS.md 决策前置）**：`thirdparty/jolt/Jolt/Core/JobSystem.h` 头注释三支柱——(a) 子 job 带依赖计数、父体尾显式 RemoveDependency 归 0 由 JobSystem 入队；(b) Barrier::AddJob 等待期间合法；(c) WaitForJobs 必须帮跑否则全池互等饿死。三支柱各有专属机械用例。零 Jolt 头。
- **验收实测（DX12）**：`jobsync selftest: PASS (20 cases)`（创建者持有防早跑 / null 句柄 / Jolt 手动释放链 / link 线性链严格有序 / 菱形 join 恰一次 / fan-in 64→1 / fan-out 1→64 / Barrier 混合 / 等待期动态 AddJob / 满池帮跑不饿死 / 常驻多轮逐位一致 / 句柄先析构 job 仍完成 / 发布后 link 拒绝 / 过度释放防回绕）；回归护栏 jobs 25 项、scheduler 11 项、jobgraph 9 项全绿；负向 exit 1；`server.py --check` 退出 0。
- **生命周期契约（固化，防早跑竞态）**：create 后计数 = numDependencies + 1（创建者持有）；链接完上游必须 `launch()`/`removeDependency()` 发布；**每个 job 的创建者持有都必须显式释放——link 压住的计数只防早跑、不代替发布**（selftest 死锁教训，见踩坑 1）；发布后 link 拒绝；过度释放防回绕。
- **边界（写死）**：physics 继续用 `JobSystemSingleThreaded`，**M-P0 逐样本位一致契约一字不动**，翻并行开关须用户单独拍板；ECS 静态 system 依赖图走既有 jobgraph，不经本原语；同实例 ThreadPool 的 parallelFor/scope 串行边界不变。
- **踩坑（固化）**：
  1. **自测死锁**：link 只加下游依赖计数，下游「创建者持有」未释放 → `wait` 永等；线性链/菱形/fan-out 三处用例同病同修（发布 = launch 释放持有）。
  2. **用例断言脑补**：菱形根 job 体为空没碰 order 计数，join 序号应为 2 不是 3。
  3. **PASS 行硬编码用例数漂移**：检查从 14 加到 20 后汇总行没同步；以日志解析实测为准。
  4. **friend 缺失**：`Barrier` 访问 `JobHandle::job_` 需显式 `friend class Barrier`。

---

## 复用面盘点（M-J5 前置，2026-11-09）

> 2026-11-09，归并自原 `docs/p8/JOB-REUSE-REVIEW.md`。性质：**只读盘点 + 结论落档**，未改任何代码、未占坑、未打 tag。
> 触发：用户「已有的那些用了别的方案的也要 review 一下，看看能不能用回来」。
> 底线：`core/jobs/` 零图形符号；`nl_core` 静态库已被全部工具链接（CMakeLists.txt:139），**任何 core 层 / 工具层消费者接入无需新增链接依赖**。

### 本轮读过的证据

#### 三处参考（按铁律定向读，只学与 job 系统覆盖面对应的模块）

| 来源 | 文件 | 结论 |
|---|---|---|
| bevy | `crates/bevy_tasks/src/usages.rs:57-75`、`slice.rs:44-52/141-143` | `ComputeTaskPool`（帧内必须完成）/ `AsyncComputeTaskPool`（跨帧）/ `IoTaskPool`（阻塞 IO）三池分流；`par_chunks_map` 用 `scope.spawn` 逐块、按 index 回收。**分池 + 按类型挑池是常态**。 |
| UE5 | `Core/Public/Async/ParallelFor.h:84-116/481-496`、`TaskGraphInterfaces.h:272-447` | `ParallelFor` 有 `MinBatchSize` / `ForceSingleThread` / `PumpRenderingThread`；TaskGraph 有 `WaitUntilTasksComplete` / `ProcessThreadUntilIdle` / `ENamedThreads`。**命名线程 + 前置依赖 + 等待**是工业口径；`ParallelFor` 允许在渲染线程帮忙执行，帧内数据并行不是禁区。 |
| Carrot | `engine/task/TaskScheduler.h:102-186`、`.cpp:23-26/237-271/282-302`、`engine/physics/PhysicsSystem.h:159-161`、`.cpp:117` | 4 条 `TaskLane`（MainLoop / Rendering / FrameParallelWork / AssetLoading）+ fiber + `Async::Counter` 依赖计数；`parallelFor` 走 lane 调度、主线程参与。**关键**：`PhysicsSystem.h` 注释明写「you would implement the JobSystem interface yourself and let Jolt Physics run on top of your own job scheduler. JobSystemThreadPool is an example implementation」——第三方库的 JobSystem 接口本就应由引擎自己的调度器实现。Carrot 用的是 Jolt 自带 `JobSystemThreadPool`，是「没接管」的示例，不是正解。 |

#### 开发历史

本文 M-J0~J4 全篇、`docs/MILESTONES.md` P8 M-J0~J3 段、`docs/PARALLEL-TRACKS.md` §2.2/§2.4/§7、`docs/NON-RENDER-ROADMAP.md:28/43/63`、`docs/ROADMAP.md` P9 V4 段、`docs/p8/P8-{ECS,NAV,PHYS,AUDIO,SCRIPT}-PLAN.md`、`.workbuddy/memory/2026-11-09.md`、`MEMORY.md`（注入）。

### 盘点清单：哪些地方「用了别的方案」

| # | 落点 | 现在用的方案 | 能不能换回 `nl::core::jobs` | 判断 / 障碍 |
|---|---|---|---|---|
| 1 | `core/src/material_graph/compile.cpp:69-72`、`material-graph-tools/main.cpp:272-275` | **4 路 `std::system("slangc ...")` 全串行**（spirv×2 + dxil×2），编辑器 Compile 与 check/watch 各一份重复实现 | ✅ **能，零架构障碍** | 四路互不依赖、各写各的输出文件；`scope<bool>` 4 spawn 即可，结果按 spawn 序汇总。热重载每次都跑，wall-clock 收益直接可见。坑：① 两份实现要同步改（或抽共用 `recompileGraph`）；② `report +=` 是共享写，必须改按序收集；③ 需实测 `system()` 多线程并发拉 cmd.exe 是否干净（不同进程，dxcompiler.dll 无共享状态，理论安全）。 |
| 2 | `thirdparty/jolt/Jolt/Core/JobSystem.h:150-166` + `nl/physics/World.cpp:160/299/433`（feat/physics） | **Jolt 自带 `JobSystemSingleThreaded`**；M-P0 确定性契约 = 单线程 + 1/60 + collisionSteps 1 | ⚠️ **技术上最该换回，但要升池 + 拍板** | Jolt 的 `JobSystem` 是可实现的抽象接口（`CreateJob` 带 `inNumDependencies`、`Barrier::AddJob`、`WaitForJobs` 支持等待期动态加 job），把它适配到自家池 = 让物理跑在引擎调度器上，是「并发地基」名副其实的落点。障碍：我们的池只有同步 fork-join + 无依赖 `spawn`，**表达不了依赖计数 / barrier**（正是 M-J4 留白的 (d) 前置依赖图）；且换并行会破 M-P0 已固化的「逐样本位一致」确定性契约，须单独拍板是否放宽。 |
| 3 | `core/src/anim/skinning.cpp:100/121`（`skinPositions`/`skinNormals`）、`computePalette:87` | 逐顶点 / 逐骨骼**串行纯函数循环** | ✅ **离线段能，帧内段要等常驻池** | 只读 palette/base、写各槽位，天然 `parallelFor`，确定性最好保。`anim-tools` 批量帧 dump（`fkdump`/`skindump`）现在就能接；`renderer/src/scene.cpp:2378` 每帧调用点若接「每次 new 一个池」会亏，须先升常驻池。**注意**：CPU 并行不替代已定的 GPU 调色板升级路线，只作过渡/离线收益。 |
| 4 | `core/src/scene/World.cpp:209-210` `World::tick` | 逐 `System*` **串行** `tick(dt)` | ⚠️ 能，但需依赖图 | 要安全并行得先有 system 间读写集 / 前置依赖声明（bevy access conflict / UE prerequisites 口径），否则 World 增删实体与 system tick 互相踩。50 物体规模本身收益存疑，价值在「把池升成调度底座」的验证消费者。 |
| 5 | `nanite-tools/main.cpp:777` `check-orient` 多 target | 串行 for 循环（单 target 内已用 spatial hash 把 45min→4.9s） | ✅ 能，收益小 | 多 target 独立，`scope` 可并行。不是瓶颈，顺手做。 |
| 6 | `docs/p8/P8-NAV-PLAN.md:45` 多 tile 并行烘焙 | **计划里已写明用 Job 系统合流**（「原 parallel_for 分块体素化作废，合流仅用于多 tile 并行烘焙，逐 tile MD5 一致」） | ✅ 已登记，不是别的方案 | M-N3 开工时复核任务卡写明「吃 `nl::core::jobs`，不自写 `std::thread`」即可。 |
| 7 | `docs/PARALLEL-TRACKS.md:81` 动作系统多 clip / 多骨骼并行采样 | 登记为「可选吃 Job 系统」，实际未接 | ✅ 能 | 与 #3 同一批 core 层纯函数，一并纳入。 |
| 8 | `tools/regression.py:48/327` | Python `ThreadPoolExecutor`（用例间并行、同用例双后端串行） | ❌ **不该换** | 并行单位是「起 viewer.exe 进程」，跨语言/跨进程；C++ 进程内池够不着。它已有同款确定性纪律（`records.sort` 按用例表定序，regression.py:332-339），属同构不同层，不是漏接。 |
| 9 | 音频 miniaudio `ma_engine` / `ma_device` 回调线程 | 库内部实时音频线程 | ❌ **不该换** | 实时回调有硬实时约束，塞通用线程池会 glitch。库边界，非漏接。（旧 OpenAL 路线的 `AudioThread` 已随方案变更作废，P8-AUDIO-PLAN:70） |
| 10 | 脚本轨道 .NET hosting / collectible ALC 热更 | CLR 自带线程池 / GC 线程 | ❌ 不该换 | 跨运行时边界。 |
| 11 | `tools/showcase/server.py`、`tools/wt_pool.py`、`scripts/make-*-glb.py` | `subprocess` 起进程 / 串行生成 | ❌ 不该换 | 进程编排与资产生成，非 CPU 并行任务。 |
| 12 | GPU 侧 cull / DAG 遍历 / Hi-Z（renderer + *.slang） | GPU compute 并行 | ❌ 不属本范畴 | 与 CPU job 系统是两套并行域，别混为一谈记漏账。 |

### 分档结论

- **A 档 · 立刻能用回来（零架构改动，纯 core/工具层）**：#1 四路 slangc 编译并行、#3 离线动画批量帧、#5 check-orient 多 target、#7 多 clip 采样。判据照旧：产物 / 输出逐字节一致 + 工具版交代 + 负向用例，不进 `kStage`、不进渲染回归。
- **B 档 · 要先把池升成调度底座（M-J5 真正的内容）**：#2 Jolt `JobSystem` 适配、#4 ECS system 调度。前提 = 常驻池 + 依赖计数 / barrier（对 UE `TGraphTask::CreateTask(Prerequisites)` 与 Carrot `Async::Counter` 口径）+ 分池（对 bevy 三池 / Carrot 四 lane）。**#2 还需单独拍板物理确定性契约是否放宽**，不得默认改。
- **C 档 · 保留现状，不算欠账**：#8~#12，跨语言 / 跨进程 / 库内部线程 / 实时约束 / GPU 域。
- **#6 是已在案的合流点**，开工时落实即可。

---

## 消费者登记表（待接，只做指针索引）

> 本表只做**索引**，权威正文仍在各自文档，本文不复制、不替代；改动消费者描述请改原文档，避免多处维护漂移。

| 消费者 | 权威落点 | 触发条件 | 归属 | 状态 |
|---|---|---|---|---|
| 多 tile 并行烘焙 | `docs/p8/P8-NAV-PLAN.md:45` | M-N3 开工 | 已在案（#6） | 已写明吃 Job 系统 + 逐 tile MD5 一致；任务卡复核「不自写 `std::thread`」即可 |
| 动作系统多 clip / 多骨骼采样 | `docs/PARALLEL-TRACKS.md:81`、`§7` 增量阶梯第四级 | 多角色 / 混合树 | A 档（#7） | 可选登记，实际未接 |
| ECS system 并行调度 | `docs/p8/P8-ECS-PLAN.md`（E1b 渲染集成） | E1b | B 档（#4） | 需读写集 / 前置依赖声明；E1a 是纯逻辑内核，现无 system 调度器 |
| Jolt `JobSystem` 适配 | `docs/p8/P8-PHYS-PLAN.md` + feat/physics `nl/physics/World.cpp` | M-J5 B 档拍板 | B 档（#2） | 依赖计数 / barrier + M-P0 确定性契约待拍板 |
| P9 V4 residency / `IoTaskPool` | `docs/ROADMAP.md` P9 V4 段 + 本文 M-J4 | 超显存超大场景 + 独立流式线程 | M-J4 | 不排期，仅接口桩预留 |
| 材质图 4 路编译 / 离线动画批量帧 / `check-orient` 多 target | 本文 §复用面盘点 #1/#3/#5 | 立即可接 | A 档 | 零架构障碍 |

---

## M-J5 不变量与已固化踩坑

- `nl_core` 静态库已被全部工具链接（CMakeLists.txt:139），core / 工具层接入**无需新增链接依赖**；Job 系统只放 `core/jobs/`，图形 API / NVRHI / ImGui 符号零触及。
- `parallelFor` **不可嵌套**（M-J0 写死）；多线程不可并发调同一实例；`spawn` 可嵌套但任务不得阻塞等待其它任务（M-J1）。**M-J5a 起这条只约束老 `ThreadPool`**：有前置依赖的任务走 `jobgraph::Graph`，不要在 `scope` 里 `cv.wait` 干等（worker 全阻塞会死锁）。
- **Taskflow 静态图不做环检测**（成环会挂死），封装层必须 Kahn 拓扑拒环；work-stealing **不承诺无依赖任务的执行顺序**，归并只按登记序，不按完成序。
- **老 `ThreadPool` 与 `jobgraph::Graph` 并存**：M-J5a 不替换 M-J0~J3 的数据并行 / fork-join 消费者，26 项 jobs selftest 一行不退；统一常驻化是 M-J5b 的事。
- 池目前**无常驻实例**（消费者各自局部构造）；M-J5 升池之前不接帧内循环——每次 new 一个池会亏，且会污染帧时间与确定性判据。
- **4 路 slangc 有两份重复实现**：编辑器 `compile.cpp` 走 `_mg_ed_*`，工具 `main.cpp` 走 `_mg_hot_*`；B4-PLAN 写的「check/watch 都走 recompileGraph，无重复实现」与实际不符。并行化时两处要同步改，或按原计划抽共用函数；`report +=` 是共享写，必须改成按 spawn 序收集。
- 确定性是硬判据：并行化**不允许**改变输出，任何 MD5 不一致即 FAIL；任何为提速牺牲产物确定性的改法都禁止。

---

## 支线任务（2026-11-08 登记）：可视化证据图补交

- **任务性质**：支线任务（side quest），独立领走、不阻塞主线（S4/S5）；主线下次开工前或任意空闲会话可执行。

- [x] **现状缺口**：p8-jobs 展示卡 4 条 `kind:"tool"` 用例（selftest-jobs / cluster-parallel / lod-parallel / negative-unknown-suite）`cases.json` 中 `preview` 均为 `None`，`example/` 下无 job 相关 PNG。验收纪律要求的「一张真实输出证据图」实际未交付，本条为欠账。
- [x] **补交方式**：从工具**真实输出**生成证据图（不许手绘示意）：
  1. **加速比对比图 + MD5 表**——`example/p8-jobs/lod-parallel.png`：IcoSphereDense 5 层真实计时（threads=1 5.913s vs threads=32 1.982s，**2.98×**）+ 逐层 MD5 对照表 6/6 MATCH；
  2. **selftest 结果图**——`example/p8-jobs/selftest-jobs.png`：selftest jobs 真实输出 26 项逐条 + `jobs selftest: PASS`；
  3. **cluster 并行图**——`example/p8-jobs/cluster-parallel.png`：4 子网格逐块真实统计（coverage 100% / limits PASS）+ 末行并行计时；
  4. **负向用例图**——`example/p8-jobs/negative-unknown.png`：`selftest: unknown suite` 拒绝 + exit 1。
- [x] 生成器脚本 `scripts/make-job-evidence.py`（Pillow 渲染，计时/MD5 全部从 `build/showcase/runs/job-offline/` 真实日志与产物解析，无硬编码数据）；`cases.json` 4 条 `preview` 已回填（JSON 有效、仅 +9/-5 行 diff）。
- [x] **验收说明**：`python tools/showcase/server.py --check` 当前 FAIL，但 FAIL 项为「缺 `s5.0` 版本用例」——归属 S5.0 主线收口件（主 worktree 存在另一条 S5.0 工作线的未提交占坑改动，kStage 已 bump 至 S5.0）。本支线的 preview 回填与 4 张图不触发任何 check 项；待 S5.0 收口时随其版本归位一并转绿。
- [x] 执行纪要（四栏）已补 `docs/MILESTONES.md`（2026-11-08 支线行）。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M-J0 池 + parallel_for | 2~3 天 | `job-pool` |
| M-J1 scope fork-join | 2 天 | `job-scope` |
| M-J2 cluster / stat 并行 | 2~3 天 | `job-cluster` |
| M-J3 simplify / dag 并行 | 3~4 天 | `job-offline` |
| M-J4 命名线程 / IO 分池（可选） | 不排期（P9 V4） | — |
| M-J5a DAG 内核 + A 档复用（vendored Taskflow） | 2026-11-09 收官 | `m-j5a` |
| M-J5b 常驻池 + 分池 | 待 M-J5a 收口 | — |
| M-J5c Jolt 接入 | **2026-11-09 完成（tag `phys-j5c`）**：PoolJobSystem 两层验收全绿（N=1 逐位硬证明 / N=4 run-to-run 且与串行逐字节一致） | example/phys-j5c/ |
| M-J5c ECS system 调度接入 | 待 ECS 轨道腾手（走既有 jobgraph） | — |

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
| 显式依赖 DAG 怎么用 / 环与确定性 | `thirdparty/taskflow/VENDOR.md`（版本 / 风险 / API 速查）、vendored `core/task.hpp`、`core/executor.hpp`；本项目封装 `nl/core/jobgraph.hpp` |
| 不要过度设计的反向证据 | Carrot 仅 `AsyncResource.hpp` + 散落 `std::thread`，无 TaskGraph |

---

## 底线

- Job 系统只放 `core/jobs/`，**图形 API / NVRHI / ImGui 符号零触及**（沿用 `core/` 纪律）。
- **（2026-11-09 作废重写）** 原「不引入第三方线程库，标准库 + 自写池足够」已按用户拍板推翻：关联系统优先 vendored 成熟库，手写预算集中给 nanite / lumen；硬闸见 `AGENTS.md`「自研边界与第三方库选型闸」（开源 + 无使用风险 + LICENSE 入库 + 零构建期新依赖 + VENDOR.md 溯源）。bevy / UE 只学结构与边界处理，一行不抄。
- 并行化离线管线时**先保确定性、再谈加速比**；任何为提速而牺牲产物确定性的改法都禁止。
- 不动 S0~S14 编号；本轨道与渲染主线零冲突，可随时插做。
- **单一源**：本文是 job 轨道唯一权威计划文档；待接消费者在其它计划文档里的描述保持原样，本文 §消费者登记表只做指针索引、不复制正文。盘点与实施计划同处一份，避免状态漂移。
- **（2026-11-09 更新）** 轨道重启与 M-J5a 范围已获用户明确批准并登记 `claim --track job-system --milestone M-J5a`；`M-J5b/c` 仍须各自开工前确认，不得顺手扩范围。
