# recomp/common tests. Every input is synthetic (synthetic.cpp); no game data (CLAUDE.md rule 1).

# Synthetic PS3 binaries, shared by the unit tests and the ps3inspect end-to-end test.
add_library(pspocket_test_synthetic STATIC synthetic.cpp)
target_include_directories(pspocket_test_synthetic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pspocket_test_synthetic PUBLIC pspocket::recomp_common)
pspocket_set_warnings(pspocket_test_synthetic)

pspocket_add_test(test_recomp_common
    SOURCES
        test_crypto.cpp
        test_hash.cpp
        test_nid.cpp
        test_toml_manifest.cpp
        test_elf.cpp
        test_robustness.cpp
        test_self_decrypt.cpp
        test_firmware.cpp
        test_npd.cpp
        test_npdrm.cpp
        test_pkg.cpp
        test_prx_place.cpp
    LIBS pspocket_test_synthetic pspocket::recomp_common)
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(test_recomp_common PRIVATE ZLIB::ZLIB) # the SELF test compresses its segments
endif()
# The parser fuzzing of executables, NPD files and packages (D-603, mutation_fuzz.hpp).
target_sources(test_recomp_common PRIVATE test_fuzz_formats.cpp)

# RPCS3's own NPDRM and package code for the differential tests (D-580, D-583, rpcs3_ref/rpcs3_ref.hpp), from the
# RPCS3 tree the build reads its keys from (PSPOCKET_RPCS3_DIR): its PolarSSL AES and SHA-1 as they are, and the
# functions scripts/rpcs3-extract.py copies out of its key_vault.cpp, utils.cpp, unedat.cpp, unself.cpp and unpkg.cpp.
# All of it in the build directory; without RPCS3's source the tests have nothing of RPCS3's to compare with.
set(rpcs3_crypto "")
foreach(dir "${PSPOCKET_RPCS3_DIR}/rpcs3/Crypto" "${PSPOCKET_RPCS3_DIR}/Crypto")
    if(PSPOCKET_RPCS3_DIR AND NOT rpcs3_crypto AND EXISTS "${dir}/unedat.cpp" AND EXISTS "${dir}/aes.cpp")
        set(rpcs3_crypto "${dir}")
    endif()
endforeach()
if(rpcs3_crypto)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(rpcs3_extracted ${CMAKE_CURRENT_BINARY_DIR}/rpcs3_ref/rpcs3_ref_extracted.inc)
    add_custom_command(
        OUTPUT ${rpcs3_extracted}
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/rpcs3-extract.py ${rpcs3_crypto} ${rpcs3_extracted}
        DEPENDS ${PROJECT_SOURCE_DIR}/scripts/rpcs3-extract.py ${rpcs3_crypto}/key_vault.cpp ${rpcs3_crypto}/utils.cpp
                ${rpcs3_crypto}/unedat.cpp ${rpcs3_crypto}/unedat.h ${rpcs3_crypto}/unself.cpp
                ${rpcs3_crypto}/unpkg.cpp ${rpcs3_crypto}/unpkg.h
        COMMENT "RPCS3's NPDRM and package functions for the tests (scripts/rpcs3-extract.py)"
        VERBATIM)
    add_library(pspocket_rpcs3_ref STATIC rpcs3_ref/rpcs3_ref.cpp ${rpcs3_extracted}
        ${rpcs3_crypto}/aes.cpp ${rpcs3_crypto}/aesni.cpp ${rpcs3_crypto}/sha1.cpp ${rpcs3_crypto}/lz.cpp)
    # The stand-in util/types.hpp before anything of RPCS3's (its own needs the rest of RPCS3).
    target_include_directories(pspocket_rpcs3_ref
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rpcs3_ref ${rpcs3_crypto} ${CMAKE_CURRENT_BINARY_DIR}/rpcs3_ref)
    target_link_libraries(pspocket_rpcs3_ref PUBLIC pspocket::recomp_common)
    target_compile_options(pspocket_rpcs3_ref PRIVATE -w) # RPCS3's code, not held to pspocket's warnings
    target_link_libraries(test_recomp_common PRIVATE pspocket_rpcs3_ref)
    target_compile_definitions(test_recomp_common PRIVATE PSPOCKET_HAVE_RPCS3_REF=1)
endif()
target_compile_definitions(test_recomp_common PRIVATE PSPOCKET_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(PSPOCKET_SANITIZE)
    # Make UBSan findings fail the test instead of only printing them.
    set_tests_properties(test_recomp_common PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()

# ps3inspect end to end: a generator writes synthetic binaries, ps3inspect inventories them, a CMake script
# checks the exit codes and the JSON (string(JSON), no extra dependencies).
add_executable(make_synthetic_ps3_binaries make_synthetic_ps3_binaries.cpp)
target_link_libraries(make_synthetic_ps3_binaries PRIVATE pspocket_test_synthetic)
pspocket_set_warnings(make_synthetic_ps3_binaries)

if(TARGET ps3inspect)
    # Cross builds run both tools through the emulator; '|' stands in for ';' so the list survives add_test.
    string(REPLACE ";" "|" ps3inspect_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")
    add_test(NAME ps3inspect_end_to_end
        COMMAND ${CMAKE_COMMAND}
            -DEMULATOR=${ps3inspect_emulator}
            -DGENERATOR=$<TARGET_FILE:make_synthetic_ps3_binaries>
            -DPS3INSPECT=$<TARGET_FILE:ps3inspect>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/ps3inspect_e2e
            -P ${CMAKE_CURRENT_SOURCE_DIR}/ps3inspect_e2e.cmake)
    if(PSPOCKET_SANITIZE)
        set_tests_properties(ps3inspect_end_to_end PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
endif()
