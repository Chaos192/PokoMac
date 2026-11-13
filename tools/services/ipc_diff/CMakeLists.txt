# SPDX-License-Identifier: GPL-3.0-or-later
# The service differential build (ipc_diff.py). Two implementations of the same runtime source files answer the same requests:
#   nxrt_ipc_replay          the runtime library as it is in the tree
#   nxrt_ipc_replay_alt      the same program with the NXRT_IPC_DIFF_ALT files (the other implementation of some of the library's sources:
#                            an older one taken from git, or a new one not in the tree yet) linked ahead of the library
#   nxrt_services_test_alt   the service tests (runtime/tests/services) on the alt files
# The alt objects define every symbol the library's own copies of those files define, so those copies are never pulled in (a symbol that
# only the library's copy defines and something uses is a duplicate-symbol link error: the two copies must have the same interface).
# NXRT_IPC_DIFF_ALT_QUIET (default on) compiles the alt files without warnings (-w), for old files nobody may read, the build output
# included.
cmake_minimum_required(VERSION 3.25)
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(nxrt_ipc_diff C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NXRT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime" CACHE PATH "The runtime source folder")
set(NXRT_IPC_DIFF_ALT "" CACHE STRING "The other implementation's source files (absolute paths)")
option(NXRT_IPC_DIFF_ALT_QUIET "Compile the alt files without warnings" ON)
get_filename_component(NXRT_ROOT "${NXRT_ROOT}" ABSOLUTE)
set(NXRT_BUILD_GAMES OFF CACHE BOOL "" FORCE)
add_subdirectory("${NXRT_ROOT}" nxrt)

# the runtime folder's own compile definitions and options (NOMINMAX, WIN32_LEAN_AND_MEAN, its warning flags) for the targets made here
get_directory_property(NXRT_DIR_DEFS DIRECTORY "${NXRT_ROOT}" COMPILE_DEFINITIONS)
get_directory_property(NXRT_DIR_OPTS DIRECTORY "${NXRT_ROOT}" COMPILE_OPTIONS)
function(nxrt_ipc_exe name)
    add_executable(${name} EXCLUDE_FROM_ALL ${ARGN})
    target_link_libraries(${name} PRIVATE nxrt)
    target_compile_definitions(${name} PRIVATE ${NXRT_DIR_DEFS})
    target_compile_options(${name} PRIVATE ${NXRT_DIR_OPTS})
    if(WIN32)
        target_link_options(${name} PRIVATE /HIGHENTROPYVA:NO /STACK:16777216)   # guest ranges stay free, as for the game exe
    elseif(APPLE)
        target_link_options(${name} PRIVATE "-Wl,-platform_version,macos,12.0,12.0")
    endif()
endfunction()
set(_replay "${NXRT_ROOT}/tests/ipc_replay/replay_main.cpp")
nxrt_ipc_exe(nxrt_ipc_replay ${_replay})

if(NXRT_IPC_DIFF_ALT)
    # the alt files, compiled as the library compiles its own sources: its target and folder properties, its usage requirements
    add_library(nxrt_ipc_alt OBJECT ${NXRT_IPC_DIFF_ALT})
    foreach(p COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES COMPILE_FEATURES)
        get_target_property(_v nxrt ${p})
        if(_v)
            set_property(TARGET nxrt_ipc_alt APPEND PROPERTY ${p} "${_v}")
        endif()
    endforeach()
    get_directory_property(_dd DIRECTORY "${NXRT_ROOT}" COMPILE_DEFINITIONS)
    if(_dd)
        set_property(TARGET nxrt_ipc_alt APPEND PROPERTY COMPILE_DEFINITIONS "${_dd}")
    endif()
    target_link_libraries(nxrt_ipc_alt PRIVATE nxrt)
    if(TARGET pokopia_coop)   # the library links it privately; network.cpp's and ldn.cpp's #include "coop.h" needs its folder here too
        target_link_libraries(nxrt_ipc_alt PRIVATE pokopia_coop)
    endif()
    # the alt files' own #include "x.h" of their neighbours resolve in the source folders
    target_include_directories(nxrt_ipc_alt BEFORE PRIVATE "${NXRT_ROOT}/src/services" "${NXRT_ROOT}/src")
    if(NXRT_IPC_DIFF_ALT_QUIET)
        if(MSVC)
            target_compile_options(nxrt_ipc_alt PRIVATE /w)
        else()
            target_compile_options(nxrt_ipc_alt PRIVATE -w)
        endif()
    endif()
    nxrt_ipc_exe(nxrt_ipc_replay_alt ${_replay} $<TARGET_OBJECTS:nxrt_ipc_alt>)
    # the service tests on the alt files, as runtime/CMakeLists.txt builds nxrt_services_test
    file(GLOB _svc_tests CONFIGURE_DEPENDS "${NXRT_ROOT}/tests/services/*.cpp")
    nxrt_ipc_exe(nxrt_services_test_alt ${_svc_tests} $<TARGET_OBJECTS:nxrt_ipc_alt>)
    target_include_directories(nxrt_services_test_alt PRIVATE "${NXRT_ROOT}/tests/services" "${NXRT_ROOT}/src/services")
    if(TARGET opus)
        target_link_libraries(nxrt_services_test_alt PRIVATE opus)
        target_compile_definitions(nxrt_services_test_alt PRIVATE NXRT_TEST_OPUS=1)
    endif()
    if(TARGET mbedtls)
        target_link_libraries(nxrt_services_test_alt PRIVATE mbedtls mbedx509 mbedcrypto)
        target_compile_definitions(nxrt_services_test_alt PRIVATE NXRT_TEST_TLS=1)
    endif()
endif()
