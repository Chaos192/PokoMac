# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Toolchain file: LLVM clang-cl (C:\Program Files\LLVM\bin) with the MSVC x64 environment (run through scripts/nx_env.cmd,
# or from a shell where vcvars64.bat has run), Ninja, optional sccache.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER "C:/Program Files/LLVM/bin/clang-cl.exe")
set(CMAKE_CXX_COMPILER "C:/Program Files/LLVM/bin/clang-cl.exe")
set(CMAKE_LINKER "C:/Program Files/LLVM/bin/lld-link.exe")
set(CMAKE_RC_COMPILER "C:/Program Files/LLVM/bin/llvm-rc.exe")
set(CMAKE_MT "C:/Program Files/LLVM/bin/llvm-mt.exe")
# static MSVC runtime (/MT): the lifted libraries and the runtime that links them must agree, and players then need no
# Visual C++ Redistributable (CMake needs policy CMP0091 NEW, i.e. cmake_minimum_required 3.15 or later, for this to apply)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# the debug info format of an exe built here is not useful to the oracles: keep objects small
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "")
if(NOT DEFINED ENV{NX_NO_SCCACHE})
  find_program(NX_SCCACHE sccache HINTS "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links")
  if(NX_SCCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "${NX_SCCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${NX_SCCACHE}")
  endif()
endif()
