# ---- P8 M-C0：开发者控制台工具（A 类轨道，工具版交代）----
# core/console 纯逻辑内核的机械单测 + demo 注册表执行器（selftest / exec）。
# 只链 nl_core，零图形 API；不依赖 renderer / viewer。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(nanite-console main.cpp)
target_link_libraries(nanite-console PRIVATE nl_core)

if(MSVC)
    target_compile_options(nanite-console PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(nanite-console PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# P8 M-C0：core/console 纯逻辑内核机械单测（A 类轨道，无渲染依赖）
add_test(NAME p8-console-selftest COMMAND nanite-console selftest)
