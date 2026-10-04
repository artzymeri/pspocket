// sys_io: controllers (cellPad) and keyboards (cellKb). Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellPad.cpp/.h,
// cellKb.cpp/.h and Emu/Io/pad_types.h, Keyboard.h (structure layouts and constants).
//
// Devices: one virtual controller on port 0, always connected, reported as RPCS3 reports a DualShock 3 (standard
// device type; PS3 conformity, press mode, sensor mode, high-precision sticks, actuator). The host feeds it with
// set_pad_input (pspocket/hle/input.hpp); at rest it reports no buttons and centred sticks. No keyboard is connected,
// as with RPCS3's default null keyboard handler. RPCS3's sys_config machinery (the event thread that delivers connect
// events) is not reproduced: the controller is reported connected, with the assignment-changed flag, from cellPadInit.
//
// cellPadGetData reports changes only, as RPCS3 does: the first read after init, and any read where the controller's
// state differs from the last one reported, returns data; otherwise CELL_PAD_LEN_NO_CHANGE. With sensors on, data comes
// back at least every 10 ms. Rumble (cellPadSetActDirect) is accepted and ignored for now.

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/input.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kPadChan = "cellPad";
constexpr std::string_view kKbChan = "cellKb";

enum : u32 {
    CELL_PAD_ERROR_INVALID_PARAMETER = 0x80121102,
    CELL_PAD_ERROR_ALREADY_INITIALIZED = 0x80121103,
    CELL_PAD_ERROR_UNINITIALIZED = 0x80121104,
    CELL_PAD_ERROR_NO_DEVICE = 0x80121107,
    CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD = 0x80121108,

    CELL_KB_ERROR_FATAL = 0x80121001,
    CELL_KB_ERROR_INVALID_PARAMETER = 0x80121002,
    CELL_KB_ERROR_ALREADY_INITIALIZED = 0x80121003,
    CELL_KB_ERROR_UNINITIALIZED = 0x80121004,
    CELL_KB_ERROR_NO_DEVICE = 0x80121007,
    CELL_KB_ERROR_SYS_SETTING_FAILED = 0x80121008,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 CELL_MAX_PADS = 127, CELL_PAD_MAX_PORT_NUM = 7, CELL_PAD_MAX_CODES = 64;
constexpr u32 CELL_PAD_STATUS_CONNECTED = 1, CELL_PAD_STATUS_ASSIGN_CHANGES = 2;
constexpr u32 CELL_PAD_SETTING_PRESS_ON = 2, CELL_PAD_SETTING_SENSOR_ON = 4;
constexpr u32 CELL_PAD_CAPABILITY_PS3_CONFORMITY = 1, CELL_PAD_CAPABILITY_PRESS_MODE = 2,
              CELL_PAD_CAPABILITY_SENSOR_MODE = 4, CELL_PAD_CAPABILITY_HP_ANALOG_STICK = 8,
              CELL_PAD_CAPABILITY_ACTUATOR = 16;
constexpr u32 CELL_PAD_DEV_TYPE_STANDARD = 0;
constexpr s32 CELL_PAD_LEN_NO_CHANGE = 0, CELL_PAD_LEN_CHANGE_DEFAULT = 8, CELL_PAD_LEN_CHANGE_PRESS_ON = 20,
              CELL_PAD_LEN_CHANGE_SENSOR_ON = 24;
constexpr u32 kVirtualPadCapability = CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE |
                                      CELL_PAD_CAPABILITY_SENSOR_MODE | CELL_PAD_CAPABILITY_HP_ANALOG_STICK |
                                      CELL_PAD_CAPABILITY_ACTUATOR;

// CellPadInfo2: max_connect, now_connect, system_info, then port_status[7], port_setting[7], device_capability[7],
// device_type[7] (u32 each).
namespace info2_off {
constexpr u32 max_connect = 0x0, now_connect = 0x4, system_info = 0x8, port_status = 0xC, port_setting = 0x28,
              device_capability = 0x44, device_type = 0x60, size = 0x7C;
}
// CellPadData: s32 len, u16 button[64].
constexpr u32 kPadDataButtons = 4;

constexpr u32 CELL_KB_MAX_KEYBOARDS = 127, CELL_KB_RMODE_PACKET = 1, CELL_KB_CODETYPE_ASCII = 1;
// CellKbInfo: max_connect, now_connect, info (u32 each), u8 status[127].
constexpr u32 kKbInfoSize = 12 + CELL_KB_MAX_KEYBOARDS;

struct IoState {
    std::mutex mutex;
    // cellPad
    u32 pad_max_connect = 0; // 0: not initialized
    std::array<u32, CELL_PAD_MAX_PORT_NUM> port_setting{};
    std::array<u32, CELL_PAD_MAX_PORT_NUM> reported_status{};
    PadInput input;                                  // what the host set
    std::array<u16, CELL_PAD_MAX_CODES> output{};    // the last state reported (RPCS3 pad->data)
    bool buffer_cleared = true;                      // report everything at the next read
    std::chrono::steady_clock::time_point last_sensor_report{};
    bool rumble_logged = false;
    // cellKb
    bool kb_initialized = false;
    u32 kb_max_connect = 0;
};

IoState& io() { return HleProcess::current()->state<IoState>(); }

u32 pad_ports(const IoState& s) { return std::min(s.pad_max_connect, CELL_PAD_MAX_PORT_NUM); }

bool pad_connected(const IoState& s, u32 port)
{
    return port < pad_ports(s) && (s.reported_status[port] & CELL_PAD_STATUS_CONNECTED);
}

// The virtual controller's state in CellPadData button[] order.
std::array<u16, CELL_PAD_MAX_CODES> pad_buttons(const PadInput& in)
{
    std::array<u16, CELL_PAD_MAX_CODES> b{};
    b[2] = in.digital1;
    b[3] = in.digital2;
    b[4] = in.right_x;
    b[5] = in.right_y;
    b[6] = in.left_x;
    b[7] = in.left_y;
    for (u32 i = 0; i < 12; i++) b[8 + i] = in.pressure[i];
    for (u32 i = 0; i < 4; i++) b[20 + i] = in.sensor[i];
    return b;
}

// ------------------------------------------------------------------------------------------------- cellPad

s32 cellPadInit(PPUContext&, u32 max_connect)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (s.pad_max_connect) return error(CELL_PAD_ERROR_ALREADY_INITIALIZED);
    if (max_connect == 0 || max_connect > CELL_MAX_PADS) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    s.pad_max_connect = max_connect;
    s.port_setting.fill(0); // CELL_PAD_SETTING_PRESS_OFF | CELL_PAD_SETTING_SENSOR_OFF
    s.reported_status.fill(0);
    s.reported_status[0] = CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES; // the virtual controller
    s.buffer_cleared = true;
    PS_LOG_INFO(kPadChan, "cellPadInit({}): one virtual controller on port 0", max_connect);
    return lv2::CELL_OK;
}

s32 cellPadEnd(PPUContext&)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    s.pad_max_connect = 0;
    return lv2::CELL_OK;
}

s32 cellPadGetInfo2(PPUContext& ctx, u32 info)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (!info) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    u8* base = guest_base(ctx);
    std::memset(base + info, 0, info2_off::size);
    ps_write32(base, info + info2_off::max_connect, pad_ports(s));
    ps_write32(base, info + info2_off::system_info, 0);
    u32 now_connect = 0;
    for (u32 i = 0; i < pad_ports(s); i++) {
        ps_write32(base, info + info2_off::port_status + 4 * i, s.reported_status[i]);
        ps_write32(base, info + info2_off::port_setting + 4 * i, s.port_setting[i]);
        s.reported_status[i] &= ~CELL_PAD_STATUS_ASSIGN_CHANGES;
        if (!(s.reported_status[i] & CELL_PAD_STATUS_CONNECTED)) continue;
        ps_write32(base, info + info2_off::device_capability + 4 * i, kVirtualPadCapability);
        ps_write32(base, info + info2_off::device_type + 4 * i, CELL_PAD_DEV_TYPE_STANDARD);
        now_connect++;
    }
    ps_write32(base, info + info2_off::now_connect, now_connect);
    return lv2::CELL_OK;
}

// RPCS3 cellPadGetData and pad_get_data (the standard-controller path).
s32 cellPadGetData(PPUContext& ctx, u32 port, u32 data)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_PADS || !data) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (!pad_connected(s, port)) return error(CELL_PAD_ERROR_NO_DEVICE);

    u8* base = guest_base(ctx);
    const u32 setting = s.port_setting[port];
    const auto now = pad_buttons(s.input);
    bool changed = false;
    for (u32 i = 2; i < 20; i++) changed |= now[i] != s.output[i];
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        for (u32 i = 20; i < 24; i++) changed |= now[i] != s.output[i];
    }
    s.output = now;

    s32 len;
    const auto t = std::chrono::steady_clock::now();
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        // A DualShock 3 sends sensor data every ~10 ms even when nothing changes (RPCS3).
        if (changed || s.buffer_cleared || t - s.last_sensor_report >= std::chrono::milliseconds(10)) {
            len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
            s.last_sensor_report = t;
        } else {
            len = CELL_PAD_LEN_NO_CHANGE;
        }
    } else if (changed || s.buffer_cleared) {
        len = (setting & CELL_PAD_SETTING_PRESS_ON) ? CELL_PAD_LEN_CHANGE_PRESS_ON : CELL_PAD_LEN_CHANGE_DEFAULT;
    } else {
        len = CELL_PAD_LEN_NO_CHANGE;
    }
    s.buffer_cleared = false;

    ps_write32(base, data, static_cast<u32>(len));
    if (len > CELL_PAD_LEN_NO_CHANGE) {
        const u32 b = data + kPadDataButtons;
        ps_write16(base, b + 0, 0);                                              // always 0
        ps_write16(base, b + 2, static_cast<u16>((0x7 << 4) | std::min(len / 2, 15))); // 0x7, then len / 2
        const u32 copied = (setting & CELL_PAD_SETTING_PRESS_ON) ? CELL_PAD_LEN_CHANGE_PRESS_ON
                                                                  : CELL_PAD_LEN_CHANGE_DEFAULT;
        for (u32 i = 2; i < CELL_PAD_LEN_CHANGE_PRESS_ON; i++) ps_write16(base, b + 2 * i, i < copied ? now[i] : 0);
        if (len == CELL_PAD_LEN_CHANGE_SENSOR_ON) {
            for (u32 i = 20; i < 24; i++) ps_write16(base, b + 2 * i, now[i]);
        }
    }
    return lv2::CELL_OK;
}

s32 cellPadSetPortSetting(PPUContext&, u32 port, u32 setting)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_PADS) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (port >= CELL_PAD_MAX_PORT_NUM) return lv2::CELL_OK; // RPCS3: no NO_DEVICE here
    s.port_setting[port] = setting & (CELL_PAD_SETTING_PRESS_ON | CELL_PAD_SETTING_SENSOR_ON);
    PS_LOG_DEBUG(kPadChan, "cellPadSetPortSetting(port {}, {:#x})", port, setting);
    return lv2::CELL_OK;
}

// CellPadActParam: u8 motor[2], u8 reserved[6].
s32 cellPadSetActDirect(PPUContext& ctx, u32 port, u32 param)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_PADS || !param) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    u8* base = guest_base(ctx);
    // RPCS3: a signed check of the process SDK version against 0x1FFFFF.
    if (static_cast<s32>(thread_of(ctx).kernel.config().sdk_version) > 0x1FFFFF) {
        for (u32 i = 0; i < 6; i++) {
            if (ps_read8(base, param + 2 + i)) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
        }
    }
    if (port >= pad_ports(s) || !pad_connected(s, port)) return error(CELL_PAD_ERROR_NO_DEVICE);
    if (!(kVirtualPadCapability & CELL_PAD_CAPABILITY_ACTUATOR)) return error(CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD);
    if (!s.rumble_logged) {
        PS_LOG_INFO(kPadChan, "cellPadSetActDirect(port {}, motors {} {}): rumble is not implemented; ignored", port,
            ps_read8(base, param), ps_read8(base, param + 1));
        s.rumble_logged = true;
    }
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- cellKb (none)

s32 cellKbInit(PPUContext&, u32 max_connect)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (s.kb_initialized) return error(CELL_KB_ERROR_ALREADY_INITIALIZED);
    if (max_connect == 0 || max_connect > CELL_KB_MAX_KEYBOARDS) return error(CELL_KB_ERROR_INVALID_PARAMETER);
    s.kb_initialized = true;
    s.kb_max_connect = std::min(max_connect, 7u);
    PS_LOG_INFO(kKbChan, "cellKbInit({}): no keyboard is connected", max_connect);
    return lv2::CELL_OK;
}

s32 cellKbEnd(PPUContext&)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.kb_initialized) return error(CELL_KB_ERROR_UNINITIALIZED);
    s.kb_initialized = false;
    return lv2::CELL_OK;
}

s32 cellKbGetInfo(PPUContext& ctx, u32 info)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.kb_initialized) return error(CELL_KB_ERROR_UNINITIALIZED);
    if (!info) return error(CELL_KB_ERROR_INVALID_PARAMETER);
    u8* base = guest_base(ctx);
    std::memset(base + info, 0, kKbInfoSize);
    ps_write32(base, info + 0, s.kb_max_connect); // now_connect, info and every status stay 0
    return lv2::CELL_OK;
}

// RPCS3's checks for the functions that need a keyboard; with none connected, a valid port is NO_DEVICE.
s32 kb_port(u32 port)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.kb_initialized) return error(CELL_KB_ERROR_UNINITIALIZED);
    if (port >= CELL_KB_MAX_KEYBOARDS) return error(CELL_KB_ERROR_INVALID_PARAMETER);
    return lv2::CELL_OK;
}

s32 cellKbRead(PPUContext&, u32 port, u32 data)
{
    if (const s32 r = kb_port(port); r != lv2::CELL_OK) return r;
    if (!data) return error(CELL_KB_ERROR_INVALID_PARAMETER);
    return error(CELL_KB_ERROR_NO_DEVICE);
}

s32 cellKbClearBuf(PPUContext&, u32 port)
{
    if (const s32 r = kb_port(port); r != lv2::CELL_OK) return r;
    return error(CELL_KB_ERROR_NO_DEVICE);
}

s32 cellKbSetCodeType(PPUContext&, u32 port, u32 type)
{
    if (const s32 r = kb_port(port); r != lv2::CELL_OK) return r;
    if (type > CELL_KB_CODETYPE_ASCII) return error(CELL_KB_ERROR_INVALID_PARAMETER);
    return lv2::CELL_OK; // RPCS3: a port without a keyboard is accepted
}

s32 cellKbSetReadMode(PPUContext&, u32 port, u32 mode)
{
    if (const s32 r = kb_port(port); r != lv2::CELL_OK) return r;
    if (mode > CELL_KB_RMODE_PACKET) return error(CELL_KB_ERROR_INVALID_PARAMETER);
    return lv2::CELL_OK;
}

s32 cellKbSetLEDStatus(PPUContext&, u32 port, u8 led)
{
    if (const s32 r = kb_port(port); r != lv2::CELL_OK) return r;
    if (led > 7) return error(CELL_KB_ERROR_SYS_SETTING_FAILED);
    return error(CELL_KB_ERROR_FATAL); // RPCS3: no keyboard on the port
}

} // namespace

void set_pad_input(HleProcess& process, const PadInput& input)
{
    IoState& s = process.state<IoState>();
    std::lock_guard lock(s.mutex);
    s.input = input;
}

void register_sys_io(Registry& r)
{
    r.add("sys_io", {
        PS_HLE_FUNC(0x1CF98800, cellPadInit),
        PS_HLE_FUNC(0x4D9B75D5, cellPadEnd),
        PS_HLE_FUNC(0xA703A51D, cellPadGetInfo2),
        PS_HLE_FUNC(0x8B72CDA1, cellPadGetData),
        PS_HLE_FUNC(0x578E3C98, cellPadSetPortSetting),
        PS_HLE_FUNC(0xF65544EE, cellPadSetActDirect),
        PS_HLE_FUNC(0x433F6EC0, cellKbInit),
        PS_HLE_FUNC(0xBFCE3285, cellKbEnd),
        PS_HLE_FUNC(0x2F1774D5, cellKbGetInfo),
        PS_HLE_FUNC(0xFF0A21B7, cellKbRead),
        PS_HLE_FUNC(0x2073B7F6, cellKbClearBuf),
        PS_HLE_FUNC(0xA5F85E4D, cellKbSetCodeType),
        PS_HLE_FUNC(0xDEEFDFA7, cellKbSetReadMode),
        PS_HLE_FUNC(0x3F72C56E, cellKbSetLEDStatus),
    });
}

} // namespace pspocket::hle
