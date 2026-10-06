# runtime/prepare: preparing a game on the device (route B, stage 4, D-154): the executable translated, compiled into
# a game library, linked and signed, with progress and the time left. Needs the device's compiler (runtime/codegen).
if(NOT TARGET pspocket_codegen)
    return()
endif()
add_library(pspocket_prepare STATIC src/prepare.cpp src/spu.cpp src/system.cpp)
target_include_directories(pspocket_prepare PUBLIC include)
target_link_libraries(pspocket_prepare PUBLIC pspocket::codesign
    PRIVATE pspocket::codegen pspocket::game pspocket::recomp_ppu pspocket::recomp_spu pspocket::recomp_common)
target_compile_definitions(pspocket_prepare PRIVATE PSPOCKET_VERSION="${PROJECT_VERSION}")
pspocket_set_warnings(pspocket_prepare)
add_library(pspocket::prepare ALIAS pspocket_prepare)

# Host tool: a game prepared on the Mac as the phone prepares it.
if(NOT IOS)
    add_executable(pspocket-prepare tools/pspocket_prepare.cpp)
    target_link_libraries(pspocket-prepare PRIVATE pspocket::prepare pspocket::codegen pspocket::game)
    target_compile_definitions(pspocket-prepare PRIVATE PSPOCKET_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # CMake repeats static libraries that other static libraries need; Apple's linker warns about the repeats.
    target_link_options(pspocket-prepare PRIVATE -Wl,-no_warn_duplicate_libraries)
    pspocket_set_warnings(pspocket-prepare)
endif()
