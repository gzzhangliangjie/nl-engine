# Shader 编写规范 + Slang 导出流程

> 适用范围：M0.3 起所有图形 shader。`renderer/shaders/` 下的 `.slang` 源码与 `CMakeLists.txt` 的 Slang 编译块。
> 关联：`docs/s0/S0-PLAN.md` M0.3 / M0.4 / M0.5 目标 · `docs/MILESTONES.md` M0.3 / M0.4 / M0.5 台账。
> 纪律：零厂商 SDK。Slang 独立获取（vendored 到 `thirdparty/slang/dist/`，已 gitignore，不入仓库），DXIL 所需的 `dxcompiler.dll` 来自已装的 Windows SDK，**不额外下载任何东西**。

---

## 1. 源码编写规范

### 1.1 单一真相源（single source of truth）
- 一个渲染效果 = **一份** `.slang`，放在 `renderer/shaders/`。
- 同一份源码由 `slangc` 编译出 **SPIR-V（Vulkan）** 与 **DXIL（DX12）** 两个目标，两后端各自加载——这是通用管线的验收核心，不是「顺带试一次」。
- 语法用 **HLSL 兼容写法**，不要写 GLSL / SPIR-V 专用注解（`[[spirv::...]]` 之类）。保持后端无关。
- 命名：`<effect>.slang`（当前：`triangle.slang` = M0.3 回归、`model.slang` = M0.4 带贴图模型、`hud.slang` = M0.5 ImGui HUD 叠加）。三份都同出 SPIR-V + DXIL。

### 1.2 入口点与语义约定
| 用途 | 入口函数名 | NVRHI ShaderType | 关键输出语义 |
|---|---|---|---|
| 顶点 | `vsMain` | `ShaderType::Vertex` | `float4 position : SV_Position` |
| 片元 | `psMain` | `ShaderType::Pixel` | `float4 : SV_Target` |

- 顶点输入结构体用 HLSL 语义标注，当前约定：
  ```hlsl
  // triangle.slang（M0.3 回归）
  struct VSInput  { float3 position : POSITION;  float3 color : COLOR; };
  struct VSOutput { float4 position : SV_Position; float3 color : COLOR; };

  // model.slang（M0.4，与 SceneVertex 32 字节布局严格对应）
  struct VSInput  { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD; };
  struct VSOutput { float4 position : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD; };

  // hud.slang（M0.5，与 HudVertex 32 字节布局严格对应）
  struct VSInput  { float2 position : POSITION; float2 uv : TEXCOORD; float4 color : COLOR; };
  struct VSOutput { float4 position : SV_Position; float2 uv : TEXCOORD; float4 color : COLOR; };
  ```
- **⚠️ shader 里的顶点输入布局（`POSITION` / `COLOR` / `NORMAL` / `TEXCOORD` 的 slot、格式、步长）必须与 backend 侧 C++ 的 `VertexAttributeDesc` 完全一致**，否则会出现「能编译但顶点错乱 / 黑屏」。改任一侧都要同步另一侧。
- `model.slang` 的全局资源统一用 D3D 风格寄存器：`cbuffer SceneConstants : register(b0)`、`Texture2D g_baseColor : register(t0)`、`SamplerState g_sampler : register(s0)`；**不写 `[[vk::binding]]` 之类 GLSL/SPIR-V 专用注解**，Vulkan 侧靠编译期 shift 参数解决（见 §2.5）。

### 1.3 新增一个 shader 的步骤
1. 在 `renderer/shaders/` 新增 `<name>.slang`，遵守 1.1 / 1.2。
2. **无需改 CMake**（`file(GLOB renderer/shaders/*.slang)` 自动收集，见 §3）。新增后重跑 build 脚本触发重新 configure 并生成 `<name>.{vs,ps}.{spv,dxil}`。
3. backend 侧用 `NL_SHADER_DIR` 读字节码，按后端分别 `setEntryName`（见 §2.4）。M0.4 起由共享 `renderer/src/scene.cpp` 按 `model.<vs|ps>.<spv|dxil>` 加载、M0.5 起由 `renderer/src/hud.cpp` 按 `hud.<vs|ps>.<spv|dxil>` 加载，两后端不再各硬编码一份路径；新增 shader 时照此加一个共享类（详见 `docs/IMGUI-GUIDE.md`）。
4. 两后端各跑一次 `viewer.exe --screenshot`，确认画面一致、调试层零输出。

---

## 2. Slang 导出流程（编译 → 字节码 → 加载）

### 2.1 工具链
- `thirdparty/slang/dist/bin/slangc.exe`（Slang v2026.21，vendored，gitignore，不提交）。
- `slangc` **没有 `--version`**，查版本/参数用 `slangc -h` / `slangc -h target`。
- DXIL 目标需要 Windows SDK 的 `dxcompiler.dll` + `dxil.dll`；**不下载**，CMake 直接 GLOB 已装 SDK：
  ```
  file(GLOB _dxc_list "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxcompiler.dll")
  ```
  取版本号最高的一项作为 `-dxc-path`。

### 2.2 编译命令（每个 shader 出 4 个产物）
每个源 `<name>.slang` 出 `<name>.{vs,ps}.{spv,dxil}` 四个文件（`vs`/`ps` × `spv`/`dxil`）。`<name>` = 源文件名（去扩展名），当前三个：`triangle`（M0.3 回归保留）、`model`（M0.4 带贴图模型）、`hud`（M0.5 ImGui 叠加）；M0.5 的 `--selftest` 机械核对其中 `model` + `hud` 共 **8 个产物**的魔数：

```bash
# SPIR-V（Vulkan）—— shift 参数对齐 NVRHI 的 VulkanBindingOffsets（见 §2.5）
slangc <name>.slang -target spirv -entry <vsMain|psMain> -stage <vertex|fragment> \
       -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -o <name>.vs.spv

# DXIL（DX12）—— 必须带 -dxc-path 指向 Windows SDK 的 x64 目录；不加 shift
slangc <name>.slang -dxc-path <WinSDK_x64_dir> -target dxil -entry <vsMain|psMain> -stage <vertex|fragment> -o <name>.vs.dxil
```


- `-stage` 取值：`vertex` / `fragment`（M0.3/M0.4 用到这两个；mesh/compute 后续阶段再加）。
- `-entry` 取值与 1.2 入口名一致（`vsMain` / `psMain`）。
- `-fvk-*-shift` 对所有 shader 统一加（无资源的 `triangle.slang` 不受影响）。缺 `-fvk-t-shift` 会触发 slangc 的 `E39029` 警告；三者与 NVRHI 的绑定偏移必须严格对应（见 §2.5）。

### 2.3 产物落盘与注入
- 输出目录：`${CMAKE_BINARY_DIR}/shaders` → 实际 `build/<backend>/shaders/`。
- CMake 用自定义 target `nl_shaders`（带 `ALL`）在 build 时生成；改 shader 源码会自动重编。
- 产物绝对路径通过编译宏注入 C++：`target_compile_definitions(nl_renderer PRIVATE NL_SHADER_DIR="${_SHADER_OUT}")`。
- `Scene` / `Hud` 都用 `readFileBytes(NL_SHADER_DIR "/<name>.<vs|ps>.<spv|dxil>")` 读字节码，再 `device->createShader(desc, blob, size)`。**不依赖进程 CWD**，避免运行时路径问题。当前加载 `<name>` = `model`（3D）与 `hud`（叠加）；`triangle.slang` 只作为 M0.3 历史回归源保留编译产物。
- 模型资产路径同理由 `NL_ASSET_DIR`（`assets/models/Suzanne.glb`）注入，同样不依赖 CWD。
- `viewer` 通过 `add_dependencies(viewer nl_shaders)` 保证运行时字节码已生成。

### 2.4 ⚠️ 致命坑：SPIR-V 入口点永远叫 `main`
**Slang 编译 SPIR-V 时，入口点名恒为 `main`，完全无视 `-entry` 参数；DXIL 才真正按 `-entry` 命名。**

后果：
- Vulkan 侧若把 `ShaderDesc.setEntryName("vsMain")`，NVRHI 在 SPIR-V 里找不到 `vsMain` 入口 → **`createShader` 静默返回 NULL** → pipeline 建不出来 → 三角形全黑、调试层可能零报错，极难定位。
- 正确做法（已固化在 `renderer/src/scene.cpp` 的参数里，两个后端各自传）：
  ```cpp
  // backend_vulkan.cpp —— SPIR-V 入口固定为 "main"
  scene_.init(nvrhiDevice_, "spv",  "main",   "main",   outError);
  // backend_dx12.cpp —— DXIL 认 -entry，用真实入口名
  scene_.init(device_,      "dxil", "vsMain", "psMain", outError);
  hud_.init  (device_,      "dxil", "vsMain", "psMain", outError);

  // backend_vulkan.cpp —— HUD 同样走 "main"
  hud_.init  (nvrhiDevice_, "spv",  "main",   "main",   outError);
  ```
- **自检字节码合法性**（当初就是靠这个定位的）：
  - `.dxil` 文件头魔数 `44 58 42 43`（ASCII `DXBC`）。
  - `.spv`  文件头魔数 `03 02 23 07`（SPIR-V）。
  - 用 `xxd` / 二进制查看器核对；若 Vulkan 端字节数与 DX12 端差异过大（如差一截），优先怀疑入口名/ stage 不匹配。

### 2.5 ⚠️ 资源绑定：HLSL register 与 Vulkan binding 的桥
Vulkan 没有 b/t/s 三套独立寄存器命名空间，NVRHI 用 `VulkanBindingOffsets` 默认偏移把 slot 映射到 binding：**SRV 0 / Sampler 128 / ConstantBuffer 256（set 0）**。`slangc` 的 shift 参数必须与之逐项对齐：

| shader 声明 | DX12 根签名 | SPIR-V binding | NVRHI `BindingLayoutItem` |
|---|---|---|---|
| `cbuffer SceneConstants : register(b0)`（`model.slang`，160 B） | b0 | **256**（`-fvk-b-shift 256 0`） | `VolatileConstantBuffer(0)` |
| `cbuffer HudConstants : register(b0)`（`hud.slang`，16 B `float4`） | b0 | **256**（同上） | `VolatileConstantBuffer(0)` |
| `Texture2D g_baseColor : register(t0)` / `g_font : register(t0)` | t0 | **0**（`-fvk-t-shift 0 0`） | `Texture_SRV(0)` |
| `SamplerState g_sampler : register(s0)` | s0 | **128**（`-fvk-s-shift 128 0`） | `Sampler(0)` |

- 用 `slangc` 编出 `.spv` 后，解析 `OpDecorate ... Binding/DescriptorSet` 即可机械核对（M0.4 实测：vs = `{256}`，ps = `{0, 128, 256}`，全部 set 0）。
- `SceneConstants` 是 40 个 float / **160 字节**：`mvp[16] + model[16] + lightDir[4] + options[4]`，与 C++ 的 `SceneConstants` 结构体 `static_assert(sizeof == 160)` 对齐。C++ 侧的 `FrameParams` 尾部还有 `bool cullBack`，**绝不能整块 memcpy 进 cbuffer**，只拷贝前 160 字节。
- MVP 走 NVRHI 的 **volatile constant buffer**（`setIsVolatile(true).setMaxVersions(16)`），每帧 `writeBuffer` 一版，语义接近 push constant 而两后端由 NVRHI 统一处理；比手写 root constants / VkPushConstantRange 更不容易踩跨 API 坑。

---

## 3. 当前局限
- **CMake 已改为 `file(GLOB renderer/shaders/*.slang)` 自动收集**：新增 shader 只需在 `renderer/shaders/` 放一份 `<name>.slang`，无需改 CMake，重新跑 build 脚本即触发重新 configure 并生成 `<name>.{vs,ps}.{spv,dxil}`。注意 `file(GLOB)` 在 configure 时求值，纯新增文件（不碰 CMakeLists）时若没触发重编，手动重跑一次 build 脚本即可。
- **shader 名仍由共享类固定拼接**：M0.4 把两后端重复的路径/入口名收进了 `renderer/src/scene.cpp`，M0.5 又加了一个 `renderer/src/hud.cpp`（`<name>` = `hud`），两者各自是常量；多材质系统（shader 名 → 产物 + 入口 + pipeline 的注册表）留到 S1。
- HUD 只绑一张字体图集、忽略 `ImDrawCmd::TextureId`；自定义纹理 / 中文 / 输入框 / docking 的扩法见 `docs/IMGUI-GUIDE.md`。
- 多 pass / 多 render target 的 shader 尚未涉及，`-stage` 目前只用 `vertex`/`fragment`。
- 贴图用 `RGBA8_UNORM`（不做 sRGB → linear 转换），`model.slang` 里乘 2.2 曝光补偿官方 Suzanne 贴图偏暗；正式色彩管理留到后续阶段。

## 4. 验收自检
```bash
scripts/build-m0.2.bat DX12        # 或 VULKAN
build/<backend>/viewer.exe --selftest                                   # 14/14，含 8 个产物魔数
build/<backend>/viewer.exe --screenshot out.png --frames 8 --no-hud
build/<backend>/viewer.exe --screenshot hud.png --frames 8 --hud --size 960x540
build/<backend>/viewer.exe --screenshot inner.png --dist 0.6 --cull none
```
- `spv` = `03 02 23 07`、`dxil` = `DXBC`，`model` 与 `hud` 各 4 个产物共 8 个，由 `--selftest` 逐项核对并打印字节数。
- 两后端都渲染出带贴图的 Suzanne（黄色眼睛、灰色皮毛细节），`deviceErrors=0`，`--no-hud` 正常视角 PNG **逐字节相同**；`--hud` 两张必然不同（面板印的后端名 / 调试层不同），见 `docs/IMGUI-GUIDE.md` §5。
- `--dist 0.6 --cull none` 相机在模型内部能看到带贴图内壁；同参数 `--cull back` 只剩背景，证明剔除真的生效。
- 改 shader → 重新 build 自动重编字节码，无需手动跑 slangc。
