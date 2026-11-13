# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Portions Copyright (c) 2026 Chaos192
# libopus 1.5.2 (BSD-3-Clause, Xiph.Org) for hwopus: the decoder objects and the Switch 2 hardware audio codec (src/services/audio_opus.cpp).
# Fetched by URL into NXRT_DEPS_DIR and checked against its SHA-256, built from source with the rest (its CMake requires 3.16, so the static
# MSVC runtime of CMAKE_MSVC_RUNTIME_LIBRARY applies to it too). The neural DRED / OSCE features stay off: the work-buffer sizes the
# runtime reports do not depend on them anyway (audio_opus.h). NXRT_OPUS=OFF builds without it: the decoders then decode nothing.
option(NXRT_OPUS "Build libopus into the runtime (hwopus decodes Opus; off: every decode fails)" ON)
if(NOT NXRT_OPUS)
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_OPUS=0)
    return()
endif()
include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/deps_dir.cmake)   # NXRT_DEPS_DIR
FetchContent_Declare(opus
    URL https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
if(MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # clang-cl: opus adds no per-file -msse4.1 / -mavx2 for an MSVC-style compiler, so its SSE4.1 and AVX2 paths do not compile. Those
    # are speed-ups chosen at run time; SSE / SSE2 (every x64 CPU) stay on.
    set(OPUS_X86_MAY_HAVE_SSE4_1 OFF CACHE BOOL "" FORCE)
    set(OPUS_X86_MAY_HAVE_AVX2 OFF CACHE BOOL "" FORCE)
    set(OPUS_X86_PRESUME_SSE4_1 OFF CACHE BOOL "" FORCE)
    set(OPUS_X86_PRESUME_AVX2 OFF CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(opus)
target_link_libraries(nxrt PRIVATE opus)
target_compile_definitions(nxrt PRIVATE NXRT_HAVE_OPUS=1)
