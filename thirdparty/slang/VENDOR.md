# Slang — vendored 发行快照溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | Slang — Shading Language and Compiler（shader 编译器工具链 + 运行时库） |
| 版本 | **2026.21**（`dist/bin/slangc.exe -v` 实测输出；上游按年.构建号发版） |
| 上游 | https://github.com/shader-slang/slang |
| 获取方式 | **发行快照**（`dist/` 布局：bin/include/lib/cmake/share），不入上游源码树；根 CMakeLists `NL_SLANG_BIN=${THIRDPARTY}/slang/dist/bin`、`SLANGC` 硬依赖（缺失 FATAL_ERROR，提示跑下载步骤） |
| 许可证 | **Apache-2.0 WITH LLVM-exception**（`LICENSE` 原文随库入库；**实锤口径**——M-PKG0 踩坑⑥纠偏：不是 MIT，许可证必须拉原文不许凭记忆写） |
| 形态 | 发行二进制 + 头（slangc.exe / gfx.dll / slang-compiler.dll 等 + include）；worktree 间经 junction 共享主树 `thirdparty/slang/dist` |
| 用途与消费点 | shader 编译：根 CMakeLists 四路 slangc（vs/ps × DX12/Vulkan）产物链；材质图 emitter 验收（B3）；运行时库未链入（纯进程调用 slangc） |
| 引入里程碑 | S0（shader 编译链）+ T1 B3（材质图验收）；M-SC0 起多 worktree junction 共享 |

## 风险核查（选型闸逐条）

1. **许可兼容**：Apache-2.0 WITH LLVM-exception——与本项目兼容；LLVM-exception 例外条款放宽链接边界（编译器类库惯例），原文件已在库内。
2. **无厂商锁定**：独立开源项目；slangc 是独立 CLI，不读任何厂商 SDK 路径。
3. **零构建期新依赖**：发行快照自包含（DLL 随 dist 布局）；**不联网拉取**，快照入库即锁定。
4. **已知风险如实记录**：① 二进制快照非源码构建——升级 = 换 dist 快照 + 全量重编 shader 链；② `-fvk-u-shift 384 0` 等 SPIR-V 旗标与 slang/glslang 版本行为绑定，升级需重验；③ dist 体积大（不入 zip 包的裁剪口径见 tools/package.py）。
5. **可替换性**：编译链经根 CMakeLists SLANGC 单点收口，业务侧只见编译产物；换编译器（如 dxc 直用）改编译链不改 shader 源。
