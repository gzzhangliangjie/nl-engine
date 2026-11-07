# 穿插项计划：PBR 直射光 + 贴图采样（tag：`pbr-direct`）

> 状态：**计划已预写（2026-11-07），开工时按本页执行，动手前如需调整先改这里**。
> 定位：主线「穿插项」，插在 S1~S6 之间（建议 S2/S3 卡壳换脑子时），**是 S7 的前置**——S7 本来就含 PBR 直光，这里提前做掉，S7 只剩 vis buffer + vertex pulling + 材质解析。
> 硬规则不变：example 归位 + cases.json + `--check` 退出 0；tag 用名字 `pbr-direct`，不占 m0.* 编号。

## 为什么是它（背景）

- S7 的验收判据是「多材质 PBR 着色与直接画 GBuffer 的对照组逐像素一致」——如果 PBR 直光到 S7 才第一次写，S7 要同时背 vis buffer 编码 + vertex pulling + 材质解析 + BRDF 四件事，违背「一个里程碑一件事」。
- M0.8 已把「一个 shader 多个着色模式」的机制建好（`g_options.x` 分支），加 `pbr` 模式是纯增量；Suzanne.glb 内嵌 `_BaseColor.png` + `_MetallicRoughness.png`，贴图数据是现成的，只是没送 GPU。

## 做什么

### 1. glTF 材质进 GPU（`renderer/src/scene.cpp`）

- tinygltf 已解析 `model.materials[0].pbrMetallicRoughness`：`baseColorFactor`（vec4）、`baseColorTexture`、`metallicFactor`、`roughnessFactor`、`metallicRoughnessTexture`。Suzanne 的 factor 全 1，实际值在贴图里。
- 上传两张纹理（staging 路径与 M0.4 相同）：
  - `t0` = baseColor，**`Format::RGBA8_UNORM_SRGB`**（glTF 语义 baseColor 是 sRGB）；
  - `t1` = metallicRoughness，**`Format::RGBA8_UNORM`**（线性数据，G=roughness、B=metallic、R=occlusion 兼容位）；
  - `s0` = 共用一个 Sampler（线性 + mipmap，anisotropy 4~8）。
- 绑定遵守既有约定（`SHADER-CONVENTIONS.md` §2）：SRV 从 0 / Sampler 128 / CB 256 起，shader 只写 `register(t0/t1/s0/b0/b1)`，**禁用 `[[vk::binding]]`**；slangc 的 `-fvk-*-shift` 参数不动。
- glTF 组合规则在 shader 内照抄规范：`baseColor = factor * srgbToLinear(tex)`；`roughness = tex.g`、`metallic = tex.b`（factor 相乘）。

### 2. `model.slang`：shading mode 4 = `pbr`

- 复用 M0.8 的分支机制，`g_options.x = 4` 进 Cook-Torrance 直射光：
  - `D = GGX(NdotH, roughness²)`（Trowbridge-Reitz）；
  - `V = Smith 高度相关近似`（Geometry 项合并进 visibility）；
  - `F = Schlick`（F0 = mix(0.04, baseColor, metallic)；metallic 下 baseColor 不再进 diffuse）；
  - 漫反射 Lambert * (1 - F) * (1 - metallic)。
- 光照：**单方向光**，方向/颜色/强度放**新开的 cbuffer `b1`**（`LightConstants`：dir、color、cameraPos ~48 B）。**不塞进现有 `SceneConstants`**——它严格 160 B 且尾部有非 POD 字段，禁止整块 memcpy 的老规矩不变。
- 环境光：常数 `ambient ≈ 0.03 * baseColor`（或简单半球），不做 IBL——够 S7 用。
- 法线用世界空间法线（已在 VS 输出），直射光不需要切线/TBN。

### 3. 参数与 HUD

- `FrameParams` 加 `int lightSel`/光照方向参数（或先 CLI 固定，HUD 滑条视面板高度决定）。
- `--shading pbr` 合法值入枚举；非法值照旧 exit 1。
- HUD 的 shading Combo 加 `pbr` 项（面板上限 510，加完 960×540 实拍确认不裁底——M0.6/M0.7/M0.8 三连教训）。

### 4. example 归位（老流程）

- `example/pbr-direct/`：Suzanne `--shading pbr` 双后端基线 + `--shading textured` 对照 + `run.bat` + cases.json；`kStage` 改 `pbr-direct`；`server.py --check` 退出 0。

## 验收判据

1. `--shading pbr` 双后端出正确 PBR 画面、`deviceErrors=0`、调试层零输出。
2. **不带新参数行为与 M0.8 完全一致**：Suzanne `--no-hud` 基线 MD5 不变，selftest 全过。
3. baseColor 贴图 sRGB 正确性肉眼可辨：与「误用 UNORM 直读」的对照图放一起（过亮的错误一目了然），正确路径下高光形状随 metallic/roughness 变化合理。
4. `model.slang` 单源出 SPIR-V + DXIL，魔数 selftest 照旧覆盖（产物数量不变）。
5. `example/pbr-direct` 归位 + cases.json + `--check` 退出 0。

## 踩坑预判

- **sRGB 纹理格式**：baseColor 用 UNORM 直读 = 全图过亮，PBR 观感全毁。两后端对 `SRGB` SRV 采样都原生支持（NVRHI 透传），但要在 PNG 数据 → NVRHI 纹理这条路核对不重复 gamma（M0.4 上传的是同一张贴图当显示色，语义变了格式必须跟着变）。
- **glTF ORM 编码**：roughness 在 **G**、metallic 在 **B**，记反了高光会诡异；写 shader 时注释写死出处（glTF 2.0 spec §3.9.3）。
- **新 cbuffer 的绑定**：`b1` 在 SPIR-V 侧 = 256 + 1；确认 NVRHI `BindingLayout` 侧也按 slot 1 对上（M0.5 起只有 b0 的先例，第一次加第二个 CB，validation 报错就查这里）。
- **确定性**：光照方向/颜色全部来自 cbuffer 固定值，无常数传播问题；截图回归可用。
- **HUD 面板高度**：加 combo 项不增行，但若加光照滑条就增行——加完必拍 960×540。

## 不做什么（边界）

- 不做 IBL / split-sum / BRDF LUT（S7 直光够用，间接光进 S9 以后的事）。
- 不做阴影贴图、多光源、点光/聚光。
- 不做 tonemap / bloom / 后处理链（离屏 RT 是 S7+ 的事，穿插项不铺）。
- 不做材质系统 / 多材质 ID 间接表（那是 S7 的正题）。

## 卡住翻哪

- PBR 公式与实现顺序：Filament《Physically Based Rendering in Filament》§4（zauonlok/renderer 的 `renderer/shader/` 也有完整可读的 C89 版 Cook-Torrance，逐段对照）。
- glTF 材质语义：glTF 2.0 spec §3.9（metallicRoughness）+ Khronos glTF Sample Viewer 行为。
- NVRHI 绑定/采样：`thirdparty/nvrhi/doc/ProgrammingGuide.md` 绑定与资源章节（回 vendored 头核对签名）。
