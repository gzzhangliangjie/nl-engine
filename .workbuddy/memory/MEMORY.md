# nl-engine 项目长期约定

> 铁律权威落点：根 `AGENTS.md`；台账 `docs/MILESTONES.md`（每里程碑四栏 + 踩坑，过程细节查这里）；并行轨道 `docs/PARALLEL-TRACKS.md`。本文件只记「AGENTS.md 没写、但容易忘」的不变量与当前状态。

## 本机环境
- VS2022 MSVC 14.38（**C++17**，禁 C++20 API）+ Win SDK 26100；CMake 3.31.6 便携版 `E:\UE_STUDY_2026\.build-tools\`。构建 `scripts/wt.bat <track> build|ctest|regression|clean`（2026-11-13 拍板：track 名显式定树 cwd 无关；`build-m0.2.bat` 内接 wt_gate.py 非注册 wt 拒跑 exit 3，主仓须 `NL_BUILD_MAINREPO=1`）。同秒 mtime 跳重链 → 删 exe 强制重链；**改 `.slang` 后必须删两棵 `build/{DX12,VULKAN}/viewer.exe` 再重链**。裸 Git Bash configure 报 nmake not found = 环境三件套没 export。
- cmd bat 三坑：① `shift` 顶掉 `%0` → `%~dp0` 开头先固化；② `for` 体内 `%VAR%` 解析期展开 → 循环体别用变量（显式写两遍）；③ 参数别拼进引号路径串。**run.bat 复制模板先核对 set 变量与引用一一对应**（M-T3 事故：用了 `%OUT%` 没定义，产物全落仓库根）。
- ctest 快进程随机丢 stdout（非回归）→ 统一 `--repeat until-pass:3`；missing anchor 先重跑再定性。ctest 新增用例后必须双后端重新 configure；固定步长累加 float/double 必须同源。
- Bash cwd 不跨命令 → 每条命令开头 `cd <wt> && pwd`，一律 `git -C <wt>` / 绝对路径。GitHub 直连断；nl-engine 无 remote，tag/分支只落本地。

## 架构不变量
- NVRHI+Slang 通用管线；构建链禁厂商图形 SDK；OpenGL 已否决；`NL_GRAPHICS_BACKEND` 唯一开关；图形符号只在 renderer/；一份 .slang 双后端 = 验收核心。
- SPIR-V 绑定 shift 256/0/128；禁 `[[vk::binding]]`；volatile CB `setMaxVersions(16)` 每帧全量覆写；SceneConstants 160B 禁整块 memcpy；HUD 面板高≤560、字符串 ASCII（详 `SHADER-CONVENTIONS.md` §2.7~2.19）。
- **CB 尺寸以 `dxc -dumpbin` 反汇编 cbuffer Size/offset 为准**：分配小于声明 → shader 读越界得 0 → 全线程静默早退无报错；CPU 常量数组长度 = cbuffer 字节/4。root sig layout 漏槽 → PSO 失败（多于无害）。slang 无 C++ lambda → `inout` 参数函数。
- **冻结基线的代码形态也是基线的一部分**（M11.1）：改冻结路径前先核 bug 还是基线语义；重开须单独拍板 + 重拍。
- Q11-1 拍板 A：GPU 数值进断言走**统计不变量**，CPU 仿真侧保持 bytes。
- M8.1：DX12 帧命令列表常驻 `frameCmd_` 成员、Vulkan per-frame 新建——两后端不对称，勿顺手统一；AS build 上传+setBufferState+build 必须同命令列表。
- **Scene 新增 GPU 资源必须同步进 shutdown 清单**（S15 UAF 教训：~Scene 时 VK device 已死）；**资源初值语义与消费方全集绑定**（页池未初始化被读整池 → HZB 误剔）。
- **ECS 先例**：NavAgentSystem/TrafficCarSystem = `SignedSystem<Comp, Transform>` 固定步长累加器同构；组件只持 POD 零引擎模块依赖（防 scene→traffic/nav 反向依赖）；sim 引擎在系统内。**newEntity 延迟落表坑**：按名查找族只扫 entities_，须先 flushEntityCreationAndRemoval。
- **viewer 无裸 usage 模式**：无参 = 起窗口渲染不退出（M-T3 事故同 M15.2 族）；工具 usage 段用各 tools exe（exit 2）。`--traffic-steps` 类「调 N 次 tick」必须钉 `GameClock::kStep*1000` ms（真实 dt 抖动 → 步数不定 → PNG 无法对拍）。

## 当前进度（2026-11-14）
- **kStage 仍 S8.2**。**已 merge 回 master**：S0~S13、S15（`165216d`）、网络栈 net-m0~3（`9aa58df`）、寻路 nav-m0~6（`5356c80`）、天气 M-WEA0~3 整轨（`4090d94`+`ebf5f50`，2026-11-14 收官）；相关 wt 已拆，分支与 tag 保留。
- **未 merge 回 master（wt 在册，整轨 merge 待用户指令；S13 教训：merge 后必须重跑该轨全部历史版本卡）**：
  - **script 轨道**（feat/script）：M-SC4~SC7 收官（tag `m-sc4`~`m-sc7`）——绑定生成器（exports.api 单源 → gen.py 五件套 + 防漂移 static_assert + CMake 新鲜度闸）→ Time/Input/Log → Camera → ECS 模型挂载（拍板 A：binding 只动组件 + Model 集合签名比对重建）。下一步 M-SC8 Physics 载具 → M-SC10 属性面板 → M-SC11 收口。**教训：没落码的计划条目不是「既定决策」**。
  - **交通轨道 T0**（worldgen wt）：**M-T0~T3 全收官**（tag `traffic-m0`~`traffic-m3` @ `4652a16`）。M-T0 车道级展开图 + A* + C1 跟车律；M-T1 信号+占用锁；M-T2 组共享锁+gap-out+loop（城市吞吐 17/32/646.9m，+143%）；M-T3 CityCar 接 viewer（TrafficCarSystem ECS + `--traffic-city`，42 车永续车流，cases 卡 134）。**核心教训（自适应控制几何）**：固定常数按 10m 夹具标定在 ~4m 路网系统性持锁泄漏 → `stopS=length−min(entryStop,0.6·length)`、`clearEff=min(3,0.3·最小邻接边长)` 根修；死锁定性先 LOCK dump 实证再改。**T1 教训：并行度放开前先审「组内不相交」几何前提，豁免口径从拓扑升级到几何**。后续项：需求感应进 citygen signal 填充（W 轨）、红绿灯摆件可视化（另立项）、冲突型路口。
  - **粒子轨 M-PT0~PT2 收官**（feat/particles，卡 particles-pt0~pt5）。
  - **M-CRASH0 / M-PERF0 / M-PERF1 收官**（tags `crash-m0`/`perf-m0`/`perf-m1`；PassTimers 13 pass；权威落点 `docs/perf/PERF-PLAN.md`）。
- 独立后续项（需单独拍板）：shader trace-coverage（PERF C 档）；`tools/analyze-crash.bat` UTF-8 rem 违规顺手修；行驶段循环 demo 旗标。

## 工程纪律（详 AGENTS.md）
- 收口第一屏 = 四项自查 + 三要素表格；默认不跑全量回归（`--only` 挑 2~3，**是单个 `version[/case]` 不吃叠加**），全量只在 tag 收口跑一次；双后端分档 bytes/near/FAIL，禁笼统「一致」。
- 开工闸：待拍板未清不许落码；第一屏报「待拍板 N / 已拍板 N / 冻结面」；拍板后全仓 grep 旧口径同步勘误。主线按 §1b 不建 track / 不 claim。
- git checkout/restore/switch 前必 `status --porcelain`；还原只许显式点名。kStage bump 波及 = args 含 `--hud` 且 exe=current 用例全重拍。
- cases.json 手工追加必须 python json 库 indent=1 + 尾换行 + `open(..., 'w', encoding='utf-8', newline='\n')`（Windows 文本模式翻 CRLF = 字节级工具假 FAIL）；追加后必须跑 regression 冒烟；工具卡带 case 级 `"exe"` 双后端字典；expectLog 锚以实测 log 为准禁手写。**双后端截图基线必须分文件**（DX12 无后缀 / VULKAN 带 `-VULKAN`，run.bat 同名覆盖 = 假 bytes）。
- 行尾根修 v2（2026-11-14）：`.gitattributes` 全局 `* text=auto eol=lf` 放最前 + 现有 CRLF/binary 显式钉在后；「git status M 但 diff 空」= eol 类症状。
- regression.py 静默挂死已修；**长任务一律后台化**（分钟级命令后台跑 + 完成通知回来；禁前台 600s 阻塞与 sleep 轮询）。**秒级验证带 `timeout 60` 前缀**（超时真杀不留僵尸；前台超时自动转后台 ≠ 完成，立即查+杀）。**后台任务 ≠ 安全**：完成时长偏离预期（过快/过慢）都先核再信；后台命令尾部写结果锚 `; echo "$(date '+%F %T') <任务> rc=$rc" >> .workbuddy/bg-last.log`；秒级验证不串进分钟级后台链（冒烟失败信号会被埋到链尾）。
- **长任务三条硬纪律**：① 后台完成通知到达 = 下一动作立即查结果报数；② 运行期间每次工具调用间主动播报进度；③ 禁阻塞等待长任务。

## .nmesh / ECS / job
- 尾段 magic NLOD / NFLB+NSUB / NPAG；**Meshlet::triangleOffset 是字节偏移不是三角号**。ComponentOf 计数器在非模板 inline；jobgraph link 完必须 launch；jobsync 建图期全 link、放行统一 `launchAll`。⚠️ JobRuntime 析构次序：pool 声明在 adapter 之后（pool 先死）。

## 并行轨道（详 PARALLEL-TRACKS §7~8）
- wt junction 建/删只用 PowerShell 原生 API（`New-Item -ItemType Junction` / `[IO.Directory]::Delete($p,$false)`），禁 cmd；共享 `.workbuddy` + slang dist + skip-worktree。CLOSED 轨道 = wt 移除 + 分支保留，重开走 §7.1b。树闸 `tree_gate.py`；开场 `claim` / 收尾 `release` 对称；CLOSED 轨道 claim 须 --force。
