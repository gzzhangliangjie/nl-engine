# nl-engine 项目长期约定

> 协作铁律 / 决策前置 / 里程碑纪律的**权威落点在仓库根 `AGENTS.md`**，本文件只记易忘的精确不变量与当前状态，不重复那边的细则。

## 架构（2026-11-06 定稿，不可动摇）
- 通用管线 NVRHI + Slang，**构建链不依赖任何厂商图形 SDK**（`CMakeLists.txt` 禁 `VULKAN_SDK`/`find_package(Vulkan)`）。
- 后端对称：D3D12 走 Windows SDK；Vulkan 头走 `NVRHI_FETCH_VULKAN_HEADERS`，loader=驱动自带 `vulkan-1.dll`；Slang 独立获取。
- **OpenGL 后端已否决**（vendored NVRHI 无 GL，破坏「一份 .slang 出 SPIR-V/DXIL」单源约定）。
- `NL_GRAPHICS_BACKEND`(AUTO/DX12/VULKAN) 唯一开关；Windows AUTO→DX12。
- 图形 API / NVRHI / ImGui 符号**只在 `renderer/`**；`core/`/`viewer/` 零命中。
- 一份 .slang 双目标(SPIR-V+DXIL)两后端加载成功 = 通用管线验收核心。

## 本机环境（2026-11-06 实测）
- VS2022 + MSVC 14.38 + Win SDK 10.0.26100.0；CMake 3.31.6 便携版 `E:\UE_STUDY_2026\.build-tools\`。
- CMake 必用 `NMake Makefiles`（VS 生成器被 HTTPS_PROXY/https_proxy 大小写撞键搞挂）。

## 当前进度
- 主线 S0~S4.2：M0.1~M0.9(58/58) → M1.0~M1.3(82/82) → M2.0~M2.3a(118/118) → M3.0~M3.2(150/150) → M4.0(s4-indirect) → M4.1 GPU 视锥剔除(s4-cull) → M4.2 cull 计时+1.3M dense 资产+60fps 验收(**176/176 ALL GREEN**，tag s4-done，S4 收官)。
- 当前 `kStage=S5.2`；**M5.2 GPU 三角数 HUD 已验收，S5 全线收官**（tag `s5-done`）：cull.slang u2 / dag.slang u3 原子三角计数器（InterlockedAdd clusterMeta[id*4+1]）+ 回读环形 8B（[0]=簇数 [4]=三角数）延迟两帧 + HUD `s5 tris` 行。验收：锚点逐位（dag 近景 237 簇 → 3936 == finest 总数，双后端）；拉远谱 thr 2..64 → 3870/2360/426/142/104/104 单调；回归 8 组全 bytes。**滞回已砍**（用户拍板：三处参考均无运行时滞回，破坏 M5.1 无状态性且必要性未实测；UE-DIFFS D2 留痕）。M5.2 顺手修两个潜伏 bug：viewer cut thr 滑条回读条件改 `dagTop && dagGroupCount>0`（原 `dagTop||dagMode` 在 dag 遍历模式把命令行 --cut-thr 每帧覆盖回 1.0）；历史 selftest 卡（m0.9/s2.0/s3.2 exe=current）expectLog 15/15 → 16/16。主线下一步 = S6 Hi-Z 两遍遮挡剔除（`docs/s6/S6-PLAN.md`：M6.0 深度+min 金字塔 / M6.1 保守盒测试 / M6.2 开关验收，S5-PLAN 交接「第二遍剔除复用 S4 indirect buffer」）或 P9 真实资产，等用户指令。**勘误（2026-11-09）**：M5.1/M5.2 收口日志曾把 S6 误写成「bevy 全局队列多 pass 升级路径」——那是 UE-DIFFS D3a 的升级路径（触发条件=多实例/千组级负载不均，未触发），canonical 文档（ROADMAP/S6-PLAN/UE-DIFFS D6「遮蔽剔除排在 S6+」）从未改过 S6=Hi-Z；系日志缩写「S6 升级路径」被误抄成同位语所致，已在 2026-11-09 日志勘误。
- 当前 `kStage=S6.0`；**M6.0 Hi-Z 深度预通道 + 手写 min 金字塔已收口**（commit 59d6b72，tag `s6-hiz` 已打，全量回归闸 228/228 ALL GREEN）：hiz.slang 6 入口（vsHizDepth/psHizDepth/csHizDown/csHizTop/vsHizViz/psHizViz）+ Scene `initHiz`（ensureHizTargets 懒建）+ viewer `--mode hiz` + HUD mip 滑条 + 右下角 160x160 层级可视化。验收：金字塔最高层 top min **3.292 双后端逐位一致**（dist 4.2 最近径向距离，机械判据）+ mip0/mip3 灰度剪影近亮远暗肉眼核对 + 全量回归 228/228。深度度量 = 视点径向距离 `distance(worldPos, eye)`（与 M5.0 误差公式同空间）。**两个实测教训（已固化 SHADER-CONVENTIONS §2.7/§2.8）**：① **每管线必须单一 BindingSet**——slangc 把资源全编进 vk set 0，NVRHI Vulkan legacy 模式把多 BindingLayout 拆多 vk set → 第二套 set 里的 b1 绑不上读 NaN（DX12 无 set 概念侥幸不炸，NVRHI validation 不报错）；hiz 预通道因此合并为单 `hizPrepassSet_`（b0+b1+t1..t4）。② **Hi-Z 金字塔尺寸必须 POT**——非 POT 视口的 ceil 对半链超 mipLevels 上限（640 宽上限 10 层要 11 层），DX12 debug layer 报错、Vulkan 无 VVL 静默建坏；取整到 1024x512 后保守性不破。诊断手法：compute 写哨兵（0xDEADBEEF）验证 UAV/copy 链路 + 绝对 LOD 多点采样定位坏层。M6.0 顺手修：R32F RT 补 setClearValue（deviceErrors 9→0）、回读环形重建补 keepInitialState、viewer selftest 产物 18→32。**M6.1 拍板点**：保守盒测试串进 DAG 遍历（bevy 式 HZB second pass 长在队列多 pass 结构上 vs 现有单 dispatch 改造），见 S6-PLAN。主线下一步 = M6.1 → M6.2（tag s6-done）。
- **UE 差异台账（2026-11-08 用户点名固化）**：所有与 UE 不一致处随手记 `docs/s5/UE-DIFFS.md`（D1 投影口径/D2 二值 cut/D3 遍历结构/D3a GPU 遍历=每线程栈 vs bevy 全局队列/D4 简化器/D5 无实例误差旋钮/D6 无遮蔽剔除——M6.0 金字塔已落地、D6 待 M6.2 收口时更新/D7 无软光栅 + D8/D9 待办），AGENTS.md 已挂入口；里程碑收口/review 前过一遍。
- kStage bump 波及面修正：HUD 标题行含 stage 字符串，但**只有 exe=current 的版本需要重拍**（当前 exe=current 版本：m0.9/s2.0/s3.2/s4.1/s4.2/s5.0 共 6 对 12 张，S5.1 已重拍）；m0.1~m0.8/s1.0 走 tag 快照 exe（`build/showcase/{tag}/`，HUD 内嵌旧 stage 字符串），基线不随 kStage 变。**重拍基线前先查该版本 cases.json 的 exe 字段指向快照还是当前构建**（M5.0 曾用当前 exe 重拍 8 对历史版本基线被总闸当场抓住）。
- M4.1 真剔除姿态=进模近断面（dist0.45/640x720：GPU 30/237 == frustum_check.py 离线）；轨道相机「背对」整模仍在视锥内，S4-PLAN 旧前提已修正。
- 并行研究模块：VSM 插 S9 后；TSR 拆 S7 后两段。
- **A 类并行轨道：动作系统已收官并合入 master**（merge `5a1912f`，2026-11-09；worktree 已移除、分支 `feat/action-system` 保留，tag `anim-ma1`~`anim-ma5c`）：nl/anim 纯逻辑层 + anim-tools + viewer skin 模式 + 四资产蒙皮演示页全在 master；GPU 调色板等更大规模蒙皮资产再立项。合并教训：merge-tree 预演不可信（报 0 实撞 11 文件）；跨布局 merge 后 cases.json exe 模板与 cameraSelfTest 断言都要按 master 口径适配。
- P9 真实资产验收四里程碑 V1~V4（V4 流式分页必做，非可选项）。

## 工程纪律（易忘点）
- 每阶段：目标→亲眼可见用例→验收判据→放行；tag 在验收后打。
- **收口自查清单（2026-11-09 M6.0 教训：漏交代物被用户两次点名）**：宣布收口前逐项过——① 渲染交代物（`example/<版本>/` 基线 PNG + run.bat + cases.json 登记 + `server.py --check` 退出 0）② 工具交代物（tool 卡 + TOOLS-GUIDE）③ `MILESTONES.md` 四栏台账 ④ 固化文档（S*-PLAN/SHADER-CONVENTIONS/UE-DIFFS）。四项齐了才许宣布收口，不许等用户点名。
- **总闸（2026-11-08 用户拍板：回归按需跑，非必要不跑）**：默认不跑回归；改了什么就 `regression.py --only <版本>[/用例]` 挑 2~3 条关键用例验。全量（双后端 × 全量历史用例，`--jobs` 并行）**只在打 tag 收口时跑一次**；其余场景（含验证工具自身改动）禁止起全量。并行口径 = 用例间并行、同用例双后端串行（工具用例写共享路径，同用例并行会打架）。
- 双后端一致分档：`bytes`(MD5 逐字节)/`near`(≤1/255 且 ≤2px)/`FAIL`，禁笼统写「一致」。
- 展示台 8787 端口残留先 `netstat -ano | grep 8787` 清。
- 工具 exe：`build/{DX12,VULKAN}/nanite-tools.exe` 是各后端构建树的**直接产物**（build-m0.2.bat 就输出在那），**不要从根目录 `build/nanite-tools.exe` cp**（那是废弃旧树的残留，2026-11-09 S6.0 收口时误 cp 盖掉新产物导致回归假 FAIL）。NMake 同秒 mtime 陷阱会跳过重链：删掉 exe 再编强制重链；验证口诀 `nanite-tools selftest --suite nope` 应打印含 `fallback, jobs` 的完整套件列表。
- `nanite-tools dag` 六项校验全 PASS 才 exit 0；`selftest dag` 128×128 全链全量断言不抽样。
- 每次 `kStage` bump 必须重拍 m0.9 gate-hud / s2.0 lod-hud / s3.2 dag-hud 三对基线；且 HUD 布局演进会波及含该模式的所有 HUD 基线（M4.2 给 cull 模式加两行，s4.1/cull-inside-hud 一对也重拍，共 8 张）——总闸 regression 兜底。
- **example/*/run.bat 里 for 块内引用循环内 set 的变量必须延迟展开 `!VAR!`**（`%VAR%` 解析期为空 → 执行字面 `""` 静默 no-op 假绿；M4.0 旧脚本即此病，M4.1 已修）。
- UE 源码**只读不抄**（EULA）；Carrot/bevy 抄思路不抄代码。

## 资产格式 .nmesh（M2.2~M3.2 定稿）
- v2 = v1 + 尾部 `NMeshMetaHeader(24B,'NLOD')` + `float[3*lockedCount]`；v3 加 `'NFLB'` 自描述尾段（按 magic 识别，v1/v2 兼容）。
- 组记录 32B：error / simplifyError / sphere[4] / genFirst / genCount；组 error=max(组简化误差,成员簇误差) 逐位精确；顶层组 genCount=0。
- **`Meshlet::triangleOffset` 是字节偏移不是三角号**（读三角窗口 `base=triangleOffset+t*3`）。
- 按组重切硬前提：父簇绝不跨组 → 每父簇恰一生成组 → S5 组原子 cut 才成立。单层保留率上界 ~80%(8簇/组整簇产出)。
- fallback = weldPositions 顶层拼接（2.6%≈UE 默认 2%），不做第二次独立简化。
- v4 分页（128KiB 字节预算 + `NPAG` 页表，页分区种子复用 S3 dagGroups）**代码落地推迟到 P9 V1**，但 V1 即写 v4 不破 M3.2 回归基线。

## 渲染 / HUD 约定（改前看 `docs/SHADER-CONVENTIONS.md` / `docs/IMGUI-GUIDE.md`）
- SPIR-V 绑定偏移固定：`slangc -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0`；shader 只写 `register(b0/t0/s0)`，禁 `[[vk::binding]]`。
- 入口名分两阶段（详见 `docs/SHADER-CONVENTIONS.md` §2.4）：**编译期** `-entry` 决定 DXIL blob 内入口名（vsMain/vsCluster/vsCull…），SPIR-V 恒 `main`（无视 `-entry`）；**加载期** NVRHI 运行时 `setEntryName` 仅 Vulkan 用作 SPIR-V `pName`（必须 `main`），DX12 忽略它直接用 blob 唯一入口（传啥都行）。
- MVP 用 volatile CB（`setMaxVersions(16)`）；`SceneConstants` 严格 160B，**禁整块 memcpy**（尾部有 bool cullBack）。
- pipeline 必 `setFrontCounterClockwise(true)`，否则剔掉正脸；`--dist 0.6 --cull none` 看内壁。
- 资源/绘制逻辑收 `renderer/src/scene.cpp`（两后端共享，只依赖 nvrhi.h）；backend 只管 swapchain/depth/framebuffer/present。
- ImGui vendored **v1.91.9**，自写 `renderer/src/hud.cpp`（ImDrawData 摊平 + R32_UINT + 逐 cmd scissor）；`hudProjection()` Y 尺度必须负；面板高度≤560（M4.2 起，510 装不下 cull GPU 行+曲线，小画布仍 min(cap,H-28) 兜底）；HUD 字符串须 ASCII；`renderFrame(...,hud=nullptr)` 不碰 ImGui（截图默认 `--no-hud`）。
- 新增 GPU 资源 slot 级同步表必在 `createTargets()` 初始化（renderFrame 在 ensurePipeline 前按 slot 访问，放错=首帧 139 段错误）。
- **viewer 截图模式（--screenshot）建隐藏窗口**（`WindowDesc.visible=false`，2026-11-08 固化）：可见窗口首显必抢前台焦点（实测 ~0.85 s/用例，--jobs 并行跑闸打断打字）；像素来自 GPU readback 与窗口可见性无关，基线零影响。别把该 hint 拿掉，交互模式仍 visible=true。8 个 tag 快照 exe 已由 `rebuild.py --force` 重编（`apply_hidden_window_patch()` 自动注入等价补丁，s0-init 除外——无截图模式且用例 manual），全量跑闸零抢焦点；快照源码 = tag + 此补丁，非纯 tag 原样。

## 简化器口径（M2.3a 定稿）
- meshopt 多 pass 主循环（src 并入 dst + collapse_locked + cos75°）+ link condition + 双层边界锁（组间边界 + 模型开放边界 LockBorder）+ 绝对朝向锚（判决实验定稿保留）。`check-orient` 扫全链 inward（exit 1=FAIL）。

## 参考 / 调试工具链
- NVRHI 权威参考 `thirdparty/nvrhi/doc/`（`Tutorial.md`+`ProgrammingGuide.md`），每处调用回 vendored `nvrhi.h`(2021 代) 核对签名。
- Slang 示例 `thirdparty/slang-reference/`；NVRHI 完整示例 `thirdparty/nvrhi-reference/donut-samples/vertex_buffer/`；Falcor=Slang 生产级参考。
- 调试：L0 画面证据 → L1 RenderDoc（外部 Launch 零改动，精确抓帧才加 `--capture-frame N`）→ L2 NVRHI timestamp（M0.6 补）→ L3 Nsight/PIX/PresentMon/Tracy。

## 并行轨道纪律（2026-11-08 用户拍板）
- 固定 `feat/*` 分支 + worktree 模式。**新轨道 = 新 `feat/*` 分支 + 新 worktree**；轨道收官 = 内容全量合回 master + worktree 移除、**分支保留**（job-system/action-system/material-graph 三轨均按此先例收口，2026-11-09）。
- **材质图轨道已收官（2026-11-09）**：B2~B4（tag `mg-b2b4`）+ T1 编辑器 M1~M3（tag `mg-t1`，merge `e5505bc`）全量合入 master；调试工具级 `viewer --editor` + 节点库 10 种 + uniform 曝光（MaterialParams b1，运行时绑定留 B5）；定位=可调参表面材质薄层，UI 封顶不再投入，复杂 shader 手写与图无关。唯一遗留=T1 拆解 6 预览视口 + B5 接 PBR（都等 S7，届时按 §7.1b 重建 worktree）。终点定义见 `docs/material-graph/SIDE-TASKS.md`。
- worktree `junction` 共享主树 `.workbuddy` 与 `thirdparty/slang/dist`；`.workbuddy` 已 `skip-worktree`（git status 干净）。**拆 worktree 时 junction 只能删链接**（`.NET Directory.Delete(path,false)` / `cmd rmdir`），MSYS `rm -r` 会穿透删共享内容。
- 纪律三条（详 `docs/PARALLEL-TRACKS.md` §7）：① 实例注册表登记占用说明；② 固化文档闸（merge 回 master 前必写 `S*-PLAN`/`MILESTONES` 验收纪要）；③ 假绿防护（占坑≠文件指向，merge 前双向 `git status` 自查：wt 看得到、master 没有）。
- 工作线粘性：同一轨道回同一 worktree；一条工作线只 merge 一次（验收 FAIL→补丁→复验在占坑内闭环）。
- **wt_pool 占坑闸（2026-11-09 用户拍板，事故防复发）**：任务号=里程碑编号；**每个对话开场声明里程碑编号**（没给就提示补充），补上后 `python tools/wt_pool.py claim --track <轨道> --milestone <编号>` 登记归属；动 wt 前过 `check`/`guard`；CLOSED 轨道硬拒绝；锁=主树根 `.wt_pool_busy.json` 全 wt 共享（已 .gitignore）；跨 wt 用绝对路径调主树脚本；selftest 16 项。详 AGENTS.md 并行轨道纪律第 7 条 + PARALLEL-TRACKS §7.6。

## 非渲染候选拍板（2026-11-08）
- **总原则（用户拍板）：有参考的内容就考虑从头写**——不因有现成开源库而改集成路线；参照实现用于对照与节奏校准。
- **寻路（NavMesh+A*）：从零写**——Carrot `pathfinding/`（2336 行，SparseVoxelGrid→OpenHeightField→DistanceField→Region→Contour→AStar，debugDraw 分步可视化）为从零节奏参照，UE `Runtime/Navmesh`（Recast v1.4.2+Detour）只读；候选 ★★★★，**已立项 `docs/p8/P8-NAV-PLAN.md`（M-N0~M-N5）**。
- **物理：vendored Jolt（MIT）不自研**——原则例外：Carrot physics 本身就是 Jolt 集成层（2902 行）、UE 5.x 亦提供 Jolt 后端、bevy 生态 avian，三处均不自研求解器；集成方式参照 Carrot `physics/`。**已立项 `docs/p8/P8-PHYS-PLAN.md`（M-P0~M-P5）**。想学原理另开 Box2D-Lite mini 求解器支线（不进引擎）。
- **3D 音频**：集成层+解码器从零写、设备混音/3D 定位用 OpenAL Soft（Carrot `audio/` 1358 行同分层：OpenAL 封装+自写 WAV/MP3/Vorbis 解码器+独立 AudioThread）；混响卷积/遮挡音频不在范围。**已立项 `docs/p8/P8-AUDIO-PLAN.md`（M-S0~M-S5）**。
- **开发者控制台**：**已立项 `docs/p8/P8-CONSOLE-PLAN.md`（M-C0~C3），worktree `feat/console` 已建**（2026-11-08 拍板）——两层架构：`core/console/` 纯逻辑（命令/CVar/解析，零 ImGui）+ renderer 侧 ImGui 前端（呼出/补全/日志）；Carrot `console/`（369 行）参照、UE `IConsoleManager.h` 只读；不依赖场景整合，随时可开工。
- **排期拍板（2026-11-08）：三条执行都放到场景整合之后**——依赖 P8 E1（ECS-lite 场景：多物体/变换层级；物理 M-P5 还依赖 E3 序列化）；届时新建 `feat/pathfinding` / `feat/physics` / `feat/audio` worktree（PARALLEL-TRACKS §7.1b）。
- **worktree 收尾即同步关联文档（用户拍板固化进 AGENTS.md 纪律第 6 条 + PARALLEL-TRACKS §7.5）**：track 内完成里程碑后必须同步更新 master 侧 AGENTS.md 注册表 / PARALLEL-TRACKS §7.1 / 相关候选文档，跨轨道文档更新在主 worktree 提交（教训：M-A5.1 落地后 master 侧注册表停在「候选」）。
