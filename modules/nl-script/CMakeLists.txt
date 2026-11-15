# ---- M-SC0：nl-script —— C# hosting（nl/script）自测工具，A 类并行轨道 ----
# 退出码约定：0=PASS / 1=断言失败或脚本运行失败 / 2=用法错误（与展示台 expectExit 0/1/2 对齐）。
# nethost.dll 运行期紧挨 exe（官方 hosting 样例布局：链 libnethost 导入库 + copy dll）。
# 2026-11-10 自根 CMakeLists.txt 拆出。
add_executable(nl-script main.cpp)
# glfw：PRIVATE include 面（M-SC5 s9 段经 nl/core/input.hpp→action_map.hpp 拉 GLFW 头；
# nl_core 对 glfw 是 PRIVATE 不传播，viewer main.cpp 同款显式接法——只吃键码常量，无 GLFW 调用）。
target_link_libraries(nl-script PRIVATE nl_core glfw)
add_custom_command(TARGET nl-script POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${THIRDPARTY}/dotnet-hosting/bin/nethost.dll $<TARGET_FILE_DIR:nl-script>)

if(MSVC)
    target_compile_options(nl-script PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(nl-script PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
