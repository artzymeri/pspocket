# SPU recompiler: program images (SPU ELF / raw LS), analyzer, lifter (C11 generation), assembler helper.
add_library(pspocket_recomp_spu STATIC
    src/analyzer.cpp
    src/image.cpp
    src/lifter.cpp
    src/liveness.cpp)
target_include_directories(pspocket_recomp_spu PUBLIC include)
target_link_libraries(pspocket_recomp_spu PUBLIC pspocket::spu pspocket::recomp_common)
pspocket_set_warnings(pspocket_recomp_spu)
add_library(pspocket::recomp_spu ALIAS pspocket_recomp_spu)

# spu-lift: SPU ELF or raw LS image -> C translation units.
add_executable(spu-lift src/spu_lift_main.cpp)
target_link_libraries(spu-lift PRIVATE pspocket::recomp_spu)
pspocket_set_warnings(spu-lift)

# spu-live: interprocedural register liveness of an SPU program (what a native replacement must leave exact, D-965).
add_executable(spu-live src/spu_live_main.cpp)
target_link_libraries(spu-live PRIVATE pspocket::recomp_spu)
pspocket_set_warnings(spu-live)
