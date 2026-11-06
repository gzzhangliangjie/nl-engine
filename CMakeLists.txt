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

# ---- core：日志 / 断言 / 窗口。不依赖任何图形 API 头文件（GLFW 只进 .cpp） ----
add_library(nl_core STATIC
    core/src/log.cpp
    core/src/assert.cpp
    core/src/window.cpp
)
target_include_directories(nl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_link_libraries(nl_core PRIVATE glfw)

# ---- renderer：所有图形 API / NVRHI 符号只活在这里 ----
add_library(nl_renderer STATIC
    renderer/src/renderer.cpp
    renderer/src/png_writer.cpp
    renderer/src/backend_dx12.cpp
    renderer/src/backend_vulkan.cpp
)
target_include_directories(nl_renderer
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/renderer/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src
            ${THIRDPARTY}/stb
)
target_link_libraries(nl_renderer PUBLIC nl_core)
target_compile_definitions(nl_renderer PRIVATE
    NL_BACKEND_${NL_BACKEND_UPPER}=1
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

set(_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/triangle.slang")
set(_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${_SHADER_OUT}")

set(_SHADER_ARTIFACTS "")
foreach(_ent vs ps)
    if(_ent STREQUAL "vs")
        set(_stage vertex)
        set(_entry vsMain)
    else()
        set(_stage fragment)
        set(_entry psMain)
    endif()
    list(APPEND _SHADER_ARTIFACTS "${_SHADER_OUT}/tri.${_ent}.spv"
                                  "${_SHADER_OUT}/tri.${_ent}.dxil")
    add_custom_command(OUTPUT "${_SHADER_OUT}/tri.${_ent}.spv"
        COMMAND "${SLANGC}" "${_SHADER_SRC}" -target spirv -entry ${_entry} -stage ${_stage}
                -o "${_SHADER_OUT}/tri.${_ent}.spv"
        MAIN_DEPENDENCY "${_SHADER_SRC}"
        COMMENT "Slang -> SPIR-V (${_ent})")
    add_custom_command(OUTPUT "${_SHADER_OUT}/tri.${_ent}.dxil"
        COMMAND "${SLANGC}" "${_SHADER_SRC}" -dxc-path "${NL_DXC_DIR}"
                -target dxil -entry ${_entry} -stage ${_stage}
                -o "${_SHADER_OUT}/tri.${_ent}.dxil"
        MAIN_DEPENDENCY "${_SHADER_SRC}"
        COMMENT "Slang -> DXIL (${_ent})")
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

if(MSVC)
    # NVRHI 上游头文件带少量非当前代码页字符，压掉 C4819 噪音（不影响代码）
    add_compile_options(/wd4819)
    foreach(t nl_core nl_renderer viewer)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8 /bigobj)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endforeach()
endif()

# 无窗口、无 GPU 的机械验收用例：CI 里 `ctest` 直接跑
enable_testing()
add_test(NAME m0.2-selftest COMMAND viewer --selftest)
