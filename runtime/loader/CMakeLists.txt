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
