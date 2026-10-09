# HLE linkage, sysPrxForUser and the process loader: registry checks, stub calls, lwmutex on real lv2 threads, a
# synthetic executable booted end to end, and pspocket-boot's fault report.
pspocket_add_test(test_hle
    SOURCES test_hle.cpp test_sysPrxForUser.cpp test_audio.cpp test_audio_buffering.cpp test_spu_programs.cpp test_gcm.cpp test_np.cpp test_np2.cpp test_np_tus.cpp test_np_clans.cpp test_commerce2.cpp test_spurs_api.cpp test_spurs_jobchain.cpp test_spurs_queue.cpp test_spurs_run.cpp test_spurs_semaphore.cpp test_sys_io.cpp test_sysutil.cpp test_sysutil_misc.cpp test_media.cpp test_avc.cpp test_imejp.cpp test_rec.cpp test_pamf.cpp test_dmux.cpp test_vdec.cpp test_vdec_vt.cpp test_adec.cpp test_pngdec.cpp test_jpgdec.cpp test_gifdec.cpp test_ovis.cpp test_devices.cpp test_savedata.cpp test_misc_hle.cpp test_pad_script.cpp test_trophy.cpp test_trophy_files.cpp test_voice.cpp test_rtc.cpp test_net.cpp test_lle.cpp test_sync.cpp test_ac_sysutil_fs_pad.cpp test_ac_gcm_spurs_prx.cpp test_la_noire.cpp test_spu_threads.cpp test_gcm_sys.cpp test_sysprx_libc.cpp test_lv2_compare.cpp test_l10n.cpp test_httputil.cpp test_key2char.cpp test_spurs_task2.cpp test_spurs_compare.cpp test_sheap_compare.cpp test_net_compare.cpp test_libm.cpp test_libc.cpp test_libc_stdio.cpp test_libc_process.cpp test_font.cpp test_http_compare.cpp test_sail.cpp test_spursjq.cpp test_fiber.cpp test_sync2.cpp test_fs_compare.cpp test_gcm_compare.cpp test_lv2dbg.cpp test_usb_compare.cpp test_vpost.cpp test_atrac.cpp test_imgenc.cpp test_snd.cpp test_rudp.cpp test_libstdcxx.cpp test_libstdcxx_facets.cpp test_sysutil_compare.cpp test_sysutil_cxml_compare.cpp test_sysutil_more.cpp test_np_compare.cpp test_freetype_compare.cpp test_fontft_compare.cpp test_ssl_compare.cpp test_config_compare.cpp test_atrac_multi.cpp test_daisy.cpp test_libmixer.cpp
    LIBS pspocket::loader pspocket_test_synthetic pspocket::recomp_ppu pspocket::recomp_spu)
# HLE modules' internal headers (sysutil.hpp: the event queue other modules use), and lv2's (internal.hpp: the RSX
# device's flip mode and tiles, which cellGcmSys sets through lv2, white-box).
target_include_directories(test_hle PRIVATE ${PROJECT_SOURCE_DIR}/runtime/hle/src ${PROJECT_SOURCE_DIR}/runtime/lv2/src)
# FFmpeg, to check the synthetic H.264 the cellVdec tests write (h264_writer.hpp) with a decoder of its own.
if(TARGET pspocket::ffmpeg)
    target_link_libraries(test_hle PRIVATE pspocket::ffmpeg)
endif()
# libpng's reader, to check what cellPngEnc writes with a decoder of its own (D-566).
if(TARGET pspocket::libpng)
    target_link_libraries(test_hle PRIVATE pspocket::libpng)
endif()

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
