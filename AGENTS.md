# AGENTS.md — nl-engine 项目协作铁律

> 本文件是 **Agent 协作的权威指令**，每次在本仓库开工前必须通读。项目其余约定见 `docs/ROADMAP.md` 与 `.workbuddy/memory/MEMORY.md`。

---

## 决策前置铁律（最高优先级，2026-11-07 固化）

**每开始一轮新任务，必须同时读过下面三处参考代码 + 本项目开发历史，带着证据做决断；任何超出「已定计划内机械执行」的判定，不许自己拍板——先把三处证据 + 历史决策摆出来，再让用户定。**

用户已多次指出此前每轮任务没有遵守本条。违反即视为失职，用户可随时追责（「这轮你读三处代码了吗」）。

### 为什么单列这条（背景）

- 三处来源口径经常**不一致**：bevy 的 DAG 管 LOD 层级、BVH 管 GPU 遍历剔除（两者配合不是替代）；UE 是工业终态的满分答卷；Carrot 是从零起步的节奏参照。漏看任何一处都会得出偏颇结论。
- 本项目有大量**已固化约定**（`MEMORY.md` / `ROADMAP.md`「里程碑交代规范」/ 各 `S*-PLAN.md`）。擅做决定会破坏一致性、造成返工。
- **用户实测结论 > 网页/官方文档看到的任何信息**（见用户级记忆「沟通与判断原则」）。不看历史，就容易把已被否掉的方案重新拎回来。
- UE 代码**只读不抄**（EULA 与本项目 MIT 不兼容，一行都不许进 repo）；Carrot / bevy 是「抄思路不抄代码」。

---

## 每轮任务必做的 4 步前置（checklist，缺一不可）

1. **Carrot 代码** — `E:\UE_STUDY_2026\Carrot\engine\engine\render\` + `engine\resources\shaders\`（.slang）。
2. **bevy 代码** — `E:\UE_STUDY_2026\bevy\crates\bevy_pbr\src\meshlet\` + `crates\bevy_solari\src\realtime\`。
3. **UE5 官方代码** — `E:\UnrealEngine\5.8.4\Engine\Source\Runtime\Renderer\Private\`（Nanite / Lumen 模块），只读对照表点名的文件，定向读不漫游。
4. **开发历史** — 至少读：
   - `docs/MILESTONES.md`（四栏台账：动作 / 提交版本 / 完成情况 / 踩坑）
   - `docs/carrot-history.md`
   - `docs/bevy-history.md`
   - `.workbuddy/memory/MEMORY.md`（长期约定）
   - `.workbuddy/memory/2026-*.md`（最近一份当日 / 前日日志）
   - 当前步骤的 `docs/s*/S*-PLAN.md` 与 `...-TOOLS-GUIDE.md`

> **当前步骤对应哪几份文件**：先查 `docs/ROADMAP.md`「步骤 → 本地代码对照总表」。查不到对应行时，按功能就近读三处源码的同类模块，并在回复里**明确写出读了哪几个文件、得出什么结论**，不许含糊带过。

---

## 「不许擅做决定」的判定边界

| 类别 | 处理方式 |
|---|---|
| **可以自己做的** | 在已定计划 / `S*-PLAN.md` 验收判据**内**、不改变架构 / 范围 / 约定的机械执行（照计划实现某函数、补测试、跑回归、按既有规范补图）。 |
| **必须摆证据 + 让用户定的** | ① 触碰架构、范围、技术选型、约定（后端、资产格式 `.nmesh`、退出码语义、展示台规范）；② 与历史决策 / 约定冲突或要扩展它；③ 三处来源口径不一致、需二选一或综合；④ 任何「看起来更优」但偏离既定路线的改动。 |

**做法**：回复里**先列「三处代码分别怎么说 + 历史约定怎么定」**，再给建议，**最后用提问或选项请用户拍板**，不要直接落地。

> 遇到用户架构层面的质疑，先自查是不是自相矛盾，认了一次改到位，**不要拿依赖清单 / 官方文档 / 理论可行性辩护，也不要留「也可以……」的尾巴**（见用户级记忆「沟通与判断原则」）。

---

## 自我纠偏

- 若发现某一轮已经没遵守（漏读来源 / 擅做决定）：**立刻在回复里承认、补读三处、重做决断**，并同步更新 `MEMORY.md` 与本文件对应条目。
- 纠正结论后，**同步清理所有相关文档里的旧表述**，避免下次又捡回来。
- 本文件就是给用户的「可追责入口」：用户可随时要求复述本轮读了哪些来源。

---

## 里程碑交付纪律（每个里程碑收口必做，2026-11-07 固化）

**每个里程碑（M0.x 各阶段、S* 各子阶段）验收通过、准备放行之前，必须完成下列「交代」，缺一不可；tag 在验收通过之后才打，严禁「先 tag 再补证明」。**

1. **先验收再放行**：每阶段节奏是 `目标 → 亲眼可见用例 → 验收判据 → 放行`。用例必须是能亲眼看到 / 截图 / 演示的东西，不能是「我觉得写完了」。
2. **必须有「交代」**（渲染版本 / 工具版本各一套）：
   - **渲染版本** = `example/` 截图基线（基线 PNG + `run.bat` 进 `example/<版本>/`，登记唯一数据源 `example/cases.json`）。
   - **工具版本** = 展示台网页 `kind:"tool"` 用例（点运行实跑、输出展开；usage / 正向 / 负向三条起步，`expectExit` 约定 0/1/2）+ `docs/s*/...-TOOLS-GUIDE.md` 式归档。工具不产 GPU 截图时，也要有从工具真实输出生成的证据图（cases.json `preview` 静态图，不许手绘示意）。
3. **每轮迭代必须更新 `example/`**：基线 PNG + `run.bat` 进 `example/<版本>/`；验收 / tag 前跑 `python tools/showcase/server.py --check` 退出 0 才放行（以 `renderer/src/renderer.cpp` 的 `kStage` 判定阶段）。
4. **回归按需跑，非必要不跑（2026-11-08 用户拍板，覆盖一切「顺手跑全量」的惯性）**：
   - **默认不跑回归**。改了什么就只跑受影响的几条关键用例：`python tools/regression.py --only <版本>[/用例]`（可叠加 `--backend` 单后端）挑 2~3 条即可。
   - **全量总闸（双后端 × 全部历史用例）只在打 tag 收口放行时跑一次**（M0.9 起生效，用 `--jobs N` 默认 4 提速）。除此之外任何场景——包括「验证 regression 工具本身的改动」——**禁止起全量**，一律小子集验证。
   - 并行口径：用例间并行、同用例双后端串行（工具用例写共享路径，同用例并行会打架）。
5. **每个小里程碑本地提交一波 + 打 tag**：`docs/MILESTONES.md` 四栏（动作 / 提交版本 / 完成情况 / 踩坑）写实追加，不许只写「done」。
6. **固化文档**：本阶段新产生的约定、不变量、口径变更，必须写回 canonical 文档（`docs/s*/S*-PLAN.md`、`docs/ROADMAP.md`、`docs/MILESTONES.md`、或 `.workbuddy/memory/MEMORY.md`），不许只活在 commit message 或口头里。

> 规范详版见 `docs/ROADMAP.md`「里程碑交代规范」节与 `.workbuddy/memory/MEMORY.md`「开发节奏」节。

---

## 代码开发与双后端纪律（2026-11-07 固化）

通用管线是 **NVRHI + Slang 单源**，DX12 与 Vulkan 是**对称双后端**，不是两个项目。任何渲染 / 管线 / shader / 资产改动，都必须按下面三条过：

1. **双后端必须同时通过**：DX12 和 Vulkan 必须**同时验证通过**，缺一不可——只在一个后端过了不算完成。验收核心 = 一份 `.slang` 双目标（SPIR-V + DXIL）两后端都能加载并出正确画面。
2. **双后端画面一致分档（禁止笼统写「一致」）**：两后端结果必须落到下列三档之一，验收时写明是哪一档、并附证据（截图 / `regression.py` 比对）：
   - `bytes`：MD5 逐字节相同（最优）。
   - `near`：像素差 ≤ 1/255 且差异像素 ≤ 2 个（可接受近似）。
   - `FAIL`：超出 `near` 阈值（不可接受，必须修）。
   不允许只写「两后端一致」而不给分档。
3. **与官方效果对齐**：关键算法 / 视觉结果必须和参考实现对比核验（UE5 官方 = 工业终态满分答卷、bevy = 同代实证、Carrot = 节奏参照）：
   - 几何 / 简化 / DAG 类，对照 UE `NaniteBuilder`、`SimplifyTriangles`、bevy `meshlet` 的产出口径（如 fallback 焊接 2.6% ≈ UE 默认 2%、朝向锚判定随折叠同步更新、按组重切保证父子簇不重叠）。
   - 任何偏离官方口径的地方，必须**解释原因并留痕**（写进 `S*-PLAN.md` / `MILESTONES.md` 踩坑栏），不许静默偏离。
   - **偏离集中台账**（2026-11-08 固化）：所有与 UE5 官方做法的不一致，除上述散落留痕外，必须随手追加到 **`docs/s5/UE-DIFFS.md`**（一条 = UE 做法 + 出处、我们现状、定性〔有意简化/未实现/计划内待办〕、升级路径/触发条件）；每个里程碑收口 / review 前过一遍该文档，确认升级路径仍成立。

> 双后端比对由 `python tools/regression.py`（全量历史用例 × 双后端比对基线）执行，提交 / tag 前必须全绿。完整口径见 `.workbuddy/memory/MEMORY.md`「架构」「S2/S3 约定」与 `docs/ROADMAP.md`。

---

## 并行轨道 worktree 纪律（2026-11-08 固化）

**多并行任务互不干扰靠固定的 `feat/*` worktree 实现，不动态建临时分支、不引入更复杂的调度机制。** 单用户开发、无多进程抢坑，固定分支 + 工作线粘性已满足"互不干扰"。

1. **什么进 worktree / 什么不进**：
   - **进**：A 类并行轨道（引擎化 / 非渲染，不碰 `kStage`、不进渲染回归）—— 如材质图、Job 系统、动作系统。
   - **不进**：VSM（S9 后插入）、TSR（S7 后拆分，见 `ROADMAP.md`）是主线 S 节奏的插入项，留在主 worktree 顺着 S 轨走，不拆出。
2. **实例注册表（三个已建 worktree，占用说明见 `docs/PARALLEL-TRACKS.md` §7）**：

   | 轨道 | worktree 绝对路径 | 分支 | 计划归属 |
   |---|---|---|---|
   | Job / Task 并行 | `E:/UE_STUDY_2026/nl-engine-job-system` | `feat/job-system` | 已立项 `docs/p8/P8-JOB-PLAN.md` |
   | 材质图 → Slang | `E:/UE_STUDY_2026/nl-engine-material-graph` | `feat/material-graph` | 计划待立项（B 路自建） |
   | 动作系统 / 动画控制器 | `E:/UE_STUDY_2026/nl-engine-action-system` | `feat/action-system` | 候选（`PARALLEL-TRACKS.md` 2.4 纯逻辑层） |

   三者均 `junction` 共享主 worktree 的 `.workbuddy` 与 `thirdparty/slang/dist`；`.workbuddy` 已 `skip-worktree`（git status 干净，不会误把纪律源 commit 进 feature 分支）。
3. **固化文档闸**：track 分支 **merge 回 master 前**，必须在本轨道 `S*-PLAN` / `MILESTONES.md` 写验收纪要（目标达成 / 判据 / 证据 / 踩坑），否则不 merge。坑点只活在工作记忆 = 下个会话必重踩。
4. **假绿防护 · 占坑 ≠ 文件指向**：
   - 在 track worktree 干活时，所有编辑绝对路径、shell 每条命令的 cwd **显式在对应 wt 路径内**，不依赖默认落点；
   - merge 回 master 前**双向自查**：`git -C <wt> status` 看得到本批改动、主 worktree `git status` **没有**本批改动（出现即落错树，立即迁移）；
   - 验证可信度自查：改了什么就在产物里找什么——文本 / 数值类改动后若无任何差异是红旗不是绿灯。
5. **工作线粘性**：同一轨道回同一 worktree，一条工作线**只 merge 一次**（验收 FAIL→补丁→复验全部在占坑内闭环，不中途 merge 又裸干）。任务收尾才 merge 回 master，下个任务回同一坑。

> 完整版（含占用说明、M-J0 等里程碑衔接）见 `docs/PARALLEL-TRACKS.md` §7。

---

## 本项目其余硬约束（速查，完整见各文档）

- **UE 源码只读不抄**：EULA 与项目 MIT 不兼容，一行 UE 代码都不许进 repo；参考结构与边界情况处理思路。
- **构建链不依赖任何厂商图形 SDK**：`CMakeLists.txt` 不得出现 `VULKAN_SDK` / `find_package(Vulkan)`。后端对称：D3D12 走 Windows SDK，Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，Slang 独立获取。
- **图形 API / NVRHI / ImGui 符号只允许在 `renderer/`**；`core/` / `viewer/` 零命中。
- **双后端纪律（必须同时通过 / 画面一致分档 / 与官方对齐）**：见上方「代码开发与双后端纪律」节。
- **构建 / 编译 / 运行期踩坑汇总**：见 `docs/DEV-PITFALLS.md`（NMake 必用、Vulkan 静态链接宏、139 段错误、SPIR-V 入口恒 `main`、NMake 时间戳陷阱、`SceneConstants` 160B 等）。
- **里程碑必须有「交代」**：见上方「里程碑交付纪律」节（渲染版 = `example/` 基线、工具版 = 展示台用例 + 归档；tag 在验收后打、提交前 regression 全绿、四栏台账写实、约定写回 canonical 文档）。
- **用户实测结论 > 任何网页 / 官方文档信息**。
