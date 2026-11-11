# ---- viewer：可见用例 + 程序内截图（--screenshot） ----
# 2026-11-10 自根 CMakeLists.txt 拆出：新轨道不再改根文件（追加冲突根治）。
add_executable(viewer main.cpp)
# M-E2：nl_editor_bridge = 编辑器 RPC client + SetParent 嵌入（--editor-bridge 时用）
target_link_libraries(viewer PRIVATE nl_core nl_renderer glfw nl_editor_bridge)
# M-E4b：announce.caps 需要后端名；根 CMake 决议后的 NL_BACKEND_UPPER 以字符串宏注入
target_compile_definitions(viewer PRIVATE NL_BACKEND_NAME="${NL_BACKEND_UPPER}")
# shader 字节码先于 viewer 生成（viewer 运行时要读 NL_SHADER_DIR 下的 .spv/.dxil）
add_dependencies(viewer nl_shaders)

if(MSVC)
    target_compile_options(viewer PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(viewer PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# 无窗口、无 GPU 的机械验收用例：CI 里 `ctest` 直接跑
add_test(NAME m0.5-selftest COMMAND viewer --selftest)
