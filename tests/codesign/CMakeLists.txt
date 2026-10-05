pspocket_add_test(test_codesign
    SOURCES test_codesign.cpp
    LIBS pspocket::codesign)
if(APPLE)
    target_link_libraries(test_codesign PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
