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
