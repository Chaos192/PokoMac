# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch) (without its jit service plugin).
# nxrt_missed_test (tests/missed/missed_test.cpp): code the lift did not see, end to end on a hand-made module. The module is written and
# lifted by recomp/tests/missed_fixture.py (Python with Capstone: NXRT_TEST_PYTHON) at build time, so the test always runs the lifter of
# this checkout. Run: nxrt_missed_test --work <dir> --python <python> --zig <zig> [--expect cached]   (twice: fresh, then cached)
get_filename_component(_nxrt_recomp_dir "${NXRT_RECOMP_INCLUDE}/.." ABSOLUTE)
set(_fx "${CMAKE_CURRENT_BINARY_DIR}/missed_fixture")
file(GLOB _nxrt_lifter_py CONFIGURE_DEPENDS "${_nxrt_recomp_dir}/nxrecomp/*.py" "${_nxrt_recomp_dir}/nxrecomp/lift/*.py" "${_nxrt_recomp_dir}/nxrecomp/lift/tier2/*.py")
add_custom_command(
    OUTPUT "${_fx}/lift/c_000.cpp" "${_fx}/lift/module_fixture.cpp" "${_fx}/lift/funcs.h" "${_fx}/fixture.map.json.gz" "${_fx}/fixture.elf"
    COMMAND "${NXRT_TEST_PYTHON}" -I "${_nxrt_recomp_dir}/tests/missed_fixture.py" "${_fx}"
    DEPENDS "${_nxrt_recomp_dir}/tests/missed_fixture.py" ${_nxrt_lifter_py}
    COMMENT "Lifting the missed-code test module"
    VERBATIM)
add_executable(nxrt_missed_test EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/tests/missed/missed_test.cpp" "${_fx}/lift/c_000.cpp" "${_fx}/lift/module_fixture.cpp")
target_include_directories(nxrt_missed_test PRIVATE "${_fx}/lift")
target_compile_definitions(nxrt_missed_test PRIVATE
    NXRT_FIXTURE_MAP="${_fx}/fixture.map.json.gz" NXRT_FIXTURE_ELF="${_fx}/fixture.elf" NXRT_RECOMP_DIR="${_nxrt_recomp_dir}"
    NXRT_PKP_BUILD="${CMAKE_CURRENT_SOURCE_DIR}/../game-build/release_b/pkp_build.py")
target_link_libraries(nxrt_missed_test PRIVATE nxrt)
if(WIN32)
    target_link_options(nxrt_missed_test PRIVATE /HIGHENTROPYVA:NO /STACK:16777216)
else()
    target_link_options(nxrt_missed_test PRIVATE "-Wl,-platform_version,macos,12.0,12.0")
endif()
