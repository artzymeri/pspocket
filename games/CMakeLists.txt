# Native code a game's directory provides (CLAUDE.md rule 6, D-979): games/<title>/spu/ holds native replacements of
# that game's own SPU jobs, as data the runtime's replacement machinery takes (pspocket/spu/replace.hpp): each table
# registers its known builds (program hash, code range, listed functions) with add_build() when the program loads, so
# nothing in the runtime names a title. An OBJECT library linked straight into each executable that registers or
# lifts SPU programs: its objects (and their registrations) are always linked, never dropped as unreferenced archive
# members. The iOS app compiles the same sources into its runtime (app/ios/project.yml), which it force-loads.
file(GLOB pspocket_games_spu_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*/spu/*.c ${CMAKE_CURRENT_SOURCE_DIR}/*/spu/*.cpp)
if(NOT pspocket_games_spu_sources OR NOT TARGET pspocket_spu)
    return()
endif()
add_library(pspocket_games_spu OBJECT ${pspocket_games_spu_sources})
target_link_libraries(pspocket_games_spu PRIVATE pspocket::spu)
target_include_directories(pspocket_games_spu PRIVATE ${PROJECT_SOURCE_DIR}/runtime/spu/replace)
pspocket_set_warnings(pspocket_games_spu)
# Floating-point results must not depend on the compiler, as the SPU runtime's (fp.h).
target_compile_options(pspocket_games_spu PRIVATE -ffp-contract=off -fno-fast-math)

# Whatever registers SPU programs (and so derives the replaced ones) or lifts them (the listed functions are the lift's
# boundaries, D-966).
foreach(t pspocket-boot spu-check spu-native-check spu-lift spu-live pspocket-prepare)
    if(TARGET ${t})
        target_link_libraries(${t} PRIVATE pspocket_games_spu)
    endif()
endforeach()
