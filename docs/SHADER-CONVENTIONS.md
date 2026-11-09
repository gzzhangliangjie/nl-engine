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

### 2.6 M0.8 调试着色与线框的接法（2026-11-07）
- **着色模式走 cbuffer 分支，不走多套 shader**：`g_options.x` = 0 textured / 1 normal / 2 uv / 3 tri-id，一个 `psMain` 里 `if` 分支。四种模式共用同一对 vs/ps 字节码，pipeline 不感知着色模式。
- **三角形 ID 用 `SV_PrimitiveID`，PS 直接收、VS 不用传**：rasterizer 生成的系统值，DXIL 侧签名匹配允许注入，SPIR-V 侧对应 `gl_PrimitiveID` 内建（fragment 输入合法）。**slangc 双目标实测通过**（2026-11-07，SPIR-V `03 02 23 07` / DXIL `DXBC`），NVRHI validation 与 D3D12 Debug Layer 零报错。颜色用整数 hash（pcg 风格），无浮点随机源 → 同参重渲染 PNG 确定，可进截图回归。
- **线框不是 shader**：NVRHI `RasterState::setFillWireframe()`（光栅 fill mode），零几何 shader、零索引改动。Scene 的 pipeline 组合因此从 2 套扩到 **4 套（solid/wire × cull back/none）**，着色模式与线框正交。
- **FrameParams 尾部新增 `int shading / bool wireframe`**，同样不进 cbuffer 整块拷贝：`Scene::draw` 只把 `shading` 写进 `options[0]`，其余 options 原样带过。
