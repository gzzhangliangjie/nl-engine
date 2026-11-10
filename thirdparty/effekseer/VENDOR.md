# Effekseer — VENDOR.md（选型闸留档）

## 基本信息

| 项 | 值 |
|---|---|
| 上游 | https://github.com/effekseer/Effekseer |
| 入库版本 | main 分支 commit `b570008`（"Add Visual Studio 2026 DXLib runtime builds"，2026-11-10 vendored；二进制格式支持至 `Version18 = 1810`，老版本由 loader 内置迁移） |
| 许可证 | MIT（随库 `LICENSE` 原文入库；上游另有 LICENSE_TOOL，属编辑器工具侧，未入库故不涉及） |
| 入库子集 | `Dev/Cpp/Effekseer/`（core 运行库全部，57 cpp / 121 h）+ 根 `LICENSE` |
| 排除项 | `Effekseer/Network/`（winsock2 + flatbuffers 依赖，离线转换器用不上；core 无反向依赖，已 git grep 实核）；上游 CMakeLists.txt（引用父级宏 filterfolder/warnerror，无法 standalone，改为本目录自有 CMakeLists 显式列源，逐条对应上游 effekseer_src 去 Network） |
| 用途 | M-PT1 `vfx-tools/nfx-convert`：解析 `.efkefc`（SKFE chunk 格式）→ 遍历 EffectNode 树参数 → 生成 `.nfx` v1。只使用 parse 侧 API（Effect/EfkEfcFactory），sim 侧（Manager/Instance）编译进来但不调用 |
| C++ 标准 | C++17 兼容（MSVC 14.38 实测通过为收口判据） |
| 风险核查 | MIT 无显式专利授权条款——如实写明，不宣称「绝对零风险」；项目 MIT 兼容、允许商用/修改/再分发；无已知专利/授权诉讼风险；Effekseer 是活跃维护的成熟库（关联系统用成熟库为通例，AGENTS.md 选型闸 2026-11-09 口径） |

## 边界

- 第三方库只解决 `.efkefc` 解析这一关联问题，**不替代渲染主线**：`.nfx` 是本项目自定义资产格式，运行时加载端（nl-engine 侧）零 Effekseer 依赖——转换器把 Effekseer 参数面折算进 `.nfx` 子集，子集外模块（模型/ribbon/track/材质文件/音效/GPU 粒子/力场/程序化模型等）在转换期 exit 2 显式报错。
- 升级时同步更新本文件（版本/commit/子集/排除项）。

## 测试素材（同仓库分发）

上游 `Examples/Resources/` 随库含 20 个 `.efkefc`（含贴图）：sprite 类做正向用例，`GpuParticles_*` / 模型类做 exit 2 负向用例；官方声明示例免费、无使用限制。
