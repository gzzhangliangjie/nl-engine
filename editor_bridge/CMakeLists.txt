# editor_bridge/CMakeLists.txt — 引擎侧 bridge（静态库，供 viewer 在 M-E2 接线）
# 不碰 renderer/ 任何图形路径；仅 winsock2 + 自研 msgpack。
add_library(nl_editor_bridge STATIC
    rpc_bridge.cpp
    embed_window.cpp
)
target_include_directories(nl_editor_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nl_editor_bridge PRIVATE ws2_32)
# 源码含中文注释：显式 UTF-8，避免默认 GBK 代码页误读导致解析崩溃
target_compile_options(nl_editor_bridge PRIVATE /utf-8)
