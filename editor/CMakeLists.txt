# editor/CMakeLists.txt — 编辑器宿主进程（C++ / Dear ImGui，独立进程）
# 依赖：nl_imgui_dll（imgui 核心，SHARED——宿主与插件 DLL 必须共享同一 ImGuiContext）、
#       glfw（窗口）、nl_editor_bridge（RPC 协议 + msgpack）、
#       nl_editor_common（editor.exe 与插件 DLL 共享的 RpcHost 静态库）、
#       opengl32（Windows SDK，GL2 后端）、ws2_32（winsock）。零新增 vendored 依赖。

# ---- M-E5：editor.exe 与插件 DLL 共享的静态库 ----
# RpcHost/Plugin 接口的实现只有一份（否则插件 DLL 里各自编一份 rpc_host.cpp，
# 静态状态分裂）。插件经宿主传入的 RpcHost* 调方法，符号从本库解析。
# ABI 前提：插件 DLL 与宿主同仓同 MSVC 工具链编译（本仓单一工具链，成立）。
add_library(nl_editor_common STATIC
    rpc_host.cpp
)
target_include_directories(nl_editor_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/editor_bridge   # msgpack_min.hpp
)
target_link_libraries(nl_editor_common PUBLIC ws2_32)
target_compile_options(nl_editor_common PRIVATE /utf-8)

# 编辑器宿主 EXE
add_executable(editor
    main.cpp
    viewport.cpp
    plugin.cpp
    imgui_backend.cpp
)
target_include_directories(editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/editor_bridge   # msgpack_min.hpp
)
target_link_libraries(editor PRIVATE
    nl_imgui_dll
    nl_editor_common
    nl_editor_bridge
    glfw
    opengl32
)
# 源码含中文注释：显式 UTF-8（根 CMake 只给 nl_core/nl_renderer 加了，编辑器轨道需自管）
# M-E2：NOMINMAX = viewport.cpp 用 windows.h + std::max，防 min/max 宏污染
target_compile_options(editor PRIVATE /utf-8)
target_compile_definitions(editor PRIVATE NOMINMAX)

# ---- M-E5：示例插件 DLL（plugins/ 目录动态扫描加载的验收物）----
# 输出到 <build>/<backend>/plugins/（editor.exe 同目录下），discoverDlls() 扫描加载。
add_library(editor_plugin_hello SHARED
    plugins/hello_plugin.cpp
)
target_link_libraries(editor_plugin_hello PRIVATE
    nl_editor_common
    nl_imgui_dll
)
set_target_properties(editor_plugin_hello PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
)
target_compile_options(editor_plugin_hello PRIVATE /utf-8)
target_compile_definitions(editor_plugin_hello PRIVATE NOMINMAX)

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
