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
)
target_include_directories(nl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_include_directories(nl_core PRIVATE ${THIRDPARTY}/tinygltf)
target_link_libraries(nl_core PRIVATE glfw)

# ---- renderer：所有图形 API / NVRHI 符号只活在这里 ----
add_library(nl_renderer STATIC
    renderer/src/renderer.cpp
    renderer/src/png_writer.cpp
    renderer/src/scene.cpp
    renderer/src/hud.cpp
    renderer/src/backend_dx12.cpp
    renderer/src/backend_vulkan.cpp
)
target_include_directories(nl_renderer
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/renderer/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src
            ${THIRDPARTY}/stb
)
target_link_libraries(nl_renderer PUBLIC nl_core PRIVATE nl_imgui)
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
# 每个源 <name>.slang 出 8 个产物（model 双入口后）：
#   <name>.vs.{spv,dxil}         入口 vsMain
#   <name>.vscluster.{spv,dxil}  入口 vsCluster（M1.3：多带 CLUSTERCOLOR 顶点属性）
#   <name>.ps.{spv,dxil}         入口 psMain
#   <name>.pscluster.{spv,dxil}  入口 psCluster（M1.3）
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
set(_ENTRIES_MODEL "vsMain|vs|vertex" "vsCluster|vscluster|vertex"
                   "psMain|ps|fragment" "psCluster|pscluster|fragment")
set(_ENTRIES_DEFAULT "vsMain|vs|vertex" "psMain|ps|fragment")
foreach(_SRC ${NL_SHADER_SRCS})
    get_filename_component(_SRC_NAME "${_SRC}" NAME_WE)   # 源文件名（无扩展名），如 "triangle"
    if(_SRC_NAME STREQUAL "model")
        set(_ENTRIES "${_ENTRIES_MODEL}")
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
        #   b0 -> binding 256, t0 -> binding 0, s0 -> binding 128（set 0）。
        # DX12 侧仍用 b0/t0/s0 各自的 slot 0，故同一份 .slang 无需任何 GLSL 注解。
        add_custom_command(OUTPUT "${_SHADER_OUT}/${_SRC_NAME}.${_suffix}.spv"
            COMMAND "${SLANGC}" "${_SRC}" -target spirv -entry ${_entry} -stage ${_stage}
                    -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0
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
add_executable(nanite-tools nanite-tools/main.cpp)
target_link_libraries(nanite-tools PRIVATE nl_core)

if(MSVC)
    # NVRHI 上游头文件带少量非当前代码页字符，压掉 C4819 噪音（不影响代码）
    add_compile_options(/wd4819)
    foreach(t nl_core nl_renderer viewer nanite-tools)
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
