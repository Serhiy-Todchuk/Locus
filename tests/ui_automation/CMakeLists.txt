# S5.L -- Manual-only UI Automation test driver.
#
# NOT registered with ctest -- the driver pops real windows on the desktop
# session, steals focus, and (for the chat_round_trip script) needs a live
# LM Studio. Same protocol as tests/integration/: run on explicit request.
#
# Build:
#   cmake --build build/release --config Release --target locus_ui_tests
#
# Run one script:
#   build/release/tests/ui_automation/Release/locus_ui_tests.exe \
#       --script tests/ui_automation/scripts/smoke.json
#
# Run every script:
#   build/release/tests/ui_automation/Release/locus_ui_tests.exe --all

add_executable(locus_ui_tests
    main.cpp
    uia_session.cpp
    op_dispatcher.cpp
    script_runner.cpp
    agentic_server.cpp
)

target_include_directories(locus_ui_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Bake the build-time scripts dir + a sensible output dir into the binary
# so users can run the exe with no flags and have it find its scripts.
# NOMINMAX keeps windows.h from clobbering std::min / std::max.
# (WIN32_LEAN_AND_MEAN is deliberately NOT set -- it strips RPC headers that
# UIAutomation.h's forward declarations depend on, causing a cascade of
# C2371 redefinition errors in UIAutomationCore.h.)
target_compile_definitions(locus_ui_tests PRIVATE
    LOCUS_UI_TESTS_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
    LOCUS_UI_TESTS_OUTPUT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/output"
    LOCUS_UI_TESTS_DEFAULT_GUI="$<TARGET_FILE:locus_gui>"
    NOMINMAX
)

# nlohmann::json header-only -- pull from the same find_package() the root
# CMakeLists already invoked.
target_link_libraries(locus_ui_tests PRIVATE
    nlohmann_json::nlohmann_json
)

# Native deps: Windows UI Automation client + WIC (PNG writer) + GDI.
# uiautomationcore.lib + windowscodecs.lib + gdi32.lib are part of the
# Windows SDK; no vcpkg dep needed.
target_link_libraries(locus_ui_tests PRIVATE
    uiautomationcore
    ole32
    oleaut32
    windowscodecs
    gdi32
    user32
    ws2_32
)

set_target_properties(locus_ui_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# locus_gui must exist for screenshots / launching to work; declaring the
# dependency makes "build the test target" implicitly build the GUI too.
add_dependencies(locus_ui_tests locus_gui)

# Deliberately NOT calling catch_discover_tests / add_test() -- manual-only.
