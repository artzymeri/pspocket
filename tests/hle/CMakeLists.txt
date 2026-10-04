# HLE linkage, sysPrxForUser and the process loader: registry checks, stub calls, lwmutex on real lv2 threads, a
# synthetic executable booted end to end, and pspocket-boot's fault report.
pspocket_add_test(test_hle
    SOURCES test_hle.cpp test_sysPrxForUser.cpp test_gcm.cpp test_sys_io.cpp test_sysutil.cpp
    LIBS pspocket::loader pspocket_test_synthetic pspocket::recomp_ppu)
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
