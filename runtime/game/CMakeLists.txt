# runtime/game: a game's translated code as a library loaded at run time (route B, D-152): the ABI between the host
# and a game library (include/pspocket/game/library.h), the host's side (src/host.cpp: the runtime table, loading),
# and the library's entry (library/library_entry.c: compiled into every game library, never into the host).

# The headers translated code includes, and this ABI's own: their hash is the library's compatibility key (the
# library's entry is not in it: what it binds is library.h's layout, and its own code travels in the library).
set(PSPOCKET_GAME_ABI_HEADERS
    ${PROJECT_SOURCE_DIR}/runtime/core/include/pspocket/core/be.h
    ${PROJECT_SOURCE_DIR}/runtime/ppu/include/pspocket/ppu/context.h
    ${PROJECT_SOURCE_DIR}/runtime/ppu/include/pspocket/ppu/fpu.h
    ${PROJECT_SOURCE_DIR}/runtime/ppu/include/pspocket/ppu/lifted.h
    ${PROJECT_SOURCE_DIR}/runtime/ppu/include/pspocket/ppu/runtime.h
    ${PROJECT_SOURCE_DIR}/runtime/ppu/include/pspocket/ppu/vmx.h
    ${PROJECT_SOURCE_DIR}/runtime/spu/include/pspocket/spu/context.h
    ${PROJECT_SOURCE_DIR}/runtime/spu/include/pspocket/spu/fp.h
    ${PROJECT_SOURCE_DIR}/runtime/spu/include/pspocket/spu/lifted.h
    ${PROJECT_SOURCE_DIR}/runtime/spu/include/pspocket/spu/runtime.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/pspocket/game/library.h
    CACHE INTERNAL "")
# What translates: the lifters, and the decoders they share with the interpreters.
file(GLOB_RECURSE pspocket_game_translator_files CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/recomp/ppu/src/*.cpp ${PROJECT_SOURCE_DIR}/recomp/ppu/include/*.hpp
    ${PROJECT_SOURCE_DIR}/recomp/spu/src/*.cpp ${PROJECT_SOURCE_DIR}/recomp/spu/include/*.hpp
    ${PROJECT_SOURCE_DIR}/runtime/ppu/include/pspocket/ppu/decoder.hpp ${PROJECT_SOURCE_DIR}/runtime/ppu/include/pspocket/ppu/ops.inc
    ${PROJECT_SOURCE_DIR}/runtime/spu/include/pspocket/spu/decoder.hpp ${PROJECT_SOURCE_DIR}/runtime/spu/include/pspocket/spu/ops.inc)
list(SORT pspocket_game_translator_files)
set(pspocket_game_abi_source ${CMAKE_BINARY_DIR}/generated/game/game_abi.c)
add_custom_command(OUTPUT ${pspocket_game_abi_source}
    COMMAND ${CMAKE_COMMAND} "-DOUT=${pspocket_game_abi_source}" "-DABI_FILES=${PSPOCKET_GAME_ABI_HEADERS}"
        "-DTRANSLATOR_FILES=${pspocket_game_translator_files}" -P ${PROJECT_SOURCE_DIR}/cmake/GameAbiHash.cmake
    DEPENDS ${PSPOCKET_GAME_ABI_HEADERS} ${pspocket_game_translator_files} ${PROJECT_SOURCE_DIR}/cmake/GameAbiHash.cmake
    COMMENT "Hashing the game library ABI and the lifters"
    VERBATIM)

add_library(pspocket_game STATIC src/host.cpp ${pspocket_game_abi_source})
target_include_directories(pspocket_game PUBLIC include)
target_link_libraries(pspocket_game PUBLIC pspocket::ppu pspocket::spu pspocket::core PRIVATE pspocket::codesign)
# Where the library's entry source is, for the tools that make game libraries on this machine.
target_compile_definitions(pspocket_game PUBLIC
    PSPOCKET_GAME_LIBRARY_ENTRY_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/library/library_entry.c")
pspocket_set_warnings(pspocket_game)
add_library(pspocket::game ALIAS pspocket_game)
