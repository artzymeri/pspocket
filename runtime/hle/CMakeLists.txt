# Firmware modules on the host (HLE) and the per-process import linkage. Portable C++; the iOS app compiles src/ too.
add_library(pspocket_hle STATIC
    src/cellAudio.cpp
    src/cellGame.cpp
    src/cellGcmSys.cpp
    src/cellHttp.cpp
    src/cellNetCtl.cpp
    src/cellOskDialog.cpp
    src/cellResc.cpp
    src/cellRtc.cpp
    src/cellSaveData.cpp
    src/cellSheap.cpp
    src/cellSpurs.cpp
    src/cellSpursQueue.cpp
    src/cellSpursSemaphore.cpp
    src/cellSsl.cpp
    src/cellSync.cpp
    src/sys_prx.cpp
    src/sys_spu_.cpp
    src/cellSysmodule.cpp
    src/cellSysutil.cpp
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
    src/sceNpTrophy.cpp
    src/spu_programs.cpp
    src/spurs_custom.cpp
    src/spurs_jobchain.cpp
    src/spurs_scheduler.cpp
    src/spurs_taskset.cpp
    src/sysPrxForUser.cpp
    src/sysPrxForUser_libc.cpp
    src/sys_fs.cpp
    src/sys_io.cpp
    src/sys_net.cpp
    src/sysutil_misc.cpp
    src/devices.cpp
    src/trophy_files.cpp)
target_include_directories(pspocket_hle PUBLIC include)
target_link_libraries(pspocket_hle PUBLIC pspocket::core pspocket::ppu pspocket::spu pspocket::lv2 pspocket::rsx)
# SHA-1, for the checksum of trophy packs (trophy_files.cpp).
target_link_libraries(pspocket_hle PRIVATE pspocket::recomp_common)
pspocket_set_warnings(pspocket_hle)
add_library(pspocket::hle ALIAS pspocket_hle)
