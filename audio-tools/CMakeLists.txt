# ---- P8 M-S0：audio-tools（A 类轨道，工具版交代）----
# 只链 nl_core（nl/audio + nl_miniaudio），零图形 API。
# 退出码约定：0=PASS / 1=断言或运行失败 / 2=用法错误（展示台 expectExit 0/1/2）。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
# 注意：assets 在仓库根，本目录已不是仓库根，统一用根级 NL_ROOT 变量。
add_executable(audio-tools main.cpp)
target_link_libraries(audio-tools PRIVATE nl_core)
target_compile_definitions(audio-tools PRIVATE
    NL_AUDIO_ASSET_DIR="${NL_ROOT}/assets/audio"
    NL_AUDIO_ROOT="${NL_ROOT}")

if(MSVC)
    target_compile_options(audio-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(audio-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- P8 M-S0：音频离线渲染确定性机械单测（A 类轨道，工具版交代）----
# 不进渲染回归总闸；判据 = 两次独立 noDevice 渲染逐样本位级一致 +
# provenance 篡改显式拒绝（防假绿，照 phys-tools check 教训）。
add_test(NAME s0-audio-selftest COMMAND audio-tools selftest)
set_tests_properties(s0-audio-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "audio selftest: PASS")
