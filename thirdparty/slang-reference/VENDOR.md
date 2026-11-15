# slang-reference — 参考样例溯源

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。**本目录是只读参考样例，不进构建链、不链入任何目标。**

| 项 | 值 |
|---|---|
| 内容 | Slang 上游参考样例：`triangle/`、`hello-world/`（含 `hello-world/vulkan-api.cpp` 作 Vulkan 对接参考）、`example-base/` |
| 上游 | https://github.com/shader-slang/slang（samples 部分） |
| 获取方式 | vendored 入库（S0；台账 147 行） |
| 许可证 | **Apache-2.0 WITH LLVM-exception**（`LICENSE` 原文随目录入库，同 slang 上游） |
| 形态 | 只读参考源码——**不 add_subdirectory、不产目标、不链接**；供 renderer 对接期定向实读 |
| 消费方式 | 文档级实读（S0 期 Vulkan 对接参考）；无运行时/编译期消费 |
| 引入里程碑 | S0 |

## 风险核查

1. 只读参考不进构建 → 零许可义务触发（不产生分发物）。
2. 与 `thirdparty/slang`（编译器快照）语义域分离：本目录是**人读样例**，不是可链接库。
3. 升级 slang 时本目录无需同步（参考性质，不代表锁定版本行为）。
