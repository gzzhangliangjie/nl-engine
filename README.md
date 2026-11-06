# nl-engine — 手撸 Nanite + Lumen

> NVRHI + Slang 通用图形管线（DX12 / Vulkan 双后端，零厂商 SDK），参照 Carrot 的架构自己写一份。
> 铁律：**每一步都必须有可运行的源码产出和一条能亲眼看到的验证效果，验收不过不进下一步。**

## 目录

```
nl-engine/
├── README.md          ← 本文件：项目总览
├── docs/
│   ├── PLAN.md        ← 方案：技术决策、框架选型、工程组织
│   ├── ROADMAP.md     ← 步骤：S0~S14 详细路线图 + 本地代码对照表
│   ├── LINKS.md       ← 学习链接：按用途分类，标注什么时候用
│   ├── MILESTONES.md  ← 里程碑台账：每阶段的动作 / 提交版本 / 完成情况 / 踩坑
│   ├── carrot-history.md  ← Carrot 2023 之后 Nanite×GI 实况时间线（S1~S13）
│   ├── s0/
│   │   ├── S0-PLAN.md         ← S0 六阶段 + 阶段门禁（目标/可见用例/验收/放行）
│   │   └── CARROT-BOOTSTRAP.md← Carrot 2020 从零搭建流程，S0 照它走
│   └── kb/            ← 三份离线知识库（HTML，浏览器直接打开）
│       ├── index.html            主知识库（术语字典/踩坑/资源库/里程碑打卡）
│       ├── graphics-handbook.html 图形学基础手册（G0~G8）
│       └── unity-handbook.html   Unity SRP 落地手册（U0~U8）
├── nanite-tools/      ← （S1 起）离线工具：cluster / simplify / dag / stat
├── viewer/            ← （S0 起）运行时：--mode 每步加一个可开关模式
└── assets/            ← .nmesh 资产与测试模型
```

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
