// libgem (cellGem): PlayStation Move motion controllers with none connected, RPCS3's default ("Move: Null") and its
// camera default (none). Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellGem.cpp/.h (gem_config_data, the functions along
// the paths the null handlers take) and Input/ps_move_tracker.cpp (hsv_to_rgb, rgb_to_hsv).
//
// The library initialises and keeps each controller's settings (sphere colour, hue, rumble, calibration, filtering),
// but no controller is ever ready: the state functions answer CELL_GEM_NOT_CONNECTED (an informational value, not an
// error) and the inertial state INVALID_PARAMETER, as in RPCS3. Calibration completes 0.5 s after cellGemCalibrate, as
// RPCS3's gem thread completes it for any handler; here it completes at the first call after that time. With no camera
// an update reports CELL_GEM_NO_VIDEO, and a video conversion writes nothing.
//
// Differences from RPCS3:
// - Video conversion: RPCS3's gem thread finishes a conversion at once when there is no camera, so a later
//   cellGemConvertVideoFinish usually finds none in progress (CONVERT_NOT_STARTED, a race). Here a started conversion
//   stays pending until cellGemConvertVideoFinish takes it, which then answers CELL_OK, as on a PS3.
// - Pointers are checked before use (D-042): one that is not guest memory is logged with the call site and refused
//   with INVALID_PARAMETER where RPCS3 would fault.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string_view>
#include <tuple>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellGem";

enum : u32 {
    CELL_GEM_ERROR_RESOURCE_ALLOCATION_FAILED = 0x80121801,
    CELL_GEM_ERROR_ALREADY_INITIALIZED = 0x80121802,
    CELL_GEM_ERROR_UNINITIALIZED = 0x80121803,
    CELL_GEM_ERROR_INVALID_PARAMETER = 0x80121804,
    CELL_GEM_ERROR_INVALID_ALIGNMENT = 0x80121805,
    CELL_GEM_ERROR_UPDATE_NOT_FINISHED = 0x80121806,
    CELL_GEM_ERROR_UPDATE_NOT_STARTED = 0x80121807,
    CELL_GEM_ERROR_CONVERT_NOT_FINISHED = 0x80121808,
    CELL_GEM_ERROR_CONVERT_NOT_STARTED = 0x80121809,
    CELL_GEM_ERROR_NOT_A_HUE = 0x8012180B,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// Informational results (CellGemStatus values that are not errors). With no controller ready, the state functions stop
// at NOT_CONNECTED before the calibration and hue results RPCS3 gives a connected one.
constexpr s32 CELL_GEM_NOT_CONNECTED = 1, CELL_GEM_NO_VIDEO = 6;
constexpr u32 CELL_GEM_MAX_NUM = 4, CELL_GEM_VERSION = 2, CELL_GEM_STATE_FLAG_TIMESTAMP = 2;
constexpr u32 CELL_GEM_DONT_TRACK_HUE = 2 << 24, CELL_GEM_DONT_CARE_HUE = 4 << 24;
constexpr u64 CELL_GEM_FLAG_CALIBRATION_OCCURRED = 1 << 0, CELL_GEM_FLAG_CALIBRATION_SUCCEEDED = 1 << 1;
constexpr s32 CELL_GEM_NO_VIDEO_OUTPUT = 1;
constexpr s32 CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME = 0x4;
constexpr u64 kCalibrationUs = 500'000; // RPCS3 gem_controller::calibration_time_us
constexpr u64 SYS_MEMORY_PAGE_SIZE_64K = 0x200;

// Guest structures (cellGem.h).
constexpr u32 kAttributeSize = 24;         // CellGemAttribute: version, max_connect, memory_ptr, spurs_addr, u8[8]
constexpr u32 kInfoSize = 40;              // CellGemInfo: max_connect, now_connect, status[4], port[4]
constexpr u32 kImageStateSize = 0x30;      // CellGemImageState
constexpr u32 kCameraStateSize = 20;       // CellGemCameraState
constexpr u32 kVideoConvertAttrSize = 0x28; // CellGemVideoConvertAttribute (with alpha and padding)

struct Color {
    f32 r = 0, g = 0, b = 0;
};

// RPCS3 gem_color: each channel clamped to [0, 1].
Color clamped(f32 r, f32 g, f32 b) { return {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f)}; }

// RPCS3 gem_color::get_default_color: green, gold, red, pink.
Color default_color(u32 gem_num)
{
    static constexpr Color colors[CELL_GEM_MAX_NUM] = {{0.0f, 1.0f, 0.0f}, {1.0f, 0.85f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {0.9f, 0.0f, 0.5f}};
    return colors[gem_num];
}

// RPCS3 gem_controller, the fields a controller that never connects uses.
struct Controller {
    bool enabled_magnetometer = true;
    bool calibrated_magnetometer = false;
    bool enabled_filtering = false;
    bool enabled_tracking = false;
    bool enabled_led = false;
    bool hue_set = false;
    u8 rumble = 0;
    Color sphere_rgb;
    u32 hue = 0;
    bool is_calibrating = false;
    u64 calibration_start_us = 0;
    u64 calibration_status_flags = 0;
};

struct GemState {
    std::mutex mutex;
    bool initialized = false;
    u32 max_connect = 0;
    u32 memory = 0; // what cellGemInit allocated (none when the game passed its own memory)
    u64 runtime_status_flags = 0;
    bool pitch_correction = false;
    std::array<Controller, CELL_GEM_MAX_NUM> controllers{};
    bool updating = false;
    u32 camera_frame = 0;
    bool converting = false;
    s32 video_data_out_size = -1;
    u64 start_timestamp_us = 0;
};

GemState& gem() { return HleProcess::current()->state<GemState>(); }

u64 now_us() { return lv2::guest_system_time_us(); }

bool check_gem_num(u32 gem_num) { return gem_num < CELL_GEM_MAX_NUM; }

// RPCS3 reset_controller with the null handler: defaults, the default sphere colour, not connected.
void reset_controller(GemState& s, u32 gem_num)
{
    s.controllers[gem_num] = Controller{};
    s.controllers[gem_num].sphere_rgb = default_color(gem_num);
}

// RPCS3 update_calibration_status (its gem thread runs it at 10 Hz): a calibration ends 0.5 s after it started.
void update_calibration(GemState& s)
{
    const u64 now = now_us();
    for (Controller& c : s.controllers) {
        if (!c.is_calibrating || now - c.calibration_start_us < kCalibrationUs) continue;
        c.is_calibrating = false;
        c.calibration_start_us = 0;
        c.calibration_status_flags = CELL_GEM_FLAG_CALIBRATION_SUCCEEDED | CELL_GEM_FLAG_CALIBRATION_OCCURRED;
        c.calibrated_magnetometer = true;
        c.enabled_tracking = true;
    }
}

// The state locked, initialised and calibrations brought up to date; a function's first steps.
struct Locked {
    GemState& s;
    std::lock_guard<std::mutex> lock;
    explicit Locked(GemState& state) : s(state), lock(state.mutex)
    {
        if (s.initialized) update_calibration(s);
    }
};

bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::string(what) + " is not guest memory");
    return false;
}

// RPCS3 ps_move_tracker::hsv_to_rgb: 8-bit channels.
std::tuple<u8, u8, u8> hsv_to_rgb(u16 hue, f32 saturation, f32 value)
{
    const f32 h = hue / 60.0f;
    const f32 chroma = value * saturation;
    const f32 x = chroma * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    const f32 m = value - chroma;
    f32 r = 0, g = 0, b = 0;
    switch (static_cast<int>(std::ceil(h))) {
    case 0:
    case 1: r = chroma + m, g = x + m, b = m; break;
    case 2: r = x + m, g = chroma + m, b = m; break;
    case 3: r = m, g = chroma + m, b = x + m; break;
    case 4: r = m, g = x + m, b = chroma + m; break;
    case 5: r = x + m, g = m, b = chroma + m; break;
    case 6: r = chroma + m, g = m, b = x + m; break;
    default: break;
    }
    const auto byte = [](f32 v) { return static_cast<u8>(std::clamp(std::round(v * 255.0f), 0.0f, 255.0f)); };
    return {byte(r), byte(g), byte(b)};
}

// RPCS3 ps_move_tracker::rgb_to_hsv's hue.
s16 rgb_hue(f32 r, f32 g, f32 b)
{
    const f32 cmax = std::max({r, g, b}), cmin = std::min({r, g, b}), delta = cmax - cmin;
    if (delta == 0.0f) return 0;
    if (cmax == r) return static_cast<s16>((static_cast<s16>(60.0f * (g - b) / delta) + 360) % 360);
    if (cmax == g) return static_cast<s16>((static_cast<s16>(60.0f * (b - r) / delta) + 120 + 360) % 360);
    return static_cast<s16>((static_cast<s16>(60.0f * (r - g) / delta) + 240 + 360) % 360);
}

// RPCS3 cellGemGetVideoConvertSize.
s32 video_convert_size(s32 format)
{
    switch (format) {
    case 6: return 320 * 240 * 4;                           // RGBA_320x240
    case 2: return 640 * 480 * 4;                           // RGBA_640x480
    case 3: return 640 * 480 * 3;                           // YUV_640x480
    case 4: return 640 * 480 + 320 * 480 + 320 * 480;       // YUV422_640x480
    case 5: return 640 * 480 + 320 * 240 + 320 * 240;       // YUV411_640x480
    case 7:
    case 8:
    case 9: return 640 * 480;                               // the restored Bayer outputs
    case CELL_GEM_NO_VIDEO_OUTPUT: return 0;
    default: return -1;
    }
}

u32 memory_size(u32 max_connect) { return max_connect <= 2 ? 0x120000 : 0x140000; }

// ------------------------------------------------------------------------------------------------- init and end

// cellGemInit(const CellGemAttribute*): SPURS and 1 to 4 controllers wanted; the library's memory is allocated when
// the game gives none.
s32 cellGemInit(PPUContext& ctx, u32 attribute)
{
    if (!attribute) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, attribute, kAttributeSize, "cellGemInit: the CellGemAttribute")) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    u8* base = guest_base(ctx);
    const u32 max_connect = ps_read32(base, attribute + 4);
    const u32 memory_ptr = ps_read32(base, attribute + 8);
    const u32 spurs = ps_read32(base, attribute + 12);
    if (!spurs || !max_connect || max_connect > CELL_GEM_MAX_NUM) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    GemState& s = gem();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(CELL_GEM_ERROR_ALREADY_INITIALIZED);
    s.memory = 0;
    if (!memory_ptr) {
        StackVar addr(ctx, base, 4);
        if (lv2::sys_memory_allocate(thread_of(ctx), memory_size(max_connect), SYS_MEMORY_PAGE_SIZE_64K, addr.addr()) !=
            lv2::CELL_OK) {
            return error(CELL_GEM_ERROR_RESOURCE_ALLOCATION_FAILED);
        }
        s.memory = ps_read32(base, addr.addr());
    }
    s.initialized = true;
    s.max_connect = max_connect;
    s.updating = false;
    s.camera_frame = 0;
    s.converting = false;
    s.runtime_status_flags = 0;
    for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++) reset_controller(s, i);
    s.start_timestamp_us = now_us();
    PS_LOG_INFO(kChan, "cellGemInit(max_connect {}): no Move controller is connected", max_connect);
    return lv2::CELL_OK;
}

s32 cellGemEnd(PPUContext& ctx)
{
    GemState& s = gem();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) {
        s.updating = false;
        return error(CELL_GEM_ERROR_UNINITIALIZED);
    }
    s.initialized = false;
    if (const u32 addr = std::exchange(s.memory, 0)) lv2::sys_memory_free(thread_of(ctx), addr);
    return lv2::CELL_OK;
}

s32 cellGemGetMemorySize(PPUContext&, s32 max_connect)
{
    if (max_connect > static_cast<s32>(CELL_GEM_MAX_NUM) || max_connect <= 0) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    return static_cast<s32>(memory_size(static_cast<u32>(max_connect)));
}

// cellGemGetInfo(CellGemInfo*): no controller connected; every status DISCONNECTED and port 0.
s32 cellGemGetInfo(PPUContext& ctx, u32 info)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!info) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, info, kInfoSize, "cellGemGetInfo: the CellGemInfo")) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    u8* base = guest_base(ctx);
    std::memset(base + info, 0, kInfoSize);
    ps_write32(base, info, l.s.max_connect);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- per controller

// The checks most per-controller functions share: initialised, then a controller number below 4.
s32 controller_checks(GemState& s, u32 gem_num)
{
    if (!s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num)) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    return lv2::CELL_OK;
}

s32 cellGemCalibrate(PPUContext&, u32 gem_num)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, gem_num); r != lv2::CELL_OK) return r;
    Controller& c = l.s.controllers[gem_num];
    if (c.is_calibrating) return lv2::err(lv2::CELL_EBUSY);
    c.is_calibrating = true;
    c.calibration_start_us = now_us();
    return lv2::CELL_OK;
}

s32 cellGemInvalidateCalibration(PPUContext&, s32 gem_num)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, static_cast<u32>(gem_num)); r != lv2::CELL_OK) return r;
    Controller& c = l.s.controllers[static_cast<u32>(gem_num)];
    c.calibrated_magnetometer = false;
    c.is_calibrating = false;
    c.calibration_start_us = 0;
    c.calibration_status_flags = 0;
    c.hue_set = false;
    c.enabled_tracking = false;
    return lv2::CELL_OK;
}

s32 cellGemClearStatusFlags(PPUContext&, u32 gem_num, u64 mask)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, gem_num); r != lv2::CELL_OK) return r;
    l.s.controllers[gem_num].calibration_status_flags &= ~mask;
    return lv2::CELL_OK;
}

s32 cellGemGetStatusFlags(PPUContext& ctx, u32 gem_num, u32 flags)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num) || !flags) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, flags, 8, "cellGemGetStatusFlags: flags")) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    ps_write64(guest_base(ctx), flags, l.s.runtime_status_flags | l.s.controllers[gem_num].calibration_status_flags);
    return lv2::CELL_OK;
}

// The magnetometer needs a connected controller (never, here).
s32 cellGemEnableMagnetometer(PPUContext&, u32 gem_num, u32)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, gem_num); r != lv2::CELL_OK) return r;
    return CELL_GEM_NOT_CONNECTED;
}

s32 cellGemEnableMagnetometer2(PPUContext&, u32 gem_num, u32)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, gem_num); r != lv2::CELL_OK) return r;
    return CELL_GEM_NOT_CONNECTED;
}

s32 cellGemEnableCameraPitchAngleCorrection(PPUContext&, u32 enable)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    l.s.pitch_correction = enable != 0;
    return lv2::CELL_OK;
}

s32 cellGemFilterState(PPUContext&, u32 gem_num, u32 enable)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, gem_num); r != lv2::CELL_OK) return r;
    l.s.controllers[gem_num].enabled_filtering = enable != 0;
    return lv2::CELL_OK;
}

// cellGemForceRGB(gem_num, float r, g, b) (f1..f3): the sphere's colour; tracking stops; the hue follows the colour.
s32 cellGemForceRGB(PPUContext& ctx, u32 gem_num)
{
    const f32 r = static_cast<f32>(ctx.fpr[1]), g = static_cast<f32>(ctx.fpr[2]), b = static_cast<f32>(ctx.fpr[3]);
    Locked l(gem());
    if (const s32 res = controller_checks(l.s, gem_num); res != lv2::CELL_OK) return res;
    Controller& c = l.s.controllers[gem_num];
    c.sphere_rgb = clamped(r, g, b);
    c.enabled_tracking = false;
    c.hue = static_cast<u32>(static_cast<s32>(rgb_hue(r, g, b)));
    return lv2::CELL_OK;
}

// cellGemGetRGB(gem_num, float* r, float* g, float* b).
s32 cellGemGetRGB(PPUContext& ctx, u32 gem_num, u32 r, u32 g, u32 b)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num) || !r || !g || !b) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, r, 4, "cellGemGetRGB: r") || !usable(ctx, g, 4, "cellGemGetRGB: g") ||
        !usable(ctx, b, 4, "cellGemGetRGB: b"))
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    u8* base = guest_base(ctx);
    const Color& c = l.s.controllers[gem_num].sphere_rgb;
    ps_writef32(base, r, c.r);
    ps_writef32(base, g, c.g);
    ps_writef32(base, b, c.b);
    return lv2::CELL_OK;
}

s32 cellGemSetRumble(PPUContext&, u32 gem_num, u8 rumble)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, gem_num); r != lv2::CELL_OK) return r;
    l.s.controllers[gem_num].rumble = rumble;
    return lv2::CELL_OK;
}

s32 cellGemGetRumble(PPUContext& ctx, u32 gem_num, u32 rumble)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num) || !rumble) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, rumble, 1, "cellGemGetRumble: rumble")) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    ps_write8(guest_base(ctx), rumble, l.s.controllers[gem_num].rumble);
    return lv2::CELL_OK;
}

// cellGemSetYaw(gem_num, vec_float4 z_direction): only a real controller takes it (RPCS3).
s32 cellGemSetYaw(PPUContext&, u32 gem_num)
{
    Locked l(gem());
    return controller_checks(l.s, gem_num);
}

s32 cellGemReset(PPUContext&, u32 gem_num)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, gem_num); r != lv2::CELL_OK) return r;
    reset_controller(l.s, gem_num);
    l.s.start_timestamp_us = now_us();
    return lv2::CELL_OK;
}

s32 cellGemGetTrackerHue(PPUContext& ctx, u32 gem_num, u32 hue)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num) || !hue) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    const Controller& c = l.s.controllers[gem_num];
    if (!c.enabled_tracking || c.hue > 359) return error(CELL_GEM_ERROR_NOT_A_HUE);
    if (!usable(ctx, hue, 4, "cellGemGetTrackerHue: hue")) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    ps_write32(guest_base(ctx), hue, c.hue);
    return lv2::CELL_OK;
}

// cellGemTrackHues(const u32 req_hues[4], u32 res_hues[4]): per controller, a hue (the sphere lit in it), DONT_CARE
// (RPCS3's pick per controller: blue, red, green, purple) or DONT_TRACK.
s32 cellGemTrackHues(PPUContext& ctx, u32 req, u32 res)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!req) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, req, 16, "cellGemTrackHues: req_hues") || (res && !usable(ctx, res, 16, "cellGemTrackHues: res_hues"))) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    u8* base = guest_base(ctx);
    static constexpr u32 kDontCareHues[CELL_GEM_MAX_NUM] = {240, 0, 120, 300};
    for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++) {
        Controller& c = l.s.controllers[i];
        const u32 want = ps_read32(base, req + 4 * i);
        if (want == CELL_GEM_DONT_TRACK_HUE) {
            c.enabled_tracking = c.enabled_led = c.hue_set = false;
            if (res) ps_write32(base, res + 4 * i, CELL_GEM_DONT_TRACK_HUE);
            continue;
        }
        if (want != CELL_GEM_DONT_CARE_HUE && want > 359) {
            PS_LOG_WARN(kChan, "cellGemTrackHues: controller {} asks for hue {} (kept, as RPCS3)", i, want);
        }
        c.enabled_tracking = c.enabled_led = c.hue_set = true;
        c.hue = want == CELL_GEM_DONT_CARE_HUE ? kDontCareHues[i] : want;
        const auto [r, g, b] = hsv_to_rgb(static_cast<u16>(c.hue), 1.0f, 1.0f);
        c.sphere_rgb = clamped(r / 255.0f, g / 255.0f, b / 255.0f);
        if (res) ps_write32(base, res + 4 * i, c.hue);
    }
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- states

// cellGemGetState(gem_num, flag, time_parameter, CellGemState*): no controller is ready.
s32 cellGemGetState(PPUContext&, u32 gem_num, u32 flag, u64, u32 state)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num) || flag > CELL_GEM_STATE_FLAG_TIMESTAMP || !state) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    return CELL_GEM_NOT_CONNECTED;
}

// RPCS3 checks readiness with the parameters: a controller that isn't ready is INVALID_PARAMETER here.
s32 cellGemGetInertialState(PPUContext&, u32 gem_num, u32, u64, u32 state)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    (void)gem_num;
    (void)state;
    return error(CELL_GEM_ERROR_INVALID_PARAMETER);
}

// cellGemGetImageState(gem_num, CellGemImageState*): all zero (nothing seen) with the null handler.
s32 cellGemGetImageState(PPUContext& ctx, u32 gem_num, u32 state)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num) || !state) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, state, kImageStateSize, "cellGemGetImageState: the CellGemImageState")) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    std::memset(guest_base(ctx) + state, 0, kImageStateSize);
    return lv2::CELL_OK;
}

// The external port (Sharp Shooter, racing wheel): needs a connected controller.
s32 cellGemReadExternalPortDeviceInfo(PPUContext&, u32 gem_num, u32 ext_id, u32)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num) || !ext_id) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    return CELL_GEM_NOT_CONNECTED;
}

s32 cellGemWriteExternalPort(PPUContext&, u32 gem_num, u32)
{
    Locked l(gem());
    if (const s32 r = controller_checks(l.s, gem_num); r != lv2::CELL_OK) return r;
    return CELL_GEM_NOT_CONNECTED;
}

s32 cellGemGetAccelerometerPositionInDevice(PPUContext& ctx, u32 gem_num, u32 pos)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!check_gem_num(gem_num) || !pos) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, pos, 16, "cellGemGetAccelerometerPositionInDevice: pos")) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    std::memset(guest_base(ctx) + pos, 0, 16);
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- camera and hues

// cellGemGetCameraState(CellGemCameraState*): RPCS3's fixed answer, with no init check.
s32 cellGemGetCameraState(PPUContext& ctx, u32 state)
{
    if (!state) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, state, kCameraStateSize, "cellGemGetCameraState: the CellGemCameraState")) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    u8* base = guest_base(ctx);
    ps_write32(base, state + 0, 0);              // exposure
    ps_writef32(base, state + 4, 1.0f / 60.0f);  // exposure_time
    ps_writef32(base, state + 8, 1.0f);          // gain
    ps_writef32(base, state + 12, 0.0f);         // pitch_angle
    ps_writef32(base, state + 16, 0.0f);         // pitch_angle_estimate
    return lv2::CELL_OK;
}

// cellGemPrepareCamera(max_exposure, float image_quality): RPCS3 sets the camera's attributes, which fail quietly
// with no camera.
s32 cellGemPrepareCamera(PPUContext&, s32)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    return lv2::CELL_OK;
}

s32 cellGemGetEnvironmentLightingColor(PPUContext& ctx, u32 r, u32 g, u32 b)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!r || !g || !b) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, r, 4, "r") || !usable(ctx, g, 4, "g") || !usable(ctx, b, 4, "b")) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    u8* base = guest_base(ctx);
    for (const u32 p : {r, g, b}) ps_writef32(base, p, 128.0f); // RPCS3
    return lv2::CELL_OK;
}

// cellGemGetHuePixels(camera_frame, hue, u8 pixels[640 * 480]): no pixel has the hue.
s32 cellGemGetHuePixels(PPUContext& ctx, u32 frame, u32 hue, u32 pixels)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!frame || !pixels || hue > 359) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, pixels, 640 * 480, "cellGemGetHuePixels: pixels")) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    std::memset(guest_base(ctx) + pixels, 0, 640 * 480);
    return lv2::CELL_OK;
}

// Every hue is trackable without a real controller's tracker (RPCS3 hue_is_trackable).
s32 cellGemGetAllTrackableHues(PPUContext& ctx, u32 hues)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!hues) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, hues, 360, "cellGemGetAllTrackableHues: hues")) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    std::memset(guest_base(ctx) + hues, 1, 360);
    return lv2::CELL_OK;
}

s32 cellGemIsTrackableHue(PPUContext&, u32 hue)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (hue > 359) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    return 1;
}

// cellGemHSVtoRGB(float h, s, v, float* r, g, b): h in f1, s in f2, v in f3; the pointers in r3 to r5.
s32 cellGemHSVtoRGB(PPUContext& ctx, u32 r, u32 g, u32 b)
{
    f32 h = static_cast<f32>(ctx.fpr[1]);
    const f32 s = std::clamp(static_cast<f32>(ctx.fpr[2]), 0.0f, 1.0f);
    const f32 v = std::clamp(static_cast<f32>(ctx.fpr[3]), 0.0f, 1.0f);
    if (!r || !g || !b) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, r, 4, "r") || !usable(ctx, g, 4, "g") || !usable(ctx, b, 4, "b")) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    const f32 c = v * s;
    const f32 x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const f32 m = v - c;
    f32 rt = 0, gt = 0, bt = 0;
    if (h < 60.0f) rt = c, gt = x;
    else if (h < 120.0f) rt = x, gt = c;
    else if (h < 180.0f) gt = c, bt = x;
    else if (h < 240.0f) gt = x, bt = c;
    else if (h < 300.0f) rt = x, bt = c;
    else rt = c, bt = x;
    u8* base = guest_base(ctx);
    ps_writef32(base, r, std::clamp(rt + m, 0.0f, 1.0f));
    ps_writef32(base, g, std::clamp(gt + m, 0.0f, 1.0f));
    ps_writef32(base, b, std::clamp(bt + m, 0.0f, 1.0f));
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- updates, video

// cellGemUpdateStart(camera_frame, timestamp): with no camera there is no image (CELL_GEM_NO_VIDEO). RPCS3 marks the
// update started before it checks the frame's alignment, so a misaligned frame still needs cellGemUpdateFinish.
s32 cellGemUpdateStart(PPUContext&, u32 frame, u64)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (std::exchange(l.s.updating, true)) return error(CELL_GEM_ERROR_UPDATE_NOT_FINISHED);
    if (frame & 127) return error(CELL_GEM_ERROR_INVALID_ALIGNMENT);
    l.s.camera_frame = frame;
    return CELL_GEM_NO_VIDEO;
}

s32 cellGemUpdateFinish(PPUContext&)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!l.s.updating) return error(CELL_GEM_ERROR_UPDATE_NOT_STARTED);
    l.s.updating = false;
    return l.s.camera_frame ? lv2::CELL_OK : CELL_GEM_NO_VIDEO;
}

// cellGemPrepareVideoConvert(const CellGemVideoConvertAttribute*): RPCS3's checks; the output size is kept.
s32 cellGemPrepareVideoConvert(PPUContext& ctx, u32 attr)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!attr) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (!usable(ctx, attr, kVideoConvertAttrSize, "cellGemPrepareVideoConvert: the attribute")) {
        return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    }
    u8* base = guest_base(ctx);
    const s32 version = static_cast<s32>(ps_read32(base, attr));
    const s32 format = static_cast<s32>(ps_read32(base, attr + 4));
    const s32 flags = static_cast<s32>(ps_read32(base, attr + 8));
    const u32 buffer_memory = ps_read32(base, attr + 28), video_data_out = ps_read32(base, attr + 32);
    if (version != static_cast<s32>(CELL_GEM_VERSION)) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (format != CELL_GEM_NO_VIDEO_OUTPUT && !video_data_out) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if ((flags & CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME) && !buffer_memory) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if ((video_data_out & 127) || (buffer_memory & 15)) return error(CELL_GEM_ERROR_INVALID_ALIGNMENT);
    l.s.video_data_out_size = video_convert_size(format);
    return lv2::CELL_OK;
}

// cellGemConvertVideoStart(video_frame) / Finish(): with no camera the conversion writes nothing (RPCS3).
s32 cellGemConvertVideoStart(PPUContext&, u32 frame)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!frame) return error(CELL_GEM_ERROR_INVALID_PARAMETER);
    if (frame & 127) return error(CELL_GEM_ERROR_INVALID_ALIGNMENT);
    if (l.s.converting) return error(CELL_GEM_ERROR_CONVERT_NOT_FINISHED);
    l.s.converting = true;
    return lv2::CELL_OK;
}

s32 cellGemConvertVideoFinish(PPUContext&)
{
    Locked l(gem());
    if (!l.s.initialized) return error(CELL_GEM_ERROR_UNINITIALIZED);
    if (!l.s.converting) return error(CELL_GEM_ERROR_CONVERT_NOT_STARTED);
    l.s.converting = false;
    return lv2::CELL_OK;
}

} // namespace

void register_gem(Registry& r)
{
    r.add("libgem", {
        PS_HLE_FUNC(0x13EA7C64, cellGemInit),
        PS_HLE_FUNC(0xE1F85A80, cellGemEnd),
        PS_HLE_FUNC(0x2E0A170D, cellGemGetMemorySize),
        PS_HLE_FUNC(0x9E1DFF96, cellGemGetInfo),
        PS_HLE_FUNC(0xAFA99EAD, cellGemCalibrate),
        PS_HLE_FUNC(0xE3E4F0D6, cellGemInvalidateCalibration),
        PS_HLE_FUNC(0x9B9714A4, cellGemClearStatusFlags),
        PS_HLE_FUNC(0xFEE33481, cellGemGetStatusFlags),
        PS_HLE_FUNC(0x1A2518A2, cellGemEnableMagnetometer),
        PS_HLE_FUNC(0xE2E65CFC, cellGemEnableMagnetometer2),
        PS_HLE_FUNC(0x4219DE31, cellGemEnableCameraPitchAngleCorrection),
        PS_HLE_FUNC(0x6FC4C791, cellGemFilterState),
        PS_HLE_FUNC(0xCE6D7791, cellGemForceRGB),
        PS_HLE_FUNC(0x1B30CC22, cellGemGetRGB),
        PS_HLE_FUNC(0x49609306, cellGemSetRumble),
        PS_HLE_FUNC(0x6DB6B007, cellGemGetRumble),
        PS_HLE_FUNC(0x77E08704, cellGemSetYaw),
        PS_HLE_FUNC(0xDE54E2FC, cellGemReset),
        PS_HLE_FUNC(0x18EA899A, cellGemGetTrackerHue),
        PS_HLE_FUNC(0x928AC5F8, cellGemTrackHues),
        PS_HLE_FUNC(0x6441D38D, cellGemGetState),
        PS_HLE_FUNC(0xD37B127A, cellGemGetInertialState),
        PS_HLE_FUNC(0x92CC4B34, cellGemGetImageState),
        PS_HLE_FUNC(0x11077159, cellGemReadExternalPortDeviceInfo),
        PS_HLE_FUNC(0x1F6328D8, cellGemWriteExternalPort),
        PS_HLE_FUNC(0x6A5B7048, cellGemGetAccelerometerPositionInDevice),
        PS_HLE_FUNC(0x8BEFAC67, cellGemGetCameraState),
        PS_HLE_FUNC(0xA03EF587, cellGemPrepareCamera),
        PS_HLE_FUNC(0x02EB41BB, cellGemGetEnvironmentLightingColor),
        PS_HLE_FUNC(0xB8EF56A6, cellGemGetHuePixels),
        PS_HLE_FUNC(0x2D2C2764, cellGemGetAllTrackableHues),
        PS_HLE_FUNC(0xFB5887F9, cellGemIsTrackableHue),
        PS_HLE_FUNC(0xC7622586, cellGemHSVtoRGB),
        PS_HLE_FUNC(0x0ECD2261, cellGemUpdateStart),
        PS_HLE_FUNC(0x41AE9C31, cellGemUpdateFinish),
        PS_HLE_FUNC(0xC07896F9, cellGemPrepareVideoConvert),
        PS_HLE_FUNC(0x6DCE048C, cellGemConvertVideoStart),
        PS_HLE_FUNC(0x1A13D010, cellGemConvertVideoFinish),
    });
}

} // namespace pspocket::hle
