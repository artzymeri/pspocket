# Firmware modules on the host (HLE) and the per-process import linkage. Portable C++; the iOS app compiles src/ too.
add_library(pspocket_hle STATIC
    src/cellAudio.cpp
    src/cellGame.cpp
    src/cellGcmSys.cpp
    src/cellHttp.cpp
    src/cellHttpAuth.cpp
    src/cellHttpCache.cpp
    src/cellHttpClient.cpp
    src/cellHttpConnection.cpp
    src/cellHttpCookie.cpp
    src/cellHttps.cpp
    src/cellHttpTransaction.cpp
    src/cellHttpUtil.cpp
    src/cellKey2char.cpp
    src/cellL10n.cpp
    src/cellNetCtl.cpp
    src/cellOskDialog.cpp
    src/cellResc.cpp
    src/cellRtc.cpp
    src/cellSaveData.cpp
    src/cellSheap.cpp
    src/cellSpudll.cpp
    src/cellSpurs.cpp
    src/cellSpursQueue.cpp
    src/cellSpursSemaphore.cpp
    src/cellSsl.cpp
    src/cellSslLib.cpp
    src/cellSslConn.cpp
    src/cellSync.cpp
    src/sys_prx.cpp
    src/sys_spu_.cpp
    src/cellSysmodule.cpp
    src/cellSysutil.cpp
    src/cellSysutilCxml.cpp
    src/cellSysutilPacket.cpp
    src/cellSysutilSystem.cpp
    src/cellSysutilWebBrowser.cpp
    src/cellVoice.cpp
    src/modules.cpp
    src/pad_script.cpp
    src/process.cpp
    src/psf.cpp
    src/rpcs3_stubs.cpp
    src/sceNp.cpp
    src/sceNp2.cpp
    src/sceNpCommerce2.cpp
    src/sceNpSns.cpp
    src/sceNpTus.cpp
    src/sceNpClans.cpp
    src/sceNpUtil.cpp
    src/cellSearch.cpp
    src/cellMusic.cpp
    src/media_export.cpp
    src/cellSysutilAvc2.cpp
    src/cellImeJp.cpp
    src/cellRec.cpp
    src/cellPamf.cpp
    src/cellDmux.cpp
    src/cellOvis.cpp
    src/cellAdec.cpp
    src/adec_lpcm.cpp
    src/adec_atracx.cpp
    src/cellPngDec.cpp
    src/cellGifDec.cpp
    src/cellJpgDec.cpp
    src/jpeg_decoder.cpp
    src/cellVdec.cpp
    src/vdec_hw.cpp
    src/cellDmuxPamf.cpp
    src/dmux_pamf.cpp
    src/sceNpTrophy.cpp
    src/spu_programs.cpp
    src/spurs_custom.cpp
    src/spurs_jobchain.cpp
    src/spurs_scheduler.cpp
    src/spurs_taskset.cpp
    src/lv2_heap.cpp
    src/sysPrxForUser.cpp
    src/sysPrxForUser_libc.cpp
    src/sysPrxForUser_sys.cpp
    src/sys_libc.cpp
    src/sys_libc_wide.cpp
    src/sys_libc_malloc.cpp
    src/sys_libc_stdio.cpp
    src/sys_libc_time.cpp
    src/sys_libc_process.cpp
    src/sys_libm.cpp
    src/cellFont.cpp
    src/cellFontFT.cpp
    src/cell_FreeType2.cpp
    src/freetype_bridge.cpp
    src/freetype_glyph.cpp
    src/freetype_cache.cpp
    src/cellSail.cpp
    src/cellSailPlayer.cpp
    src/cellSailDescriptor.cpp
    src/cellSailAdapters.cpp
    src/cellSailControl.cpp
    src/cellSailFileSource.cpp
    src/cellSailRec.cpp
    src/cellSpursJq.cpp
    src/cellFiber.cpp
    src/cellSync2.cpp
    src/cellDaisy.cpp
    src/sys_lv2dbg.cpp
    src/usbd.cpp
    src/vdec_cores.cpp
    src/cellVpost.cpp
    src/cellAtrac.cpp
    src/cellJpgEnc.cpp
    src/cellPngEnc.cpp
    src/libsnd3.cpp
    src/libsynth2.cpp
    src/libmixer.cpp
    src/cellCelpEnc.cpp
    src/cellRudp.cpp
    src/cellNetAoi.cpp
    src/libad.cpp
    src/sys_libstdcxx.cpp
    src/stdcxx_streams.cpp
    src/stdcxx_facets.cpp
    src/sysutil_more.cpp
    src/sys_fs.cpp
    src/sys_io.cpp
    src/sys_net.cpp
    src/sysutil_misc.cpp
    src/devices.cpp
    src/gem.cpp
    src/trophy_files.cpp)
target_include_directories(pspocket_hle PUBLIC include)
target_link_libraries(pspocket_hle PUBLIC pspocket::core pspocket::ppu pspocket::spu pspocket::lv2 pspocket::rsx)
# SHA-1, for the checksum of trophy packs (trophy_files.cpp).
target_link_libraries(pspocket_hle PRIVATE pspocket::recomp_common)
# FFmpeg's decoders for the media libraries (D-181, cellVdec D-195); without them those build without decoding.
if(TARGET pspocket::ffmpeg)
    target_link_libraries(pspocket_hle PRIVATE pspocket::ffmpeg)
endif()
# FreeType 2.2.1 for the font libraries (D-500); without it the firmware's font modules serve the games.
if(TARGET pspocket::freetype)
    target_link_libraries(pspocket_hle PRIVATE pspocket::freetype)
endif()
# libpng for cellPngDec (D-199); without it the firmware's libpngdec serves the games.
if(TARGET pspocket::libpng)
    target_link_libraries(pspocket_hle PRIVATE pspocket::libpng)
endif()
pspocket_set_warnings(pspocket_hle)
add_library(pspocket::hle ALIAS pspocket_hle)
