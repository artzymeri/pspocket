# recomp/common tests. Every input is synthetic (synthetic.cpp); no game data (CLAUDE.md rule 1).

# Synthetic PS3 binaries, shared by the unit tests and the ps3inspect end-to-end test.
add_library(pspocket_test_synthetic STATIC synthetic.cpp)
target_include_directories(pspocket_test_synthetic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pspocket_test_synthetic PUBLIC pspocket::recomp_common)
pspocket_set_warnings(pspocket_test_synthetic)

pspocket_add_test(test_recomp_common
    SOURCES
        test_hash.cpp
        test_nid.cpp
        test_toml_manifest.cpp
        test_elf.cpp
        test_robustness.cpp
        test_self_decrypt.cpp
    LIBS pspocket_test_synthetic pspocket::recomp_common)
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(test_recomp_common PRIVATE ZLIB::ZLIB) # the SELF test compresses its segments
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
