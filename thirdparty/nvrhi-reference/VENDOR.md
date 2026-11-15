# nvrhi-reference — 参考样例溯源

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。**本目录是只读参考样例，不进构建链、不链入任何目标。**

| 项 | 值 |
|---|---|
| 内容 | NVRHI 上游 donut-samples 子集：`donut-samples/vertex_buffer/`（等） |
| 上游 | https://github.com/NVIDIA-SDK/NVRHI（donut-samples 部分） |
| 获取方式 | vendored 入库（S0；台账 147 行——确认 vendored 头为 2021 老接口时的对接参考） |
| 许可证 | **MIT**（`LICENSE` 原文随目录入库，注记"Extracted from thirdparty/nvrhi/LICENSE.txt, same NVIDIA NVRHI/donut upstream"；同上游 MIT） |
| 形态 | 只读参考源码——**不 add_subdirectory、不产目标、不链接**；供 renderer 对接期定向实读 |
| 消费方式 | 文档级实读（S0 期 vertex buffer 对接参考）；无运行时/编译期消费 |
| 引入里程碑 | S0 |

## 风险核查

1. 只读参考不进构建 → 零许可义务触发（不产生分发物）。
2. 与 `thirdparty/nvrhi`（编译库）语义域分离：本目录是**人读样例**，不是可链接库。
3. 升级 NVRHI 时本目录无需同步（参考性质）。
