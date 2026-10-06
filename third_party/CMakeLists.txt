# Pinned third-party code. Each entry records its version and licence in third_party/README.md.

if(PSPOCKET_BUILD_TESTS)
    # Catch2 v3.16.0, amalgamated (BSL-1.0)
    add_library(pspocket_catch2 STATIC catch2/catch_amalgamated.cpp)
    target_include_directories(pspocket_catch2 SYSTEM PUBLIC catch2)
    add_library(pspocket::catch2 ALIAS pspocket_catch2)
endif()

# FFmpeg 7.1.1's LGPL decoders (D-181), built by scripts/build-ffmpeg.sh into build/ffmpeg/{macos,ios}: pspocket's own
# media libraries (cellVdec, cellAdec, cellAtrac, cellDmux) decode with them as RPCS3's do. Optional: without them
# those libraries build without their decoders (code under #if PSPOCKET_HAVE_FFMPEG), and decoding fails, logged.
set(PSPOCKET_FFMPEG_DIR "" CACHE PATH "FFmpeg prefix (include/, lib/); default: build/ffmpeg/macos when it exists")
set(pspocket_ffmpeg_dir "${PSPOCKET_FFMPEG_DIR}")
if(NOT pspocket_ffmpeg_dir AND APPLE AND NOT IOS AND EXISTS ${PROJECT_SOURCE_DIR}/build/ffmpeg/macos/lib/libavcodec.a)
    set(pspocket_ffmpeg_dir ${PROJECT_SOURCE_DIR}/build/ffmpeg/macos)
endif()
if(pspocket_ffmpeg_dir AND EXISTS ${pspocket_ffmpeg_dir}/lib/libavcodec.a)
    add_library(pspocket_ffmpeg INTERFACE)
    target_include_directories(pspocket_ffmpeg SYSTEM INTERFACE ${pspocket_ffmpeg_dir}/include)
    target_link_libraries(pspocket_ffmpeg INTERFACE ${pspocket_ffmpeg_dir}/lib/libavcodec.a
        ${pspocket_ffmpeg_dir}/lib/libswscale.a ${pspocket_ffmpeg_dir}/lib/libswresample.a
        ${pspocket_ffmpeg_dir}/lib/libavutil.a)
    target_compile_definitions(pspocket_ffmpeg INTERFACE PSPOCKET_HAVE_FFMPEG=1)
    add_library(pspocket::ffmpeg ALIAS pspocket_ffmpeg)
    message(STATUS "FFmpeg decoders: ${pspocket_ffmpeg_dir}")
else()
    message(STATUS "FFmpeg decoders: none (scripts/build-ffmpeg.sh): the media libraries build without decoding")
endif()

