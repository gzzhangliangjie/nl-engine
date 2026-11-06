cmake_minimum_required(VERSION 3.24)
project(nl-engine VERSION 0.0.1 LANGUAGES CXX)

# S0 / M0.1：最小可跑骨架。
# 搭建顺序参考 docs/s0/CARROT-BOOTSTRAP.md（Carrot 2020-11 从零起步的实况，仅作参考）。
# 纪律：viewer/ 与 core/ 里不得出现任何 vk 开头的符号；图形 API 只允许活在 renderer/。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# ---- renderer：M0.1 只放占位状态；M0.2 起在这里接 NVRHI device / swapchain ----
add_library(nl_renderer STATIC
    renderer/src/renderer.cpp
)
target_include_directories(nl_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/renderer/include)

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
