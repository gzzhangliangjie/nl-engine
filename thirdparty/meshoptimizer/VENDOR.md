# meshoptimizer — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | meshoptimizer — mesh 处理算法库（简化/压缩/优化顶点缓存等） |
| 版本 | **v1.3**（`src/meshoptimizer.h` → `MESHOPTIMIZER_VERSION 1030 /* 1.3 */`） |
| 上游 | https://github.com/zeux/meshoptimizer |
| 获取方式 | vendored 子集入库（M0.7 起；README 标注「文档级对照」→ M2.1 起作 nanite-tools 的简化对照 oracle 编入） |
| 许可证 | **MIT**（`LICENSE.md` 原文随库入库） |
| 形态 | 纯源码静态库（根 CMakeLists `add_subdirectory(thirdparty/meshoptimizer EXCLUDE_FROM_ALL)`） |
| 用途与消费点 | **仅 nanite-tools 的简化对照 oracle**——core/simplify 的 QEM 是自研正式实现，不依赖 meshopt；meshopt 只用于离线对拍验证 QEM 质量 |
| 引入里程碑 | M0.7（文档级对照，台账 5 行）→ M2.1（oracle 编入，根 CMakeLists 注释） |

## 风险核查（选型闸逐条）

1. **许可兼容**：MIT 原文在库内，无风险面。
2. **无厂商锁定**：独立开源库，零 EULA、不联网。
3. **零构建期新依赖**：纯源码静态库，EXCLUDE_FROM_ALL 按需链。
4. **已知风险如实记录**：仅作 oracle 对照不进运行时路径——运行时零依赖承诺不破；升级对照面小（oracle 输出只进离线对拍证据）。
5. **可替换性**：oracle 是对照基准不是依赖——替换简化算法库只需重拍对拍证据，正式实现（自研 QEM）零改动。
