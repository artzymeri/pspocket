# pspocket_add_test(<name> SOURCES ... LIBS ...): a Catch2 test executable registered with CTest.
function(pspocket_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    # own_temp_dir.cpp: a temporary directory of the process's own (D-924).
    add_executable(${name} ${T_SOURCES} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/own_temp_dir.cpp)
    target_link_libraries(${name} PRIVATE pspocket::catch2 ${T_LIBS})
    pspocket_set_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

foreach(sub core common ppu spu lv2 hle rsx disc codesign codegen game homebrew games)
    pspocket_component_enabled(tests/${sub} enabled)
    if(enabled AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${sub}/CMakeLists.txt)
        add_subdirectory(${sub})
    endif()
endforeach()
