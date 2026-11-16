# game — M-GAME0 standalone playable game.exe (GAME-DEV-ORDER step 7).
# Renderer exe, parallel to viewer: links nl_core + nl_renderer + glfw.
# Viewer stays pure showcase (zero source change, zero regression).
add_executable(game main.cpp)
target_link_libraries(game PRIVATE nl_core nl_renderer nl_profiler glfw)
target_compile_definitions(game PRIVATE NL_BACKEND_NAME="${NL_BACKEND_UPPER}")
add_dependencies(game nl_shaders)   # shader bytecode must exist before link
if(MSVC)
  target_compile_options(game PRIVATE /W4 /permissive- /utf-8 /bigobj)
  target_compile_definitions(game PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME game-m0-selftest COMMAND game --selftest)   # headless anchors
