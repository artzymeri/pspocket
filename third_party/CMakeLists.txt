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
    # H.264 on the media engine (D-254): FFmpeg's VideoToolbox hwaccel needs Apple's frameworks.
    if(APPLE AND EXISTS ${pspocket_ffmpeg_dir}/include/libavutil/hwcontext_videotoolbox.h)
        target_link_libraries(pspocket_ffmpeg INTERFACE "-framework VideoToolbox" "-framework CoreMedia"
            "-framework CoreVideo" "-framework CoreFoundation")
    endif()
    add_library(pspocket::ffmpeg ALIAS pspocket_ffmpeg)
    message(STATUS "FFmpeg decoders: ${pspocket_ffmpeg_dir}")
else()
    message(STATUS "FFmpeg decoders: none (scripts/build-ffmpeg.sh): the media libraries build without decoding")
endif()


# libpng (D-199): Sony's libpngdec is libpng 1.2.8 on zlib 1.2.3; pspocket's cellPngDec is built on libpng too, from a
# source tree outside the repository (default: the one in the RPCS3 checkout the build already reads its keys from,
# 3rdparty/libpng/libpng), with the system's zlib. Optional: without it cellPngDec is not pspocket's (the firmware's
# libpngdec serves the games, D-175).
set(pspocket_libpng_default "")
foreach(rpcs3 "${PSPOCKET_RPCS3_DIR}" "${CMAKE_SOURCE_DIR}/external/rpcs3")
    if(rpcs3 AND NOT pspocket_libpng_default AND EXISTS "${rpcs3}/3rdparty/libpng/libpng/png.h")
        get_filename_component(pspocket_libpng_default "${rpcs3}/3rdparty/libpng/libpng" ABSOLUTE)
    endif()
endforeach()
set(PSPOCKET_LIBPNG_DIR "${pspocket_libpng_default}" CACHE PATH "libpng source tree (D-199)")
if(PSPOCKET_LIBPNG_DIR AND EXISTS ${PSPOCKET_LIBPNG_DIR}/png.h)
    find_package(ZLIB REQUIRED)
    set(pspocket_libpng_sources)
    foreach(f png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwrite
              pngwtran pngwutil)
        list(APPEND pspocket_libpng_sources ${PSPOCKET_LIBPNG_DIR}/${f}.c)
    endforeach()
    add_library(pspocket_libpng STATIC ${pspocket_libpng_sources})
    configure_file(${PSPOCKET_LIBPNG_DIR}/scripts/pnglibconf.h.prebuilt ${CMAKE_CURRENT_BINARY_DIR}/libpng/pnglibconf.h
        COPYONLY)
    target_include_directories(pspocket_libpng SYSTEM PUBLIC ${PSPOCKET_LIBPNG_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libpng)
    # No NEON filters: the C ones only, until a measurement asks for them (rule 8).
    target_compile_definitions(pspocket_libpng PRIVATE PNG_ARM_NEON_OPT=0)
    target_compile_definitions(pspocket_libpng INTERFACE PSPOCKET_HAVE_LIBPNG=1)
    target_compile_options(pspocket_libpng PRIVATE -w)
    target_link_libraries(pspocket_libpng PUBLIC ZLIB::ZLIB)
    add_library(pspocket::libpng ALIAS pspocket_libpng)
    message(STATUS "libpng: ${PSPOCKET_LIBPNG_DIR}")
else()
    message(STATUS "libpng: none (PSPOCKET_LIBPNG_DIR): cellPngDec is not pspocket's")
endif()
