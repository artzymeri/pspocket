# Process loader: ELF executable -> lv2 process with HLE-linked imports. Uses the toolchain's ELF and module parsers
# (recomp/common), which are portable C++.
add_library(pspocket_loader STATIC src/loader.cpp src/disc.cpp src/lle.cpp src/system.cpp)
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
if(PSPOCKET_GAME_SPU)
    file(GLOB_RECURSE pspocket_game_spu_sources CONFIGURE_DEPENDS ${PSPOCKET_GAME_SPU}/*.c)
    if(NOT pspocket_game_spu_sources)
        message(FATAL_ERROR "PSPOCKET_GAME_SPU=${PSPOCKET_GAME_SPU} holds no lifted programs")
    endif()
    add_library(pspocket_game_spu STATIC ${pspocket_game_spu_sources})
    target_link_libraries(pspocket_game_spu PRIVATE pspocket::spu)
    pspocket_set_warnings(pspocket_game_spu)
    pspocket_set_generated_warnings(pspocket_game_spu)
    target_link_libraries(pspocket-boot PRIVATE pspocket_game_spu)
    target_compile_definitions(pspocket-boot PRIVATE PSPOCKET_HAVE_GAME_SPU=1)
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

# The game's PPU code translated ahead of time (D-092): ppu-lift over the executable's loaded segments
# (--name game_ppu --prefix game_ppu_), from the user's own game, outside the repository (CLAUDE.md rule 1);
# -DPSPOCKET_GAME_PPU=DIR builds it into pspocket-boot, which registers it in the dispatch table. About 800 MB of C
# for GTA V: built without debug information.
set(PSPOCKET_GAME_PPU "" CACHE PATH "Lifted PPU code (ppu-lift output, --name game_ppu) to build into pspocket-boot")
if(PSPOCKET_GAME_PPU)
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
