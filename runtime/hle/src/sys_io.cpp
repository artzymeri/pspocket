// sys_io: controllers (cellPad), keyboards (cellKb), mice (cellMouse) and sys_config. Reference: RPCS3
// rpcs3/Emu/Cell/Modules/cellPad.cpp/.h, cellKb.cpp/.h, cellMouse.cpp/.h, sys_io_.cpp and Emu/Io/pad_types.h,
// Keyboard.h, MouseHandler.h (structure layouts and constants).
//
// Devices: one virtual controller on port 0, always connected, reported as RPCS3 reports a DualShock 3 (standard
// device type; PS3 conformity, press mode, sensor mode, high-precision sticks, actuator). The host feeds it with
// set_pad_input (pspocket/hle/input.hpp); at rest it reports no buttons and centred sticks. A game may add custom
// controllers of its own (cellPadLdd*: ports 1 to 6, fed by the game). No keyboard and no mouse are connected, as with
// RPCS3's default null handlers; cellKbCnvRawCode converts key codes all the same. The controller is reported
// connected, with the assignment-changed flag, from cellPadInit. sys_config_* are libio's own config service agent
// over lv2's sys_config (D-533, below).
//
// cellPadGetData reports changes only, as RPCS3 does: the first read after init or cellPadClearBuf, and any read where
// the controller's state differs from the last one reported, returns data; otherwise CELL_PAD_LEN_NO_CHANGE. With
// sensors on, data comes back at least every 10 ms. Rumble (cellPadSetActDirect) is accepted and ignored for now. The
// old cellPadGetInfo reports the controller with the USB IDs RPCS3 gives a standard pad without its own: Sony's
// PlayStation 3 controller (054C:0268).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/input.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kPadChan = "cellPad";
constexpr std::string_view kKbChan = "cellKb";

enum : u32 {
    CELL_PAD_ERROR_FATAL = 0x80121101,
    CELL_PAD_ERROR_INVALID_PARAMETER = 0x80121102,
    CELL_PAD_ERROR_ALREADY_INITIALIZED = 0x80121103,
    CELL_PAD_ERROR_UNINITIALIZED = 0x80121104,
    CELL_PAD_ERROR_NO_DEVICE = 0x80121107,
    CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD = 0x80121108,
    CELL_PAD_ERROR_TOO_MANY_DEVICES = 0x80121109,
    CELL_PADFILTER_ERROR_INVALID_PARAMETER = 0x80121401,

    CELL_KB_ERROR_FATAL = 0x80121001,
    CELL_KB_ERROR_INVALID_PARAMETER = 0x80121002,
    CELL_KB_ERROR_ALREADY_INITIALIZED = 0x80121003,
    CELL_KB_ERROR_UNINITIALIZED = 0x80121004,
    CELL_KB_ERROR_NO_DEVICE = 0x80121007,
    CELL_KB_ERROR_SYS_SETTING_FAILED = 0x80121008,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 CELL_MAX_PADS = 127, CELL_PAD_MAX_PORT_NUM = 7, CELL_PAD_MAX_CODES = 64;
constexpr u32 CELL_PAD_STATUS_CONNECTED = 1, CELL_PAD_STATUS_ASSIGN_CHANGES = 2, CELL_PAD_STATUS_CUSTOM_CONTROLLER = 4;
constexpr u32 CELL_PAD_SETTING_PRESS_ON = 2, CELL_PAD_SETTING_SENSOR_ON = 4;
constexpr u32 CELL_PAD_CAPABILITY_PS3_CONFORMITY = 1, CELL_PAD_CAPABILITY_PRESS_MODE = 2,
              CELL_PAD_CAPABILITY_SENSOR_MODE = 4, CELL_PAD_CAPABILITY_HP_ANALOG_STICK = 8,
              CELL_PAD_CAPABILITY_ACTUATOR = 16;
constexpr u32 CELL_PAD_DEV_TYPE_STANDARD = 0, CELL_PAD_DEV_TYPE_LDD = 5;
constexpr s32 CELL_PAD_LEN_NO_CHANGE = 0, CELL_PAD_LEN_CHANGE_DEFAULT = 8, CELL_PAD_LEN_CHANGE_PRESS_ON = 20,
              CELL_PAD_LEN_CHANGE_SENSOR_ON = 24;
constexpr u32 kVirtualPadCapability = CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE |
                                      CELL_PAD_CAPABILITY_SENSOR_MODE | CELL_PAD_CAPABILITY_HP_ANALOG_STICK |
                                      CELL_PAD_CAPABILITY_ACTUATOR;
// RPCS3 Input/product_info.cpp: the first standard-class product, a pad's IDs when its handler gives none.
constexpr u16 kVirtualPadVendorId = 0x054C, kVirtualPadProductId = 0x0268;
constexpr u32 CELL_PAD_MAX_CAPABILITY_INFO = 32;

// CellPadInfo (the layout before CellPadInfo2, 648 bytes): max_connect, now_connect, system_info (u32 each), then
// u16 vendor_id[127], u16 product_id[127], u8 status[127].
namespace info_off {
constexpr u32 max_connect = 0x0, now_connect = 0x4, system_info = 0x8, vendor_id = 0xC, product_id = 0x10A,
              status = 0x208, size = 0x288;
}
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

// What a port reports besides its status (RPCS3 pad_data_internal), and a custom (LDD) controller's data.
struct PortInfo {
    u32 capability = 0;
    u32 device_type = CELL_PAD_DEV_TYPE_STANDARD;
    u32 pclass_type = 0; // CELL_PAD_PCLASS_TYPE_STANDARD
    u32 pclass_profile = 0;
    bool ldd = false; // a custom controller the game registered (cellPadLddRegisterController)
    std::array<u16, CELL_PAD_MAX_CODES> ldd_data{};
};

struct IoState {
    std::mutex mutex;
    // cellPad
    u32 pad_max_connect = 0; // 0: not initialized
    std::array<u32, CELL_PAD_MAX_PORT_NUM> port_setting{};
    std::array<u32, CELL_PAD_MAX_PORT_NUM> reported_status{};
    std::array<PortInfo, CELL_PAD_MAX_PORT_NUM> ports{};
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
    s.ports.fill(PortInfo{});
    s.reported_status[0] = CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES; // the virtual controller
    s.ports[0].capability = kVirtualPadCapability;
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
        ps_write32(base, info + info2_off::device_capability + 4 * i, s.ports[i].capability);
        ps_write32(base, info + info2_off::device_type + 4 * i, s.ports[i].device_type);
        now_connect++;
    }
    ps_write32(base, info + info2_off::now_connect, now_connect);
    return lv2::CELL_OK;
}

// cellPadGetInfo(CellPadInfo*), the old layout (RPCS3 cellPadGetInfo): max_connect as cellPadInit got it (not limited
// to the 7 ports, as cellPadGetInfo2's is), system_info, and for each of the first max_connect ports (7 at most) its
// status and, with a controller connected, its USB vendor and product IDs. A port's assignment-changed flag is cleared
// before its status is written, so this function never reports it (and a cellPadGetInfo2 after it doesn't either).
s32 cellPadGetInfo(PPUContext& ctx, u32 info)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (!info) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    u8* base = guest_base(ctx);
    std::memset(base + info, 0, info_off::size);
    ps_write32(base, info + info_off::max_connect, s.pad_max_connect);
    ps_write32(base, info + info_off::system_info, 0);
    u32 now_connect = 0;
    for (u32 i = 0; i < pad_ports(s); i++) {
        s.reported_status[i] &= ~CELL_PAD_STATUS_ASSIGN_CHANGES;
        ps_write8(base, info + info_off::status + i, static_cast<u8>(s.reported_status[i]));
        if (!(s.reported_status[i] & CELL_PAD_STATUS_CONNECTED)) continue;
        ps_write16(base, info + info_off::vendor_id + 2 * i, kVirtualPadVendorId);
        ps_write16(base, info + info_off::product_id + 2 * i, kVirtualPadProductId);
        now_connect++;
    }
    ps_write32(base, info + info_off::now_connect, now_connect);
    return lv2::CELL_OK;
}

// cellPadGetCapabilityInfo(port, CellPadCapabilityInfo*): u32 info[32], zero but info[0], the controller's device
// capability (RPCS3; what cellPadGetInfo2 reports as device_capability). Checks: initialized, a port below 127 and a
// pointer (INVALID_PARAMETER), then a connected controller on a port below max_connect and 7 (NO_DEVICE).
s32 cellPadGetCapabilityInfo(PPUContext& ctx, u32 port, u32 info)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_PADS || !info) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (!pad_connected(s, port)) return error(CELL_PAD_ERROR_NO_DEVICE);
    u8* base = guest_base(ctx);
    std::memset(base + info, 0, 4 * CELL_PAD_MAX_CAPABILITY_INFO);
    ps_write32(base, info, s.ports[port].capability);
    return lv2::CELL_OK;
}

// cellPadClearBuf(port): RPCS3 clear_pad_buffer. The last state reported goes back to rest (no buttons, sticks
// centred, the sensors' resting values) and the next cellPadGetData reports in full, changed or not. Checks as
// cellPadGetCapabilityInfo's, without the pointer.
s32 cellPadClearBuf(PPUContext&, u32 port)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_PADS) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (!pad_connected(s, port)) return error(CELL_PAD_ERROR_NO_DEVICE);
    s.output = pad_buttons(PadInput{});
    s.buffer_cleared = true;
    return lv2::CELL_OK;
}

// RPCS3 pad_get_data: CellPadData for a connected port, the state locked.
void pad_get_data(IoState& s, u8* base, u32 port, u32 data)
{
    const u32 setting = s.port_setting[port];
    if (s.ports[port].ldd) {
        // A custom controller reports what the game inserted, always (RPCS3).
        const s32 len = (setting & CELL_PAD_SETTING_SENSOR_ON) ? CELL_PAD_LEN_CHANGE_SENSOR_ON
            : (setting & CELL_PAD_SETTING_PRESS_ON)            ? CELL_PAD_LEN_CHANGE_PRESS_ON
                                                               : CELL_PAD_LEN_CHANGE_DEFAULT;
        ps_write32(base, data, static_cast<u32>(len));
        for (u32 i = 0; i < static_cast<u32>(len); i++)
            ps_write16(base, data + kPadDataButtons + 2 * i, s.ports[port].ldd_data[i]);
        return;
    }
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
}

// RPCS3 cellPadGetData.
s32 cellPadGetData(PPUContext& ctx, u32 port, u32 data)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_PADS || !data) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (!pad_connected(s, port)) return error(CELL_PAD_ERROR_NO_DEVICE);
    pad_get_data(s, guest_base(ctx), port, data);
    return lv2::CELL_OK;
}

// cellPadGetDataExtra(port, device_type, data): cellPadGetData, then the device type (standard: BD remotes are not
// emulated) and the BD remote's two words (button[24], button[25]) cleared (RPCS3).
s32 cellPadGetDataExtra(PPUContext& ctx, u32 port, u32 device_type, u32 data)
{
    if (const s32 r = cellPadGetData(ctx, port, data); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (device_type) ps_write32(base, device_type, CELL_PAD_DEV_TYPE_STANDARD);
    ps_write16(base, data + kPadDataButtons + 2 * 24, 0);
    ps_write16(base, data + kPadDataButtons + 2 * 25, 0);
    return lv2::CELL_OK;
}

// sys_io_3733EA3C: the firmware's PS1 emulator's name for cellPadGetDataExtra (RPCS3).
s32 sys_io_3733EA3C(PPUContext& ctx, u32 port, u32 device_type, u32 data)
{
    return cellPadGetDataExtra(ctx, port, device_type, data);
}

// The checks the per-port functions share (RPCS3), the state locked: initialized, a port below 127 (and not `bad`),
// then a port below max_connect with a controller connected (CELL_PAD_ERROR_NO_DEVICE). Nullopt when all pass.
std::optional<s32> port_checks(IoState& s, u32 port, bool bad = false)
{
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_PADS || bad) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (port >= pad_ports(s)) return error(CELL_PAD_ERROR_NO_DEVICE);
    if (!pad_connected(s, port)) return error(CELL_PAD_ERROR_NO_DEVICE);
    return std::nullopt;
}

// cellPadGetRawData: RPCS3 checks and writes nothing (its TODO).
s32 cellPadGetRawData(PPUContext&, u32 port, u32 data)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (const auto r = port_checks(s, port, !data)) return *r;
    PS_LOG_WARN(kPadChan, "cellPadGetRawData(port {}): no raw data (RPCS3 writes none either)", port);
    return lv2::CELL_OK;
}

// Whether the port's controller has press-sensitive buttons (1) or motion sensors (1): a value, not an error code.
s32 cellPadInfoPressMode(PPUContext&, u32 port)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (const auto r = port_checks(s, port)) return *r;
    return (s.ports[port].capability & CELL_PAD_CAPABILITY_PRESS_MODE) ? 1 : 0;
}

s32 cellPadInfoSensorMode(PPUContext&, u32 port)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (const auto r = port_checks(s, port)) return *r;
    return (s.ports[port].capability & CELL_PAD_CAPABILITY_SENSOR_MODE) ? 1 : 0;
}

// Press and sensor modes, per port: a mode of 0 or 1; ports 7 and up are accepted and ignored; the controller must
// offer the mode (UNSUPPORTED_GAMEPAD, which a port without one gets too, as RPCS3's capability check).
s32 set_mode(u32 port, u32 mode, u32 capability, u32 setting_bit)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_PADS || mode > 1) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (port >= CELL_PAD_MAX_PORT_NUM) return lv2::CELL_OK;
    if (!(s.ports[port].capability & capability)) return error(CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD);
    if (mode) s.port_setting[port] |= setting_bit;
    else s.port_setting[port] &= ~setting_bit;
    return lv2::CELL_OK;
}

s32 cellPadSetPressMode(PPUContext&, u32 port, u32 mode)
{
    return set_mode(port, mode, CELL_PAD_CAPABILITY_PRESS_MODE, CELL_PAD_SETTING_PRESS_ON);
}

s32 cellPadSetSensorMode(PPUContext&, u32 port, u32 mode)
{
    return set_mode(port, mode, CELL_PAD_CAPABILITY_SENSOR_MODE, CELL_PAD_SETTING_SENSOR_ON);
}

// Custom controllers (LDD): the game registers one, takes the first port no controller uses (RPCS3 AddLddPad: ports 1
// to 6 here, the virtual controller has 0), and feeds its data itself (cellPadLddDataInsert). It reports as a PS3
// conformant pad of device type LDD, with the custom controller status bit.
s32 cellPadLddRegisterController(PPUContext&)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    for (u32 i = 1; i < CELL_PAD_MAX_PORT_NUM; i++) {
        if (s.ports[i].ldd || (s.reported_status[i] & CELL_PAD_STATUS_CONNECTED)) continue;
        s.ports[i] = PortInfo{};
        s.ports[i].ldd = true;
        s.ports[i].capability = CELL_PAD_CAPABILITY_PS3_CONFORMITY;
        s.ports[i].device_type = CELL_PAD_DEV_TYPE_LDD;
        s.port_setting[i] = 0;
        s.reported_status[i] = CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES | CELL_PAD_STATUS_CUSTOM_CONTROLLER;
        PS_LOG_INFO(kPadChan, "cellPadLddRegisterController: a custom controller on port {}", i);
        return static_cast<s32>(i);
    }
    return error(CELL_PAD_ERROR_TOO_MANY_DEVICES);
}

s32 ldd_handle(IoState& s, s32 handle, s32 not_ldd)
{
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (handle < 0 || handle >= static_cast<s32>(CELL_PAD_MAX_PORT_NUM)) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (!s.ports[static_cast<u32>(handle)].ldd) return not_ldd;
    return lv2::CELL_OK;
}

s32 cellPadLddUnregisterController(PPUContext&, s32 handle)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (const s32 r = ldd_handle(s, handle, error(CELL_PAD_ERROR_NO_DEVICE)); r != lv2::CELL_OK) return r;
    const u32 port = static_cast<u32>(handle);
    s.ports[port] = PortInfo{};
    s.reported_status[port] = CELL_PAD_STATUS_ASSIGN_CHANGES; // disconnected
    return lv2::CELL_OK;
}

// CellPadData from the game for its custom controller (s32 len, u16 button[64]): the buttons are what reads report.
s32 cellPadLddDataInsert(PPUContext& ctx, s32 handle, u32 data)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (!data) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (const s32 r = ldd_handle(s, handle, error(CELL_PAD_ERROR_NO_DEVICE)); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        s.ports[static_cast<u32>(handle)].ldd_data[i] = ps_read16(base, data + kPadDataButtons + 2 * i);
    return lv2::CELL_OK;
}

// The handle is the port (RPCS3); a handle that isn't a custom controller is CELL_PAD_ERROR_FATAL.
s32 cellPadLddGetPortNo(PPUContext&, s32 handle)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (const s32 r = ldd_handle(s, handle, error(CELL_PAD_ERROR_FATAL)); r != lv2::CELL_OK) return r;
    return handle;
}

// CellPadPeriphInfo: cellPadGetInfo2's fields, then pclass_type[7] and pclass_profile[7].
s32 cellPadPeriphGetInfo(PPUContext& ctx, u32 info)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (!info) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    u8* base = guest_base(ctx);
    constexpr u32 pclass_type = info2_off::size, pclass_profile = pclass_type + 4 * CELL_PAD_MAX_PORT_NUM;
    std::memset(base + info, 0, pclass_profile + 4 * CELL_PAD_MAX_PORT_NUM);
    ps_write32(base, info + info2_off::max_connect, s.pad_max_connect);
    u32 now_connect = 0;
    for (u32 i = 0; i < pad_ports(s); i++) {
        ps_write32(base, info + info2_off::port_status + 4 * i, s.reported_status[i]);
        ps_write32(base, info + info2_off::port_setting + 4 * i, s.port_setting[i]);
        s.reported_status[i] &= ~CELL_PAD_STATUS_ASSIGN_CHANGES;
        if (!(s.reported_status[i] & CELL_PAD_STATUS_CONNECTED)) continue;
        ps_write32(base, info + info2_off::device_capability + 4 * i, s.ports[i].capability);
        ps_write32(base, info + info2_off::device_type + 4 * i, s.ports[i].device_type);
        ps_write32(base, info + pclass_type + 4 * i, s.ports[i].pclass_type);
        ps_write32(base, info + pclass_profile + 4 * i, s.ports[i].pclass_profile);
        now_connect++;
    }
    ps_write32(base, info + info2_off::now_connect, now_connect);
    return lv2::CELL_OK;
}

// CellPadPeriphData: be32 pclass_type, pclass_profile, then CellPadData. Ports 0 to 6 only.
s32 cellPadPeriphGetData(PPUContext& ctx, u32 port, u32 data)
{
    IoState& s = io();
    std::lock_guard lock(s.mutex);
    if (!s.pad_max_connect) return error(CELL_PAD_ERROR_UNINITIALIZED);
    if (port >= CELL_PAD_MAX_PORT_NUM || !data) return error(CELL_PAD_ERROR_INVALID_PARAMETER);
    if (port >= pad_ports(s) || !pad_connected(s, port)) return error(CELL_PAD_ERROR_NO_DEVICE);
    u8* base = guest_base(ctx);
    pad_get_data(s, base, port, data + 8);
    ps_write32(base, data + 0, s.ports[port].pclass_type);
    ps_write32(base, data + 4, s.ports[port].pclass_profile);
    return lv2::CELL_OK;
}

// The pad filter: RPCS3 checks the state pointer, and its filter clamps the input to 0..1023 (its TODO).
s32 cellPadFilterIIRInit(PPUContext&, u32 sos, s32 cutoff)
{
    if (!sos) return error(CELL_PADFILTER_ERROR_INVALID_PARAMETER);
    PS_LOG_DEBUG(kPadChan, "cellPadFilterIIRInit({:#x}, cutoff {})", sos, cutoff);
    return lv2::CELL_OK;
}

u32 cellPadFilterIIRFilter(PPUContext&, u32 sos, u32 filter_in)
{
    (void)sos;
    return std::min(filter_in, 1023u);
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
    if (!(s.ports[port].capability & CELL_PAD_CAPABILITY_ACTUATOR)) return error(CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD);
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

s32 cellKbGetConfiguration(PPUContext&, u32 port, u32 config)
{
    if (const s32 r = kb_port(port); r != lv2::CELL_OK) return r;
    (void)config; // RPCS3: the config pointer is checked only after the device's status
    return error(CELL_KB_ERROR_NO_DEVICE);
}

// RPCS3 cellKbCnvRawCode: a USB key code, with the modifier keys and LEDs, as the character it types in a layout.
u32 cellKbCnvRawCode(PPUContext&, u32 arrange, u32 mkey, u32 led, u16 raw)
{
    enum : u16 {
        RAWDAT = 0x8000, KEYPAD = 0x4000,
        K_A = 0x04, K_Q = 0x14, K_Z = 0x1D, K_1 = 0x1E, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_0, K_ENTER,
        K_ESCAPE, K_BS, K_TAB, K_SPACE, K_MINUS, K_2E, K_2F, K_30, K_31, K_32, K_SEMICOLON, K_34, K_106_KANJI,
        K_COMMA, K_PERIOD, K_SLASH, K_CAPS_LOCK, K_NUM_LOCK = 0x53, K_KPAD_SLASH, K_KPAD_ASTERISK, K_KPAD_MINUS,
        K_KPAD_PLUS, K_KPAD_ENTER, K_KPAD_1, K_KPAD_9 = 0x61, K_KPAD_0, K_APPLICATION = 0x65, K_BACKSLASH_106 = 0x87,
        K_KANA, K_YEN_106, K_HENKAN, K_MUHENKAN, K_LESS = 0x90, K_HASHTAG, K_SSHARP, K_BACK_QUOTE,
    };
    enum : u32 { MAPPING_101 = 0, MAPPING_106 = 1, MAPPING_GERMAN = 3, MAPPING_FRENCH = 5 };

    if (raw <= 0x03 || raw == K_ESCAPE || raw == K_106_KANJI || (raw >= K_CAPS_LOCK && raw <= K_NUM_LOCK) ||
        raw == K_APPLICATION || raw == K_KANA || raw == K_HENKAN || raw == K_MUHENKAN)
        return raw | RAWDAT;

    const bool alt = mkey & (0x04 | 0x40), shift = mkey & (0x02 | 0x20);
    const bool caps_lock = led & 0x02, num_lock = led & 0x01;
    const bool shift_lock = caps_lock && (arrange == MAPPING_GERMAN || arrange == MAPPING_FRENCH);

    if (num_lock) {
        if (raw == K_KPAD_SLASH) return 0x2F | KEYPAD;
        if (raw == K_KPAD_ASTERISK) return 0x2A | KEYPAD;
        if (raw == K_KPAD_MINUS) return 0x2D | KEYPAD;
        if (raw == K_KPAD_PLUS) return 0x2B | KEYPAD;
        if (raw == K_KPAD_ENTER) return 0x0A | KEYPAD;
        if (raw == K_KPAD_0) return 0x30 | KEYPAD;
        if (raw >= K_KPAD_1 && raw <= K_KPAD_9) return (raw - 0x28) | KEYPAD;
    }

    // Caps lock capitalises letters only, except in layouts where it is a shift lock.
    const auto ascii = [&](u16 plain, u16 shifted = 0, u16 altered = 0) -> u32 {
        const bool letter = (plain >= 'a' && plain <= 'z') || (plain >= 'A' && plain <= 'Z');
        if ((shift || (caps_lock && (shift_lock || letter))) && shifted) return shifted;
        if (alt && altered) return altered;
        return plain;
    };
    struct Key { u16 raw, plain, shifted, altered = 0; };
    static constexpr Key k106[] = {
        {K_1, '1', '!'}, {K_2, '2', '"'}, {K_3, '3', '#'}, {K_4, '4', '$'}, {K_5, '5', '%'}, {K_6, '6', '&'},
        {K_7, '7', '\''}, {K_8, '8', '('}, {K_9, '9', ')'}, {K_0, '0', '~'}, {K_2E, '^', '~'}, {K_2F, '@', '`'},
        {K_30, '[', '{'}, {K_32, ']', '}'}, {K_SEMICOLON, ';', '+'}, {K_34, ':', '*'}, {K_COMMA, ',', '<'},
        {K_PERIOD, '.', '>'}, {K_SLASH, '/', '?'}, {K_BACKSLASH_106, '\\', '_'}, {K_YEN_106, 190, '|'},
    };
    static constexpr Key k101[] = {
        {K_1, '1', '!'}, {K_2, '2', '@'}, {K_3, '3', '#'}, {K_4, '4', '$'}, {K_5, '5', '%'}, {K_6, '6', '^'},
        {K_7, '7', '&'}, {K_8, '8', '*'}, {K_9, '9', '('}, {K_0, '0', ')'}, {K_MINUS, '-', '_'}, {K_2E, '=', '+'},
        {K_2F, '[', '{'}, {K_30, ']', '}'}, {K_31, '\\', '|'}, {K_SEMICOLON, ';', ':'}, {K_34, '\'', '"'},
        {K_COMMA, ',', '<'}, {K_PERIOD, '.', '>'}, {K_SLASH, '/', '?'}, {K_BACK_QUOTE, '`', '~'},
    };
    static constexpr Key kGerman[] = {
        {K_1, '1', '!'}, {K_2, '2', '"'}, {K_3, '3', 245}, {K_4, '4', '$'}, {K_5, '5', '%'}, {K_6, '6', '&'},
        {K_7, '7', '/', '{'}, {K_8, '8', '(', '['}, {K_9, '9', ')', ']'}, {K_0, '0', '=', '}'},
        {K_MINUS, '-', '_'}, {K_2E, '^', 248}, {K_COMMA, ',', ';'}, {K_PERIOD, '.', ':'},
        {K_KPAD_PLUS, '+', '*', '~'}, {K_LESS, '<', '>', '|'}, {K_HASHTAG, '#', '\''}, {K_SSHARP, 225, '?', '\\'},
        {K_BACK_QUOTE, 239, '`'}, {K_Q, 'q', 'Q', '@'},
    };
    const std::span<const Key> layout = arrange == MAPPING_106      ? std::span<const Key>(k106)
                                        : arrange == MAPPING_101    ? std::span<const Key>(k101)
                                        : arrange == MAPPING_GERMAN ? std::span<const Key>(kGerman)
                                                                    : std::span<const Key>();
    for (const Key& k : layout)
        if (k.raw == raw) return ascii(k.plain, k.shifted, k.altered);

    if (raw >= K_A && raw <= K_Z) return shift != caps_lock ? raw + 0x3D : raw + 0x5D;
    if (raw >= K_1 && raw <= K_9) return raw + 0x13;
    if (raw == K_0) return 0x30;
    if (raw == K_ENTER) return 0x0A;
    if (raw == K_BS) return 0x08;
    if (raw == K_TAB) return 0x09;
    if (raw == K_SPACE) return 0x20;
    return 0;
}

// ------------------------------------------------------------------------------------------- cellMouse (none)
// RPCS3 cellMouse.cpp with its null mouse handler: the library initialises, no mouse is ever connected.

enum : u32 {
    CELL_MOUSE_ERROR_FATAL = 0x80121201,
    CELL_MOUSE_ERROR_INVALID_PARAMETER = 0x80121202,
    CELL_MOUSE_ERROR_ALREADY_INITIALIZED = 0x80121203,
    CELL_MOUSE_ERROR_UNINITIALIZED = 0x80121204,
    CELL_MOUSE_ERROR_NO_DEVICE = 0x80121207,
};
constexpr u32 CELL_MAX_MICE = 127;
// CellMouseInfo: max_connect, now_connect, info (u32 each), u16 vendor_id[127], u16 product_id[127], u8 status[127].
constexpr u32 kMouseInfoSize = 648;

struct MouseState {
    std::mutex mutex;
    bool initialized = false;
    u32 max_connect = 0;
};

MouseState& mouse() { return HleProcess::current()->state<MouseState>(); }

s32 cellMouseInit(PPUContext&, u32 max_connect)
{
    MouseState& m = mouse();
    std::lock_guard lock(m.mutex);
    if (m.initialized) return error(CELL_MOUSE_ERROR_ALREADY_INITIALIZED);
    if (max_connect == 0 || max_connect > CELL_MAX_MICE) return error(CELL_MOUSE_ERROR_INVALID_PARAMETER);
    m.initialized = true;
    m.max_connect = std::min(max_connect, 7u);
    PS_LOG_INFO("cellMouse", "cellMouseInit({}): no mouse is connected", max_connect);
    return lv2::CELL_OK;
}

s32 cellMouseEnd(PPUContext&)
{
    MouseState& m = mouse();
    std::lock_guard lock(m.mutex);
    if (!m.initialized) return error(CELL_MOUSE_ERROR_UNINITIALIZED);
    m.initialized = false;
    return lv2::CELL_OK;
}

s32 cellMouseGetInfo(PPUContext& ctx, u32 info)
{
    MouseState& m = mouse();
    std::lock_guard lock(m.mutex);
    if (!m.initialized) return error(CELL_MOUSE_ERROR_UNINITIALIZED);
    if (!info) return error(CELL_MOUSE_ERROR_INVALID_PARAMETER);
    u8* base = guest_base(ctx);
    std::memset(base + info, 0, kMouseInfoSize);
    ps_write32(base, info, m.max_connect); // now_connect, info and every status stay 0
    return lv2::CELL_OK;
}

// The checks before a port's device: initialised, and the port (and the pointer, when `pointer` is set) valid.
s32 mouse_port(u32 port, bool pointer_ok = true)
{
    MouseState& m = mouse();
    std::lock_guard lock(m.mutex);
    if (!m.initialized) return error(CELL_MOUSE_ERROR_UNINITIALIZED);
    if (port >= CELL_MAX_MICE || !pointer_ok) return error(CELL_MOUSE_ERROR_INVALID_PARAMETER);
    return lv2::CELL_OK;
}

s32 cellMouseClearBuf(PPUContext&, u32 port)
{
    if (const s32 r = mouse_port(port); r != lv2::CELL_OK) return r;
    return error(CELL_MOUSE_ERROR_NO_DEVICE);
}

s32 cellMouseGetData(PPUContext&, u32 port, u32 data)
{
    if (const s32 r = mouse_port(port, data != 0); r != lv2::CELL_OK) return r;
    return error(CELL_MOUSE_ERROR_NO_DEVICE);
}

s32 cellMouseGetDataList(PPUContext&, u32 port, u32 data)
{
    if (const s32 r = mouse_port(port, data != 0); r != lv2::CELL_OK) return r;
    return error(CELL_MOUSE_ERROR_NO_DEVICE);
}

s32 cellMouseGetTabletDataList(PPUContext&, u32 port, u32 data)
{
    if (const s32 r = mouse_port(port, data != 0); r != lv2::CELL_OK) return r;
    return error(CELL_MOUSE_ERROR_NO_DEVICE);
}

s32 cellMouseGetRawData(PPUContext&, u32 port, u32 data)
{
    if (const s32 r = mouse_port(port, data != 0); r != lv2::CELL_OK) return r;
    return error(CELL_MOUSE_ERROR_NO_DEVICE);
}

// RPCS3: the tablet functions check only the port, then answer FATAL for a port without a mouse.
s32 cellMouseInfoTabletMode(PPUContext&, u32 port, u32)
{
    if (const s32 r = mouse_port(port); r != lv2::CELL_OK) return r;
    return error(CELL_MOUSE_ERROR_FATAL);
}

s32 cellMouseSetTabletMode(PPUContext&, u32 port, u32)
{
    if (const s32 r = mouse_port(port); r != lv2::CELL_OK) return r;
    return error(CELL_MOUSE_ERROR_FATAL);
}

// --------------------------------------------------------------------------------------------------- sys_config
// libio's config service agent (sys_io.sprx, fw 4.92), as libio has it (D-533). One agent object on liblv2's heap
// (_sys_malloc), made at the first call under a spin guard that waits with sys_timer_usleep(1000): its recursive
// lwmutex ("_io_mtx") at +0, the event queue at +0x18, lv2's sys_config handle at +0x1C (-1 until opened) and the I/O
// error handler at +0x20. The first sys_config_start makes the queue ("_io_eq_", priority order, 32 deep), opens
// sys_config on it and starts the "_cfg_ev_hndlr" thread (priority 512, 8 KB stack, joinable), which receives the
// queue's events while the start count is not zero: a service event (source 1) is read with
// sys_config_get_service_event into a buffer from the heap and handed to its listener's callbacks (a registered
// service: (service ID, user ID, verbosity, its data or NULL, the data's size); an unregistered one: (service ID, user
// ID)); an I/O error event (source 2) is read with sys_config_get_io_event and handed to the handler (its first word,
// the rest, the rest's size). Listeners are 20-byte nodes on the heap (next, previous, lv2's listener ID, the two
// callbacks' descriptors) in libio's list, oldest first. The callbacks run under the lwmutex, which every call takes
// with a 4-second timeout and goes on without when that fails, as libio does. libio's diagnostics go out through
// _sys_printf with libio's text. The last sys_config_stop frees the listeners, destroys the queue (forcing the thread
// out), closes the handle, joins the thread, then deletes the agent.

constexpr std::string_view kCfgChan = "sys_config";

namespace cfg {
constexpr u32 queue = 0x18, handle = 0x1C, io_handler = 0x20, size = 0x28; // the lwmutex at +0
constexpr u32 next = 0, prev = 4, listener = 8, on_registered = 12, on_unregistered = 16, node_size = 20;
constexpr u32 CELL_ENOMEM = 0x80010004, CELL_EINVAL = 0x80010002, CELL_EABORT = 0x8001000C,
              CELL_ECANCELED = 0x80010013, CELL_EEXIST = 0x80010014;
constexpr u32 kLockTimeout = 4000000; // microseconds
} // namespace cfg

struct ConfigState {
    std::atomic<u32> guard{0};   // libio's spin guard around making and deleting the agent
    std::atomic<u32> agent{0};   // the agent on liblv2's heap, or 0
    std::atomic<s32> started{0}; // sys_config_start's count (the event thread reads it unlocked)
    u64 thread = 0;              // the event thread
    u32 head = 0, tail = 0;      // the listeners, oldest first
};

ConfigState& config_state() { return HleProcess::current()->state<ConfigState>(); }

// liblv2's export `name`, which libio imports: pspocket's own sysPrxForUser.
u64 lv2_lib(PPUContext& ctx, std::string_view name, std::span<const u64> args)
{
    const Function* f = registry().find("sysPrxForUser", recomp::nid_of(name));
    if (!f) PS_FATAL(kCfgChan, "sysPrxForUser::{}, which libio imports, is not pspocket's", name);
    std::array<u64, 8> saved{};
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    for (u32 i = 0; i < args.size() && i < 8; i++) ctx.gpr[3 + i] = args[i];
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

u64 lv2_lib(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return lv2_lib(ctx, name, std::span<const u64>(args.begin(), args.size()));
}

u32 io_malloc(PPUContext& ctx, u32 size) { return static_cast<u32>(lv2_lib(ctx, "_sys_malloc", {size})); }
void io_free(PPUContext& ctx, u32 addr) { lv2_lib(ctx, "_sys_free", {addr}); }

// _sys_printf(fmt, [str,] args...), the format and the string on the caller's stack.
void io_printf(PPUContext& ctx, std::string_view fmt, std::initializer_list<u64> args, std::string_view str = {})
{
    u8* b = guest_base(ctx);
    StackVar text(ctx, b, static_cast<u32>(fmt.size() + str.size() + 2));
    std::memcpy(b + text.addr(), fmt.data(), fmt.size());
    const u32 at = text.addr() + static_cast<u32>(fmt.size()) + 1;
    if (!str.empty()) std::memcpy(b + at, str.data(), str.size()); // an empty view's data() may be null (D-605)
    std::vector<u64> a{text.addr()};
    if (!str.empty()) a.push_back(at);
    a.insert(a.end(), args.begin(), args.end());
    lv2_lib(ctx, "_sys_printf", a);
}

void cfg_lock(PPUContext& ctx, u32 a)
{
    if (const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_lock", {a, cfg::kLockTimeout})); r != lv2::CELL_OK)
        io_printf(ctx, "config_service_agent::lock() failed. [%x]\n", {static_cast<u64>(static_cast<s64>(r))});
}

void cfg_unlock(PPUContext& ctx, u32 a)
{
    if (const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_unlock", {a})); r != lv2::CELL_OK)
        io_printf(ctx, "config_service_agent::unlock() failed. [%x]\n", {static_cast<u64>(static_cast<s64>(r))});
}

void cfg_guard(PPUContext& ctx, ConfigState& c)
{
    while (c.guard.fetch_add(1) != 0) {
        c.guard.fetch_sub(1);
        lv2_syscall(ctx, 141, {1000}); // sys_timer_usleep
    }
}

// The agent, made at the first call (libio's config_service_agent::instance).
u32 cfg_agent(PPUContext& ctx)
{
    ConfigState& c = config_state();
    u8* b = guest_base(ctx);
    cfg_guard(ctx, c);
    if (const u32 a = c.agent) {
        c.guard.fetch_sub(1);
        return a;
    }
    const u32 a = io_malloc(ctx, cfg::size);
    if (!a) {
        io_printf(ctx, "config_service_agent::initialize() %d Can't allocate memory.\n", {50});
        PS_FATAL(kCfgChan, "libio's config service agent: liblv2's heap has no {} bytes (libio goes on with NULL "
            "and faults)", cfg::size);
    }
    {
        StackVar attr(ctx, b, 16); // sys_lwmutex_attribute_t {protocol, recursive, name[8]}
        ps_write32(b, attr.addr(), 2);        // SYS_SYNC_PRIORITY
        ps_write32(b, attr.addr() + 4, 0x10); // SYS_SYNC_RECURSIVE
        std::memcpy(b + attr.addr() + 8, "_io_mtx", 7);
        if (const s32 r = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_create", {a, attr.addr()})); r != lv2::CELL_OK)
            io_printf(ctx, "%s %d sys_lwmutex_create() failed. [%d]\n", {312, static_cast<u64>(static_cast<s64>(r))},
                "mutex");
    }
    ps_write32(b, a + cfg::handle, 0xFFFFFFFF);
    ps_write32(b, a + cfg::io_handler, 0);
    ps_write32(b, a + cfg::queue, 0);
    c.agent = a;
    c.guard.fetch_sub(1);
    return a;
}

// The listener node with lv2's ID `id`, or 0.
u32 cfg_find(u8* b, const ConfigState& c, u32 id)
{
    for (u32 n = c.head; n; n = ps_read32(b, n + cfg::next))
        if (ps_read32(b, n + cfg::listener) == id) return n;
    return 0;
}

void cfg_append(u8* b, ConfigState& c, u32 n)
{
    if (c.tail) {
        ps_write32(b, c.tail + cfg::next, n);
        ps_write32(b, n + cfg::prev, c.tail);
        c.tail = n;
        return;
    }
    c.head = c.tail = n;
    ps_write32(b, n + cfg::prev, 0);
    ps_write32(b, n + cfg::next, 0);
}

// Takes the node with lv2's ID `id` out of the list: the node, or 0.
u32 cfg_unlink(u8* b, ConfigState& c, u32 id)
{
    const u32 n = cfg_find(b, c, id);
    if (!n) return 0;
    const u32 prev = ps_read32(b, n + cfg::prev), next = ps_read32(b, n + cfg::next);
    if (prev) {
        ps_write32(b, prev + cfg::next, next);
        if (next) ps_write32(b, next + cfg::prev, prev);
        else c.tail = prev;
    } else if (next) {
        c.head = next;
        ps_write32(b, next + cfg::prev, 0);
    } else {
        c.head = c.tail = 0;
    }
    return n;
}

// A callback of the game's, through its descriptor; libio calls whatever the word holds.
void cfg_callback(PPUContext& ctx, u32 opd, std::string_view what, std::initializer_list<u64> args)
{
    if (opd && guest_readable(thread_of(ctx), opd, 8)) {
        call_guest(ctx, opd, args);
        return;
    }
    log_call_site(ctx, guest_base(ctx), std::format("sys_config: the listener's {} callback is {:#x}, not a function "
        "descriptor (libio calls it and faults); not called", what, opd));
}

// A service event read into `buf` (sys_config_service_event_t): its listener's callback, under the lock.
void cfg_deliver(PPUContext& ctx, u32 a, u32 buf, bool registered, u32 data, u32 size)
{
    ConfigState& c = config_state();
    u8* b = guest_base(ctx);
    cfg_lock(ctx, a);
    if (c.started != 0 && buf) {
        if (const u32 n = cfg_find(b, c, ps_read32(b, buf))) {
            const u64 sid = ps_read64(b, buf + 8), uid = ps_read64(b, buf + 16);
            if (registered)
                cfg_callback(ctx, ps_read32(b, n + cfg::on_registered), "registered",
                    {sid, uid, ps_read64(b, buf + 24), data, size});
            else
                cfg_callback(ctx, ps_read32(b, n + cfg::on_unregistered), "unregistered", {sid, uid});
        }
    }
    cfg_unlock(ctx, a);
}

// Event source 1: (handle, registered << 32 | event ID, size).
s32 cfg_service_event(PPUContext& ctx, u32 a, u64 d1, u64 d2, u64 d3)
{
    u8* b = guest_base(ctx);
    const u32 size = static_cast<u32>(d3), handle = ps_read32(b, a + cfg::handle);
    if (static_cast<u32>(d1) != handle) {
        io_printf(ctx, "config_service_agent::handle_service_event() : Invalid config_customer_id. [%lx:%lx]\n",
            {handle, static_cast<u32>(d1)});
    }
    u32 buf = 0, have = 0;
    if (size) {
        buf = io_malloc(ctx, size);
        if (!buf) return error(cfg::CELL_ENOMEM);
        have = size;
    }
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 518, {handle, static_cast<u32>(d2), buf, have}));
        r != lv2::CELL_OK) {
        io_printf(ctx, "config_service_agent::handle_service_event() : sys_config_get_service_event() failed. [%d]\n",
            {static_cast<u64>(static_cast<s64>(r))});
        return r; // libio keeps the buffer
    }
    switch (d2 >> 32) {
    case 0:
        cfg_deliver(ctx, a, buf, false, 0, 0);
        if (buf) io_free(ctx, buf);
        return lv2::CELL_OK;
    case 1: {
        const u32 n = buf ? ps_read32(b, buf + 32) : 0;
        cfg_deliver(ctx, a, buf, true, n ? buf + 40 : 0, n);
        io_free(ctx, buf);
        return lv2::CELL_OK;
    }
    default:
        io_printf(ctx, "config_service_agent::event_handler() : invalid event.\n", {});
        return error(cfg::CELL_EABORT); // libio keeps the buffer
    }
}

// Event source 2: (handle, event ID, size); the handler gets the event's first word, the rest and its size.
s32 cfg_io_event(PPUContext& ctx, u32 a, u64 d2, u64 d3)
{
    u8* b = guest_base(ctx);
    cfg_lock(ctx, a);
    s32 r = lv2::CELL_OK;
    if (ps_read32(b, a + cfg::io_handler)) {
        const u32 size = static_cast<u32>(d3);
        u32 buf = 0, have = 0;
        if (size) {
            buf = io_malloc(ctx, size);
            if (!buf) {
                cfg_unlock(ctx, a);
                return error(cfg::CELL_ENOMEM);
            }
            have = size;
        }
        r = static_cast<s32>(lv2_syscall(ctx, 523, {ps_read32(b, a + cfg::handle), static_cast<u32>(d2), buf, have}));
        if (r == lv2::CELL_OK) {
            const u32 handler = ps_read32(b, a + cfg::io_handler);
            const u32 first = buf && guest_readable(thread_of(ctx), buf, 4) ? ps_read32(b, buf) : 0;
            if (handler) {
                call_guest(ctx, handler, {static_cast<u64>(static_cast<s64>(static_cast<s32>(first))), buf + 4u,
                    size - 4u});
            } else {
                r = error(cfg::CELL_EABORT);
            }
            io_free(ctx, buf);
        } else if (buf) {
            io_free(ctx, buf);
        }
    }
    cfg_unlock(ctx, a);
    return r;
}

// The event thread's loop (libio's config_service_agent::event_handler).
void cfg_event_loop(PPUContext& ctx, u32 a)
{
    ConfigState& c = config_state();
    u8* b = guest_base(ctx);
    StackVar event(ctx, b, 32); // sys_event_t, which lv2 answers in r4..r7
    while (c.started != 0) {
        u64 saved[10];
        for (u32 k = 0; k < 10; k++) saved[k] = ctx.gpr[3 + k];
        ctx.gpr[3] = ps_read32(b, a + cfg::queue);
        ctx.gpr[4] = event.addr();
        ctx.gpr[5] = 0;
        ctx.gpr[11] = 130; // sys_event_queue_receive
        thread_of(ctx).kernel.syscall(ctx);
        const s32 r = static_cast<s32>(ctx.gpr[3]);
        const u64 source = ctx.gpr[4], d1 = ctx.gpr[5], d2 = ctx.gpr[6], d3 = ctx.gpr[7];
        for (u32 k = 0; k < 10; k++) ctx.gpr[3 + k] = saved[k];
        if (r != lv2::CELL_OK) {
            if (static_cast<u32>(r) == cfg::CELL_ECANCELED && c.started != 0)
                io_printf(ctx, "%s %d m_ref_count_ != 0. [%d]\n", {400, static_cast<u64>(c.started.load())},
                    "event_handler");
            return;
        }
        ps_write64(b, event.addr(), source);
        ps_write64(b, event.addr() + 8, d1);
        ps_write64(b, event.addr() + 16, d2);
        ps_write64(b, event.addr() + 24, d3);
        if (source == 1) {
            if (const s32 e = cfg_service_event(ctx, a, d1, d2, d3); e != lv2::CELL_OK)
                io_printf(ctx, "config_service_agent::event_handler() : handle_service_event() failed. [%d]\n",
                    {static_cast<u64>(static_cast<s64>(e))});
        } else if (source == 2) {
            if (const s32 e = cfg_io_event(ctx, a, d2, d3); e != lv2::CELL_OK)
                io_printf(ctx, "config_service_agent::event_handler() : handle_io_error_event() failed. [%d]\n",
                    {static_cast<u64>(static_cast<s64>(e))});
        } else {
            io_printf(ctx, "config_service_agent::event_handler() : Invalid config_event_type [%d]\n", {d1});
        }
    }
}

// The "_cfg_ev_hndlr" thread's entry.
void _pspocket_sys_config_event_thread(PPUContext& ctx, u64)
{
    cfg_event_loop(ctx, cfg_agent(ctx));
    lv2_lib(ctx, "sys_ppu_thread_exit", {0});
}

s32 sys_config_start(PPUContext& ctx)
{
    ConfigState& c = config_state();
    u8* b = guest_base(ctx);
    const u32 a = cfg_agent(ctx);
    cfg_lock(ctx, a);
    if (++c.started != 1) {
        cfg_unlock(ctx, a);
        return lv2::CELL_OK;
    }
    s32 r;
    {
        StackVar attr(ctx, b, 16); // sys_event_queue_attribute_t {protocol, type, name[8]}
        ps_write32(b, attr.addr(), 2); // SYS_SYNC_PRIORITY
        ps_write32(b, attr.addr() + 4, 1); // SYS_PPU_QUEUE
        std::memcpy(b + attr.addr() + 8, "_io_eq_", 7);
        r = static_cast<s32>(lv2_syscall(ctx, 128, {a + cfg::queue, attr.addr(), 0, 32})); // sys_event_queue_create
    }
    if (r == lv2::CELL_OK) {
        r = static_cast<s32>(lv2_syscall(ctx, 516, {ps_read32(b, a + cfg::queue), a + cfg::handle})); // sys_config_open
        if (r == lv2::CELL_OK) {
            StackVar v(ctx, b, 32); // the thread's ID, then its name
            std::memcpy(b + v.addr() + 8, "_cfg_ev_hndlr", 13);
            const u32 entry = HleProcess::current()->link_function("sys_io",
                recomp::nid_of("_pspocket_sys_config_event_thread"), "_pspocket_sys_config_event_thread");
            r = static_cast<s32>(lv2_lib(ctx, "sys_ppu_thread_create", {v.addr(), entry, 0, 512, 0x2000, 1,
                v.addr() + 8}));
            if (r == lv2::CELL_OK) {
                c.thread = ps_read64(b, v.addr());
                cfg_unlock(ctx, a);
                return lv2::CELL_OK;
            }
            lv2_syscall(ctx, 517, {ps_read32(b, a + cfg::handle)}); // sys_config_close
        }
        lv2_syscall(ctx, 129, {ps_read32(b, a + cfg::queue), 0}); // sys_event_queue_destroy
    }
    cfg_unlock(ctx, a); // the count stays raised, as libio leaves it
    return r;
}

// libio's config_service_agent::stop.
s32 cfg_stop(PPUContext& ctx, u32 a)
{
    ConfigState& c = config_state();
    u8* b = guest_base(ctx);
    cfg_lock(ctx, a);
    if (c.started == 0) {
        io_printf(ctx, "config_service_agent::stop() %d m_ref_count_ = %d\n", {114, 0});
        cfg_unlock(ctx, a);
        return error(cfg::CELL_EABORT);
    }
    if (--c.started == 0) {
        while (const u32 n = c.head) {
            const u32 next = ps_read32(b, n + cfg::next);
            if (next) {
                ps_write32(b, next + cfg::prev, 0);
                c.head = next;
            } else {
                c.head = c.tail = 0;
            }
            io_free(ctx, n);
        }
        s32 r = static_cast<s32>(lv2_syscall(ctx, 129, {ps_read32(b, a + cfg::queue), 1})); // destroy, forced
        if (r != lv2::CELL_OK) {
            io_printf(ctx, "config_service_agent::finalize() %d sys_event_queue_destroy() failed. [%d]\n",
                {136, static_cast<u64>(static_cast<s64>(r))});
            cfg_unlock(ctx, a);
            return r;
        }
        ps_write32(b, a + cfg::queue, 0);
        r = static_cast<s32>(lv2_syscall(ctx, 517, {ps_read32(b, a + cfg::handle)}));
        if (r != lv2::CELL_OK) {
            io_printf(ctx, "config_service_agent::finalize() %d sys_config_close() failed. [%d]\n",
                {145, static_cast<u64>(static_cast<s64>(r))});
            cfg_unlock(ctx, a);
            return r;
        }
        StackVar status(ctx, b, 8);
        r = static_cast<s32>(lv2_syscall(ctx, 44, {c.thread, status.addr()})); // sys_ppu_thread_join
        if (r != lv2::CELL_OK) {
            io_printf(ctx, "config_service_agent::stop() %d sys_ppu_thread_join() failed. [%d]\n",
                {155, static_cast<u64>(static_cast<s64>(r))});
        }
    }
    cfg_unlock(ctx, a);
    return lv2::CELL_OK;
}

s32 sys_config_stop(PPUContext& ctx)
{
    ConfigState& c = config_state();
    const s32 r = cfg_stop(ctx, cfg_agent(ctx));
    cfg_guard(ctx, c);
    if (c.started == 0) {
        if (const u32 a = c.agent) {
            if (const s32 e = static_cast<s32>(lv2_lib(ctx, "sys_lwmutex_destroy", {a})); e != lv2::CELL_OK)
                io_printf(ctx, "%s %d sys_lwmutex_destroy() failed. [%d]\n", {321, static_cast<u64>(static_cast<s64>(e))},
                    "~mutex");
            io_free(ctx, a);
            c.agent = 0;
        }
    }
    c.guard.fetch_sub(1);
    return r;
}

// sys_config_add_service_listener(service_id, min_verbosity, in, size, callbacks[2], type, u32* listener): lv2's
// listener, then libio's node with the two callbacks (registered, unregistered).
s32 sys_config_add_service_listener(PPUContext& ctx, s64 service_id, u64 min_verbosity, u32 in, u32 size,
    u32 callbacks, u32 type, u32 out)
{
    if (service_id < 0 && (in || size)) return error(cfg::CELL_EINVAL);
    ConfigState& c = config_state();
    u8* b = guest_base(ctx);
    const u32 a = cfg_agent(ctx);
    cfg_lock(ctx, a);
    const s32 r = static_cast<s32>(lv2_syscall(ctx, 519, {ps_read32(b, a + cfg::handle),
        static_cast<u64>(service_id), min_verbosity, in, size, type, out}));
    if (r == lv2::CELL_OK) {
        const u32 n = io_malloc(ctx, cfg::node_size);
        if (!n) {
            // libio fills the node before it looks at the pointer: a fault at NULL + 12 on a console without memory.
            lv2_syscall(ctx, 520, {ps_read32(b, a + cfg::handle), ps_read32(b, out)});
            cfg_unlock(ctx, a);
            return error(cfg::CELL_ENOMEM);
        }
        ps_write32(b, n + cfg::on_registered, ps_read32(b, callbacks));
        ps_write32(b, n + cfg::listener, ps_read32(b, out));
        ps_write32(b, n + cfg::prev, 0);
        ps_write32(b, n + cfg::on_unregistered, ps_read32(b, callbacks + 4));
        ps_write32(b, n + cfg::next, 0);
        cfg_append(b, c, n);
    }
    cfg_unlock(ctx, a);
    return r;
}

s32 sys_config_remove_service_listener(PPUContext& ctx, u32 listener)
{
    ConfigState& c = config_state();
    u8* b = guest_base(ctx);
    const u32 a = cfg_agent(ctx);
    cfg_lock(ctx, a);
    const u32 n = cfg_unlink(b, c, listener);
    const s32 r = static_cast<s32>(lv2_syscall(ctx, 520, {ps_read32(b, a + cfg::handle), listener}));
    cfg_unlock(ctx, a);
    if (n) io_free(ctx, n);
    return r;
}

// sys_config_register_service(service_id, user_id, verbosity, data, size, u32* service).
s32 sys_config_register_service(PPUContext& ctx, u64 service_id, u64 user_id, u64 verbosity, u32 data, u32 size,
    u32 out)
{
    u8* b = guest_base(ctx);
    const u32 a = cfg_agent(ctx);
    cfg_lock(ctx, a);
    const s32 r = static_cast<s32>(lv2_syscall(ctx, 521, {ps_read32(b, a + cfg::handle), service_id, user_id,
        verbosity, data, size, out}));
    cfg_unlock(ctx, a);
    return r;
}

s32 sys_config_unregister_service(PPUContext& ctx, u32 service)
{
    u8* b = guest_base(ctx);
    const u32 a = cfg_agent(ctx);
    cfg_lock(ctx, a);
    const s32 r = static_cast<s32>(lv2_syscall(ctx, 522, {ps_read32(b, a + cfg::handle), service}));
    cfg_unlock(ctx, a);
    return r;
}

// sys_config_register_io_error_handler(handler): one handler at a time.
s32 sys_config_register_io_error_handler(PPUContext& ctx, u32 handler)
{
    u8* b = guest_base(ctx);
    const u32 a = cfg_agent(ctx);
    cfg_lock(ctx, a);
    s32 r = error(cfg::CELL_EEXIST);
    if (!ps_read32(b, a + cfg::io_handler)) {
        r = static_cast<s32>(lv2_syscall(ctx, 524, {ps_read32(b, a + cfg::handle)}));
        if (r == lv2::CELL_OK) ps_write32(b, a + cfg::io_handler, handler);
    }
    cfg_unlock(ctx, a);
    return r;
}

s32 sys_config_unregister_io_error_handler(PPUContext& ctx)
{
    u8* b = guest_base(ctx);
    const u32 a = cfg_agent(ctx);
    cfg_lock(ctx, a);
    const s32 r = static_cast<s32>(lv2_syscall(ctx, 525, {ps_read32(b, a + cfg::handle)}));
    if (r == lv2::CELL_OK) ps_write32(b, a + cfg::io_handler, 0);
    cfg_unlock(ctx, a);
    return r;
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
        PS_HLE_FUNC(0x3AAAD464, cellPadGetInfo),
        PS_HLE_FUNC(0xDBF4C59C, cellPadGetCapabilityInfo),
        PS_HLE_FUNC(0x0D5F2C14, cellPadClearBuf),
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
        PS_HLE_FUNC(0x6BC09C61, cellPadGetDataExtra),
        PS_HLE_FUNC(0x3733EA3C, sys_io_3733EA3C),
        PS_HLE_FUNC(0x3F797DFF, cellPadGetRawData),
        PS_HLE_FUNC(0x0E2DFAAD, cellPadInfoPressMode),
        PS_HLE_FUNC(0x78200559, cellPadInfoSensorMode),
        PS_HLE_FUNC(0xF83F8182, cellPadSetPressMode),
        PS_HLE_FUNC(0xBE5BE3BA, cellPadSetSensorMode),
        PS_HLE_FUNC(0x20A97BA2, cellPadLddRegisterController),
        PS_HLE_FUNC(0xE442FAA8, cellPadLddUnregisterController),
        PS_HLE_FUNC(0xBAFD6409, cellPadLddDataInsert),
        PS_HLE_FUNC(0x8B8231E5, cellPadLddGetPortNo),
        PS_HLE_FUNC(0x4CC9B68D, cellPadPeriphGetInfo),
        PS_HLE_FUNC(0x8A00F264, cellPadPeriphGetData),
        PS_HLE_FUNC(0x610BF503, cellPadFilterIIRInit),
        PS_HLE_FUNC(0x3699D0D2, cellPadFilterIIRFilter),
        PS_HLE_FUNC(0x1F71ECBE, cellKbGetConfiguration),
        PS_HLE_FUNC(0x4AB1FA77, cellKbCnvRawCode),
        PS_HLE_FUNC(0xC9030138, cellMouseInit),
        PS_HLE_FUNC(0xE10183CE, cellMouseEnd),
        PS_HLE_FUNC(0x5BAF30FB, cellMouseGetInfo),
        PS_HLE_FUNC(0x3EF66B95, cellMouseClearBuf),
        PS_HLE_FUNC(0x3138E632, cellMouseGetData),
        PS_HLE_FUNC(0x6BD131F0, cellMouseGetDataList),
        PS_HLE_FUNC(0x21A62E9B, cellMouseGetTabletDataList),
        PS_HLE_FUNC(0xA328CC35, cellMouseGetRawData),
        PS_HLE_FUNC(0x4D0B3B1F, cellMouseInfoTabletMode),
        PS_HLE_FUNC(0x2D16DA4F, cellMouseSetTabletMode),
        PS_HLE_FUNC(0x068FCBC6, sys_config_start),
        PS_HLE_FUNC(0x6D367953, sys_config_stop),
        PS_HLE_FUNC(0x6AE10596, sys_config_add_service_listener),
        PS_HLE_FUNC(0xF5D9D571, sys_config_remove_service_listener),
        PS_HLE_FUNC(0x78F058A2, sys_config_register_service),
        PS_HLE_FUNC(0x5F81900C, sys_config_unregister_service),
        PS_HLE_FUNC(0x662BB2AD, sys_config_register_io_error_handler),
        PS_HLE_FUNC(0x8E06D13B, sys_config_unregister_io_error_handler),
        ::pspocket::hle::Function{{}, "_pspocket_sys_config_event_thread",
            recomp::nid_of("_pspocket_sys_config_event_thread"),
            &::pspocket::hle::detail::thunk<&_pspocket_sys_config_event_thread>, true},
    });
}

} // namespace pspocket::hle
