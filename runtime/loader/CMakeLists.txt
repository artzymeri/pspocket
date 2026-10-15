# Process loader: ELF executable -> lv2 process with HLE-linked imports. Uses the toolchain's ELF and module parsers
# (recomp/common), which are portable C++.
add_library(pspocket_loader STATIC src/loader.cpp src/disc.cpp src/game_modules.cpp src/hdd_game.cpp src/lle.cpp
    src/system.cpp)
target_include_directories(pspocket_loader PUBLIC include)
target_link_libraries(pspocket_loader PUBLIC pspocket::hle pspocket::lv2 pspocket::recomp_common
    PRIVATE pspocket::disc pspocket::game)
pspocket_set_warnings(pspocket_loader)
add_library(pspocket::loader ALIAS pspocket_loader)

# Bring-up host: runs an executable under the interpreter until it exits or hits a gap.
add_executable(pspocket-boot tools/boot_main.cpp)
target_link_libraries(pspocket-boot PRIVATE pspocket::loader pspocket::game)
# --gpu metal / --frames: the game's frames through the Metal backend, saved as PNG (Apple platforms).
if(TARGET pspocket::rsx_metal)
    target_link_libraries(pspocket-boot PRIVATE pspocket::rsx_metal)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_METAL=1)
endif()
# --audio: sound out of cellAudio through AVAudioEngine (Apple platforms).
if(TARGET pspocket::platform_apple)
    target_link_libraries(pspocket-boot PRIVATE pspocket::platform_apple)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_AUDIO_OUTPUT=1)
endif()
# --spu-translate: SPU programs translated while the game runs (D-236), with the device's compiler when this build has
# it (-DPSPOCKET_LLVM_DIR).
if(TARGET pspocket_prepare)
    target_link_libraries(pspocket-boot PRIVATE pspocket::prepare pspocket::codegen)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_SESSION_TRANSLATOR=1
        PSPOCKET_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    if(APPLE)
        target_link_options(pspocket-boot PRIVATE -Wl,-no_warn_duplicate_libraries)
    endif()
endif()
pspocket_set_warnings(pspocket-boot)

# The game's lifted SPU programs (D-076): scripts/lift-spu.sh writes them, from the user's own game, outside the
# repository (CLAUDE.md rule 1); -DPSPOCKET_GAME_SPU=DIR builds them into pspocket-boot. Lifted code is never built
# with -ffast-math (CLAUDE.md).
set(PSPOCKET_GAME_SPU "" CACHE PATH "Lifted SPU programs (scripts/lift-spu.sh output) to build into pspocket-boot")
# A second build directory (an agent's clone, a sanitizer build) can link the game's code another build compiled from
# the same lift instead of compiling it again (an hour and more for GTA V): -DPSPOCKET_GAME_SPU_LIB and
# -DPSPOCKET_GAME_PPU_LIB name that build's libpspocket_game_spu.a and libpspocket_game_ppu.a. Only while this tree's
# ABI headers (PSPOCKET_GAME_ABI_HEADERS) are the ones it was built with.
set(PSPOCKET_GAME_SPU_LIB "" CACHE FILEPATH "libpspocket_game_spu.a of another build of PSPOCKET_GAME_SPU, linked as it is")
set(PSPOCKET_GAME_PPU_LIB "" CACHE FILEPATH "libpspocket_game_ppu.a of another build of PSPOCKET_GAME_PPU, linked as it is")
if(PSPOCKET_GAME_SPU AND PSPOCKET_GAME_SPU_LIB)
    add_library(pspocket_game_spu STATIC IMPORTED)
    set_target_properties(pspocket_game_spu PROPERTIES IMPORTED_LOCATION ${PSPOCKET_GAME_SPU_LIB}
        INTERFACE_LINK_LIBRARIES pspocket::spu)
    target_link_libraries(pspocket-boot PRIVATE pspocket_game_spu)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_GAME_SPU=1)
elseif(PSPOCKET_GAME_SPU)
    file(GLOB_RECURSE pspocket_game_spu_sources CONFIGURE_DEPENDS ${PSPOCKET_GAME_SPU}/*.c)
    if(NOT pspocket_game_spu_sources)
        message(FATAL_ERROR "PSPOCKET_GAME_SPU=${PSPOCKET_GAME_SPU} holds no lifted programs")
    endif()
    # The cold twins of the translated functions (spu-lift's N_resume_*.c, D-961) at -O1: half the compile time, and
    # they run only when the run loop resumes a function after a label.
    set(pspocket_game_spu_twins ${pspocket_game_spu_sources})
    list(FILTER pspocket_game_spu_twins INCLUDE REGEX "_resume_[0-9]+\\.c$")
    if(pspocket_game_spu_twins)
        set_source_files_properties(${pspocket_game_spu_twins} PROPERTIES COMPILE_OPTIONS -O1)
    endif()
    add_library(pspocket_game_spu STATIC ${pspocket_game_spu_sources})
    target_link_libraries(pspocket_game_spu PRIVATE pspocket::spu)
    pspocket_set_warnings(pspocket_game_spu)
    pspocket_set_generated_warnings(pspocket_game_spu)
    target_link_libraries(pspocket-boot PRIVATE pspocket_game_spu)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_GAME_SPU=1)
endif()
# A second translation of the game's SPU programs, to compare two translations in one session (D-962):
# scripts/spu-ab-lift.sh renames a lift's symbols (spb_ for spu_) into DIR, -DPSPOCKET_GAME_SPU_B=DIR builds it in, and
# PSPOCKET_SPU_AB=S switches the SPURS workers to it every other S seconds.
set(PSPOCKET_GAME_SPU_B "" CACHE PATH "A second lift (scripts/spu-ab-lift.sh output) for PSPOCKET_SPU_AB's half B")
if(PSPOCKET_GAME_SPU_B AND PSPOCKET_GAME_SPU)
    file(GLOB_RECURSE pspocket_game_spu_b_sources CONFIGURE_DEPENDS ${PSPOCKET_GAME_SPU_B}/*.c)
    set(pspocket_game_spu_b_twins ${pspocket_game_spu_b_sources})
    list(FILTER pspocket_game_spu_b_twins INCLUDE REGEX "_resume_[0-9]+\\.c$")
    if(pspocket_game_spu_b_twins)
        set_source_files_properties(${pspocket_game_spu_b_twins} PROPERTIES COMPILE_OPTIONS -O1)
    endif()
    add_library(pspocket_game_spu_b STATIC ${pspocket_game_spu_b_sources})
    target_link_libraries(pspocket_game_spu_b PRIVATE pspocket::spu)
    pspocket_set_warnings(pspocket_game_spu_b)
    pspocket_set_generated_warnings(pspocket_game_spu_b)
    target_link_libraries(pspocket-boot PRIVATE pspocket_game_spu_b)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_GAME_SPU_B=1)
endif()
# spu-check DUMP: translated SPU functions against the interpreter from random states (D-132): the game's built in
# (-DPSPOCKET_GAME_SPU), or a game or system library's (--library, D-191).
add_executable(spu-check tools/spu_check.cpp)
target_link_libraries(spu-check PRIVATE pspocket::game pspocket::spu pspocket::core)
if(PSPOCKET_GAME_SPU)
    target_link_libraries(spu-check PRIVATE pspocket_game_spu)
    target_compile_definitions(spu-check PRIVATE PSPOCKET_HAVE_GAME_SPU=1)
endif()
if(APPLE)
    target_link_options(spu-check PRIVATE -Wl,-no_warn_duplicate_libraries) # the static libraries pspocket::game brings again
endif()
pspocket_set_warnings(spu-check)
# spu-native-check CAPTURE...: native replacements of SPU middleware functions against the lifted ones, on states
# captured from the game (D-965, PSPOCKET_SPU_CAPTURE).
add_executable(spu-native-check tools/spu_native_check.cpp)
target_link_libraries(spu-native-check PRIVATE pspocket::game pspocket::spu pspocket::core)
if(PSPOCKET_GAME_SPU)
    target_link_libraries(spu-native-check PRIVATE pspocket_game_spu)
    target_compile_definitions(spu-native-check PRIVATE PSPOCKET_HAVE_GAME_SPU=1)
endif()
if(APPLE)
    target_link_options(spu-native-check PRIVATE -Wl,-no_warn_duplicate_libraries)
endif()
pspocket_set_warnings(spu-native-check)
# spu-job-replay TRACE_DIR: SPURS jobs replayed from a job trace (PSPOCKET_JOB_TRACE, D-987), their writes against the
# game's; --coverage: the instructions they run.
add_executable(spu-job-replay tools/spu_job_replay.cpp)
target_link_libraries(spu-job-replay PRIVATE pspocket::game pspocket::spu pspocket::core)
if(PSPOCKET_GAME_SPU)
    target_link_libraries(spu-job-replay PRIVATE pspocket_game_spu)
    target_compile_definitions(spu-job-replay PRIVATE PSPOCKET_HAVE_GAME_SPU=1)
endif()
if(APPLE)
    target_link_options(spu-job-replay PRIVATE -Wl,-no_warn_duplicate_libraries)
endif()
pspocket_set_warnings(spu-job-replay)
# edge-gpu-check PROBE_DIR: EDGE geometry's GPU path (the stages' stand-ins and the kernels' code on the CPU) against
# the SPU, on states of the game's jobs saved by spu-job-replay --probe (D-988).
add_executable(edge-gpu-check tools/edge_gpu_check.cpp)
target_link_libraries(edge-gpu-check PRIVATE pspocket::spu pspocket::core)
target_include_directories(edge-gpu-check PRIVATE ${PROJECT_SOURCE_DIR}/runtime/rsx/metal/src)
pspocket_set_warnings(edge-gpu-check)

# Programs lifted again alone (a lifter change that touches a few programs, D-965), compiled into pspocket-boot,
# spu-check and spu-native-check ahead of the game's other lifted programs: each overrides the copy in
# PSPOCKET_GAME_SPU_LIB, whose members for it the linker then leaves out (the same symbols are already defined).
set(PSPOCKET_GAME_SPU_EXTRA "" CACHE PATH "Lifted SPU programs (spu-lift output) linked ahead of the game's library")
if(PSPOCKET_GAME_SPU_EXTRA AND PSPOCKET_GAME_SPU)
    file(GLOB_RECURSE pspocket_game_spu_extra_sources CONFIGURE_DEPENDS ${PSPOCKET_GAME_SPU_EXTRA}/*.c)
    add_library(pspocket_game_spu_extra OBJECT ${pspocket_game_spu_extra_sources})
    target_link_libraries(pspocket_game_spu_extra PRIVATE pspocket::spu)
    pspocket_set_warnings(pspocket_game_spu_extra)
    pspocket_set_generated_warnings(pspocket_game_spu_extra)
    foreach(t pspocket-boot spu-check spu-native-check spu-job-replay)
        target_link_libraries(${t} PRIVATE pspocket_game_spu_extra)
    endforeach()
endif()

# The game's PPU code translated ahead of time (D-092): ppu-lift over the executable's loaded segments
# (--name game_ppu --prefix game_ppu_), from the user's own game, outside the repository (CLAUDE.md rule 1);
# -DPSPOCKET_GAME_PPU=DIR builds it into pspocket-boot, which registers it in the dispatch table. About 800 MB of C
# for GTA V: built without debug information.
set(PSPOCKET_GAME_PPU "" CACHE PATH "Lifted PPU code (ppu-lift output, --name game_ppu) to build into pspocket-boot")
if(PSPOCKET_GAME_PPU AND PSPOCKET_GAME_PPU_LIB)
    add_library(pspocket_game_ppu STATIC IMPORTED)
    set_target_properties(pspocket_game_ppu PROPERTIES IMPORTED_LOCATION ${PSPOCKET_GAME_PPU_LIB}
        INTERFACE_LINK_LIBRARIES pspocket::ppu)
    target_link_libraries(pspocket-boot PRIVATE pspocket_game_ppu)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_GAME_PPU=1)
elseif(PSPOCKET_GAME_PPU)
    file(GLOB pspocket_game_ppu_sources CONFIGURE_DEPENDS ${PSPOCKET_GAME_PPU}/*.c)
    if(NOT pspocket_game_ppu_sources)
        message(FATAL_ERROR "PSPOCKET_GAME_PPU=${PSPOCKET_GAME_PPU} holds no lifted code")
    endif()
    add_library(pspocket_game_ppu STATIC ${pspocket_game_ppu_sources})
    target_link_libraries(pspocket_game_ppu PRIVATE pspocket::ppu)
    target_include_directories(pspocket_game_ppu PRIVATE ${PSPOCKET_GAME_PPU})
    target_compile_options(pspocket_game_ppu PRIVATE -g0)
    pspocket_set_warnings(pspocket_game_ppu)
    pspocket_set_generated_warnings(pspocket_game_ppu)
    target_link_libraries(pspocket-boot PRIVATE pspocket_game_ppu)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_GAME_PPU=1)
endif()
