pspocket_add_test(test_disc
    SOURCES test_disc.cpp test_psn.cpp
    LIBS pspocket::loader pspocket::disc pspocket_test_synthetic pspocket::recomp_common)
target_include_directories(test_disc PRIVATE ${PROJECT_SOURCE_DIR}/tests/common)
# The parser fuzzing of disc images and PARAM.SFO (D-603, tests/common/mutation_fuzz.hpp).
target_sources(test_disc PRIVATE test_fuzz_disc.cpp)
