# Game libraries (route B, D-152): made from the synthetic self-test program with the device's compiler, loaded and
# run. Only with an LLVM build (PSPOCKET_LLVM_DIR) and the generated self-test program.
if(TARGET pspocket_codegen AND TARGET pspocket_selftest_ppu_generated)
    pspocket_add_test(test_game_library
        SOURCES test_game_library.cpp ${CMAKE_BINARY_DIR}/generated/selftest/ps_selftest_ppu_image.c
        LIBS pspocket::game pspocket::codegen pspocket::codesign pspocket::prepare)
    add_dependencies(test_game_library pspocket_selftest_ppu_generated)
    if(APPLE)
        # The static libraries prepare brings come again with the test's own (as pspocket-prepare's).
        target_link_options(test_game_library PRIVATE -Wl,-no_warn_duplicate_libraries)
    endif()
    target_include_directories(test_game_library PRIVATE ${CMAKE_BINARY_DIR}/generated/selftest)
    target_compile_definitions(test_game_library PRIVATE PSPOCKET_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PSPOCKET_SELFTEST_GENERATED_DIR="${CMAKE_BINARY_DIR}/generated/selftest")
endif()
