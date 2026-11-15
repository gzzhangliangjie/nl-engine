# stb — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | stb — 单头文件公用库（本目录只收 2 个头） |
| 版本 | **stb_image.h v2.21 / stb_image_write.h v1.15**（各自头文件顶部版本注释） |
| 上游 | https://github.com/nothings/stb |
| 获取方式 | vendored 头文件子集入库（S0：仅 `stb_image.h` + `stb_image_write.h` 两个头 + `LICENSE`） |
| 许可证 | **MIT / Public Domain 双许可**（`LICENSE` 原文随库入库，注记"Extracted verbatim from stb_image.h v2.21"；选 MIT 表述使用） |
| 形态 | 单头库，消费 TU 内 `STB_IMAGE_IMPLEMENTATION` / `STB_IMAGE_WRITE_IMPLEMENTATION` 展开 |
| 用途与消费点 | stb_image_write：png_writer 无头截图链（S0 `--screenshot` 回归）；stb_image：贴图加载（RmlUi `LoadTexture` 直通等） |
| 引入里程碑 | S0（台账 S0 节 117 行） |

## 风险核查（选型闸逐条）

1. **许可兼容**：MIT / PD 双许可任选，零风险面。
2. **无厂商锁定**：个人开源库，无 EULA、零外部依赖。
3. **零构建期新依赖**：单头自包含，无编译目标。
4. **已知风险如实记录**：v2.21 非上游最新（stb 上游演进慢且 API 稳定，锁定快照即可）；stb_image 对畸形文件解析历史上有 CVE 记录——本项目只读自产/可信资产，不解析外部不可信图。
5. **可替换性**：单头直 include，替换即换头重编。
