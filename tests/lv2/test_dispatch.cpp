// The syscall dispatcher: name table, unimplemented syscalls, argument and result conventions, IDs.

#include "lv2_test_util.hpp"

using namespace pspocket;
using namespace pspocket::lv2;
using namespace pspocket::lv2::test;

TEST_CASE("lv2: syscall numbers used by the tests match the generated name table", "[lv2][dispatch]")
{
    const std::pair<u32, const char*> expected[] = {
        {kGetPid, "sys_process_getpid"}, {kProcessExit3, "sys_process_exit3"},
        {kGetNumberOfObject, "sys_process_get_number_of_object"}, {kGetSdkVersion, "sys_process_get_sdk_version"},
        {kGetParamSfo, "_sys_process_get_paramsfo"}, {kThreadExit, "_sys_ppu_thread_exit"},
        {kThreadYield, "sys_ppu_thread_yield"}, {kThreadJoin, "sys_ppu_thread_join"},
        {kThreadDetach, "sys_ppu_thread_detach"}, {kThreadGetJoinState, "sys_ppu_thread_get_join_state"},
        {kThreadSetPriority, "sys_ppu_thread_set_priority"}, {kThreadGetPriority, "sys_ppu_thread_get_priority"},
        {kThreadGetStackInfo, "sys_ppu_thread_get_stack_information"}, {kThreadCreate, "_sys_ppu_thread_create"},
        {kThreadStart, "sys_ppu_thread_start"}, {kThreadRename, "sys_ppu_thread_rename"},
        {kTimerCreate, "sys_timer_create"}, {kTimerDestroy, "sys_timer_destroy"},
        {kTimerGetInfo, "sys_timer_get_information"}, {kTimerStart, "_sys_timer_start"}, {kTimerStop, "sys_timer_stop"},
        {kTimerConnect, "sys_timer_connect_event_queue"}, {kTimerDisconnect, "sys_timer_disconnect_event_queue"},
        {kFlagCreate, "sys_event_flag_create"}, {kFlagDestroy, "sys_event_flag_destroy"},
        {kFlagWait, "sys_event_flag_wait"}, {kFlagTryWait, "sys_event_flag_trywait"}, {kFlagSet, "sys_event_flag_set"},
        {kSemCreate, "sys_semaphore_create"}, {kSemDestroy, "sys_semaphore_destroy"}, {kSemWait, "sys_semaphore_wait"},
        {kSemTryWait, "sys_semaphore_trywait"}, {kSemPost, "sys_semaphore_post"},
        {kLwMutexCreate, "_sys_lwmutex_create"}, {kLwMutexDestroy, "_sys_lwmutex_destroy"},
        {kLwMutexLock, "_sys_lwmutex_lock"}, {kLwMutexUnlock, "_sys_lwmutex_unlock"},
        {kLwMutexTryLock, "_sys_lwmutex_trylock"}, {kMutexCreate, "sys_mutex_create"},
        {kMutexDestroy, "sys_mutex_destroy"}, {kMutexLock, "sys_mutex_lock"}, {kMutexTryLock, "sys_mutex_trylock"},
        {kMutexUnlock, "sys_mutex_unlock"}, {kCondCreate, "sys_cond_create"}, {kCondDestroy, "sys_cond_destroy"},
        {kCondWait, "sys_cond_wait"}, {kCondSignal, "sys_cond_signal"}, {kCondSignalAll, "sys_cond_signal_all"},
        {kCondSignalTo, "sys_cond_signal_to"}, {kLwCondCreate, "_sys_lwcond_create"},
        {kLwCondDestroy, "_sys_lwcond_destroy"}, {kLwCondQueueWait, "_sys_lwcond_queue_wait"},
        {kSemGetValue, "sys_semaphore_get_value"}, {kLwCondSignal, "_sys_lwcond_signal"},
        {kLwCondSignalAll, "_sys_lwcond_signal_all"}, {kLwMutexUnlock2, "_sys_lwmutex_unlock2"},
        {kFlagClear, "sys_event_flag_clear"}, {kRwCreate, "sys_rwlock_create"}, {kRwDestroy, "sys_rwlock_destroy"},
        {kRwRLock, "sys_rwlock_rlock"}, {kRwTryRLock, "sys_rwlock_tryrlock"}, {kRwRUnlock, "sys_rwlock_runlock"},
        {kRwWLock, "sys_rwlock_wlock"}, {kRwTryWLock, "sys_rwlock_trywlock"}, {kRwWUnlock, "sys_rwlock_wunlock"},
        {kQueueCreate, "sys_event_queue_create"}, {kQueueDestroy, "sys_event_queue_destroy"},
        {kQueueReceive, "sys_event_queue_receive"}, {kQueueTryReceive, "sys_event_queue_tryreceive"},
        {kFlagCancel, "sys_event_flag_cancel"}, {kQueueDrain, "sys_event_queue_drain"},
        {kPortCreate, "sys_event_port_create"}, {kPortDestroy, "sys_event_port_destroy"},
        {kPortConnectLocal, "sys_event_port_connect_local"}, {kPortDisconnect, "sys_event_port_disconnect"},
        {kPortSend, "sys_event_port_send"}, {kFlagGet, "sys_event_flag_get"},
        {kPortConnectIpc, "sys_event_port_connect_ipc"}, {kUsleep, "sys_timer_usleep"},
        {kTimeGetCurrent, "sys_time_get_current_time"}, {kTimebaseFrequency, "sys_time_get_timebase_frequency"},
        {kSystemTime, "sys_time_get_system_time"}, {kContainerCreate, "sys_memory_container_create"},
        {kContainerDestroy, "sys_memory_container_destroy"}, {kContainerGetSize, "sys_memory_container_get_size"},
        {kMemAllocate, "sys_memory_allocate"}, {kMemFree, "sys_memory_free"},
        {kMemAllocateFromContainer, "sys_memory_allocate_from_container"},
        {kMemPageAttribute, "sys_memory_get_page_attribute"}, {kMemUserSize, "sys_memory_get_user_memory_size"},
        {kMmAllocateAddress, "sys_mmapper_allocate_address"}, {kMmFreeAddress, "sys_mmapper_free_address"},
        {kMmAllocateShm, "sys_mmapper_allocate_shared_memory"}, {kMmFreeShm, "sys_mmapper_free_shared_memory"},
        {kMmMap, "sys_mmapper_map_shared_memory"}, {kMmUnmap, "sys_mmapper_unmap_shared_memory"},
        {kMmSearchAndMap, "sys_mmapper_search_and_map"}, {kTtyRead, "sys_tty_read"}, {kTtyWrite, "sys_tty_write"},
        {kFsOpen, "sys_fs_open"}, {kFsRead, "sys_fs_read"}, {kFsWrite, "sys_fs_write"}, {kFsClose, "sys_fs_close"},
        {kFsOpendir, "sys_fs_opendir"}, {kFsReaddir, "sys_fs_readdir"}, {kFsClosedir, "sys_fs_closedir"},
        {kFsStat, "sys_fs_stat"}, {kFsFstat, "sys_fs_fstat"}, {kFsMkdir, "sys_fs_mkdir"},
        {kFsRename, "sys_fs_rename"}, {kFsRmdir, "sys_fs_rmdir"}, {kFsUnlink, "sys_fs_unlink"},
        {kFsLseek, "sys_fs_lseek"}, {kFsFsync, "sys_fs_fsync"}, {kFsTruncate, "sys_fs_truncate"},
        {kFsFtruncate, "sys_fs_ftruncate"},
    };
    for (const auto& [number, name] : expected) {
        INFO("syscall " << number);
        REQUIRE(syscall_name(number) != nullptr);
        CHECK(std::string(syscall_name(number)) == name);
    }
    CHECK(syscall_name(6) == nullptr); // unused slot
    CHECK(syscall_name(1024) == nullptr);
}

TEST_CASE("lv2: unimplemented syscalls log their number and name and return CELL_ENOSYS", "[lv2][dispatch]")
{
    Fixture f;
    Caller c(*f.kernel);
    LogCapture log;
    const u64 before = f.kernel->unimplemented_count();

    // Every named slot is bound (D-528): an unnamed slot and an out-of-range number.
    CHECK(c.call64(6, 1, 2, 3) == to_r3(err(CELL_ENOSYS)));
    CHECK(c.call64(6, 1, 2, 3) == 0xFFFFFFFF80010003ull); // sign-extended, as RPCS3's error_code
    CHECK(c.call(6) == rc(CELL_ENOSYS));
    CHECK(log.contains("lv2", "UNIMPLEMENTED syscall 6 (unnamed)"));
    CHECK(c.call(5000) == rc(CELL_ENOSYS));
    CHECK(log.contains("lv2", "UNIMPLEMENTED syscall 5000 (invalid)"));
    CHECK(f.kernel->unimplemented_count() == before + 4);
    for (u32 n = 0; n < 1024; n++) {
        INFO("syscall " << n);
        if (syscall_name(n)) CHECK(f.kernel->syscall_bound(n));
    }
}

TEST_CASE("lv2: arguments take the low word, results are sign-extended", "[lv2][dispatch]")
{
    Fixture f;
    Caller c(*f.kernel);
    CHECK(c.call64(kMutexLock, 0x12345678, 0) == 0xFFFFFFFF80010005ull); // CELL_ESRCH

    const u32 idp = f.alloc(4);
    REQUIRE(c.call(kMutexCreate, idp, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)) == CELL_OK);
    const u32 id = f.r32(idp);
    // Garbage in the upper word of a u32 argument is ignored (RPCS3 ppu_gpr_cast).
    CHECK(c.call(kMutexLock, (0xDEADBEEFull << 32) | id, 0) == CELL_OK);
    CHECK(c.call(kMutexUnlock, (0x1ull << 40) | id) == CELL_OK);
    // A 64-bit result.
    CHECK(c.call64(kTimebaseFrequency) == 79800000ull);
}

TEST_CASE("lv2: object IDs follow RPCS3's IdManager layout", "[lv2][dispatch]")
{
    Fixture f;
    Caller c(*f.kernel);
    CHECK(c.id() == 0x01000000); // the first PPU thread ID

    const u32 out = f.alloc(4);
    REQUIRE(c.call(kMutexCreate, out, mutex_attr(f, SYS_SYNC_PRIORITY, SYS_SYNC_NOT_RECURSIVE)) == CELL_OK);
    const u32 mutex = f.r32(out);
    CHECK(mutex == 0x85000000); // lv2_obj: base + index * 0x100, the 8192 slots shared by all types
    REQUIRE(c.call(kCondCreate, out, mutex, cond_attr(f)) == CELL_OK);
    const u32 cond = f.r32(out);
    CHECK(cond == 0x86000100);
    REQUIRE(c.call(kSemCreate, out, sync_attr(f, SYS_SYNC_FIFO), 0, 1) == CELL_OK);
    CHECK(f.r32(out) == 0x96000200);

    // A valid index with the wrong type is not found.
    CHECK(c.call(kCondSignal, mutex) == rc(CELL_ESRCH));
    CHECK(c.call(kMutexLock, cond, 0) == rc(CELL_ESRCH));
    CHECK(c.call(kMutexLock, 0x85000100, 0) == rc(CELL_ESRCH)); // index 1 is the condition variable

    // Destroyed IDs are not handed out again until the table has filled once.
    REQUIRE(c.call(kMutexDestroy, mutex) == rc(CELL_EPERM)); // a condition variable is bound to it
    REQUIRE(c.call(kCondDestroy, cond) == CELL_OK);
    REQUIRE(c.call(kMutexDestroy, mutex) == CELL_OK);
    CHECK(c.call(kMutexLock, mutex, 0) == rc(CELL_ESRCH));
    REQUIRE(c.call(kMutexCreate, out, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)) == CELL_OK);
    CHECK(f.r32(out) == 0x85000300);
}

TEST_CASE("lv2: bad guest pointers return CELL_EFAULT instead of faulting", "[lv2][dispatch]")
{
    Fixture f;
    Caller c(*f.kernel);
    const u32 out = f.alloc(4);
    CHECK(c.call(kMutexCreate, out, 0) == rc(CELL_EFAULT));
    CHECK(c.call(kMutexCreate, 0, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)) == rc(CELL_EFAULT));
    CHECK(c.call(kMutexCreate, 0x50000000, mutex_attr(f, SYS_SYNC_FIFO, SYS_SYNC_NOT_RECURSIVE)) == rc(CELL_EFAULT));
    // A structure straddling the end of committed memory.
    const u32 edge = Fixture::kScratch + Fixture::kScratchSize - 20;
    CHECK(c.call(kMutexCreate, out, edge) == rc(CELL_EFAULT));
}

// D-166: a slot RPCS3 answers with null_func returns CELL_OK and logs its first call; it is not counted as
// unimplemented. Unused slots (uns_func) still return CELL_ENOSYS.
TEST_CASE("lv2: RPCS3's null_func syscalls return CELL_OK", "[lv2][dispatch]")
{
    Fixture f;
    Caller c(*f.kernel);
    LogCapture log;
    const u64 before = f.kernel->unimplemented_count();
    CHECK(c.call(988) == CELL_OK); // a debug exception handler, as GTA V's CRT registers
    CHECK(log.contains("lv2", "syscall 988 (sys_dbg_..._ppu_exception_handler) is not implemented; returns CELL_OK"));
    // A call a retail console's lv2 does not have is its not-implemented slot's CELL_ENOSYS (D-936), not a null slot.
    CHECK(c.call(215, 1, 2) == rc(CELL_ENOSYS)); // sys_dbg_mat_set_condition
    CHECK(c.call(54) == CELL_OK);  // an unnamed null_func slot (ROOT)
    CHECK(f.kernel->unimplemented_count() == before);
    CHECK(c.call(6) == rc(CELL_ENOSYS)); // uns_func
}
