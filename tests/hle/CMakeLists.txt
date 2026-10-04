# HLE linkage, sysPrxForUser and the process loader: registry checks, stub calls, lwmutex on real lv2 threads, a
# synthetic executable booted end to end, and pspocket-boot's fault report.
pspocket_add_test(test_hle
    SOURCES test_hle.cpp test_sysPrxForUser.cpp test_audio.cpp test_audio_buffering.cpp test_spu_programs.cpp test_gcm.cpp test_np.cpp test_spurs_jobchain.cpp test_spurs_queue.cpp test_spurs_run.cpp test_spurs_semaphore.cpp test_sys_io.cpp test_sysutil.cpp test_savedata.cpp test_misc_hle.cpp test_pad_script.cpp test_trophy.cpp test_trophy_files.cpp test_voice.cpp test_rtc.cpp
    LIBS pspocket::loader pspocket_test_synthetic pspocket::recomp_ppu pspocket::recomp_spu)
# HLE modules' internal headers (sysutil.hpp: the event queue other modules use).
target_include_directories(test_hle PRIVATE ${PROJECT_SOURCE_DIR}/runtime/hle/src)

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
