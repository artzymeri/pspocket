# Firmware modules on the host (HLE) and the per-process import linkage. Portable C++; the iOS app compiles src/ too.
add_library(pspocket_hle STATIC
    src/modules.cpp
    src/process.cpp
    src/sysPrxForUser.cpp)
target_include_directories(pspocket_hle PUBLIC include)
target_link_libraries(pspocket_hle PUBLIC pspocket::core pspocket::ppu pspocket::lv2)
pspocket_set_warnings(pspocket_hle)
add_library(pspocket::hle ALIAS pspocket_hle)
