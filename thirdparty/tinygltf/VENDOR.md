# tinygltf — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | tinygltf — glTF 2.0 加载器（单头 + 内嵌 nlohmann json + stb_image） |
| 版本 | **v2.6.3**（`tiny_gltf.h` changelog 注释行：`v2.6.3 Fix GLB file with empty BIN chunk was not handled. PR#382 and PR#383`） |
| 上游 | https://github.com/syoyo/tinygltf |
| 获取方式 | 本机 Carrot vendored 复制（零网络）：`tiny_gltf.h` / `json.hpp` / `stb_image.h` / `LICENSE` 四文件 |
| 许可证 | **MIT**（`LICENSE` 原文随库入库） |
| 形态 | 单头库，只在 `renderer/src/scene.cpp` 一个 TU 展开（TINYGLTF_IMPLEMENTATION），避免符号重复 |
| 用途与消费点 | glTF 场景/网格/动画加载：scene.cpp（主消费）+ `nl/anim/gltf_anim`（rigged glTF→Skeleton+AnimClip，声明侧第二消费 TU）；组件 serialise 复用其 json.hpp（nl_core PUBLIC include） |
| 引入里程碑 | M0.4（场景加载；台账 171 行）+ M-A5 第二消费点 |

## 风险核查（选型闸逐条）

1. **许可兼容**：MIT 原文在库内，无风险面。
2. **无厂商锁定**：个人开源库，零外部依赖、不联网。
3. **零构建期新依赖**：三文件自包含。
4. **已知风险如实记录（有台账实证）**：① vendored 版字段名与新版不同——新版 `model.scene`，本版 `defaultScene`（M0.4 踩坑①，注释已标明版本差异）；② 上游用 fastgltf 的替代方案评估过、因「tinygltf 本机已有、零新依赖」未采用（M0.4 偏离留痕 10）；③ 内嵌 stb_image 与 thirdparty/stb 双份——语义域不同（tinygltf 内嵌版仅服务 glTF 解码），不合并避免跨域耦合。
5. **可替换性**：加载层被 scene.cpp / gltf_anim 薄封装消费；换库（如 fastgltf / cgltf）改封装 TU 即可。
