// lv2's remaining small families, as a retail console running a game (D-527 part 2). RPCS3 answers most of these with
// a todo or a null slot (CELL_OK, nothing written: rpcs3/Emu/Cell/lv2/sys_process.cpp, sys_time.cpp, sys_console.cpp,
// sys_trace.cpp, sys_btsetting.cpp, sys_bdemu.cpp, sys_gamepad.cpp, sys_bluetooth.cpp, sys_mmapper.cpp, sys_ss.cpp and
// lv2.cpp's table); pspocket's answers follow its console:
//
// - Debugging and tracing hardware is a debug or test kernel's: the lv1 trace (sys_trace_*), the PC trace and
//   performance monitor, DECI3 (the debugger link), the Blu-ray disc emulator: CELL_ENOSYS, as RPCS3 answers the trace
//   calls and sys_storage_set_emulated_speed ("only debug kernel has this"). The USB/Bluetooth audio calls are
//   marked deprecated in RPCS3's table: CELL_ENOSYS.
// - The Bluetooth module, its settings and the NP DRM key cache are the system software's: a game gets CELL_ENOSYS
//   (RPCS3's table marks the family's management calls ROOT); a root process gets CELL_OK and nothing, there being
//   no Bluetooth module to drive.
// - One process: no children to wait for, detach or kill (CELL_ESRCH), no other process to spawn (root: CELL_ENOTSUP);
//   the calls RPCS3 marks ROOT or DBG are a game's CELL_ENOSYS.
// - Calls Sony's game-loadable modules make keep working for a game whatever RPCS3's mark (lv2 must let them: the
//   modules run in the game's process): the SPU thread group's system next/switch group hints and start-on-exit
//   (libatxenc, libaacenc, libat3enc; pspocket schedules groups on host threads, nothing to hint), the shared-memory
//   flag, attribute and access-right calls (libatxenc, libat3enc; pspocket's guest memory is read-write throughout),
//   the security framework (libmcadpt), the disc access control and media ID (liblv2), the ad signature (libad_core,
//   libmedi), the file-system watch events (liblv2coredump: none happen), the YCON gamepad interface (libgem, libio:
//   no such devices) and the LDD pad calls (libio, which pspocket serves itself): CELL_OK.
// - sys_console_write2 prints to the log as a TTY write does; sys_console_write is root's.

#include <map>
#include <string>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kMiscChan = "lv2";
constexpr u32 SYS_BLUETOOTH_ERROR_NOSYS = 0x80111003; // RPCS3 sys_bluetooth.h

void log_once(std::string_view name, std::string_view what)
{
    static std::mutex m;
    static std::map<std::string_view, bool> logged;
    std::lock_guard lock(m);
    if (logged[name]) return;
    logged[name] = true;
    PS_LOG_INFO(kMiscChan, "{}: {}", name, what);
}

bool is_root(PpuThread& t) { return t.kernel.config().root; }

Result debug_kernel(std::string_view name)
{
    log_once(name, "a debug kernel's call, not a retail console's: CELL_ENOSYS");
    return err(CELL_ENOSYS);
}

Result system_only(PpuThread& t, std::string_view name)
{
    if (is_root(t)) {
        log_once(name, "nothing to do on this console: CELL_OK");
        return CELL_OK;
    }
    log_once(name, "the system software's call, not a game's: CELL_ENOSYS");
    return err(CELL_ENOSYS);
}

Result quiet_ok(std::string_view name, std::string_view why)
{
    log_once(name, why);
    return CELL_OK;
}

Result console_print(PpuThread& t, u32 buf, u32 len)
{
    Kernel::Impl& s = t.kernel.impl();
    if (static_cast<s32>(len) <= 0) return CELL_OK;
    if (!s.guest.valid(buf, len)) return err(CELL_EFAULT);
    std::string msg(reinterpret_cast<const char*>(s.guest.ptr(buf)), len);
    while (!msg.empty() && msg.back() == '\n') msg.pop_back();
    std::lock_guard lock(s.tty_mutex);
    PS_LOG_INFO("console", "{}", msg);
    return CELL_OK;
}

} // namespace

// ------------------------------------------------------------------------------------------------- processes

Result sys_process_wait_for_child(PpuThread& t, u32, u32, u64)
{
    if (!is_root(t)) return system_only(t, "sys_process_wait_for_child");
    return err(CELL_ESRCH);
}
Result sys_process_get_status(PpuThread& t, u64) { return system_only(t, "sys_process_get_status"); }
Result sys_process_detach_child(PpuThread& t, u64)
{
    if (!is_root(t)) return system_only(t, "sys_process_detach_child");
    return err(CELL_ESRCH);
}
// sys_process_kill(pid): a parent's call on a child; the game has none.
Result sys_process_kill(PpuThread&, u32) { return err(CELL_ESRCH); }
Result _sys_process_spawn(PpuThread& t)
{
    if (!is_root(t)) return system_only(t, "_sys_process_spawn");
    log_once("_sys_process_spawn", "one process only: CELL_ENOTSUP");
    return err(CELL_ENOTSUP);
}
Result sys_process_wait_for_child2(PpuThread& t, u64, u64, u64, u64, u64, u64)
{
    if (!is_root(t)) return system_only(t, "sys_process_wait_for_child2");
    return err(CELL_ESRCH);
}
Result sys_process_spawns_a_self2(PpuThread& t, u32, u32, u64, u32, u32, u32, u32, u32)
{
    if (!is_root(t)) return system_only(t, "sys_process_spawns_a_self2");
    log_once("sys_process_spawns_a_self2", "one process only: CELL_ENOTSUP");
    return err(CELL_ENOTSUP);
}
// _sys_process_get_number_of_object(object, nump): root's sys_process_get_number_of_object.
Result _sys_process_get_number_of_object(PpuThread& t, u32 object, u32 nump)
{
    if (!is_root(t)) return system_only(t, "_sys_process_get_number_of_object");
    return sys_process_get_number_of_object(t, object, nump);
}
Result sys_process_get_ppu_guid(PpuThread&) { return quiet_ok("sys_process_get_ppu_guid", "no PPU GUID: CELL_OK"); }

// sys_time_get_rtc(u64* rtc): root; the wall clock's seconds.
Result sys_time_get_rtc(PpuThread& t, u32 rtc)
{
    if (!is_root(t)) return system_only(t, "sys_time_get_rtc");
    s64 sec = 0, nsec = 0;
    host_current_time(sec, nsec);
    if (!t.kernel.impl().guest.put64(rtc, static_cast<u64>(sec))) return err(CELL_EFAULT);
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- console, trace

Result sys_console_write(PpuThread& t, u32 buf, u32 len)
{
    if (!is_root(t)) return system_only(t, "sys_console_write");
    return console_print(t, buf, len);
}
Result sys_console_write2(PpuThread& t, u32 buf, u32 len) { return console_print(t, buf, len); }

Result sys_trace_create(PpuThread&) { return debug_kernel("sys_trace_create"); }
Result sys_trace_start(PpuThread&) { return debug_kernel("sys_trace_start"); }
Result sys_trace_stop(PpuThread&) { return debug_kernel("sys_trace_stop"); }
Result sys_trace_update_top_index(PpuThread&) { return debug_kernel("sys_trace_update_top_index"); }
Result sys_trace_destroy(PpuThread&) { return debug_kernel("sys_trace_destroy"); }
Result sys_trace_drain(PpuThread&) { return debug_kernel("sys_trace_drain"); }
Result sys_trace_attach_process(PpuThread&) { return debug_kernel("sys_trace_attach_process"); }
Result sys_trace_allocate_buffer(PpuThread&) { return debug_kernel("sys_trace_allocate_buffer"); }
Result sys_trace_free_buffer(PpuThread&) { return debug_kernel("sys_trace_free_buffer"); }
Result sys_trace_create2(PpuThread&) { return debug_kernel("sys_trace_create2"); }
Result sys_trace_create2_in_cbepm(PpuThread&) { return debug_kernel("sys_trace_create2_in_cbepm"); }
Result sys_control_performance_monitor(PpuThread&) { return debug_kernel("sys_control_performance_monitor"); }
Result sys_performance_monitor_hidden(PpuThread&) { return debug_kernel("sys_performance_monitor_hidden"); }
Result sys_performance_monitor_bookmark(PpuThread&) { return debug_kernel("sys_performance_monitor_bookmark"); }
Result sys_lv1_pc_trace_create(PpuThread&) { return debug_kernel("sys_lv1_pc_trace_create"); }
Result sys_lv1_pc_trace_start(PpuThread&) { return debug_kernel("sys_lv1_pc_trace_start"); }
Result sys_lv1_pc_trace_stop(PpuThread&) { return debug_kernel("sys_lv1_pc_trace_stop"); }
Result sys_lv1_pc_trace_get_status(PpuThread&) { return debug_kernel("sys_lv1_pc_trace_get_status"); }
Result sys_lv1_pc_trace_destroy(PpuThread&) { return debug_kernel("sys_lv1_pc_trace_destroy"); }
Result sys_bdemu_send_command(PpuThread&, u64, u64, u64, u32, u64) { return debug_kernel("sys_bdemu_send_command"); }

Result sys_deci3_open(PpuThread&) { return debug_kernel("sys_deci3_open"); }
Result sys_deci3_create_event_path(PpuThread&) { return debug_kernel("sys_deci3_create_event_path"); }
Result sys_deci3_close(PpuThread&) { return debug_kernel("sys_deci3_close"); }
Result sys_deci3_send(PpuThread&) { return debug_kernel("sys_deci3_send"); }
Result sys_deci3_receive(PpuThread&) { return debug_kernel("sys_deci3_receive"); }
Result sys_deci3_open2(PpuThread&) { return debug_kernel("sys_deci3_open2"); }
Result sys_deci3_initialize(PpuThread&) { return debug_kernel("sys_deci3_initialize"); }
Result sys_deci3_terminate(PpuThread&) { return debug_kernel("sys_deci3_terminate"); }
Result sys_deci3_debug_mode(PpuThread&) { return debug_kernel("sys_deci3_debug_mode"); }
Result sys_deci3_show_status(PpuThread&) { return debug_kernel("sys_deci3_show_status"); }
Result sys_deci3_echo_test(PpuThread&) { return debug_kernel("sys_deci3_echo_test"); }
Result sys_deci3_send_dcmp_packet(PpuThread&) { return debug_kernel("sys_deci3_send_dcmp_packet"); }
Result sys_deci3_dump_cp_register(PpuThread&) { return debug_kernel("sys_deci3_dump_cp_register"); }
Result sys_deci3_dump_cp_buffer(PpuThread&) { return debug_kernel("sys_deci3_dump_cp_buffer"); }
Result sys_deci3_test(PpuThread&) { return debug_kernel("sys_deci3_test"); }

Result sys_usbbtaudio_initialize(PpuThread&) { return debug_kernel("sys_usbbtaudio_initialize"); }
Result sys_usbbtaudio_finalize(PpuThread&) { return debug_kernel("sys_usbbtaudio_finalize"); }
Result sys_usbbtaudio_discovery(PpuThread&) { return debug_kernel("sys_usbbtaudio_discovery"); }
Result sys_usbbtaudio_cancel_discovery(PpuThread&) { return debug_kernel("sys_usbbtaudio_cancel_discovery"); }
Result sys_usbbtaudio_pairing(PpuThread&) { return debug_kernel("sys_usbbtaudio_pairing"); }
Result sys_usbbtaudio_set_passkey(PpuThread&) { return debug_kernel("sys_usbbtaudio_set_passkey"); }
Result sys_usbbtaudio_connect(PpuThread&) { return debug_kernel("sys_usbbtaudio_connect"); }
Result sys_usbbtaudio_disconnect(PpuThread&) { return debug_kernel("sys_usbbtaudio_disconnect"); }

// ------------------------------------------------------------------------------------------------- the system's

Result sys_bt_read_firmware_version(PpuThread& t) { return system_only(t, "sys_bt_read_firmware_version"); }
Result sys_bt_complete_wake_on_host(PpuThread& t) { return system_only(t, "sys_bt_complete_wake_on_host"); }
Result sys_bt_disable_bluetooth(PpuThread& t) { return system_only(t, "sys_bt_disable_bluetooth"); }
Result sys_bt_enable_bluetooth(PpuThread& t) { return system_only(t, "sys_bt_enable_bluetooth"); }
Result sys_bt_bccmd(PpuThread& t) { return system_only(t, "sys_bt_bccmd"); }
Result sys_bt_read_hq(PpuThread& t) { return system_only(t, "sys_bt_read_hq"); }
Result sys_bt_hid_get_remote_status(PpuThread& t) { return system_only(t, "sys_bt_hid_get_remote_status"); }
Result sys_bt_register_controller(PpuThread& t) { return system_only(t, "sys_bt_register_controller"); }
Result sys_bt_clear_registered_contoller(PpuThread& t) { return system_only(t, "sys_bt_clear_registered_contoller"); }
Result sys_bt_connect_accept_controller(PpuThread& t) { return system_only(t, "sys_bt_connect_accept_controller"); }
Result sys_bt_get_local_bdaddress(PpuThread& t) { return system_only(t, "sys_bt_get_local_bdaddress"); }
Result sys_bt_hid_get_data(PpuThread& t) { return system_only(t, "sys_bt_hid_get_data"); }
Result sys_bt_hid_set_report(PpuThread& t) { return system_only(t, "sys_bt_hid_set_report"); }
Result sys_bt_sched_log(PpuThread& t) { return system_only(t, "sys_bt_sched_log"); }
Result sys_bt_cancel_connect_accept_controller(PpuThread& t)
{
    return system_only(t, "sys_bt_cancel_connect_accept_controller");
}
Result sys_btsetting_if(PpuThread& t, u64, u32) { return system_only(t, "sys_btsetting_if"); }
Result sys_npdrm_check_ekc(PpuThread& t) { return system_only(t, "sys_npdrm_check_ekc"); }
Result sys_npdrm_regist_ekc(PpuThread& t) { return system_only(t, "sys_npdrm_regist_ekc"); }
Result sys_ss_get_cache_of_analog_sunset_flag(PpuThread& t)
{
    return system_only(t, "sys_ss_get_cache_of_analog_sunset_flag");
}
Result sys_ss_protected_file_db(PpuThread& t) { return system_only(t, "sys_ss_protected_file_db"); }
Result sys_ss_virtual_trm_manager(PpuThread& t) { return system_only(t, "sys_ss_virtual_trm_manager"); }
Result sys_ss_factory_data_manager(PpuThread& t) { return system_only(t, "sys_ss_factory_data_manager"); }

// sys_bluetooth_aud_serial_get_event_579(arg, u32* out1..out4): RPCS3's checks; no event.
Result sys_bluetooth_aud_serial_get_event_579(PpuThread&, u32 arg, u32 out1, u32 out2, u32 out3, u32 out4)
{
    if (!out1 || !out2 || !out3 || !out4) return err(CELL_EINVAL);
    if (arg > 1) return static_cast<Result>(SYS_BLUETOOTH_ERROR_NOSYS);
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- module-made calls

Result sys_spu_thread_group_start_on_exit(PpuThread&)
{
    return quiet_ok("sys_spu_thread_group_start_on_exit", "groups run on host threads, nothing to arrange: CELL_OK");
}
Result sys_spu_thread_group_system_set_next_group(PpuThread&)
{
    return quiet_ok("sys_spu_thread_group_system_set_next_group", "no group switching to hint: CELL_OK");
}
Result sys_spu_thread_group_system_unset_next_group(PpuThread&)
{
    return quiet_ok("sys_spu_thread_group_system_unset_next_group", "no group switching to hint: CELL_OK");
}
Result sys_spu_thread_group_system_set_switch_group(PpuThread&)
{
    return quiet_ok("sys_spu_thread_group_system_set_switch_group", "no group switching to hint: CELL_OK");
}
Result sys_spu_thread_group_system_unset_switch_group(PpuThread&)
{
    return quiet_ok("sys_spu_thread_group_system_unset_switch_group", "no group switching to hint: CELL_OK");
}
Result sys_mmapper_set_shared_memory_flag(PpuThread&)
{
    return quiet_ok("sys_mmapper_set_shared_memory_flag", "no shared-memory flags kept: CELL_OK");
}
Result sys_mmapper_change_address_access_right(PpuThread&, u32, u64)
{
    return quiet_ok("sys_mmapper_change_address_access_right", "guest memory is read-write throughout: CELL_OK");
}
Result sys_mmapper_get_shared_memory_attribute(PpuThread&)
{
    return quiet_ok("sys_mmapper_get_shared_memory_attribute", "nothing to report: CELL_OK");
}
Result sys_memory_budget_set(PpuThread&) { return quiet_ok("sys_memory_budget_set", "no budgets kept: CELL_OK"); }
Result sys_ss_sec_hw_framework(PpuThread&) { return quiet_ok("sys_ss_sec_hw_framework", "nothing to do: CELL_OK"); }
Result sys_ss_disc_access_control(PpuThread&) { return quiet_ok("sys_ss_disc_access_control", "nothing to do: CELL_OK"); }
Result sys_ss_ad_sign(PpuThread&) { return quiet_ok("sys_ss_ad_sign", "nothing to do: CELL_OK"); }
Result sys_ss_media_id(PpuThread&) { return quiet_ok("sys_ss_media_id", "nothing to report: CELL_OK"); }
Result sys_fsw_connect_event(PpuThread&) { return quiet_ok("sys_fsw_connect_event", "no file-system events: CELL_OK"); }
Result sys_fsw_disconnect_event(PpuThread&)
{
    return quiet_ok("sys_fsw_disconnect_event", "no file-system events: CELL_OK");
}
Result sys_gamepad_ycon_if(PpuThread&, u32 packet, u32, u32)
{
    if ((packet & 0xFF) > 9) PS_LOG_ERROR(kMiscChan, "sys_gamepad_ycon_if: unknown packet {}", packet & 0xFF);
    return quiet_ok("sys_gamepad_ycon_if", "no YCON devices: CELL_OK");
}
Result sys_pad_ldd_register_controller(PpuThread&)
{
    return quiet_ok("sys_pad_ldd_register_controller", "LDD pads are libio's (native): CELL_OK");
}
Result sys_pad_ldd_unregister_controller(PpuThread&)
{
    return quiet_ok("sys_pad_ldd_unregister_controller", "LDD pads are libio's (native): CELL_OK");
}
Result sys_pad_ldd_data_insert(PpuThread&)
{
    return quiet_ok("sys_pad_ldd_data_insert", "LDD pads are libio's (native): CELL_OK");
}
Result sys_pad_dbg_ldd_set_data_insert_mode(PpuThread&)
{
    return quiet_ok("sys_pad_dbg_ldd_set_data_insert_mode", "LDD pads are libio's (native): CELL_OK");
}
Result sys_pad_ldd_get_port_no(PpuThread&)
{
    return quiet_ok("sys_pad_ldd_get_port_no", "LDD pads are libio's (native): CELL_OK");
}
Result sys_fs_sync(PpuThread&) { return quiet_ok("sys_fs_sync", "the host's files hold every write: CELL_OK"); }
Result sys_fs_get_fs_info_size(PpuThread&) { return quiet_ok("sys_fs_get_fs_info_size", "nothing to report: CELL_OK"); }
Result sys_fs_get_fs_info(PpuThread&) { return quiet_ok("sys_fs_get_fs_info", "nothing to report: CELL_OK"); }

void register_misc_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 2, sys_process_wait_for_child);
    PS_LV2_BIND(table, 4, sys_process_get_status);
    PS_LV2_BIND(table, 5, sys_process_detach_child);
    PS_LV2_BIND(table, 19, sys_process_kill);
    PS_LV2_BIND(table, 21, _sys_process_spawn);
    PS_LV2_BIND(table, 23, sys_process_wait_for_child2);
    PS_LV2_BIND(table, 27, sys_process_spawns_a_self2);
    PS_LV2_BIND(table, 28, _sys_process_get_number_of_object);
    PS_LV2_BIND(table, 31, sys_process_get_ppu_guid);
    PS_LV2_BIND(table, 119, sys_time_get_rtc);
    PS_LV2_BIND(table, 398, sys_console_write);
    PS_LV2_BIND(table, 213, sys_console_write2);
    PS_LV2_BIND(table, 60, sys_trace_create);
    PS_LV2_BIND(table, 61, sys_trace_start);
    PS_LV2_BIND(table, 62, sys_trace_stop);
    PS_LV2_BIND(table, 63, sys_trace_update_top_index);
    PS_LV2_BIND(table, 64, sys_trace_destroy);
    PS_LV2_BIND(table, 65, sys_trace_drain);
    PS_LV2_BIND(table, 66, sys_trace_attach_process);
    PS_LV2_BIND(table, 67, sys_trace_allocate_buffer);
    PS_LV2_BIND(table, 68, sys_trace_free_buffer);
    PS_LV2_BIND(table, 69, sys_trace_create2);
    PS_LV2_BIND(table, 77, sys_trace_create2_in_cbepm);
    PS_LV2_BIND(table, 960, sys_control_performance_monitor);
    PS_LV2_BIND(table, 961, sys_performance_monitor_hidden);
    PS_LV2_BIND(table, 962, sys_performance_monitor_bookmark);
    PS_LV2_BIND(table, 963, sys_lv1_pc_trace_create);
    PS_LV2_BIND(table, 964, sys_lv1_pc_trace_start);
    PS_LV2_BIND(table, 965, sys_lv1_pc_trace_stop);
    PS_LV2_BIND(table, 966, sys_lv1_pc_trace_get_status);
    PS_LV2_BIND(table, 967, sys_lv1_pc_trace_destroy);
    PS_LV2_BIND(table, 699, sys_bdemu_send_command);
    PS_LV2_BIND(table, 880, sys_deci3_open);
    PS_LV2_BIND(table, 881, sys_deci3_create_event_path);
    PS_LV2_BIND(table, 882, sys_deci3_close);
    PS_LV2_BIND(table, 883, sys_deci3_send);
    PS_LV2_BIND(table, 884, sys_deci3_receive);
    PS_LV2_BIND(table, 885, sys_deci3_open2);
    PS_LV2_BIND(table, 890, sys_deci3_initialize);
    PS_LV2_BIND(table, 891, sys_deci3_terminate);
    PS_LV2_BIND(table, 892, sys_deci3_debug_mode);
    PS_LV2_BIND(table, 893, sys_deci3_show_status);
    PS_LV2_BIND(table, 894, sys_deci3_echo_test);
    PS_LV2_BIND(table, 895, sys_deci3_send_dcmp_packet);
    PS_LV2_BIND(table, 896, sys_deci3_dump_cp_register);
    PS_LV2_BIND(table, 897, sys_deci3_dump_cp_buffer);
    PS_LV2_BIND(table, 899, sys_deci3_test);
    PS_LV2_BIND(table, 640, sys_usbbtaudio_initialize);
    PS_LV2_BIND(table, 641, sys_usbbtaudio_finalize);
    PS_LV2_BIND(table, 642, sys_usbbtaudio_discovery);
    PS_LV2_BIND(table, 643, sys_usbbtaudio_cancel_discovery);
    PS_LV2_BIND(table, 644, sys_usbbtaudio_pairing);
    PS_LV2_BIND(table, 645, sys_usbbtaudio_set_passkey);
    PS_LV2_BIND(table, 646, sys_usbbtaudio_connect);
    PS_LV2_BIND(table, 647, sys_usbbtaudio_disconnect);
    PS_LV2_BIND(table, 583, sys_bt_read_firmware_version);
    PS_LV2_BIND(table, 584, sys_bt_complete_wake_on_host);
    PS_LV2_BIND(table, 585, sys_bt_disable_bluetooth);
    PS_LV2_BIND(table, 586, sys_bt_enable_bluetooth);
    PS_LV2_BIND(table, 587, sys_bt_bccmd);
    PS_LV2_BIND(table, 588, sys_bt_read_hq);
    PS_LV2_BIND(table, 589, sys_bt_hid_get_remote_status);
    PS_LV2_BIND(table, 590, sys_bt_register_controller);
    PS_LV2_BIND(table, 591, sys_bt_clear_registered_contoller);
    PS_LV2_BIND(table, 592, sys_bt_connect_accept_controller);
    PS_LV2_BIND(table, 593, sys_bt_get_local_bdaddress);
    PS_LV2_BIND(table, 594, sys_bt_hid_get_data);
    PS_LV2_BIND(table, 595, sys_bt_hid_set_report);
    PS_LV2_BIND(table, 596, sys_bt_sched_log);
    PS_LV2_BIND(table, 597, sys_bt_cancel_connect_accept_controller);
    PS_LV2_BIND(table, 635, sys_btsetting_if);
    PS_LV2_BIND(table, 470, sys_npdrm_check_ekc);
    PS_LV2_BIND(table, 471, sys_npdrm_regist_ekc);
    PS_LV2_BIND(table, 860, sys_ss_get_cache_of_analog_sunset_flag);
    PS_LV2_BIND(table, 861, sys_ss_protected_file_db);
    PS_LV2_BIND(table, 862, sys_ss_virtual_trm_manager);
    PS_LV2_BIND(table, 869, sys_ss_factory_data_manager);
    PS_LV2_BIND(table, 579, sys_bluetooth_aud_serial_get_event_579);
    PS_LV2_BIND(table, 167, sys_spu_thread_group_start_on_exit);
    PS_LV2_BIND(table, 244, sys_spu_thread_group_system_set_next_group);
    PS_LV2_BIND(table, 245, sys_spu_thread_group_system_unset_next_group);
    PS_LV2_BIND(table, 246, sys_spu_thread_group_system_set_switch_group);
    PS_LV2_BIND(table, 247, sys_spu_thread_group_system_unset_switch_group);
    PS_LV2_BIND(table, 333, sys_mmapper_set_shared_memory_flag);
    PS_LV2_BIND(table, 336, sys_mmapper_change_address_access_right);
    PS_LV2_BIND(table, 338, sys_mmapper_get_shared_memory_attribute);
    PS_LV2_BIND(table, 344, sys_memory_budget_set);
    PS_LV2_BIND(table, 864, sys_ss_sec_hw_framework);
    PS_LV2_BIND(table, 876, sys_ss_disc_access_control);
    PS_LV2_BIND(table, 878, sys_ss_ad_sign);
    PS_LV2_BIND(table, 879, sys_ss_media_id);
    PS_LV2_BIND(table, 633, sys_fsw_connect_event);
    PS_LV2_BIND(table, 634, sys_fsw_disconnect_event);
    PS_LV2_BIND(table, 621, sys_gamepad_ycon_if);
    PS_LV2_BIND(table, 570, sys_pad_ldd_register_controller);
    PS_LV2_BIND(table, 571, sys_pad_ldd_unregister_controller);
    PS_LV2_BIND(table, 572, sys_pad_ldd_data_insert);
    PS_LV2_BIND(table, 573, sys_pad_dbg_ldd_set_data_insert_mode);
    PS_LV2_BIND(table, 574, sys_pad_ldd_register_controller);
    PS_LV2_BIND(table, 575, sys_pad_ldd_get_port_no);
    PS_LV2_BIND(table, 839, sys_fs_sync);
    PS_LV2_BIND(table, 843, sys_fs_get_fs_info_size);
    PS_LV2_BIND(table, 844, sys_fs_get_fs_info);
}

} // namespace pspocket::lv2
