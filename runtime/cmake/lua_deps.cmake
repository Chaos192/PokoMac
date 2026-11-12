# SPDX-License-Identifier: GPL-3.0-or-later
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
# Lua 5.4.7 (MIT, PUC-Rio): the cheats overlay's scripting (src/cheats/luavm.cpp, docs/CHEATS.md "Lua scripts"). Fetched by URL into
# NXRT_DEPS_DIR and checked against its SHA-256 (as the other pinned sources, gfx_deps.cmake), built as a small static library: the core
# and the libraries a sandboxed script may use (base, coroutine, math, string, table, utf8). io, os, package (loadlib) and debug are not
# built, so a script has no file, process or module access; luavm.cpp also removes dofile / loadfile / string.dump and loads text only.
include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/deps_dir.cmake)   # NXRT_DEPS_DIR
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
FetchContent_Declare(nxrt_lua_src
    URL https://github.com/lua/lua/archive/refs/tags/v5.4.7.tar.gz
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    DOWNLOAD_NAME lua-v5.4.7.tar.gz
    URL_HASH SHA256=5c39111b3fc4c1c9e56671008955a1730f54a15b95e1f1bd0752b868b929d8e3
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(nxrt_lua_src)
set(_nxrt_lua_files lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm
    lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib)
set(_nxrt_lua_sources "")
foreach(f ${_nxrt_lua_files})
    list(APPEND _nxrt_lua_sources "${nxrt_lua_src_SOURCE_DIR}/${f}.c")
endforeach()
# compiled as C++: a Lua error is then a C++ exception, which unwinds the C++ frames of the overlay's API functions (a longjmp would not)
set_source_files_properties(${_nxrt_lua_sources} PROPERTIES LANGUAGE CXX)
add_library(nxrt_lua STATIC ${_nxrt_lua_sources})
target_include_directories(nxrt_lua SYSTEM PUBLIC "${nxrt_lua_src_SOURCE_DIR}")
set_target_properties(nxrt_lua PROPERTIES POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE CXX)
if(MSVC)
    target_compile_options(nxrt_lua PRIVATE /w)
else()
    target_compile_options(nxrt_lua PRIVATE -w)
endif()
