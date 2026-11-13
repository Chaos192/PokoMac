# SPDX-License-Identifier: GPL-3.0-or-later
# Portions Copyright (c) 2026 Chaos192
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# NXRT_DEPS_DIR: where the pinned third-party archives are downloaded to (gfx_deps, opus_deps, lua_deps, tls_deps). One folder shared by
# every build folder and checkout, so a clean build does not download them again. In order:
#   1. the NXRT_DEPS_DIR environment variable, which wins (a machine can keep the archives off its system drive for every build at once);
#   2. -DNXRT_DEPS_DIR=<dir> or the value already in the build folder's cache;
#   3. the host's cache folder: %LOCALAPPDATA%\nxrt\deps (Windows), ~/Library/Caches/nxrt/deps (macOS), $XDG_CACHE_HOME or ~/.cache (else).
# The folder is keyed by file name only, so every archive whose URL ends in a bare tag (vulkan-sdk-1.4.363.0.zip is both Vulkan-Headers'
# and volk's, v3.4.0.zip, v5.4.7.tar.gz) is declared with DOWNLOAD_NAME <project>-<file>; without it two projects overwrote each other's
# archive and a configure extracted Vulkan-Headers as volk ("Cannot find source file .../volk-src/volk.c").
include_guard(GLOBAL)
if(DEFINED ENV{NXRT_DEPS_DIR} AND NOT "$ENV{NXRT_DEPS_DIR}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{NXRT_DEPS_DIR}" _nxrt_deps_env)
    set(NXRT_DEPS_DIR "${_nxrt_deps_env}" CACHE PATH "Where the pinned third-party sources are downloaded to" FORCE)
elseif(NOT DEFINED NXRT_DEPS_DIR)
    if(CMAKE_HOST_WIN32)
        set(_nxrt_deps_default "$ENV{LOCALAPPDATA}/nxrt/deps")
    elseif(CMAKE_HOST_APPLE)
        set(_nxrt_deps_default "$ENV{HOME}/Library/Caches/nxrt/deps")
    elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(_nxrt_deps_default "$ENV{XDG_CACHE_HOME}/nxrt/deps")
    else()
        set(_nxrt_deps_default "$ENV{HOME}/.cache/nxrt/deps")
    endif()
    file(TO_CMAKE_PATH "${_nxrt_deps_default}" _nxrt_deps_default)
    set(NXRT_DEPS_DIR "${_nxrt_deps_default}" CACHE PATH "Where the pinned third-party sources are downloaded to")
endif()
