# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone cheat-VM test: compiles runtime/src/cheats/cheat_vm.cpp with no other runtime dependency, so it runs everywhere.
#   cmake -S runtime/tests/cheats -B build/cheat-test && cmake --build build/cheat-test && ./build/cheat-test/nxrt_cheat_vm_test
cmake_minimum_required(VERSION 3.20)
project(nxrt_cheat_vm_test CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(nxrt_cheat_vm_test cheat_vm_test.cpp)
target_include_directories(nxrt_cheat_vm_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

# The scanner and AOB tests stub gmem, so they also need no runtime.
add_executable(nxrt_scanner_test scanner_test.cpp)
target_include_directories(nxrt_scanner_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

add_executable(nxrt_aob_test aob_test.cpp)
target_include_directories(nxrt_aob_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

# The engine test stubs gmem/hid/config/log and needs the runtime headers (game.h et al.).
add_executable(nxrt_cheat_engine_test engine_test.cpp)
target_include_directories(nxrt_cheat_engine_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../recomp/include")

# The VM's write router (vmcode.cpp, ASM cheats as live code patches) over a fake game; includes cheat_vm.cpp and vmcode.cpp directly.
add_executable(nxrt_vmcode_test vmcode_test.cpp)
target_include_directories(nxrt_vmcode_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

add_executable(nxrt_export_test export_test.cpp)
target_include_directories(nxrt_export_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

add_executable(nxrt_pointerscan_test pointerscan_test.cpp)
target_include_directories(nxrt_pointerscan_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

# The A64 disassembler/assembler test includes disasm.cpp directly, so it needs no runtime.
add_executable(nxrt_disasm_test disasm_test.cpp)
target_include_directories(nxrt_disasm_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

# The saved-table round trip includes cheattable.cpp directly; no runtime.
add_executable(nxrt_cheattable_test cheattable_test.cpp)
target_include_directories(nxrt_cheattable_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

# CE-style AOB scripts: includes scanner/aob/disasm/aobscript.cpp directly; no runtime.
add_executable(nxrt_aobscript_test aobscript_test.cpp)
target_include_directories(nxrt_aobscript_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")

# Lua scripts: Lua 5.4 from the pinned archive (cmake/lua_deps.cmake, compiled as C++), luavm/scanner/aob.cpp included directly.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/lua_deps.cmake")
add_executable(nxrt_luavm_test luavm_test.cpp)
target_include_directories(nxrt_luavm_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/cheats")
target_link_libraries(nxrt_luavm_test PRIVATE nxrt_lua)

enable_testing()
add_test(NAME cheat_vm COMMAND nxrt_cheat_vm_test)
add_test(NAME scanner COMMAND nxrt_scanner_test)
add_test(NAME aob COMMAND nxrt_aob_test)
add_test(NAME cheat_engine COMMAND nxrt_cheat_engine_test)
add_test(NAME vmcode COMMAND nxrt_vmcode_test)
add_test(NAME cheat_export COMMAND nxrt_export_test)
add_test(NAME pointerscan COMMAND nxrt_pointerscan_test)
add_test(NAME disasm COMMAND nxrt_disasm_test)
add_test(NAME cheattable COMMAND nxrt_cheattable_test)
add_test(NAME aobscript COMMAND nxrt_aobscript_test)
add_test(NAME luavm COMMAND nxrt_luavm_test)
