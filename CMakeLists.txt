cmake_minimum_required(VERSION 3.24)
project(nl-engine VERSION 0.0.1 LANGUAGES C CXX)

# S0 / M0.1：最小可跑骨架。
# 搭建节奏参考 docs/s0/CARROT-BOOTSTRAP.md（Carrot 2020-11 从零起步的实况，仅作参考）。
#
# 【架构纪律】这是通用管线，构建系统不依赖任何厂商图形 SDK：
#   - 不读取 VULKAN_SDK / VK_SDK_PATH，不 find_package(Vulkan)，不要求 vkcube
#   - D3D12 / OpenGL 的头文件与库来自 Windows SDK（系统自带）
#   - Vulkan 头走 KhronosGroup/Vulkan-Headers（M0.2 由 NVRHI 的
#     NVRHI_FETCH_VULKAN_HEADERS 拉），loader 是显卡驱动自带的 vulkan-1.dll
#   - Slang 独立获取，不从任何 SDK 路径里找
#   viewer/ 与 core/ 里不得出现任何图形 API 符号；图形 API 只允许活在 renderer/。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 后端选择：只是编译期配置，M0.1 不链接任何图形 API ----
set(NL_GRAPHICS_BACKEND "AUTO" CACHE STRING "Graphics backend: AUTO | DX12 | VULKAN")
set_property(CACHE NL_GRAPHICS_BACKEND PROPERTY STRINGS AUTO DX12 VULKAN)
string(TOUPPER "${NL_GRAPHICS_BACKEND}" NL_BACKEND_UPPER)
if(NL_BACKEND_UPPER STREQUAL "AUTO")
    if(WIN32)
        # Windows 零额外依赖：头 / 库 / debug layer 全在 Windows SDK 里
        set(NL_BACKEND_UPPER "DX12")
    else()
        set(NL_BACKEND_UPPER "VULKAN")
    endif()
endif()
if(NOT NL_BACKEND_UPPER STREQUAL "DX12" AND NOT NL_BACKEND_UPPER STREQUAL "VULKAN")
    message(FATAL_ERROR "NL_GRAPHICS_BACKEND must be AUTO, DX12 or VULKAN")
endif()
message(STATUS "nl-engine backend: ${NL_BACKEND_UPPER} (no vendor SDK required)")

# ---- thirdparty：GLFW 源码本地 vendored，零网络（M0.0 已复用 Carrot/thirdparty/glfw） ----
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

# ---- renderer：M0.1 只报告选定了哪个后端；M0.2 起在这里接 NVRHI device / swapchain ----
add_library(nl_renderer STATIC
    renderer/src/renderer.cpp
)
target_include_directories(nl_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/renderer/include)
target_compile_definitions(nl_renderer PRIVATE NL_BACKEND_${NL_BACKEND_UPPER}=1)

# ---- viewer：可见用例（窗口 + 标题栏实时帧号 + ESC 退出）与 --selftest ----
add_executable(viewer viewer/main.cpp)
target_link_libraries(viewer PRIVATE nl_core nl_renderer glfw)

if(MSVC)
    foreach(t nl_core nl_renderer viewer)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endforeach()
endif()

# 验收判据之一：无窗口自检可进 CI
enable_testing()
add_test(NAME m0.1-selftest COMMAND viewer --selftest)
