# SPDX-License-Identifier: GPL-3.0-or-later
# Portions Copyright (c) 2026 Chaos192
# Graphics shell (S4b): SDL3 platform + input queue, Vulkan 1.3 device layer, NVN front end, ImGui overlay.
# Built as the static library nxrt_gfx (links nxrt).
include(${CMAKE_CURRENT_LIST_DIR}/gfx_deps.cmake)

# The offline tiling library of the tools repository is compiled in as source (tools/tiling, tegra_tiling.h): found at
# ../tools or, from a worktree under runtime-wt/<name>, at ../../tools.
set(NXRT_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools" CACHE PATH "The tools repository (tools/tiling is used)")
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/tiling/src/tegra_tiling.cpp")
    get_filename_component(_nxrt_td "${CMAKE_CURRENT_SOURCE_DIR}/../../tools" ABSOLUTE)
    set(NXRT_TOOLS_DIR "${_nxrt_td}" CACHE PATH "The tools repository (tools/tiling is used)" FORCE)
endif()
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
    message(FATAL_ERROR "tools/tiling not found: set NXRT_TOOLS_DIR to the tools repository (looked in ${NXRT_TOOLS_DIR})")
endif()
add_library(nxrt_tiling STATIC "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
target_include_directories(nxrt_tiling PUBLIC "${NXRT_TOOLS_DIR}/tiling/include")

# ASTC -> BC7 transcoder (tools/textures): the prebuilt static libraries of the tools repository (scripts/build.ps1 there) and the
# decoder DLLs that astc2bc7 loads at run time (copied next to every executable that links nxrt_gfx).
set(NXRT_A2B_DIR "${NXRT_TOOLS_DIR}/textures/build/release" CACHE PATH "Build folder of tools/textures (astc2bc7 and bc7e_ispc libraries, astcdec-* decoders)")
if(WIN32)
    set(_a2b_lib "${NXRT_A2B_DIR}/astc2bc7.lib")
    set(_bc7e_lib "${NXRT_A2B_DIR}/bc7e_ispc.lib")
    set(_a2b_dll_glob "${NXRT_A2B_DIR}/astcdec-*-shared.dll")
else()
    set(_a2b_lib "${NXRT_A2B_DIR}/libastc2bc7.a")
    set(_bc7e_lib "${NXRT_A2B_DIR}/libbc7e_ispc.a")
    set(_a2b_dll_glob "${NXRT_A2B_DIR}/libastcdec-*-shared.dylib")
endif()
if(NOT EXISTS "${_a2b_lib}" OR NOT EXISTS "${_bc7e_lib}")
    message(FATAL_ERROR "${_a2b_lib} not found: build tools/textures first (scripts/build.ps1 or scripts/build_mac.sh there) or set NXRT_A2B_DIR")
endif()
add_library(nxrt_astc2bc7 INTERFACE)
target_include_directories(nxrt_astc2bc7 INTERFACE "${NXRT_TOOLS_DIR}/textures/include")
target_link_libraries(nxrt_astc2bc7 INTERFACE "${_a2b_lib}" "${_bc7e_lib}")
file(GLOB NXRT_A2B_DLLS "${_a2b_dll_glob}")

file(GLOB_RECURSE NXRT_GFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/*.cpp")
if(APPLE)
    # DXGI display queries and the DLSS / FidelityFX DLL loaders are Windows components: platform/mac/gfx has their macOS side.
    list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "/src/gfx/extras/(win_display|temporal_dlss|temporal_ffx)[.]cpp$")
    file(GLOB NXRT_GFX_MAC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/mac/gfx/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/platform/mac/gfx/*.mm")
    list(APPEND NXRT_GFX_SOURCES ${NXRT_GFX_MAC_SOURCES})
endif()
add_library(nxrt_gfx STATIC ${NXRT_GFX_SOURCES})
# The About page of the F1 menu shows the commit this build was made from (read when CMake configures; absent without git).
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=10 HEAD
        OUTPUT_VARIABLE NXRT_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _nxrt_git_rc)
    if(_nxrt_git_rc EQUAL 0 AND NXRT_BUILD_COMMIT)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/play/play_ui_about.cpp" PROPERTIES COMPILE_DEFINITIONS "NXRT_BUILD_COMMIT=\"${NXRT_BUILD_COMMIT}\"")
    endif()
endif()
target_include_directories(nxrt_gfx PUBLIC include src/gfx/nvn/third_party PRIVATE src/gfx src/gfx/nvn)
# The cheats overlay's typeface and themes (ported from AnySwitch by Chaos192, github.com/Chaos192/AnySwitch).
# Inter (OFL-1.1, the launcher's; assets/fonts/LICENSE-Inter.txt): the three subset TTFs compiled in as their bytes
# (src/gfx/play/cheats_theme.cpp), so the overlay looks the same whatever folder the game runs from.
set(_nxrt_inter_ok ON)
foreach(_f Inter-Regular Inter-Regular-Ext Inter-SemiBold)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/${_f}.ttf")
    if(NOT EXISTS "${_src}")
        set(_nxrt_inter_ok OFF)
        continue()
    endif()
    string(TOLOWER "${_f}" _n)
    string(REPLACE "-" "_" _n "${_n}")
    file(READ "${_src}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nxrt_gen/cheats_font_${_n}.inc" CONTENT "${_bytes}
" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
endforeach()
if(_nxrt_inter_ok)
    target_include_directories(nxrt_gfx PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nxrt_gen")
    target_compile_definitions(nxrt_gfx PRIVATE NXRT_HAVE_CHEATS_FONTS=1)
endif()
# The cheats overlay's built-in themes and the look settings' defaults and ranges (runtime/assets/cheats: themes/*.json, look.json, which
# the launcher imports too), compiled in as NUL-terminated bytes (src/gfx/play/cheats_theme_file.cpp).
file(GLOB _nxrt_themes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/cheats/themes/*.json")
set(_nxrt_look "${CMAKE_CURRENT_SOURCE_DIR}/assets/cheats/look.json")
if(_nxrt_themes AND EXISTS "${_nxrt_look}")
    list(SORT _nxrt_themes)
    set(_inc "")
    set(_list "")
    foreach(_f ${_nxrt_themes} ${_nxrt_look})
        get_filename_component(_n "${_f}" NAME_WE)
        string(MAKE_C_IDENTIFIER "${_n}" _id)
        file(READ "${_f}" _hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
        if(_f STREQUAL _nxrt_look)
            string(APPEND _inc "const unsigned char kLookSpec[] = {${_bytes}0x00};\n")
        else()
            string(APPEND _inc "const unsigned char kTheme_${_id}[] = {${_bytes}0x00};\n")
            string(APPEND _list "    {\"${_n}\", kTheme_${_id}},\n")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_f}")
    endforeach()
    string(APPEND _inc "struct BuiltinTheme { const char* name; const unsigned char* json; };\nconst BuiltinTheme kBuiltinThemes[] = {\n${_list}};\n")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nxrt_gen/cheats_themes.inc" CONTENT "${_inc}" @ONLY)
    target_include_directories(nxrt_gfx PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nxrt_gen")
    target_compile_definitions(nxrt_gfx PRIVATE NXRT_HAVE_CHEATS_THEMES=1)
    unset(_inc)
    unset(_list)
endif()
unset(_hex)
unset(_bytes)
target_link_libraries(nxrt_gfx PUBLIC nxrt nxrt_volk nxrt_vma nxrt_imgui nxrt_tiling nxrt_astc2bc7 nxrt_zstd SDL3::SDL3-static)
if(APPLE)
    target_link_libraries(nxrt_gfx PUBLIC "-framework AppKit" "-framework Metal" "-framework MetalFX" "-framework QuartzCore")
    # the MetalFX backend keeps Metal objects in C++ members: built with ARC
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/platform/mac/gfx/temporal_metalfx.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
# The core calls into the graphics shell (src/core/switch2.cpp: the HDR layer luminance; src/testtools: the screenshot wait), so whatever
# links nxrt links nxrt_gfx too, as the game exe does. Static libraries may depend on each other.
target_link_libraries(nxrt INTERFACE nxrt_gfx)
target_compile_definitions(nxrt_gfx PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

add_executable(nxrt_gfx_demo apps/gfx_demo.cpp)
target_link_libraries(nxrt_gfx_demo PRIVATE nxrt_gfx)
if(WIN32)
    target_link_options(nxrt_gfx_demo PRIVATE /MAP /DEBUG /STACK:8388608)
endif()
foreach(dll ${NXRT_A2B_DLLS})
    add_custom_command(TARGET nxrt_gfx_demo POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_gfx_demo>")
endforeach()

# The launcher (apps/launcher).
if(WIN32)
    include(${CMAKE_CURRENT_LIST_DIR}/launcher.cmake)
endif()

# The cheats overlay's screenshot test (tests/cheats_ui; ported from AnySwitch by Chaos192, github.com/Chaos192/AnySwitch): every main
# screen drawn on the CPU (no window, no GPU) at two UI scales and compared with tests/cheats_ui/ref. Not part of a game build's default
# target.
file(GLOB NXRT_CHEATS_UI_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cheats_ui/*.cpp")
if(NXRT_CHEATS_UI_TEST_SOURCES)
    add_executable(nxrt_cheats_ui_test EXCLUDE_FROM_ALL ${NXRT_CHEATS_UI_TEST_SOURCES})
    target_link_libraries(nxrt_cheats_ui_test PRIVATE nxrt_gfx)
    target_include_directories(nxrt_cheats_ui_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx")
    # its stand-in title folder (tests/cheats_ui/title: icon.jpg, a picture made for the test)
    target_compile_definitions(nxrt_cheats_ui_test PRIVATE NXRT_CHEATS_UI_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cheats_ui")
    if(WIN32)
        target_link_options(nxrt_cheats_ui_test PRIVATE /HIGHENTROPYVA:NO /STACK:16777216)
    elseif(APPLE)
        target_link_options(nxrt_cheats_ui_test PRIVATE "-Wl,-platform_version,macos,12.0,12.0")
    endif()
    foreach(dll ${NXRT_A2B_DLLS})
        add_custom_command(TARGET nxrt_cheats_ui_test POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_cheats_ui_test>")
    endforeach()
endif()

# The F1 menu's screenshot and input test (tests/menu_ui; ported from AnySwitch by Chaos192, github.com/Chaos192/AnySwitch): its pages, pins
# and groups drawn on the CPU (no window, no GPU, the cheats test's rasterizer) at two UI scales and compared with tests/menu_ui/ref, plus
# the toggle, pin and input checks. Not part of a game build.
file(GLOB NXRT_MENU_UI_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/menu_ui/*.cpp")
if(NXRT_MENU_UI_TEST_SOURCES)
    add_executable(nxrt_menu_ui_test EXCLUDE_FROM_ALL ${NXRT_MENU_UI_TEST_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/tests/cheats_ui/soft_raster.cpp"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tests/cheats_ui/blur_ref.cpp")
    target_link_libraries(nxrt_menu_ui_test PRIVATE nxrt_gfx)
    target_include_directories(nxrt_menu_ui_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx")
    target_compile_definitions(nxrt_menu_ui_test PRIVATE NXRT_MENU_UI_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/menu_ui"
                               NXRT_CHEATS_UI_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cheats_ui")
    if(WIN32)
        target_link_options(nxrt_menu_ui_test PRIVATE /HIGHENTROPYVA:NO /STACK:16777216)
    elseif(APPLE)
        target_link_options(nxrt_menu_ui_test PRIVATE "-Wl,-platform_version,macos,12.0,12.0")
    endif()
    foreach(dll ${NXRT_A2B_DLLS})
        add_custom_command(TARGET nxrt_menu_ui_test POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_menu_ui_test>")
    endforeach()
endif()
