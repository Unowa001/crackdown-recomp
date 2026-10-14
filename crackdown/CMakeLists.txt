# crackdown - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(crackdown LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(CRACKDOWN_SOURCES
    src/main.cpp
    src/hooks_events.cpp
    src/hooks_storage.cpp
    src/host_tuning.cpp
    src/hooks_sleep.cpp
    src/hooks_intro.cpp
    src/xui_patch.cpp
    src/hooks_pc_options.cpp
    src/pc_settings.cpp
    common/toml_edit.cpp
    src/hooks_capture.cpp
    src/hooks_xaudio.cpp
    src/hooks_framerate.cpp
    src/hooks_diag_physics.cpp
    src/hooks_diag_audio.cpp
    src/hooks_coop_trace.cpp
    src/hooks_input_replay.cpp
    src/ap/ap_probe.cpp
    src/ap/session_state.cpp
    src/ap/ap_trace.cpp
    src/ap/bridge.cpp
    src/ap/catalog.cpp
    src/ap/client.cpp
)
# SHA256 for the XUI patch applier: the SDK's bundled implementation (also used by the launcher).
set(CRACKDOWN_SHA256_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/crypto")

if(WIN32)
    add_executable(crackdown WIN32 ${CRACKDOWN_SOURCES})
else()
    add_executable(crackdown ${CRACKDOWN_SOURCES})
endif()
target_sources(crackdown PRIVATE "${CRACKDOWN_SHA256_DIR}/sha256.cpp")
target_include_directories(crackdown PRIVATE "${CRACKDOWN_SHA256_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/tomlplusplus/include")

rexglue_setup_target(crackdown GPU_PLUGINS xenos)

# AP catalog: one versioned JSON for the C++ bridge and the Python world,
# embedded as bytes (string literals this size exceed MSVC limits).
set(AP_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/ap/catalog.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AP_CATALOG}")
file(READ "${AP_CATALOG}" _ap_hex HEX)
string(LENGTH "${_ap_hex}" _ap_len)
math(EXPR _ap_bytes "${_ap_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _ap_array "${_ap_hex}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ap_generated/ap_catalog_data.h.tmp"
    "// Generated from crackdown/ap/catalog.json; do not edit.\n#pragma once\n"
    "inline constexpr unsigned char kCatalogJson[] = {${_ap_array}};\n"
    "inline constexpr unsigned long kCatalogJsonSize = ${_ap_bytes};\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/ap_generated/ap_catalog_data.h.tmp"
    "${CMAKE_CURRENT_BINARY_DIR}/ap_generated/ap_catalog_data.h" COPYONLY)
target_include_directories(crackdown PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ap_generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/inja/third_party/include")

# A small audited COPY/INSERT patch, never a modified game package. Copy on
# every build so recipe changes are picked up without requiring an exe relink.
# External rather than embedded: Windows reviewers can audit/remove the exact
# shipped patch and exercise fail-open behavior without rebuilding the exe.
set(PC_SCENE_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/../patches/xui/Scene.xpatch")
set(PC_MENU_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/../patches/xui/menuHUD_3.xpatch")
add_custom_target(crackdown_xui_patch ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:crackdown>/xui"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PC_MENU_PATCH}" "$<TARGET_FILE_DIR:crackdown>/xui/menuHUD_3.xpatch"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PC_SCENE_PATCH}" "$<TARGET_FILE_DIR:crackdown>/xui/Scene.xpatch"
    DEPENDS "${PC_MENU_PATCH}" "${PC_SCENE_PATCH}"
    VERBATIM)
add_dependencies(crackdown crackdown_xui_patch)
install(FILES "${PC_MENU_PATCH}" "${PC_SCENE_PATCH}" DESTINATION xui)

option(CRACKDOWN_PC_OPTIONS_TESTS "Build the host XUIPAT1 integrity tests" OFF)
if(CRACKDOWN_PC_OPTIONS_TESTS)
    enable_testing()
    add_executable(xui_patch_tests tests/xui_patch_tests.cpp src/xui_patch.cpp "${CRACKDOWN_SHA256_DIR}/sha256.cpp")
    target_include_directories(xui_patch_tests PRIVATE src "${CRACKDOWN_SHA256_DIR}")
    target_link_libraries(xui_patch_tests PRIVATE rex::runtime)
    add_test(NAME xui_patch_integrity COMMAND xui_patch_tests)
    add_executable(pc_settings_tests tests/pc_settings_tests.cpp src/pc_settings.cpp common/toml_edit.cpp)
    target_include_directories(pc_settings_tests PRIVATE src
        "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/thirdparty/tomlplusplus/include")
    add_test(NAME pc_settings_persistence COMMAND pc_settings_tests)
endif()

# Public-symbol PDB on Windows: link-only (no -g), so it costs a relink, not a
# recompile. Lets cdb/WinDbg show frames as sub_XXXXXXXX (guest address).
# /DEBUG turns off lld's default /OPT:REF,ICF, so restore them explicitly.
if(WIN32)
    target_link_options(crackdown PRIVATE -Wl,/DEBUG:FULL -Wl,/OPT:REF,/OPT:ICF)
endif()
