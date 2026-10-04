# Process loader: ELF executable -> lv2 process with HLE-linked imports. Uses the toolchain's ELF and module parsers
# (recomp/common), which are portable C++.
add_library(pspocket_loader STATIC src/loader.cpp src/disc.cpp)
target_include_directories(pspocket_loader PUBLIC include)
target_link_libraries(pspocket_loader PUBLIC pspocket::hle pspocket::lv2 pspocket::recomp_common PRIVATE pspocket::disc)
pspocket_set_warnings(pspocket_loader)
add_library(pspocket::loader ALIAS pspocket_loader)

# Bring-up host: runs an executable under the interpreter until it exits or hits a gap.
add_executable(pspocket-boot tools/boot_main.cpp)
target_link_libraries(pspocket-boot PRIVATE pspocket::loader)
pspocket_set_warnings(pspocket-boot)
