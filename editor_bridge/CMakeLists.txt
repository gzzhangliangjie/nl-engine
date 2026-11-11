# editor_bridge/CMakeLists.txt — 引擎侧 bridge（静态库，供 viewer 在 M-E2 接线）
# 不碰 renderer/ 任何图形路径；仅 winsock2 / Win32 命名管道 + 自研 msgpack。
# M-E6：拆出 nl_editor_transport —— 编辑器侧（nl_editor_common / 插件 DLL）与引擎侧
# （nl_editor_bridge）共用同一份传输实现，避免两个进程各编一份 endpoint 解析漂移。
add_library(nl_editor_transport STATIC
    transport.cpp
)
target_include_directories(nl_editor_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nl_editor_transport PUBLIC ws2_32)
target_compile_options(nl_editor_transport PRIVATE /utf-8)
target_compile_definitions(nl_editor_transport PRIVATE NOMINMAX)

add_library(nl_editor_bridge STATIC
    rpc_bridge.cpp
    embed_window.cpp
)
target_include_directories(nl_editor_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nl_editor_bridge PUBLIC nl_editor_transport)
# 源码含中文注释：显式 UTF-8，避免默认 GBK 代码页误读导致解析崩溃
target_compile_options(nl_editor_bridge PRIVATE /utf-8)
