add_library(pspocket_disc STATIC
    src/iso9660.cpp
    src/extract.cpp)
target_include_directories(pspocket_disc PUBLIC include)
target_link_libraries(pspocket_disc PUBLIC pspocket::core)
pspocket_set_warnings(pspocket_disc)
add_library(pspocket::disc ALIAS pspocket_disc)

# Host CLI around the same C API the app uses.
add_executable(ps3disc tools/ps3disc.cpp)
target_link_libraries(ps3disc PRIVATE pspocket::disc)
pspocket_set_warnings(ps3disc)
