# lv2 kernel tests: syscalls through the real PPU `sc` hook with synthetic PPU contexts, multi-threaded tests per
# primitive, the filesystem over a temporary directory, a guest program run by the PPU interpreter, and the thread
# and wait report.
pspocket_add_test(test_lv2
    SOURCES test_dispatch.cpp test_threads.cpp test_mutex_cond.cpp test_rwlock_sema.cpp test_event.cpp
            test_lw.cpp test_time.cpp test_memory.cpp test_fs.cpp test_process.cpp test_report.cpp test_usbd.cpp test_devices.cpp test_services.cpp test_misc_calls.cpp
    LIBS pspocket::lv2)
# White-box access (wait-queue lengths) for the ordering tests.
target_include_directories(test_lv2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/runtime/lv2/src)
