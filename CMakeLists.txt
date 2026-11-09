cmake_minimum_required(VERSION 3.24)
project(nl-engine VERSION 0.0.1 LANGUAGES C CXX)

# S0 / M0.2：NVRHI device + swapchain + 程序内截图。
# 搭建节奏参考 docs/s0/CARROT-BOOTSTRAP.md（Carrot 2020-11 从零起步的实况，仅作参考）。
#
# 【架构纪律】这是通用管线，构建系统不依赖任何厂商图形 SDK、也不联网拉依赖：
#   - 不读取 VULKAN_SDK / VK_SDK_PATH，不 find_package(Vulkan)，不要求 vkcube
#   - D3D12 / OpenGL 的头文件与库来自 Windows SDK（系统自带）
#   - Vulkan 头走本地 vendored 的 KhronosGroup/Vulkan-Headers；loader 是驱动自带的 vulkan-1.dll
#   - DirectX-Headers 只是 NVRHI D3D12 后端需要的开源辅助头，同样本地 vendored
#   - Slang 独立获取，不从任何 SDK 路径里找
#   viewer/ 与 core/ 里不得出现任何图形 API 符号；图形 API 只允许活在 renderer/。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 后端选择：编译期配置，切换后端不需要装任何东西 ----
set(NL_GRAPHICS_BACKEND "AUTO" CACHE STRING "Graphics backend: AUTO | DX12 | VULKAN")
set_property(CACHE NL_GRAPHICS_BACKEND PROPERTY STRINGS AUTO DX12 VULKAN)
string(TOUPPER "${NL_GRAPHICS_BACKEND}" NL_BACKEND_UPPER)
if(NL_BACKEND_UPPER STREQUAL "AUTO")
    if(WIN32)
        set(NL_BACKEND_UPPER "DX12")   # Windows 零额外依赖：头 / 库 / debug layer 全在 Windows SDK 里
    else()
        set(NL_BACKEND_UPPER "VULKAN")
    endif()
endif()
if(NOT NL_BACKEND_UPPER STREQUAL "DX12" AND NOT NL_BACKEND_UPPER STREQUAL "VULKAN")
    message(FATAL_ERROR "NL_GRAPHICS_BACKEND must be AUTO, DX12 or VULKAN")
endif()
message(STATUS "nl-engine backend: ${NL_BACKEND_UPPER} (no vendor SDK, no network)")

set(THIRDPARTY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

# ---- 本地 headers targets：NVRHI 会按这些名字检测，检测到就不走 FetchContent 联网 ----
add_library(Vulkan-Headers INTERFACE)
target_include_directories(Vulkan-Headers INTERFACE ${THIRDPARTY}/vulkan-headers/include)
add_library(Vulkan::Headers ALIAS Vulkan-Headers)

add_library(DirectX-Headers STATIC
    ${THIRDPARTY}/directx-headers/src/d3dx12_property_format_table.cpp)
target_include_directories(DirectX-Headers PUBLIC ${THIRDPARTY}/directx-headers/include)
target_include_directories(DirectX-Headers PRIVATE ${THIRDPARTY}/directx-headers/include/directx)
add_library(Microsoft::DirectX-Headers ALIAS DirectX-Headers)

add_library(DirectX-Guids STATIC
    ${THIRDPARTY}/directx-headers/src/dxguids.cpp)
target_include_directories(DirectX-Guids PUBLIC ${THIRDPARTY}/directx-headers/include)
add_library(Microsoft::DirectX-Guids ALIAS DirectX-Guids)

# ---- NVRHI：只编译当前后端，纯本地源码 ----
if(NL_BACKEND_UPPER STREQUAL "DX12")
    set(_NL_WITH_DX12 ON)
    set(_NL_WITH_VULKAN OFF)
else()
    set(_NL_WITH_DX12 OFF)
    set(_NL_WITH_VULKAN ON)
endif()
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)   # NVRHI 自带校验层，两后端都有
set(NVRHI_WITH_VULKAN ${_NL_WITH_VULKAN} CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 ${_NL_WITH_DX12} CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_DIRECTX_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/nvrhi EXCLUDE_FROM_ALL)

# 静态链接时 NVRHI 不会定义 Vulkan 动态派发器（VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE）
# 的存储与 init——它们只在 NVRHI_SHARED_LIBRARY_BUILD 时生成。我们显式补上该宏，
# 否则 nvrhi_vk 在链接（存储未定义）或运行（dispatcher 未初始化）时会崩。DX12 后端不受影响。
if(NL_BACKEND_UPPER STREQUAL "VULKAN")
    target_compile_definitions(nvrhi_vk PRIVATE NVRHI_SHARED_LIBRARY_BUILD=1)
endif()

# ---- thirdparty：GLFW 源码本地 vendored，零网络 ----
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/glfw EXCLUDE_FROM_ALL)

# ---- M0.5：ImGui 1.91.9 核心源码本地 vendored（零网络、零厂商 SDK） ----
# 只用 core 文件；官方 D3D12/Vulkan 后端不用——HUD 由 renderer/src/hud.cpp 一份
# NVRHI 共享代码绘制，避免两套裸 API 后端把图形符号散到 renderer/ 之外。
add_library(nl_imgui STATIC
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_tables.cpp
    thirdparty/imgui/imgui_widgets.cpp
)
target_include_directories(nl_imgui PUBLIC ${THIRDPARTY}/imgui)

# ---- T1：imgui-node-editor v0.9.4 vendored（MIT，MaterialX Graph Editor 同款节点 UI）----
# 只收库本体源码；依赖 nl_imgui（v1.91.9）。
add_library(nl_imgui_node_editor STATIC
    thirdparty/imgui-node-editor/imgui_node_editor.cpp
    thirdparty/imgui-node-editor/imgui_node_editor_api.cpp
    thirdparty/imgui-node-editor/imgui_canvas.cpp
    thirdparty/imgui-node-editor/crude_json.cpp
)
target_include_directories(nl_imgui_node_editor PUBLIC ${THIRDPARTY}/imgui-node-editor)
target_link_libraries(nl_imgui_node_editor PUBLIC nl_imgui)

# ---- M2.0：METIS vendored（簇聚组图分割，nanite-tools 离线管线用）----
add_subdirectory(thirdparty/metis EXCLUDE_FROM_ALL)

# ---- M2.1：meshoptimizer vendored（仅 nanite-tools 的简化对照 oracle；
# core/simplify 的 QEM 是正式实现，不依赖 meshopt）----
add_subdirectory(thirdparty/meshoptimizer EXCLUDE_FROM_ALL)

# ---- core：日志 / 断言 / 窗口 / 轨道相机 / 模式分发器 / glTF 读取 / 切簇。不依赖任何图形 API 头 ----
add_library(nl_core STATIC
    core/src/log.cpp
    core/src/assert.cpp
    core/src/window.cpp
    core/src/camera.cpp
    core/src/modes.cpp
    core/src/mesh_io.cpp       # M1.0：tinygltf 实现 TU 只在此展开（原在 scene.cpp）
    core/src/meshlet.cpp       # M1.1：meshlet 贪心切簇（参考 meshoptimizer clusterizer）
    core/src/meshlet_io.cpp    # M1.2：.nmesh 二进制资产读写
    core/src/grouping.cpp      # M2.0：簇聚组（core 里唯一碰 METIS 符号的文件）
    core/src/simplify.cpp      # M2.1：组内 QEM 简化 + 边界锁定
    core/src/lod.cpp           # M2.2：逐层 LOD 循环（聚组→简化→重切）
    core/src/lod_error.cpp     # M5.0：屏幕空间误差投影（zeux/bevy cut 公式，纯数学）
    core/src/fallback.cpp      # M3.1：fallback mesh（顶层 weld 拼接）
    core/src/anim/anim.cpp     # M-A1：动作系统（nl/anim）—— clip 采样，零图形 API
    core/src/anim/state_machine.cpp  # M-A3：动作状态机（优先级转移 + crossfade + UE 缩时）
    core/src/anim/blend_space.cpp    # M-A4：1D blend space（找段 + 权重插值，UE 1D 口径）
    core/src/anim/skeleton.cpp       # M-A4b：骨架拓扑 + FK（local -> model，纯 (T,R)）
    core/src/anim/skinning.cpp       # M-A5：CPU 蒙皮（调色板 model*IBM + LBS）
    core/src/anim/gltf_anim.cpp      # M-A4b：rigged glTF/GLB -> Skeleton + AnimClip（tinygltf 声明侧）
    core/src/jobs.cpp          # P8 M-J0：线程池 + parallel_for（A 类并行轨道，纯 CPU）
    core/src/material_graph/graph.cpp   # B 路材质图：节点图数据模型 + 序列化
    core/src/material_graph/emit.cpp    # B 路材质图：图 -> Slang 代码生成（B3）
    core/src/material_graph/compile.cpp # T1：图编译验收（emit + slangc 四路，纯进程调用）
    core/src/scene/World.cpp   # M-E0：ECS-lite World/Entity 核心（A 类并行轨道，零图形 API）
    core/src/scene/Component.cpp # M-E1：TransformComponent 层级变换（复合/写回分解）
    core/src/scene/System.cpp  # M-E2：System 实体列表维护（签名匹配，World 推送口径）
    core/src/script/Vm.cpp     # M-SC0：C# hosting 内核（hostfxr/nethost，零图形 API）
)
target_include_directories(nl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
# dotnet-hosting 三头件（hostfxr.h/nethost.h/coreclr_delegates.h，SDK 8.0.19 pack 自带
# vendored）只允许 core/src/script/Vm.cpp 出现（PIMPL 纪律，Vm.hpp 不暴露），故 PRIVATE。
target_include_directories(nl_core PRIVATE ${THIRDPARTY}/dotnet-hosting/include)
# libnethost 不可用（是静态库，与 nethost.h 默认 dllimport 语义不符 → __imp_ 符号
# 解析失败，M-SC0 踩坑）：用 nethost.lib 真导入库 + nethost.dll 运行期紧挨 exe
# （官方 hosting 样例布局）。静态库 PRIVATE 依赖以 LINK_ONLY 传给 exe 链接行。
if(MSVC)
    target_link_libraries(nl_core PRIVATE ${THIRDPARTY}/dotnet-hosting/lib/nethost.lib)
endif()
# tinygltf PUBLIC：M-E1 起组件序列化接口（nl/scene/Component.hpp）在头里暴露
# vendored nlohmann json（P8-ECS-PLAN 拍板：tinygltf 内嵌 json.hpp，零新依赖），
# 消费方（nanite-scene / M-E2 viewer）需解析 json.hpp 路径。
target_include_directories(nl_core PUBLIC ${THIRDPARTY}/tinygltf)
target_link_libraries(nl_core PRIVATE glfw nl_metis)
# 注：grouping.obj 引用 METIS 符号；viewer 不调用 groupClusters，链接器不会
# 把 grouping.obj 拉进 viewer（MSVC 静态库按引用拉 obj），故 viewer 无 METIS 负担。

# ---- renderer：所有图形 API / NVRHI 符号只活在这里 ----
add_library(nl_renderer STATIC
    renderer/src/renderer.cpp
    renderer/src/png_writer.cpp
    renderer/src/scene.cpp
    renderer/src/hud.cpp
    renderer/src/editor.cpp        # T1：材质图编辑器窗口（imgui-node-editor 接线）
    renderer/src/backend_dx12.cpp
    renderer/src/backend_vulkan.cpp
)
target_include_directories(nl_renderer
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/renderer/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src
            ${THIRDPARTY}/stb
)
target_link_libraries(nl_renderer PUBLIC nl_core PRIVATE nl_imgui nl_imgui_node_editor)
target_compile_definitions(nl_renderer PRIVATE
    NL_BACKEND_${NL_BACKEND_UPPER}=1
    NL_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/models"
    NL_MESHLET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/meshlets"   # M1.3：.nmesh 资产
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(NL_BACKEND_UPPER STREQUAL "DX12")
    target_link_libraries(nl_renderer PUBLIC nvrhi nvrhi_d3d12 d3d12 dxgi)
else()
    # NVRHI 的 Vulkan 后端用 Vulkan-Hpp 动态派发，不需要链接 vulkan-1.lib
    target_link_libraries(nl_renderer PUBLIC nvrhi nvrhi_vk)
    # Vulkan-Headers 不随 NVRHI 暴露为 PUBLIC 头，renderer 自身也要吃 vulkan.h
    target_include_directories(nl_renderer PRIVATE ${THIRDPARTY}/vulkan-headers/include)
    target_compile_definitions(nl_renderer PRIVATE
        VK_USE_PLATFORM_WIN32_KHR
        VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    )
endif()

# ---- M0.3：Slang 着色器编译（同一份源码出 SPIR-V 与 DXIL 两个目标） ----
# 纪律：零厂商 SDK。DXIL 需要的 dxcompiler.dll/dxil.dll 来自已装的 Windows SDK，不额外下载。
set(NL_SLANG_BIN "${THIRDPARTY}/slang/dist/bin")
set(SLANGC "${NL_SLANG_BIN}/slangc.exe")
if(NOT EXISTS "${SLANGC}")
    message(FATAL_ERROR "slangc not found at ${SLANGC}. Run the Slang download step first.")
endif()

# 探测 Windows SDK 里的 dxcompiler.dll（用于 DXIL 目标）。GLOB 取版本最高的那一个。
file(GLOB _dxc_list "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxcompiler.dll")
if(_dxc_list)
    list(SORT _dxc_list)            # 升序，取版本最高的 SDK
    list(GET _dxc_list -1 _dxc_dll)
    get_filename_component(NL_DXC_DIR "${_dxc_dll}" DIRECTORY)
    message(STATUS "nl-engine dxc: ${NL_DXC_DIR}")
else()
    message(FATAL_ERROR "dxcompiler.dll not found in Windows SDK — required for the DXIL target.")
endif()

# 自动收集 renderer/shaders/ 下所有 .slang（不再硬编码单个文件）。
# model.slang 当前 6 个入口（3 对 vs+ps），各出 {spv,dxil} 两份（共 12 个产物）：
#   <name>.vs.{spv,dxil}          入口 vsMain
#   <name>.vscluster.{spv,dxil}   入口 vsCluster（M1.3：多带 CLUSTERCOLOR 顶点属性）
#   <name>.vscull.{spv,dxil}      入口 vsCull（M4.0：GPU vertex pulling）
#   <name>.ps.{spv,dxil}          入口 psMain
#   <name>.pscluster.{spv,dxil}   入口 psCluster（M1.3）
#   <name>.pscull.{spv,dxil}      入口 psCull（M4.0）
# cull.slang 单 compute 入口（M4.1）：<name>.cs.{spv,dxil} 入口 csCull
file(GLOB NL_SHADER_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/*.slang")
if(NOT NL_SHADER_SRCS)
    message(FATAL_ERROR "No .slang shaders found in renderer/shaders/")
endif()

set(_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${_SHADER_OUT}")

set(_SHADER_ARTIFACTS "")
# 入口清单按源文件给：model.slang 双入口（M1.3 cluster 场景），其余单入口。
# D3D12 debug layer 对「VS 签名有但 input layout 不提供」的属性报 error，
# 所以 normal 场景必须用不带 CLUSTERCOLOR 的 vsMain/psMain 对，两个场景各用各的入口。
set(_ENTRIES_MODEL "vsMain|vs|vertex" "vsCluster|vscluster|vertex" "vsCull|vscull|vertex"
                   "psMain|ps|fragment" "psCluster|pscluster|fragment" "psCull|pscull|fragment")
set(_ENTRIES_DEFAULT "vsMain|vs|vertex" "psMain|ps|fragment")
# M4.1：cull.slang 单 compute 入口（GPU 视锥剔除 pass）。
set(_ENTRIES_CULL "csCull|cs|compute")
# M5.1：dag.slang 单 compute 入口（DAG 遍历选层，显式栈）。
set(_ENTRIES_DAG "csDag|cs|compute")
# M6.0：hiz.slang 四入口（深度预通道 vs/ps + 金字塔 min 降采样两个 compute +
# 层级可视化 vs/ps）。两个 compute 入口各出一份产物（hiz.cs / hiz.cstop）。
set(_ENTRIES_HIZ "vsHizDepth|vshizdepth|vertex" "psHizDepth|pshizdepth|fragment"
                 "csHizDown|cs|compute" "csHizTop|cstop|compute"
                 "vsHizViz|vshizviz|vertex" "psHizViz|pshizviz|fragment")
foreach(_SRC ${NL_SHADER_SRCS})
    get_filename_component(_SRC_NAME "${_SRC}" NAME_WE)   # 源文件名（无扩展名），如 "triangle"
    if(_SRC_NAME STREQUAL "model")
        set(_ENTRIES "${_ENTRIES_MODEL}")
    elseif(_SRC_NAME STREQUAL "cull")
        set(_ENTRIES "${_ENTRIES_CULL}")
    elseif(_SRC_NAME STREQUAL "dag")
        set(_ENTRIES "${_ENTRIES_DAG}")
    elseif(_SRC_NAME STREQUAL "hiz")
        set(_ENTRIES "${_ENTRIES_HIZ}")
    else()
        set(_ENTRIES "${_ENTRIES_DEFAULT}")
    endif()
    foreach(_SPEC ${_ENTRIES})
        string(REPLACE "|" ";" _SPEC_LIST "${_SPEC}")
        list(GET _SPEC_LIST 0 _entry)
        list(GET _SPEC_LIST 1 _suffix)
        list(GET _SPEC_LIST 2 _stage)
        list(APPEND _SHADER_ARTIFACTS "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.spv"
                                      "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.dxil")
        # SPIR-V 目标必须按 NVRHI 的默认 VulkanBindingOffsets 平移寄存器：
        #   b0 -> binding 256, t0 -> binding 0, s0 -> binding 128, u0 -> binding 384（set 0）。
        #   -fvk-u-shift 对无 UAV 的 shader 是无操作，统一加免去逐文件特判。
        # DX12 侧仍用 b0/t0/s0/u0 各自的 slot 0，故同一份 .slang 无需任何 GLSL 注解。
        add_custom_command(OUTPUT "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.spv"
            COMMAND "${SLANGC}" "${_SRC}" -target spirv -entry ${_entry} -stage ${_stage}
                    -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-u-shift 384 0
                    -o "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.spv"
            MAIN_DEPENDENCY "${_SRC}"
            COMMENT "Slang -> SPIR-V (${_SRC_NAME}.${_suffix})")
        add_custom_command(OUTPUT "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.dxil"
            COMMAND "${SLANGC}" "${_SRC}" -dxc-path "${NL_DXC_DIR}"
                    -target dxil -entry ${_entry} -stage ${_stage}
                    -o "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.dxil"
            MAIN_DEPENDENCY "${_SRC}"
            COMMENT "Slang -> DXIL (${_SRC_NAME}.${_suffix})")
    endforeach()
endforeach()

# 改 shader 自动重编字节码；ALL 保证每次 build 都检查过期
add_custom_target(nl_shaders ALL DEPENDS ${_SHADER_ARTIFACTS})
# renderer 按编译期注入的 NL_SHADER_DIR 绝对路径读取字节码，不依赖 CWD
target_compile_definitions(nl_renderer PRIVATE NL_SHADER_DIR="${_SHADER_OUT}")

# ---- viewer：可见用例 + 程序内截图（--screenshot） ----
add_executable(viewer viewer/main.cpp)
target_link_libraries(viewer PRIVATE nl_core nl_renderer glfw)
# shader 字节码先于 viewer 生成（viewer 运行时要读 NL_SHADER_DIR 下的 .spv/.dxil）
add_dependencies(viewer nl_shaders)

# ---- M1.0：nanite-tools —— S1 离线网格工具（cluster / simplify / dag / stat） ----
# 只链 nl_core（日志 + mesh_io），零图形 API；不依赖 renderer / viewer。
# M2.1：另链 meshoptimizer —— 仅 simplify 子命令的对照 oracle（打印对照行），
# core 自身不依赖 meshopt；viewer 不链它。
add_executable(nanite-tools nanite-tools/main.cpp)
target_link_libraries(nanite-tools PRIVATE nl_core meshoptimizer)

# ---- M-A1：anim-tools —— 动作系统（A 类并行轨道）自测工具，只链 nl_core ----
# 退出码约定：0=PASS / 1=断言失败 / 2=用法错误（与展示台 expectExit 0/1/2 对齐）。
add_executable(anim-tools anim-tools/main.cpp)
target_link_libraries(anim-tools PRIVATE nl_core)

# ---- B 路材质图工具（A 类轨道，工具版交代）----
# 第 2 步种子：节点图数据模型 + 稳定序列化。只链 nl_core（日志），零图形 API。
add_executable(material-graph-tools material-graph-tools/main.cpp)
target_link_libraries(material-graph-tools PRIVATE nl_core)

# ---- M-E0：nanite-scene —— ECS-lite 场景（nl/scene）自测工具，A 类并行轨道 ----
# 退出码约定：0=PASS / 1=断言失败 / 2=用法错误（与展示台 expectExit 0/1/2 对齐）。
add_executable(nanite-scene nanite-scene/main.cpp)
target_link_libraries(nanite-scene PRIVATE nl_core)

# ---- M-SC0：nl-script —— C# hosting（nl/script）自测工具，A 类并行轨道 ----
# 退出码约定：0=PASS / 1=断言失败或脚本运行失败 / 2=用法错误（与展示台 expectExit 0/1/2 对齐）。
# nethost.dll 运行期紧挨 exe（官方 hosting 样例布局：链 libnethost 导入库 + copy dll）。
add_executable(nl-script nl-script/main.cpp)
target_link_libraries(nl-script PRIVATE nl_core)
add_custom_command(TARGET nl-script POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${THIRDPARTY}/dotnet-hosting/bin/nethost.dll $<TARGET_FILE_DIR:nl-script>)

if(MSVC)
    # NVRHI 上游头文件带少量非当前代码页字符，压掉 C4819 噪音（不影响代码）
    add_compile_options(/wd4819)
    foreach(t nl_core nl_renderer viewer nanite-tools anim-tools material-graph-tools nanite-scene nl-script)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8 /bigobj)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endforeach()
endif()

# 无窗口、无 GPU 的机械验收用例：CI 里 `ctest` 直接跑
enable_testing()
add_test(NAME m0.5-selftest COMMAND viewer --selftest)
# M1.0：nanite-tools 对官方 Suzanne 资产的机械验收（顶点 / 三角 / 子网格数）
add_test(NAME s1-tools-stat
         COMMAND nanite-tools stat ${CMAKE_CURRENT_SOURCE_DIR}/assets/models/Suzanne.glb)
set_tests_properties(s1-tools-stat PROPERTIES
    PASS_REGULAR_EXPRESSION "triangles : 3936")
# M1.1：切簇算法机械单测（覆盖率 / 128 tri 与 64 vert 上限 / bounds 合理性 / 负向输入）
add_test(NAME s1-cluster-selftest COMMAND nanite-tools selftest cluster)
set_tests_properties(s1-cluster-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "cluster selftest: PASS")
# M1.1：真实资产切簇验收（覆盖率 100% + limits PASS）
add_test(NAME s1-cluster-suzanne
         COMMAND nanite-tools cluster ${CMAKE_CURRENT_SOURCE_DIR}/assets/models/Suzanne.glb)
set_tests_properties(s1-cluster-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "limits     : PASS")
# M1.2：.nmesh roundtrip 单测（字段一致 + 逐字节重写一致 + 坏 magic / 截断拒绝）
add_test(NAME s1-nmesh-roundtrip COMMAND nanite-tools selftest nmesh)
set_tests_properties(s1-nmesh-roundtrip PROPERTIES
    PASS_REGULAR_EXPRESSION "nmesh selftest: PASS")
# M1.2：真实资产写盘 → stat 读回（-cluster -o 与 .nmesh stat 的端到端验收）
add_test(NAME s1-nmesh-suzanne
         COMMAND nanite-tools cluster ${CMAKE_CURRENT_SOURCE_DIR}/assets/models/Suzanne.glb
                 -o ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh)
set_tests_properties(s1-nmesh-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "saved      :")
# M2.0：聚组算法机械单测（覆盖连续 / 组号连续 / 组内连通交叉验证 / 断连块不跨组 / 负向）
add_test(NAME s2-group-selftest COMMAND nanite-tools selftest group)
set_tests_properties(s2-group-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "group selftest: PASS")
# M2.0：真实资产聚组验收（Suzanne.nmesh 由 s1-nmesh-suzanne 先写出；依赖同目录测试顺序）
add_test(NAME s2-group-suzanne
         COMMAND nanite-tools simplify ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh)
set_tests_properties(s2-group-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "connectivity: PASS"
    DEPENDS s1-nmesh-suzanne)
# M2.1：QEM 简化 + 边界锁定机械单测（边界点集合交叉验证 / 边界点逐点位级不变 /
# 接缝边双侧存活 / 简化率 / 确定性重跑 / 负向拒绝）
add_test(NAME s2-qem-selftest COMMAND nanite-tools selftest qem)
set_tests_properties(s2-qem-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "qem selftest: PASS")
# M2.1：真实资产 QEM 简化验收（Suzanne 3936 tri → ~50%，边界锁定 + meshopt oracle 对照）
add_test(NAME s2-qem-suzanne
         COMMAND nanite-tools simplify ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh -v)
set_tests_properties(s2-qem-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "oracle      : meshopt LockBorder"
    DEPENDS s1-nmesh-suzanne)
# M2.2：逐层 LOD 循环机械单测（每层边界独立重算 / 约减半 / parent error 单调 / 簇上限 / v1-v2 资产兼容 / 确定性）
add_test(NAME s2-loop-selftest COMMAND nanite-tools selftest loop)
set_tests_properties(s2-loop-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "loop selftest: PASS")
# M2.2：真实资产逐层落盘（L0..LN，每层 .nmesh v2 带 lockedPositions）
add_test(NAME s2-loop-suzanne
         COMMAND nanite-tools lod ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh
                 -o ${CMAKE_CURRENT_BINARY_DIR}/lod-suzanne --max-levels 6)
set_tests_properties(s2-loop-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "lod files   : "
    DEPENDS s1-nmesh-suzanne)
# M3.0：DAG 血缘机械单测（全链不变量 / 顶层 terminal / 按组重切产出 / v3 roundtrip / 篡改检测）
add_test(NAME s3-dag-selftest COMMAND nanite-tools selftest dag)
set_tests_properties(s3-dag-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "dag selftest: PASS")
# M3.0：真实资产 DAG 装配 + 校验（每父簇恰一生成组 / error 链逐位精确 / 无环）
add_test(NAME s3-dag-suzanne
         COMMAND nanite-tools dag ${CMAKE_CURRENT_BINARY_DIR}/lod-suzanne)
set_tests_properties(s3-dag-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "dag         : OK"
    DEPENDS s2-loop-suzanne)

# M3.1：fallback 拼接自检（weld 收敛、三角守恒、边流形 ≤2、v3 roundtrip）
add_test(NAME s3-fallback-selftest COMMAND nanite-tools selftest fallback)
set_tests_properties(s3-fallback-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "fallback selftest: PASS")

# M3.1：真实资产 fallback 落盘（写进 lod0 尾段，输入文件不动）
add_test(NAME s3-fallback-suzanne
         COMMAND nanite-tools lod ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh
                 -o ${CMAKE_CURRENT_BINARY_DIR}/fb-suzanne --max-levels 6)
set_tests_properties(s3-fallback-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "fallback    : "
    DEPENDS s1-nmesh-suzanne)

# ---- M-A 轨道（A 类并行：动作系统，工具版交代，不进渲染回归总闸） ----
# M-A1：关键帧采样机械单测（lerp/slerp 锚点、clamp、Step、常量、wrap、validate 负向、确定性）
add_test(NAME ma1-anim-curve-selftest COMMAND anim-tools selftest curve)
set_tests_properties(ma1-anim-curve-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim curve selftest: PASS")
# M-A1：负向——未知 selftest 用例显式拒绝（exit 2）
add_test(NAME ma1-anim-unknown-case COMMAND anim-tools selftest bogus)
set_tests_properties(ma1-anim-unknown-case PROPERTIES WILL_FAIL TRUE)
# M-A2：线性/加法混合机械单测（lerp 锚点 w=0/0.5/1、加法 quat 相对叠加语义、骨架不一致拒绝）
add_test(NAME ma2-anim-blend-selftest COMMAND anim-tools selftest blend)
set_tests_properties(ma2-anim-blend-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim blend selftest: PASS")
# M-A3：状态机机械单测（优先级取首个、crossfade 权重曲线、UE 回退缩时+姿态连续、
# 10 步确定性场景逐值断言、validate 负向、两次运行一致）
add_test(NAME ma3-anim-statemachine-selftest COMMAND anim-tools selftest statemachine)
set_tests_properties(ma3-anim-statemachine-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim statemachine selftest: PASS")
# M-A4：1D blend space 机械单测（段权重锚点、域外 clamp、单 sample 退化、
# validate 负向、参数域扫掠两次运行逐位一致）
add_test(NAME ma4-anim-blendspace-selftest COMMAND anim-tools selftest blendspace)
set_tests_properties(ma4-anim-blendspace-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim blendspace selftest: PASS")
# M-A4b：骨架 FK + rigged glTF 入口机械单测（FK 手算锚点、sway t=0.5 世界位置、
# wrap 循环、两次加载逐位一致、缺文件/scale/拓扑序负向）
add_test(NAME ma4b-anim-skeleton-selftest
         COMMAND anim-tools selftest skeleton
                 ${CMAKE_CURRENT_SOURCE_DIR}/assets/models/RiggedChain.glb)
set_tests_properties(ma4b-anim-skeleton-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim skeleton selftest: PASS")

# ---- M-A5：蒙皮确定性自测（刚体/LBS 手算锚点 + CesiumMan rest 调色板≈identity）----
add_test(NAME ma5-anim-skinning-selftest
         COMMAND anim-tools selftest skinning
                 ${CMAKE_CURRENT_SOURCE_DIR}/assets/models/rigged/CesiumMan.glb)
set_tests_properties(ma5-anim-skinning-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim skinning selftest: PASS")

# ---- B 路材质图：节点图数据模型 + 序列化机械单测（A 类轨道第 2 步验收）----
# 不进渲染回归总闸（bytes/near），走工具版交代。
add_test(NAME b2-graph-selftest COMMAND material-graph-tools selftest)
set_tests_properties(b2-graph-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "material-graph selftest: PASS")

# ---- B 路材质图 B3：图 -> Slang 代码生成 + 双后端编译验收（A 类轨道）----
# 走工具版交代：emit 示例图 -> slangc 编译 SPIR-V + DXIL 各 vs/ps 入口。
add_test(NAME b3-graph-genslang COMMAND material-graph-tools check)
set_tests_properties(b3-graph-genslang PROPERTIES
    PASS_REGULAR_EXPRESSION "genslang   : OK"
    ENVIRONMENT "NL_SLANG_BIN=${NL_SLANG_BIN};NL_DXC_DIR=${NL_DXC_DIR}")

# ---- B 路材质图 B4：热重载（改图即重编译）----
# watch --once 对 fixture 图做单次 recompile（emit + 双后端编译），验收热重载链路本身；
# 与 check 同源，只是走 watch 的加载 + 轮询首轮路径。不进渲染总闸，走工具版交代。
add_test(NAME b4-graph-hot-reload
         COMMAND material-graph-tools watch
                 ${CMAKE_CURRENT_SOURCE_DIR}/material-graph-tools/sample.mg --once)
set_tests_properties(b4-graph-hot-reload PROPERTIES
    PASS_REGULAR_EXPRESSION "OK"
    DEPENDS b3-graph-genslang
    ENVIRONMENT "NL_SLANG_BIN=${NL_SLANG_BIN};NL_DXC_DIR=${NL_DXC_DIR}")

# ---- M-E0：ECS-lite World/Entity 核心机械单测（A 类轨道，P8-ECS-PLAN.md 判据）----
# 不进渲染回归总闸（bytes/near），走工具版交代。
add_test(NAME e0-ecs-core-selftest COMMAND nanite-scene selftest --suite ecs-core)
set_tests_properties(e0-ecs-core-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "ecs-core selftest: PASS")
# 负向：未知 suite = 用法错误退出码 2
add_test(NAME e0-ecs-unknown-suite COMMAND nanite-scene selftest --suite bogus)
set_tests_properties(e0-ecs-unknown-suite PROPERTIES WILL_FAIL TRUE)

# ---- M-E1：组件 + 层级机械单测（数值锚 = 手算对照，P8-ECS-PLAN.md 判据）----
add_test(NAME e1-ecs-hier-selftest COMMAND nanite-scene selftest --suite ecs-hier)
set_tests_properties(e1-ecs-hier-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "ecs-hier selftest: PASS")

# ---- M-E2：System + 场景 roundtrip（P8-ECS-PLAN.md 判据）----
add_test(NAME e2-ecs-system-selftest COMMAND nanite-scene selftest --suite ecs-system)
set_tests_properties(e2-ecs-system-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "ecs-system selftest: PASS")
add_test(NAME e2-ecs-serial-selftest COMMAND nanite-scene selftest --suite ecs-serial)
set_tests_properties(e2-ecs-serial-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "ecs-serial selftest: PASS")
# 负向：非法 JSON / 未知组件名 = 显式退出码 2（M-E2 判据原样落地）
add_test(NAME e2-ecs-badjson-exit2 COMMAND nanite-scene deserialise-check badjson)
set_tests_properties(e2-ecs-badjson-exit2 PROPERTIES
    PASS_REGULAR_EXPRESSION "rejected")
add_test(NAME e2-ecs-unknown-comp-exit2 COMMAND nanite-scene deserialise-check unknown)
set_tests_properties(e2-ecs-unknown-comp-exit2 PROPERTIES
    PASS_REGULAR_EXPRESSION "rejected")
add_test(NAME e2-ecs-good-scene COMMAND nanite-scene deserialise-check good)
set_tests_properties(e2-ecs-good-scene PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS")
