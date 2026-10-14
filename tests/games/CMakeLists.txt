# Native code of games' own SPU jobs (games/<title>/spu, D-979) against oracles built from the lifted code's helpers,
# on synthetic inputs (CLAUDE.md rule 1).
if(TARGET pspocket_games_spu)
    pspocket_add_test(test_games SOURCES test_gta5_audio.cpp gta5_audio_oracle.c LIBS pspocket::spu pspocket_games_spu)
    target_include_directories(test_games PRIVATE ${PROJECT_SOURCE_DIR}/games/gta5/spu)
    if(APPLE)
        target_link_options(test_games PRIVATE -Wl,-no_warn_duplicate_libraries) # the SPU library the objects bring again
    endif()
    # The oracles are C over pspocket/spu/lifted.h, as the native code: no contraction, no fast-math (fp.h).
    set_source_files_properties(gta5_audio_oracle.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
endif()
