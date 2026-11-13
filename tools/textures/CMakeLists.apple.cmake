# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Portions Copyright (c) 2026 Chaos192
# macOS (Apple silicon) build of astc2bc7: bc7e.ispc compiled for NEON, astcenc's NEON decompressor linked statically into
# libastc2bc7.a (one archive, so the runtime links astc2bc7 and bc7e_ispc as on Windows). scripts/build_mac.sh runs it.
set(A2B_ISPC "${TP}/ispc/ispc-v1.32.0-macOS.universal/bin/ispc" CACHE FILEPATH "Intel ISPC compiler (BSD-3)")

set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR ON CACHE BOOL "" FORCE)
set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NONE OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/astc-encoder ${CMAKE_BINARY_DIR}/astcenc EXCLUDE_FROM_ALL)

set(BC7E_SRC ${TP}/bc7enc_rdo/bc7e.ispc)
set(BC7E_OUT ${CMAKE_BINARY_DIR}/bc7e)
file(MAKE_DIRECTORY ${BC7E_OUT})
add_custom_command(
    OUTPUT ${BC7E_OUT}/bc7e.o ${BC7E_OUT}/bc7e_ispc.h
    COMMAND ${A2B_ISPC} -O2 ${BC7E_SRC} -o ${BC7E_OUT}/bc7e.o -h ${BC7E_OUT}/bc7e_ispc.h
            --target=neon-i32x4 --arch=aarch64 --target-os=macos
            --opt=fast-math --opt=disable-assertions --woff
    DEPENDS ${BC7E_SRC}
    COMMENT "ispc bc7e.ispc (neon)")
add_library(bc7e_ispc STATIC ${BC7E_OUT}/bc7e.o)
set_target_properties(bc7e_ispc PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(bc7e_ispc INTERFACE ${BC7E_OUT})

add_library(astc2bc7_core STATIC src/a2b.cpp ${BC7E_OUT}/bc7e_ispc.h)
target_include_directories(astc2bc7_core PUBLIC include PRIVATE ${TP}/astc-encoder/Source ${BC7E_OUT})
target_compile_options(astc2bc7_core PRIVATE -Wno-unused-parameter -O2)
add_dependencies(astc2bc7_core astcdec-neon-static bc7e_ispc)

# libastc2bc7.a = the core + the astcenc decompressor (libtool merges the archives).
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/libastc2bc7.a
    COMMAND libtool -static -o ${CMAKE_BINARY_DIR}/libastc2bc7.a $<TARGET_FILE:astc2bc7_core> $<TARGET_FILE:astcdec-neon-static>
    DEPENDS astc2bc7_core astcdec-neon-static
    COMMENT "libastc2bc7.a")
add_custom_target(astc2bc7 ALL DEPENDS ${CMAKE_BINARY_DIR}/libastc2bc7.a bc7e_ispc)
