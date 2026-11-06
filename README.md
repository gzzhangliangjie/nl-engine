# nl-engine — 手撸 Nanite + Lumen

> NVRHI + Slang 通用图形管线（DX12 / Vulkan 双后端，零厂商 SDK），参照 Carrot 的架构自己写一份。
> 铁律：**每一步都必须有可运行的源码产出和一条能亲眼看到的验证效果，验收不过不进下一步。**

## 目录

```
nl-engine/
├── README.md          ← 本文件：项目总览
├── CMakeLists.txt     ← 标准 CMake 构建链；NL_GRAPHICS_BACKEND=AUTO|DX12|VULKAN
├── core/              ← 日志 / 断言 / 窗口 / 轨道相机（零图形 API 符号）
├── renderer/          ← 唯一允许出现 NVRHI / 图形 API 的模块；shaders/ 放 .slang
│   └── src/scene.cpp  ← 两后端共享：glTF 加载 / staging 上传 / 绑定集 / pipeline
├── viewer/            ← 可见用例 + --screenshot 回归
├── assets/models/     ← Suzanne.glb（Khronos 官方 glTF 样例，单文件内嵌贴图）
├── thirdparty/        ← glfw / nvrhi / vulkan-headers / directx-headers / stb / tinygltf（本地 vendored）
├── docs/
│   ├── PLAN.md        ← 方案：技术决策、框架选型、工程组织
│   ├── ROADMAP.md     ← 步骤：S0~S14 详细路线图 + 本地代码对照表
│   ├── LINKS.md       ← 学习链接：按用途分类，标注什么时候用
│   ├── MILESTONES.md  ← 里程碑台账：每阶段的动作 / 提交版本 / 完成情况 / 踩坑
│   ├── SHADER-CONVENTIONS.md ← Slang 双目标编译 + 入口名 / 绑定偏移规范
│   ├── carrot-history.md  ← Carrot 2023 之后 Nanite×GI 实况时间线（S1~S13）
│   ├── s0/
│   │   ├── S0-PLAN.md         ← S0 六阶段 + 阶段门禁（目标/可见用例/验收/放行）
│   │   └── CARROT-BOOTSTRAP.md← Carrot 2020 从零搭建流程，S0 照它走
│   └── kb/            ← 三份离线知识库（HTML，浏览器直接打开）
│       ├── index.html            主知识库（术语字典/踩坑/资源库/里程碑打卡）
│       ├── graphics-handbook.html 图形学基础手册（G0~G8）
│       └── unity-handbook.html   Unity SRP 落地手册（U0~U8）
├── nanite-tools/      ← （S1 起）离线工具：cluster / simplify / dag / stat
└── scripts/           ← 一键 configure+build+selftest、模型打包脚本
```

## 当前进度与运行（S0 / M0.4）

零厂商图形 SDK：D3D12 头/库/调试层来自 Windows SDK，Vulkan 头走本地 vendored `Vulkan-Headers`、loader 用驱动自带 `vulkan-1.dll`，Slang 独立获取。构建只需 VS2022 + Windows SDK + 便携 CMake。

```bash
scripts/build-m0.2.bat DX12          # 或 VULKAN：configure + build + selftest
build/<backend>/viewer.exe            # 窗口：左键拖动旋转、滚轮缩放、C 切剔除、R 重置、F12 存图、ESC 退出
build/<backend>/viewer.exe --screenshot out.png --frames 8
build/<backend>/viewer.exe --screenshot inner.png --dist 0.6 --cull none   # 关剔除看内壁
```

M0.4 验收：一份 `model.slang` 同出 SPIR-V 与 DXIL，DX12/Vulkan 各渲染带贴图 Suzanne，正常视角 PNG 逐字节相同、`deviceErrors=0`，resize 640×360 → 1024×768 前后均非黑。证据见 `docs/screenshots/m0.4-*.png` 与 `docs/MILESTONES.md`。

## 两个程序

| 程序 | 职责 |
|---|---|
| `nanite-tools` | 命令行离线工具：切簇 → QEM 简化 → DAG 构建 → `.nmesh` 资产（带版本号） |
| `viewer` | 运行时渲染器：每完成一步加一个模式 `--mode cluster / cull / dag / hiz / visbuf / rt / shadow / probe / cache / filter / final`，旧模式永远保留当对照 |

## 纪律

1. 图形调用全部收敛进独立渲染器模块，不散落到业务代码——将来换底座（NVRHI/Diligent）只动这一个模块
2. 每完成一步：git tag + 存一张验收截图
3. 卡壳超过三天：翻 `docs/ROADMAP.md` 对照总表里标注的参考源码，不要闭门造车

## 参考仓库（本机）

- `E:\UE_STUDY_2026\Carrot\` — 唯一 Nanite+Lumen 双撸完成的项目（C++/Vulkan + slang），**带完整 1180 提交历史**：既看它 2020 年怎么从零搭起来（`docs/s0/CARROT-BOOTSTRAP.md`），也看它后期怎么做 Nanite×GI（`docs/carrot-history.md`）。`thirdparty/` 里 glfw / glm / imgui / tinygltf / stb 源码已 checkout，可直接复用，不用联网。
- `E:\UE_STUDY_2026\bevy\` — meshlet（虚拟几何）+ solari（ReSTIR GI）两套单项最佳实现（Rust/wgpu）

详细文档入口：`docs/PLAN.md`（方案）→ `docs/ROADMAP.md`（步骤）→ `docs/LINKS.md`（链接）。
**开工纪律**：每个阶段先定目标和「能亲眼看到的用例」，验收通过才进下一阶段，tag 在验收后打。
