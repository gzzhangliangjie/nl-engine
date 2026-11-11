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

## 自研边界与第三方库选型闸（2026-11-09 用户拍板，覆盖旧原则）

**核心诉求只有 nanite / lumen 手写**；关联系统（任务调度 / 物理 / 寻路 / 音频 / 脚本 hosting / 编辑器与工具链等）**优先 vendored 成熟库，不再以「有参考就从头写」为默认**。用户原话：「可以直接用 Taskflow，这些关联系统完全自己写的确意义不大，核心诉求是 nanite 和 lumen 手写」「复杂度一上来，很多自己写的就不够用了」「当然前提是那些成熟库，必须是开源并且无使用风险的」。

本条**作废旧原则**：`docs/NON-RENDER-ROADMAP.md` 头部「有参考的内容就考虑从头写，参照实现只作对照与节奏校准」，以及各计划里把 Jolt / Recast-Detour / miniaudio / Taskflow 称作「例外」的表述。从此**用成熟库是通例，自研是例外**；自研只在「核心算法（nanite / lumen 及其渲染主线）/ 无成熟方案 / 成熟方案确不适用」三种情况下成立，且须单独拍板。

### 选型硬闸（缺一不可，不得跳过）

1. **开源且无使用风险**：宽松许可证（MIT / ZLib / Apache-2.0 / MIT-0 / Public Domain 等），**LICENSE 原文随 vendored 入库**；禁 copyleft 传染（GPL/LGPL/MPL 须单独评审）、禁 EULA 限制的厂商 SDK、禁只读参考实现的代码（UE）、禁来源不明或已停维护且无替代的项目。
2. **许可文本与项目 MIT 兼容**：允许商用 / 修改 / 再分发；无已知专利 / 授权诉讼风险。**MIT 无显式专利授权条款这一事实必须如实写明，不许对外宣称「绝对零风险」**。
3. **零构建期新依赖**：header-only 或纯源码，不联网拉取、不要求装厂商 SDK、不引入新运行时 DLL；本机 MSVC 14.38 / C++17 能编译（沿用「构建链不依赖任何厂商图形 SDK」铁律）。
4. **溯源留档**：`thirdparty/<lib>/VENDOR.md` 写明版本 / tag 或 commit / 上游地址 / 许可证 / C++ 标准 / 入库子集与排除项 / 风险核查结论；升级时同步更新。
5. **边界不变**：UE 源码仍只读不抄，Carrot / bevy 仍抄思路不抄代码；第三方库只解决关联系统，**不得替代 nanite / lumen 手写主线**。业务代码不直接暴露第三方类型，经 `nl/` 薄封装隔离，保证可替换。

### 已按本闸落地（统一归档，不再称「例外」）

| 库 | 版本 | 许可证 | 用途 |
|---|---|---|---|
| Taskflow | 3.11.0 | MIT | M-J5a 显式依赖 DAG（v4 要 C++20，故取 v3 末版） |
| Jolt Physics | 5.3.1 | MIT | 物理 |
| Recast/Detour | 上游 1.6.0 | ZLib | 寻路（**已收官 2026-11-11**：M-N0~M-N6 整轨 merge `5356c80`，tag `nav-m0`~`nav-n6` / `nav-done`，`feat/pathfinding` + `milestone/nav-done` 分支保留） |
| miniaudio | 0.11.26 | Public Domain / Unlicense | 3D 音频 |
| NVRHI / Slang / meshoptimizer / METIS / imgui / glfw / tinygltf 等 | — | 各自 LICENSE | 既有 vendored，沿用 |

---

## 每轮任务必做的 4 步前置（checklist，缺一不可）

> **第 0 步 · 对话开场归属声明（2026-11-09 用户拍板，见并行轨道纪律第 7 条）**：每个对话开场先声明本次归属的**里程碑编号**；用户没给就提示补充，补上后 `python tools/wt_pool.py claim --track <轨道> --milestone <编号>` 登记，然后才动手。**收尾必须对称释放（2026-11-09 用户拍板）**：本轮里程碑做完（或对话要结束时），`python tools/wt_pool.py release --track <轨道> --milestone <编号>` 释放占用——否则 `.wt_pool_busy.json` 里的锁一直挂着，其他对话全被挡。正常收尾不带 `--force`（`--strict` 可校验文档闸）；异常中断后接手清理才用 `--force`。

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

> **（2026-11-08 追加，M5.1 失职教训）证据与当前里程碑模块一一对应**：新里程碑（M*.x）开工时，即使同一阶段内上一里程碑（M*.x-1）已经读过三处，也**必须按对照总表重读本里程碑点名的文件**——误差公式读过的 `cull_shared.wesl` 不能顶替遍历结构要读的 `cull_bvh.wesl`。复用旧证据前先自问：这份证据覆盖的是**本轮要落码的那个模块**吗？不是就重读。

### 开工闸：计划内不得留「待拍板」架构判定（2026-11-10 用户拍板固化，M9.0 教训）

- **开工前必查**：当前里程碑的 `S*-PLAN.md` / 任务书 / 拍板记录里，凡出现「待拍板 / 待定 / 待用户确认 / TBD」且涉及**架构、范围、技术选型、绑定布局、资源格式**，一律视为**未满足开工条件**；先按本节补齐三处实读 + 本项目历史约定，把 2~3 个可拍板方案（含代价矩阵）摆给用户，**拿到拍板并写回计划之后**才允许落码。
- **架构判定未定不许开里程碑落码**：不许把待定项写成一句「待拍板」就开始写相邻代码。第一行落盘代码（RT / UAV / `register` 槽位 / 格式 / BindingSet 布局）就等于把决策隐式拍死，后续消费端（S10/S12/S15）返工最贵；也不许在注释、CMake、README、commit message 里顺手把形态写死。
- **判定边界**：只有「已定计划内机械执行」可直接落码；与待定架构**无关**的纯搬移 / 测试 / 回归可以并行做，但必须在回复里显式声明「冻结面」有哪些、哪些一行没碰。
- **本轮反例**：M9.0 开工时 `docs/s9/S9-PLAN.md` Q4「GBuffer 落盘形态」仍标「待拍板」，助手却已启动落码，还自行把「新增独立 GBuffer 落盘」写进任务书第 3 条（Q1 用户只拍了「GBuffer 抽取 + 公共反解头」）。纠正动作 = 冻结落盘 → 定向实读 Carrot / bevy / UE 三处 + 历史约定 → `docs/s9/Q4-GBUFFER-OPTIONS.md` 摊方案 → 交用户拍板 → 结论写回 Q4 并删任务书挂账句。

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

> ⚠️ **本节是全部纪律里违反率最高的一节**（2026-11-09 用户再次暴怒点名：M6.0 收口漏 example 卡、漏台账、漏修改文件清单，「我不知道提醒多少次了 还是不听话」）。下面第 9 条的四项自查清单是硬闸：**清单没逐项打勾，不许宣布收口、不许打 tag**。每漏一项都视为失职，用户可追责。

1. **先验收再放行**：每阶段节奏是 `目标 → 亲眼可见用例 → 验收判据 → 放行`。用例必须是能亲眼看到 / 截图 / 演示的东西，不能是「我觉得写完了」。
   1b. **收口硬闸（2026-11-09 固化，机械强制）**：里程碑收口前必须跑 `python tools/wt_pool.py close --track <轨道> --milestone <编号> --tag <tag> [--version <example版本>]`——机械校验（tag annotated 且在 MILESTONES 台账、example 交付物齐 + `server.py --check`、`git show --stat` 原文机器生成）**任一失败 exit 1 = 不许宣布收口、不许打 tag 流程视为未完成**；通过后生成的报告（主树根 `.wt_pool_close/`）补齐「待填」格后**原样作为收口回复第一屏**；最后 `release --done`（校验报告无未填格才放行）。中途结束对话（里程碑没做完）仍用不带 `--done` 的 `release`。**例外（2026-11-10 M9.0 实读澄清）**：该闸只对**并行轨道**生效 —— `TRACKS` 未注册主线轨道、`release --done` 强制要求 active claim，`.wt_pool_close/` 全部报告来自 feat/* 轨道；S7 / S8 主线收口从未跑过。**主线（master）收口按既有口径执行**：annotated tag + `MILESTONES.md` 四栏台账 + 全量总闸 + `server.py --check`，不新增 track、不 claim，也不要为凑闸去改注册表。
2. **必须有「交代」**（渲染版本 / 工具版本各一套）：
   - **渲染版本** = `example/` 截图基线（基线 PNG + `run.bat` 进 `example/<版本>/`，登记唯一数据源 `example/cases.json`）。
   - **工具版本** = 展示台网页 `kind:"tool"` 用例（点运行实跑、输出展开；usage / 正向 / 负向三条起步，`expectExit` 约定 0/1/2）+ `docs/s*/...-TOOLS-GUIDE.md` 式归档。工具不产 GPU 截图时，也要有从工具真实输出生成的证据图（cases.json `preview` 静态图，不许手绘示意）。
3. **每轮迭代必须更新 `example/`**：基线 PNG + `run.bat` 进 `example/<版本>/`；验收 / tag 前跑 `python tools/showcase/server.py --check` 退出 0 才放行（以 `renderer/src/renderer.cpp` 的 `kStage` 判定阶段）。**覆盖边界（2026-11-10 M9.0 实读澄清）**：`--check` 只校验 `kStage` 对应的那一张版本卡；若本里程碑按拍板**不 bump `kStage`**（如 M9.0 保持 `S8.2`），新版本卡不在其覆盖范围内 —— 此时新卡齐备必须由 `example/<版本>/run.bat verify` + `regression.py --only <版本>` 双重机械校验，`--check` 只用来证明旧阶段没回归，不得当成新卡已验收的证据。
4. **回归按需跑，非必要不跑（2026-11-08 用户拍板，覆盖一切「顺手跑全量」的惯性）**：
   - **默认不跑回归**。改了什么就只跑受影响的几条关键用例：`python tools/regression.py --only <版本>[/用例]`（可叠加 `--backend` 单后端）挑 2~3 条即可。
   - **全量总闸（双后端 × 全部历史用例）只在打 tag 收口放行时跑一次**（M0.9 起生效，用 `--jobs N` 默认 4 提速）。除此之外任何场景——包括「验证 regression 工具本身的改动」——**禁止起全量**，一律小子集验证。
   - 并行口径：用例间并行、同用例双后端串行（工具用例写共享路径，同用例并行会打架）。
4b. **ctest 单测命令统一带 `--repeat until-pass:3`（2026-11-10 实锤固化）**：便携 cmake 3.31.6 的 ctest 对 0.07s 级快进程**随机丢全部 stdout**（竞态特征：`Required regular expression not found` + `<end of output>` + 时长正常），挂时**产物文件已写出**（进程实际跑完，纯 ctest 捕获竞态，python subprocess 直拉同命令 100/100 全过实锤）。凡 ctest 报 missing anchor / 空输出失败，先带 `--repeat until-pass:3` 重跑再定性——竞态重试即绿，**真回归稳定挂重试后仍挂，不会被此闸掩盖**。判别两族用 python 直拉对照：直拉也挂 = 真秒退（0xC0000409 族），直拉全过 = ctest 竞态。
5. **每个小里程碑本地提交一波 + 打 tag**：`docs/MILESTONES.md` 四栏（动作 / 提交版本 / 完成情况 / 踩坑）写实追加，不许只写「done」。
   5b. **大版本里程碑收官必须留分支记录（2026-11-09 用户拍板）**：主工程每个大版本（S* 收官）打完 tag 后，必须**在同一 commit 上建 `milestone/<tag名>` 分支**（如 `milestone/s6-done`），与 tag 一并推远端（无 remote 时先建本地，配好远端后补推），推完用 `git ls-remote` 确认分支与 tag 都到了远端。并行轨道（feat/*）的 tag 不在此列。历史上 s0-done~s6-done 已于 2026-11-09 补建。
6. **固化文档**：本阶段新产生的约定、不变量、口径变更，必须写回 canonical 文档（`docs/s*/S*-PLAN.md`、`docs/ROADMAP.md`、`docs/MILESTONES.md`、或 `.workbuddy/memory/MEMORY.md`），不许只活在 commit message 或口头里。
7. **收口必须报「提交版本 + 修改文件清单 + 固化内容」（2026-11-08 拍板；2026-11-09 用户点名扩充 ③）**：每段工作做完，收口回复里必须给出——①**提交版本**：commit 短哈希 + tag 列表（一行一个里程碑，写实不许漏）；②**固化内容**：本轮写进了哪些 canonical 文档 / 台账 / 注册表、各补/改了什么；③**修改文件清单**：按类别列出本次提交改动的**每一个文件**及一句话说明（用 `git show --stat` 核对，不许只报「N 个文件 +N 行」的数量糊弄——M6.0 收口就栽在只报数量没列清单上）。三项缺一即为失职；发现遗漏当场补齐再报。
8. **交互纪律（2026-11-09 用户拍板固化，起因：动作系统 worktree 清理往返事故）**：①**确认类问题（有没有 / 是不是 / 在哪 / 什么时候）只回答状态，一个句号落地，禁止附带任何工具调用与执行**；②**删除 / 移除 / 恢复类操作（worktree、文件、分支、目录）必须先列影响面清单、等用户明确批准才动手——用户的疑问句一律不当作批准**；③用户说「打住 / 停 / 等」= 冻结一切操作；④命令被用户中断 = 停止，报告执行到哪一步，等指令，禁止自行进入「评估→修复→重建」连环动作；⑤修复自己造成的损坏属于例外（必须做），但修复前报告方案，修复后如实报告做了什么。
9. **收口前四项自查清单（2026-11-09 用户拍板固化，起因：M6.0 收口漏交代物被连续点名）**：宣布收口 / 打 tag **之前**，逐项过下列清单，**任何一项没完成就不许宣布收口，更不许等用户点名**——用户已经点名过多次，再漏就是明知故犯：
   - ① **渲染交代物**：`example/<版本>/` 基线 PNG（**肉眼核验过**，不是拍完就完）+ `run.bat`（含 verify 机器断言）+ `example/cases.json` 登记版本卡与用例卡 + `python tools/showcase/server.py --check` 退出 0；
   - ② **工具交代物**：展示台 tool 卡（usage / 正向 / 负向）+ TOOLS-GUIDE 归档（纯研究轮次无工具产出时可免，但要在收口回复里写明「本轮无工具交代物」）；
   - ③ **`docs/MILESTONES.md` 四栏台账**：动作 / 提交版本 / 完成情况 / 踩坑，写实追加；
   - ④ **固化文档**：新约定 / 铁律写回 `SHADER-CONVENTIONS.md` / `S*-PLAN.md` / `UE-DIFFS.md` / `MEMORY.md` 等对应 canonical 文档。
   - 收口回复里**逐项报告四项状态**（如「① example/s6-hiz 卡 + server --check OK；② 无工具产出；③ 台账已追加；④ §2.7/§2.8 已固化」），让用户一眼可验，不许含糊。
   - **渲染轮次收口必须主动把验收证据图展示给用户**（卡片/预览直接打开，2026-11-09 用户拍板：不许只在文字里报文件路径等用户问「图呢」；M6.1 收口两次被点名教训）。纯文档 / 无新截图的轮次也要显式写「本轮无新证据图」。
   - **执行机制（2026-11-09 pbr-direct 收口第三次漏项后拍板）**：上述清单靠「写的时候记得」**已被证明无效**——规则明文在手边（本节 + 记忆注入）仍连漏三次（M-E1、两次点名、pbr-direct 又漏文件清单+证据图）。故收口回复**第一屏必须是四项自查 + 三要素的清单表格本身**：先跑 `git show --stat`、先 `present_files` 打开证据图，再逐格填表；清单在正文最前面，哪格空了一眼可见。**禁止**把清单藏在叙述后面、或先写验收结论再补交代——结构上让漏项无处可藏。

10. **开工闸（与收口闸对称，2026-11-10 用户拍板固化）**：里程碑「开工」同样有硬闸——**计划内不得留「待拍板」架构判定；架构判定未定不许开里程碑落码**。检查点：① 逐条读 `S*-PLAN.md` 的拍板记录与任务书，确认无「待拍板」架构项；② 有则先补决策前置功课（三处定向实读 + 历史约定 + 2~3 个方案的 RT/UAV 数量格式 / 写入 pass / 绑定布局 / 显存带宽精度 / 消费端扩展面 / 基线改动面），交用户拍板；③ 拍板结论写回 `S*-PLAN.md` 拍板记录，并**同步删掉任务书里对应的「待拍板」挂账句**；④ 之后才允许写第一行架构代码。与第 9 条一样，靠「写的时候记得」已被证明无效：**开工回复第一屏必须报告「本轮待拍板项 N 个 / 已拍板 N 个 / 冻结面清单」**。详见「决策前置铁律」节的「开工闸」条。
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

## git 危险操作闸门纪律（2026-11-10 固化，起因：checkout 误还原在途修复）

**还原 / 切换 / 清理类 git 操作一律先过闸门，禁止「顺手一把梭」。** 事故实录（E1b merge 收口轮）：`git checkout -- example/` 本意清音频噪声，把同路径下**未提交的 32 张重拍基线 + cases.json 五处用例修复**一并打回原形，最终闸打出 43 FAIL 假象。

1. **`git checkout -- <path>` / `git restore` 前置闸门**：必须先跑 `git status --porcelain=v1 -- <path>` 列出将被还原的文件，并逐项核对其中是否含**本会话刚产生的未提交工作**（基线重拍 / 用例修复 / 脚本产物）。只要含一项在途修复 → 禁止整路径 checkout，改走下面第 2 条。
2. **还原必须显式点名**：与「Windows 清理临时文件用显式文件名、通配符会误删基线」同一哲学——还原噪声文件只允许逐个写显式路径（`git checkout -- example/audio-s0/probe.log`），禁止整目录 / 通配；噪声多时先列清单再逐个执行。
3. **在途工作优先落盘**：会话中段要清理工作区时，正确顺序是先把在途修复 `git add` 到暂存或 `git stash push -- <修复文件>`，再清噪声；事后还原 stash 必须核对内容。
4. **`git switch` / `git checkout <branch>` 同闸**：切分支前 `git status` 确认无未提交工作，或已妥善 stash/提交；禁止带脏工作区盲切。
5. **危险操作后必须自证**：任何还原 / stash / 切分支动作执行后，立即重跑受影响面最小的验证（如单用例回归），确认预期内容仍在，才允许继续后续步骤。
6. **行尾 / eol 规则变更必须当场体检（2026-11-10 固化，起因：主树 25 个 s 系 bat 陈旧 LF 检出）**：Git 在 `.gitattributes` 的 eol 规则变更时**不会自动重检出**已有工作树文件——index 是 LF、工作树也是 LF，status 干净，只有读原始字节的工具能抓到；长寿命检出（主树）永远比 wt 先踩。铁律：① 凡改 `.gitattributes` 的 eol/文本规则，**当场**跑 `git ls-files --eol` 审计（`i/lf + w/lf + attr eol=crlf` 交集 = 陈旧名单）并立即重 smudge，不留到下次回归才爆；② bat 门禁 FAIL 报 LF-only 时直接 `python tools/check-bat-encoding.py --fix` 机械修复（修复后 `git add` 清幻影 M，`git diff --cached` 必须为空才算干净）；③ 修复动作是纯工作树操作，index/提交零变化，**禁止**用「改完提交行尾 churn」的方式处理。

---

## 并行轨道 worktree 纪律（2026-11-08 固化）

**多并行任务互不干扰靠固定的 `feat/*` worktree 实现，不动态建临时分支、不引入更复杂的调度机制。** 单用户开发、无多进程抢坑，固定分支 + 工作线粘性已满足"互不干扰"。

1. **什么进 worktree / 什么不进**：
   - **进**：A 类并行轨道（引擎化 / 非渲染，不碰 `kStage`、不进渲染回归）—— 如材质图、Job 系统、动作系统。
   - **不进**：VSM（S9 后插入）、TSR（S7 后拆分，见 `ROADMAP.md`）是主线 S 节奏的插入项，留在主 worktree 顺着 S 轨走，不拆出。
2. **实例注册表（四个登记轨道，占用说明见 `docs/PARALLEL-TRACKS.md` §7）**：

   | 轨道 | worktree 绝对路径 | 分支 | 计划归属 |
   |---|---|---|---|
   | Job / Task 并行 | `E:/UE_STUDY_2026/nl-engine-job-system`（junction 共享 `.workbuddy`/`slang dist`） | `feat/job-system`（保留） | **M-J5a 已合回 master**（merge `0accf6e`，2026-11-09）：vendored Taskflow 3.11.0 做显式依赖 DAG + A 档 4 路 slangc fan-in 并行 + 抽共用编译入口 `recompileGraph`；M-J0~J3 已收官 + 双向合并回 master + 可视化证据图补交；M-J4 仍等 P9 V4，M-J5b/c 为后续步。计划 `docs/p8/P8-JOB-PLAN.md`（REUSE 盘点与后续消费者已并成一份） |
   | 材质图 → Slang | ~~`E:/UE_STUDY_2026/nl-engine-material-graph`~~（worktree 已移除，2026-11-09） | `feat/material-graph`（保留） | **已收官**：B2~B4（tag `mg-b2b4`）+ T1 编辑器 M1~M3（tag `mg-t1`，merge commit `e5505bc`）全量合入 master；B5 接 PBR + T1-6 预览视口排 S7 后，届时按 §7.1b 重建 |
   | 动作系统 / 动画控制器 | ~~`E:/UE_STUDY_2026/nl-engine-action-system`~~（worktree 已移除，2026-11-09） | `feat/action-system`（保留） | **已收官**：M-A0~M-A5.4 全量 merge 回 master（merge commit `5a1912f`，tag `anim-ma1`~`anim-ma5c`）；nl/anim 纯逻辑层 + anim-tools 自测 + viewer skin 模式 + skindump/skin-demo 四资产演示页；验收纪要 `docs/MILESTONES.md` M-A 节 + `docs/action-system/ACTION-PLAN.md` + `ANIM-TOOLS-GUIDE.md`；GPU 调色板等更大规模蒙皮资产再立项 |
   | 开发者控制台 | ~~`E:/UE_STUDY_2026/nl-engine-console`~~（worktree 已拆，2026-11-09） | `feat/console`（保留） | **已收官**：M-C0~C3 全量 merge 回 master（merge commit `9e12cc6`，tag `console-mc3`）：core/console 纯逻辑内核（CVar 四类型 + ExecCode 四档）+ ImGui 前端（`` ` `` 呼出/Tab 补全/历史/可拖缩）+ installBuiltins（help/cvar.list/cvar.set）+ cam.*/hud.show 真参数回调 + panelShow 解耦；selftest 49/49、双后端构建+selftest 绿、展示台 console-mc3 卡五用例实跑；归档 `docs/p8/CONSOLE-TOOLS-GUIDE.md` + `docs/screenshots/CONSOLE-EVIDENCE.md`；计划 `docs/p8/P8-CONSOLE-PLAN.md` |
   | ECS-lite 场景 | `E:/UE_STUDY_2026/nl-engine-ecs` | `feat/ecs` | **E1a 收官 / 全量 merge 回 master**（2026-11-09，merge commit `2d4ad49`，tags `ecs-e0`~`ecs-e3`）：Carrot 式 OOP 组件内核（World/Entity 延迟增删 + Component 层级 + System 签名收编 + 场景 JSON roundtrip 字节一致）+ `nanite-scene` 工具 + 展示台 `p8-ecs` 卡 4 条 + `docs/p8/ECS-TOOLS-GUIDE.md`；wt 保留（E1b 渲染集成后挂等 S6 收官 tag `s6-done`，届时另编号触发渲染纪律）；计划 `docs/p8/P8-ECS-PLAN.md` |
   | 物理（Jolt vendored 集成） | `E:/UE_STUDY_2026/nl-engine-physics` | `feat/physics` | **M-P0 + M-P1 + M-P2 已收口**（tags `phys-p0` / `phys-p1` / `phys-p2`，2026-11-09 提前解锁排期闸）：worktree 已建（junction 共享 + skip-worktree），Jolt v5.3.1 + 最小世界 + 4 层碰撞矩阵与双向关联 + 6 箱体堆叠稳定/休眠岛三用例；M-P0/M-P1 TSV 双基线逐字节回归锁死；**M-P5 场景集成仍 gate 在 P8 E3 之后**；计划 `docs/p8/P8-PHYS-PLAN.md` |
   | 3D 空间音频（vendored miniaudio） | `E:/UE_STUDY_2026/nl-engine-audio` | `feat/audio` | **M-S0 已收口**（tag `audio-s0`，2026-11-09 用户指令「开工 M-S0 建 feat/audio」= 提前解禁 M-S0~S2）：worktree 已建（junction 共享 + skip-worktree）；vendored v0.11.26 + `nl/audio` 薄封装 + `audio-tools` 离线确定性 PCM dump（双后端 selftest 21/21）+ 展示台 `audio-s0` 卡 7 用例 + 波形/验收双证据图；归档 `docs/p8/AUDIO-TOOLS-GUIDE.md`；**M-S3 场景集成仍 gate 在 P8 E1/E1b 之后**；计划 `docs/p8/P8-AUDIO-PLAN.md` |
   | 驾驶能力轴：碰撞事件桥 + 载具物理 | `E:/UE_STUDY_2026/nl-engine-drive` | `feat/drive` | **M-D0 已收口**（tag `drive-m0`，2026-11-10）：Jolt `ContactListener` 三回调 → Jolt-free `ContactEvent` POD + `simulateContactProbe` 三体事件流 + phys-tools `contact-trace`/`contact-check`/selftest `contacts` 节；Q2 owner 缓存实证 6/6 removed 命中（cache_misses=0）；DX12/VULKAN TSV 逐字节一致、ctest 4/4、`example/drive-m0` 五段式 ALL GREEN；归档 `docs/p8/DRIVE-TOOLS-GUIDE.md`；**M-D1 已收官**（tag `drive-m1`，2026-11-10）：bevy 两帧保活双缓冲 `ContactEventStream` + PostPhysics 自足 `ContactEventSystem`（`TickGroup` 枚举序为唯一顺序源，owner→Entity 双 `exists()` 过滤）+ phys-tools `ecs-trace`/`ecs-check`，真实帧循环 240 帧 41 事件全消费 `dropped=0`、DX12/VULKAN TSV 逐字节一致、双后端 ctest 各 7/7、`example/drive-m1` 五段式 ALL GREEN；**M-D2 / M-D3 均已收官**（tag `drive-m2` / `drive-m3`，2026-11-10）：M-D2 = Jolt `VehicleConstraint` 无头集成（Q-D2-1 拍板 A fresh world、4 轮 raycast 接触边沿合成事件喂 M-D1 流）；M-D3 = `DebugScene` scenario 2 训练场 + viewer 注入管线 / `vehicle.*` cvar / 默认关独立 HUD（Q-D3-1~4 拍板，相机代码零改动、`kStage` 保持 `S8.2`）。**整轨 M-D0~D3 已全量 merge 回 master**（2026-11-10，merge commit `a2e3cff`，tags `drive-m0`~`drive-m3` 全成 master 祖先；冲突 1 处 = `MILESTONES.md` 两侧四栏并集保留，`example/cases.json` 由 `merge-cases.py` 语义并集 80→84 张卡）；合并后双后端构建 + selftest 16/16×2、归档 TSV 六组逐字节零回归、ctest 双后端各 12/12、`--only drive-m3` 12/12 bytes、`server.py --check` exit 0。**wt 已拆**（2026-11-10 按 camera 惯例：先摘 junction 再 `git worktree remove`，TRACKS 标 CLOSED；`feat/drive` 分支与四个 tag 留档）；camera b 段前置已解除。计划 `docs/p8/P8-DRIVE-PLAN.md` |

   三者均 `junction` 共享主 worktree 的 `.workbuddy` 与 `thirdparty/slang/dist`；`.workbuddy` 已 `skip-worktree`（git status 干净，不会误把纪律源 commit 进 feature 分支）。
   **Junction 建/删操作铁律（2026-11-09 用户点名低级错误后固化——建与删的坑都踩过两遍）**：Git Bash 里一切 `cmd //c + Windows 路径` 的尝试都会被 MSYS 路径转换破坏（建：`mklink /J` 坏；删：`rmdir` 坏或进交互模式挂起），**禁止发起**，一律 PowerShell 工具原生 API 一次到位——**建**：`New-Item -ItemType Junction -Path <link> -Target <目标>`；**删**（只摘链接、绝不穿透目标）：`[IO.Directory]::Delete($path, $false)`。删完必验证目标目录内容完好再继续拆 wt（`git worktree remove --force` 前必须先摘全部 junction，否则 force 会穿透删共享内容）。
   **待建 worktree（已立项，执行排 P8 场景整合 E1 之后）**：~~寻路 `feat/pathfinding`~~（**2026-11-11 整轨 M-N0~M-N6 收官**：merge `5356c80` + `d3d6127`，tag `nav-m0`~`nav-n6` / `nav-done`，worktree 已拆 / TRACKS CLOSED / 分支 + `milestone/nav-done` 保留；重开按 §7.1b）；**3D 音频 `feat/audio` 已按下方「例外 4」提前建树开工，不再属于待建**。**例外（2026-11-09 拍板）**：ECS 场景轨道（`feat/ecs`，`docs/p8/P8-ECS-PLAN.md`）作为 E1 的 E1a 拆分段**提前开工**，不受此排期闸限制。**例外 2（2026-11-09 拍板）**：脚本轨道（`feat/script`，`docs/p8/P8-SCRIPT-PLAN.md`）E4 脚本段**解禁提前**（鸣潮方向 C#/.NET hosting，目标扩展+热更），M-SC0 无依赖即刻开工。**→ 已收官（2026-11-09）**：M-SC0~SC3 全链 merge 回 master（merge commit `1b49b5f`，tags `m-sc0`~`m-sc3`；M-SC3 = 脚本热更 bootstrap 中转 + collectible ALC）；worktree 已拆（junction 链接先摘后拆，见上方铁律），`feat/script` 分支保留；归档 `docs/p8/SCRIPT-TOOLS-GUIDE.md`。**例外 3（2026-11-09 用户指令「建 feat/physics 开发物理模块内容」）**：物理轨道（上表行）提前开工 M-P0~M-P3（这几步只吃 Jolt 世界 + 体封装，不依赖场景整合；计划 §排期约束括号内亦允许与 E1 并行）；**E3（序列化 + Inspector）依赖的 M-P5 不解锁**，仍按原排期闸执行。**例外 4（2026-11-09 用户指令「开工 M-S0 建 feat/audio」）**：音频轨道（上表行）提前建树开工 **M-S0~S2**（纯逻辑 / 无设备离线渲染，不依赖场景整合，照物理 §7.1e 先例）；**M-S3 场景集成仍 gate 在 P8 E1/E1b 之后**，不随本轮解锁；M-S0 已收口（tag `audio-s0`），详见 `docs/p8/AUDIO-TOOLS-GUIDE.md` 与 `docs/MILESTONES.md` P8 M-S0 节。**选型二次确认（2026-11-09）**：用户拍板物理用现成库、不自研（成本取舍），候选评审后**继续走 Jolt 5.3.1**；例外理由不再是 UE 口径（UE5 自研 Chaos，见 `UE-DIFFS.md` D10），UE 口径不参与选型。
3. **固化文档闸**：track 分支 **merge 回 master 前**，必须在本轨道 `S*-PLAN` / `MILESTONES.md` 写验收纪要（目标达成 / 判据 / 证据 / 踩坑），否则不 merge。坑点只活在工作记忆 = 下个会话必重踩。
4. **假绿防护 · 占坑 ≠ 文件指向**：
   - 在 track worktree 干活时，所有编辑绝对路径、shell 每条命令的 cwd **显式在对应 wt 路径内**，不依赖默认落点；
   - **cd 必须落在 command 文本里（2026-11-10 M-GL5 事故加固）**：`cd <树路径> &&` 必须写在**每条** shell 命令的开头——写在命令描述/参数说明/任务清单里不算数，只有 command 文本本身带 cd 才有效；会话中途 cwd 会被重置回默认树，长命令链的每一节都要自带 cd。危险等级分层：`regression.py` 等接了树闸的工具落错树会被拒（exit 2），但 `build-m0.2.bat`、ctest 等未接闸的命令落错树 = **直接在错树里构建/重链**（M-GL5 两次误构建主树即此因）；拿不准就 `cd <树> && pwd` 双保险，构建/回归前必跑；
   - merge 回 master 前**双向自查**：`git -C <wt> status` 看得到本批改动、主 worktree `git status` **没有**本批改动（出现即落错树，立即迁移）；
   - 验证可信度自查：改了什么就在产物里找什么——文本 / 数值类改动后若无任何差异是红旗不是绿灯。
5. **工作线粘性**：同一轨道回同一 worktree，一条工作线**只 merge 一次**（验收 FAIL→补丁→复验全部在占坑内闭环，不中途 merge 又裸干）。任务收尾才 merge 回 master，下个任务回同一坑。
6. **收尾即同步关联文档（2026-11-08 用户拍板固化）**：worktree 内每完成一个里程碑 / 阶段，除在 track 分支写本轨道验收纪要外，**必须同步更新 master 侧关联文档**——`AGENTS.md` 实例注册表状态、`docs/PARALLEL-TRACKS.md` §7 注册表、相关候选 / 计划文档（如 `NON-RENDER-ROADMAP.md`）——不许让 master 侧文档停在过时值。注册表 / 跨轨道文档的更新在**主 worktree 提交**（避免与 feature 分支双份冲突；教训：动作系统 M-A5.1 已落地，master 侧注册表仍写「候选」，下个会话即误判进度）。
7. **对话归属登记 = wt_pool 占坑闸（2026-11-09 用户拍板，事故防复发）**：**任务号 = 里程碑编号**（如 M-C1、S4.2，里程碑编号天然关联轨道，不另设任务编号体系）。①**每个对话开场必须声明里程碑编号**；用户没给就先提示补充，补上后立即登记归属：`python tools/wt_pool.py claim --track <轨道> --milestone <编号> --note "本对话用途"`。②**动任何 worktree（编辑 / merge / 删除 / 清理）前先过闸**：`python tools/wt_pool.py check --track <轨道> --milestone <编号>`；CLOSED 轨道一律不碰（要重开按 §7.1b 重建）；被其他里程碑占用一律停手。③锁文件 `.wt_pool_busy.json` 在主树根，全部 worktree 共享一把锁（已 .gitignore）。④工具自测 `python tools/wt_pool.py selftest`（21 项），全表 `python tools/wt_pool.py status`。本条是对话轨道边界的机械可查实现——「这个对话只处理自己轨道的东西」不再只靠自觉。
8. **树边界闸（2026-11-10 用户拍板，防占坑会话越界污染）**：占坑会话伸手改主树 / 别的 wt（跑错树的 regression、重拍错树的 example 基线）是错乱主因，两道机械闸：①**cwd 自查**：claim 后第一步、以及任何切换目录之后跑 `python tools/wt_pool.py scope [--milestone <编号>]`——在自己 wt 内 = exit 0；在主树且锁里有活跃 claim = exit 2 警告（主线会话核实后可忽略）；在别的 track 的 wt 内 = exit 1 拒绝。②**脚本树闸**：共享入口工具（`tools/regression.py`、`tools/showcase/server.py`、`tools/reshot-hud-s71.py`，后续同类入口照接）开头已接 `tools/tree_gate.py`——**脚本所在树 ≠ cwd 所在树 → exit 2 拒绝**，跑工具一律用自己的树的副本；刻意例外（CI 沙箱 cwd 无关等）设 `NL_TREE_GATE_ALLOW=1` 放行（输出 WARN）。③`wt_pool.py` 本身不受树闸限制（它就是跨树占坑工具，锁在共享 .git）。

> 完整版（含占用说明、M-J0 等里程碑衔接）见 `docs/PARALLEL-TRACKS.md` §7。

---

## 本项目其余硬约束（速查，完整见各文档）

- **回复语言：简体中文（2026-11-09 固化）**：本项目所有 Agent 回复默认使用简体中文；代码、标识符、技术术语、命令、文件路径等保留英文原文不翻译。
- **UE 源码只读不抄**：EULA 与项目 MIT 不兼容，一行 UE 代码都不许进 repo；参考结构与边界情况处理思路。
- **构建链不依赖任何厂商图形 SDK**：`CMakeLists.txt` 不得出现 `VULKAN_SDK` / `find_package(Vulkan)`。后端对称：D3D12 走 Windows SDK，Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，Slang 独立获取。
- **图形 API / NVRHI / ImGui 符号只允许在 `renderer/`**；`core/` / `viewer/` 零命中。
- **双后端纪律（必须同时通过 / 画面一致分档 / 与官方对齐）**：见上方「代码开发与双后端纪律」节。
- **构建 / 编译 / 运行期踩坑汇总**：见 `docs/DEV-PITFALLS.md`（NMake 必用、Vulkan 静态链接宏、139 段错误、SPIR-V 入口恒 `main`、NMake 时间戳陷阱、`SceneConstants` 160B 等）。
- **里程碑必须有「交代」**：见上方「里程碑交付纪律」节（渲染版 = `example/` 基线、工具版 = 展示台用例 + 归档；tag 在验收后打、提交前 regression 全绿、四栏台账写实、约定写回 canonical 文档）。
- **展示台网站（里程碑 review 入口，2026-11-09 固化）**：`tools/showcase/start.bat` 或 `python tools/showcase/server.py --port 8787` → http://127.0.0.1:8787/；侧栏按里程碑版本回放成功用例，数据源唯一 `example/cases.json`、基线图在 `example/<版本>/`；验收闸是 `--check` 退出 0；起服务前先 `netstat -ano | grep 8787` 查残留进程（详见 `docs/DEV-PITFALLS.md`）。
- **用户实测结论 > 任何网页 / 官方文档信息**。
