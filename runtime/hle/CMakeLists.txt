# Firmware modules on the host (HLE) and the per-process import linkage. Portable C++; the iOS app compiles src/ too.
add_library(pspocket_hle STATIC
    src/cellAudio.cpp
    src/cellGame.cpp
    src/cellGcmSys.cpp
    src/cellNetCtl.cpp
    src/cellSheap.cpp
    src/cellSpurs.cpp
    src/cellSysmodule.cpp
    src/cellSysutil.cpp
    src/modules.cpp
    src/process.cpp
    src/psf.cpp
    src/sceNp.cpp
    src/spurs_custom.cpp
    src/spurs_scheduler.cpp
    src/spurs_taskset.cpp
    src/sysPrxForUser.cpp
    src/sys_fs.cpp
    src/sys_io.cpp
    src/sys_net.cpp)
target_include_directories(pspocket_hle PUBLIC include)
target_link_libraries(pspocket_hle PUBLIC pspocket::core pspocket::ppu pspocket::spu pspocket::lv2 pspocket::rsx)
pspocket_set_warnings(pspocket_hle)
add_library(pspocket::hle ALIAS pspocket_hle)
