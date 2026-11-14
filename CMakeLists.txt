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

# ---- M-CRASH0：Release 也产 PDB（崩溃 dump 符号化的硬前提）----
# 纯 Release 默认无 /Zi /DEBUG → exe 一直更新、PDB 停在旧配置（实锤：build/*/nanite-tools.pdb
# 停在 11-09，而 exe 每轮重链）。/Zi 只进 obj 调试段、/DEBUG 只在链接期产 PDB，
# **不改代码生成** → GPU 基线 / 输出零扰动。代价 = 本轮一次全量重编。
if(MSVC)
    string(APPEND CMAKE_C_FLAGS_RELEASE " /Zi")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " /Zi")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
endif()

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
# M-TR0：Tracy v0.15.0（BSD-3-Clause）性能/内存观测，**默认 OFF**。
# 选型闸与 0.15 实测口径见 thirdparty/tracy/VENDOR.md；GUI 是进程外工具，不入库。
# OFF 时只留一个 INTERFACE 头目标（nl/core/profiler.hpp 的宏为空转，连第三方头都不 include）；
# ON 时编单个伞形翻译单元 TracyClient.cpp 并把 TRACY_ENABLE 传给所有消费者。
option(NL_TRACY "Enable Tracy v0.15.0 profiling instrumentation" OFF)
add_library(nl_profiler INTERFACE)
if(NL_TRACY)
    add_library(tracy_client STATIC ${THIRDPARTY}/tracy/public/TracyClient.cpp)
    target_compile_features(tracy_client PUBLIC cxx_std_11)
    target_include_directories(tracy_client SYSTEM PUBLIC ${THIRDPARTY}/tracy/public)
    # PUBLIC 是硬要求：所有 include Tracy.hpp 的 TU 必须看到同一个 TRACY_ENABLE，
    # 否则 client 库与业务侧宏面不一致（上游集成同样用 PUBLIC）。
    target_compile_definitions(tracy_client PUBLIC TRACY_ENABLE)
    # Windows 系统库由 TracyClient.cpp 内 #pragma comment(lib, ...) 自动链，不额外加。
    if(MSVC)
        target_compile_options(tracy_client PRIVATE /utf-8 /W0 /bigobj)
    endif()
    target_compile_definitions(nl_profiler INTERFACE NL_TRACY=1)
    target_link_libraries(nl_profiler INTERFACE tracy_client)
    message(STATUS "nl-engine profiling: Tracy v0.15.0 ENABLED (opt-in)")
else()
    message(STATUS "nl-engine profiling: Tracy DISABLED (default, zero cost)")
endif()
# 仓库根绝对路径：子目录 CMakeLists 里引用 assets/ 等根级路径统一用它
# （2026-11-10 工具 target 拆分后，子目录的 CMAKE_CURRENT_SOURCE_DIR 不再是仓库根）
set(NL_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ---- Taskflow v3.11.0（MIT / header-only / C++17）：M-J5a 显式依赖 DAG ----
# 选型闸与风险核查见 thirdparty/taskflow/VENDOR.md；cuda/ 未入库，无 CUDA 依赖。
# 只用 INTERFACE 目标暴露 include 路径：不编源、不产库、不联网、不新增 DLL。
add_library(taskflow INTERFACE)
# 上游布局：include root 是仓库根（vendored 后 = ${THIRDPARTY}），
# 业务侧写 #include <taskflow/taskflow.hpp>；不可用 .../taskflow 本身作 root。
target_include_directories(taskflow INTERFACE ${THIRDPARTY})

# ---- spdlog v1.17.0（MIT / 编译型 + bundled fmt）：A 类日志轨道 M-LOG0 内核 ----
# 选型闸与风险核查见 thirdparty/spdlog/VENDOR.md；tests/bench/example 未入库。
# SPDLOG_COMPILED_LIB 必须 PUBLIC：消费方（nl_core log.cpp）走同一套非 header-only
# 符号面，防止头内 inline 定义与库内定义重复。
add_library(nl_spdlog STATIC
    thirdparty/spdlog/src/spdlog.cpp
    thirdparty/spdlog/src/async.cpp
    thirdparty/spdlog/src/cfg.cpp
    thirdparty/spdlog/src/color_sinks.cpp
    thirdparty/spdlog/src/file_sinks.cpp
    thirdparty/spdlog/src/stdout_sinks.cpp
    thirdparty/spdlog/src/bundled_fmtlib_format.cpp
)
target_include_directories(nl_spdlog PUBLIC ${THIRDPARTY}/spdlog/include)
target_compile_definitions(nl_spdlog PUBLIC SPDLOG_COMPILED_LIB)
if(MSVC)
    # fmt bundled：Unicode 支持强制 /utf-8（base.h 静态断言），对齐 nl_core 同款口径。
    target_compile_options(nl_spdlog PRIVATE /utf-8)
endif()

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

# ---- M-E5：imgui 共享 DLL（编辑器宿主与插件 DLL 必须共享同一 ImGuiContext）----
# nl_imgui(STATIC) 保留给 renderer/viewer（不同进程，各持一份无碍）；editor.exe
# 与 plugins/*.dll 全部链接本目标——若插件 DLL 静链一份 imgui 会产生第二个
# ImGuiContext，宿主绘制/插件绘制互相看不到对方的状态，属必炸路径。
# WINDOWS_EXPORT_ALL_SYMBOLS = MSVC 自动生成 .def 导出全部符号，免逐个 dllexport。
add_library(nl_imgui_dll SHARED
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_tables.cpp
    thirdparty/imgui/imgui_widgets.cpp
)
target_include_directories(nl_imgui_dll PUBLIC ${THIRDPARTY}/imgui)
set_target_properties(nl_imgui_dll PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

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

# ---- P8 M-S0：miniaudio v0.11.26 vendored（Public Domain / MIT-0 二选一，本项目选 PD）----
# 单头库：MINIAUDIO_IMPLEMENTATION 只在 thirdparty/miniaudio/miniaudio.c 这一个 TU 展开。
# nl/audio 只 include 声明头；零外部依赖、纯 C、不动双后端图形链。
add_library(nl_miniaudio STATIC thirdparty/miniaudio/miniaudio.c)
target_include_directories(nl_miniaudio PUBLIC ${THIRDPARTY}/miniaudio)

# ---- P8 M-P0：Jolt Physics 5.3.1 vendored（纯 CPU，零图形 SDK / 零联网）----
# 上游 Build/CMakeLists 作子目录接入；samples/tests 只在它是根项目时启用。
# 统一 CRT / RTTI / 异常口径：默认是 /MT 静态 CRT，会和 nl_core/viewer 的 /MD
# 混链打架（Carrot 同样显式关掉）。详见 thirdparty/jolt/VENDOR.md。
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/jolt/Build EXCLUDE_FROM_ALL)

# ---- P8 M-N0：Recast/Detour 1.6.0 vendored（纯 CPU，零图形 SDK / 零联网）----
# 自写薄脚本接（上游模块 CMakeLists 带 SOVERSION/install 且 DebugUtils 依赖
# 未入库的 DetourTileCache）。版本锁 / 子集 / UE 1.4.2 口径差异见
# thirdparty/recastnavigation/VENDOR.md。
add_subdirectory(thirdparty/recastnavigation EXCLUDE_FROM_ALL)

# ---- PT M-PT1：Effekseer core vendored（MIT，.efkefc 解析层，离线转换器专用）----
# 单静态库零外部依赖（Network 子集未入库）；运行时（nl-engine）不碰它，只有
# vfx-tools/nfx-convert 链接——运行时零新依赖铁律不破。详见 thirdparty/effekseer/VENDOR.md。
add_subdirectory(thirdparty/effekseer EXCLUDE_FROM_ALL)

# ---- NET M-NET0：ENet 1.3.18 vendored（MIT，UDP 传输：可靠/不可靠通道 + 连接管理）----
# 符号面隔离：enet_ 类型只允许出现在 core/src/net/Net.cpp（PIMPL）；
# 版本锁 / 子集 / 风险核查见 thirdparty/enet/VENDOR.md。
add_subdirectory(thirdparty/enet EXCLUDE_FROM_ALL)

# ---- core：日志 / 断言 / 窗口 / 轨道相机 / 模式分发器 / glTF 读取 / 切簇。不依赖任何图形 API 头 ----
add_library(nl_core STATIC
    core/src/log.cpp
    core/src/assert.cpp
    core/src/window.cpp
    core/src/input.cpp         # P8 M-I0：输入状态层（三态 + 鼠标全量 + 注入 API，A 类轨道）
    core/src/action_map.cpp    # P8 M-I1：ActionMap/ActionSet 映射层（Carrot 解析规则，纯逻辑零 GLFW）
    core/src/camera.cpp
    core/src/camera_rig.cpp    # P8 M-CAM0：CameraRig 基座（相机族共享矩阵口径 + 正交/透视）
    core/src/chase_camera.cpp  # P8 M-CAM0：ChaseCamera 跟车（阻尼/boom/look-ahead，纯 CPU）
    core/src/free_camera.cpp   # P8 M-CAM0：FreeCamera 自由飞行（无阻尼直出，纯 CPU）
    core/src/camera_director.cpp  # P8 M-CAM1a：相机接线封装层（spectator 映射+适配+模式管理）
    core/src/cinematic_camera.cpp # P8 M-CAM1a：CinematicCamera 桩（状态机真实现，轨迹面 M-CAM3a）
    core/src/camera_smoother.cpp  # P8 M-CAM4：CameraSmoother / CameraTrauma（平滑/抖动，默认关）
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
    core/src/profiler_memory.cpp  # P8 M-TR1：Tracy 内存钩（operator new/delete -> TracyAllocS/FreeS）+ K2 PSAPI 进程内存
    core/src/jobgraph.cpp      # P8 M-J5a：显式依赖 DAG 薄封装（vendored Taskflow 只在此 TU 出现）
    core/src/scheduler.cpp     # P8 M-J5b：进程级常驻调度底座（组合 compute/io 两个 ThreadPool）
    core/src/jobsync.cpp       # P8 M-J5c-prep：动态依赖计数 job + Barrier（Jolt 合约同构，Jolt 无关）
    core/src/console.cpp       # P8 M-C0：开发者控制台纯逻辑内核（A 类轨道，零图形符号）
    core/src/material_graph/graph.cpp   # B 路材质图：节点图数据模型 + 序列化
    core/src/material_graph/emit.cpp    # B 路材质图：图 -> Slang 代码生成（B3）
    core/src/material_graph/compile.cpp # T1：图编译验收（emit + slangc 四路，纯进程调用）
    core/src/scene/World.cpp   # M-E0：ECS-lite World/Entity 核心（A 类并行轨道，零图形 API）
    core/src/scene/Component.cpp # M-E1：TransformComponent 层级变换（复合/写回分解）
    core/src/scene/System.cpp  # M-E2：System 实体列表维护（签名匹配，World 推送口径）
    core/src/scene/TrafficCarComponent.cpp # P8 M-T3：ECS 交通车组件序列化
    core/src/scene/SignalLampComponent.cpp  # P8 M-T4：红绿灯摆件组件序列化
    core/src/physics/World.cpp # P8 M-P0：Jolt 最小世界（PIMPL，core 内唯一 Jolt 符号面）
    core/src/physics/ContactEventStream.cpp # P8 M-D1：双缓冲 ContactEventStream（bevy 两帧保活，Jolt-free）
    core/src/physics/ContactEventSystem.cpp # P8 M-D1：scene::System 消费者（owner->Entity 过滤，自足系统）
    core/src/physics/Vehicle.cpp # P8 M-D2：VehicleConstraint 探针式集成（fresh world，Jolt-free 公开头）

    core/src/nav/Nav.cpp       # P8 M-N0：nl/nav 集成层（PIMPL，core 内唯一 Recast/Detour 符号面）
    core/src/nav/GeoLoad.cpp   # P8 M-N2：glb 几何汇集共享层（viewer 面板与 nav-tools 同源）
    core/src/nav/SceneGeo.cpp  # P8 M-N3：E1 场景 World JSON -> nav Geometry（组件 + 层级变换）
    core/src/nav/Crowd.cpp     # P8 M-N4：DetourCrowd 薄封装（PIMPL，core 内唯一 Crowd 符号面）
    core/src/nav/NavAgentComponent.cpp  # P8 M-N6：E1 ECS NavAgent 组件序列化
    core/src/nav/NavAgentSystem.cpp     # P8 M-N6：固定步长 ECS 系统驱动 Crowd 增量链路
    core/src/nav/Arena.cpp              # P8 M-N6：viewer --nav-crowd 合成 arena 几何
    core/src/traffic/LaneGraph.cpp # P8 M-T0.0：roadnet.json -> 车道级展开图（A 类轨道 feat/worldgen，零图形 API）
    core/src/traffic/LanePath.cpp  # P8 M-T0.1：车道图 A* 寻路（确定性线性扫 open 表，零图形 API）
    core/src/traffic/TrafficControl.cpp # P8 M-T1.0：红绿灯相位 + 路口占用锁（A 类轨道 feat/worldgen，零图形 API）
    core/src/traffic/TrafficSim.cpp # P8 M-T0.2：直线跟车仿真（C1 恒速+安全间距，固定 dt 车序逐位可复现）
    core/src/traffic/TrafficCarSystem.cpp # P8 M-T3：ECS 交通车系统（固定步长 sim + Transform 写回，零图形 API）
    core/src/script/Vm.cpp     # M-SC0：C# hosting 内核（hostfxr/nethost，零图形 API）
    core/src/script/Bindings.cpp # M-SC1：console 绑定面（ApiTable 函数指针 + SEH 命令回调）
    core/src/script/ScriptHost.cpp # M-SC3：脚本热更编排（collectible ALC 换载 + console.reload-script）
    core/src/audio/AudioWorld.cpp  # P8 M-S0：nl/audio 薄封装（noDevice 离线 + 实时双路径）
    core/src/audio/DebugController.cpp  # P8 M-S2：调试态单例（音量/独奏/冻结/面板开关）
    core/src/audio/audio_cvars.cpp      # P8 M-S2：audio.* CVar + audio.dump 命令注册
    core/src/tick_groups.cpp   # P8 M-GL5：命名 TickGroup 注册制（逻辑帧组序契约）
    core/src/game_clock.cpp    # P8 M-GL0：双帧模型 GameClock（A 类并行轨道，纯逻辑零依赖）
    core/src/weather.cpp       # WEA M-WEA0：天气状态层（离散态+平滑过渡，纯 CPU 零图形符号）
    core/src/crash.cpp         # M-CRASH0：进程级崩溃捕获（关联系统，薄封装，落本地）
    core/src/net/Net.cpp       # NET M-NET0：nl/net 传输层薄封装（PIMPL，core 内唯一 ENet 符号面）
    core/src/net/SnapshotCodec.cpp # NET M-NET1：快照位打包编解码（PIMPL，core 内唯一 bitsery 符号面）
    core/src/net/Sync.cpp      # NET M-NET2：同步层（控制消息严格解码 + 插值缓冲 + 会话）
)
# NET M-NET1：bitsery header-only 只进 SnapshotCodec.cpp 单 TU（PIMPL 纪律：
# net/Snapshot.hpp 不暴露 bitsery 类型，故 PRIVATE）。
target_include_directories(nl_core PRIVATE ${THIRDPARTY}/bitsery/include)
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
# M-J5a：Taskflow 头文件需对全部 nl_core 消费者可见（header-only，INTERFACE 传播）。
target_link_libraries(nl_core PUBLIC taskflow)
# M-TR0：profiler 薄封装头（宏空转 / ON 时透传 Tracy）对 core/renderer/viewer 全部可见。
target_link_libraries(nl_core PUBLIC nl_profiler)
target_link_libraries(nl_core PRIVATE glfw nl_metis imm32 nl_miniaudio Jolt nl_recast)
# M-LOG0：spdlog 内核只进 log.cpp（PIMPL 纪律：log.hpp 不暴露 spdlog 类型，故 PRIVATE）。
target_link_libraries(nl_core PRIVATE nl_spdlog)
# NET M-NET0：ENet 只进 net/Net.cpp（PIMPL 纪律：net/Net.hpp 不暴露 enet_ 类型，故 PRIVATE）。
target_link_libraries(nl_core PRIVATE nl_enet)
# M-LOG0：文件落盘命名 `<exe>-<backend>-<时间戳>.log`（Q-L2 拍板）——backend 以编译期
# 字符串宏注入 nl_core（与 viewer 的 NL_BACKEND_NAME 同源，core 不碰任何图形 API 头）。
target_compile_definitions(nl_core PRIVATE NL_LOG_BACKEND_NAME="${NL_BACKEND_UPPER}")
# M-P3：启用 Jolt 调试渲染（碰撞形状线框提取）。Jolt 库本身已以 PUBLIC 带上该宏，
# 这里显式复述，避免单配置 Release 下 generator-expression 传播的任何边界情况。
target_compile_definitions(nl_core PRIVATE JPH_DEBUG_RENDERER)
# M-CRASH0：注入 git sha 到崩溃 dump 元数据（configure 时取 HEAD，失败回退 unknown）
execute_process(COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE NL_BUILD_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT NL_BUILD_GIT_SHA)
    set(NL_BUILD_GIT_SHA "unknown")
endif()
target_compile_definitions(nl_core PRIVATE NL_BUILD_GIT_SHA="${NL_BUILD_GIT_SHA}")
# imm32（M-C1）：ImmAssociateContext 解除中文 IME 对 ` 热键的拦截，nl_core window 消费。
# 注：grouping.obj 引用 METIS 符号；viewer 不调用 groupClusters，链接器不会
# 把 grouping.obj 拉进 viewer（MSVC 静态库按引用拉 obj），故 viewer 无 METIS 负担。

# ---- renderer：所有图形 API / NVRHI 符号只活在这里 ----
add_library(nl_renderer STATIC
    renderer/src/renderer.cpp
    renderer/src/png_writer.cpp
    renderer/src/scene.cpp
    renderer/src/hud.cpp
    renderer/src/editor.cpp        # T1：材质图编辑器窗口（imgui-node-editor 接线）
    renderer/src/nav_panel.cpp     # P8 M-N2：NavMesh 调参/五阶段面板（ImDrawList 画布）
    renderer/src/console_ui.cpp   # P8 M-C1：开发者控制台 ImGui 前端（GLFW->ImGuiKey 映射在此）
    renderer/src/audio_debug_ui.cpp  # P8 M-S2：独立默认关的音频声源调试面板（不碰常驻 HUD）
    renderer/src/input_debug.cpp  # P8 M-I2：input-hud 调试面板（注入路径快照 + 注入按钮；默认关）
    renderer/src/backend_dx12.cpp
    renderer/src/backend_vulkan.cpp
)
target_include_directories(nl_renderer
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/renderer/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src
            ${THIRDPARTY}/stb
)
# M-TR0：nl_profiler 必须进 renderer/viewer 的链接面，否则 ON 构建下这两侧 TU 的
# NL_TRACY 不定义、NL_PROFILE_* 全空转（等于开了也不埋点）。core 侧已在其 CMake 链接。
target_link_libraries(nl_renderer PUBLIC nl_core nl_profiler PRIVATE nl_imgui nl_imgui_node_editor glfw)
# 注1：glfw 仅 console_ui.cpp 吃 GLFW 键值常量（M-C1 键盘映射），窗口库本身仍归 core/viewer。
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
#   <name>.vsscene.{spv,dxil}     入口 vsScene（M-E1b.0：ECS 场景实例化，SV_InstanceID）
#   <name>.ps.{spv,dxil}          入口 psMain
#   <name>.pscluster.{spv,dxil}   入口 psCluster（M1.3）
#   <name>.pscull.{spv,dxil}      入口 psCull（M4.0）
#   <name>.pscene.{spv,dxil}      入口 psScene（M-E1b.0：N 物体 M 方向灯）
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
# M7.0（S7.0）：visbuf 路径的 4 个新入口全部收进独立的 visbuf.slang，
# model.slang 一个字节都不动（老产物 100% 不变）。64 位原子在 DXIL 上必须
# SM6.6（spike 实测：默认 profile 报 "64-bit atomic operations should only be
# used in Shader Model 6.6+"），所以只给 _SM66_ARTIFACTS 列出的**新产物**
# 加 -profile sm_6_6，否则 exe=current 的 HUD 基线要全部重拍。
set(_ENTRIES_MODEL "vsMain|vs|vertex" "vsCluster|vscluster|vertex" "vsCull|vscull|vertex"
                   "vsScene|vsscene|vertex"
                   "psMain|ps|fragment" "psCluster|pscluster|fragment" "psCull|pscull|fragment"
                   "psScene|pscene|fragment"
                   "vsShadow|vsshadow|vertex" "psShadow|psshadow|fragment")
set(_ENTRIES_DEFAULT "vsMain|vs|vertex" "psMain|ps|fragment")
# M4.1：cull.slang 单 compute 入口（GPU 视锥剔除 pass）。
set(_ENTRIES_CULL "csCull|cs|compute")
# M5.1：dag.slang 单 compute 入口（DAG 遍历选层，显式栈）。
set(_ENTRIES_DAG "csDag|cs|compute")
# M7.0：visbuf.slang 四入口——簇光栅（vertex pulling + u64 atomicMin 写 vis）
# 与全屏可视化（解包 → 簇/三角颜色）。
set(_ENTRIES_VISBUF "vsVisRaster|vsvisraster|vertex" "psVisRaster|psvisraster|fragment"
                    "vsVisViz|vsvisviz|vertex" "psVisViz|psvisviz|fragment")
# M7.1（S7）：material.slang 单全屏 pass——vis 反解 + 材质着色。与 visbuf.slang 同源
# visbuf_common.slangh，故也吃 _SHADER_EXTRA_DEPS（见下方 STREQUAL 条件）。
# M7.2：NonUniformResourceIndex 选槽（材质 id 逐像素非一致）进白名单 sm_6_6。
set(_ENTRIES_MATERIAL "vsMatResolve|vsmatresolve|vertex" "psMatResolve|psmatresolve|fragment")
# M15.0/M15.1（S15）：vsm.slang 五入口——深度 pass vs/ps（M15.0）+ 清页 quad
# vs/ps（M15.1，blend 关覆写 1.0）+ 页请求标页 compute（M15.1，UE PageMarking 对位）。
# M15.2：+剔除 4 compute（csHzb 12 级 min 链 + count/scan/fill 三段剔除，拍板 B）。
set(_ENTRIES_VSM "vsMain|vs|vertex" "psMain|ps|fragment"
                 "vsMainClear|vsclear|vertex" "psMainClear|psclear|fragment"
                 "csMark|cs|compute"
                 "csHzb|cshzb|compute" "csCullCount|cscullcount|compute"
                 "csCullScan|cscullscan|compute" "csCullFill|cscullfill|compute")
# M9.0（S9）：direct.slang 单 compute 入口（8x8 直光对照 pass）。BRDF 逐字搬移
# material.slang mode 5，资源槽位与 material pass 同源（§2.7 单一 BindingSet）。
# M15.1：+csDirectVsm（--vsm 直光 pass，b6/t14/t15）——拆双入口（拍板 B）：
# 纯净 csDirect blob 回 s9 冻结基线 bytes，VSM 代码隔离进 csvsm 产物。
set(_ENTRIES_DIRECT "csDirect|cs|compute" "csDirectVsm|csvsm|compute"
                    "csReference|csref|compute" "csReferenceGI|csrefgi|compute")
# 公共头用 .slangh 后缀避免被上面的 GLOB 当成独立 shader 源；它只被 #include。
set(_VISBUF_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/visbuf_common.slangh")
# M9.0 (S9): GBuffer 反解公共头（material.slang include；S9.1/S10/S12 复用）
set(_GBUFFER_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/gbuffer_common.slangh")
# M10.1：八面体映射 / 解析天穹公共头（probe_trace/probe_viz include；S12 复用）
set(_OCTAHEDRAL_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/octahedral.slangh")
set(_SKY_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/sky.slangh")
# M-WAT1：水体公共头（Gerstner + 解析法线单源；visbuf/direct/material/ssr 四处 DEPS）。
set(_WATER_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/water_common.slangh")
# M15.0（S15）：VSM 投影采样公共头（direct.slang include；vsm.slang 不 include）
set(_VSM_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/vsm_common.slangh")
# DXIL 需要 SM6.6 的产物白名单（SPIR-V 不需要，spike 实测直通）。
set(_SM66_ARTIFACTS "visbuf.vsvisraster" "visbuf.psvisraster"
                    "visbuf.vsvisviz" "visbuf.psvisviz"
                    "material.vsmatresolve" "material.psmatresolve"
                    "tlas_probe.cs"
                    "rt.cs"
                    "direct.cs"
                    "direct.csvsm"
                    "direct.csref"
                    "direct.csrefgi"
                    "probe_trace.cstrace"
                    "world_cache.cscacheupdate"
                    "ssr.csssr"
                    "ssr.csssrdepth")
# M6.0：hiz.slang 四入口（深度预通道 vs/ps + 金字塔 min 降采样两个 compute +
# 层级可视化 vs/ps）。两个 compute 入口各出一份产物（hiz.cs / hiz.cstop）。
set(_ENTRIES_HIZ "vsHizDepth|vshizdepth|vertex" "psHizDepth|pshizdepth|fragment"
                 "csHizDown|cs|compute" "csHizTop|cstop|compute"
                 "vsHizViz|vshizviz|vertex" "psHizViz|pshizviz|fragment")
# M8.1：tlas_probe.slang 单 compute 入口（TLAS 闭环探针：Slang RayQuery 固定射线，
# hit t 写回 4B 读回日志——M8.1 自检「transform 下帧生效」的机器可读证据；
# 视觉调试射线留 M8.2）。Inline ray query DXIL 要 SM6.5+（进 _SM66_ARTIFACTS）。
set(_ENTRIES_TLAS_PROBE "csTlasProbe|cs|compute")
# M8.2：rt.slang 三入口——RayQuery 每像素相机射线 compute + 调试图全屏呈现 vs/ps。
set(_ENTRIES_RT "csRt|cs|compute" "vsRtPresent|vsrtpresent|vertex" "psRtPresent|psrtpresent|fragment" "psRefPresent|psrefpresent|fragment" "psBlitToHdr|psblittohdr|fragment" "psTonemap|pstonemap|fragment" "psCompare|pscompare|fragment")
# M10.0（S10）：velocity.slang 双入口——独立 velocity prepass（Q10-1 拍板 UE 口径：
# 单独一条小 pass 重画几何写 velocity_，不碰 T2 场景 PSO）。
set(_ENTRIES_VELOCITY "vsVelocity|vsvelocity|vertex" "psVelocity|psvelocity|fragment")
# M10.0（S10）：probe.slang 单 compute 入口——cascade 探针布点（反投影+法线偏移）。
set(_ENTRIES_PROBE "csPlace|csplace|compute")
# M10.0（S10）：probe_viz.slang 双入口——探针点可视化（每探针一实例 quad + 按级着色）。
set(_ENTRIES_PROBE_VIZ "vsProbeDots|vsprobedots|vertex" "psProbeDots|psprobedots|fragment")
# M10.1（S10）：probe_trace.slang 单 compute 入口——探针追踪（每探针每 texel 1 条
# 方向射线 + 命中点直光 shadow ray，Q12-1/Q12-2 拍板 UE 口径）。
# 后缀短名 cstrace（tlas_probe.cs 同款口径）；产物 = probe_trace.cstrace。
set(_ENTRIES_PROBE_TRACE "csTraceProbe|cstrace|compute")
# M10.2（S10）：probe_gather.slang 双 compute 入口——final gather（每像素探针
# 插值 Q13-1 深度选级 + UE 权重）与直光合成（Q13-2 ambient 替换）。
set(_ENTRIES_PROBE_GATHER "csGather|csgather|compute" "csGatherCompose|csgathercompose|compute")
# S11 M11.0：world_cache.slang 三 compute 入口——世界缓存 flat hashmap（Q11-0 方案 A：
# 无墓碑 + life 衰减清零立即重用 + 3 步探测）。半分辨率填充 / 全表衰减 / cache-viz 伪彩。
set(_ENTRIES_WORLD_CACHE "csCacheFill|cscachefill|compute" "csCacheDecay|cscachedecay|compute" "csCacheViz|cscacheviz|compute" "csCacheUpdate|cscacheupdate|compute" "csCacheBlend|cscacheblend|compute")
# M11.2 验收：probe_atlas_dump.slang 单 compute 入口——irradAtlas_[0] 直出画面
#（纯拷贝，无 RayQuery → 不入 _SM66 白名单，DXIL 用默认 profile）。
set(_ENTRIES_PROBE_ATLAS_DUMP "csIrradDump|csirraddump|compute")
# S12 M12.0：filter_temporal.slang 单 compute 入口——GI 分量独立时域累积
#（Q12-A 方案 A；纯纹理采样无 RayQuery → 不入 _SM66 白名单，默认 profile）。
set(_ENTRIES_FILTER_TEMPORAL "csFilterTemporal|csfiltertemporal|compute")
# S12 M12.1：filter_spatial.slang 单 compute 入口——5x5 联合双边（Q12-C A）
#（纯纹理采样无 RayQuery → 不入 _SM66 白名单，默认 profile）。
set(_ENTRIES_FILTER_SPATIAL "csFilterSpatial|csfilterspatial|compute")
# M-WAT0（water 轨）：ssr.slang 五 compute 入口——SSR 本体（march + 深度/金字塔/
# 场景色链/合成，详见 ssr.slang 文件头）。csSSR 用 NonUniformResourceIndex 选
# MR 纹理槽（direct.slang 同款）→ 进 _SM66 白名单；其余纯纹理读默认 profile。
set(_ENTRIES_SSR "csSSRDepth|csssrdepth|compute" "csSSRDown|csssrdown|compute"
                 "csSSRMip|csssrmip|compute" "csSSR|csssr|compute"
                 "csSSRCompose|csssrcompose|compute")
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
    elseif(_SRC_NAME STREQUAL "visbuf")
        set(_ENTRIES "${_ENTRIES_VISBUF}")
    elseif(_SRC_NAME STREQUAL "material")
        set(_ENTRIES "${_ENTRIES_MATERIAL}")
    elseif(_SRC_NAME STREQUAL "vsm")
        set(_ENTRIES "${_ENTRIES_VSM}")
    elseif(_SRC_NAME STREQUAL "tlas_probe")
        set(_ENTRIES "${_ENTRIES_TLAS_PROBE}")
    elseif(_SRC_NAME STREQUAL "rt")
        set(_ENTRIES "${_ENTRIES_RT}")
    elseif(_SRC_NAME STREQUAL "direct")
        set(_ENTRIES "${_ENTRIES_DIRECT}")
    elseif(_SRC_NAME STREQUAL "velocity")
        set(_ENTRIES "${_ENTRIES_VELOCITY}")
    elseif(_SRC_NAME STREQUAL "probe")
        set(_ENTRIES "${_ENTRIES_PROBE}")
    elseif(_SRC_NAME STREQUAL "probe_viz")
        set(_ENTRIES "${_ENTRIES_PROBE_VIZ}")
    elseif(_SRC_NAME STREQUAL "probe_trace")
        set(_ENTRIES "${_ENTRIES_PROBE_TRACE}")
    elseif(_SRC_NAME STREQUAL "probe_gather")
        set(_ENTRIES "${_ENTRIES_PROBE_GATHER}")
    elseif(_SRC_NAME STREQUAL "world_cache")
        set(_ENTRIES "${_ENTRIES_WORLD_CACHE}")
    elseif(_SRC_NAME STREQUAL "probe_atlas_dump")
        set(_ENTRIES "${_ENTRIES_PROBE_ATLAS_DUMP}")
    elseif(_SRC_NAME STREQUAL "filter_temporal")
        set(_ENTRIES "${_ENTRIES_FILTER_TEMPORAL}")
    elseif(_SRC_NAME STREQUAL "filter_spatial")
        set(_ENTRIES "${_ENTRIES_FILTER_SPATIAL}")
    elseif(_SRC_NAME STREQUAL "ssr")
        set(_ENTRIES "${_ENTRIES_SSR}")
    else()
        set(_ENTRIES "${_ENTRIES_DEFAULT}")
    endif()
    foreach(_SPEC ${_ENTRIES})
        string(REPLACE "|" ";" _SPEC_LIST "${_SPEC}")
        list(GET _SPEC_LIST 0 _entry)
        list(GET _SPEC_LIST 1 _suffix)
        list(GET _SPEC_LIST 2 _stage)
        list(FIND _SM66_ARTIFACTS "${_SRC_NAME}.${_suffix}" _sm66_idx)
        if(_sm66_idx GREATER_EQUAL 0)
            set(_DXIL_PROFILE -profile sm_6_6)
        else()
            set(_DXIL_PROFILE "")
        endif()
        # 只有真正 #include 公共头的新入口需要这条依赖；老入口的构建规则保持
        # 原样（DEPENDS 列表一变，NMake 会把它们全部判为过期重编）。
        if(_SRC_NAME STREQUAL "visbuf")
            set(_SHADER_EXTRA_DEPS "${_VISBUF_COMMON}" "${_WATER_COMMON}")
        elseif(_SRC_NAME STREQUAL "material" OR _SRC_NAME STREQUAL "direct"
               OR _SRC_NAME STREQUAL "probe")
            set(_SHADER_EXTRA_DEPS "${_VISBUF_COMMON}" "${_GBUFFER_COMMON}" "${_SKY_COMMON}" "${_VSM_COMMON}" "${_WATER_COMMON}")
        elseif(_SRC_NAME STREQUAL "probe_trace")
            set(_SHADER_EXTRA_DEPS "${_OCTAHEDRAL_COMMON}" "${_SKY_COMMON}")
        elseif(_SRC_NAME STREQUAL "probe_gather")
            set(_SHADER_EXTRA_DEPS "${_OCTAHEDRAL_COMMON}" "${_GBUFFER_COMMON}" "${_SKY_COMMON}")
        elseif(_SRC_NAME STREQUAL "world_cache")
            set(_SHADER_EXTRA_DEPS "${_GBUFFER_COMMON}")
        elseif(_SRC_NAME STREQUAL "probe_viz")
            set(_SHADER_EXTRA_DEPS "${_OCTAHEDRAL_COMMON}" "${_GBUFFER_COMMON}")
        elseif(_SRC_NAME STREQUAL "ssr")
            set(_SHADER_EXTRA_DEPS "${_VISBUF_COMMON}" "${_GBUFFER_COMMON}" "${_SKY_COMMON}" "${_WATER_COMMON}")
        else()
            set(_SHADER_EXTRA_DEPS "")
        endif()
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
            DEPENDS ${_SHADER_EXTRA_DEPS}
            COMMENT "Slang -> SPIR-V (${_SRC_NAME}.${_suffix})")
        add_custom_command(OUTPUT "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.dxil"
            COMMAND "${SLANGC}" "${_SRC}" -dxc-path "${NL_DXC_DIR}"
                    -target dxil -entry ${_entry} -stage ${_stage} ${_DXIL_PROFILE}
                    -o "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.dxil"
            MAIN_DEPENDENCY "${_SRC}"
            DEPENDS ${_SHADER_EXTRA_DEPS}
            COMMENT "Slang -> DXIL (${_SRC_NAME}.${_suffix})")
    endforeach()
endforeach()

# 改 shader 自动重编字节码；ALL 保证每次 build 都检查过期
add_custom_target(nl_shaders ALL DEPENDS ${_SHADER_ARTIFACTS})
# renderer 按编译期注入的 NL_SHADER_DIR 绝对路径读取字节码，不依赖 CWD
target_compile_definitions(nl_renderer PRIVATE NL_SHADER_DIR="${_SHADER_OUT}")

# ---- viewer 与全部工具：target 定义拆到各自目录的 CMakeLists.txt（2026-11-10）----
# 目的：并行轨道加工具 / 加 ctest 用例不再改根文件（追加冲突源根治）。
# 新轨道加工具 = 新建 <dir>/main.cpp + <dir>/CMakeLists.txt（照抄任一现役工具的骨架）
# + 下面清单加一行 add_subdirectory(<dir>)。
if(MSVC)
    # NVRHI 上游头文件带少量非当前代码页字符，压掉 C4819 噪音（不影响代码）
    add_compile_options(/wd4819)
    target_compile_options(nl_core PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(nl_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(nl_renderer PRIVATE /W4 /permissive- /utf-8 /bigobj)
endif()

# exe / dll 统一输出到顶层 build/<backend>/：cases.json 的 exe 路径与
# scripts/build-m0.2.bat 的 selftest 都依赖这个布局（target 进子目录后
# 默认会落到 build/<backend>/<dir>/，必须显式钉回去）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# enable_testing 必须先于 add_subdirectory：目录属性在 add_subdirectory 时拷贝，
# 后调用不会传播进子目录。
enable_testing()

add_subdirectory(viewer)
add_subdirectory(nanite-tools)
add_subdirectory(anim-tools)
add_subdirectory(material-graph-tools)
add_subdirectory(nanite-scene)
add_subdirectory(nl-script)
add_subdirectory(nanite-console)
add_subdirectory(audio-tools)
add_subdirectory(net-tools)
add_subdirectory(phys-tools)
add_subdirectory(nav-tools)
add_subdirectory(city-tools)      # W0 M-W0.0：大世界生成工具（A 类轨道 feat/worldgen，纯 CPU 零图形）
add_subdirectory(traffic-tools)   # P8 M-T0.0：交通 lane graph 工具（A 类轨道 feat/worldgen，只链 nl_core 零图形）
add_subdirectory(gl-tools)
add_subdirectory(input-tools)
add_subdirectory(vfx-tools)
add_subdirectory(cam-tools)
add_subdirectory(gi-tools)        # S11 M11.0：世界缓存 CPU 仿真（hashmap 语义先仿真后上 GPU）
add_subdirectory(log-tools)       # A 类日志轨道 M-LOG0：nl::core::log 无头验收
add_subdirectory(editor_bridge)   # 引擎侧 bridge（静态库，M-E2 供 viewer 接线）
add_subdirectory(editor)          # 编辑器宿主（C++/ImGui，独立进程）
