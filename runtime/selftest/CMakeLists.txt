add_library(pspocket_selftest STATIC
    src/selftest.cpp
    src/probes_core.cpp)
target_include_directories(pspocket_selftest PUBLIC include PRIVATE src)
target_link_libraries(pspocket_selftest PUBLIC pspocket::core)
target_compile_definitions(pspocket_selftest PRIVATE PSPOCKET_VERSION="${PROJECT_VERSION}")
pspocket_set_warnings(pspocket_selftest)
add_library(pspocket::selftest ALIAS pspocket_selftest)
