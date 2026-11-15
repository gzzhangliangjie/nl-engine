# ---- P8 M-CAM0：cam-tools —— 玩法相机族无头验收工具（A 类轨道 track camera）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 input-tools / 展示台 expectExit）。
# 只链 nl_core（ChaseCamera/FreeCamera 纯 CPU 类），零图形 API、零 GPU、不碰 kStage。
# 2026-11-10 照 input-tools 三步骨架立（main.cpp -> 目录 CMakeLists -> 根清单一行）。
add_executable(cam-tools main.cpp)
# M-CAM1a：director-f 节吃 InputState 注入路径（input.hpp 公开 API 用 GLFW 键码
# 常量），同 input-tools 链 glfw 的理由（拿 include 路径，非窗口）。
target_link_libraries(cam-tools PRIVATE nl_core glfw)

if(MSVC)
    target_compile_options(cam-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(cam-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# P8 M-CAM0：相机机械锚（init/clamp + 五锚场景 A~E + 正交/透视基座断言）
add_test(NAME mcam0-cam-selftest COMMAND cam-tools selftest)
add_test(NAME mcam0-cam-usage-negative COMMAND cam-tools selftest bogus)
set_tests_properties(mcam0-cam-usage-negative PROPERTIES WILL_FAIL TRUE)
