# PPU recompiler: analyzer, lifter, C code generation, assembler helper and synthetic programs.
add_library(pspocket_recomp_ppu STATIC
    src/code_image.cpp
    src/analyzer.cpp
    src/lifter.cpp
    src/synthetic.cpp)
target_include_directories(pspocket_recomp_ppu PUBLIC include)
target_link_libraries(pspocket_recomp_ppu PUBLIC pspocket::ppu)
pspocket_set_warnings(pspocket_recomp_ppu)
add_library(pspocket::recomp_ppu ALIAS pspocket_recomp_ppu)

# ppu-lift: raw image + seeds -> C translation units.
add_executable(ppu-lift src/ppu_lift_main.cpp)
target_link_libraries(ppu-lift PRIVATE pspocket::recomp_ppu)
pspocket_set_warnings(ppu-lift)

# Builds the self-test benchmark image (used by runtime/selftest/CMakeLists.txt).
add_executable(ppu-selftest-gen src/selftest_gen_main.cpp)
target_link_libraries(ppu-selftest-gen PRIVATE pspocket::recomp_ppu)
pspocket_set_warnings(ppu-selftest-gen)
