# Pinned third-party code. Each entry records its version and licence in third_party/README.md.

if(PSPOCKET_BUILD_TESTS)
    # Catch2 v3.16.0, amalgamated (BSL-1.0)
    add_library(pspocket_catch2 STATIC catch2/catch_amalgamated.cpp)
    target_include_directories(pspocket_catch2 SYSTEM PUBLIC catch2)
    add_library(pspocket::catch2 ALIAS pspocket_catch2)
endif()

# FFmpeg 7.1.1's LGPL decoders (D-181), built by scripts/build-ffmpeg.sh into build/ffmpeg/{macos,ios,linux}: pspocket's
# own media libraries (cellVdec, cellAdec, cellAtrac, cellDmux) decode with them as RPCS3's do. Optional: without them
# those libraries build without their decoders (code under #if PSPOCKET_HAVE_FFMPEG), and decoding fails, logged.
set(PSPOCKET_FFMPEG_DIR "" CACHE PATH
    "FFmpeg prefix (include/, lib/); default: build/ffmpeg/macos on the Mac, build/ffmpeg/linux on Linux, when it exists")
set(pspocket_ffmpeg_dir "${PSPOCKET_FFMPEG_DIR}")
if(NOT pspocket_ffmpeg_dir AND APPLE AND NOT IOS AND EXISTS ${PROJECT_SOURCE_DIR}/build/ffmpeg/macos/lib/libavcodec.a)
    set(pspocket_ffmpeg_dir ${PROJECT_SOURCE_DIR}/build/ffmpeg/macos)
endif()
# A Linux host's own build (D-576), for the host's architecture: not for a cross build (the AArch64 qemu tree).
if(NOT pspocket_ffmpeg_dir AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING
        AND EXISTS ${PROJECT_SOURCE_DIR}/build/ffmpeg/linux/lib/libavcodec.a)
    set(pspocket_ffmpeg_dir ${PROJECT_SOURCE_DIR}/build/ffmpeg/linux)
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
    # Elsewhere the static libraries need threads and the C maths library (their .pc files: -pthread -lm).
    if(NOT APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(pspocket_ffmpeg INTERFACE Threads::Threads m)
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

# FreeType 2.2.1 for cellFont, cellFontFT and cell_FreeType2 (D-500, D-525): Sony's libfreetype is FreeType 2.2.1 (its
# library is cell_FreeType2_2_1), so pspocket's font libraries are built on the same version, from
# external/freetype-2.2.1 (gitignored, D-201). Its module list, options and system layer are pspocket's own
# (runtime/hle/src/freetype), made to match Sony's build (no zlib, as Sony's). Optional: without it the firmware's font
# modules serve the games.
set(pspocket_freetype_default "")
if(EXISTS "${PROJECT_SOURCE_DIR}/external/freetype-2.2.1/include/ft2build.h")
    set(pspocket_freetype_default "${PROJECT_SOURCE_DIR}/external/freetype-2.2.1")
endif()
set(PSPOCKET_FREETYPE_DIR "${pspocket_freetype_default}" CACHE PATH "FreeType 2.2.1 source tree")
if(PSPOCKET_FREETYPE_DIR AND EXISTS ${PSPOCKET_FREETYPE_DIR}/include/ft2build.h)
    set(ft ${PSPOCKET_FREETYPE_DIR}/src)
    add_library(pspocket_freetype STATIC
        ${PROJECT_SOURCE_DIR}/runtime/hle/src/freetype/pspocket_ftsystem.c ${ft}/base/ftinit.c ${ft}/base/ftdebug.c ${ft}/base/ftbase.c ${ft}/base/ftbbox.c
        ${ft}/base/ftglyph.c ${ft}/base/ftbitmap.c ${ft}/base/ftstroke.c ${ft}/base/ftsynth.c ${ft}/base/ftmm.c
        ${ft}/base/fttype1.c ${ft}/base/ftwinfnt.c ${ft}/base/ftgxval.c ${ft}/base/ftotval.c ${ft}/base/ftpfr.c
        ${ft}/base/ftbdf.c ${ft}/base/ftxf86.c
        ${ft}/autofit/autofit.c ${ft}/truetype/truetype.c ${ft}/type1/type1.c ${ft}/cff/cff.c ${ft}/cid/type1cid.c
        ${ft}/pfr/pfr.c ${ft}/type42/type42.c ${ft}/winfonts/winfnt.c
        ${ft}/psaux/psaux.c ${ft}/psnames/psnames.c ${ft}/pshinter/pshinter.c ${ft}/raster/raster.c ${ft}/sfnt/sfnt.c
        ${ft}/smooth/smooth.c ${ft}/gzip/ftgzip.c ${ft}/lzw/ftlzw.c ${ft}/cache/ftcache.c)
    target_include_directories(pspocket_freetype SYSTEM BEFORE PUBLIC ${PROJECT_SOURCE_DIR}/runtime/hle/src/freetype
        ${PSPOCKET_FREETYPE_DIR}/include)
    target_compile_definitions(pspocket_freetype PUBLIC "FT_CONFIG_MODULES_H=<pspocket_ftmodule.h>"
        "FT_CONFIG_OPTIONS_H=<pspocket_ftoption.h>")
    # DARWIN_NO_CARBON: FreeType 2.2.1 would build its Carbon (Mac OS 9 font) support on Apple systems.
    target_compile_definitions(pspocket_freetype PRIVATE FT2_BUILD_LIBRARY)
    target_compile_definitions(pspocket_freetype PUBLIC DARWIN_NO_CARBON)
    target_compile_definitions(pspocket_freetype INTERFACE PSPOCKET_HAVE_FREETYPE=1)
    target_compile_options(pspocket_freetype PRIVATE -w)
    add_library(pspocket::freetype ALIAS pspocket_freetype)
    message(STATUS "FreeType: ${PSPOCKET_FREETYPE_DIR}")
endif()
