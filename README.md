# nl-engine — 手撸 Nanite + Lumen

> NVRHI + Slang 通用图形管线（DX12 / Vulkan 双后端，零厂商 SDK），参照 Carrot 的架构自己写一份。
> 铁律：**每一步都必须有可运行的源码产出和一条能亲眼看到的验证效果，验收不过不进下一步。**

## 目录

```
nl-engine/
├── README.md          ← 本文件：项目总览
├── CMakeLists.txt     ← 标准 CMake 构建链；NL_GRAPHICS_BACKEND=AUTO|DX12|VULKAN
├── core/              ← 日志 / 断言 / 窗口 / 轨道相机 / --mode 分发（零图形 API 符号）
├── renderer/          ← 唯一允许出现 NVRHI / 图形 API / ImGui 的模块；shaders/ 放 .slang
│   ├── src/scene.cpp  ← 两后端共享：glTF 加载 / staging 上传 / 绑定集 / pipeline
│   └── src/hud.cpp    ← 两后端共享：自写 NVRHI ImGui 渲染后端（不用官方 D3D12/VK backend）
├── viewer/            ← 可见用例 + --screenshot 回归 + HudFrame 双向 POD
├── assets/models/     ← Suzanne.glb（Khronos 官方样例）+ TorusKnotDense.glb / IcoSphereDense.glb（程序化致密测试资产，gen-dense-mesh.py 生成）
├── thirdparty/        ← glfw / nvrhi / imgui(1.91.9) / vulkan-headers / directx-headers / stb / tinygltf / meshoptimizer(v1.3 文档级对照，不入构建)（本地 vendored）
├── docs/
│   ├── PLAN.md        ← 方案：技术决策、框架选型、工程组织
│   ├── ROADMAP.md     ← 步骤：S0~S14 详细路线图 + 本地代码对照表 + 调试工具分层
│   ├── LINKS.md       ← 学习链接：按用途分类，标注什么时候用
│   ├── MILESTONES.md  ← 里程碑台账：每阶段的动作 / 提交版本 / 完成情况 / 踩坑
│   ├── SHADER-CONVENTIONS.md ← Slang 双目标编译 + 入口名 / 绑定偏移规范（含 hud.slang）
│   ├── IMGUI-GUIDE.md ← HUD 使用说明：怎么加控件 / 下拉框 / 输入边界 / 渲染约定 / 截图复现
│   ├── blogs/         ← 大佬博客研读笔记（博客原文 × 源码实况 × 我们的对照）
│   │   ├── jms55-bevy-meshlet.md          JMS55《Virtual Geometry in Bevy 0.14/0.15/0.16》× bevy 源码（S1~S5 配套）
│   │   ├── jglrxavpok-carrot-nanite.md    《Recreating Nanite》全系列 × Carrot 源码（S1~S12 配套）
│   │   └── index.html                     浏览器直接打开的可视化阅读页（重生成：python tools/blogs/build.py）
│   ├── carrot-history.md  ← Carrot 2023 之后 Nanite×GI 实况时间线（S1~S13）
│   ├── s0/
│   │   ├── S0-PLAN.md         ← S0 六阶段 + 阶段门禁 + 可观测性/调试工具计划
│   │   └── CARROT-BOOTSTRAP.md← Carrot 2020 从零搭建流程，S0 照它走
│   └── kb/            ← 三份离线知识库（HTML，浏览器直接打开）
│       ├── index.html            主知识库（术语字典/踩坑/资源库/里程碑打卡）
│       ├── graphics-handbook.html 图形学基础手册（G0~G8）
│       └── unity-handbook.html   Unity SRP 落地手册（U0~U8）
├── example/           ← 每个里程碑的成功用例：基线 PNG + 一键复现 run.bat + cases.json 清单
├── tools/showcase/    ← 成果展示台：按版本加载 example/ 用例、现场跑 viewer 看效果、双后端对比
├── nanite-tools/      ← （S1 起）离线工具：cluster / simplify / dag / stat
└── scripts/           ← 一键 configure+build+selftest、模型打包脚本
```

## 当前进度与运行（S0 完成 + M0.6~M0.9 能力补齐，下一步进 S1）

**2026-11-07：S0 收口（tag `s0-done`）后进入能力补齐版本序列，M0.6（GPU 计时）、M0.7（高模测试资产 + --model）、M0.8（线框 / 调试着色）、M0.9（一键全量回归总闸）已完成——0.x 必做序列到此冻结，下一步进 S1 切簇。** 零厂商图形 SDK：D3D12 头/库/调试层来自 Windows SDK，Vulkan 头走本地 vendored `Vulkan-Headers`、loader 用驱动自带 `vulkan-1.dll`，Slang 独立获取。构建只需 VS2022 + Windows SDK + 便携 CMake。

```bash
scripts/build-m0.2.bat DX12          # 或 VULKAN：configure + build + selftest（15/15）
build/<backend>/viewer.exe            # 交互窗口（默认开 HUD）：拖动旋转、滚轮缩放、C 切剔除、V 切线框、N 循环着色、R 重置、F12 存图、ESC 退出
build/<backend>/viewer.exe --mode normal                 # 模式分发器；未知模式退出 1 并列出可用名
build/<backend>/viewer.exe --screenshot out.png --frames 8   # 截图回归默认 --no-hud，保持 M0.1~M0.4 基线不变
build/<backend>/viewer.exe --screenshot hud.png --frames 8 --hud --size 960x540
build/<backend>/viewer.exe --screenshot dense.png --hud --dist 5.5 --model TorusKnotDense.glb   # 20 万 tri 纽结
build/<backend>/viewer.exe --screenshot n.png --shading normal --model TorusKnotDense.glb   # 法线着色看朝向
build/<backend>/viewer.exe --screenshot w.png --wireframe                  # 线框（光栅 fill mode）
build/<backend>/viewer.exe --screenshot t.png --shading tri-id             # 三角形ID 伪随机色
build/<backend>/viewer.exe --screenshot inner.png --dist 0.6 --cull none   # 关剔除看内壁
build/<backend>/viewer.exe --selftest
python tools/regression.py            # M0.9 总闸：全部 example 用例 × 双后端现场重跑并比对基线，全绿 exit 0（--jobs 4 并行提速；日常开发用 --only <版本> 只验当前阶段）
python scripts/gen-dense-mesh.py --shape torusknot --out assets/models/TorusKnotDense.glb   # 程序化致密资产（零网络、MD5 确定）
```

- **交互窗口**：Suzanne 上叠 ImGui HUD——FPS / 帧时间曲线 / yaw·pitch·dist / 后端名 + adapter + 调试层 / mode / stage，控件可直接改背面剔除、清屏色、重置相机（双向 POD 每帧回写）。控件怎么加、下拉框 / 选择菜单能不能用，见 `docs/IMGUI-GUIDE.md`。
- **截图可复现**：实时 FPS 写进 PNG 会让基线 MD5 永远对不上，所以 `--screenshot` 的 HUD 时序数据用固定样本并标注 `capture/replay (fixed samples)`；交互窗口显示真实采样。
- **S0 总验收**：`model.slang` 与 `hud.slang` 各自同出 SPIR-V + DXIL（`--selftest` 机械核对 8 个产物魔数）；两后端 `deviceErrors=0`、selftest 14/14、ctest 100%；`--no-hud` 正常视角双后端 PNG 逐字节相同（MD5 `7e528087…`），`--hud` 两张必然不同（面板印的后端名 / 调试层不同，不是 3D 画面不一致）。证据：`docs/screenshots/s0.png`、`m0.5-*.png`、`example/m0.5/`、`docs/MILESTONES.md`。
- **M0.6 GPU 帧内计时（tag `m0.6`）**：两后端共享的 `GpuTimer` 用一对 NVRHI timer query 包住整帧 GPU 命令，HUD 在 CPU `frame ms` 旁新增 `GPU x.xx ms (NVRHI timer)` 行 + gpu ms 曲线——两者并排一眼分清 CPU 帧间隔与 GPU 耗时。`--no-hud` 完全不建 query，M0.1~M0.5 基线逐字节不变。计划见 `docs/s0/M0.6-PLAN.md`。
- **M0.7 高模测试资产（tag `m0.7`）**：`scripts/gen-dense-mesh.py` 纯 stdlib 程序化生成致密网格 GLB（环面纽结 204,800 tri / 细分球 327,680 tri，零网络、同参数 MD5 确定、内嵌棋盘格 PNG）；viewer 新增 `--model <name.glb>`（assets/ 下白名单），HUD 增 `model : <名> (N tri)` 行；`thirdparty/meshoptimizer` v1.3 作 S1 切簇对照基准（文档级，不入构建）。S1 切簇的命门就位。计划见 `docs/s0/M0.7-PLAN.md`。
- **M0.8 线框 / 调试着色（tag `m0.8`）**：`model.slang` 单源新增 4 种着色模式（`g_options.x` 分支：textured / normal / uv / tri-id，tri-id 用 `SV_PrimitiveID` 整数 hash，slangc 双目标实测通过）；线框走 NVRHI 光栅 fill mode（pipeline 2×2 组合，零几何 shader）；`--shading` / `--wireframe` / V、N 键；HUD 增 shading 下拉框 + wireframe 复选框（双向 POD 通道）。不带新参数时行为与 M0.7 逐字节一致。S1 排查切簇 / 剔除的双眼睛就位。计划见 `docs/s0/M0.8-PLAN.md`。
- **M0.9 一键全量回归总闸（tag `m0.9`）**：`python tools/regression.py` 把 `example/cases.json` 里全部版本的全部可自动用例 × 双后端现场重跑（历史 exe 走 tag 快照，当前版走 build/），PNG 与基线逐字节比对（near 档：像素差 ≤1/255 且 ≤2 像素的 GPU 取整噪声）、selftest 校验日志、`deviceErrors=0`，全绿 exit 0，报告落 `build/regression/report.md`。复用展示台的 exe 解析 / 参数校验 / 像素比对，单一语义源。S1 起每次改动的提交前总闸。计划见 `docs/s0/M0.9-PLAN.md`。
- **调试工具**：帧结构调试主用 RenderDoc（L1，进程外零改动），GPU 帧内计时走 NVRHI timestamp query（L2 整帧部分已由 M0.6 补掉，按 pass 拆分 `--profile` 为穿插项视需要插），Nsight / PIX / PresentMon / Tracy 做时间线佐证（L3）。分层计划见 `docs/s0/S0-PLAN.md`。

## 成果展示台（按版本看用例）

`example/` 是**用例的唯一数据源**：每个版本一个目录，放该版本验收成功的基线 PNG、一键复现 `run.bat` 和 `cases.json` 元数据。展示台读取它，把各版本用例按时间线陈列，并能现场调用对应 tag 的 `viewer.exe` 重新渲染，直接看到效果。

```bash
tools\showcase\start.bat                      # Windows 一键启动（自动开浏览器）
python tools/showcase/server.py --port 8787   # 或手动起服务，访问 http://127.0.0.1:8787/
python tools/showcase/rebuild.py              # 干净 clone 后重建历史 tag 的 viewer.exe（build/ 不入库）
python tools/showcase/server.py --check       # 放行门禁：本轮用例是否已归位 example/（退出码 0 才放行）
```

- 页面可切换 M0.1~M0.7、切换 DX12 / Vulkan / 双后端并排；用例卡显示基线截图、验收判据、复现命令，点「运行看效果」由**该版本自己的 exe** 现场出图（输出到 `build/showcase/runs/`，不覆盖基线；历史版本 exe 按 tag 快照从 `build/showcase/<tag>/` 解析，缺失时先跑 `tools/showcase/rebuild.py`）。「运行看效果」是 `--screenshot` 截图回归，窗口闪一下即退是设计使然；要看真实可交互窗口，点侧栏「开交互窗口」（不截图、不比对基线，窗口保持到手动关闭）。
- M0.4 可调 yaw / pitch / dist / cull 实时换视角；M0.5 有 `hud`（可调 dist）、`controls`（HUD 控件值 → 绿背景 / Suzanne 正脸的静态对照）、`nohud` / `resize` / `selftest`；M0.6 有 `hud`（GPU ms 行 + 双曲线）/ `nohud`（零 query parity）/ `selftest`；M0.7 有 `hud`（TorusKnotDense 204800 tri + model 行，可调 dist）/ `icosphere`（327680 tri 换形状）/ `selftest`。HUD 控件点选是交互用例（标 `manual`，网页不自动跑）。
- 双后端结果除 MD5 外还做像素级比对：正常视角逐字节一致，内壁/resize 只差 1~2 个像素（≤1/255，GPU 取整噪声），页面如实标注而不是笼统写「一致」。M0.5 的 `--hud` 两图预期不同，清单 note 里写明原因。
- 历史 exe 由 `git archive <tag>` 现场重建，Slang 工具链用 NTFS junction 复用主工作区，不重复下载/复制。
- 每个 `example/<版本>/run.bat` 也能单独跑：`run.bat all` 或 `run.bat <用例名>`，两后端各出一张 PNG。

## 两个程序

| 程序 | 职责 |
|---|---|
| `nanite-tools` | 命令行离线工具：切簇 → QEM 简化 → DAG 构建 → `.nmesh` 资产（带版本号） |
| `viewer` | 运行时渲染器：`--mode` 分发器已定型，M0.5 只有 `normal`；S1 起每完成一步加一个模式（`cluster / cull / dag / hiz / visbuf / …`），只改 `core/modes.*` + `runMode()` case，旧模式永远保留当对照 |

## 纪律

1. 图形调用全部收敛进独立渲染器模块，不散落到业务代码——将来换底座（NVRHI/Diligent）只动这一个模块
2. 每完成一步：git tag + 存一张验收截图 + **把本轮成功用例归位到 `example/`**
3. 卡壳超过三天：翻 `docs/ROADMAP.md` 对照总表里标注的参考源码，不要闭门造车
4. **每轮迭代的放行门禁（2026-11-07 定）**：本轮产出的成功用例必须落进 `example/<版本>/`（基线 PNG + `run.bat`）并登记到 `example/cases.json`；验收前跑 `python tools/showcase/server.py --check`，**退出码非 0 不放行、不打 tag**。检查以 `renderer/src/renderer.cpp` 的 `kStage` 为当前阶段，代码进了新阶段而 `example/` 没跟上就报错；展示台侧栏也会同步显示归位状态。S0 的 M0.x 与 S1~S14 每一步同样适用

## 参考仓库（本机）

- `E:\UE_STUDY_2026\Carrot\` — 唯一 Nanite+Lumen 双撸完成的项目（C++/Vulkan + slang），**带完整 1180 提交历史**：既看它 2020 年怎么从零搭起来（`docs/s0/CARROT-BOOTSTRAP.md`），也看它后期怎么做 Nanite×GI（`docs/carrot-history.md`）。`thirdparty/` 里 glfw / glm / imgui / tinygltf / stb 源码已 checkout，可直接复用，不用联网。
- `E:\UE_STUDY_2026\bevy\` — meshlet（虚拟几何）+ solari（ReSTIR GI）两套单项最佳实现（Rust/wgpu）

详细文档入口：`docs/PLAN.md`（方案）→ `docs/ROADMAP.md`（步骤）→ `docs/s0/S0-PLAN.md`（当前门禁 + 调试工具计划）→ `docs/MILESTONES.md`（台账）→ `docs/SHADER-CONVENTIONS.md`（shader）→ `docs/IMGUI-GUIDE.md`（HUD）→ `docs/DEV-PITFALLS.md`（构建/编译/运行踩坑汇总）→ `docs/LINKS.md`（链接）。
**开工纪律**：每个阶段先定目标和「能亲眼看到的用例」，验收通过才进下一阶段，tag 在验收后打；本轮用例必须归位 `example/`（`--check` 退出 0）才放行。
