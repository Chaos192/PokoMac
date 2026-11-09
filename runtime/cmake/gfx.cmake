# SPDX-License-Identifier: GPL-3.0-or-later
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
target_link_libraries(nxrt_gfx PUBLIC nxrt nxrt_volk nxrt_vma nxrt_imgui nxrt_tiling nxrt_astc2bc7 nxrt_zstd SDL3::SDL3-static)
if(APPLE)
    target_link_libraries(nxrt_gfx PUBLIC "-framework AppKit" "-framework Metal" "-framework QuartzCore")
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
