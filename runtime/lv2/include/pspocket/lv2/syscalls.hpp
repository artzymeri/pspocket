#pragma once

// The implemented lv2 syscalls as typed C++ functions, with RPCS3's argument order (rpcs3/Emu/Cell/lv2/*.h).
// The dispatcher decodes r3..r10 into these parameters (u32 arguments take the low word, as RPCS3's
// ppu_gpr_cast); HLE modules call them directly. Guest pointers are plain u32 guest addresses. A Result return is
// written to r3 sign-extended; u64 returns are written as is.
//
// _sys_ppu_thread_exit and the process exits do not return: they unwind the thread with ThreadExit.

#include "pspocket/core/types.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::lv2 {

class PpuThread;

// ---- process, tty (sys_process.cpp, sys_tty.cpp)
s32 sys_process_getpid(PpuThread& t);
void sys_process_exit3(PpuThread& t, s32 status);
Result sys_process_get_number_of_object(PpuThread& t, u32 object, u32 nump);
s32 sys_process_getppid(PpuThread& t);
void _sys_process_exit(PpuThread& t, s32 status, u32 arg2, u32 arg3);
Result sys_process_get_sdk_version(PpuThread& t, u32 pid, u32 version);
void _sys_process_exit2(PpuThread& t, s32 status, u32 arg, u32 arg_size, u32 arg4);
Result _sys_process_get_paramsfo(PpuThread& t, u32 buffer);
Result sys_tty_read(PpuThread& t, s32 ch, u32 buf, u32 len, u32 preadlen);
Result sys_tty_write(PpuThread& t, s32 ch, u32 buf, u32 len, u32 pwritelen);

// ---- PPU threads (sys_ppu_thread.cpp)
void _sys_ppu_thread_exit(PpuThread& t, u64 errorcode);
s32 sys_ppu_thread_yield(PpuThread& t);
Result sys_ppu_thread_join(PpuThread& t, u32 thread_id, u32 vptr);
Result sys_ppu_thread_detach(PpuThread& t, u32 thread_id);
Result sys_ppu_thread_get_join_state(PpuThread& t, u32 isjoinable);
Result sys_ppu_thread_set_priority(PpuThread& t, u32 thread_id, s32 prio);
Result sys_ppu_thread_get_priority(PpuThread& t, u32 thread_id, u32 priop);
Result sys_ppu_thread_get_stack_information(PpuThread& t, u32 sp);
Result sys_ppu_thread_stop(PpuThread& t, u32 thread_id);
Result sys_ppu_thread_restart(PpuThread& t);
Result _sys_ppu_thread_create(PpuThread& t, u32 thread_id, u32 param, u64 arg, u64 unk, s32 prio, u64 stacksize,
    u64 flags, u32 threadname);
Result sys_ppu_thread_start(PpuThread& t, u32 thread_id);
Result sys_ppu_thread_rename(PpuThread& t, u32 thread_id, u32 name);
Result sys_ppu_thread_recover_page_fault(PpuThread& t, u32 thread_id);
Result sys_ppu_thread_get_page_fault_context(PpuThread& t, u32 thread_id, u32 ctxt);
// sysPrxForUser's sys_ppu_thread_get_id has no syscall; HLE uses this.
u32 sys_ppu_thread_get_id(PpuThread& t);

// ---- timers and time (sys_timer.cpp, sys_time.cpp)
Result sys_timer_create(PpuThread& t, u32 timer_id);
Result sys_timer_destroy(PpuThread& t, u32 timer_id);
Result sys_timer_get_information(PpuThread& t, u32 timer_id, u32 info);
Result _sys_timer_start(PpuThread& t, u32 timer_id, u64 base_time, u64 period);
Result sys_timer_stop(PpuThread& t, u32 timer_id);
Result sys_timer_connect_event_queue(PpuThread& t, u32 timer_id, u32 queue_id, u64 name, u64 data1, u64 data2);
Result sys_timer_disconnect_event_queue(PpuThread& t, u32 timer_id);
Result sys_timer_usleep(PpuThread& t, u64 sleep_time);
Result sys_timer_sleep(PpuThread& t, u32 sleep_time);
Result sys_time_set_timezone(PpuThread& t, s32 timezone, s32 summertime);
Result sys_time_get_timezone(PpuThread& t, u32 timezone, u32 summertime);
Result sys_time_get_current_time(PpuThread& t, u32 sec, u32 nsec);
Result sys_time_set_current_time(PpuThread& t, s64 sec, s64 nsec);
u64 sys_time_get_timebase_frequency(PpuThread& t);
Result sys_time_get_system_time(PpuThread& t); // not in a retail lv2: CELL_ENOSYS (D-938)

// Guest system time: microseconds since the timebase origin (ps_timebase_now at 79.8 MHz), the clock timers and
// sys_time_get_system_time use.
u64 guest_system_time_us();
// What sys_time_get_current_time and sys_time_get_timezone report, for HLE modules: the host's wall clock (seconds and
// nanoseconds since 1970) and its time zone (minutes east of UTC without daylight saving time, and the daylight
// saving offset in minutes).
void host_current_time(s64& sec, s64& nsec);
void host_time_zone(s32& timezone, s32& summertime);

// ---- event flags (sys_event_flag.cpp)
Result sys_event_flag_create(PpuThread& t, u32 id, u32 attr, u64 init);
Result sys_event_flag_destroy(PpuThread& t, u32 id);
Result sys_event_flag_wait(PpuThread& t, u32 id, u64 bitptn, u32 mode, u32 result, u64 timeout);
Result sys_event_flag_trywait(PpuThread& t, u32 id, u64 bitptn, u32 mode, u32 result);
Result sys_event_flag_set(PpuThread& t, u32 id, u64 bitptn);
Result sys_event_flag_clear(PpuThread& t, u32 id, u64 bitptn);
Result sys_event_flag_cancel(PpuThread& t, u32 id, u32 num);
Result sys_event_flag_get(PpuThread& t, u32 id, u32 flags);

// ---- semaphores (sys_semaphore.cpp)
Result sys_semaphore_create(PpuThread& t, u32 sem_id, u32 attr, s32 initial_val, s32 max_val);
Result sys_semaphore_destroy(PpuThread& t, u32 sem_id);
Result sys_semaphore_wait(PpuThread& t, u32 sem_id, u64 timeout);
Result sys_semaphore_trywait(PpuThread& t, u32 sem_id);
Result sys_semaphore_post(PpuThread& t, u32 sem_id, s32 count);
Result sys_semaphore_get_value(PpuThread& t, u32 sem_id, u32 count);

// ---- lightweight mutex / condition, kernel side (sys_lwmutex.cpp, sys_lwcond.cpp)
Result _sys_lwmutex_create(PpuThread& t, u32 lwmutex_id, u32 protocol, u32 control, s32 has_name, u64 name);
Result _sys_lwmutex_destroy(PpuThread& t, u32 lwmutex_id);
Result _sys_lwmutex_lock(PpuThread& t, u32 lwmutex_id, u64 timeout);
Result _sys_lwmutex_unlock(PpuThread& t, u32 lwmutex_id);
Result _sys_lwmutex_trylock(PpuThread& t, u32 lwmutex_id);
Result _sys_lwmutex_unlock2(PpuThread& t, u32 lwmutex_id);
Result _sys_lwcond_create(PpuThread& t, u32 lwcond_id, u32 lwmutex_id, u32 control, u64 name);
Result _sys_lwcond_destroy(PpuThread& t, u32 lwcond_id);
Result _sys_lwcond_queue_wait(PpuThread& t, u32 lwcond_id, u32 lwmutex_id, u64 timeout);
Result _sys_lwcond_signal(PpuThread& t, u32 lwcond_id, u32 lwmutex_id, u64 ppu_thread_id, u32 mode);
Result _sys_lwcond_signal_all(PpuThread& t, u32 lwcond_id, u32 lwmutex_id, u32 mode);

// ---- mutex, condition variable, rwlock (sys_mutex.cpp, sys_cond.cpp, sys_rwlock.cpp)
Result sys_mutex_create(PpuThread& t, u32 mutex_id, u32 attr);
Result sys_mutex_destroy(PpuThread& t, u32 mutex_id);
Result sys_mutex_lock(PpuThread& t, u32 mutex_id, u64 timeout);
Result sys_mutex_trylock(PpuThread& t, u32 mutex_id);
Result sys_mutex_unlock(PpuThread& t, u32 mutex_id);
Result sys_cond_create(PpuThread& t, u32 cond_id, u32 mutex_id, u32 attr);
Result sys_cond_destroy(PpuThread& t, u32 cond_id);
Result sys_cond_wait(PpuThread& t, u32 cond_id, u64 timeout);
Result sys_cond_signal(PpuThread& t, u32 cond_id);
Result sys_cond_signal_all(PpuThread& t, u32 cond_id);
Result sys_cond_signal_to(PpuThread& t, u32 cond_id, u32 thread_id);
Result sys_rwlock_create(PpuThread& t, u32 rw_lock_id, u32 attr);
Result sys_rwlock_destroy(PpuThread& t, u32 rw_lock_id);
Result sys_rwlock_rlock(PpuThread& t, u32 rw_lock_id, u64 timeout);
Result sys_rwlock_tryrlock(PpuThread& t, u32 rw_lock_id);
Result sys_rwlock_runlock(PpuThread& t, u32 rw_lock_id);
Result sys_rwlock_wlock(PpuThread& t, u32 rw_lock_id, u64 timeout);
Result sys_rwlock_trywlock(PpuThread& t, u32 rw_lock_id);
Result sys_rwlock_wunlock(PpuThread& t, u32 rw_lock_id);
Result _sys_rwlock_trywlock(PpuThread& t, u32 rw_lock_id);

// ---- event queues and ports (sys_event.cpp)
Result sys_event_queue_create(PpuThread& t, u32 equeue_id, u32 attr, u64 event_queue_key, s32 size);
Result sys_event_queue_destroy(PpuThread& t, u32 equeue_id, s32 mode);
Result sys_event_queue_receive(PpuThread& t, u32 equeue_id, u32 dummy_event, u64 timeout);
Result sys_event_queue_tryreceive(PpuThread& t, u32 equeue_id, u32 event_array, s32 size, u32 number);
Result sys_event_queue_drain(PpuThread& t, u32 equeue_id);
Result sys_event_port_create(PpuThread& t, u32 eport_id, s32 port_type, u64 name);
Result sys_event_port_destroy(PpuThread& t, u32 eport_id);
Result sys_event_port_connect_local(PpuThread& t, u32 eport_id, u32 equeue_id);
Result sys_event_port_disconnect(PpuThread& t, u32 eport_id);
Result sys_event_port_send(PpuThread& t, u32 eport_id, u64 data1, u64 data2, u64 data3);
Result sys_event_port_connect_ipc(PpuThread& t, u32 eport_id, u64 ipc_key);

// ---- memory (sys_memory.cpp, sys_mmapper.cpp)
Result sys_memory_container_create(PpuThread& t, u32 cid, u64 size);
Result sys_memory_container_destroy(PpuThread& t, u32 cid);
Result sys_memory_container_get_size(PpuThread& t, u32 mem_info, u32 cid);
Result sys_memory_container_destroy_parent_with_childs(PpuThread& t, u32 cid, u32 must_0, u32 mc_child);
Result sys_mmapper_enable_page_fault_notification(PpuThread& t, u32 start_addr, u32 event_queue_id);
Result sys_memory_allocate(PpuThread& t, u64 size, u64 flags, u32 alloc_addr);
Result sys_memory_free(PpuThread& t, u32 start_addr);
Result sys_memory_allocate_from_container(PpuThread& t, u64 size, u32 cid, u64 flags, u32 alloc_addr);
Result sys_memory_get_page_attribute(PpuThread& t, u32 addr, u32 attr);
Result sys_memory_get_user_memory_size(PpuThread& t, u32 mem_info);
Result sys_mmapper_allocate_fixed_address(PpuThread& t);
Result sys_mmapper_free_shared_memory(PpuThread& t, u32 mem_id);
Result sys_mmapper_allocate_address(PpuThread& t, u64 size, u64 flags, u64 alignment, u32 alloc_addr);
Result sys_mmapper_free_address(PpuThread& t, u32 addr);
Result sys_mmapper_allocate_shared_memory(PpuThread& t, u64 ipc_key, u64 size, u64 flags, u32 mem_id);
Result sys_mmapper_map_shared_memory(PpuThread& t, u32 addr, u32 mem_id, u64 flags);
Result sys_mmapper_unmap_shared_memory(PpuThread& t, u32 addr, u32 mem_id);
Result sys_mmapper_search_and_map(PpuThread& t, u32 start_addr, u32 mem_id, u64 flags, u32 alloc_addr);
Result sys_mmapper_allocate_shared_memory_from_container(PpuThread& t, u64 ipc_key, u64 size, u32 cid, u64 flags,
    u32 mem_id);
Result sys_mmapper_allocate_shared_memory_ext(PpuThread& t, u64 ipc_key, u64 size, u32 flags, u32 entries,
    s32 entry_count, u32 mem_id);
Result sys_mmapper_allocate_shared_memory_from_container_ext(PpuThread& t, u64 ipc_key, u64 size, u64 flags, u32 cid,
    u32 entries, s32 entry_count, u32 mem_id);

// ---- filesystem (sys_fs.cpp)
enum : s32 {
    CELL_FS_O_RDONLY = 000000,
    CELL_FS_O_WRONLY = 000001,
    CELL_FS_O_RDWR = 000002,
    CELL_FS_O_ACCMODE = 000003,
    CELL_FS_O_CREAT = 000100,
    CELL_FS_O_EXCL = 000200,
    CELL_FS_O_TRUNC = 001000,
    CELL_FS_O_APPEND = 002000,
    CELL_FS_O_MSELF = 010000,
    CELL_FS_O_UNK = 01000000,
};
enum : s32 {
    CELL_FS_SEEK_SET = 0,
    CELL_FS_SEEK_CUR = 1,
    CELL_FS_SEEK_END = 2,
};
enum : s32 {
    CELL_FS_S_IFDIR = 0040000,
    CELL_FS_S_IFREG = 0100000,
};
enum : u8 {
    CELL_FS_TYPE_UNKNOWN = 0,
    CELL_FS_TYPE_DIRECTORY = 1,
    CELL_FS_TYPE_REGULAR = 2,
};
inline constexpr u32 kCellFsStatSize = 52;    // CellFsStat (4-byte aligned 64-bit fields)
inline constexpr u32 kCellFsDirentSize = 258; // CellFsDirent
inline constexpr u32 kCellFsDirectoryEntrySize = 312; // CellFsDirectoryEntry: CellFsStat, CellFsDirent, 4-aligned

Result sys_fs_open(PpuThread& t, u32 path, s32 flags, u32 fd, s32 mode, u32 arg, u64 size);
Result sys_fs_read(PpuThread& t, u32 fd, u32 buf, u64 nbytes, u32 nread);
Result sys_fs_write(PpuThread& t, u32 fd, u32 buf, u64 nbytes, u32 nwrite);
Result sys_fs_close(PpuThread& t, u32 fd);
Result sys_fs_opendir(PpuThread& t, u32 path, u32 fd);
Result sys_fs_readdir(PpuThread& t, u32 fd, u32 dir, u32 nread);
Result sys_fs_closedir(PpuThread& t, u32 fd);
Result sys_fs_stat(PpuThread& t, u32 path, u32 sb);
Result sys_fs_fstat(PpuThread& t, u32 fd, u32 sb);
Result sys_fs_mkdir(PpuThread& t, u32 path, s32 mode);
Result sys_fs_rename(PpuThread& t, u32 from, u32 to);
Result sys_fs_rmdir(PpuThread& t, u32 path);
Result sys_fs_unlink(PpuThread& t, u32 path);
Result sys_fs_lseek(PpuThread& t, u32 fd, s64 offset, s32 whence, u32 pos);
Result sys_fs_fdatasync(PpuThread& t, u32 fd);
Result sys_fs_fsync(PpuThread& t, u32 fd);
Result sys_fs_truncate(PpuThread& t, u32 path, u64 size);
Result sys_fs_ftruncate(PpuThread& t, u32 fd, u64 size);
Result sys_fs_chmod(PpuThread& t, u32 path, s32 mode);
Result sys_fs_utime(PpuThread& t, u32 path, u32 timep);
Result sys_fs_fcntl(PpuThread& t, u32 fd, u32 op, u32 arg, u32 size);
Result sys_fs_test(PpuThread& t, u32 arg1, u32 arg2, u32 arg3, u32 arg4, u32 buf, u32 buf_size);
Result sys_fs_fget_block_size(PpuThread& t, u32 fd, u32 sector_size, u32 block_size, u32 arg4, u32 out_flags);
Result sys_fs_get_block_size(PpuThread& t, u32 path, u32 sector_size, u32 block_size, u32 arg4);
Result sys_fs_disk_free(PpuThread& t, u32 path, u32 total_free, u32 avail_free);
Result sys_fs_get_mount_info_size(PpuThread& t, u32 len);
Result sys_fs_get_mount_info(PpuThread& t, u32 info, u64 len, u32 out_len);
Result sys_fs_lsn_get_cda_size(PpuThread& t, u32 fd, u32 ptr);
Result sys_fs_lsn_lock(PpuThread& t, u32 fd);
Result sys_fs_lsn_unlock(PpuThread& t, u32 fd);
Result sys_fs_link(PpuThread& t, u32 from, u32 to);
Result sys_fs_access(PpuThread& t, u32 path, s32 mode);
Result sys_fs_acl_read(PpuThread& t, u32 path, u32 acl);
Result sys_fs_acl_write(PpuThread& t, u32 path, u32 acl);
Result sys_fs_lsn_get_cda(PpuThread& t, u32 fd, u32 buf, u64 size, u32 count);
Result sys_fs_lsn_read(PpuThread& t, u32 fd, u32 ctrl, u32 buf, u64 unk);
Result sys_fs_lsn_write(PpuThread& t, u32 fd, u32 ctrl, u32 buf, u64 unk);
Result sys_fs_symbolic_link(PpuThread& t, u32 target, u32 link);
Result sys_fs_chown(PpuThread& t, u32 path, s32 uid, s32 gid);
Result sys_fs_mapped_allocate(PpuThread& t, u32 fd, u64 size, u32 out);
Result sys_fs_mapped_free(PpuThread& t, u32 fd, u32 ptr);
Result sys_fs_truncate2(PpuThread& t, u32 fd, u64 size);
Result sys_fs_newfs(PpuThread& t, u32 dev_name, u32 file_system, s32 unk1, u32 str1);
Result sys_fs_mount(PpuThread& t, u32 dev_name, u32 file_system, u32 path, s32 unk1, s32 prot, s32 unk2, u32 str1,
    u32 str_len);
Result sys_fs_unmount(PpuThread& t, u32 path, s32 unk1, s32 force);

// ---- virtual memory (sys_vm.cpp)
Result sys_vm_memory_map(PpuThread& t, u64 vsize, u64 psize, u32 cid, u64 flag, u64 policy, u32 addr_out);
Result sys_vm_memory_map_different(PpuThread& t, u64 vsize, u64 psize, u32 cid, u64 flag, u64 policy, u32 addr_out);
Result sys_vm_unmap(PpuThread& t, u32 addr);
Result sys_vm_append_memory(PpuThread& t, u32 addr, u64 size);
Result sys_vm_return_memory(PpuThread& t, u32 addr, u64 size);
Result sys_vm_lock(PpuThread& t, u32 addr, u32 size);
Result sys_vm_unlock(PpuThread& t, u32 addr, u32 size);
Result sys_vm_touch(PpuThread& t, u32 addr, u32 size);
Result sys_vm_flush(PpuThread& t, u32 addr, u32 size);
Result sys_vm_invalidate(PpuThread& t, u32 addr, u32 size);
Result sys_vm_store(PpuThread& t, u32 addr, u32 size);
Result sys_vm_sync(PpuThread& t, u32 addr, u32 size);
Result sys_vm_test(PpuThread& t, u32 addr, u32 size, u32 result);
Result sys_vm_get_statistics(PpuThread& t, u32 addr, u32 stat);

// ---- raw SPUs (raw_spu.cpp, D-528)
Result sys_raw_spu_create(PpuThread& t, u32 id, u32 attr);
Result sys_raw_spu_destroy(PpuThread& t, u32 id);

// ---- SPU (sys_spu.cpp)
Result sys_spu_initialize(PpuThread& t, u32 max_usable_spu, u32 max_raw_spu);
Result sys_spu_thread_group_log(PpuThread& t, s32 command, u32 stat);
Result _sys_spu_image_get_information(PpuThread& t, u32 img, u32 entry_point, u32 nsegs);
Result sys_spu_image_open(PpuThread& t, u32 img, u32 path);
Result sys_spu_image_open_by_fd(PpuThread& t, u32 img, s32 fd, s64 offset);
Result _sys_spu_image_import(PpuThread& t, u32 img, u32 src, u32 size, u32 arg4);
Result _sys_spu_image_close(PpuThread& t, u32 img);
Result _sys_spu_image_get_segments(PpuThread& t, u32 img, u32 segments, s32 nseg);
Result sys_spu_thread_group_create(PpuThread& t, u32 id, u32 num, s32 prio, u32 attr);
Result sys_spu_thread_group_destroy(PpuThread& t, u32 id);
Result sys_spu_thread_initialize(PpuThread& t, u32 thread, u32 group, u32 spu_num, u32 img, u32 attr, u32 arg);
Result sys_spu_thread_set_argument(PpuThread& t, u32 id, u32 arg);
Result sys_spu_thread_get_exit_status(PpuThread& t, u32 id, u32 status);
Result sys_spu_thread_recover_page_fault(PpuThread& t, u32 id);
Result sys_spu_thread_group_start(PpuThread& t, u32 id);
Result sys_spu_thread_group_suspend(PpuThread& t, u32 id);
Result sys_spu_thread_group_resume(PpuThread& t, u32 id);
Result sys_spu_thread_group_yield(PpuThread& t, u32 id);
Result sys_spu_thread_group_terminate(PpuThread& t, u32 id, s32 value);
Result sys_spu_thread_group_join(PpuThread& t, u32 id, u32 cause, u32 status);
Result sys_spu_thread_group_set_priority(PpuThread& t, u32 id, s32 priority);
Result sys_spu_thread_group_get_priority(PpuThread& t, u32 id, u32 priority);
Result sys_spu_thread_write_ls(PpuThread& t, u32 id, u32 lsa, u64 value, u32 type);
Result sys_spu_thread_read_ls(PpuThread& t, u32 id, u32 lsa, u32 value, u32 type);
Result sys_spu_thread_write_spu_mb(PpuThread& t, u32 id, u32 value);
Result sys_spu_thread_set_spu_cfg(PpuThread& t, u32 id, u64 value);
Result sys_spu_thread_get_spu_cfg(PpuThread& t, u32 id, u32 value);
Result sys_spu_thread_write_snr(PpuThread& t, u32 id, u32 number, u32 value);
Result sys_spu_thread_group_connect_event(PpuThread& t, u32 id, u32 eq, u32 et);
Result sys_spu_thread_group_disconnect_event(PpuThread& t, u32 id, u32 et);
Result sys_spu_thread_connect_event(PpuThread& t, u32 id, u32 eq, u32 et, u32 spup);
Result sys_spu_thread_disconnect_event(PpuThread& t, u32 id, u32 et, u32 spup);
Result sys_spu_thread_bind_queue(PpuThread& t, u32 id, u32 spuq, u32 spuq_num);
Result sys_spu_thread_unbind_queue(PpuThread& t, u32 id, u32 spuq_num);
Result sys_spu_thread_group_connect_event_all_threads(PpuThread& t, u32 id, u32 eq, u64 req, u32 spup);
Result sys_spu_thread_group_disconnect_event_all_threads(PpuThread& t, u32 id, u32 spup);
Result sys_spu_thread_group_set_cooperative_victims(PpuThread& t, u32 id, u32 threads_mask);
Result sys_spu_thread_group_syscall_253(PpuThread& t, u32 id, u32 info);

// ---- USB (usbd.cpp): no device attached (D-522).
Result sys_usbd_initialize(PpuThread& t, u32 handle);
Result sys_usbd_finalize(PpuThread& t, u32 handle);
Result sys_usbd_get_device_list(PpuThread& t, u32 handle, u32 list, u32 max);
Result sys_usbd_register_extra_ldd(PpuThread& t, u32 handle, u32 name, u32 len, u32 vendor, u32 pmin, u32 pmax);
Result sys_usbd_unregister_extra_ldd(PpuThread& t, u32 handle, u32 name, u32 len);
Result sys_usbd_register_ldd(PpuThread& t, u32 handle, u32 name, u32 len);
Result sys_usbd_unregister_ldd(PpuThread& t, u32 handle, u32 name, u32 len);
Result sys_usbd_get_descriptor_size(PpuThread& t, u32 handle, u32 device);
Result sys_usbd_get_descriptor(PpuThread& t, u32 handle, u32 device, u32 descriptor, u32 size);
Result sys_usbd_open_pipe(PpuThread& t, u32 handle, u32 device, u32 unk1, u64 unk2, u64 unk3, u32 endpoint, u64 attr);
Result sys_usbd_open_default_pipe(PpuThread& t, u32 handle, u32 device);
Result sys_usbd_close_pipe(PpuThread& t, u32 handle, u32 pipe);
Result sys_usbd_receive_event(PpuThread& t, u32 handle, u32 arg1, u32 arg2, u32 arg3);
Result sys_usbd_event_port_send(PpuThread& t, u32 handle, u64 arg1, u64 arg2, u64 arg3);
Result sys_usbd_transfer_data(PpuThread& t, u32 handle, u32 pipe, u32 buf, u32 size, u32 request, u32 type);
Result sys_usbd_isochronous_transfer_data(PpuThread& t, u32 handle, u32 pipe, u32 request);
Result sys_usbd_get_transfer_status(PpuThread& t, u32 handle, u32 transfer, u32 unk1, u32 result, u32 count);
Result sys_usbd_get_isochronous_transfer_status(PpuThread& t, u32 handle, u32 transfer, u32 unk1, u32 req, u32 result);
Result sys_usbd_get_device_location(PpuThread& t, u32 handle, u32 device, u32 location);
Result sys_usbd_get_device_speed(PpuThread& t, u32 handle, u32 device, u32 speed);
Result sys_usbd_detect_event(PpuThread& t);
Result sys_usbd_attach(PpuThread& t, u32 handle, u32 ldd, u32 device_high, u32 device_low);
Result sys_usbd_send_event(PpuThread& t, u32 handle, u32 device);
Result sys_usbd_allocate_memory(PpuThread& t, u32 handle, u32 addr, u64 size);
Result sys_usbd_free_memory(PpuThread& t, u32 handle, u32 addr);

// ---- RSX (rsx.cpp). Called by HLE cellGcmSys, and by number (668, 670, 672-675, D-519).
Result sys_rsx_memory_allocate(PpuThread& t, u32 mem_handle, u32 mem_addr, u64 size, u64 flags, u64 a5, u64 a6, u64 a7);
Result sys_rsx_context_allocate(PpuThread& t, u32 context_id, u32 lpar_dma_control, u32 lpar_driver_info,
    u32 lpar_reports, u64 mem_ctx, u64 system_mode);
Result sys_rsx_context_iomap(PpuThread& t, u32 context_id, u64 io, u64 ea, u64 size, u64 flags);
Result sys_rsx_context_iounmap(PpuThread& t, u32 context_id, u64 io, u64 size);
Result sys_rsx_context_attribute(PpuThread& t, u32 context_id, u32 package_id, u64 a3, u64 a4, u64 a5, u64 a6);
Result sys_rsx_device_map(PpuThread& t, u32 dev_addr, u32 a2, u32 dev_id);
Result sys_rsx_memory_free(PpuThread& t, u32 mem_handle);
Result sys_rsx_context_free(PpuThread& t, u32 context_id);

} // namespace pspocket::lv2
