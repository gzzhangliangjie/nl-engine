# editor/CMakeLists.txt — 编辑器宿主进程（C++ / Dear ImGui，独立进程）
# 依赖：nl_imgui（imgui 核心）、glfw（窗口）、nl_editor_bridge（RPC 协议 + msgpack）、
#       opengl32（Windows SDK，GL2 后端）、ws2_32（winsock）。零新增 vendored 依赖。

# 编辑器宿主 EXE
add_executable(editor
    main.cpp
    viewport.cpp
    plugin.cpp
    imgui_backend.cpp
    rpc_host.cpp
)
target_include_directories(editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/editor_bridge   # msgpack_min.hpp
)
target_link_libraries(editor PRIVATE
    nl_imgui
    glfw
    nl_editor_bridge
    opengl32
    ws2_32
)
# 源码含中文注释：显式 UTF-8（根 CMake 只给 nl_core/nl_renderer 加了，编辑器轨道需自管）
# M-E2：NOMINMAX = viewport.cpp 用 windows.h + std::max，防 min/max 宏污染
target_compile_options(editor PRIVATE /utf-8)
target_compile_definitions(editor PRIVATE NOMINMAX)

# M-E1 验收：同进程 server+client 双线程，验证握手/announce/echo/心跳，可无头运行
add_executable(rpc_self_test
    rpc_self_test.cpp
    rpc_host.cpp
)
target_include_directories(rpc_self_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/editor_bridge
)
target_link_libraries(rpc_self_test PRIVATE
    nl_editor_bridge
    ws2_32
)
target_compile_options(rpc_self_test PRIVATE /utf-8)
target_compile_definitions(rpc_self_test PRIVATE NOMINMAX)
