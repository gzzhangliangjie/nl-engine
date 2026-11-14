# NL-GUARD-PLAN — WorkBuddy PreToolUse 机械闸落码计划

> 2026-11-14 深夜拍板前评审稿。目的：把散在 MEMORY.md / AGENTS.md / wt_gate / pre-commit 里「靠记忆执行」的纪律，机械化为 WorkBuddy PreToolUse 钩子，在工具调用边界拦截。**本文是唯一实现依据，后续会话照此落码，不必重读 wt_gate / rea_potato / 会话日志。**
>
> 起因：M-SC6 收口轮四条命令（add/commit/tag/close 闸）全部漏 `cd <wt> && pwd` 前缀，tag `m-sc6` 打到主仓 HEAD（截图实锤）。cwd 纪律 2026-11-14 已定过一次（连踩 5+ 次），靠记忆必然再犯 → 必须机械闸。

---

## 0. 已核实的外部事实（不重复查证）

- **WorkBuddy 支持 PreToolUse 钩子**，文档 `workbuddy.cn/docs/cli/hooks` + `docs/cli/permissions`，契约与 ZCode（rea_potato 在用）一致：
  - stdin 收 JSON：`{"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"...","run_in_background":...},"cwd":"..."}` —— **cwd 由 harness 机器提供，闸不依赖 agent 记忆**。
  - stdout 出 JSON：`{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"allow|deny|ask","permissionDecisionReason":"..."}}`；`permissionDecision` 在常规权限规则前生效。
  - 退出码：0 = 按 JSON 决策；**2 = 阻断**（stderr 回填给模型）；其他非 0 = 非阻断放行。
  - matcher 支持工具名（`Bash`、`Edit|Write`、`TaskOutput` 等）。
  - 配置落点：settings 的 `hooks` 键。本机 `C:\Users\zlj10\.workbuddy\settings.json` 现无 `hooks` 键，干净可加。
- **rea_potato 参照实现**：真身 `E:\rea_potato\Tools\hook_redline_guard.py`（1771 行，含 `--selftest` 样本自测 / `--stats` 账本 / log 滚动），薄壳 `~/.zcode/cli/hooks/redline_guard.py`（fail-open）。potato_ue 同构（两壳两真身，scope 门卫互不越界）。**抄思路不抄代码**。
- 本机已装 Python `C:\Python313\python.exe`（ZCode 钩子同款）。

## 1. 现有闸的覆盖缺口（钩子补的就是这些洞）

| 现有闸 | 覆盖 | 缺口 |
|---|---|---|
| `wt_gate.py check-cmd` | 只挂在**脚本入口**（regression/dump 等写文件类工具） | 裸 git（add/commit/tag/branch/merge）、裸 rm、重定向写全不过闸 |
| `wt_gate.py check-build` | .bat 构建入口 | 同上 |
| `.git/hooks/pre-commit` | master 上 commit（有 active claim 拒） | **不防 master 上 tag**（m-sc6 事故实锤）、不防 branch/merge |
| `wt_pool.py` | claim/release 流程 | 不判命令 cwd |

## 2. 挪进钩子的规则清单（已评审，待拍板档位）

### A 族：cwd 归属（最高价值，2026-11-13/14 事故全在这族）

| # | 规则 | 档位建议 | 纠正指令（写进 deny reason） |
|---|---|---|---|
| A1 | 主树 cwd + 删/写/追加类命令（`rm`、`>`/`>>`、`git add/commit/tag/branch/merge`）+ **存在 active claim** → deny | deny | `cd <对应wt>` 或 `NL_CMD_MAINREPO=1`（与 pre-commit 现语义对齐：**无 claim 时主树放行**，主树是合法工作场景） |
| A2 | master 上 `git tag` / `git branch` / `git merge` 且有 active claim → deny | deny | 补 pre-commit 盲区（tag）；理由同 A1 |
| A3 | 命令含 `cases.json` 且写落点解析为主树 → deny | deny | 2026-11-13 误写主树事故 |

**A 族实现要点**：钩子内自己查——cwd 经 `tree_root_of`/`main_root_of`（wt_gate.py 已有同款函数，直接 import 或复制）→ 读主树 `.wt_pool_busy.json` 判 active claim → 命令串按 `|| && | ; \n` 切段逐段判。

### B 族：命令形态（与 cwd 无关，纯文本可判）

| # | 规则 | 档位建议 | 依据 |
|---|---|---|---|
| B1 | `rm` 相对路径删构建产物（`rm ... build/...` 无 `/` 前缀） | deny | 主树产物误删×2 根因；放行形态 = 绝对路径或 `wt.bat clean` |
| B2 | 裸 `rm` 通配符（`*.tmp`/`*.log`）在个人/主树目录 | deny | 用户级记忆「No-Go Zones」 |
| B3 | `git restore .` / `checkout -- .` / `restore <path>` 整树或点名还原 | deny（显式点名才放） | 「还原只许显式点名」铁律 |
| B4 | `git checkout/restore/switch` 且目标路径 dirty → ask（钩子内跑 `git status --porcelain -- <path>` 实查） | ask | 「checkout 前必 status」机械化；rea_potato git-checkout-ref 同款 |
| B5 | `git reset --hard` / `git clean -f` | ask | rea_potato 同款 |
| B6 | `regression.py` 不带 `--only` → deny | deny | 「禁顺手全量」（2026-11-08 拍板） |
| B7 | ctest 缺 `--repeat until-pass:3` → deny | deny | ctest 快进程丢 stdout 竞态（2026-11-10 固化） |
| B8 | `run_in_background=true` 且命令缺 `bg-last.log` 尾锚 → ask | ask | AGENTS 9b「后台命令尾部必须写结果锚」（2026-11-14 拍板）；`tool_input.run_in_background` 字段可见 |
| B9 | viewer 直跑缺 `--screenshot` | warn | 574dcaf 已从工具侧焊死，钩子做兜底提醒 |
| B10 | `TaskOutput` timeout > 120000ms | warn | 防 600s 阻塞烧积分复发（matcher 配 `TaskOutput`） |

**档位语义**：deny = 直接拦 + reason 给纠正命令；ask = 强制弹用户确认；warn = 放行 + `systemMessage` 提醒。可先落 deny + ask 两档，warn 观察一轮再定。

### 不挪的（留原位，勿重复造）

- pre-commit 的提交内容校验、`wt_pool.py close` 报告闸 → 流程语义层；
- kStage bump 重拍波及、eol 判定、构建 MD5、双后端字典 → 跑时状态 / 内容层，钩子判不了；
- 沙箱删除 fail-closed / 回收站语义 → OS 层。

## 3. 设计铁律（从 rea_potato 移植，三条不可破）

1. **薄壳 fail-open**：壳 `~/.workbuddy/hooks/nl_guard.py`（~30 行，runpy 加载真身），真身 `tools/nl_guard.py` 在主仓版本化。**壳任何异常（真身缺失/读不了/内部报错）一律 exit 0 放行**——闸坏只失效不封路。历史教训：真身被删 → python exit 2 = PreToolUse deny = 全会话 Bash 锁死。
2. **scope 门卫**：cwd 不在 `E:/UE_STUDY_2026/nl-engine` 家族前缀（主树 / `_pool\wtN` / 平铺 wt）→ 静默 allow。不误伤其他项目（用户 2026-11-04 定的规则；rea_potato SCOPE_PREFIX 同款）。
3. **deny reason 必带纠正命令**（`cd <wt>` / `NL_CMD_MAINREPO=1` / `wt.bat <track> ...`），留痕写 `.workbuddy/gate-blocks.log`（wt_gate 已在写同一本账，格式：`时间 gate=<名> cwd=<cwd> reason=<规则名>`）。

其他移植细节：
- **活文本语义**：单引号内、`git commit -m "..."` 等死引号内容抹掉再匹配，防误伤（rea_potato spans_of/live_text 可照抄思路）。
- **调试放行**：环境变量 `NL_GUARD_DISABLE=1` 全闸临时失效。
- **自测**：`--selftest` 样本集（每条规则至少 正向命中 / 负向放行 两个样本），落码时全绿才算完成；`--stats` 看拦截账本。
- **钩子超时**：15s 级（ZCode 同款），钩子内 git 子查询必须短超时 + 异常吞掉按 fail-open 放行。

## 4. 落码清单（拍板后执行）

1. `tools/nl_guard.py`：真身（读 stdin JSON → scope 判 → A/B 族规则 → JSON 输出 + 留痕 + selftest）。
2. `~/.workbuddy/hooks/nl_guard.py`：薄壳，指真身，fail-open。
3. `C:\Users\zlj10\.workbuddy\settings.json`：加 `hooks` 键——`PreToolUse` matcher `Bash` + `TaskOutput` 各挂壳（`type: command`，绝对 python 路径）。
4. `--selftest` 全绿 + 用一条已知违规命令（如主树裸 `git tag`）实测 deny 一次（沙箱内可先模拟 stdin 验证，真钩子行为需用户终端确认）。
5. 文档收尾：本文件标注落码 commit；MEMORY.md「cwd 漂移双闸 B+C」条目更新为「双闸 B+C + 钩子层兜底」。
6. 提交：主仓 commit（`NL_COMMIT_MAINREPO=1` 视 claim 状态）+ 写台账。

## 5. 待拍板项（落码前必须清零，AGENTS 开工闸）

| 项 | 选项 |
|---|---|
| Q1 A1/A2 覆盖面 | 建议已定：有 active claim 才拦主树，无 claim 放行（与 pre-commit 对齐）。**待确认** |
| Q2 B 族档位 | 建议已定：B1/B2/B3/B6/B7 deny；B4/B5/B8 ask；B9/B10 warn（可缓落）。**待确认** |
| Q3 首批范围 | 建议：A 族 + B1/B3/B6/B7 先落（实锤事故族），B4/B5/B8 二批，B9/B10 观察。**待确认** |
