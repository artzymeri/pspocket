pspocket_add_test(test_disc
    SOURCES test_disc.cpp
    LIBS pspocket::loader pspocket::disc pspocket_test_synthetic pspocket::recomp_common)
target_include_directories(test_disc PRIVATE ${PROJECT_SOURCE_DIR}/tests/common)
