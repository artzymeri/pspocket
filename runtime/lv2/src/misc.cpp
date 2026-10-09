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
// - The update manager (sys_ss 863) and the AV manager's UART (367-370) are root's (RPCS3 checks it too). For root:
//   RPCS3's update manager (the installed version, the EEPROM cells it presets, buffers from user memory, version
//   3.40 as the applicable one); the UART with no AV manager behind it: what is sent is taken, nothing comes back
//   (pspocket's video and audio output settings are its own cellVideoOut and cellAudioOut).

#include <map>
#include <set>
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

// ------------------------------------------------------------------------------------------------- root's managers

namespace {

struct RootManagers {
    std::mutex lock;
    std::map<u32, u8> eeprom{{0x48C18, 0x00}, {0x48C19, 0x00}, {0x48C1A, 0x00}, {0x48C1B, 0x01}, {0x48C1C, 0x00},
        {0x48C1D, 0x00}, {0x48C1E, 0x00}, {0x48C1F, 0x00}}; // RPCS3's: the system language, the VSH target
    std::map<u32, u64> buffers;                              // update-manager buffers: address -> bytes
    bool uart_initialized = false;
};

RootManagers& root_managers(Kernel& k)
{
    Kernel::Impl& s = k.impl();
    std::lock_guard lock(s.system_mutex);
    if (!s.root_managers_state) s.root_managers_state = std::make_shared<RootManagers>();
    return *static_cast<RootManagers*>(s.root_managers_state.get());
}

// The installed system software as RPCS3's update manager reports it: 4.92 is 0x492 << 40.
u64 update_manager_version(PpuThread& t)
{
    const u64 v = t.kernel.config().system_sw_version / 100; // 49200 -> 492
    const u64 bcd = ((v / 100) << 8) | (((v / 10) % 10) << 4) | (v % 10);
    return bcd << 40;
}

} // namespace

// sys_ss_update_manager(packet, a1..a6): root.
Result sys_ss_update_manager(PpuThread& t, u64 packet, u64 a1, u64 a2, u64 a3, u64, u64, u64)
{
    if (!is_root(t)) return system_only(t, "sys_ss_update_manager");
    Kernel::Impl& s = t.kernel.impl();
    RootManagers& m = root_managers(t.kernel);
    std::lock_guard lock(m.lock);
    const auto allocate = [&](u64 size, u32 out) -> Result {
        if (!out) return err(CELL_EFAULT);
        const u64 bytes = (size + 0xFFFF) & ~u64{0xFFFF};
        u32 addr = 0;
        {
            std::lock_guard ml(s.memory.mutex);
            addr = bytes && bytes <= 0x10000000 ? s.memory.allocate_kernel(bytes) : 0;
        }
        if (!addr) return err(CELL_ENOMEM);
        if (!s.guest.put32(out, addr)) {
            std::lock_guard ml(s.memory.mutex);
            s.memory.free_kernel(addr);
            return err(CELL_EFAULT);
        }
        m.buffers[addr] = bytes;
        return CELL_OK;
    };
    switch (packet) {
    case 0x6001: case 0x6002: case 0x6004: case 0x6005: case 0x6006: case 0x6007: case 0x6008: case 0x6009:
    case 0x600A: case 0x600D: case 0x6010: case 0x6013:
        return CELL_OK; // asynchronous operations and flags with nothing to do (RPCS3)
    case 0x6003: // the installed package's version
        if (!a2) return err(CELL_EFAULT);
        if (!s.guest.put64(static_cast<u32>(a2), update_manager_version(t))) return err(CELL_EFAULT);
        return CELL_OK;
    case 0x600B: { // read an EEPROM cell (0xFF when unset)
        if (!a2) return err(CELL_EFAULT);
        const auto it = m.eeprom.find(static_cast<u32>(a1));
        if (!s.guest.valid(static_cast<u32>(a2), 1)) return err(CELL_EFAULT);
        s.guest.w8(static_cast<u32>(a2), it == m.eeprom.end() ? 0xFF : it->second);
        return CELL_OK;
    }
    case 0x600C: // write an EEPROM cell (0xFF unsets it)
        if (static_cast<u8>(a2) != 0xFF) m.eeprom[static_cast<u32>(a1)] = static_cast<u8>(a2);
        else m.eeprom.erase(static_cast<u32>(a1));
        return CELL_OK;
    case 0x600E: return allocate(static_cast<u32>(a1), static_cast<u32>(a2));
    case 0x600F: { // release a buffer
        const auto it = m.buffers.find(static_cast<u32>(a1));
        if (it == m.buffers.end()) return err(CELL_ENOMEM);
        {
            std::lock_guard ml(s.memory.mutex);
            s.memory.free_kernel(it->first);
        }
        m.buffers.erase(it);
        return CELL_OK;
    }
    case 0x6011: // the applicable version: 3.40
        if (!a2) return err(CELL_EFAULT);
        if (!s.guest.put64(static_cast<u32>(a2), u64{0x30040} << 32)) return err(CELL_EFAULT);
        return CELL_OK;
    case 0x6012: return allocate(static_cast<u32>(a2), static_cast<u32>(a3)); // from a memory container
    default: PS_LOG_ERROR(kMiscChan, "sys_ss_update_manager: unknown packet {:#x}", packet); return err(CELL_EINVAL);
    }
}

// The AV manager's UART: root (see the top).
constexpr u32 NOT_BLOCKING_BIG_OP = 0, BLOCKING_BIG_OP = 1, NOT_BLOCKING_OP = 2;

Result sys_uart_initialize(PpuThread& t)
{
    if (!is_root(t)) return system_only(t, "sys_uart_initialize");
    RootManagers& m = root_managers(t.kernel);
    std::lock_guard lock(m.lock);
    if (m.uart_initialized) return err(CELL_EPERM);
    m.uart_initialized = true;
    return CELL_OK;
}

Result sys_uart_receive(PpuThread& t, u32, u64 size, u32 mode)
{
    if (!is_root(t)) return system_only(t, "sys_uart_receive");
    if (!size) return CELL_OK;
    if (mode & ~(BLOCKING_BIG_OP | NOT_BLOCKING_BIG_OP)) return err(CELL_EINVAL);
    RootManagers& m = root_managers(t.kernel);
    std::lock_guard lock(m.lock);
    if (!m.uart_initialized) return err(CELL_ESRCH);
    if (size > 0x20000) return err(CELL_EINVAL); // RPCS3 aborts ("buffer is too big")
    if (mode & BLOCKING_BIG_OP) PS_LOG_WARN(kMiscChan, "sys_uart_receive: no AV manager answers; nothing read");
    return 0;
}

Result sys_uart_send(PpuThread& t, u32 buffer, u64 size, u32 mode)
{
    if (!is_root(t)) return system_only(t, "sys_uart_send");
    if (!size) return CELL_OK;
    if (mode & ~(BLOCKING_BIG_OP | NOT_BLOCKING_OP | NOT_BLOCKING_BIG_OP)) return err(CELL_EINVAL);
    RootManagers& m = root_managers(t.kernel);
    std::lock_guard lock(m.lock);
    if (!m.uart_initialized) return err(CELL_ESRCH);
    if (size > 0x20000) return err(CELL_EINVAL);
    if (!t.kernel.impl().guest.valid(buffer, size)) return err(CELL_EFAULT);
    if ((mode & NOT_BLOCKING_OP) && size > 0x800) return err(CELL_EAGAIN); // more than the transmit buffer holds
    return static_cast<Result>(size);
}

// sys_uart_get_params(vuart_params* buffer): the receive and transmit buffers' sizes (0x800 each).
Result sys_uart_get_params(PpuThread& t, u32 buffer)
{
    if (!is_root(t)) return system_only(t, "sys_uart_get_params");
    RootManagers& m = root_managers(t.kernel);
    std::lock_guard lock(m.lock);
    if (!m.uart_initialized) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    if (!g.valid(buffer, 16)) return err(CELL_EFAULT);
    g.w64(buffer, 0x800);
    g.w64(buffer + 8, 0x800);
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
    PS_LV2_BIND(table, 863, sys_ss_update_manager);
    PS_LV2_BIND(table, 367, sys_uart_initialize);
    PS_LV2_BIND(table, 368, sys_uart_receive);
    PS_LV2_BIND(table, 369, sys_uart_send);
    PS_LV2_BIND(table, 370, sys_uart_get_params);
}

} // namespace pspocket::lv2
