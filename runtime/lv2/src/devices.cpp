// lv2's small device and system-manager calls, as a retail console running a game (D-527): the GPIO (LEDs and DIP
// switches), I/O buffers, the system manager (sys_sm) and the debugger's process-memory access. References: RPCS3
// rpcs3/Emu/Cell/lv2/sys_gpio.cpp, sys_io.cpp, sys_sm.cpp, sys_dbg.cpp, and its syscall table's permission column
// (lv2.cpp: ROOT, DBG, PM).
//
// Permissions: the calls RPCS3's table marks ROOT, DBG (debug) or PM (product mode) are for system processes. A game
// is none of these (Config::root false), so lv2 answers them CELL_ENOSYS, as it answers sys_ss's root-only packages
// (system.cpp) and RPCS3's sys_sm_shutdown answers a game. A root process gets RPCS3's answers: for the system
// manager services it leaves as null_func (LED requests, fan policy, temperatures, the RTC alarm, logs, the hardware
// configuration), CELL_OK with nothing written, as a console whose system manager has nothing to report.

#include <map>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kDevChan = "lv2";

// sys_gpio device IDs (RPCS3 sys_gpio.h).
constexpr u64 SYS_GPIO_LED_DEVICE_ID = 1, SYS_GPIO_DIP_SWITCH_DEVICE_ID = 2;

// An I/O buffer (RPCS3 lv2_io_buf): IDs from 0x44000000, one apart; the blocks lv2 gave out for it.
struct IoBuffer {
    u32 block_count = 0, block_size = 0, blocks = 0, unk1 = 0;
    std::map<u32, u64> allocations; // address -> bytes
};

struct Devices {
    std::mutex lock;
    std::map<u32, IoBuffer> io_buffers;
    u32 next_io_buffer = 0x44000000;
};

Devices& devices(Kernel& k)
{
    Kernel::Impl& s = k.impl();
    std::lock_guard lock(s.system_mutex);
    if (!s.devices_state) s.devices_state = std::make_shared<Devices>();
    return *static_cast<Devices*>(s.devices_state.get());
}

bool is_root(PpuThread& t) { return t.kernel.config().root; }

// A call for system processes made by a game: CELL_ENOSYS, logged once per call.
Result not_for_games(std::string_view name)
{
    static std::mutex m;
    static std::map<std::string_view, bool> logged;
    std::lock_guard lock(m);
    if (!logged[name]) {
        logged[name] = true;
        PS_LOG_WARN(kDevChan, "{}: a system process's call (root, debug or product mode), not a game's: CELL_ENOSYS",
            name);
    }
    return err(CELL_ENOSYS);
}

// A system-manager service for root processes that has nothing to report on this console (RPCS3 null_func).
Result sm_service(PpuThread& t, std::string_view name)
{
    if (!is_root(t)) return not_for_games(name);
    static std::mutex m;
    static std::map<std::string_view, bool> logged;
    std::lock_guard lock(m);
    if (!logged[name]) {
        logged[name] = true;
        PS_LOG_INFO(kDevChan, "{}: no system-manager service to report, CELL_OK", name);
    }
    return CELL_OK;
}

void free_io_blocks(Kernel& k, IoBuffer& io)
{
    Kernel::Impl& s = k.impl();
    std::lock_guard ml(s.memory.mutex);
    for (const auto& [addr, bytes] : io.allocations) {
        (void)bytes;
        s.memory.free_kernel(addr);
    }
    io.allocations.clear();
}

} // namespace

// ------------------------------------------------------------------------------------------------- sys_gpio

// sys_gpio_get(device_id, u64* value): a retail console has no LEDs or DIP switches, so both read 0.
Result sys_gpio_get(PpuThread& t, u64 device_id, u32 value)
{
    if (device_id != SYS_GPIO_LED_DEVICE_ID && device_id != SYS_GPIO_DIP_SWITCH_DEVICE_ID) return err(CELL_ESRCH);
    if (!t.kernel.impl().guest.put64(value, 0)) return err(CELL_EFAULT);
    return CELL_OK;
}

// sys_gpio_set(device_id, mask, value): the LEDs take it (nothing to light), the DIP switches cannot be set.
Result sys_gpio_set(PpuThread&, u64 device_id, u64, u64)
{
    switch (device_id) {
    case SYS_GPIO_LED_DEVICE_ID: return CELL_OK;
    case SYS_GPIO_DIP_SWITCH_DEVICE_ID: return err(CELL_EINVAL);
    default: return err(CELL_ESRCH);
    }
}

// ------------------------------------------------------------------------------------------------- sys_io

// sys_io_buffer_create(block_count, block_size, blocks, unk1, u32* handle).
Result sys_io_buffer_create(PpuThread& t, u32 block_count, u32 block_size, u32 blocks, u32 unk1, u32 handle)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!handle) return err(CELL_EFAULT);
    if (!s.guest.valid(handle, 4)) return err(CELL_EFAULT);
    Devices& d = devices(t.kernel);
    std::lock_guard lock(d.lock);
    if (d.io_buffers.size() >= 2048) return err(CELL_ESRCH); // RPCS3's ID count: idm::make fails
    u32 id = d.next_io_buffer;
    while (d.io_buffers.contains(id)) id = id + 1 < 0x44000000 + 2048 ? id + 1 : 0x44000000;
    d.next_io_buffer = id + 1 < 0x44000000 + 2048 ? id + 1 : 0x44000000;
    d.io_buffers[id] = IoBuffer{block_count, block_size, blocks, unk1, {}};
    s.guest.w32(handle, id);
    return CELL_OK;
}

// sys_io_buffer_destroy(handle): the buffer goes, with the blocks it gave out; an unknown one is CELL_OK too (RPCS3).
Result sys_io_buffer_destroy(PpuThread& t, u32 handle)
{
    Devices& d = devices(t.kernel);
    std::lock_guard lock(d.lock);
    const auto it = d.io_buffers.find(handle);
    if (it == d.io_buffers.end()) return CELL_OK;
    free_io_blocks(t.kernel, it->second);
    d.io_buffers.erase(it);
    return CELL_OK;
}

// sys_io_buffer_allocate(handle, u32* block): block_count * block_size bytes of user memory (RPCS3 vm::main), kept
// by lv2 in 64 KB pages.
Result sys_io_buffer_allocate(PpuThread& t, u32 handle, u32 block)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!block) return err(CELL_EFAULT);
    Devices& d = devices(t.kernel);
    std::lock_guard lock(d.lock);
    const auto it = d.io_buffers.find(handle);
    if (it == d.io_buffers.end()) return err(CELL_ESRCH);
    if (!s.guest.valid(block, 4)) return err(CELL_EFAULT);
    const u64 size = u64{it->second.block_count} * it->second.block_size;
    if (!size) return err(CELL_ENOMEM); // vm::alloc(0) fails
    const u64 bytes = (size + 0xFFFF) & ~u64{0xFFFF};
    u32 addr = 0;
    {
        std::lock_guard ml(s.memory.mutex);
        addr = bytes <= 0x10000000 ? s.memory.allocate_kernel(bytes) : 0;
    }
    if (!addr) return err(CELL_ENOMEM);
    it->second.allocations[addr] = bytes;
    s.guest.w32(block, addr);
    return CELL_OK;
}

// sys_io_buffer_free(handle, block): a block the buffer gave out goes back; another address is left alone (RPCS3's
// vm::dealloc fails quietly), CELL_OK either way.
Result sys_io_buffer_free(PpuThread& t, u32 handle, u32 block)
{
    Kernel::Impl& s = t.kernel.impl();
    Devices& d = devices(t.kernel);
    std::lock_guard lock(d.lock);
    const auto it = d.io_buffers.find(handle);
    if (it == d.io_buffers.end()) return err(CELL_ESRCH);
    const auto a = it->second.allocations.find(block);
    if (a == it->second.allocations.end()) {
        PS_LOG_WARN(kDevChan, "sys_io_buffer_free({:#x}, {:#x}): not one of its blocks, nothing freed", handle, block);
        return CELL_OK;
    }
    {
        std::lock_guard ml(s.memory.mutex);
        s.memory.free_kernel(a->first);
    }
    it->second.allocations.erase(a);
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- sys_sm

// sys_sm_set_shop_mode(mode): root.
Result sys_sm_set_shop_mode(PpuThread& t, s32) { return sm_service(t, "sys_sm_set_shop_mode"); }

// sys_sm_get_ext_event2(u64* a1, u64* a2, u64* a3, u64 a4): root; no event (CELL_EAGAIN), the three outputs 0.
Result sys_sm_get_ext_event2(PpuThread& t, u32 a1, u32 a2, u32 a3, u64 a4)
{
    if (!is_root(t)) return not_for_games("sys_sm_get_ext_event2");
    Kernel::Impl& s = t.kernel.impl();
    if (a4 != 0 && a4 != 1) return err(CELL_EINVAL);
    for (const u32 p : {a1, a2, a3}) {
        if (!s.guest.put64(p, 0)) return err(CELL_EFAULT);
    }
    return err(CELL_EAGAIN);
}

// sys_sm_shutdown(op, param, size): root. A shutdown ends the process; a reboot too (pspocket cannot start the
// console's system software again; RPCS3 restarts the game); the LPAR operations are not supported.
Result sys_sm_shutdown(PpuThread& t, u32 op16, u32, u64)
{
    const u32 op = op16 & 0xFFFF;
    if (!is_root(t)) return not_for_games("sys_sm_shutdown");
    switch (op) {
    case 0x100:
    case 0x1100:
        PS_LOG_INFO(kDevChan, "sys_sm_shutdown({:#x}): shutdown, the process ends", op);
        exit_process(t, 0);
    case 0x200:
    case 0x1200:
        PS_LOG_WARN(kDevChan, "sys_sm_shutdown({:#x}): reboot, the process ends", op);
        exit_process(t, 0);
    case 0x8201:
    case 0x8202:
    case 0x8204: PS_LOG_WARN(kDevChan, "sys_sm_shutdown: LPAR operation {:#x} not supported", op); return err(CELL_ENOTSUP);
    default: return err(CELL_EINVAL);
    }
}

// sys_sm_get_params(u8* a, u8* b, u32* c, u64* d): debug processes; RPCS3's values 0, 0, 0x200, 7.
Result sys_sm_get_params(PpuThread& t, u32 a, u32 b, u32 c, u32 d)
{
    if (!is_root(t)) return not_for_games("sys_sm_get_params");
    const Guest& g = t.kernel.impl().guest;
    if (!g.valid(a, 1)) return err(CELL_EFAULT);
    g.w8(a, 0);
    if (!g.valid(b, 1)) return err(CELL_EFAULT);
    g.w8(b, 0);
    if (!g.put32(c, 0x200)) return err(CELL_EFAULT);
    if (!g.put64(d, 7)) return err(CELL_EFAULT);
    return CELL_OK;
}

Result sys_sm_get_inter_lpar_parameter(PpuThread& t) { return sm_service(t, "sys_sm_get_inter_lpar_parameter"); }
Result sys_sm_initialize(PpuThread& t) { return sm_service(t, "sys_sm_initialize"); }
Result sys_game_get_temperature(PpuThread& t) { return sm_service(t, "sys_game_get_temperature"); }
Result sys_sm_get_tzpb(PpuThread& t) { return sm_service(t, "sys_sm_get_tzpb"); }
Result sys_sm_request_led(PpuThread& t) { return sm_service(t, "sys_sm_request_led"); }
Result sys_sm_control_led(PpuThread& t, u32, u32) { return sm_service(t, "sys_sm_control_led"); }
Result sys_sm_get_system_info(PpuThread& t) { return sm_service(t, "sys_sm_get_system_info"); }
Result sys_sm_ring_buzzer2(PpuThread& t) { return sm_service(t, "sys_sm_ring_buzzer2"); }
Result sys_sm_set_fan_policy(PpuThread& t) { return sm_service(t, "sys_sm_set_fan_policy"); }
Result sys_sm_request_error_log(PpuThread& t) { return sm_service(t, "sys_sm_request_error_log"); }
Result sys_sm_request_be_count(PpuThread& t) { return sm_service(t, "sys_sm_request_be_count"); }
Result sys_sm_ring_buzzer(PpuThread& t, u64, u64, u64) { return sm_service(t, "sys_sm_ring_buzzer"); }
Result sys_sm_get_hw_config(PpuThread& t) { return sm_service(t, "sys_sm_get_hw_config"); }
Result sys_sm_request_scversion(PpuThread& t) { return sm_service(t, "sys_sm_request_scversion"); }
Result sys_sm_request_system_event_log(PpuThread& t) { return sm_service(t, "sys_sm_request_system_event_log"); }
Result sys_sm_set_rtc_alarm(PpuThread& t) { return sm_service(t, "sys_sm_set_rtc_alarm"); }
Result sys_sm_get_rtc_alarm(PpuThread& t) { return sm_service(t, "sys_sm_get_rtc_alarm"); }
Result sys_sm_get_fan_policy(PpuThread& t) { return sm_service(t, "sys_sm_get_fan_policy"); }

// ------------------------------------------------------------------------------------------------- sys_dbg

// sys_dbg_read_process_memory and sys_dbg_write_process_memory are not in a retail console's lv2 (its table points
// them at the not-implemented slot, D-938): CELL_ENOSYS.
Result sys_dbg_read_process_memory(PpuThread&) { return retail_absent("sys_dbg_read_process_memory"); }
Result sys_dbg_write_process_memory(PpuThread&) { return retail_absent("sys_dbg_write_process_memory"); }

void register_device_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 630, sys_gpio_set);
    PS_LV2_BIND(table, 631, sys_gpio_get);
    PS_LV2_BIND(table, 624, sys_io_buffer_create);
    PS_LV2_BIND(table, 625, sys_io_buffer_destroy);
    PS_LV2_BIND(table, 626, sys_io_buffer_allocate);
    PS_LV2_BIND(table, 627, sys_io_buffer_free);
    PS_LV2_BIND(table, 377, sys_sm_set_shop_mode);
    PS_LV2_BIND(table, 378, sys_sm_get_ext_event2);
    PS_LV2_BIND(table, 379, sys_sm_shutdown);
    PS_LV2_BIND(table, 380, sys_sm_get_params);
    PS_LV2_BIND(table, 381, sys_sm_get_inter_lpar_parameter);
    PS_LV2_BIND(table, 382, sys_sm_initialize);
    PS_LV2_BIND(table, 383, sys_game_get_temperature);
    PS_LV2_BIND(table, 384, sys_sm_get_tzpb);
    PS_LV2_BIND(table, 385, sys_sm_request_led);
    PS_LV2_BIND(table, 386, sys_sm_control_led);
    PS_LV2_BIND(table, 387, sys_sm_get_system_info);
    PS_LV2_BIND(table, 388, sys_sm_ring_buzzer2);
    PS_LV2_BIND(table, 389, sys_sm_set_fan_policy);
    PS_LV2_BIND(table, 390, sys_sm_request_error_log);
    PS_LV2_BIND(table, 391, sys_sm_request_be_count);
    PS_LV2_BIND(table, 392, sys_sm_ring_buzzer);
    PS_LV2_BIND(table, 393, sys_sm_get_hw_config);
    PS_LV2_BIND(table, 394, sys_sm_request_scversion);
    PS_LV2_BIND(table, 395, sys_sm_request_system_event_log);
    PS_LV2_BIND(table, 396, sys_sm_set_rtc_alarm);
    PS_LV2_BIND(table, 397, sys_sm_get_rtc_alarm);
    PS_LV2_BIND(table, 408, sys_sm_get_tzpb);
    PS_LV2_BIND(table, 409, sys_sm_get_fan_policy);
    PS_LV2_BIND(table, 904, sys_dbg_read_process_memory);
    PS_LV2_BIND(table, 905, sys_dbg_write_process_memory);
}

} // namespace pspocket::lv2
