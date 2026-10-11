// The PlayStation Eye camera (cellCamera) and microphones (cellMic) with no device attached: RPCS3's default
// configuration (camera "Null", microphone type "Null"). Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellCamera.cpp/.h and
// cellMic.cpp/.h, the paths each function takes with those handlers.
//
// Camera: cellCameraInit succeeds and nothing is ever attached or opened. Open finds no device after checking its
// arguments; OpenEx, GetType and GetBufferSize answer DEVICE_NOT_FOUND, the attribute and read functions NOT_OPEN
// (RPCS3's answers with the null handler, which it returns as values rather than errors); the Is* queries answer 0.
// Notify queues are accepted and get no events. The asynchronous forms (D-665) are refused by their synchronous forms'
// checks, so no completion event is ever sent and no request waits for its Post; Read2 and ResetAttribute answer as the
// read and attribute functions, SetContainer keeps its container (D-666).
//
// Microphone: cellMicInit succeeds with no microphone registered, so every function on a device answers
// DEVICE_NOT_FOUND after RPCS3's argument checks. As in RPCS3, cellMicIsAttached answers 1 regardless, cellMicGetType
// reports a USB microphone for devices 0 to 7 (RPCS3's default type, which Guitar Hero: Warriors of Rock needs) and
// cellMicGetStatus a closed device with RPCS3's fixed levels. A notify queue must exist (by key); it gets no attach
// events, since no microphone is registered.
//
// Pointers are checked before use (D-042): where RPCS3 writes through a pointer unchecked, one that is not guest
// memory is logged with the call site and refused with the library's PARAM error.

#include <mutex>
#include <set>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"

namespace pspocket::hle {
namespace {

constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::string(what) + " is not guest memory");
    return false;
}

// ------------------------------------------------------------------------------------------------------ cellCamera

constexpr std::string_view kCameraChan = "cellCamera";

enum : u32 {
    CELL_CAMERA_ERROR_ALREADY_INIT = 0x80140801,
    CELL_CAMERA_ERROR_NOT_INIT = 0x80140803,
    CELL_CAMERA_ERROR_PARAM = 0x80140804,
    CELL_CAMERA_ERROR_NOT_OPEN = 0x80140806,
    CELL_CAMERA_ERROR_DEVICE_NOT_FOUND = 0x80140807,
    CELL_CAMERA_ERROR_FORMAT_UNKNOWN = 0x8014080a,
    CELL_CAMERA_ERROR_RESOLUTION_UNKNOWN = 0x8014080b,
    CELL_CAMERA_ERROR_BAD_FRAMERATE = 0x8014080c,
};
constexpr s32 CELL_CAMERA_FORMAT_UNKNOWN = 0, CELL_CAMERA_RESOLUTION_UNKNOWN = 0;
// CellCameraInfo / CellCameraInfoEx start with be s32 format, resolution, framerate.
constexpr u32 kCameraInfoChecked = 12;

struct CameraState {
    std::mutex mutex;
    bool init = false;
    // cellCameraSetContainer's arguments (D-666): kept for an open, which never comes with no camera attached.
    bool container_set = false;
    s32 container_dev = 0;
    u32 container = 0;
    std::set<std::string_view> logged; // the functions whose "no camera" answer was logged once
};

CameraState& camera() { return HleProcess::current()->state<CameraState>(); }

bool camera_init()
{
    CameraState& c = camera();
    std::lock_guard lock(c.mutex);
    return c.init;
}

// A function's answer for want of a camera, logged the first time each function gives it.
s32 no_camera(std::string_view call, s32 r)
{
    CameraState& c = camera();
    std::lock_guard lock(c.mutex);
    if (c.logged.insert(call).second)
        PS_LOG_INFO(kCameraChan, "{}: no camera is attached -> {:#010x}", call, static_cast<u32>(r));
    return r;
}

s32 cellCameraInit(PPUContext&)
{
    CameraState& c = camera();
    std::lock_guard lock(c.mutex);
    if (c.init) return error(CELL_CAMERA_ERROR_ALREADY_INIT);
    c.init = true;
    PS_LOG_INFO(kCameraChan, "cellCameraInit(): no camera is attached");
    return lv2::CELL_OK;
}

s32 cellCameraEnd(PPUContext&)
{
    CameraState& c = camera();
    std::lock_guard lock(c.mutex);
    if (!c.init) return error(CELL_CAMERA_ERROR_NOT_INIT);
    c.init = false;
    return lv2::CELL_OK;
}

// RPCS3 check_camera_info, for its default camera type (unknown): the format, resolution and frame rate only.
s32 check_camera_info(u8* base, u32 info)
{
    if (static_cast<s32>(ps_read32(base, info)) == CELL_CAMERA_FORMAT_UNKNOWN) {
        return error(CELL_CAMERA_ERROR_FORMAT_UNKNOWN);
    }
    if (static_cast<s32>(ps_read32(base, info + 4)) == CELL_CAMERA_RESOLUTION_UNKNOWN) {
        return error(CELL_CAMERA_ERROR_RESOLUTION_UNKNOWN);
    }
    if (static_cast<s32>(ps_read32(base, info + 8)) <= 24) return error(CELL_CAMERA_ERROR_BAD_FRAMERATE);
    return lv2::CELL_OK;
}

// RPCS3 cellCameraOpen's checks before it looks for the camera: the info pointer, init, the device number, then the
// info's format, resolution and frame rate (`what` names the info in the log when it is not guest memory).
s32 open_checks(PPUContext& ctx, s32 dev, u32 info, std::string_view what)
{
    if (!info) return error(CELL_CAMERA_ERROR_PARAM);
    if (!camera_init()) return error(CELL_CAMERA_ERROR_NOT_INIT);
    if (dev != 0) return error(CELL_CAMERA_ERROR_PARAM);
    if (!usable(ctx, info, kCameraInfoChecked, what)) return error(CELL_CAMERA_ERROR_PARAM);
    return check_camera_info(guest_base(ctx), info);
}

// cellCameraOpen(dev_num, CellCameraInfo*): the arguments are checked, then no device is found.
s32 cellCameraOpen(PPUContext& ctx, s32 dev, u32 info)
{
    if (const s32 r = open_checks(ctx, dev, info, "cellCameraOpen: the CellCameraInfo"); r != lv2::CELL_OK) return r;
    PS_LOG_INFO(kCameraChan, "cellCameraOpen({}): no camera is attached", dev);
    return error(CELL_CAMERA_ERROR_DEVICE_NOT_FOUND);
}

// RPCS3 cellCameraOpenEx with the null handler: DEVICE_NOT_FOUND once the info pointer is there (no init check).
s32 cellCameraOpenEx(PPUContext&, s32 dev, u32 info)
{
    if (!info) return error(CELL_CAMERA_ERROR_PARAM);
    PS_LOG_INFO(kCameraChan, "cellCameraOpenEx({}): no camera is attached", dev);
    return error(CELL_CAMERA_ERROR_DEVICE_NOT_FOUND);
}

// RPCS3 check_init_and_open: the device number, then init, then an open camera (never, here).
s32 init_and_open(s32 dev)
{
    if (dev != 0) return error(CELL_CAMERA_ERROR_PARAM);
    if (!camera_init()) return error(CELL_CAMERA_ERROR_NOT_INIT);
    return error(CELL_CAMERA_ERROR_NOT_OPEN);
}

s32 cellCameraClose(PPUContext&, s32 dev) { return init_and_open(dev); }
s32 cellCameraReset(PPUContext&, s32 dev) { return init_and_open(dev); }
s32 cellCameraStart(PPUContext&, s32 dev) { return init_and_open(dev); }
s32 cellCameraStop(PPUContext&, s32 dev) { return init_and_open(dev); }

// The asynchronous forms (D-665): OpenAsync/OpenPost, CloseAsync/ClosePost, StartAsync/StartPost, StopAsync/StopPost,
// ResetAsync/ResetPost. RPCS3 registers their names only (UNIMPLEMENTED_FUNC answering CELL_OK, no arguments). In the
// SDK's model an Async call makes its synchronous form's checks and starts the operation; its end comes to the notify
// queues whose flag asks for it (CELL_CAMERA_EFLAG_OPEN 0x02, CLOSE 0x04, START 0x08, STOP 0x10, RESET 0x20 of
// cellCameraSetNotifyEventQueue2) as the event CELL_CAMERA_OPEN (3), CLOSE, START, STOP or RESET (7) (RPCS3
// cellCamera.h), and the Post call then completes it and answers its result. With no camera attached each Async call is
// refused by its synchronous form's checks, in their order (Open's arguments, then no camera: DEVICE_NOT_FOUND; Close,
// Start, Stop and Reset: the camera is never open, NOT_OPEN), so nothing is started, no event is sent and no request
// waits for its Post. A Post finds none and answers as its Async does from what it reads, the device number: what else
// each takes (OpenPost the info to fill, perhaps) is not known, so it is not read.

// cellCameraOpenAsync(dev_num, CellCameraInfoEx*): Open's checks (only the first three words of the info are read, the
// format, resolution and frame rate CellCameraInfo and CellCameraInfoEx share), then no camera.
s32 cellCameraOpenAsync(PPUContext& ctx, s32 dev, u32 info)
{
    if (const s32 r = open_checks(ctx, dev, info, "cellCameraOpenAsync: the CellCameraInfoEx"); r != lv2::CELL_OK)
        return r;
    return no_camera("cellCameraOpenAsync", error(CELL_CAMERA_ERROR_DEVICE_NOT_FOUND));
}

// cellCameraOpenPost(dev_num, ...): no open was started. Init and the device number in Open's order, then no camera.
s32 cellCameraOpenPost(PPUContext&, s32 dev)
{
    if (!camera_init()) return error(CELL_CAMERA_ERROR_NOT_INIT);
    if (dev != 0) return error(CELL_CAMERA_ERROR_PARAM);
    return no_camera("cellCameraOpenPost", error(CELL_CAMERA_ERROR_DEVICE_NOT_FOUND));
}

// The other Async and Post functions: RPCS3 check_init_and_open, as their synchronous forms; the camera is never open.
s32 on_open_camera(std::string_view call, s32 dev)
{
    const s32 r = init_and_open(dev);
    return r == error(CELL_CAMERA_ERROR_NOT_OPEN) ? no_camera(call, r) : r;
}

s32 cellCameraCloseAsync(PPUContext&, s32 dev) { return on_open_camera("cellCameraCloseAsync", dev); }
s32 cellCameraClosePost(PPUContext&, s32 dev) { return on_open_camera("cellCameraClosePost", dev); }
s32 cellCameraStartAsync(PPUContext&, s32 dev) { return on_open_camera("cellCameraStartAsync", dev); }
s32 cellCameraStartPost(PPUContext&, s32 dev) { return on_open_camera("cellCameraStartPost", dev); }
s32 cellCameraStopAsync(PPUContext&, s32 dev) { return on_open_camera("cellCameraStopAsync", dev); }
s32 cellCameraStopPost(PPUContext&, s32 dev) { return on_open_camera("cellCameraStopPost", dev); }
s32 cellCameraResetAsync(PPUContext&, s32 dev) { return on_open_camera("cellCameraResetAsync", dev); }
s32 cellCameraResetPost(PPUContext&, s32 dev) { return on_open_camera("cellCameraResetPost", dev); }

// cellCameraGetDeviceGUID(dev_num, u32* guid): always 0 (RPCS3).
s32 cellCameraGetDeviceGUID(PPUContext& ctx, s32, u32 guid)
{
    if (!camera_init()) return error(CELL_CAMERA_ERROR_NOT_INIT);
    if (guid) {
        if (!usable(ctx, guid, 4, "cellCameraGetDeviceGUID: guid")) return error(CELL_CAMERA_ERROR_PARAM);
        ps_write32(guest_base(ctx), guid, 0);
    }
    return lv2::CELL_OK;
}

// The functions that need a device: DEVICE_NOT_FOUND once initialised (RPCS3's null-handler answer).
s32 not_found_once_init()
{
    if (!camera_init()) return error(CELL_CAMERA_ERROR_NOT_INIT);
    return error(CELL_CAMERA_ERROR_DEVICE_NOT_FOUND);
}

s32 cellCameraGetType(PPUContext&, s32, u32) { return not_found_once_init(); }
s32 cellCameraGetBufferSize(PPUContext&, s32, u32) { return not_found_once_init(); }

// The attribute, buffer info and read functions: NOT_OPEN once initialised with a valid device number.
s32 not_open(s32 dev, bool dev_first)
{
    if (dev_first && dev != 0) return error(CELL_CAMERA_ERROR_PARAM);
    if (!camera_init()) return error(CELL_CAMERA_ERROR_NOT_INIT);
    if (dev != 0) return error(CELL_CAMERA_ERROR_PARAM);
    return error(CELL_CAMERA_ERROR_NOT_OPEN);
}

s32 cellCameraGetAttribute(PPUContext&, s32 dev, s32, u32, u32) { return not_open(dev, false); }
s32 cellCameraSetAttribute(PPUContext&, s32 dev, s32, u32, u32) { return not_open(dev, false); }
s32 cellCameraGetBufferInfo(PPUContext&, s32 dev, u32) { return not_open(dev, false); }
s32 cellCameraGetBufferInfoEx(PPUContext&, s32 dev, u32) { return not_open(dev, false); }
s32 cellCameraReadComplete(PPUContext&, s32 dev, u32, u32) { return not_open(dev, false); }

// cellCameraResetAttribute(dev_num, ...) (D-666): RPCS3 registers the name only. As Get/SetAttribute: init, the device
// number, then NOT_OPEN; what it takes past the device number (an attribute, perhaps) is not known and not read.
s32 cellCameraResetAttribute(PPUContext&, s32 dev)
{
    const s32 r = not_open(dev, false);
    return r == error(CELL_CAMERA_ERROR_NOT_OPEN) ? no_camera("cellCameraResetAttribute", r) : r;
}

// cellCameraReadEx and cellCameraRead: RPCS3 answers NOT_OPEN for the null handler before it checks the device number.
s32 cellCameraReadEx(PPUContext&, s32, u32)
{
    if (!camera_init()) return error(CELL_CAMERA_ERROR_NOT_INIT);
    return error(CELL_CAMERA_ERROR_NOT_OPEN);
}

s32 cellCameraRead(PPUContext& ctx, s32 dev, u32, u32) { return cellCameraReadEx(ctx, dev, 0); }

// cellCameraRead2(dev_num, ...) (D-666): RPCS3 registers the name only. It answers as Read and ReadEx do (NOT_OPEN once
// initialised, before the device number is looked at, RPCS3's null-handler order); what it takes past the device number
// is not known and not read.
s32 cellCameraRead2(PPUContext& ctx, s32 dev)
{
    const s32 r = cellCameraReadEx(ctx, dev, 0);
    return r == error(CELL_CAMERA_ERROR_NOT_OPEN) ? no_camera("cellCameraRead2", r) : r;
}

// The extension unit: RPCS3 checks the device number and pointer, then wants an attached (or open) camera.
s32 cellCameraPrepExtensionUnit(PPUContext&, s32 dev, u32 guid)
{
    if (dev != 0 || !guid) return error(CELL_CAMERA_ERROR_PARAM);
    return error(CELL_CAMERA_ERROR_DEVICE_NOT_FOUND);
}

s32 ctrl_extension_unit(s32 dev)
{
    if (dev != 0) return error(CELL_CAMERA_ERROR_PARAM);
    return error(CELL_CAMERA_ERROR_NOT_OPEN);
}

s32 cellCameraCtrlExtensionUnit(PPUContext&, s32 dev, u8, u16, u16, u32) { return ctrl_extension_unit(dev); }
s32 cellCameraGetExtensionUnit(PPUContext&, s32 dev, u16, u16, u32) { return ctrl_extension_unit(dev); }
s32 cellCameraSetExtensionUnit(PPUContext&, s32 dev, u16, u16, u32) { return ctrl_extension_unit(dev); }

// cellCameraSetContainer(dev_num, container) (D-666): RPCS3 registers the name only. Read as the memory container for
// the camera's buffers (CellCameraInfoEx's container at open): kept with the device number as given, CELL_OK,
// initialised or not (a game may hand it over before cellCameraInit). Nothing is checked: a check made on a wrong reading
// of the arguments would refuse a call games get CELL_OK for today. No camera ever opens to use it.
s32 cellCameraSetContainer(PPUContext&, s32 dev, u32 container)
{
    CameraState& c = camera();
    std::lock_guard lock(c.mutex);
    c.container_set = true;
    c.container_dev = dev;
    c.container = container;
    PS_LOG_INFO(kCameraChan, "cellCameraSetContainer({}, {:#x}): kept; no camera is attached to open with it", dev,
        container);
    return lv2::CELL_OK;
}

// Queries: no camera is available, attached, open or started.
s32 cellCameraIsAvailable(PPUContext&, s32) { return 0; }
s32 cellCameraIsAttached(PPUContext&, s32) { return 0; }
s32 cellCameraIsOpen(PPUContext&, s32) { return 0; }
s32 cellCameraIsStarted(PPUContext&, s32) { return 0; }

// Notify queues: accepted once initialised; with no camera they get no events (RPCS3 doesn't record them either).
s32 queue_once_init()
{
    if (!camera_init()) return error(CELL_CAMERA_ERROR_NOT_INIT);
    return lv2::CELL_OK;
}

s32 cellCameraSetNotifyEventQueue(PPUContext&, u64) { return queue_once_init(); }
s32 cellCameraRemoveNotifyEventQueue(PPUContext&, u64) { return queue_once_init(); }
s32 cellCameraSetNotifyEventQueue2(PPUContext&, u64, u64, u64) { return queue_once_init(); }
s32 cellCameraRemoveNotifyEventQueue2(PPUContext&, u64) { return queue_once_init(); }

// --------------------------------------------------------------------------------------------------------- cellMic

constexpr std::string_view kMicChan = "cellMic";

enum : u32 {
    CELL_MICIN_ERROR_ALREADY_INIT = 0x80140101,
    CELL_MICIN_ERROR_NOT_INIT = 0x80140103,
    CELL_MICIN_ERROR_PARAM = 0x80140104,
    CELL_MICIN_ERROR_EVENT_QUEUE = 0x8014010d,
    CELL_MICIN_ERROR_DEVICE_NOT_FOUND = 0x8014010e,
};
constexpr s32 CELL_MAX_MICS = 8;
constexpr s32 CELLMIC_TYPE_UNDEF = -1, CELLMIC_TYPE_USBAUDIO = 3;
constexpr u32 kMicStatusSize = 36; // CellMicStatus: seven be s32, two be f32

struct MicState {
    std::mutex mutex;
    bool init = false;
    u64 queue_key = 0; // the notify queue (no events: no microphone is registered)
    u64 queue_source = 0;
};

MicState& mic() { return HleProcess::current()->state<MicState>(); }

bool mic_init()
{
    MicState& m = mic();
    std::lock_guard lock(m.mutex);
    return m.init;
}

s32 cellMicInit(PPUContext&)
{
    MicState& m = mic();
    std::lock_guard lock(m.mutex);
    if (m.init) return error(CELL_MICIN_ERROR_ALREADY_INIT);
    m.init = true;
    PS_LOG_INFO(kMicChan, "cellMicInit(): no microphone is registered");
    return lv2::CELL_OK;
}

s32 cellMicEnd(PPUContext&)
{
    MicState& m = mic();
    std::lock_guard lock(m.mutex);
    if (!m.init) return error(CELL_MICIN_ERROR_NOT_INIT);
    m.init = false;
    m.queue_key = m.queue_source = 0;
    return lv2::CELL_OK;
}

// A function on device `dev`: NOT_INIT, else DEVICE_NOT_FOUND (no device is registered).
s32 no_mic()
{
    if (!mic_init()) return error(CELL_MICIN_ERROR_NOT_INIT);
    return error(CELL_MICIN_ERROR_DEVICE_NOT_FOUND);
}

s32 cellMicOpenEx(PPUContext&, s32, s32, s32, s32, s32, u8) { return no_mic(); }
s32 cellMicOpen(PPUContext&, s32, s32) { return no_mic(); }
s32 cellMicOpenRaw(PPUContext&, s32, s32, s32) { return no_mic(); }
s32 cellMicClose(PPUContext&, s32) { return no_mic(); }
s32 cellMicStart(PPUContext&, s32) { return no_mic(); }
s32 cellMicStop(PPUContext&, s32) { return no_mic(); }
s32 cellMicReset(PPUContext&, s32) { return no_mic(); }
s32 cellMicSetDeviceAttr(PPUContext&, s32, u32, u32, u32) { return no_mic(); }
s32 cellMicRead(PPUContext&, s32, u32, u32) { return no_mic(); }
s32 cellMicReadRaw(PPUContext&, s32, u32, s32) { return no_mic(); }
s32 cellMicReadAux(PPUContext&, s32, u32, s32) { return no_mic(); }
s32 cellMicReadDsp(PPUContext&, s32, u32, s32) { return no_mic(); }

s32 cellMicStartEx(PPUContext&, s32, u32 flags)
{
    if (flags & ~3u) return error(CELL_MICIN_ERROR_PARAM);
    return no_mic();
}

s32 cellMicGetDeviceAttr(PPUContext&, s32 dev, u32, u32, u32)
{
    if (!mic_init()) return error(CELL_MICIN_ERROR_NOT_INIT);
    if (dev < 0) return error(CELL_MICIN_ERROR_PARAM);
    return error(CELL_MICIN_ERROR_DEVICE_NOT_FOUND);
}

// The functions with a value or format pointer check it first.
s32 no_mic_with(u32 pointer)
{
    if (!pointer) return error(CELL_MICIN_ERROR_PARAM);
    return no_mic();
}

s32 cellMicGetSignalAttr(PPUContext&, s32, u32, u32 value) { return no_mic_with(value); }
s32 cellMicSetSignalAttr(PPUContext&, s32, u32, u32 value) { return no_mic_with(value); }
s32 cellMicGetSignalState(PPUContext&, s32, u32, u32 value) { return no_mic_with(value); }
s32 cellMicGetFormatEx(PPUContext&, s32, u32 format, u32) { return no_mic_with(format); }
s32 cellMicGetFormat(PPUContext&, s32, u32 format) { return no_mic_with(format); }
s32 cellMicGetFormatRaw(PPUContext&, s32, u32 format) { return no_mic_with(format); }
s32 cellMicGetFormatAux(PPUContext&, s32, u32 format) { return no_mic_with(format); }
s32 cellMicGetFormatDsp(PPUContext&, s32, u32 format) { return no_mic_with(format); }

s32 cellMicIsOpen(PPUContext&, s32) { return 0; }
s32 cellMicIsAttached(PPUContext&, s32) { return 1; } // RPCS3: always

// cellMicGetDeviceGUID(dev_num, u32* guid): 0xFFFFFFFF, written before the init check (RPCS3).
s32 cellMicGetDeviceGUID(PPUContext& ctx, s32, u32 guid)
{
    if (!guid) return error(CELL_MICIN_ERROR_PARAM);
    if (!usable(ctx, guid, 4, "cellMicGetDeviceGUID: guid")) return error(CELL_MICIN_ERROR_PARAM);
    ps_write32(guest_base(ctx), guid, 0xFFFFFFFF);
    if (!mic_init()) return error(CELL_MICIN_ERROR_NOT_INIT);
    return lv2::CELL_OK;
}

s32 cellMicGetDeviceIdentifier(PPUContext& ctx, s32, u32 id)
{
    if (id) {
        if (!usable(ctx, id, 4, "cellMicGetDeviceIdentifier: id")) return error(CELL_MICIN_ERROR_PARAM);
        ps_write32(guest_base(ctx), id, 0);
    }
    if (!mic_init()) return error(CELL_MICIN_ERROR_NOT_INIT);
    return lv2::CELL_OK;
}

// cellMicGetType(dev_num, s32* type): RPCS3 reports its default device type, a USB microphone, for any of the 8 device
// slots (registered or not), else UNDEF.
s32 cellMicGetType(PPUContext& ctx, s32 dev, u32 type)
{
    if (!type) return error(CELL_MICIN_ERROR_PARAM);
    if (!mic_init()) return error(CELL_MICIN_ERROR_NOT_INIT);
    if (!usable(ctx, type, 4, "cellMicGetType: type")) return error(CELL_MICIN_ERROR_PARAM);
    const s32 t = (dev >= 0 && dev < CELL_MAX_MICS) ? CELLMIC_TYPE_USBAUDIO : CELLMIC_TYPE_UNDEF;
    ps_write32(guest_base(ctx), type, static_cast<u32>(t));
    return lv2::CELL_OK;
}

// cellMicGetStatus(dev_num, CellMicStatus*): a closed, stopped device at 48 kHz with RPCS3's fixed levels for any of
// the 8 slots; nothing written for a higher number.
s32 cellMicGetStatus(PPUContext& ctx, s32 dev, u32 status)
{
    if (!mic_init()) return error(CELL_MICIN_ERROR_NOT_INIT);
    if (dev < 0 || !status) return error(CELL_MICIN_ERROR_PARAM);
    if (dev >= CELL_MAX_MICS) return lv2::CELL_OK;
    if (!usable(ctx, status, kMicStatusSize, "cellMicGetStatus: the CellMicStatus")) {
        return error(CELL_MICIN_ERROR_PARAM);
    }
    u8* base = guest_base(ctx);
    ps_write32(base, status + 0, 48000); // raw_samprate
    ps_write32(base, status + 4, 48000); // dsp_samprate (RPCS3 reports the raw rate)
    ps_write32(base, status + 8, 5);     // dsp_volume
    ps_write32(base, status + 12, 0);    // isStart
    ps_write32(base, status + 16, 0);    // isOpen
    ps_write32(base, status + 20, 10);   // local_voice
    ps_write32(base, status + 24, 0);    // remote_voice
    ps_writef32(base, status + 28, 60.0f); // mic_energy
    ps_writef32(base, status + 32, 60.0f); // spk_energy
    return lv2::CELL_OK;
}

// Notify queues: the queue must exist (by key); no microphone is registered, so it gets no CELLMIC_ATTACH events.
s32 set_mic_queue(u64 key, u64 source)
{
    MicState& m = mic();
    std::lock_guard lock(m.mutex);
    if (!m.init) return error(CELL_MICIN_ERROR_NOT_INIT);
    if (!HleProcess::current()->kernel().event_queue_by_key(key)) return error(CELL_MICIN_ERROR_EVENT_QUEUE);
    m.queue_key = key;
    m.queue_source = source;
    return lv2::CELL_OK;
}

s32 cellMicSetNotifyEventQueue(PPUContext&, u64 key) { return set_mic_queue(key, 0); }
s32 cellMicSetNotifyEventQueue2(PPUContext&, u64 key, u64 source, u64) { return set_mic_queue(key, source); }

s32 cellMicRemoveNotifyEventQueue(PPUContext&, u64)
{
    MicState& m = mic();
    std::lock_guard lock(m.mutex);
    if (!m.init) return error(CELL_MICIN_ERROR_NOT_INIT);
    m.queue_key = m.queue_source = 0;
    return lv2::CELL_OK;
}

} // namespace

void register_devices(Registry& r)
{
    r.add("cellCamera", {
        PS_HLE_FUNC(0xBF47C5DD, cellCameraInit),
        PS_HLE_FUNC(0x5AD46570, cellCameraEnd),
        PS_HLE_FUNC(0x85E1B8DA, cellCameraOpen),
        PS_HLE_FUNC(0x5D25F866, cellCameraOpenEx),
        PS_HLE_FUNC(0x379C5DD6, cellCameraClose),
        PS_HLE_FUNC(0x81F83DB9, cellCameraReset),
        PS_HLE_FUNC(0x456DC4AA, cellCameraStart),
        PS_HLE_FUNC(0x02F5CED0, cellCameraStop),
        PS_HLE_FUNC(0x602E2052, cellCameraGetDeviceGUID),
        PS_HLE_FUNC(0x58BC5870, cellCameraGetType),
        PS_HLE_FUNC(0x7DAC520C, cellCameraGetBufferSize),
        PS_HLE_FUNC(0x532B8AAA, cellCameraGetAttribute),
        PS_HLE_FUNC(0x8CD56EEE, cellCameraSetAttribute),
        PS_HLE_FUNC(0x10697D7F, cellCameraGetBufferInfo),
        PS_HLE_FUNC(0x0E63C444, cellCameraGetBufferInfoEx),
        PS_HLE_FUNC(0xE28B206B, cellCameraReadComplete),
        PS_HLE_FUNC(0x21FC151F, cellCameraReadEx),
        PS_HLE_FUNC(0x3845D39B, cellCameraRead),
        PS_HLE_FUNC(0x61DFBE83, cellCameraPrepExtensionUnit),
        PS_HLE_FUNC(0xEB6F95FB, cellCameraCtrlExtensionUnit),
        PS_HLE_FUNC(0xB602E328, cellCameraGetExtensionUnit),
        PS_HLE_FUNC(0x2DEA3E9B, cellCameraSetExtensionUnit),
        PS_HLE_FUNC(0x8CA53DDE, cellCameraIsAvailable),
        PS_HLE_FUNC(0x7E063BBC, cellCameraIsAttached),
        PS_HLE_FUNC(0xFA160F24, cellCameraIsOpen),
        PS_HLE_FUNC(0x5EEBF24E, cellCameraIsStarted),
        PS_HLE_FUNC(0xB0647E5A, cellCameraSetNotifyEventQueue),
        PS_HLE_FUNC(0x9B98D258, cellCameraRemoveNotifyEventQueue),
        PS_HLE_FUNC(0xA7FD2F5B, cellCameraSetNotifyEventQueue2),
        PS_HLE_FUNC(0x44673F07, cellCameraRemoveNotifyEventQueue2),
        // Made from RPCS3's names without Sony's libcamera to compare them with (D-594): D-665, D-666.
        PS_HLE_UNVERIFIED(0xC76BC1B4, cellCameraOpenAsync),
        PS_HLE_UNVERIFIED(0x4C0219A5, cellCameraOpenPost),
        PS_HLE_UNVERIFIED(0x16B4BA78, cellCameraCloseAsync),
        PS_HLE_UNVERIFIED(0x0CBEDF6A, cellCameraClosePost),
        PS_HLE_UNVERIFIED(0x77D8037F, cellCameraStartAsync),
        PS_HLE_UNVERIFIED(0x5D134C93, cellCameraStartPost),
        PS_HLE_UNVERIFIED(0x3C1FF1E0, cellCameraStopAsync),
        PS_HLE_UNVERIFIED(0xBB5C565B, cellCameraStopPost),
        PS_HLE_UNVERIFIED(0x18B294D0, cellCameraResetAsync),
        PS_HLE_UNVERIFIED(0xA65CE0F8, cellCameraResetPost),
        PS_HLE_UNVERIFIED(0xE25C7B03, cellCameraRead2),
        PS_HLE_UNVERIFIED(0x1350864E, cellCameraResetAttribute),
        PS_HLE_UNVERIFIED(0x090EB5C8, cellCameraSetContainer),
    });
    r.add("cellMic", {
        PS_HLE_FUNC(0x8325E02D, cellMicInit),
        PS_HLE_FUNC(0xC6328CAA, cellMicEnd),
        PS_HLE_FUNC(0x05709BBF, cellMicOpenEx),
        PS_HLE_FUNC(0xDD1B59F0, cellMicOpen),
        PS_HLE_FUNC(0xA42AC07A, cellMicOpenRaw),
        PS_HLE_FUNC(0x8D229F8E, cellMicClose),
        PS_HLE_FUNC(0xDD724314, cellMicStart),
        PS_HLE_FUNC(0xFCFAF246, cellMicStop),
        PS_HLE_FUNC(0x6BC46AAB, cellMicReset),
        PS_HLE_FUNC(0xB2C16321, cellMicSetDeviceAttr),
        PS_HLE_FUNC(0x07E1B12C, cellMicRead),
        PS_HLE_FUNC(0x72165A7F, cellMicReadRaw),
        PS_HLE_FUNC(0x3ACC118E, cellMicReadAux),
        PS_HLE_FUNC(0xC414FAA5, cellMicReadDsp),
        PS_HLE_FUNC(0xDDD19A89, cellMicStartEx),
        PS_HLE_FUNC(0x6A024AA0, cellMicGetDeviceAttr),
        PS_HLE_FUNC(0xAC5BA03A, cellMicGetSignalAttr),
        PS_HLE_FUNC(0x323DEB41, cellMicSetSignalAttr),
        PS_HLE_FUNC(0xB30780EB, cellMicGetSignalState),
        PS_HLE_FUNC(0xAD049ECF, cellMicGetFormatEx),
        PS_HLE_FUNC(0x48108A23, cellMicGetFormat),
        PS_HLE_FUNC(0x4E0B69EE, cellMicGetFormatRaw),
        PS_HLE_FUNC(0xFDA12276, cellMicGetFormatAux),
        PS_HLE_FUNC(0x87A08D29, cellMicGetFormatDsp),
        PS_HLE_FUNC(0x186CB1FB, cellMicIsOpen),
        PS_HLE_FUNC(0x1B42101B, cellMicIsAttached),
        PS_HLE_FUNC(0x017024A8, cellMicGetDeviceGUID),
        PS_HLE_FUNC(0xFDBBE469, cellMicGetDeviceIdentifier),
        PS_HLE_FUNC(0xA52D2AE4, cellMicGetType),
        PS_HLE_FUNC(0x25C5723F, cellMicGetStatus),
        PS_HLE_FUNC(0x7903400E, cellMicSetNotifyEventQueue),
        PS_HLE_FUNC(0x6CC7AE00, cellMicSetNotifyEventQueue2),
        PS_HLE_FUNC(0x65336418, cellMicRemoveNotifyEventQueue),
    });
}

} // namespace pspocket::hle
