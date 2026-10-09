add_library(pspocket_core STATIC
    src/guest_memory.cpp
    src/mmio.cpp
    src/log.cpp
    src/png.cpp
    src/reservation.cpp
    src/thread_report.cpp
    src/timebase.cpp)
target_include_directories(pspocket_core PUBLIC include)
pspocket_set_warnings(pspocket_core)
add_library(pspocket::core ALIAS pspocket_core)
