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
`model.slang` 当前三对入口（已由 `CMakeLists.txt` 的 `_ENTRIES_MODEL` 各自编出 `.{spv,dxil}`）：

| 用途 | 顶点入口 | 片元入口 | NVRHI ShaderType | 输入签名差异 |
|---|---|---|---|---|
| 普通场景 | `vsMain` | `psMain` | Vertex / Pixel | 3 属性（position / normal / uv） |
| cluster 场景（M1.3） | `vsCluster` | `psCluster` | Vertex / Pixel | 多 1 个 `CLUSTERCOLOR` 顶点属性（D3D12 debug layer 要求与 input layout 严格对应） |
| GPU vertex pulling（M4.0） | `vsCull` | `psCull` | Vertex / Pixel | 无顶点缓冲，改从 SSBO 拉取（`SV_InstanceID` / `SV_VertexID`） |

> 每对入口的**产物后缀**与**入口名**是两个独立维度：`CMakeLists.txt` 用 `_ENTRIES_MODEL` 的 `入口名|后缀|stage` 三元组分别指定（如 `"vsCull|vscull|vertex"` → `-entry vsCull` 编译出 `model.vscull.{spv,dxil}`）。`scene_.init*` 读后缀选文件、运行时入口名按后端规则传（见 §2.4）。

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

### 2.4 ⚠️ 致命坑：入口名分「编译期」与「加载期」两个阶段，规则相反
入口名由谁决定，取决于你是在**编译期（slangc）**还是**加载期（NVRHI runtime）**看它——两阶段规则正好相反，搞混就会误判（例如以为 `initCluster` 传 `vsMain` 加载 `vsCluster` 字节码是 bug，其实不是）：

- **编译期（`slangc -entry`）**：
  - `DXIL`（`-target dxil`）：**认 `-entry`**，blob 内的入口名就是 `-entry` 写的（`vsMain` / `vsCluster` / `vsCull` …）。
  - `SPIR-V`（`-target spirv`）：**永远叫 `main`，完全无视 `-entry`**。这是 Slang 的行为，不是 NVRHI 的。
- **加载期（`ShaderDesc.setEntryName`，即 `scene_.init*` 传的第三/四个字符串参数）**：
  - **Vulkan**：NVRHI 把 `setEntryName` 当成 SPIR-V 的 `pName`（见 vendored `thirdparty/nvrhi/src/vulkan/vulkan-graphics.cpp`）。SPIR-V 入口恒为 `main` → **Vulkan 必须传 `"main"`**，传 `vsMain`/`vsCluster` 等会找不到入口。
  - **DX12**：NVRHI 直接把整段字节码喂进 PSO（见 vendored `thirdparty/nvrhi/src/d3d12/d3d12-graphics.cpp`），**完全不碰 `setEntryName`**。DX12 用的是 blob 里编译期 `-entry` 定下的那个唯一入口；运行时传的入口名是**纯摆设**（传 `vsMain`/`vsCluster` 都行，不影响结果）。

后果与正确做法：
- Vulkan 侧若把 `setEntryName` 设成 `"vsMain"`/`"vsCluster"` 等非 `"main"` 值，NVRHI 在 SPIR-V 里找不到该入口 → **`createShader` 静默返回 NULL** → pipeline 建不出来 → 画面全黑、调试层可能零报错，极难定位。
- **M-P3 实测补充（2026-11-09，phys-p3 卡）**：故障模式不止一种。若 shader 模块创建成功但 `pName` 仍是错误入口名，拒绝发生在**管线创建期**：`vkCreateGraphicsPipelines` 每次调用返回 `VK_ERROR_UNKNOWN`，验证层（VVL）对管线上报**零错误**（`Geometry` capability 报错是 model.ps 的既有噪声，与本坑无关），错误只有我们自己的 `createGraphicsPipeline failed for ...` 一行——极难定位。踩法也更新了：不一定是后端传参错，**共享代码里的管线 builder 硬编码入口名**同样中招（M-P3 的 `DebugLines::ensure` 由 `ensurePipelines` 共享调用、写死 `"vsMain"/"psMain"`，DX12 恰好忽略入口名蒙混过关，Vulkan 每帧失败 240 次）。修复：共享 builder 一律传 `"main"`（与 compute 管线同款先例，DX12 忽略之，双后端通用），见 `scene.cpp` `ensurePipelines` 内注释。定位手段：临时在 vendored `vulkan-graphics.cpp` 的 `createGraphicsPipelines` 调用后打印 `vk::to_string(res)`（用完即撤），再用 UE 5.8 自带的 `VkLayer_khronos_validation`（`VK_LAYER_PATH` 指向 `Engine/Binaries/ThirdParty/Vulkan/Win64` + `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`）排除管线状态问题。
- 两个后端现在的传参（固化在 `renderer/src/backend_dx12.cpp` 与 `backend_vulkan.cpp`，注释已对齐）：
  ```cpp
  // Vulkan：SPIR-V 入口恒 "main"（不管 normal/cluster/cull 哪套入口）
  scene_.init       (nvrhiDevice_, "spv", "main", "main", modelFile, outError);
  scene_.initCluster(nvrhiDevice_, "spv", "main", "main", modelFile, outError);
  scene_.initCull   (nvrhiDevice_, "spv", "main", "main", modelFile, outError);
  hud_.init         (nvrhiDevice_, "spv", "main", "main", outError);

  // DX12：运行时入口名被 NVRHI 忽略，真正生效的是编译期 -entry 写进 blob 的入口
  //       （vsMain/vsCluster/vsCull …）。这里传真实名仅为可读性，传啥都行。
  scene_.init       (device_,      "dxil", "vsMain",    "psMain",    modelFile, outError);
  scene_.initCluster(device_,      "dxil", "vsCluster", "psCluster", modelFile, outError);
  scene_.initCull   (device_,      "dxil", "vsCull",    "psCull",    modelFile, outError);
  hud_.init         (device_,      "dxil", "vsMain",    "psMain",    outError);
  ```
- 所以「`initCluster` 加载 `model.vscluster` 但传 `vsMain` 入口名」**不是 bug**：Vulkan 传的是 `"main"`（对），DX12 传的 `"vsMain"` 被忽略、用的是 blob 里的 `vsCluster`（也对）。新增同类入口（如未来的剔除变体）照此接线即可，不会踩坑。
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

### 2.7 ⚠️ 一个 graphics/compute 管线只用一个 BindingSet（2026-11-09，S6.0 实测教训）
- **本项目 slangc 统一把所有资源编进 vk set 0**（`-fvk-b-shift 256 0` 的 `0` = set 号）。NVRHI Vulkan 的 legacy 模式（`registerSpaceIsDescriptorSet=false`）会把管线的**多个 BindingLayout 拆成多个 vk descriptor set**（set 0, 1, …）——shader 却仍在 vk set 0 找所有资源 → 第二个 set 里的资源（如 b1）在 Vulkan 上**根本绑不上**，未绑定描述符读出垃圾（实测读出 NaN：mip0 模型像素全 NaN → min 链被 NMin 丢掉只剩清屏值 1e9 → top 回读恒 0xFFFFFFFF「pending」）。
- **DX12 没有描述符集概念，同样的双 set 代码完全正常**——所以这类 bug 只在 Vulkan 爆，且 NVRHI validation 不报错（资源确实「绑在了它该在的 set」）。诊断手段：往 compute 里写哨兵值（如 0xDEADBEEF）确认 UAV 写/copy 链路，再用绝对 LOD 多点采样纹理定位坏层。
- **项目口径：每管线恰好一个 BindingSet**，需要多份资源就并进一个 layout/set（如 hiz 预通道 = b0 mvp + b1 hiz 常量 + t1..t4 vertex pulling，单一 `hizPrepassSet_`）。若未来真需要多 set，必须改用 `registerSpaceIsDescriptorSet=true` + 按 set 号分档的 `-fvk-*-shift` 编译参数——两套口径不许混用。

### 2.8 ⚠️ Hi-Z 金字塔尺寸必须 POT（2026-11-09，S6.0 实测教训）
- D3D12 与 Vulkan 都限制 `mipLevels ≤ floor(log2(maxdim)) + 1`：640 宽的非 POT 纹理上限 10 层，ceil 对半链（640→320→…→5→3→2→1）要 11 层直接超规格。DX12 debug layer 报错；**Vulkan 无 VVL 时驱动静默建坏**（SRV/UAV 全踩坑）。修法：Hi-Z 目标尺寸向上取 2 的幂（UE HZB 同款），mip0 视口外区域每帧清 1e9，保守性不破（min 金字塔「空 = 更远 = 不遮挡」方向正确）。
- RT 贴图创建必须带 `setClearValue`（与每帧 clearColor 一致），否则 D3D12 debug layer 每帧报 ClearRenderTargetView info message 被计入 deviceErrors（acceptance 卡零容错）。
- **遮挡金字塔口径 = max（每 texel 最远表面径向距离），不是 min**（M6.1 拍板修正，2026-11-09）：遮挡判定 =「簇最近点 vs 区域最远表面」（UE `NaniteHZBCull.ush IsVisibleHZB` inverted-Z 取 min、bevy `downsample_depth.wesl` reverse-Z 取 min，两者换算到径向距离度量都是 max）。S6-PLAN 原文 min 口径在径向距离下不保守会漏画。连锁语义：①边界 clamp 读「空 texel = 1e9 参与 max 只放大」保守方向仍正确；②top 回读三态（真实值 / -2 背景主导 / -1 无数据），画面有背景像素时 top far = 1e9 是**合法值**不是异常；③dag 遍历侧 view space 是右手系 -z 朝前（camera.cpp:28），Mara&Morgan/UE 球投影公式按 +z 朝前推导，必须 `wz = -vc.z` 换轴，否则剔除静默失效（hiz==dag 无剔除的假绿）。
- **帧内禁止开第二个 immediate command list**（NVRHI「Two or more immediate command lists cannot be open」）：懒建资源（如金字塔首建）的补清屏用 just-created 标志挪到当前帧 cmd 执行，不能在 ensureHizTargets 里另开列表执行。

### 2.9 ⚠️ 64 位原子：编译 profile、设备启用、介质三件事（2026-11-09，M7.0 实测）
- **DXIL 必须 `-profile sm_6_6`**：默认 profile 下 slangc/DXC 报 `opcode '64-bit atomic operations' should only be used in Shader Model 6.6+`（纹理 / buffer / PS 三路全试过）。CMake 用 `_SM66_ARTIFACTS` 白名单**只给 visbuf 的 4 个新产物**加，老产物一个字节都不能动——否则 exe=current 的 HUD 基线要全量重拍。SPIR-V 不需要 profile，直通（`OpCapability Int64` + `Int64Atomics`）。
- **Vulkan 查询 ≠ 启用**：`reportFeatures()` 里的 `vkGetPhysicalDeviceFeatures2` 只是查；真正用 64 位原子必须在 `vkCreateDevice` 的 `pNext` 挂 `VkPhysicalDeviceShaderAtomicInt64Features`（`shaderBufferInt64Atomics=VK_TRUE`）并在 device extension 里加 `VK_KHR_shader_atomic_int64`。只查不启 = SPIR-V 编得出、运行时被驱动拒。Vulkan 1.4 头文件里这个 feature 已从 `Vulkan12Features` 拆成独立结构。
- **DX12 能力位在 Options11**：`AtomicInt64OnDescriptorHeapResourceSupported` 才对应 `RWStructuredBuffer`（structured/raw UAV）；Options9 的 `AtomicInt64OnTypedResourceSupported` / `AtomicInt64OnGroupSharedSupported` 分别管 typed resource 与 group shared，**不能拿它们当 buffer 原子的判据**。
- **介质：u64 buffer，不是 R64 纹理**：vendored NVRHI 零 R64 格式；`InterlockedMin(RWStructuredBuffer<uint64_t>[idx], v)`。buffer desc 需 `structStride=8`、`canHaveUAVs=true`、`initialState=CopyDest`、`keepInitialState=true`。`clearBufferUInt(buf, 0xFFFFFFFFu)` 正好把每个 u64 清成 `VIS_EMPTY`（DX12 自动建 R32_UINT typed clear UAV，与 structured UAV 并存合法；Vulkan 走 fillBuffer）。
- **clear 必须排在首个 `setGraphicsState` 之前**（Vulkan 的 `clearBufferUInt` 内部 `endRenderPass()`）；validation 层要求命令列表类型 ≤ `CommandQueue::Compute`，graphics 列表（枚举 0）满足。
- **公共头用 `.slangh` 后缀**：`visbuf_common.slangh` 只被 `#include`，不进 CMake 的 `*.slang` GLOB（否则会被当成独立 shader 源去找 `vsMain/psMain`）。CMake 只给真正 include 它的产物加 `DEPENDS`，避免老产物的构建规则变动被 NMake 判为过期。

### 2.10 ⚠️ 实例化禁止依赖 startInstanceLocation 的 SV_InstanceID 全局语义（2026-11-09，M-E1b.1 实测）

- **现象**：ECS 场景多 batch 时把全场实例放一个 StructuredBuffer，用 `DrawArguments::setStartInstanceLocation(batch.instanceStart)` 划窗口，认定 `SV_InstanceID` 是全局槽号。**DX12 实际不把 `StartInstanceLocation` 计入 `SV_InstanceID`** → 每个 batch 都从槽 0 开始读，画面只剩第一个 batch 的实例。
- **判据**：2-batch 最小探针（两实例 x=±4）叠成一个 blob；50 实例 / 4 batch 只剩 13 个物体，连通域位置恰与第一个 batch 的 13 个实例一一对应。单 batch 场景（M-E1b.0）不会暴露此坑。
- **定稿**：**每个 draw 批次持有自己的实例 StructuredBuffer**，shader 用 0-based `SV_InstanceID` 直接按 `id*5` 寻址，`drawIndexed` 不设 `startInstanceLocation`。与 Carrot `ModelRenderSystem::renderModels` 逐 modelRenderer 交 `InstanceData` 同口径，也绕开 DXIL / SPIR-V 对实例 base 语义的差异。
- **不走「同一 buffer 绑 SRV 子范围」**：`BindingSetItem::StructuredBuffer_SRV` 虽支持 `BufferRange`，但 Vulkan 的 `VkDescriptorBufferInfo.offset` 受 `minStorageBufferOffsetAlignment`（常 64B）约束，而批次起点按 5×16=80B 递增会踩对齐。
- 单 batch 时新旧实现同构，既有画面基线逐字节不变（M-E1b.1 实测 MD5 一致）。

### 2.11 ⚠️ ECS 场景点光源灯缓冲布局与向后兼容（2026-11-09，M-E1b.2）

- **t6 = `g_lights` 定稿**：`[0] = (lightCount, ambient, 1/ΣI(仅方向灯), pointCount)`，之后**每灯 3 个 float4**：`{dir.xyz, intensity}`、`{color.rgb, type}`、`{position.xyz, InvRadius}`。`type=0` 方向灯 / `1` 点光；`InvRadius = 1/range` 由 CPU 预算（UE `LightData.InvRadius` 同款）——**shader 内禁止再对灯参数做除法**，DXIL / SPIR-V 的除法取整会拆散双后端 bytes。M-E1b.0/1 的「每灯 2 个 float4」布局已作废。
- **衰减公式照抄 UE** `Engine/Shaders/Private/DeferredLightingCommon.ush:480-486`（实读）：`DistanceAttenuation = 1/(d²+1) * Square(saturate(1 - Square(d² * InvRadius²)))`；`L = ToLight * rsqrt(d²)`、`NoL = saturate(dot(N,L))`（点光单面；方向灯仍用 `abs(dot)` 兼容关背面剔除）。半径外窗函数平滑归零而非硬截断。
- **归一化口径**：方向灯贡献按 `1/ΣI` 加权平均（M-E1b.0 口径，避免多灯过曝），点光按绝对强度**独立累加**、不进归一化（否则 `att<<1` 会让点光几乎不可见）。**无点光时 `psScene` 必须走 M-E1b.0 原表达式分支**（以 `[0].w = pointCount` 作 uniform 分支判据），否则既有画面基线会变。
- **`vsScene` 输出 `WORLDPOS` 插值量**供点光算距离；新增 interpolator 后必须复拍 `scene` / `scale` 两卡验证既有基线（本轮实测见 `MILESTONES.md` M-E1b.2 段）。
- **组件序列化向后兼容铁律**：`LightComponent::serialise()` 对 `type=0` **不写新键**（既有场景 JSON 与 `ecs-serial` roundtrip 逐字节不变）；`deserialise` 缺键按方向灯处理；非法 `type` / `range<=0` 在 viewer 收集段显式报错退出码 2，不在组件层 assert。

### 2.12 ⚠️ ECS 逐实例 PBR 材质 + Cook-Torrance 换代口径（2026-11-09，M-E1b.3）

- **t5 = `g_instanceData` 扩到每实例 7 个 float4**：`[0..3]` 世界矩阵四列、`[4]=(tint.rgb,0)`、`[5]=(roughness, metallic, 0, 0)`。缺省 **roughness=1 / metallic=0**（matte 介电质）；`ModelComponent::serialise()` **不写这两键**（roundtrip 逐字节铁律，`selftest --all` 守护）。
- **视向量 V 来源 = b1 `LightConstants.g_cameraPos`**：ECS 分支复用 pbr-direct 同款 80B volatile CB（`maxVersions=16`），每帧只写 `cameraPos = FrameParams.eye`；BindingLayout/BindingSet 必须同步扩 slot1，**漏扩 = Vulkan b1 读 NaN**（§2.7 单一 BindingSet 铁律的 ECS 版）。
- **ambient 项必须乘 `cDiff`（= albedo·(1-metallic)）而非 `albedo`**：金属无漫反射环境光。乘 albedo 会把金属暗部染成粉底色、抹平 metallic 0/1 的材质对比——这是 M-E1b.3 起的铁律。
- **亮度锚定**：Cook-Torrance `1/π` × 方向灯 `1/ΣI` 双重压缩比 Lambert 暗 ~3×。**shader 口径不动（与 shadePbr 同式拍板），亮度用场景布光补偿**（E1b 场景 ambient 0.9 / dir color ×4.2 / point ×3.0；白 matte 锚定 E1b.2 旧亮度 ±7）。金属物体物理性偏暗是无 IBL 的正确表现，**不要当 bug 修**。
- **色彩管线**：ECS `psScene` 已换代为 `srgbToLinear(base.rgb) * tint` 进光、`pow(saturate(Lo), 1/2.2)` 出（替换旧 `*2.2f` sRGB 空间曝光补偿）；t0 贴图仍 `RGBA8_UNORM` 直读。彩色饱和度略低于 Lambert 时代 = 白色介电质 specular（F0=0.04）稀释，物理正确。
- **改 shader 后先删 exe 再 build**：NMake 同秒 mtime 会跳重链（`Built target viewer` 但 exe 未更新），regression 过期门禁会拦；两次踩中。

### 2.13 ⚠️ ECS 方向灯 2D 阴影图口径（2026-11-09，M-E1b.4）

- **资源布局**：`Texture2DArray R32F 1024×1024×shadowCount`（每阴影灯一层）+ b2 `ShadowConstantsCB`（`viewProj[4]` + `params[4][3]`，160B volatile；`params[s]` = `{enabled, bias, size}`）。灯打包复用 t6 `posRng.w` 作 shadowSlot+1（0=无阴影；方向灯分支原本不读该字段）。**>4 盏阴影灯 initScene 显式报错**（viewProj[4] 数组上限）。
- **阴影 pass 无 depth attachment，nearest 深度用 R32F MIN 混合实现**（src/dst `One` + `BlendOp::Min`）：语义 = 深度测试 LessEqual 且**顺序无关**。**坑**：无深度测试 + 无混合时 psShadow 无条件写 → 按绘制顺序后画覆盖先画（地面盘覆盖整个灯视锥 → 物体深度被冲掉 → 永远判亮）。D3D12 对 float32 RT 强制支持 MIN/MAX blend，Vulkan 规范同一张强制表（R32_SFLOAT 要求 VK_BLEND_OP_MIN/MAX），双后端对称。depth test/write 必须 disable（R32F framebuffer 无深度格式，DX12 validation 拒绝建管线）。
- **pass 顺序（draw() 内，主 pass 之前）**：b2 写零 + `beginTrackingTextureState(shadowMap_, ShaderResource)` **无条件做**（主 pass 恒绑 b2/t7，NVRHI 校验 volatile CB 未写即用 = 报错）→ 每层 `clearTextureFloat(1.0)`（clear 先于任何 setGraphicsState，Vulkan render pass 纪律同 §2.8）→ 全层状态切 RenderTarget → 逐灯逐 batch 渲染（`Viewport(1024,1024)`，SPIR-V 入口名恒 `main` 见 §2.4）→ 全层切回 ShaderResource → 主 pass。
- **接收侧采样口径**：正交下 `clip.w==1`，`recDepth = clip.z` 已在 [0,1]；texel 映射 `uv = (ndc.x*0.5+0.5, 0.5−ndc.y*0.5)`（DX 视口约定 clip.y=+1 → 图像第 0 行，NVRHI Vulkan 视口换算与 DX 对齐）；`Texture2DArray.Load(int4(texel, slice, 0))` 整数 texel 直读（第 4 分量 = array slice）；视锥外全亮。3×3 PCF clamp 到边缘。
- **坡度缩放 bias（M-E1b.4 拍板数值）**：MIN 混合存 nearest 深度，斜面 texel 内深度跨度 ∝ (1−NdotL)——PROBE-D2 实测本场景地面 ~0.004 / 头部斜面 ~0.028，固定 bias 0.003 盖不住 → 物体整体误阴影。`bias = 0.003(CPU) + 0.035×(1−NdotL)(shader)` 实测覆盖；`abs(NdotL)` 双面照亮的背面按 NdotL=0 取满 bias（保守正确）。调 bias 前先跑三通道探针（R=图值 G=接收深度 B=差值×10，一眼分类：暗黄=一致/红=图深/绿=接收深）。
- **量化闸基线必须同场景**：阴影「只变暗不变亮」断言对比的是**同场景 shadow=false**（`ecs-e1b-shadow-base.scene`）；拿不同场景（如无地面盘的 `ecs-e1b.scene`）当基线必然大面积「变亮」误报。
- **探针结论有效性前置**：改 shader 后跑任何画面探针，先核 `exe mtime > shader mtime`（NMake 同秒 mtime 跳重链，§2.12 尾条），否则结论全部作废——M-E1b.4 连环假阴性（shadowTry1~4/PROBE-S）的真实根因。


### 2.14 ⚠️ vis buffer 材质间接表（2026-11-10，M7.2；S9/S10/S13 复用纪律）

- **材质归属构建期随簇记录，渲染期不猜**：`.nmesh` 'NSUB' 尾段（v3 只加不改）存子网格簇区间（clusterFirst/clusterCount 8B/项），`clusterMeta[3]` = materialId（子网格序号）；加载期按区间展开成逐簇 id 数组，渲染期一次 SRV 查表。UE 同口径（簇编码带 MaterialTableOffset/Length，`NaniteDataDecode.ush:116-117`）；无 NSUB 的旧文件恒 materialId=0。
- **间接表布局（§2.7 单一 BindingSet 内）**：`GPUMaterial[4]` SSBO t12（32B/项：baseColorFactor float4 + mrFactor float2 + 贴图槽号 uint2）+ 贴图**固定槽数组** `g_baseColorTex[4]`(t4..t7) / `g_mrTex[4]`(t8..t11)。槽位在 Scene init 时静态分配（材质 0 沿用 texture_/mrTexture_ 句柄），空槽绑 1x1 dummy 纹理——**NVRHI 拒绝空纹理绑定**，即使 shader 逻辑上不会命中（NSUB 对账保证 materialId < materialCount）。
- **materialId 逐像素非一致 → 资源数组选槽必须 `NonUniformResourceIndex`**：全屏三角形材质 pass 相邻像素可属不同子网格，裸下标会触发未定义行为/错误层级展开。Slang 双目标实测通过（DXIL 产出 `NVN`/dot 路径、SPIR-V 产出 `OpCompositeExtract` 非一致修饰），产物进 `_SM66_ARTIFACTS`。
- **两个渲染路径共用同一份因子**：vis 路径因子在 GPUMaterial 表里；传统直画路径 per-prim 批次（共享 VB/IB 按 `startIndexLocation` 区间）把同值写进各自 b1 volatile CB——**volatile CB 全量覆写语义**（§2.5），光照/相机同值 + 各批因子必须在**每次 draw 前整块重写**，漏写因子=首帧后随机 tint。`model.slang` 零改动是多材质对照组的硬指标。
- **`kMaxVisMaterials=4` 是渲染侧上限**（贴图槽 t4..t11 + 表项 4），不是 .nmesh 格式上限（NSUB 段无子网格数限制）；扩容 = 槽位数组 + GPUMaterial 数组 + dummy 绑定三处同步，且 initCull 对账检查 `materialCount == nmeshSubmeshCount_` 显式报错。


### 2.15 ⚠️ NVRHI buffer 每帧写回：permanent 冻结与跨列表跟踪（2026-11-09，M-E1b.5 渲染链）

- **`setPermanentBufferState(buf, X)` 是一次性冻结**：之后所有写路径（`writeBuffer`/`copyBuffer` 内部 `requireBufferState(CopyDest)`）对 permanent buffer **直接校验 permanent 位、不插 barrier**——permanent=ShaderResource 时每帧写必报 `Required: 0x1000(CopyDest), present: 0x60(SRV)`（首轮实测 deviceErrors=60，30 帧 × 2 batch）。混合位 permanent（`SRV|CopyDest`）能过校验但 Vulkan 端跨 submit 的 transfer→read hazard 无 barrier，禁用。
- **跟踪状态不跨命令列表**：`commandListSubmitted()` 清空 `m_BufferStates`——每个新命令列表（initScene 的 upload 列表 / viewer 每帧 update 的 upload 列表 / renderFrame 的帧列表）都必须在自己的列表 open 后、首次绑定前 `beginTrackingBufferState(buf, 当前态)` 如实声明，否则「Unknown prior state」。
- **`beginTrackingBufferState` 参数 = 当前态，不是目标态**（NVRHI 注释原话：informs the tracker **of the current state**）——谎报会让 writeBuffer 以为无需 barrier，错误留到绑定时才爆。
- **可行解（M-E1b.5 实装）**：写读交替的 buffer **不设 permanent**，用代码成员跨列表跟踪末态（序列完全确定才可行）：`Scene::instanceTrackSRV_`——initScene/upload 列表末态恒 CopyDest（writeBuffer 无转换落盘），draw 帧列表绑定 SRV 后末态恒 SRV；update 与 draw 各自 beginTracking 如实声明，SRV↔CopyDest 的往返 barrier 由 tracker 自动插入。
- **每帧实例刷新入口**：`Scene::updateObjects(device, objects, count, err)`（M-E1b.5 新增）——单 upload 命令列表写全部 batch 的 instances（§2.8「帧内禁开第二 cmd list」精神），batch 分组由 initScene 固化的 `sceneBatchObjects_` 重建（meshFile 首现序 + batch 内 objects 原序，槽号一一对应）；闸门不能查 `ready()`（四条 scene 管线是首帧 `ensurePipeline` 懒建的，initScene 后 ready() 恒 false），查 `sceneBatches_[0].instances` 存在性。


## 3. 当前局限
- **CMake 已改为 `file(GLOB renderer/shaders/*.slang)` 自动收集**：新增单入口 shader 只需在 `renderer/shaders/` 放一份 `<name>.slang`（默认 `vsMain/psMain` 双入口）；**多入口 shader 必须在 CMakeLists 登记 `_ENTRIES_<NAME>`**（model 6 / hiz 6 / visbuf 4 个入口），需要 SM6.6 的产物进 `_SM66_ARTIFACTS` 白名单（见 §2.9）。公共 `#include` 头用 `.slangh` 后缀避开 GLOB。注意 `file(GLOB)` 在 configure 时求值，纯新增文件若不触发重编，手动重跑一次 build 脚本即可。
- **shader 名仍由共享类固定拼接**：M0.4 把两后端重复的路径/入口名收进了 `renderer/src/scene.cpp`，M0.5 又加了一个 `renderer/src/hud.cpp`（`<name>` = `hud`），两者各自是常量；多材质系统（shader 名 → 产物 + 入口 + pipeline 的注册表）留到 S1。
- HUD 只绑一张字体图集、忽略 `ImDrawCmd::TextureId`；自定义纹理 / 中文 / 输入框 / docking 的扩法见 `docs/IMGUI-GUIDE.md`。
- 多 pass / 多 render target 的 shader 尚未涉及，`-stage` 目前只用 `vertex`/`fragment`。
- 贴图用 `RGBA8_UNORM`（不做 sRGB → linear 转换），`model.slang` 里乘 2.2 曝光补偿官方 Suzanne 贴图偏暗；正式色彩管理留到后续阶段。**（2026-11-09 更新：pbr-direct `psMain` mode 5 与 ECS `psScene`（M-E1b.3，§2.12）均已改 `srgbToLinear` + gamma encode；2.2 补偿仅存于 mode 0 风格路径。）**

## 4. 验收自检
```bash
scripts/build-m0.2.bat DX12        # 或 VULKAN
build/<backend>/viewer.exe --selftest                                   # 16/16，含 40 个产物魔数
build/<backend>/viewer.exe --screenshot out.png --frames 8 --no-hud
build/<backend>/viewer.exe --screenshot hud.png --frames 8 --hud --size 960x540
build/<backend>/viewer.exe --screenshot inner.png --dist 0.6 --cull none
```
- `spv` = `03 02 23 07`、`dxil` = `DXBC`，当前共 **40 个产物**（model 12 / cull 2 / dag 2 / hiz 12 / hud 4 / visbuf 8），由 `--selftest` 逐项核对并打印字节数。
- 两后端都渲染出带贴图的 Suzanne（黄色眼睛、灰色皮毛细节），`deviceErrors=0`，`--no-hud` 正常视角 PNG **逐字节相同**；`--hud` 两张必然不同（面板印的后端名 / 调试层不同），见 `docs/IMGUI-GUIDE.md` §5。
- `--dist 0.6 --cull none` 相机在模型内部能看到带贴图内壁；同参数 `--cull back` 只剩背景，证明剔除真的生效。
- 改 shader → 重新 build 自动重编字节码，无需手动跑 slangc。

### 2.6 M0.8 调试着色与线框的接法（2026-11-07）
- **着色模式走 cbuffer 分支，不走多套 shader**：`g_options.x` = 0 textured / 1 normal / 2 uv / 3 tri-id，一个 `psMain` 里 `if` 分支。四种模式共用同一对 vs/ps 字节码，pipeline 不感知着色模式。
- **三角形 ID 用 `SV_PrimitiveID`，PS 直接收、VS 不用传**：rasterizer 生成的系统值，DXIL 侧签名匹配允许注入，SPIR-V 侧对应 `gl_PrimitiveID` 内建（fragment 输入合法）。**slangc 双目标实测通过**（2026-11-07，SPIR-V `03 02 23 07` / DXIL `DXBC`），NVRHI validation 与 D3D12 Debug Layer 零报错。颜色用整数 hash（pcg 风格），无浮点随机源 → 同参重渲染 PNG 确定，可进截图回归。
- **线框不是 shader**：NVRHI `RasterState::setFillWireframe()`（光栅 fill mode），零几何 shader、零索引改动。Scene 的 pipeline 组合因此从 2 套扩到 **4 套（solid/wire × cull back/none）**，着色模式与线框正交。
- **FrameParams 尾部新增 `int shading / bool wireframe`**，同样不进 cbuffer 整块拷贝：`Scene::draw` 只把 `shading` 写进 `options[0]`，其余 options 原样带过。

### 2.16 ⚠️ Scene::draw 主路径重构：cluster 兜底与 spot 覆盖（2026-11-10，M7.2 真回归）
- **`Scene::draw` 末段服务多类场景**：init（normal/pbr）、initSkin、**initCluster（lod/dag/s5 层次遍历）** 都走同一段尾绘。initCluster 场景 `modelBatches_` 为空、用自己的 b0-only 绑定集与 `indexCount_`——**改成 per-prim 批次循环时必须保留 `modelBatches_.empty()` 的老单批兜底**，否则三模式整只模型消失（M7.2 实测 s2.0/s3.2/s5.0 前景 0 px）。
- **draw 主路径重构的 spot 回归必须覆盖 lod 或 dag 模式**：m0.9/pbr-direct/s7.0/s7.1 全走 init/per-prim/visbuf 路径，per-prim 重写把 cluster 路径炸空后 spot 依旧全绿——直到全量闸 HUD 基线 diff 6.4 万 px 才暴露。
- **基线重拍安全闸是最后防线**：diff 超出「HUD 一行文本」签名（≈527 px / maxDelta 240）必须追因，禁止人工强刷进基线。「模型区整片白」= 位移/变色/消失三选一，用前景包围盒+质心一测便知；「diff 恰好一行 HUD 文本」才是 kStage bump 的健康签名。

### 2.17 ⚠️ 粒子/程序化叠加层纪律（2026-11-10，M-PT0；M-PT1 .nfx 复用）
- **CPU 模拟确定性契约**：固定步长 1/60s 每帧恰一步（M-GL 物理同款），RNG 用 pcg 整数 hash（`particleHash`，**禁时钟输入**）——粒子状态是帧号纯函数，同参重跑 PNG 逐字节同（DX12 实测 MD5 全等）。发射率积分用 floor 取整，禁止浮点累积比较。
- **无 input layout 的 billboard 展开**：`particles.slang` VS 用 `SV_VertexID` 位式判定角点展开两枚三角形 + `SV_InstanceID` 取粒子实例；管线 `inputLayoutElements=0` + `CullMode::None`，顶点数恒 6、instanceCount=aliveCount。
- **相机基向量来自 view 行**：billboard right/up 从列主序 view 矩阵第 0/1 行取（`view[0]/[4]/[8]`、`view[1]/[5]/[9]`），非 dag/hiz 模式下 `fp.view` 由 viewer 自行补填。
- **实例 SSBO 每帧写回**：32B StructuredBuffer（posSize+colorAlpha）走 §2.15 同款 CopyDest + keepInitialState 帧内动态晋升；CB 112B volatile 每帧全量覆写（mvp + camRight + camUp + params）。
- **叠加层证据行仅专用模式显示**：HUD `Particles : N alive` 行只在 `--particles` 开启时占位（BLAS 行先例），避免 kStage 波及全量 HUD 基线重拍。

### 2.18 M9.0 vis-resolve 单一 BindingSet 槽位表（2026-11-10，落码实读固化）
- **唯一权威 = `Scene::addResolveLayoutItems` / `addResolveSetItems`**（`renderer/src/scene.cpp`）：`material.slang::psMatResolve`（全屏 PS）与 `direct.slang::csDirect`（compute CS）共用同一份定义，禁止两条链各写一份槽表（任务书曾凭记忆编过一张错的，见 `MILESTONES.md` P8 M9.0 踩坑 5）。
- SPIR-V 平移 `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-u-shift 384 0`（§2.5）；DX12 用各自 slot。

| 槽 | 资源 | 说明 |
|---|---|---|
| b0 | SceneConstants 160B | mvp/model/lightDir/options（§2.15，禁整块 memcpy） |
| b1 | LightConstants 80B | 光色 / 相机 / pbr-naive 开关 |
| b2 | MatResolveConstants 16B | (width, height, 0, 0) |
| t0 | vis buffer SRV (u64) | 与 raster 的 u0 是同一块 buffer 的两个视图 |
| t1..t3 | vertexPool / localIndices / clusterMeta | M4 vertex pulling 同源 |
| t4..t7 | baseColor 贴图数组 [kMaxVisMaterials] | `NonUniformResourceIndex` 选槽 → DXIL SM6.6 |
| t8..t11 | metallicRoughness 贴图数组 | |
| t12 | GPUMaterial 间接表 | §2.14，S9/S10/S13 复用 |
| s0 | sampler | |
| **b3** | **DirectConstants 16B** | **仅 direct.slang**：当帧清屏色 |
| **u0** | **RGBA32_FLOAT UAV** | **仅 direct.slang**：复用 M8.2 `rtTarget_`，不新建资源 |

- 两条链各自一个 BindingSet（§2.7）：material = b0/b1/b2 + t0..t12 + s0；direct = 同表 + b3 + u0。`Scene::writeResolveConstants` 是 b1/b2 每帧写入的唯一入口（两链共用）。
- **空像素语义**：PS 用 `discard` 让 framebuffer 清屏色透出；`psRtPresent` 全屏覆盖会盖掉 clear，故 CS 必须把当帧清屏色写进 u0 的空像素。`Renderer::renderFrame` 从 `ClearColor` 填 `FrameParams::clearColor[4]`（默认 `(0.05,0.08,0.15,1)`，**不是黑色**）；`--color` 四档可切，双后端 `pulling` vs `shadow` 逐字节一致实测。
- **尺寸一致性**：`g_width/g_height` 来自 `visW_/visH_`，dispatch 尺寸来自 `rtW_/rtH_`，两者独立取整 → `drawShadow` 显式断言相等，`directComputeSet_` 带尺寸版本闸（resize 重建，别复用 `rtComputeSet_`，它依赖 TLAS）。
- **新增 shader 入口必须登记 `shaderArtifactsSelfTest` 的 `kArtifacts`**（`renderer/src/renderer.cpp`）：清单只查列出项，漏登 = 产物丢失仍全绿（M8.1 已踩过一次）。M9.0 由 64 → 66（`direct.cs` 双目标）。

### 2.19 ⚠️ T0 共享 temporal / post-chain 基建纪律（2026-11-11，T0 决策 A + UE 口径）
- **范围**：T0 里程碑承载离屏 HDR RT（**`RGBA32_FLOAT`，落码期勘误：R11G11B10/16F 均破 8-bit 字节保形，实测见 T0-PLAN 落码期勘误**）+ 最小 post-chain + 独立 velocity 纹理（`RG16_FLOAT`）+ history 双纹理 ping-pong（`RGBA32_FLOAT`）；GBuffer 属性**不落盘**（Q4 方案 A 顺延项于 T0 拍「不落盘」，即取即算，详见 `docs/SHARED-INFRA-OPTIONS.md` / `docs/t0/T0-PLAN.md`）。
- **每 pass 单一 BindingSet（§2.7）**：离屏 HDR / velocity / history / tonemap 各 pass 各建自己的单一 set，禁止跨 set 引用（多 set 在 Vulkan 静默读 NaN）；shift 沿用 §2.5（b 256 / t 0 / s 128 / u 384，set 0），**不新增 shift 偏移**。
- **纹理状态登记（§2.15 纹理侧对应）**：`createTargets()` 新增 slot 级同步表初始化；每张离屏 RT / velocity / history 逐帧 `beginTrackingTextureState` 如实声明「当前态」，按消费端在 `RenderTarget ↔ ShaderResource` 间切换；**ping-pong 纹理（history0/1）不设 permanent 态**（permanent 冻结 + 跨列表跟踪会漏 hazard）。
- **clear 并入帧内 cmd list（§2.8）**：离屏 RT 首帧建 / clear 必须并入现有帧命令列表，不另开第二个 immediate command list。
- **主路径重构保形（§2.16）**：主帧从直出 swapchain 改为渲到离屏 HDR RT 属主路径重构；spot 回归必含 lod / dag 模式，基线 diff 超 ≈527px 签名必须追因、禁强刷。
- **不预设槽位号**：velocity / history / hdr 的具体 `register(u/t)` 在落码时按 `renderer/src/scene.cpp` 实读固化进本节，禁止凭记忆编表（Q4 材料 §0 纠偏教训）。

#### T0 槽位同步表（落码实读固化，2026-11-11）

| 资源 | 格式 | 创建点 | 绑定 set（§2.7 单一） | 状态登记（§2.15/§2.19） |
|---|---|---|---|---|
| `hdrSceneColor_` | `RGBA32_FLOAT`（勘误） | `ensureRtTargets`（视口尺寸懒建；raster 主路径经 draw() 顶部懒建同入此） | `tonemapSet_` t0 SRV（读）/ blit 写点 RT | 每帧 `beginTrackingTextureState(ShaderResource=上帧末态)` → clear → `setTextureState(RenderTarget)` → 帧末 `setTextureState(ShaderResource)`；**不设 permanent** |
| `t0Depth_`（共享 backend depth_） | `D32`（后端 keepInitialState=DepthWrite 永驻态） | `setSharedDepth`（createTargets 注入；resize 重调） | raster 场景 pass depth attachment（hdrSceneFb_） | **scene 不 clear 不 tracking**（后端 recordFrame 先行清 1.0）；overlay 深度测试语义 = pre-T0 |
| `hdrColorFb_` | 仅 `hdrSceneColor_`（无 depth） | 同上 | — | blit 写点帧缓冲 |
| `velocity_` | `RG16_FLOAT` | 同上 | T3 写入时建（MRT 附件） | T3 落码时登记（当前仅建纹理，未接管线） |
| `history0_` / `history1_` | `RGBA32_FLOAT`（勘误） | 同上 | T4 使用时建（ping-pong） | 按帧奇偶交换 read/write，**不设 permanent**（T0 仅建纹理） |
| blit：`blitSet_` | — | `ensureRtTargets` | t0 = `rtTarget_` SRV | 只读，无状态切换 |
| tonemap：`tonemapSet_` | — | `ensureRtTargets` | t0 = `hdrSceneColor_` SRV | 只读，读前 `hdrSceneColor_` 已转 `ShaderResource` |

> 落码铁律：ping-pong（history0/1）与 `hdrSceneColor_` 一律不设 permanent 态；PSO 随帧缓冲 **info**（格式/采样数）懒建、跨 resize 复用，set 随纹理重建。shift 沿用 §2.5（b 256 / t 0 / s 128 / u 384，set 0）。

#### M10.0 探针管线槽位同步表（落码实读固化，2026-11-11，tag `s10-place`）

| 资源 / 管线 | 格式 / 入口 | 创建点 | 绑定 set（§2.7 单一） | 状态登记（§2.15/§2.19） |
|---|---|---|---|---|
| `velocity_`（T0 建） | `RG16_FLOAT`，`vsVelocity`/`psVelocity` 写（`velocity.xy = curUV - prevUV`，消费端 `prevUV = curUV - velocity`） | `ensureRtTargets`（velocity prepass 帧缓冲色附件） | `velocitySet_`：b0 `velocityCB_`(144B volatile) + t1..t4 pulling 池 SRV | 创建态 = `ShaderResource` + keepInitialState（**必须与帧首 beginTracking 声明对齐**，否则首帧 DX12 ECL 校验 fail）；clear value 必须与创建期一致（perf 警告计 deviceErrors） |
| `probeDepth_` | `D32`（非 permanent；velocity prepass 深度附件 + csPlace SRV 读——共享 backend `depth_` keepInitialState 永驻态不能切 SRV） | `ensureProbeTargets`（视口尺寸懒建） | `placeSet_[2]` t0 SRV | 同上：创建态 SRV + keepInitialState；逐帧 `setTextureState(DepthWrite)` → clear 1.0 → 写 → csPlace 读 |
| `probeAtlas_[2][4]` | `RGBA32_FLOAT` ×2 parity ×4 级（16/8/4/2；rgb=探针世界位置，a=deviceZ，a=0=天空） | `ensureProbeTargets` | `placeSet_[parity]` u0..u3 UAV；`dotsSet_[parity]` t0..t3 SRV | 创建态 SRV；UAV↔SRV 转换由绑定 require 自动插 barrier |
| `probePrevDelta_` | `RGBA32_FLOAT`（**RG32F typed UAV store 是 DX12 可选特性，退 RGBA32F**，rtTarget_ 先例） | `ensureProbeTargets` | `placeSet_[parity]` u4 UAV；`dotsSet_[parity]` t4 SRV | 同 atlas |
| `probeCB_` | 176B volatile：[0..15] invViewProj / [16..19] eye+projYScale / [20..23] placeParams / [24..39] vizViewProj / [40..43] vizParams（place 与 viz 共用同一份 buffer 各读自己的段） | `initProbe` | b0（placeSet_ / dotsSet_） | 每帧全量覆写 |
| `velocityCB_` | 144B volatile：[0..15] cur viewProj / [16..31] prev viewProj / [32..35] (w,h,pad,pad) | `initProbe` | b0（velocitySet_） | 每帧全量覆写（**独立 CB 不与 probeCB_ 共用**——布局不同） |
| PSO | `velocityPSO_`（RG16F+D32 格式锁定、深度测试+写入 ON、cull Back）/ `placePSO_`（compute）/ `dotsPSO_`（swapchain 格式懒建、深度关、cull None） | initProbe / drawProbeDots | 各自单一 layout | — |

**M10.0 shader 产物命名铁律**：产物 = `文件名.小写入口.后缀`（如 `velocity.vsvelocity.spv`、`probe.csplace.dxil`）——kArtifacts 登记、loadShader、CMake `_ENTRIES_*` 三处必须同口径，写 `velocity.vs` 这类短名会 selftest BAD(-1B) 或运行时加载失败。新增入口三处登记：CMakeLists `_ENTRIES_*` + `kArtifacts` + loadShader。

> M10.0 补充纪律：dispatch 是 `cmd->dispatch(x,y,z)` 三参（NVRHI 无 `DispatchArguments`）；`ComparisonFunc` 枚举值是 `LessOrEqual` 非 `LessEqual`。探针法线 = 深度邻域 3-tap 反投影 cross（GBuffer 不落盘 Q4 冻结，纯 compute 无 ddx/ddy）。

#### M10.1 探针追踪管线槽位同步表（落码实读固化，2026-11-11，tag `s10-probe`）

| 资源 / 管线 | 格式 / 入口 | 创建点 | 绑定 set（§2.7 单一） | 状态登记（§2.15/§2.19） |
|---|---|---|---|---|
| `irradAtlas_[4]` | `RGBA16_FLOAT` ×4 级（res_l·10 × res_l·10；每探针占 10×10 格 = 8 core + 1 texel border，Q12-2；a=1 有效标记） | `ensureProbeTargets` | `traceSet_[2]` u0..u3 UAV；`dotsSet_[parity]` t5..t8 SRV | 创建态 SRV（与帧首 beginTracking 声明对齐）；UAV↔SRV 由绑定 require 自动插；**M10.1 单份不 ping-pong**（无时间混合） |
| `traceCB_` | 64B volatile：[0..3] 灯向 / [4..7] 灯色·强度(白 2.5，writeResolveConstants 同源) / [8..11] 命中 albedo(统一暖灰)+ambient / [12..15] shadowBias(0.01)+tMax(50)+baseRes+octRes | `initProbe` | b0（traceSet_） | 每帧全量覆写 |
| `traceSet_[2]` | 每 parity 一份（t0..t3 绑 cur place atlas + t4 TLAS） | `ensureProbeTargets` | — | **必须 `tlasReady_` 门控懒建**——`tlas_` null（无 RT / 无 NFLB fallback 资产）时 createBindingSet 必败；TLAS 就绪后帧内补建 |
| `tracePSO_` / `csTrace_` | compute `probe_trace.cstrace`（**CMake `_ENTRIES` 第二段 = 产物短后缀**：`cstrace` 非 `cstraceprobe`，tlas_probe.cs 口径；RayQuery 进 `_SM66_ARTIFACTS`） | initProbe / ensureProbeTargets | 单一 traceLayout_ | dispatch z=4 级，线程 = 探针格 × 10×10 展平 |
| 八面体头 `octahedral.slangh` | core N×N + 1 texel border（存储 (N+2)²）；border texel wrap 对侧 core texel（UE `OctahedralMapWrapBorder` 口径，trace 时按 wrap 方向单 pass 直算）；S12 复用 | — | — | — |
| 天穹头 `sky.slangh` | `skyGradient(dir)`：zenith(0.16,0.30,0.58)/horizon(0.62,0.71,0.84) + dir.y 余弦混合，地平线下 ×0.35（常量化确定性输出，Q12-3） | — | — | 渲染背景暂不替换（仍 clearColor，M10.3 可选） |
| dots set 扩展 | t9 = `probeDepth_` SRV（viz 法线 3-tap 重建输入，零新增资源） | `ensureProbeTargets` | `dotsSet_[parity]` t9 | 与 t0..t8 同份 set |
| `probeCB_` | **176→192B**：新增 [44..47] `g_vizExtra`（x=dotMode 0=级别色/1=辐照度采样；TLAS 未就绪时降级回 0） | `initProbe` | b0（placeSet_ / dotsSet_） | 每帧全量覆写 |

**M10.1 viz 采样口径**：点色 = 八面体图按**探针法线方向**采样（法线 = probeDepth 3-tap 反投影 cross，vs 内重算）；「探针→眼睛」方向在凸面上恒 miss（画面全天空色无信息量，冒烟实锤后改法线）。atlas 全局 texel = `probeTexel·(octRes+2) + octT + 1`——**漏乘探针格基址会全黑**（读成 (0,0) 格的天空 texel）。

**M10.1 降级备案（S10-PLAN Q12 已定项）**：命中点直光 = 统一 albedo（暖灰）+ 无 NdotL（命中点法线在 104-tri 粗代理上不可得）；S11 世界缓存接管后按 hit 材质重算。探针射线 tMax=1e4（bevy 注释：截断处 miss ≠ 真 miss，截太小漏天光）；shadow ray tMax=50 / bias=0.01（M9.1 g_shadowParams 同值）。

#### M10.2 Final Gather 槽位同步表（落码实读固化，2026-11-11，tag `s10-gather`）

| 资源 / 管线 | 格式 / 入口 | 创建点 | 绑定 set（§2.7 单一） | 状态登记（§2.15/§2.19） |
|---|---|---|---|---|
| `gatherLinear_` | `RGBA16_FLOAT` 全屏（gather 输出：rgb=间接光，a=合成权重和） | `ensureProbeTargets`（视口尺寸懒建，`probeGatherFirst_=true`） | `gatherSet_[parity]` u0 UAV + t8 SRV | 创建态 SRV；**帧首 beginTracking 分支**：首帧声明 SRV / 后续帧声明 UAV（上帧末态 = csGatherCompose UAV 读，**UAV 末态不是 SRV**）；重建纹理时复位 first |
| `hdrSceneColor_`（T0 建） | `RGBA32_FLOAT`，**补 `isUAV=true` flag**（NVRHI validation 对 UAV 绑定硬查 flag，L1631） | `ensureRtTargets` | `gatherSet_[parity]` u1 UAV（RMW 写）+ t9 SRV（读） | csGatherCompose dispatch 前 `setTextureState(UAV)`；RMW = 同纹理 SRV 读 + UAV RMW 写，每线程只碰自己 texel |
| `probePrevDelta_`（M10.0 建） | `RGBA32_FLOAT` | 同上 | `gatherSet_[parity]` t7 SRV | gather 态跳过 drawProbeDots → 原靠 dots require 的 UAV→SRV 收尾转换无执行点 → drawProbeGather 显式 `setTextureState(SRV)` 补转换 |
| `gatherCB_` | 128B volatile：[0..15] invVP（独立块重求逆，不复用 cb 作用域 invVP）/ [20] viewport(x,y,z,w) / [21] (res, res, baseRes, pad) / [22] params / [24..26] 间接光 albedo 0.45/0.39/0.30 / [27] MIN_WEIGHT 0.01 / [28..30] cascade 级阈 3/5/7 | `initProbe` | b0（gatherSet_） | 每帧全量覆写 |
| `csGather_` / `csGatherCompose_` | compute `probe_gather.csgather` / `probe_gather.csgathercompose`（8×8） | `initProbe` | 单一 gatherLayout_（b0 + t0..t9 + u0..u1） | dispatch 每像素 1 线程 |
| `material.slang` ambient 开关 | psMatResolve mode 0 分支：`amb = (g_options.z > 0.5) ? 0.03 : g_lightDir.w`（**probe-viz 场景渲染走 matResolve 材质管线，model.slang psMain 不参与**——首版改错文件 PNG 逐字节零差异实锤）；shadePbrGrad（mode 5）另有同款 0.03 底开关 | material.slang | — | gather 态 visbuf 路径写 `cb.options[2]=1`（**g_options.y 被 frustumCull clusterCount / params.options[0] pbr-naive 开关双重占用，禁复用**——首版误占 g_options.y 导致 pbr-direct/naive ambient 被误清零，全量回归 64125px maxDelta52 实锤）；**语义 = ambient 微底 0.03（2026-11-11 用户拍板）**：纯零替换探针 GI 撑不住暗面（单跳直光+天空、无弹射，M10.1 降级备案）→ 死黑 3.66%；0.03 底死黑 1.43%、双计费量级可忽略，S11 补弹射后收紧 |

**M10.2 gather 权重口径（UE `LumenScreenProbeGather.usf` 实读锁定）**：cascade 级选择按**线性深度**分段（sceneDepth = length(wp−eye)，阈 3/5/7）——deviceZ 分段被冒烟实锤否掉（透视非线性全落 L3 权重全灭 → 全黑）。tile 映射 = `gridF = pix·res/viewport` 连续格坐标（**csPlace 探针屏幕格距 = viewport/res_l 非 2 的幂**；用 2 的幂 stride 会越界 8×8 atlas，Texture2D.Load 越界静默返 0）。权重 = 双线性 corner × `exp2(-10000·(planeDist/sceneDepth)²)` 主权重 + `exp2(-1000·x²)` Fallback 两级降级 × `MIN_PROBE_INTERPOLATION_WEIGHT=0.01` 门限；合成 = `pow(saturate(pow(enc,2.2)+gi), 1/2.2)`（enc 已 gamma 编码需先转线性再叠加）。间接光 = albedo × 八面体出射 radiance，采样方向 = 像素法线（probeDepth 3-tap，与 probe.slang 同口径）。
#### M10.3 reference-gi / compare 槽位同步表（落码实读固化，2026-11-11，tag `s10-done`）

| 资源 / 管线 | 格式 / 入口 | 创建点 | 绑定 set（§2.7 单一） | 状态登记（§2.15/§2.19） |
|---|---|---|---|---|
| `refGICB_` | 16B volatile：giAlbedo rgb 0.45/0.39/0.30 + a=lightDir.w | `initReference` | **b5（refComputeLayout_ 独立槽，compute-only）** | 每帧全量覆写（GI 参数与 RefConstants b1 分离，见下） |
| `csReferenceGI_` | compute `direct.csrefgi`（8×8） | `initReference` | refComputeSet_（b0/b1/b4 + t2/t3/u0 + **b5**） | M9.2 渐进累积同款：rtTarget_ 线性 sum，present 归一 |
| `compareTex_` | RGBA8_UNORM（stb_image 解码，CopyDest + keepInitialState） | `loadCompareImage`（CPU 侧一次性） | compareSet_ t0 SRV + s0 sampler（compareLayout_ All visibility） | **两段式上传（M-PT2 同款 0x87d 纪律）**：建纹无 cmd list → 首帧 beginTracking(CopyDest)→writeTexture→setTextureState(SRV) 同列表 |
| `psCompare_` / `comparePSO_` | fragment `rt.pscompare`（复用 vsRtPresent_ strip） | `drawCompareImage` 懒建（probe 链也可用，device = probeDevice_ ? : rtDevice_） | compareLayout_ 单 set | **PSO 显式 disableDepthTest/Write/Stencil**；右半呈现 = viewport/scissor 裁剪（shader 无 discard，uv 直映参考图全幅） |

### 2.20 ⚠️ 跨 compute/graphics 绑定的 volatile CB 尺寸铁律（2026-11-11，M10.3 实锤）

**refConstants_（b4/b1，M9.2 跨 compute dispatch 与 graphics present 两 set 共用）从 16B 扩 32B → 双后端 device removed**（DX12 `Present failed (0x887a0005)`；Vulkan 每帧 12s 后 removed，deviceErrors=0 无任何 validation 消息，连未扩 CB 的 M9.2 旧 `--mode reference` 都稳定挂，3/3 复现）。二分定位：A 剥 RayQuery 无效、B 禁新 PSO 创建无效、**C 回 16B 绿**。修法 = RefConstants 保持 16B，GI 参数走**独立 16B compute-only CB 绑 b5**（同 DirectConstants 32B compute-only 一直安全的先例）。**推论：跨阶段（compute+graphics）共用的 volatile CB 不许扩尺寸；要加参数就开独立 CB 新槽位**。另一同族坑：`psCompare` 等 overlay 全屏 strip 的 PSO 必须显式 `depthStencilState.disableDepthTest()`——NVRHI `DepthStencilState` 默认 `depthTestEnable=true + depthFunc=Less`，swapchain framebuffer 的 depth_ 有场景残留深度时 quad 输出整体被吞（纯红实验 `red frac=0.0` 实锤「draw 提交但不可见」），blit/refpresent 前例同款纪律。
