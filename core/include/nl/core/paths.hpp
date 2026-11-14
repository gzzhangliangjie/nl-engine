#pragma once

#include <string>

// 发版打包路径解析（B1 拍板 2026-11-15）：双层策略 ——
//   1) exe 相对目录优先（发版包内可重定位）。exe 在包根 bin/<后端>/ 下，
//      两级向上到包根：
//      <exeDir>/../../shaders/<后端>  /  <exeDir>/../../assets/{models,meshlets}
//   2) 探测不到（开发树 build/<后端>/ 布局对不上包内布局）回退
//      CMake 编译期注入的绝对路径（NL_SHADER_DIR / NL_ASSET_DIR / NL_MESHLET_DIR），
//      开发树行为与旧版完全一致。
// 结果进程内缓存（首个调用点解析一次，后续零开销）。不依赖当前工作目录（既有约定）。

namespace nl::core::paths {

// exe 所在目录（无尾分隔符）。GetModuleFileNameW，失败时返回空串。
const std::string& exeDir();

// 字节码目录。后端取编译期注入 NL_BACKEND_NAME（DX12/VULKAN，与日志命名同源），
// 探测 <exeDir>/../shaders/<后端> 且目录内有文件才采用。
const std::string& shaderDir();

// 模型资产目录（CMake 注入口径 = assets/models；../efx 等 hop 在调用点拼接）。
const std::string& assetDir();

// .nmesh 资产目录（CMake 注入口径 = assets/meshlets）。
const std::string& meshletDir();

} // namespace nl::core::paths
