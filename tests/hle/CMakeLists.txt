# HLE linkage, sysPrxForUser and the process loader: registry checks, stub calls, lwmutex on real lv2 threads, a
# synthetic executable booted end to end, and pspocket-boot's fault report.
pspocket_add_test(test_hle
    SOURCES test_hle.cpp test_sysPrxForUser.cpp test_audio.cpp test_audio_buffering.cpp test_spu_programs.cpp test_gcm.cpp test_np.cpp test_np2.cpp test_np_tus.cpp test_np_clans.cpp test_commerce2.cpp test_spurs_api.cpp test_spurs_jobchain.cpp test_spurs_queue.cpp test_spurs_run.cpp test_spurs_semaphore.cpp test_sys_io.cpp test_sysutil.cpp test_sysutil_misc.cpp test_media.cpp test_avc.cpp test_imejp.cpp test_rec.cpp test_pamf.cpp test_dmux.cpp test_vdec.cpp test_vdec_vt.cpp test_vdec_codecs.cpp test_adec.cpp test_adec_cores.cpp test_adec_more.cpp test_pngdec.cpp test_jpgdec.cpp test_gifdec.cpp test_ovis.cpp test_devices.cpp test_savedata.cpp test_misc_hle.cpp test_pad_script.cpp test_trophy.cpp test_trophy_files.cpp test_voice.cpp test_rtc.cpp test_net.cpp test_lle.cpp test_sync.cpp test_ac_sysutil_fs_pad.cpp test_ac_gcm_spurs_prx.cpp test_la_noire.cpp test_spu_threads.cpp test_gcm_sys.cpp test_sysprx_libc.cpp test_lv2_compare.cpp test_l10n.cpp test_np_commerce_compare.cpp test_httputil.cpp test_key2char.cpp test_spurs_task2.cpp test_spurs_compare.cpp test_sheap_compare.cpp test_net_compare.cpp test_libm.cpp test_libc.cpp test_libc_stdio.cpp test_libc_process.cpp test_libc_format.cpp test_libc_locale.cpp test_libc_spu.cpp test_libc_scan.cpp test_font.cpp test_http_compare.cpp test_sail.cpp test_sail_source.cpp test_spursjq.cpp test_spurs_jq_compare.cpp test_fiber.cpp test_sync2.cpp test_fs_compare.cpp test_gcm_compare.cpp test_lv2dbg.cpp test_crashdump.cpp test_usb_compare.cpp test_vpost.cpp test_atrac.cpp test_imgenc.cpp test_jpgenc.cpp test_snd.cpp test_rudp.cpp test_libstdcxx.cpp test_libstdcxx_facets.cpp test_sysutil_compare.cpp test_sysutil_cxml_compare.cpp test_sysutil_more.cpp test_np_compare.cpp test_freetype_compare.cpp test_fontft_compare.cpp test_ssl_compare.cpp test_config_compare.cpp test_atrac_multi.cpp test_daisy.cpp test_libmixer.cpp test_setjmp.cpp test_codec_cores.cpp test_fs_utility_init.cpp test_frame_watch.cpp
    LIBS pspocket::loader pspocket_test_synthetic pspocket::recomp_ppu pspocket::recomp_spu)
# HLE modules' internal headers (sysutil.hpp: the event queue other modules use), and lv2's (internal.hpp: the RSX
# device's flip mode and tiles, which cellGcmSys sets through lv2, white-box); tests/common: thread_check.hpp (D-585).
target_include_directories(test_hle PRIVATE ${PROJECT_SOURCE_DIR}/runtime/hle/src ${PROJECT_SOURCE_DIR}/runtime/lv2/src
    ${PROJECT_SOURCE_DIR}/tests/common)
# FFmpeg, to check the synthetic H.264 the cellVdec tests write (h264_writer.hpp) with a decoder of its own; the tests'
# own build of it where there is one, whose encoders write the cellAdec cores' streams (D-577).
if(TARGET pspocket::ffmpeg)
    target_link_libraries(test_hle PRIVATE pspocket::ffmpeg)
    set_target_properties(test_hle PROPERTIES PSPOCKET_FFMPEG_ENCODERS ON)
endif()
# libpng's reader, to check what cellPngEnc writes with a decoder of its own (D-566).
if(TARGET pspocket::libpng)
    target_link_libraries(test_hle PRIVATE pspocket::libpng)
endif()

# libsynth2, the SPU2 in software (D-610): test_hle "[synth2]"; its comparison with Sony's module (D-597), hidden:
# test_hle "[compare][synth2]".
target_sources(test_hle PRIVATE test_synth2.cpp test_synth2_compare.cpp)

# libsnd3 over libsynth2 (D-640 to D-643): test_hle "[snd3]" (test_snd.cpp, above); its comparison with Sony's module
# (D-644), hidden: test_hle "[compare][snd3]".
target_sources(test_hle PRIVATE test_snd3_compare.cpp)

# The C library compared with a game's statically linked copy of the SDK's (D-904): test_hle "[gamelibc]", skipped without
# PSPOCKET_TEST_AC_EBOOT and PSPOCKET_TEST_AC_LIBC_MAP (game_executable.hpp).
target_sources(test_hle PRIVATE
    test_libc_game.cpp
    test_libc_spu_game.cpp
    test_libc_locale_game.cpp
    test_libc_format_game.cpp
    test_libc_scan_game.cpp
)

# The comparisons with Sony's modules of the firmware-first libraries and of cellVdec's newest codecs (D-597): hidden,
# run with test_hle "[compare]"; skipped without the firmware (PSPOCKET_TEST_DEV_FLASH), where PSPOCKET_COMPARE_SELF=1
# runs pspocket's half twice instead.
target_sources(test_hle PRIVATE
    test_key2char_compare.cpp
    test_codec_cores_compare.cpp
    test_vpost_compare.cpp
    test_pngenc_compare.cpp
    test_jpgenc_compare.cpp
    test_libmixer_compare.cpp
    test_fiber_compare.cpp
    test_vdec_codecs_compare.cpp
    test_rudp_compare.cpp
    test_sysconf_ps1emu_compare.cpp
    test_module_management.cpp
    test_sail_rec_compare.cpp)

# The audio cores on FFmpeg against Sony's (D-944, in test_adec_cores.cpp): test_hle "[compare][adec-cores]" (about three
# minutes).
# libvdec's last functions (D-920) against Sony's libvdec: test_hle "[compare][vdec-ext]". The video codec cores
# pspocket's own (D-920): test_hle "[vdec-cores]", their comparisons with Sony's "[compare][vdec-cores]".
target_sources(test_hle PRIVATE test_vdec_ext_compare.cpp test_vdec_cores_compare.cpp test_vdec_score_compare.cpp)
# The H.264 and MVC codec cores pspocket's own (D-933): test_hle "[vdec-cores]"; their comparisons with Sony's modules
# "[compare][vdec-cores]".
target_sources(test_hle PRIVATE test_vdec_avc_compare.cpp)
target_sources(test_hle PRIVATE test_vdec_sjvtd_compare.cpp)

# The CELP libraries (D-932): libcelpenc, libcelp8enc, libcelpdec and libcelp8dec, pspocket's own: test_hle "[celp]",
# their comparisons with Sony's modules "[compare][celp]".
target_sources(test_hle PRIVATE test_celp_compare.cpp test_celp8_compare.cpp test_celp_dec_compare.cpp)

# liblv2coredump's crash dump writer (D-943): test_hle "[lv2coredump]", its comparison with Sony's module
# "[compare][lv2coredump]".
target_sources(test_hle PRIVATE test_lv2coredump_compare.cpp)

# Libraries made as a console with no PSN, no PSP peer and no profiler attached (D-635 to D-639).
target_sources(test_hle PRIVATE
    test_netaoi.cpp
    test_medi.cpp
    test_ad.cpp
    test_libprof.cpp
    test_sysmodule.cpp)

# cellAdec's decoder threads when the game overwrites a decoder's context while it is open (D-645).
target_sources(test_hle PRIVATE test_adec_context.cpp)

# cellVideoPlayerUtility as a console with no system video player (D-679): test_hle "[videoplayer]". The AV chat's
# stubs made pspocket's own (D-675 to D-678) are in test_avc.cpp: test_hle "[avc2]", "[avcext]".
target_sources(test_hle PRIVATE test_videoplayer.cpp)

# pspocket-boot end to end: a synthetic program that reads an unmapped guest address must be reported with the guest
# address and the PPU instruction address (CLAUDE.md rule 5), not die with a bare signal.
add_executable(make_boot_programs make_boot_programs.cpp)
target_link_libraries(make_boot_programs PRIVATE pspocket_test_synthetic pspocket::recomp_ppu)
pspocket_set_warnings(make_boot_programs)
set(boot_programs ${CMAKE_CURRENT_BINARY_DIR}/boot_programs)
add_custom_command(
    OUTPUT ${boot_programs}/fault.elf
    COMMAND make_boot_programs ${boot_programs}
    DEPENDS make_boot_programs
    COMMENT "Writing the synthetic pspocket-boot test programs"
    VERBATIM)
add_custom_target(boot_programs ALL DEPENDS ${boot_programs}/fault.elf)
if(TARGET pspocket-boot)
    add_test(NAME pspocket_boot_fault COMMAND pspocket-boot ${boot_programs}/fault.elf --timeout 20)
    set_tests_properties(pspocket_boot_fault PROPERTIES
        PASS_REGULAR_EXPRESSION "guest access to 0x80000000 .* at cia 0x00010204")
endif()

# pspocket-boot's thread report: a program that waits forever on an event queue must end its run at the timeout with
# a report naming the thread, the syscall, the queue and its state, and the backtrace into the caller. (Its own
# directory: the rule above writes every program into boot_programs/ too, and two rules must not write one file.)
set(boot_report_programs ${CMAKE_CURRENT_BINARY_DIR}/boot_programs_report)
add_custom_command(
    OUTPUT ${boot_report_programs}/wait_queue.elf
    COMMAND make_boot_programs ${boot_report_programs}
    DEPENDS make_boot_programs
    COMMENT "Writing the synthetic pspocket-boot thread report programs"
    VERBATIM)
add_custom_target(boot_report_programs ALL DEPENDS ${boot_report_programs}/wait_queue.elf)
if(TARGET pspocket-boot)
    add_test(NAME pspocket_boot_thread_report COMMAND pspocket-boot ${boot_report_programs}/wait_queue.elf --timeout 2)
    # The thread's line, what it waits on, and its backtrace: the `sc`, then the return into _start.
    string(CONCAT thread_report_block
        "main_thread +prio +[0-9]+  blocked +in sys_event_queue_receive \\(syscall 130\\) for [^\n]*\n"
        " +on event queue 0x8d[0-9a-f]+ \"bootwait\": 0 of 16 events queued, 1 waiter\n"
        " +at 0x00010298 <- 0x00010210  \\(sp 0x")
    set_tests_properties(pspocket_boot_thread_report PROPERTIES PASS_REGULAR_EXPRESSION "${thread_report_block}")
endif()

# pspocket-boot with an installed game (D-939): an executable in --hdd0's game folder starts with its own guest path
# as argv[0] and its USRDIR as /app_home; the program exits 0 only if both are there.
set(boot_hdd_programs ${CMAKE_CURRENT_BINARY_DIR}/boot_programs_hdd)
set(boot_hdd_eboot ${boot_hdd_programs}/hdd0/game/NPXX00000/USRDIR/EBOOT.BIN)
add_custom_command(
    OUTPUT ${boot_hdd_eboot}
    COMMAND make_boot_programs ${boot_hdd_programs}
    DEPENDS make_boot_programs
    COMMENT "Writing the synthetic pspocket-boot installed game"
    VERBATIM)
add_custom_target(boot_hdd_programs ALL DEPENDS ${boot_hdd_eboot})
if(TARGET pspocket-boot)
    add_test(NAME pspocket_boot_hdd_game
        COMMAND pspocket-boot ${boot_hdd_eboot} --hdd0 ${boot_hdd_programs}/hdd0 --timeout 20)
    set_tests_properties(pspocket_boot_hdd_game PROPERTIES PASS_REGULAR_EXPRESSION "process exit, status 0[^0-9]")
endif()

# The parser fuzzing of the HLE libraries (D-603, tests/common/mutation_fuzz.hpp): a game's files, its images, its
# movies and sound. "[fuzz]" runs them alone; PSPOCKET_FUZZ_ITERS=<n> longer.
target_sources(test_hle PRIVATE test_fuzz_files.cpp test_fuzz_images.cpp test_fuzz_media.cpp)
