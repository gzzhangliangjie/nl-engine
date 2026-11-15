# GLFW — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | GLFW — windowing / GL & Vulkan context / input library |
| 版本 | **3.5.0**（`include/GLFW/glfw3.h` → `GLFW_VERSION_MAJOR 3 / MINOR 5 / REVISION 0`） |
| 上游 | https://github.com/glfw/glfw |
| 获取方式 | 从本机 `E:\UE_STUDY_2026\Carrot\thirdparty\glfw\` 复制（Carrot Init 的 vendored 思路，零网络）；删 submodule `.git` 引用与 `docs/examples/tests`，保留 105 个构建必需文件 |
| 许可证 | **Zlib/libpng 风格宽松许可**（`LICENSE.md` 原文随库入库；Marcus Geelnard 2002-2006 / Camilla Löwy 2012-2019 及贡献者） |
| 形态 | 纯源码静态库（根 CMakeLists `add_subdirectory(thirdparty/glfw EXCLUDE_FROM_ALL)`），nl_core PRIVATE 链接 |
| 用途与消费点 | 窗口创建 / 键鼠手柄输入回调（nl_core window/input 层），viewer 唯一前台入口 |
| 引入里程碑 | S0（master 早期，2026-11-07 前）；台账「S0 准备」节留痕 |

## 风险核查（选型闸逐条）

1. **许可兼容**：Zlib 风格许可，允许商用 / 修改 / 再分发，无 copyleft 传染。
2. **无厂商锁定**：独立开源项目；Windows 下用系统 WGL/DXGI 路径，零 SDK 依赖。
3. **零构建期新依赖**：纯源码，Windows 图形系统库自动链。
4. **已知风险如实记录**：无已知授权诉讼；API 稳定（3.x 系内 ABI 兼容约定）。
5. **可替换性**：nl_core window/input 层薄封装，GLFW 类型不泄漏进业务层。
