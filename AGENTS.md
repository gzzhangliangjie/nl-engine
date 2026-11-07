# AGENTS.md — nl-engine 项目协作铁律

> 本文件是 **Agent 协作的权威指令**，每次在本仓库开工前必须通读。
> 配套详版（背景 + 例子 + 自我纠偏）：`docs/AGENT-RULES.md`。其余项目约定见 `docs/ROADMAP.md` 与 `.workbuddy/memory/MEMORY.md`。

---

## 决策前置铁律（最高优先级，2026-11-07 固化）

**每开始一轮新任务，必须同时读过下面三处参考代码 + 本项目开发历史，带着证据做决断；任何超出「已定计划内机械执行」的判定，不许自己拍板——先把三处证据 + 历史决策摆出来，再让用户定。**

用户已多次指出此前每轮任务没有遵守本条。违反即视为失职，用户可随时追责（「这轮你读三处代码了吗」）。

---

## 每轮任务必做的 4 步前置（checklist，缺一不可）

1. **Carrot 代码** — `E:\UE_STUDY_2026\Carrot\engine\engine\render\` + `engine\resources\shaders\`（.slang）。
2. **bevy 代码** — `E:\UE_STUDY_2026\bevy\crates\bevy_pbr\src\meshlet\` + `crates\bevy_solari\src\realtime\`。
3. **UE5 官方代码** — `E:\UnrealEngine\5.8.4\Engine\Source\Runtime\Renderer\Private\`（Nanite / Lumen 模块），只读对照表点名的文件，定向读不漫游。
4. **开发历史** — 至少读：
   - `docs/MILESTONES.md`（四栏台账：动作 / 提交版本 / 完成情况 / 踩坑）
   - `docs/carrot-history.md`
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

---

## 本项目其余硬约束（速查，完整见各文档）

- **UE 源码只读不抄**：EULA 与项目 MIT 不兼容，一行 UE 代码都不许进 repo；参考结构与边界情况处理思路。
- **构建链不依赖任何厂商图形 SDK**：`CMakeLists.txt` 不得出现 `VULKAN_SDK` / `find_package(Vulkan)`。后端对称：D3D12 走 Windows SDK，Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，Slang 独立获取。
- **图形 API / NVRHI / ImGui 符号只允许在 `renderer/`**；`core/` / `viewer/` 零命中。
- **一份 `.slang` 双目标（SPIR-V + DXIL）两后端加载成功** = 通用管线验收核心。
- **里程碑必须有「交代」**：渲染版本 = `example/` 截图基线；工具版本 = 展示台 `kind:"tool"` 用例（实跑输出展开）+ `docs/s*/...-TOOLS-GUIDE.md` 归档。提交 / tag 前 `python tools/regression.py` 全绿、`.workbuddy/memory/MEMORY.md` 登记四栏台账。
- **用户实测结论 > 任何网页 / 官方文档信息**。
