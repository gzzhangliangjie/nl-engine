# Shader 编写规范 + Slang 导出流程

> 适用范围：M0.3 起所有图形 shader。`renderer/shaders/` 下的 `.slang` 源码与 `CMakeLists.txt` 的 Slang 编译块。
> 关联：`docs/s0/S0-PLAN.md` M0.3 目标 · `docs/MILESTONES.md` M0.3 台账。
> 纪律：零厂商 SDK。Slang 独立获取（vendored 到 `thirdparty/slang/dist/`，已 gitignore，不入仓库），DXIL 所需的 `dxcompiler.dll` 来自已装的 Windows SDK，**不额外下载任何东西**。

---

## 1. 源码编写规范

### 1.1 单一真相源（single source of truth）
- 一个渲染效果 = **一份** `.slang`，放在 `renderer/shaders/`。
- 同一份源码由 `slangc` 编译出 **SPIR-V（Vulkan）** 与 **DXIL（DX12）** 两个目标，两后端各自加载——这是通用管线的验收核心，不是「顺带试一次」。
- 语法用 **HLSL 兼容写法**，不要写 GLSL / SPIR-V 专用注解（`[[spirv::...]]` 之类）。保持后端无关。
- 命名：`<effect>.slang`（当前：`triangle.slang`）。

### 1.2 入口点与语义约定
| 用途 | 入口函数名 | NVRHI ShaderType | 关键输出语义 |
|---|---|---|---|
| 顶点 | `vsMain` | `ShaderType::Vertex` | `float4 position : SV_Position` |
| 片元 | `psMain` | `ShaderType::Pixel` | `float4 : SV_Target` |

- 顶点输入结构体用 HLSL 语义标注，当前约定：
  ```hlsl
  struct VSInput  { float3 position : POSITION;  float3 color : COLOR; };
  struct VSOutput { float4 position : SV_Position; float3 color : COLOR; };
  ```
- **⚠️ shader 里的顶点输入布局（`POSITION` / `COLOR` 的 slot、格式、步长）必须与 backend 侧 C++ 的 `InputLayoutDesc` 完全一致**，否则会出现「能编译但顶点错乱 / 黑屏」。改任一侧都要同步另一侧。

### 1.3 新增一个 shader 的步骤
1. 在 `renderer/shaders/` 新增 `<name>.slang`，遵守 1.1 / 1.2。
2. **无需改 CMake**（`file(GLOB renderer/shaders/*.slang)` 自动收集，见 §3）。新增后重跑 build 脚本触发重新 configure 并生成 `<name>.{vs,ps}.{spv,dxil}`。
3. backend 侧用 `NL_SHADER_DIR` 读字节码，按后端分别 `setEntryName`（见 §2.4）；注意当前 backend 仍硬编码加载 `triangle.*`（M0.4 材质系统前不改）。
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
每个源 `<name>.slang` 出 `<name>.{vs,ps}.{spv,dxil}` 四个文件（`vs`/`ps` × `spv`/`dxil`）。`<name>` = 源文件名（去扩展名），当前即 `triangle`：

```bash
# SPIR-V（Vulkan）
slangc <name>.slang -target spirv -entry <vsMain|psMain> -stage <vertex|fragment> -o <name>.vs.spv

# DXIL（DX12）—— 必须带 -dxc-path 指向 Windows SDK 的 x64 目录
slangc <name>.slang -dxc-path <WinSDK_x64_dir> -target dxil -entry <vsMain|psMain> -stage <vertex|fragment> -o <name>.vs.dxil
```


- `-stage` 取值：`vertex` / `fragment`（M0.3 用到这两个；mesh/compute 后续阶段再加）。
- `-entry` 取值与 1.2 入口名一致（`vsMain` / `psMain`）。

### 2.3 产物落盘与注入
- 输出目录：`${CMAKE_BINARY_DIR}/shaders` → 实际 `build/<backend>/shaders/`。
- CMake 用自定义 target `nl_shaders`（带 `ALL`）在 build 时生成；改 shader 源码会自动重编。
- 产物绝对路径通过编译宏注入 C++：`target_compile_definitions(nl_renderer PRIVATE NL_SHADER_DIR="${_SHADER_OUT}")`。
- backend 用 `readFileBytes(NL_SHADER_DIR "/<name>.<vs|ps>.<spv|dxil>")` 读字节码，再 `device->createShader(desc, blob, size)`。**不依赖进程 CWD**，避免运行时路径问题。当前 `<name>` = `triangle`。
- `viewer` 通过 `add_dependencies(viewer nl_shaders)` 保证运行时字节码已生成。

### 2.4 ⚠️ 致命坑：SPIR-V 入口点永远叫 `main`
**Slang 编译 SPIR-V 时，入口点名恒为 `main`，完全无视 `-entry` 参数；DXIL 才真正按 `-entry` 命名。**

后果：
- Vulkan 侧若把 `ShaderDesc.setEntryName("vsMain")`，NVRHI 在 SPIR-V 里找不到 `vsMain` 入口 → **`createShader` 静默返回 NULL** → pipeline 建不出来 → 三角形全黑、调试层可能零报错，极难定位。
- 正确做法（已固化在代码注释里）：
  ```cpp
  // backend_vulkan.cpp —— SPIR-V 入口固定为 "main"
  vsDesc.setShaderType(ShaderType::Vertex).setEntryName("main");
  psDesc.setShaderType(ShaderType::Pixel ).setEntryName("main");

  // backend_dx12.cpp —— DXIL 认 -entry，用真实入口名
  vsDesc.setShaderType(ShaderType::Vertex).setEntryName("vsMain");
  psDesc.setShaderType(ShaderType::Pixel ).setEntryName("psMain");
  ```
- **自检字节码合法性**（当初就是靠这个定位的）：
  - `.dxil` 文件头魔数 `44 58 42 43`（ASCII `DXBC`）。
  - `.spv`  文件头魔数 `03 02 23 07`（SPIR-V）。
  - 用 `xxd` / 二进制查看器核对；若 Vulkan 端字节数与 DX12 端差异过大（如差一截），优先怀疑入口名/ stage 不匹配。

---

## 3. 当前局限（M0.4 前待办）
- **CMake 已改为 `file(GLOB renderer/shaders/*.slang)` 自动收集**：新增 shader 只需在 `renderer/shaders/` 放一份 `<name>.slang`，无需改 CMake，重新跑 build 脚本即触发重新 configure 并生成 `<name>.{vs,ps}.{spv,dxil}`。注意 `file(GLOB)` 在 configure 时求值，纯新增文件（不碰 CMakeLists）时若没触发重编，手动重跑一次 build 脚本即可（不靠编辑 CMakeLists 的"触碰"技巧）。
- **backend 仍按固定名加载**：`triangle.*` 目前硬编码在 `backend_*::initTriangle` 的 path 拼接里（DX12 / Vulkan 各一处）。M0.4 起应抽成「shader 名 → 产物路径 + 入口名」的注册表/材质系统，按 pipeline 选 shader，而不是写死 `triangle`。
- 多 pass / 多 render target 的 shader 尚未涉及，`-stage` 目前只用 `vertex`/`fragment`。

## 4. 验收自检
```bash
scripts/build-m0.2.bat DX12        # 或 VULKAN
build/<backend>/viewer.exe --screenshot out.png
```
- 两后端都渲染出顶点着色三角形（上红 / 左下蓝 / 右下绿，深蓝底），调试层零输出。
- 改 shader → 重新 build 自动重编字节码，无需手动跑 slangc。
