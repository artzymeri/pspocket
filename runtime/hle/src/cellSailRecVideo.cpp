// cellSailRec's video streams (D-918, part 8), as libsail_rec (fw 4.92) runs them on its control thread: a stream's
// configuration once its feeder is open (the format the feeder gives against the profile's parameters, the encoder's
// configuration from the firmware's defaults for its profile and level), its pipeline (the feeder's pictures
// converted to the encoder's layout on an SPU task of libsail_rec's, scaled by the firmware's cellVpost2, encoded by
// the firmware's MPEG-4 (cellM4vEnc), H.264 (cellAvcEnc) or MPEG-4 HD (cellM4hdEnc) encoder), and the access units
// handed to the composer.
//
// The pipeline (a slot's +0x50, 0x3C0 bytes, vp:: below): states 1 made, 2 open, 3 started, 4 draining (the converter
// and the scaler finishing), 5 the encoder flushed, 6 ended (its stop completes once the composer has given every
// unit back). Its mode (+0x1C): bit 0 a conversion (an SPU task per picture, its message 0x1F when the scaler follows,
// else 0x20), bit 1 the scaler (its message 0x20); neither: the feeder's YUV 4:2:0 picture straight to the encoder.
// A picture taken from the feeder (the pump, message 0x1E) is scheduled against the stream's clock: dropped when too
// early, encoded twice when late (the second time as a repeat); a change of its pixel aspect drains the encoder and
// opens it again. The encoder tells the recorder (message 0x21) when output is ready: each unit's record (its sizes and
// times) taken into a table, its bytes into the pipeline's ring, the composer told (message 0x23); it gives units back
// (message 0x2C).

#include <cstring>

#include "cellSailRec.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

namespace {

s32 sx(u64 v) { return static_cast<s32>(static_cast<u32>(v)); }

// ------------------------------------------------------------------------------------------------- the firmware's

// The encoders' and the scaler's functions libsail_rec imports (NID only).
namespace m4v {
constexpr std::string_view kLib = "cellM4vEnc";
constexpr u32 kAttr = 0xDF3E7FDB, kOpen = 0xBDB1E067, kReconfigure = 0x3124A0E0, kClose = 0xA314D362,
              kConfig = 0xF8292A94, kFreeInputs = 0x95D2B8F2, kPicture = 0xEA99BEC1, kEncode = 0xC289D964,
              kOptions = 0xC85CD08D, kFlush = 0x88902AC0, kOutput = 0x612F79B0, kRecon = 0xC708618A,
              kReconRelease = 0xD8C9C9A3, kReconSkip = 0x8B31F735, kAuInfo = 0xB82E6F27, kAuCopy = 0x4DE475D7;
constexpr u32 kModule = 0xF013;
}
namespace avc {
constexpr std::string_view kLib = "cellAvcEnc";
constexpr u32 kAttr = 0x58AA86CD, kOpen = 0x0A10C8D1, kReconfigure = 0x42D04FC8, kClose = 0x35D6B9CC,
              kConfig = 0x9412E394, kFreeInputs = 0xEC6AE3E8, kPicture = 0xE25F7DDA, kEncode = 0x5E06172D,
              kOptions = 0xFF14D87E, kFlush = 0xA93E4417, kOutput = 0x19F7C8D1, kRecon = 0xB68CCFE8,
              kReconRelease = 0x501FE972, kReconSkip = 0xE792E34A, kAuInfo = 0xFDCC4831, kAuCopy = 0x62526063;
constexpr u32 kModule = 0xF03A, kModuleOther = 0xF02D;
}
namespace m4hd {
constexpr std::string_view kLib = "cellM4hdEnc";
constexpr u32 kAttr = 0x3D32ADD6, kOpen = 0xB7F4D418, kClose = 0x91A0D079, kEncode = 0x2A6F33B5, kOutput = 0x9CA05AEC,
              kAuInfo = 0xE394A3BD, kAuCopy = 0xC1566DAE;
constexpr u32 kModule = 0xF046;
}
namespace vpost {
constexpr std::string_view kLib = "cellVpost2";
constexpr u32 kAttr = 0x78FD9E05, kOpen = 0xCA3CB5B5, kClose = 0x7BC6E690, kExec = 0x4EF16A92, kResult = 0xAB609749;
constexpr u32 kModule = 0xF043;
}

constexpr HostImport kImports[] = {
    {{}, m4v::kLib, m4v::kAttr, false},
    {{}, m4v::kLib, m4v::kOpen, false},
    {{}, m4v::kLib, m4v::kReconfigure, false},
    {{}, m4v::kLib, m4v::kClose, false},
    {{}, m4v::kLib, m4v::kConfig, false},
    {{}, m4v::kLib, m4v::kFreeInputs, false},
    {{}, m4v::kLib, m4v::kPicture, false},
    {{}, m4v::kLib, m4v::kEncode, false},
    {{}, m4v::kLib, m4v::kOptions, false},
    {{}, m4v::kLib, m4v::kFlush, false},
    {{}, m4v::kLib, m4v::kOutput, false},
    {{}, m4v::kLib, m4v::kRecon, false},
    {{}, m4v::kLib, m4v::kReconRelease, false},
    {{}, m4v::kLib, m4v::kReconSkip, false},
    {{}, m4v::kLib, m4v::kAuInfo, false},
    {{}, m4v::kLib, m4v::kAuCopy, false},
    {{}, avc::kLib, avc::kAttr, false},
    {{}, avc::kLib, avc::kOpen, false},
    {{}, avc::kLib, avc::kReconfigure, false},
    {{}, avc::kLib, avc::kClose, false},
    {{}, avc::kLib, avc::kConfig, false},
    {{}, avc::kLib, avc::kFreeInputs, false},
    {{}, avc::kLib, avc::kPicture, false},
    {{}, avc::kLib, avc::kEncode, false},
    {{}, avc::kLib, avc::kOptions, false},
    {{}, avc::kLib, avc::kFlush, false},
    {{}, avc::kLib, avc::kOutput, false},
    {{}, avc::kLib, avc::kRecon, false},
    {{}, avc::kLib, avc::kReconRelease, false},
    {{}, avc::kLib, avc::kReconSkip, false},
    {{}, avc::kLib, avc::kAuInfo, false},
    {{}, avc::kLib, avc::kAuCopy, false},
    {{}, m4hd::kLib, m4hd::kAttr, false},
    {{}, m4hd::kLib, m4hd::kOpen, false},
    {{}, m4hd::kLib, m4hd::kClose, false},
    {{}, m4hd::kLib, m4hd::kEncode, false},
    {{}, m4hd::kLib, m4hd::kOutput, false},
    {{}, m4hd::kLib, m4hd::kAuInfo, false},
    {{}, m4hd::kLib, m4hd::kAuCopy, false},
    {{}, vpost::kLib, vpost::kAttr, false},
    {{}, vpost::kLib, vpost::kOpen, false},
    {{}, vpost::kLib, vpost::kClose, false},
    {{}, vpost::kLib, vpost::kExec, false},
    {{}, vpost::kLib, vpost::kResult, false},
};

// The firmware's encoders read bytes of their own stack frames they never write (the H.264 encoder leaves three words
// of an access unit's information unwritten, its second time's): libsail_rec's calls find there what the control
// thread's stack holds, cleared on a thread's fresh stack. pspocket's calls clear the stack the callee's frames take
// (0x400 bytes below the frame call_guest makes) first, so the encoders see what they see on Sony's fresh stack.
void clear_callee_stack(PPUContext& ctx)
{
    const lv2::PpuThread& t = thread_of(ctx);
    const u32 top = static_cast<u32>(ctx.gpr[1]) - 0x80;
    u32 low = top - 0x400;
    if (t.stack_size && low < t.stack_addr) low = t.stack_addr;
    if (top > low) std::memset(gb(ctx) + low, 0, top - low);
}

s32 fw(PPUContext& ctx, std::string_view lib, u32 nid, std::initializer_list<u64> args)
{
    clear_callee_stack(ctx);
    return sx(firmware_call(ctx, lib, nid, args));
}

// libsail_rec's option tables (its data at 0x1CC00, read from the installed module as the audio channel tables are):
// the MPEG-4 encoder's default options (7 IDs), the H.264 encoder's (22 IDs at +0x1C), and the IDs a frame's option
// types 0 to 5 (MPEG-4, at +0x94) and 0 to 2 (H.264, at +0xAC) stand for.
constexpr u32 kM4vOptions = 0, kAvcOptions = 0x1C, kM4vTypes = 0x94, kAvcTypes = 0xAC;
constexpr u32 kM4vOptionCount = 7, kAvcOptionCount = 22;
constexpr u32 kAspectOption = 0x345D, kRefsOption = 0x3E83;
constexpr u32 kRepeatOption = 0x4321;

u32 tables(PPUContext& ctx)
{
    const u32 t = module_copy(ctx, 0x1CC00, 0xC0);
    if (!t) {
        log_call_site(ctx, gb(ctx), "cellSailRec: a video stream needs libsail_rec's option tables");
        PS_FATAL(kChan, "libsail_rec's option tables are needed for a video stream: install the system software");
    }
    return t;
}

// ------------------------------------------------------------------------------------------------- conversions

// A conversion (libsail_rec's own, in the work block the pipeline allocates, 0x400 bytes): the parameters its SPU
// program reads, its kind at +0x180 (0 ARGB or RGBA to the encoder's layout, 1 a YUV 4:2:2 coding to it, 2 YUV 4:2:0
// planar to it, 3 and 4 ARGB or RGBA and YUV 4:2:2 to YUV 4:2:0 planar), the request's token at +0x184, the taskset
// at +0x188. Each kind's coefficients are libsail_rec's tables, read from the installed module.
namespace cw {
constexpr u32 kKind = 0x180, kToken = 0x184, kTaskset = 0x188, kSize = 0x400;
}
constexpr u32 kProgramRgbScaled = 0x21480, kProgramRgb = 0x23780, kProgram422Scaled = 0x25700, kProgram422 = 0x27700,
              kProgram420 = 0x29500;
constexpr u32 kTableRgbScaled = 0x1CE50, kTableRgb = 0x1D1B0, kTable422Scaled = 0x1D510, kTable422 = 0x1D650;

void row(PPUContext& ctx, u32 dst, u32 table, u32 offset)
{
    u8 b[16]{};
    if (!module_bytes(table + offset, 16, b))
        PS_LOG_ERROR(kChan, "libsail_rec's conversion table at {:#x}: the installed module is needed", table);
    std::memcpy(gb(ctx) + dst, b, 16);
}

void words(PPUContext& ctx, u32 dst, u32 src, u32 n)
{
    for (u32 i = 0; i < n; i++) w32(ctx, dst + 4 * i, r32(ctx, src + 4 * i));
}

// The RGB kinds' coefficients: three rows by the input (ARGB 1, RGBA 0), twelve by the second selector.
void rgb_rows(PPUContext& ctx, u32 w, u32 table, u32 sel, u32 second)
{
    const u32 a = sel * 0x10, b = second * 0x10;
    row(ctx, w, table, a);
    row(ctx, w + 0x10, table, a + 0x20);
    row(ctx, w + 0x20, table, a + 0x40);
    for (u32 i = 0; i < 12; i++) row(ctx, w + 0x30 + 0x10 * i, table, b + 0x60 + 0x40 * i);
}

// The 4:2:2 kinds' coefficients: by the coding's byte order (UYVY 1, YUYV 0, VYUY 3, Y1VY0U 2) and the second selector.
void yuv_rows(PPUContext& ctx, u32 w, u32 table, s32 sel, u32 second)
{
    const u32 a = static_cast<u32>(sel) * 0x10;
    row(ctx, w, table, a);
    row(ctx, w + 0x10, table, a + 0x40);
    row(ctx, w + 0x20, table, a + 0x80);
    row(ctx, w + 0x30, table, static_cast<u32>((sel + 2) % 4) * 0x10 + 0x80);
    row(ctx, w + 0x40, table, second * 0x10 + 0xC0);
    row(ctx, w + 0x60, table, second * 0x20 + 0xE0);
    row(ctx, w + 0x50, table, second * 0x10 + 0x120);
    row(ctx, w + 0x70, table, second * 0x20 + 0xE0);
}

// The conversion made for the feeder's format (`scaled`: its output YUV 4:2:0 planar, for the scaler or MPEG-4 HD):
// the kind's parameters, the message its program sends (`notify`), the taskset.
s32 conversion_init(PPUContext& ctx, u32 format, u32 scaled, u32 w, u32 notify, u32 taskset, u32 out)
{
    const s32 unsupported = error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    const s32 h = static_cast<s16>(r16(ctx, format + 6)), width = static_cast<s16>(r16(ctx, format + 4));
    const s8 coding = static_cast<s8>(r8(ctx, format));
    const u32 pitch = r32(ctx, format + 8);
    const u32 h16 = (static_cast<u32>(h) + 15) & ~15u;
    const u32 half_w = static_cast<u32>(width) >> 1;
    u32 kind;
    if (static_cast<u32>(static_cast<s32>(coding)) <= 1) {
        const u32 argb = coding == 0 ? 1 : 0;
        if (scaled == 0) {
            // Kind 0: the rows, the pitch, the output's rows and half.
            w32(ctx, w + 0x118, static_cast<u32>(h));
            w32(ctx, w + 0x11C, pitch);
            w32(ctx, w + 0x120, h16);
            w32(ctx, w + 0x114, static_cast<u32>(width));
            w32(ctx, w + 0x124, h16 >> 1);
            words(ctx, w + 0x104, notify, 4);
            rgb_rows(ctx, w, kTableRgb, argb, 0);
            kind = 0;
        } else if (scaled == 1) {
            w32(ctx, w + 0x11C, static_cast<u32>(h));
            w32(ctx, w + 0x120, pitch);
            w32(ctx, w + 0x124, static_cast<u32>(width));
            w32(ctx, w + 0x118, static_cast<u32>(width));
            w32(ctx, w + 0x128, half_w);
            w32(ctx, w + 0x12C, half_w);
            words(ctx, w + 0x108, notify, 4);
            rgb_rows(ctx, w, kTableRgbScaled, argb, 0);
            kind = 3;
        } else {
            return unsupported;
        }
    } else {
        s32 sel;
        switch (coding) {
        case 2: sel = 1; break;
        case 4: sel = 0; break;
        case 9: sel = 3; break;
        case 10: sel = 2; break;
        case 3:
            // Kind 2: the sizes, the pitches (the chroma's half), the output's rows.
            if (scaled != 0) return unsupported;
            w32(ctx, w + 0x2C, static_cast<u32>(width));
            w32(ctx, w + 0x30, static_cast<u32>(h));
            w32(ctx, w + 0x34, pitch);
            w32(ctx, w + 0x38, pitch >> 1);
            w32(ctx, w + 0x3C, pitch >> 1);
            w32(ctx, w + 0x40, h16);
            w32(ctx, w + 0x44, h16 >> 1);
            words(ctx, w + 0x1C, notify, 4);
            w32(ctx, out, w);
            w32(ctx, w + cw::kKind, 2);
            w32(ctx, w + cw::kTaskset, taskset);
            return 0;
        default: return unsupported;
        }
        if (scaled == 0) {
            w32(ctx, w + 0xB0, static_cast<u32>(h));
            w32(ctx, w + 0xB4, pitch);
            w32(ctx, w + 0x94, h16);
            w32(ctx, w + 0xAC, static_cast<u32>(width));
            w32(ctx, w + 0x98, h16 >> 1);
            words(ctx, w + 0x9C, notify, 4);
            yuv_rows(ctx, w, kTable422, sel, 0);
            kind = 1;
        } else if (scaled == 1) {
            w32(ctx, w + 0xAC, static_cast<u32>(h));
            w32(ctx, w + 0xB0, pitch);
            w32(ctx, w + 0xB4, static_cast<u32>(width));
            w32(ctx, w + 0xA8, static_cast<u32>(width));
            w32(ctx, w + 0xB8, half_w);
            w32(ctx, w + 0xBC, half_w);
            words(ctx, w + 0x98, notify, 4);
            yuv_rows(ctx, w, kTable422Scaled, sel, 0);
            kind = 4;
        } else {
            return unsupported;
        }
    }
    w32(ctx, out, w);
    w32(ctx, w + cw::kKind, kind);
    w32(ctx, w + cw::kTaskset, taskset);
    return 0;
}

// A conversion's task: the fields at `at` the program reads, one task of the kind's program in the taskset with the
// work block and the request's token as its argument, its ID kept at `id_at`.
s32 conversion_task(PPUContext& ctx, u32 w, u32 program_vaddr, u32 id_at, u32 token)
{
    const u32 program = spu_program(ctx, program_vaddr);
    if (!program) {
        log_call_site(ctx, gb(ctx), "cellSailRec: a video stream's conversion needs libsail_rec's SPU program");
        PS_FATAL(kChan, "libsail_rec's SPU program at {:#x} is needed for this conversion: install the system software",
            program_vaddr);
    }
    StackVar attr(ctx, gb(ctx), 0x110);
    StackVar argument(ctx, gb(ctx), 16);
    StackVar id(ctx, gb(ctx), 16);
    w64(ctx, argument.addr(), w);
    w64(ctx, argument.addr() + 8, token);
    s32 r = sx(sail::module_call(ctx, "cellSpurs", "_cellSpursTaskAttributeInitialize",
        {attr.addr(), 1, 0x330000, program, 0, argument.addr()}));
    if (r != 0) return r;
    r = sx(sail::module_call(ctx, "cellSpurs", "cellSpursCreateTaskWithAttribute",
        {r32(ctx, w + cw::kTaskset), id.addr(), attr.addr()}));
    if (r != 0) return r;
    w64(ctx, w + id_at, r32(ctx, id.addr()));
    return 0;
}

// A picture converted: the source's planes (luma, and for 4:2:0 its chroma) and the destination's (luma, chroma or
// its two planes), the request's token (its message's argument).
s32 conversion_submit(PPUContext& ctx, u32 w, u32 src, u32 src_u, u32 src_v, u32 dst, u32 dst_u, u32 dst_v, u32 token)
{
    w32(ctx, w + cw::kToken, token);
    switch (r32(ctx, w + cw::kKind)) {
    case 0:
        w32(ctx, w + 0xF8, src);
        w32(ctx, w + 0xFC, dst);
        w32(ctx, w + 0x100, dst_u);
        return conversion_task(ctx, w, kProgramRgb, 0xF0, token);
    case 1:
        w32(ctx, w + 0x88, src);
        w32(ctx, w + 0x8C, dst);
        w32(ctx, w + 0x90, dst_u);
        return conversion_task(ctx, w, kProgram422, 0x80, token);
    case 2:
        w32(ctx, w + 8, src);
        w32(ctx, w + 0xC, src_u);
        w32(ctx, w + 0x10, src_v);
        w32(ctx, w + 0x14, dst);
        w32(ctx, w + 0x18, dst_u);
        return conversion_task(ctx, w, kProgram420, 0, token);
    case 3:
        w32(ctx, w + 0xF8, src);
        w32(ctx, w + 0xFC, dst);
        w32(ctx, w + 0x100, dst_u);
        w32(ctx, w + 0x104, dst_v);
        return conversion_task(ctx, w, kProgramRgbScaled, 0xF0, token);
    case 4:
        w32(ctx, w + 0x88, src);
        w32(ctx, w + 0x8C, dst);
        w32(ctx, w + 0x90, dst_u);
        w32(ctx, w + 0x94, dst_v);
        return conversion_task(ctx, w, kProgram422Scaled, 0x80, token);
    default: return error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    }
}

// ------------------------------------------------------------------------------------------------- the pipeline

// +0 the recorder, +4 the slot, +8 the stream's parameters, +0xC the clock, +0x10 the stop's handle, +0x18 the codec,
// +0x1C the mode, +0x20 a frame's duration and +0x24 the timescale, +0x28 and +0x2C the encoder's size, +0x30 and +0x34
// the chroma planes' offsets in the feeder's picture, +0x38 the units' records and +0x3C their number, +0x40 the
// ring, +0x58 the largest unit, +0x60 and +0x68 the session's start and timescale, +0x70 a frame's span in it, +0x78
// the session's start in the stream's timescale, +0x80 the frames scheduled, +0x84 encoded, +0x88 the H.264 encoder
// with more than one reference, +0x8C an encoder that holds output back, +0x90 pumps asked, +0x94 the conversion busy,
// +0x98 pictures converted, +0x9C the conversions' tokens, +0xDC conversions' room, +0xE0 and +0xE4 where the next
// conversion goes and is read, +0xE8 the scaler busy, +0xEC pictures scaled, +0xF0 the encoder's free inputs, +0xF4
// the output's collection (1 collecting, 2 done), +0xF8 output told while collecting, +0xFC units out, +0x100 their
// records taken, +0x104 units ready, +0x108 to +0x114 the records' indices (taken, copied, given, freed), +0x118 room
// for records, +0x11C units the composer holds, +0x120 and +0x124 the encoder's input picture and the one encoded
// next, +0x128 reconstructed pictures asked (into +0x134 to +0x13C), +0x168 the state, +0x16C the stop's flush, +0x170
// the picture held while the encoder is opened again, +0x174 the picture repeated, +0x178 the last encoded, +0x180 the
// session, +0x184 the conversion and +0x188 its block, +0x18C conversions at once and +0x190 their pictures, +0x1D0
// the scaler's module, +0x1D4 its work area, +0x1D8 its handle, +0x1E0 its parameters, +0x240 the encoder's module,
// +0x244 its block (its work area, the units' records), +0x248 the work area's size, +0x24C its handle, +0x250 two
// input pictures (0xA8 bytes each), +0x3B8 MPEG-4 HD's picture.
namespace vp {
constexpr u32 kRec = 0, kSlot = 4, kParams = 8, kClock = 0xC, kHandle = 0x10, kCodec = 0x18, kMode = 0x1C,
              kDuration = 0x20, kTimescale = 0x24, kWidth = 0x28, kHeight = 0x2C, kUOffset = 0x30, kVOffset = 0x34,
              kUnits = 0x38, kUnitCount = 0x3C, kRing = 0x40, kMaxAu = 0x58, kSessionTime = 0x60,
              kSessionScale = 0x68, kSpan = 0x70, kSessionBase = 0x78, kFrames = 0x80, kEncoded = 0x84,
              kMultiRef = 0x88, kHoldsOutput = 0x8C, kKick = 0x90, kConvBusy = 0x94, kConverted = 0x98,
              kTokens = 0x9C, kConvRoom = 0xDC, kConvWrite = 0xE0, kConvRead = 0xE4, kVpostBusy = 0xE8,
              kScaled = 0xEC, kInputs = 0xF0, kCollect = 0xF4, kDeferred = 0xF8, kOut = 0xFC, kInfos = 0x100,
              kReady = 0x104, kInfoAt = 0x108, kCopyAt = 0x10C, kGetAt = 0x110, kFreeAt = 0x114,
              kRecordsFree = 0x118, kGiven = 0x11C, kInputSlot = 0x120, kEncodeSlot = 0x124, kRecon = 0x128,
              kReconY = 0x134, kReconU = 0x138, kReconV = 0x13C, kState = 0x168, kFlush = 0x16C,
              kPendingFrame = 0x170, kHeldFrame = 0x174, kLastFrame = 0x178, kSession = 0x180, kConversion = 0x184,
              kConvBlock = 0x188, kConvSlots = 0x18C, kBuffers = 0x190, kVpostModule = 0x1D0, kVpostWork = 0x1D4,
              kVpost = 0x1D8, kVpostExec = 0x1E0, kModule = 0x240, kEncBlock = 0x244, kEncWork = 0x248,
              kEncoder = 0x24C, kPictures = 0x250, kPictureSize = 0xA8, kM4hd = 0x3B8, kSize = 0x3C0;
// The ring of units (at kRing), as the audio pipeline's: where the next goes, the room after it, the bytes in use
// before the wrap, the room at the start (all ones less the room skipped at the end once wrapped), its size and start.
constexpr u32 kAt = 0, kRoom = 4, kUsed = 8, kWrap = 0xC, kRingSize = 0x10, kRingStart = 0x14;
}
// MPEG-4 HD's picture (0x18 bytes): its three planes, 1 with a converted or scaled picture, 1 while encoding (2 output
// ready), the flush (1 asked, 2 told).
namespace hd {
constexpr u32 kY = 0, kU = 4, kV = 8, kOwn = 0xC, kBusy = 0x10, kFlush = 0x14;
}

u32 g(PPUContext& ctx, u32 p, u32 off) { return r32(ctx, p + off); }
void s(PPUContext& ctx, u32 p, u32 off, u32 v) { w32(ctx, p + off, v); }
u32 rec_of(PPUContext& ctx, u32 p) { return g(ctx, p, vp::kRec); }
u32 codec(PPUContext& ctx, u32 p) { return g(ctx, p, vp::kCodec); }
u32 encoder(PPUContext& ctx, u32 p) { return g(ctx, p, vp::kEncoder); }
u32 mode(PPUContext& ctx, u32 p) { return g(ctx, p, vp::kMode); }
u32 state(PPUContext& ctx, u32 p) { return g(ctx, p, vp::kState); }
void message(PPUContext& ctx, u32 p, u32 code, u32 arg) { post(ctx, rec_of(ctx, p), code, p, arg); }
u32 index_after(u32 i, u32 n) { return (i + 1) - divu(i + 1, n) * n; }

// A pump asked (message 0x1E) when nothing converts or scales, none is asked and the encoder has room.
void kick(PPUContext& ctx, u32 p)
{
    if (g(ctx, p, vp::kConvBusy) || g(ctx, p, vp::kVpostBusy) || g(ctx, p, vp::kKick) || !g(ctx, p, vp::kInputs))
        return;
    s(ctx, p, vp::kKick, 1);
    message(ctx, p, 0x1E, 0);
}

// The same with conversion room (converter and scaler both).
void kick_converted(PPUContext& ctx, u32 p)
{
    if (!g(ctx, p, vp::kConvRoom) || g(ctx, p, vp::kKick) || g(ctx, p, vp::kConvBusy)) return;
    s(ctx, p, vp::kKick, 1);
    message(ctx, p, 0x1E, 0);
}

// The encoder's free input pictures (MPEG-4 HD: its one picture, free unless encoding, or with its own buffer unless
// 1).
u32 free_inputs(PPUContext& ctx, u32 p)
{
    const u32 c = codec(ctx, p);
    StackVar out(ctx, gb(ctx), 16);
    if (c == 0x22 || c == 0x23) {
        fw(ctx, avc::kLib, avc::kFreeInputs, {encoder(ctx, p), out.addr()});
    } else if (c == 0x24) {
        const u32 o = g(ctx, p, vp::kM4hd);
        const u32 busy = r32(ctx, o + hd::kBusy);
        return r32(ctx, o + hd::kY) ? (busy != 1 ? 1 : 0) : (busy == 0 ? 1 : 0);
    } else if (c == 0x21) {
        fw(ctx, m4v::kLib, m4v::kFreeInputs, {encoder(ctx, p), out.addr()});
    }
    return r32(ctx, out.addr());
}

// The input picture the encoder hands out next (the encoder's own; MPEG-4 HD's from its picture), then the picture
// `src` scaled into it (the scaler busy) or converted (the conversion busy), the frame the request's token.
s32 input_picture(PPUContext& ctx, u32 p, u32 src, u32 frame)
{
    const u32 i = g(ctx, p, vp::kInputSlot), c = codec(ctx, p);
    const u32 pic = p + vp::kPictures + i * vp::kPictureSize;
    w32(ctx, pic + 0x24, frame);
    u32 y = 0, u = 0, v = 0;
    if (c == 0x22 || c == 0x21) {
        const s32 r = c == 0x22 ? fw(ctx, avc::kLib, avc::kPicture, {encoder(ctx, p), pic, 0})
                                : fw(ctx, m4v::kLib, m4v::kPicture, {encoder(ctx, p), pic, 0});
        if (r < 0) return r;
        y = r32(ctx, pic);
        u = r32(ctx, pic + 4);
    } else if (c == 0x24) {
        const u32 o = g(ctx, p, vp::kM4hd);
        v = r32(ctx, o + hd::kV);
        y = r32(ctx, o + hd::kY);
        u = r32(ctx, o + hd::kU);
        w32(ctx, pic + 0xC, 0x1000000);
        w32(ctx, pic, y);
        w32(ctx, pic + 4, u);
        w32(ctx, pic + 8, v);
        const u32 width = g(ctx, p, vp::kWidth);
        w32(ctx, pic + 0x10, width);
        w32(ctx, pic + 0x14, g(ctx, p, vp::kHeight));
        w32(ctx, pic + 0x18, (width + 15) & ~15u);
        w32(ctx, pic + 0x20, 0);
        w32(ctx, pic + 0x1C, ((width >> 1) + 31) & ~31u);
    }
    s(ctx, p, vp::kInputSlot, (i + 1) & 1);
    s(ctx, p, vp::kInputs, g(ctx, p, vp::kInputs) - 1);
    if (const u32 vh = g(ctx, p, vp::kVpost)) {
        StackVar out(ctx, gb(ctx), 0x90);
        fw(ctx, vpost::kLib, vpost::kExec, {vh, src, p + vp::kVpostExec, y, out.addr()});
        s(ctx, p, vp::kVpostBusy, 1);
        return 0;
    }
    const u32 conv = g(ctx, p, vp::kConversion);
    conversion_submit(ctx, conv, src, src + g(ctx, p, vp::kUOffset), src + g(ctx, p, vp::kVOffset), y, u, v, frame);
    s(ctx, p, vp::kConvBusy, 1);
    return 0;
}

// Converter and scaler: the next converted picture scaled once the scaler is idle and the encoder has room.
void scale_next(PPUContext& ctx, u32 p)
{
    const u32 n = g(ctx, p, vp::kConverted);
    if (!n || g(ctx, p, vp::kVpostBusy) || !g(ctx, p, vp::kInputs)) return;
    const u32 i = g(ctx, p, vp::kConvRead);
    s(ctx, p, vp::kConverted, n - 1);
    const u32 token = g(ctx, p, vp::kTokens + 4 * i);
    s(ctx, p, vp::kTokens + 4 * i, 0);
    input_picture(ctx, p, g(ctx, p, vp::kBuffers + 4 * i), token);
    s(ctx, p, vp::kConvRead, index_after(i, g(ctx, p, vp::kConvSlots)));
}

// The next step by the mode: straight to the encoder, a pump asked when it has room; converter and scaler, the next
// scaled; else a pump when idle.
void next(PPUContext& ctx, u32 p)
{
    const u32 m = mode(ctx, p);
    if (m == 0) {
        if (!g(ctx, p, vp::kInputs)) return;
        s(ctx, p, vp::kKick, g(ctx, p, vp::kKick) + 1);
        message(ctx, p, 0x1E, 0);
        return;
    }
    if (m == 3) return scale_next(ctx, p);
    kick(ctx, p);
}

// The encoder flushed (MPEG-4 HD: asked, the output looked at once unless it encodes).
s32 flush(PPUContext& ctx, u32 p)
{
    const u32 c = codec(ctx, p);
    if (c == 0x22) return fw(ctx, avc::kLib, avc::kFlush, {encoder(ctx, p)});
    if (c == 0x24) {
        const u32 o = g(ctx, p, vp::kM4hd);
        w32(ctx, o + hd::kFlush, 1);
        if (r32(ctx, o + hd::kBusy) != 1) {
            w32(ctx, o + hd::kFlush, 2);
            message(ctx, p, 0x21, 0);
        }
        return 0;
    }
    if (c == 0x21) return fw(ctx, m4v::kLib, m4v::kFlush, {encoder(ctx, p)});
    return 0;
}

// The frame's options (+0x20 their number, +0x24 {type, value} pairs) as the encoder's: H.264's types 0 to 2 (and,
// unless the encoder has one reference, each picture's), MPEG-4's 0 to 5 and the repeat (type 4, 100); type 4's value
// 1 or 3 (H.264: up to 4, or 100) the picture's kind for the encoder. At most 16 a picture. A repeated picture (type
// 2): H.264 with one reference repeats (100), MPEG-4 sets the repeat, MPEG-4 HD 3.
u32 options(PPUContext& ctx, u32 p, u32 frame, bool repeat)
{
    const u32 c = codec(ctx, p), n = r32(ctx, frame + 0x20), list = r32(ctx, frame + 0x24);
    StackVar out(ctx, gb(ctx), 0x80);
    const u32 t = c == 0x22 || c == 0x21 ? tables(ctx) : 0;
    u32 kind = 0, count = 0;
    const auto add = [&](u32 id, u32 value) {
        w32(ctx, out.addr() + 8 * count, id);
        w32(ctx, out.addr() + 8 * count + 4, value);
        count++;
    };
    if (c == 0x22) {
        if (!g(ctx, p, vp::kMultiRef) && repeat) return 100;
        if (!n || !list) return 0;
        for (u32 k = 0; k < n; k++) {
            const u32 type = r32(ctx, list + 8 * k), value = r32(ctx, list + 8 * k + 4);
            if (type == 4) {
                if (static_cast<s32>(value) <= 4 || value == 100) kind = value;
            } else if (type <= 2) {
                const u32 id = r32(ctx, t + kAvcTypes + 4 * type);
                if (id != 0xFFFFFFFF) add(id, value);
            }
            if (count > 15) break;
        }
        if (count) fw(ctx, avc::kLib, avc::kOptions, {encoder(ctx, p), out.addr(), count});
        return kind;
    }
    if (c == 0x24) {
        if (repeat) return 3;
        if (!n || !list) return 0;
        for (u32 k = 0; k < n; k++) {
            if (r32(ctx, list + 8 * k) != 4) continue;
            const u32 value = r32(ctx, list + 8 * k + 4);
            if (value == 1) kind = 1;
            else if (value == 100) kind = 3;
        }
        return kind;
    }
    if (c == 0x21) {
        if (repeat) {
            add(kRepeatOption, 1);
            fw(ctx, m4v::kLib, m4v::kOptions, {encoder(ctx, p), out.addr(), count});
            return 0;
        }
        if (!n || !list) return 0;
        for (u32 k = 0; k < n; k++) {
            const u32 type = r32(ctx, list + 8 * k), value = r32(ctx, list + 8 * k + 4);
            if (type == 4) {
                if (value == 1) kind = 1;
                else if (value == 100) add(kRepeatOption, 1);
            } else if (type <= 5) {
                const u32 id = r32(ctx, t + kM4vTypes + 4 * type);
                if (id != 0xFFFFFFFF) add(id, value);
            }
            if (count > 15) break;
        }
        if (count) fw(ctx, m4v::kLib, m4v::kOptions, {encoder(ctx, p), out.addr(), count});
        return kind;
    }
    return 0;
}

void collect(PPUContext& ctx, u32 p);
void stopped(PPUContext& ctx, u32 p);

// The encoder's next picture (its input picture's frame: +0x28 0 a picture, 1 one to skip, 2 a repeat): its options,
// the frame given back to the feeder once converted or scaled (unless held), encoded with its time (the frames encoded
// so far at the frame's duration, in seconds and a remainder of the timescale) and its tag (+0xC, the pixel aspect);
// the encoder's free inputs looked at again; the converted or scaled picture used. While draining (4), once nothing is
// left the encoder is flushed (5).
void encode(PPUContext& ctx, u32 p)
{
    const u32 i = g(ctx, p, vp::kEncodeSlot);
    const u32 pic = p + vp::kPictures + i * vp::kPictureSize;
    const u32 frame = r32(ctx, pic + 0x24);
    const u32 type = r32(ctx, frame + 0x28);
    const u32 kind = options(ctx, p, frame, type == 2);
    w32(ctx, frame + 0x20, 0);
    bool encoded = true;
    if (type != 2) {
        if (mode(ctx, p) && g(ctx, p, vp::kHeldFrame) != frame && g(ctx, p, vp::kPendingFrame) != frame)
            message(ctx, p, 0x22, frame);
        if (type == 1) {
            message(ctx, p, 0x1E, 0);
            encoded = false;
        }
    }
    if (encoded) {
        const u32 c = codec(ctx, p), tag = r32(ctx, frame + 0xC), ts = g(ctx, p, vp::kTimescale);
        s(ctx, p, vp::kLastFrame, frame);
        const u64 at = u64{g(ctx, p, vp::kEncoded)} * u64{g(ctx, p, vp::kDuration)};
        const s64 q64 = divs64(static_cast<s64>(at), static_cast<s64>(u64{ts}));
        const u32 q = static_cast<u32>(q64), rem = static_cast<u32>(at - static_cast<u64>(q64) * u64{ts});
        StackVar info(ctx, gb(ctx), 0x30);
        const u32 a = info.addr();
        if (c == 0x22) {
            w32(ctx, a, q);
            w32(ctx, a + 4, rem);
            w32(ctx, a + 8, ts);
            w32(ctx, a + 0xC, q);
            w32(ctx, a + 0x10, rem);
            w32(ctx, a + 0x14, ts);
            w32(ctx, a + 0x18, kind);
            w32(ctx, a + 0x20, tag);
            w32(ctx, a + 0x24, tag);
            fw(ctx, avc::kLib, avc::kEncode, {encoder(ctx, p), pic, a, 0});
        } else if (c == 0x24 || c == 0x21) {
            w32(ctx, a, q);
            w32(ctx, a + 4, rem);
            w32(ctx, a + 8, ts);
            w32(ctx, a + 0xC, kind);
            w32(ctx, a + 0x10, tag);
            if (c == 0x24) {
                fw(ctx, m4hd::kLib, m4hd::kEncode, {encoder(ctx, p), pic, a, 0});
                w32(ctx, g(ctx, p, vp::kM4hd) + hd::kBusy, 1);
            } else {
                fw(ctx, m4v::kLib, m4v::kEncode, {encoder(ctx, p), pic, a, 0});
            }
        }
        w32(ctx, pic + 0x24, 0);
        s(ctx, p, vp::kEncodeSlot, (i + 1) & 1);
        s(ctx, p, vp::kEncoded, g(ctx, p, vp::kEncoded) + 1);
        const u32 n = free_inputs(ctx, p);
        if (g(ctx, p, vp::kInputs) != n) {
            s(ctx, p, vp::kInputs, n);
            next(ctx, p);
        }
    }
    const u32 m = mode(ctx, p);
    if (m & 2) s(ctx, p, vp::kScaled, g(ctx, p, vp::kScaled) - 1);
    else if (m & 1) s(ctx, p, vp::kConverted, g(ctx, p, vp::kConverted) - 1);
    if (state(ctx, p) == 4 && !g(ctx, p, vp::kConverted) && !g(ctx, p, vp::kConvBusy) && !g(ctx, p, vp::kScaled) &&
        !g(ctx, p, vp::kVpostBusy)) {
        flush(ctx, p);
        s(ctx, p, vp::kState, 5);
    }
}

// A frame to the encoder by the mode: converter and scaler, converted into the next of the pipeline's pictures (its
// token kept for the scaler); straight, as the encoder's input picture (its planes in the feeder's picture) and
// encoded; else through the input picture. A pump less asked once it went.
s32 submit(PPUContext& ctx, u32 p, u32 frame)
{
    const u32 m = mode(ctx, p);
    const u32 src = r32(ctx, frame);
    s32 r;
    if (m == 3) {
        const u32 i = g(ctx, p, vp::kConvWrite);
        const u32 buf = g(ctx, p, vp::kBuffers + 4 * i);
        const u32 wh = g(ctx, p, vp::kWidth) * g(ctx, p, vp::kHeight);
        r = conversion_submit(ctx, g(ctx, p, vp::kConversion), src, src + g(ctx, p, vp::kUOffset),
            src + g(ctx, p, vp::kVOffset), buf, buf + wh, buf + wh + (wh >> 2), frame);
        s(ctx, p, vp::kConvBusy, 1);
        s(ctx, p, vp::kConvRoom, g(ctx, p, vp::kConvRoom) - 1);
        s(ctx, p, vp::kTokens + 4 * i, frame);
        s(ctx, p, vp::kConvWrite, index_after(i, g(ctx, p, vp::kConvSlots)));
    } else if (m == 0) {
        const u32 pic = p + vp::kPictures + g(ctx, p, vp::kEncodeSlot) * vp::kPictureSize;
        const u32 width = g(ctx, p, vp::kWidth);
        w32(ctx, pic + 0xC, 0x1000000);
        w32(ctx, pic, src);
        w32(ctx, pic + 0x10, width);
        w32(ctx, pic + 0x20, 0);
        w32(ctx, pic + 0x14, g(ctx, p, vp::kHeight));
        w32(ctx, pic + 4, src + g(ctx, p, vp::kUOffset));
        w32(ctx, pic + 8, src + g(ctx, p, vp::kVOffset));
        w32(ctx, pic + 0x24, frame);
        w32(ctx, pic + 0x18, (width + 15) & ~15u);
        w32(ctx, pic + 0x1C, (width + 31) & ~31u);
        encode(ctx, p);
        r = 0;
    } else {
        r = input_picture(ctx, p, src, frame);
    }
    if (r >= 0) s(ctx, p, vp::kKick, g(ctx, p, vp::kKick) - 1);
    return r;
}

// Draining: once nothing converts or scales, the encoder flushed (5); else 4.
void drain(PPUContext& ctx, u32 p)
{
    if (g(ctx, p, vp::kConvBusy) || g(ctx, p, vp::kConverted) || g(ctx, p, vp::kVpostBusy) || g(ctx, p, vp::kScaled)) {
        s(ctx, p, vp::kState, 4);
        return;
    }
    s(ctx, p, vp::kState, 5);
    flush(ctx, p);
}

// A frame against the stream's time: the frames scheduled so far at the frame's duration from the session's start (or
// the clock's time for it when the recorder's clock is shared), less the frame's time. `hold`: the frame waits
// (+0x170) while the encoder is drained to be opened again. Earlier than half a frame: given back and the next pumped;
// later than half a frame: encoded as a repeat and kept (+0x174) to be encoded again; else encoded.
s32 schedule(PPUContext& ctx, u32 p, u32 frame, bool hold)
{
    const u64 t = r64(ctx, frame + 0x10);
    const u32 scale = r32(ctx, p + vp::kSessionScale + 4);
    const u64 spent = u64{g(ctx, p, vp::kFrames)} * g(ctx, p, vp::kDuration) * scale;
    const u64 expected = divu64(spent, g(ctx, p, vp::kTimescale)) + r64(ctx, p + vp::kSessionTime);
    s64 diff = static_cast<s64>(expected - t);
    if (const u32 clock = g(ctx, p, vp::kClock))
        diff = static_cast<s64>(clock_time(ctx, clock, p, expected, t, scale) - t);
    if (hold) {
        s(ctx, p, vp::kFrames, g(ctx, p, vp::kFrames) + 1);
        s(ctx, p, vp::kPendingFrame, frame);
        w32(ctx, frame + 0x28, 0);
        drain(ctx, p);
        return 0;
    }
    const u32 half = static_cast<u32>(r64(ctx, p + vp::kSpan)) >> 1;
    if (static_cast<s64>(u64{half}) < diff) {
        message(ctx, p, 0x22, frame);
        message(ctx, p, 0x1E, 0);
        return 0;
    }
    s(ctx, p, vp::kFrames, g(ctx, p, vp::kFrames) + 1);
    if (static_cast<s64>(static_cast<s32>(0u - half)) <= diff) {
        w32(ctx, frame + 0x28, 0);
        return submit(ctx, p, frame);
    }
    s(ctx, p, vp::kHeldFrame, frame);
    w32(ctx, frame + 0x28, 2);
    return submit(ctx, p, frame);
}

// The encoder's free inputs changed: the repeated frame encoded again once nothing else is on its way, else the next.
void inputs_changed(PPUContext& ctx, u32 p)
{
    const u32 n = free_inputs(ctx, p);
    if (g(ctx, p, vp::kInputs) == n) return;
    s(ctx, p, vp::kInputs, n);
    const u32 held = g(ctx, p, vp::kHeldFrame);
    if (held && !g(ctx, p, vp::kConverted) && !g(ctx, p, vp::kConvBusy) && !g(ctx, p, vp::kScaled) &&
        !g(ctx, p, vp::kVpostBusy)) {
        s(ctx, p, vp::kHeldFrame, 0);
        s(ctx, p, vp::kKick, 1);
        schedule(ctx, p, held, false);
        return;
    }
    next(ctx, p);
}

// A pixel aspect's aspect-ratio information for the encoder: (index, width, height) and the format's aspect byte
// (libsail_rec's own tables: MPEG-4's indices and H.264's, an explicit ratio otherwise, its width the code's high half
// shifted as libsail_rec shifts it).
struct Sar {
    u32 idc, w, h;
    u8 byte;
};

Sar sar_info(u32 code, bool mpeg4, bool arithmetic)
{
    const u32 hi = arithmetic ? static_cast<u32>(static_cast<s32>(code) >> 16) : code >> 16;
    switch (code) {
    case 0x10001: return {1, 0, 0, 1};
    case 0xC000B: return {2, 0, 0, 2};
    case 0xA000B: return {3, 0, 0, 3};
    case 0x10000B: return {4, 0, 0, 4};
    case 0x280021: return {5, 0, 0, 5};
    default: break;
    }
    const u8 byte = code == 0x40003 ? 0xE : 0x10;
    if (!mpeg4) {
        switch (code) {
        case 0x18000B: return {6, 0, 0, 0x10};
        case 0x14000B: return {7, 0, 0, 0x10};
        case 0x20000B: return {8, 0, 0, 0x10};
        case 0x500021: return {9, 0, 0, 0x10};
        case 0x12000B: return {10, 0, 0, 0x10};
        case 0xF000B: return {11, 0, 0, 0x10};
        case 0x400021: return {12, 0, 0, 0x10};
        case 0xA00063: return {13, 0, 0, 0x10};
        default: return {255, hi, code & 0xFFFF, byte};
        }
    }
    return {15, hi, code & 0xFFFF, byte};
}

void set_sar(PPUContext& ctx, u32 params, const Sar& s)
{
    w32(ctx, params + 0x1F8, s.idc);
    w32(ctx, params + 0x1FC, s.w);
    w32(ctx, params + 0x200, s.h);
}

// A frame of the feeder's: a new session restarts the time base; its options of the pixel aspect (type 3 an aspect
// ratio, 6 a pixel aspect) change the stream's (its aspect-ratio information and format), the frame tagged with the
// aspect; a change holds the frame while the encoder is drained.
s32 frame_in(PPUContext& ctx, u32 p, u32 frame)
{
    const u32 session = r32(ctx, frame + 0xC);
    if (g(ctx, p, vp::kSession) != session) {
        const u64 scale = r64(ctx, frame + 0x18), t = r64(ctx, frame + 0x10);
        w64(ctx, p + vp::kSessionScale, scale);
        w64(ctx, p + vp::kSessionTime, t);
        s(ctx, p, vp::kSession, session);
        s(ctx, p, vp::kEncoded, 0);
        s(ctx, p, vp::kFrames, 0);
        w64(ctx, p + vp::kSpan, divu64(scale * g(ctx, p, vp::kDuration), g(ctx, p, vp::kTimescale)));
        w64(ctx, p + vp::kSessionBase, divu64(u64{g(ctx, p, vp::kTimescale)} * r64(ctx, frame + 0x10), scale));
    }
    const u32 params = g(ctx, p, vp::kParams);
    const u32 n = r32(ctx, frame + 0x20), list = r32(ctx, frame + 0x24);
    u32 cur = r32(ctx, params + 0x94);
    bool changed = false;
    if (n && list) {
        for (u32 k = 0; k < n; k++) {
            const u32 type = r32(ctx, list + 8 * k), value = r32(ctx, list + 8 * k + 4);
            u32 code;
            if (type == 3) {
                switch (value) {
                case 1: code = 0x10001; break;
                case 2: code = 0xC000B; break;
                case 3: code = 0xA000B; break;
                case 4: code = 0x10000B; break;
                case 5: code = 0x280021; break;
                case 14: code = 0x40003; break;
                default: code = cur; break;
                }
            } else if (type == 6) {
                code = value;
            } else {
                continue;
            }
            if (code == cur) continue;
            const Sar info = sar_info(code, codec(ctx, p) == 0x21, false);
            set_sar(ctx, params, info);
            w8(ctx, params + 0x91, info.byte);
            w32(ctx, params + 0x94, code);
            changed = true;
            cur = code;
        }
    }
    w32(ctx, frame + 0xC, cur);
    return schedule(ctx, p, frame, changed);
}

// The encoder closed.
void encoder_close(PPUContext& ctx, u32 p)
{
    const u32 c = codec(ctx, p), e = encoder(ctx, p);
    if (!e) return;
    if (c == 0x21) fw(ctx, m4v::kLib, m4v::kClose, {e});
    else if (c == 0x22 || c == 0x23) fw(ctx, avc::kLib, avc::kClose, {e});
    else if (c == 0x24) fw(ctx, m4hd::kLib, m4hd::kClose, {e});
    else return;
    s(ctx, p, vp::kEncoder, 0);
}

// The encoder's resources (0x18 bytes): its block, the work area's size, `third` (libsail_rec leaves H.264's
// unwritten; pspocket's 0), the SPURS, its SPU priorities (the slot's parameter 79).
void resources(PPUContext& ctx, u32 at, u32 p, u32 slot, u32 third)
{
    w32(ctx, at, g(ctx, p, vp::kEncBlock));
    w32(ctx, at + 4, g(ctx, p, vp::kEncWork));
    w32(ctx, at + 8, third);
    w32(ctx, at + 0xC, r32(ctx, rec_of(ctx, p) + rc::kSpurs));
    std::memmove(gb(ctx) + at + 0x10, gb(ctx) + slot + vs::kP79, 8);
}

void notification(PPUContext& ctx, u32 at, u32 p, u32 code)
{
    w32(ctx, at, rec_of(ctx, p) + rc::kSpursQueue);
    w32(ctx, at + 4, code);
    w32(ctx, at + 8, p);
    w32(ctx, at + 0xC, 0);
}

// The encoder opened again with the stream's configuration (`again`: reconfigured, or for MPEG-4 HD closed and opened)
// unless it is open; MPEG-4 HD's aspect ratio 1:1 unless 1:1, 10:11 or 40:33. A failure leaves it closed.
void encoder_open(PPUContext& ctx, u32 p, u32 slot, u32 params, bool again)
{
    const u32 c = codec(ctx, p), config = params + 0xA8;
    StackVar res(ctx, gb(ctx), 0x20);
    StackVar note(ctx, gb(ctx), 0x10);
    StackVar info(ctx, gb(ctx), 0x20);
    notification(ctx, note.addr(), p, 0x21);
    s32 r;
    if (c == 0x22 || c == 0x23) {
        resources(ctx, res.addr(), p, slot, 0);
        if (again) {
            fw(ctx, avc::kLib, avc::kReconfigure, {encoder(ctx, p), config, note.addr(), info.addr()});
            return;
        }
        if (encoder(ctx, p)) return;
        r = fw(ctx, avc::kLib, avc::kOpen, {config, res.addr(), note.addr(), info.addr(), p + vp::kEncoder});
    } else if (c == 0x21) {
        resources(ctx, res.addr(), p, slot, 0);
        if (again) {
            fw(ctx, m4v::kLib, m4v::kReconfigure, {encoder(ctx, p), config, note.addr(), info.addr()});
            return;
        }
        if (encoder(ctx, p)) return;
        r = fw(ctx, m4v::kLib, m4v::kOpen, {config, res.addr(), note.addr(), info.addr(), p + vp::kEncoder});
    } else if (c == 0x24) {
        if (!again && encoder(ctx, p)) return;
        const u32 idc = r32(ctx, params + 0x1F8);
        if (idc <= 5 && ((u32{1} << idc) & 0x2A)) {
            std::memmove(gb(ctx) + params + 0xB4, gb(ctx) + params + 0x1F8, 12);
        } else {
            w32(ctx, params + 0xBC, 0);
            w32(ctx, params + 0xB4, 1);
            w32(ctx, params + 0xB8, 0);
        }
        resources(ctx, res.addr(), p, slot, 0);
        if (const u32 e = encoder(ctx, p)) fw(ctx, m4hd::kLib, m4hd::kClose, {e});
        r = fw(ctx, m4hd::kLib, m4hd::kOpen, {config, res.addr(), note.addr(), info.addr(), p + vp::kEncoder});
    } else {
        return;
    }
    if (r < 0) s(ctx, p, vp::kEncoder, 0);
}

// The stop's end once no unit is out or ready: the encoder closed (MPEG-4 HD's opened again), open (2), completed.
void stopped(PPUContext& ctx, u32 p)
{
    if (g(ctx, p, vp::kCollect) | g(ctx, p, vp::kReady)) return;
    encoder_close(ctx, p);
    s(ctx, p, vp::kState, 2);
    if (codec(ctx, p) == 0x24) encoder_open(ctx, p, g(ctx, p, vp::kSlot), g(ctx, p, vp::kParams), true);
    post_completion(ctx, rec_of(ctx, p), 0, r64(ctx, p + vp::kHandle));
}

// The next ready unit's record for the composer.
void take_unit(PPUContext& ctx, u32 p, u32 out)
{
    const u32 i = g(ctx, p, vp::kGetAt), st = state(ctx, p);
    s(ctx, p, vp::kReady, g(ctx, p, vp::kReady) - 1);
    s(ctx, p, vp::kGiven, g(ctx, p, vp::kGiven) + 1);
    w32(ctx, out, r32(ctx, g(ctx, p, vp::kUnits) + 4 * i));
    s(ctx, p, vp::kGetAt, index_after(i, g(ctx, p, vp::kUnitCount)));
    if (st == 6) stopped(ctx, p);
}

// The encoder's output ready (message 0x21; told while collecting, looked at afterwards): its information (the state,
// units out, room, reconstructed pictures; MPEG-4 HD's its flush's end, or its output), the frame encoded given back
// (straight to the encoder: the encoder is done with it), the encoder's free inputs looked at, each reconstructed
// picture copied out when asked or let go, the units collected. At the encoder's end while flushing: closed and
// opened again for a held frame (then its encoding), or ended (6).
s32 output(PPUContext& ctx, u32 p)
{
    if (g(ctx, p, vp::kCollect)) {
        s(ctx, p, vp::kDeferred, 1);
        return 0;
    }
    const u32 c = codec(ctx, p);
    StackVar out(ctx, gb(ctx), 0x10);
    const u32 o = out.addr();
    if (c == 0x22 || c == 0x21) {
        const s32 r = c == 0x22 ? fw(ctx, avc::kLib, avc::kOutput, {encoder(ctx, p), o, 1})
                                : fw(ctx, m4v::kLib, m4v::kOutput, {encoder(ctx, p), o, 1});
        if (r < 0) {
            std::memset(gb(ctx) + o, 0, 0x10);
            w32(ctx, o, 2);
        }
    } else if (c == 0x24) {
        const u32 obj = g(ctx, p, vp::kM4hd);
        if (r32(ctx, obj + hd::kFlush) == 2) {
            w32(ctx, obj + hd::kFlush, 0);
            std::memset(gb(ctx) + o, 0, 0x10);
            w32(ctx, o, 2);
        } else if (fw(ctx, m4hd::kLib, m4hd::kOutput, {encoder(ctx, p), o, 1}) >= 0) {
            w32(ctx, o, 0);
        } else {
            std::memset(gb(ctx) + o, 0, 0x10);
            w32(ctx, o, 2);
        }
        w32(ctx, obj + hd::kBusy, 2);
        if (r32(ctx, obj + hd::kFlush)) {
            w32(ctx, obj + hd::kFlush, 2);
            message(ctx, p, 0x21, 0);
        }
    }
    const u32 status = r32(ctx, o), units = r32(ctx, o + 4), room = r32(ctx, o + 8);
    u32 recons = r32(ctx, o + 0xC);
    if (!mode(ctx, p)) {
        const u32 last = g(ctx, p, vp::kLastFrame);
        if (last && g(ctx, p, vp::kHeldFrame) != last && g(ctx, p, vp::kPendingFrame) != last)
            message(ctx, p, 0x22, last);
    }
    s(ctx, p, vp::kLastFrame, 0);
    inputs_changed(ctx, p);
    for (; recons; recons--) {
        if (!g(ctx, p, vp::kRecon)) {
            if (c == 0x21) fw(ctx, m4v::kLib, m4v::kReconSkip, {encoder(ctx, p)});
            else if (c == 0x22) fw(ctx, avc::kLib, avc::kReconSkip, {encoder(ctx, p)});
            continue;
        }
        if (c == 0x21 || c == 0x22) {
            StackVar rp(ctx, gb(ctx), 0x20);
            const u32 a = rp.addr();
            const u32 y = g(ctx, p, vp::kReconY), u = g(ctx, p, vp::kReconU), v = g(ctx, p, vp::kReconV);
            const s32 r = c == 0x21 ? fw(ctx, m4v::kLib, m4v::kRecon, {encoder(ctx, p), a})
                                    : fw(ctx, avc::kLib, avc::kRecon, {encoder(ctx, p), a});
            if (r >= 0) {
                const u32 w = r32(ctx, a + 0x10);
                std::memmove(gb(ctx) + y, gb(ctx) + r32(ctx, a), (((w + 15) & ~15u) * r32(ctx, a + 0x18)) & ~15u);
                const u32 chroma = (((w >> 1) + 15) & ~15u) * r32(ctx, a + 0x1C);
                std::memmove(gb(ctx) + u, gb(ctx) + r32(ctx, a + 4), chroma);
                std::memmove(gb(ctx) + v, gb(ctx) + r32(ctx, a + 8), chroma);
                if (c == 0x21) fw(ctx, m4v::kLib, m4v::kReconRelease, {encoder(ctx, p), a});
                else fw(ctx, avc::kLib, avc::kReconRelease, {encoder(ctx, p), a});
            }
        }
        s(ctx, p, vp::kRecon, g(ctx, p, vp::kRecon) - 1);
        message(ctx, p, 0x24, 0);
    }
    const u32 n = units > room ? room : units;
    if (n) s(ctx, p, vp::kCollect, 1);
    s(ctx, p, vp::kOut, n);
    collect(ctx, p);
    if (status != 2 || state(ctx, p) != 5) return 0;
    const u32 pending = g(ctx, p, vp::kPendingFrame);
    if (!pending) {
        s(ctx, p, vp::kState, 6);
        stopped(ctx, p);
        return 0;
    }
    encoder_close(ctx, p);
    encoder_open(ctx, p, g(ctx, p, vp::kSlot), g(ctx, p, vp::kParams), false);
    const u32 inputs = free_inputs(ctx, p);
    s(ctx, p, vp::kState, 3);
    s(ctx, p, vp::kInputs, inputs);
    s(ctx, p, vp::kKick, 1);
    s(ctx, p, vp::kPendingFrame, 0);
    return submit(ctx, p, pending);
}

// The units out: each one's record from the encoder (its size, its kind, its times in the stream's timescale from the
// session's start) while there is room for records; then each record's bytes into the ring while it has room, the
// composer told (message 0x23; on a flushing stop the ring emptied instead). Once all is collected (2): the next
// picture encoded, the inputs looked at (straight to the encoder), output told meanwhile looked at, and an encoder that
// holds output back asked again while flushing.
void collect(PPUContext& ctx, u32 p)
{
    u32 out = g(ctx, p, vp::kOut), room = g(ctx, p, vp::kRecordsFree);
    u32 infos;
    if (!out) {
        infos = g(ctx, p, vp::kInfos);
        if (g(ctx, p, vp::kCollect) == 1) s(ctx, p, vp::kCollect, 2);
    } else if (!room) {
        infos = g(ctx, p, vp::kInfos);
    } else {
        u32 i = g(ctx, p, vp::kInfoAt);
        for (;;) {
            const u32 unit = r32(ctx, g(ctx, p, vp::kUnits) + 4 * i);
            const u32 c = codec(ctx, p);
            const u64 base = r64(ctx, p + vp::kSessionBase);
            out--;
            room--;
            const auto field = [&](u32 off) { return u64{r32(ctx, unit + off)}; };
            if (c == 0x22) {
                fw(ctx, avc::kLib, avc::kAuInfo, {encoder(ctx, p), unit + 0x34});
                w32(ctx, unit + 4, r32(ctx, unit + 0x34));
                w32(ctx, unit + 0x30, r32(ctx, unit + 0xA8));
                w64(ctx, unit + 0x18, field(0x74) * field(0x7C) + (base + field(0x78)));
                w64(ctx, unit + 0x20, field(0x80) * field(0x88) + (base + field(0x84)));
                w64(ctx, unit + 8, field(0x5C) * field(0x64) + (field(0x60) + base));
                w64(ctx, unit + 0x10, field(0x68) * field(0x70) + (base + field(0x6C)));
            } else if (c == 0x24 || c == 0x21) {
                u64 t;
                if (c == 0x24) {
                    fw(ctx, m4hd::kLib, m4hd::kAuInfo, {encoder(ctx, p), unit + 0x34});
                    w32(ctx, unit + 0x30, r32(ctx, unit + 0x44));
                    w32(ctx, unit + 4, r32(ctx, unit + 0x4C));
                    t = field(0x38) * field(0x40) + (field(0x3C) + base);
                } else {
                    fw(ctx, m4v::kLib, m4v::kAuInfo, {encoder(ctx, p), unit + 0x34});
                    w32(ctx, unit + 0x30, r32(ctx, unit + 0x78));
                    w32(ctx, unit + 4, r32(ctx, unit + 0x34));
                    t = field(0x5C) * field(0x64) + (field(0x60) + base);
                }
                w64(ctx, unit + 0x20, t);
                w64(ctx, unit + 8, t);
            }
            infos = g(ctx, p, vp::kInfos) + 1;
            s(ctx, p, vp::kInfos, infos);
            i = index_after(i, g(ctx, p, vp::kUnitCount));
            s(ctx, p, vp::kInfoAt, i);
            if (!out) {
                if (g(ctx, p, vp::kCollect) == 1) s(ctx, p, vp::kCollect, 2);
                break;
            }
            if (!room) break;
        }
    }
    s(ctx, p, vp::kOut, out);
    s(ctx, p, vp::kRecordsFree, room);
    if (infos) {
        const u32 r = p + vp::kRing;
        StackVar copy(ctx, gb(ctx), 0x10);
        for (;;) {
            const u32 unit = r32(ctx, g(ctx, p, vp::kUnits) + 4 * g(ctx, p, vp::kCopyAt));
            const u32 size = r32(ctx, unit + 4);
            u32 at = r32(ctx, r + vp::kAt);
            const s32 wrap = static_cast<s32>(r32(ctx, r + vp::kWrap));
            const u32 space = r32(ctx, r + vp::kRoom);
            if (wrap < 0) {
                if (size > space) break;
                w32(ctx, r + vp::kRoom, space - size);
                w32(ctx, r + vp::kAt, at + size);
            } else if (size > space) {
                if (static_cast<s32>(size) > wrap) break;
                at = r32(ctx, r + vp::kRingStart);
                w32(ctx, r + vp::kWrap, ~space);
                w32(ctx, r + vp::kAt, at + size);
                w32(ctx, r + vp::kRoom, static_cast<u32>(wrap) - size);
            } else {
                w32(ctx, r + vp::kAt, at + size);
                w32(ctx, r + vp::kUsed, size + r32(ctx, r + vp::kUsed));
                w32(ctx, r + vp::kRoom, space - size);
            }
            if (!at) break;
            const u32 c = codec(ctx, p);
            w32(ctx, copy.addr() + 4, size);
            w32(ctx, copy.addr(), at);
            w32(ctx, copy.addr() + 8, 0);
            s32 cr = 0;
            if (c == 0x22) {
                cr = fw(ctx, avc::kLib, avc::kAuCopy, {encoder(ctx, p), copy.addr()});
            } else if (c == 0x24) {
                cr = fw(ctx, m4hd::kLib, m4hd::kAuCopy, {encoder(ctx, p), copy.addr()});
                w32(ctx, g(ctx, p, vp::kM4hd) + hd::kBusy, 0);
            } else if (c == 0x21) {
                cr = fw(ctx, m4v::kLib, m4v::kAuCopy, {encoder(ctx, p), copy.addr()});
            }
            if (cr < 0) break;
            infos--;
            w32(ctx, unit, at);
            s(ctx, p, vp::kCopyAt, index_after(g(ctx, p, vp::kCopyAt), g(ctx, p, vp::kUnitCount)));
            if (!g(ctx, p, vp::kFlush)) {
                s(ctx, p, vp::kReady, g(ctx, p, vp::kReady) + 1);
                message(ctx, p, 0x23, 0);
            } else {
                w32(ctx, r + vp::kAt, r32(ctx, r + vp::kRingStart));
                w32(ctx, r + vp::kRoom, r32(ctx, r + vp::kRingSize));
                w32(ctx, r + vp::kUsed, 0);
                w32(ctx, r + vp::kWrap, 0);
            }
            if (!infos) break;
        }
        if (infos) {
            s(ctx, p, vp::kInfos, infos);
            return;
        }
    }
    s(ctx, p, vp::kInfos, 0);
    if (g(ctx, p, vp::kCollect) != 2) return;
    const u32 m = mode(ctx, p);
    s(ctx, p, vp::kCollect, 0);
    bool more = false;
    if (m & 2) more = g(ctx, p, vp::kScaled) != 0;
    else if (m & 1) more = g(ctx, p, vp::kConverted) != 0;
    if (more) encode(ctx, p);
    if (!mode(ctx, p)) inputs_changed(ctx, p);
    if (g(ctx, p, vp::kDeferred)) {
        s(ctx, p, vp::kDeferred, 0);
        output(ctx, p);
    }
    if (g(ctx, p, vp::kHoldsOutput) && state(ctx, p) == 5) message(ctx, p, 0x21, 0);
}

// `n` units the composer gave back: their ring bytes free (as the audio pipeline's), room for records, then collected.
void released(PPUContext& ctx, u32 p, u32 n)
{
    s(ctx, p, vp::kGiven, g(ctx, p, vp::kGiven) - n);
    s(ctx, p, vp::kRecordsFree, g(ctx, p, vp::kRecordsFree) + n);
    u32 i = g(ctx, p, vp::kFreeAt);
    const u32 count = g(ctx, p, vp::kUnitCount), r = p + vp::kRing;
    for (u32 k = 0; k < n; k++) {
        const u32 unit = r32(ctx, g(ctx, p, vp::kUnits) + 4 * i);
        i = static_cast<u32>((u64{i} + 1) - divu64(u64{i} + 1, count) * count);
        const u32 size = r32(ctx, unit + 4);
        const u32 used = r32(ctx, r + vp::kUsed) - size;
        w32(ctx, r + vp::kUsed, used);
        if (static_cast<s32>(r32(ctx, r + vp::kWrap)) < 0) {
            if (used) {
                w32(ctx, r + vp::kRoom, r32(ctx, r + vp::kRoom) + size);
                continue;
            }
            const u32 head = r32(ctx, r + vp::kAt) - r32(ctx, r + vp::kRingStart);
            w32(ctx, r + vp::kWrap, 0);
            w32(ctx, r + vp::kUsed, head);
            w32(ctx, r + vp::kRoom, r32(ctx, r + vp::kRingSize) - head);
            continue;
        }
        if (used) {
            w32(ctx, r + vp::kWrap, size + r32(ctx, r + vp::kWrap));
            continue;
        }
        w32(ctx, r + vp::kWrap, 0);
        w32(ctx, r + vp::kAt, r32(ctx, r + vp::kRingStart));
        w32(ctx, r + vp::kRoom, r32(ctx, r + vp::kRingSize));
    }
    s(ctx, p, vp::kFreeAt, i);
    collect(ctx, p);
}

// A conversion or a scaling done (message 0x20): its result taken; converter and scaler, room for the next
// conversion; the pump asked; the next encoded unless collecting.
void converted(PPUContext& ctx, u32 p)
{
    if (const u32 vh = g(ctx, p, vp::kVpost)) {
        StackVar out(ctx, gb(ctx), 0x90);
        fw(ctx, vpost::kLib, vpost::kResult, {vh, out.addr()});
        s(ctx, p, vp::kVpostBusy, 0);
        s(ctx, p, vp::kScaled, g(ctx, p, vp::kScaled) + 1);
    } else {
        s(ctx, p, vp::kConvBusy, 0);
        s(ctx, p, vp::kConverted, g(ctx, p, vp::kConverted) + 1);
    }
    if (mode(ctx, p) == 3) {
        s(ctx, p, vp::kConvRoom, g(ctx, p, vp::kConvRoom) + 1);
        kick_converted(ctx, p);
    } else {
        kick(ctx, p);
    }
    if (!g(ctx, p, vp::kCollect)) encode(ctx, p);
}

// A conversion done with the scaler to follow (message 0x1F): the pump asked, the converted picture scaled.
void converted_for_scaler(PPUContext& ctx, u32 p)
{
    s(ctx, p, vp::kConvBusy, 0);
    kick_converted(ctx, p);
    s(ctx, p, vp::kConverted, g(ctx, p, vp::kConverted) + 1);
    scale_next(ctx, p);
}

// The pipeline's resources freed (the ring, the encoder, its block and module; the scaler, its work area, the
// pictures, its module; the conversion and its block, the common taskset given back): 0, or the scaler's failure.
s32 release(PPUContext& ctx, u32 p)
{
    const u32 rec = rec_of(ctx, p);
    if (const u32 ring = g(ctx, p, vp::kRing + vp::kRingStart)) {
        dealloc(ctx, rec, 0x80, ring);
        s(ctx, p, vp::kRing + vp::kRingStart, 0);
    }
    const u32 c = codec(ctx, p);
    if (c >= 0x21 && c <= 0x24) {
        encoder_close(ctx, p);
        if (const u32 block = g(ctx, p, vp::kEncBlock)) {
            dealloc(ctx, rec, 0x80, block);
            s(ctx, p, vp::kEncBlock, 0);
        }
        if (const u32 m = g(ctx, p, vp::kModule)) {
            unload_module(ctx, m & 0xFFFF);
            s(ctx, p, vp::kModule, 0);
        }
    }
    s32 r = 0;
    if (const u32 vh = g(ctx, p, vp::kVpost)) {
        r = fw(ctx, vpost::kLib, vpost::kClose, {vh});
        s(ctx, p, vp::kVpost, 0);
    }
    if (const u32 work = g(ctx, p, vp::kVpostWork)) {
        dealloc(ctx, rec, 0x80, work);
        s(ctx, p, vp::kVpostWork, 0);
    }
    if (const u32 buf = g(ctx, p, vp::kBuffers)) {
        dealloc(ctx, rec, 0x80, buf);
        s(ctx, p, vp::kBuffers + 4, 0);
        s(ctx, p, vp::kBuffers, 0);
    }
    if (g(ctx, p, vp::kVpostModule)) {
        r = unload_module(ctx, vpost::kModule);
        s(ctx, p, vp::kVpostModule, 0);
    }
    if (r < 0) return r;
    if (const u32 conv = g(ctx, p, vp::kConversion)) {
        const u32 taskset = r32(ctx, conv + cw::kTaskset);
        s(ctx, p, vp::kConversion, 0);
        if (taskset) release_taskset(ctx, rec);
    }
    if (const u32 block = g(ctx, p, vp::kConvBlock)) {
        dealloc(ctx, rec, 0x80, block);
        s(ctx, p, vp::kConvBlock, 0);
    }
    return 0;
}

// The configuration query (the encoder's defaults for a profile and level), its module loaded for it.
s32 config_query(PPUContext& ctx, u32 c, u32 q)
{
    if (c == 0x21) {
        load_module(ctx, m4v::kModule);
        const s32 r = fw(ctx, m4v::kLib, m4v::kConfig, {q});
        unload_module(ctx, m4v::kModule);
        return r;
    }
    if (c == 0x22) {
        load_module(ctx, avc::kModule);
        const s32 r = fw(ctx, avc::kLib, avc::kConfig, {q});
        unload_module(ctx, avc::kModule);
        return r;
    }
    return 0;
}

// The pipeline opened for the stream (`params`, the profile's video parameters with the feeder's format at +0x60 and
// the encoder's at +0x80, its configuration at +0xA8): by the feeder's coding and the codec, a conversion (to the
// encoder's layout, or YUV 4:2:0 planar for the scaler or MPEG-4 HD; its message 0x1F with the scaler, else 0x20) on
// the common taskset and the scaler (cellVpost2, when the slot scales: from the feeder's picture or the converted one
// to the encoder's size, its message 0x20); the encoder's module, its work area with the units' records (enough for
// the composer's delay at the frame rate), the encoder opened (its message 0x21), the ring of units (the largest
// unit; doubled and at least the delay's bytes at the bit rate unless the stream is the composer's first).
void open(PPUContext& ctx, u32 p, u32 slot, u32 params, u32 delay, u32 scale, u64 handle)
{
    const u32 rec = rec_of(ctx, p);
    const u32 c = r32(ctx, params + 0x14);
    s(ctx, p, vp::kCodec, c);
    const s8 coding = static_cast<s8>(r8(ctx, params + 0x60));
    const u32 spurs = r32(ctx, rec + rc::kSpurs);
    const s32 unsupported = error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM), memory = error(CELL_SAIL_ERROR_MEMORY);
    const auto fail = [&](s32 err) {
        release(ctx, p);
        post_completion(ctx, rec, err, handle);
    };
    const u8 scaling = r8(ctx, slot + 0x42F);
    u32 m = 0;
    bool convert;
    u32 scaled = 0;
    if (static_cast<u32>(static_cast<s32>(coding)) <= 1 || coding == 2 || coding == 9) {
        convert = true;
        scaled = c == 0x24 || scaling ? 1 : 0;
    } else if (coding == 3) {
        convert = c != 0x24 && !scaling;
    } else {
        return fail(unsupported);
    }
    if (convert) {
        const u32 block = alloc(ctx, rec, 0x80, cw::kSize);
        if (!block) return fail(memory);
        std::memset(gb(ctx) + block, 0, cw::kSize);
        s(ctx, p, vp::kConvBlock, block);
        StackVar note(ctx, gb(ctx), 0x10);
        notification(ctx, note.addr(), p, scaling ? 0x1F : 0x20);
        const u32 taskset = common_taskset(ctx, rec);
        if (!taskset) return fail(memory);
        const s32 r = conversion_init(ctx, params + 0x60, scaled, block, note.addr(), taskset, p + vp::kConversion);
        if (r < 0) return fail(r);
        m = 1;
    }
    if (scaling) {
        s32 r = load_module(ctx, vpost::kModule);
        if (r < 0) return fail(r);
        s(ctx, p, vp::kVpostModule, vpost::kModule);
        StackVar cfg(ctx, gb(ctx), 0x28);
        const u32 k = cfg.addr();
        if (r8(ctx, params + 0x62) != 8 && r8(ctx, params + 0x82) != 8) return fail(unsupported);
        const u32 in_w = convert ? (static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x64))) + 1) & ~1u
                                 : (r32(ctx, params + 0x68) + 1) & ~1u;
        const u32 in_h = (static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x66))) + 1) & ~1u;
        w32(ctx, k, in_w);
        w32(ctx, k + 4, in_h);
        w32(ctx, k + 0x1C, c == 0x24 ? 1 : 2);
        if (convert) {
            s(ctx, p, vp::kConvSlots, 1);
            const u32 size = ((((in_h + 15) & ~15u) * (((in_w + 15) & ~15u) * 3)) >> 1) & 0x7FFFFF80u;
            const u32 buf = alloc(ctx, rec, 0x80, size);
            if (!buf) return fail(memory);
            s(ctx, p, vp::kBuffers, buf);
        }
        const s32 out_w = static_cast<s16>(r16(ctx, params + 0x84)), out_h = static_cast<s16>(r16(ctx, params + 0x86));
        w32(ctx, k + 0x10, static_cast<u32>(out_w));
        w32(ctx, k + 0x14, static_cast<u32>(out_h));
        StackVar attr(ctx, gb(ctx), 0x10);
        r = fw(ctx, vpost::kLib, vpost::kAttr, {k, attr.addr()});
        if (r < 0) return fail(r);
        const u32 work_size = r32(ctx, attr.addr());
        const u32 work = alloc(ctx, rec, 0x80, work_size);
        if (!work) return fail(memory);
        s(ctx, p, vp::kVpostWork, work);
        StackVar res(ctx, gb(ctx), 0x28);
        const u32 a = res.addr();
        w32(ctx, a, work);
        w32(ctx, a + 4, work_size);
        w32(ctx, a + 8, spurs);
        std::memmove(gb(ctx) + a + 0xC, gb(ctx) + slot + vs::kP75, 8);
        w32(ctx, a + 0x14, r32(ctx, slot + vs::kP74));
        notification(ctx, a + 0x18, p, 0x20);
        r = fw(ctx, vpost::kLib, vpost::kOpen, {k, a, p + vp::kVpost});
        if (r < 0) {
            s(ctx, p, vp::kVpost, 0);
            return fail(r);
        }
        const u32 e = p + vp::kVpostExec;
        if (!r32(ctx, params + 0x1C)) {
            w32(ctx, e + 4, r32(ctx, params + 0x1E8));
            w32(ctx, e + 8, r32(ctx, params + 0x1EC));
        } else {
            const bool fields = scaling > 1;
            if (in_h > static_cast<u32>(out_h)) {
                w32(ctx, e + 4, fields ? 3 : 2);
                w32(ctx, e + 8, fields ? 2 : 1);
            } else {
                w32(ctx, e + 4, fields ? 0 : 1);
                w32(ctx, e + 8, fields ? 2 : 1);
            }
        }
        w32(ctx, e + 0xC, in_w);
        w32(ctx, e + 0x10, in_h);
        w32(ctx, e + 0x24, 0);
        w32(ctx, e + 0x28, static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x64))) & ~1u);
        w32(ctx, e + 0x2C, static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x66))) & ~1u);
        w32(ctx, e, 0);
        w32(ctx, e + 0x20, 0);
        w32(ctx, e + 0x14, 1);
        w32(ctx, e + 0x18, 1);
        w32(ctx, e + 0x1C, static_cast<s8>(r8(ctx, params + 0x70)) != 0 ? 1 : 0);
        w8(ctx, e + 0x48, 0);
        w32(ctx, e + 0x5C, 0);
        w32(ctx, e + 0x30, static_cast<u32>(out_w));
        w32(ctx, e + 0x34, static_cast<u32>(out_h));
        w32(ctx, e + 0x3C, 0);
        w32(ctx, e + 0x40, static_cast<u32>(out_w));
        w32(ctx, e + 0x44, static_cast<u32>(out_h));
        w32(ctx, e + 0x38, 0);
        w64(ctx, e + 0x50, 0);
        w32(ctx, e + 0x58, 0);
        m |= 2;
    }
    // The encoder.
    s(ctx, p, vp::kMode, m);
    s(ctx, p, vp::kMultiRef, 0);
    s(ctx, p, vp::kHoldsOutput, 0);
    s(ctx, p, vp::kDuration, r32(ctx, params + 0xA0));
    s(ctx, p, vp::kWidth, static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x84))));
    s(ctx, p, vp::kHeight, static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x86))));
    s(ctx, p, vp::kTimescale, r32(ctx, params + 0x34));
    StackVar note(ctx, gb(ctx), 0x10);
    notification(ctx, note.addr(), p, 0x21);
    const u32 luma = static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x66))) * r32(ctx, params + 0x68);
    s(ctx, p, vp::kUOffset, luma);
    s(ctx, p, vp::kVOffset, luma + (luma >> 2));
    const u32 config = params + 0xA8;
    // The units' records the composer's delay spans at the frame rate, after the encoder's work area.
    const auto records = [&] {
        return static_cast<u32>(divu64(u64{delay} * r32(ctx, params + 0x34), scale * r32(ctx, params + 0xA0)));
    };
    const auto lay_out = [&](u32 block, u32 work, u32 count, u32 table_bytes, u32 stride) {
        s(ctx, p, vp::kEncBlock, block);
        s(ctx, p, vp::kEncWork, work);
        const u32 table = block + ((work + 7) & ~7u);
        s(ctx, p, vp::kUnits, table);
        for (u32 i = 0; i < count; i++) w32(ctx, table + 4 * i, table + table_bytes + i * stride);
        s(ctx, p, vp::kUnitCount, count);
    };
    StackVar res(ctx, gb(ctx), 0x20);
    StackVar info(ctx, gb(ctx), 0x20);
    StackVar attr(ctx, gb(ctx), 0x10);
    u32 max_au = 0;
    s32 r;
    if (c == 0x21 || c == 0x22 || c == 0x23) {
        const bool m4 = c == 0x21;
        const u32 id = m4 ? m4v::kModule : c == 0x22 ? avc::kModule : avc::kModuleOther;
        r = load_module(ctx, id);
        if (r < 0) return fail(r);
        s(ctx, p, vp::kModule, id);
        w32(ctx, config + 0x18, m4 ? 1 : r32(ctx, slot + vs::kP77));
        r = m4 ? fw(ctx, m4v::kLib, m4v::kAttr, {config, attr.addr()})
               : fw(ctx, avc::kLib, avc::kAttr, {config, attr.addr()});
        if (r < 0) return fail(r);
        const u32 work = r32(ctx, attr.addr());
        const u32 count = records();
        const u32 table_bytes = (count * 4 + 7) & ~7u;
        const u32 block = alloc(ctx, rec, 0x80, (((work + 7) & ~7u) + (count << 8) + table_bytes + 0x7F) & ~0x7Fu);
        if (!block) return fail(memory);
        lay_out(block, work, count, table_bytes, m4 ? 0x84 : 0xB4);
        resources(ctx, res.addr(), p, slot, 0);
        if (m4) {
            if (r32(ctx, config) == 0x200) s(ctx, p, vp::kHoldsOutput, 1);
            r = fw(ctx, m4v::kLib, m4v::kOpen, {config, res.addr(), note.addr(), info.addr(), p + vp::kEncoder});
        } else {
            if (const u32 list = r32(ctx, config + 0x10); list && r32(ctx, config + 0x14)) {
                for (u32 k = 0, n = r32(ctx, config + 0x14); k < n; k++)
                    if (r32(ctx, list + 8 * k) == kRefsOption && static_cast<s32>(r32(ctx, list + 8 * k + 4)) > 1)
                        s(ctx, p, vp::kMultiRef, 1);
            }
            const u32 profile = r32(ctx, config);
            if (profile == 0x310 || profile == 0x200) s(ctx, p, vp::kHoldsOutput, 1);
            r = fw(ctx, avc::kLib, avc::kOpen, {config, res.addr(), note.addr(), info.addr(), p + vp::kEncoder});
        }
        if (r < 0) return fail(r);
        max_au = r32(ctx, info.addr() + 8);
    } else if (c == 0x24) {
        r = load_module(ctx, m4hd::kModule);
        if (r < 0) return fail(r);
        s(ctx, p, vp::kModule, m4hd::kModule);
        r = fw(ctx, m4hd::kLib, m4hd::kAttr, {config, attr.addr()});
        if (r < 0) return fail(r);
        const u32 work = r32(ctx, attr.addr());
        const u32 count = records();
        const u32 table_bytes = (count * 4 + 7) & ~7u;
        const u32 part = (((work + 7) & ~7u) + (count << 8) + table_bytes + 0x7F) & ~0x7Fu;
        // MPEG-4 HD's own YUV 4:2:0 picture when the stream converts or scales.
        u32 picture = 0, luma_size = 0;
        if (m) {
            luma_size = ((static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x84))) + 15) & ~15u) *
                        static_cast<u32>(static_cast<s16>(r16(ctx, params + 0x86)));
            picture = (((luma_size * 3) >> 1) + 0x7F) & ~0x7Fu;
        }
        const u32 block = alloc(ctx, rec, 0x80, (picture + part + 0x18) & ~7u);
        if (!block) return fail(memory);
        s(ctx, p, vp::kEncBlock, block);
        const u32 y = picture ? block + part : 0;
        const u32 obj = block + part + picture;
        s(ctx, p, vp::kM4hd, obj);
        std::memset(gb(ctx) + obj, 0, 0x18);
        w32(ctx, obj + hd::kOwn, m ? 1 : 0);
        w32(ctx, obj + hd::kV, y + ((luma_size * 5) >> 2));
        w32(ctx, obj + hd::kU, y + luma_size);
        w32(ctx, obj + hd::kY, y);
        lay_out(block, work, count, table_bytes, 0x84);
        resources(ctx, res.addr(), p, slot, 0);
        r = fw(ctx, m4hd::kLib, m4hd::kOpen, {config, res.addr(), note.addr(), info.addr(), p + vp::kEncoder});
        if (r < 0) return fail(r);
        max_au = r32(ctx, info.addr() + 8);
    } else {
        return fail(unsupported);
    }
    // The ring.
    s(ctx, p, vp::kMaxAu, max_au);
    u32 ring = max_au;
    if (!r32(ctx, params + 0x10)) {
        u32 bytes = static_cast<u32>(divu64(u64{r32(ctx, params + 0x30)} * delay, scale << 3));
        if (bytes < max_au) bytes = max_au;
        ring = bytes << 1;
    }
    const u32 buf = alloc(ctx, rec, 0x80, (ring + 0x7F) & ~0x7Fu);
    if (!buf) return fail(memory);
    s(ctx, p, vp::kSlot, slot);
    const u32 rr = p + vp::kRing;
    w32(ctx, rr + vp::kAt, buf);
    w32(ctx, rr + vp::kRoom, ring);
    w32(ctx, rr + vp::kUsed, 0);
    w32(ctx, rr + vp::kRingSize, ring);
    w32(ctx, rr + vp::kWrap, 0);
    w32(ctx, rr + vp::kRingStart, buf);
    s(ctx, p, vp::kParams, params);
    s(ctx, p, vp::kSession, 0);
    s(ctx, p, vp::kState, 2);
    post_completion(ctx, rec, 0, handle);
}

// The units ready given to the composer as it has room.
s32 deliver(PPUContext& ctx, u32 p, u32 cs, u32 entry)
{
    u32 n = g(ctx, p, vp::kReady);
    const u32 room = r32(ctx, entry + se::kCapacity);
    if (room < n) n = room;
    StackVar unit(ctx, gb(ctx), 16);
    while (n) {
        n--;
        take_unit(ctx, p, unit.addr());
        const s32 r = composer_au(ctx, cs, entry, r32(ctx, unit.addr()), true);
        if (r < 0) return r;
    }
    return 0;
}

u32 feeder_of(PPUContext& ctx, u32 p) { return r32(ctx, g(ctx, p, vp::kSlot) + vs::kFeeder); }
u32 streams(PPUContext& ctx, u32 rec) { return r32(ctx, rec + rc::kStreamState); }

// The next frame taken from the feeder (its timescale set to the recorder's) into the pipeline.
void pump(PPUContext& ctx, u32 rec, u32 f, u32 p)
{
    StackVar out(ctx, gb(ctx), 16);
    if (feeder_take(ctx, f, out.addr(), true) < 0) return;
    const u32 frame = r32(ctx, out.addr());
    w64(ctx, frame + 0x18, r32(ctx, rec + rc::kTimescale));
    frame_in(ctx, p, frame);
}

// The pump when frames wait, the pipeline holds none and a pump is asked.
void pump_if(PPUContext& ctx, u32 rec, u32 f, u32 p)
{
    if (feeder_has_frames(ctx, f) && !g(ctx, p, vp::kHeldFrame) && !g(ctx, p, vp::kPendingFrame) &&
        g(ctx, p, vp::kKick))
        pump(ctx, rec, f, p);
}

// ------------------------------------------------------------------------------------------------- the stream

// A format's pixel aspect (width << 16 | height) from its aspect-ratio byte (+0x11): 1:1, 12:11, 10:11, 16:11, 40:33,
// 4:3, the format's own (+0x14), unspecified (-1), or none (0).
u32 aspect_of(PPUContext& ctx, u32 format)
{
    switch (r8(ctx, format + 0x11)) {
    case 1: return 0x10001;
    case 2: return 0xC000B;
    case 3: return 0xA000B;
    case 4: return 0x10000B;
    case 5: return 0x280021;
    case 0xE: return 0x40003;
    case 0x10: return r32(ctx, format + 0x14);
    case 0xFF: return 0xFFFFFFFF;
    default: return 0;
    }
}

// A bit rate from the pixels a second (`q`, libsail_rec's estimate) rounded up to a step (1 Mbit/s above 999999, 128000
// above 127999, else 64000).
u32 round_rate(u64 q)
{
    const u32 lo = static_cast<u32>(q);
    const u32 step = lo > 999999 ? 1000000 : lo > 127999 ? 128000 : 64000;
    return step * divu(static_cast<u32>(q + step - 1), step);
}

} // namespace

// ------------------------------------------------------------------------------------------------- the steps

void video_pipeline_init(PPUContext& ctx, u32 p, u32 rec, u32 clock)
{
    std::memset(gb(ctx) + p, 0, vp::kSize);
    s(ctx, p, vp::kState, 1);
    s(ctx, p, vp::kRec, rec);
    s(ctx, p, vp::kClock, clock);
}

void video_pipeline_fini(PPUContext& ctx, u32 p)
{
    if (state(ctx, p) > 1) release(ctx, p);
    std::memset(gb(ctx) + p, 0, vp::kSize);
}

// Start (3): the encoder opened unless it is, the counters cleared, room for every record, MPEG-4 HD's picture idle,
// nothing held, the encoder's free inputs, a pump asked (message 0x1E).
void video_pipeline_start(PPUContext& ctx, u32 p, u32 slot, u32 params, u64 handle)
{
    encoder_open(ctx, p, slot, params, false);
    s(ctx, p, vp::kState, 3);
    std::memset(gb(ctx) + p + vp::kKick, 0, 0x98);
    s(ctx, p, vp::kRecordsFree, g(ctx, p, vp::kUnitCount));
    if (const u32 obj = g(ctx, p, vp::kM4hd)) std::memset(gb(ctx) + obj + hd::kBusy, 0, 8);
    s(ctx, p, vp::kHeldFrame, 0);
    s(ctx, p, vp::kPendingFrame, 0);
    s(ctx, p, vp::kInputs, free_inputs(ctx, p));
    s(ctx, p, vp::kKick, 1);
    s(ctx, p, vp::kConvRoom, g(ctx, p, vp::kConvSlots));
    message(ctx, p, 0x1E, 0);
    post_completion(ctx, rec_of(ctx, p), 0, handle);
}

// Stop(flag, handle): made or open completes with 1; the repeated frame given back; a frame held for the encoder's
// reopening given back (and nothing else); the flag flushes the ring once the composer holds no unit; started (3) the
// encoder drained; ended (6) completes when no unit waits.
void video_pipeline_stop(PPUContext& ctx, u32 p, u32 flag, u64 handle)
{
    if (state(ctx, p) - 1 <= 1) {
        post_completion(ctx, rec_of(ctx, p), 1, handle);
        return;
    }
    w64(ctx, p + vp::kHandle, handle);
    if (const u32 held = g(ctx, p, vp::kHeldFrame)) {
        message(ctx, p, 0x22, held);
        s(ctx, p, vp::kHeldFrame, 0);
    }
    if (const u32 pending = g(ctx, p, vp::kPendingFrame)) {
        message(ctx, p, 0x22, pending);
        s(ctx, p, vp::kPendingFrame, 0);
        return;
    }
    s(ctx, p, vp::kFlush, flag);
    if (flag && !g(ctx, p, vp::kGiven)) {
        const u32 r = p + vp::kRing;
        s(ctx, p, vp::kReady, 0);
        s(ctx, p, vp::kRecordsFree, g(ctx, p, vp::kUnitCount));
        w32(ctx, r + vp::kWrap, 0);
        w32(ctx, r + vp::kAt, r32(ctx, r + vp::kRingStart));
        w32(ctx, r + vp::kRoom, r32(ctx, r + vp::kRingSize));
        w32(ctx, r + vp::kUsed, 0);
        collect(ctx, p);
    }
    const u32 st = state(ctx, p);
    if (st == 3) drain(ctx, p);
    else if (st == 6) stopped(ctx, p);
}

void video_pipeline_close(PPUContext& ctx, u32 p, u64 handle)
{
    s32 r = 0;
    if (state(ctx, p) != 1) {
        r = release(ctx, p);
        s(ctx, p, vp::kState, 1);
    }
    post_completion(ctx, rec_of(ctx, p), r, handle);
}

// A video stream configured once its feeder is open (libsail_rec's continuation of the feeder's open): the format the
// feeder gives (+0x60 of the stream's parameters) against the profile's (+0x40): its frame rate (the timescale and a
// frame's duration; doubled for H.264; the profile's timescale when it gives one), its rows within the pitch, the
// encoder's size (the feeder's, or the profile's when the slot may scale), the pixel aspect and the color matrix; the
// encoder's configuration (+0xA8) by the codec: MPEG-4 and H.264 from the firmware's defaults for the profile and level
// (the level from the size, the frame rate and the bit rate when the profile gives none), the GOP, the bit rate and
// the game's own configuration's settings over them; MPEG-4 HD's own. Then the pipeline opened and the stream added to
// the composer (two steps for `handle`): 2, or the error.
s32 video_stream_open(PPUContext& ctx, u32 rec, u32 record, u64 handle)
{
    const u32 slot = r32(ctx, record + 0x10);
    const u32 profile = r32(ctx, rec + rc::kProfile);
    const u32 params = r32(ctx, profile + pf::kVideo) + r8(ctx, slot + vs::kIndex) * 0x220;
    const u32 fmt = params + 0x60, want = params + 0x40, enc = params + 0x80;
    const s32 fr = feeder_format(ctx, r32(ctx, slot + vs::kFeeder), fmt);
    if (fr < 0) return fr;
    const s32 unsupported = error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    std::memset(gb(ctx) + enc, 0xFF, 0x20);
    // The frame rate: its code's duration and timescale.
    const s8 want_rate = static_cast<s8>(r8(ctx, want + 3));
    const u8 rate = r8(ctx, fmt + 3);
    if (want_rate != -1 && want_rate != static_cast<s8>(rate)) return unsupported;
    w8(ctx, enc + 3, rate);
    if (rate > 7) return unsupported;
    static constexpr u32 kRates[8][2] = {{1001, 24000}, {1000, 24000}, {1000, 25000}, {1001, 30000}, {1000, 30000},
        {1000, 50000}, {1001, 60000}, {1000, 60000}};
    u32 dur = kRates[rate][0], ts = kRates[rate][1];
    const u32 c = r32(ctx, params + 0x14);
    if (c - 0x22 <= 1) {
        dur <<= 1;
        ts <<= 1;
    }
    const s32 want_ts = static_cast<s32>(r32(ctx, params + 0x34));
    const s32 want_profile = static_cast<s32>(r32(ctx, params + 0x20));
    const s32 want_level = static_cast<s32>(r32(ctx, params + 0x24));
    const s32 want_max = static_cast<s32>(r32(ctx, params + 0x2C));
    s32 bitrate = static_cast<s32>(r32(ctx, params + 0x30));
    const s32 want_gop = static_cast<s32>(r32(ctx, params + 0x38));
    if (want_ts > 0) {
        dur = static_cast<u32>(divu64(u64{dur} * static_cast<u64>(static_cast<s64>(want_ts)), ts));
        ts = static_cast<u32>(want_ts);
    }
    // The rows within the pitch (larger: a multiple of 32).
    const s32 width = static_cast<s16>(r16(ctx, fmt + 4)), height = static_cast<s16>(r16(ctx, fmt + 6));
    const s32 pitch = static_cast<s32>(r32(ctx, fmt + 8));
    const u8 coding = r8(ctx, fmt);
    s32 row = width;
    if (const u8 c1 = static_cast<u8>(coding + 1); c1 <= 11) {
        const u64 bit = u64{1} << c1;
        if (bit & 0xFE8) row = static_cast<s32>(static_cast<u32>(width) << 1);
        else if (bit & 6) row = static_cast<s32>(static_cast<u32>(width) << 2);
        else if (bit & 1) return unsupported;
    }
    if (row > pitch || (row < pitch && (pitch & 31))) return unsupported;
    // The encoder's size: the feeder's, or the profile's when the slot may scale.
    s32 out_w = static_cast<s16>(r16(ctx, want + 4)), out_h = static_cast<s16>(r16(ctx, want + 6));
    if ((out_w == width || out_w == -1) && (out_h == height || out_h == -1)) {
        out_w = width;
        out_h = height;
        w8(ctx, slot + 0x42F, 0);
    } else {
        const u8 allowed = r8(ctx, slot + vs::kP73);
        if (!allowed) return unsupported;
        w8(ctx, slot + 0x42F, allowed);
    }
    w16(ctx, enc + 4, static_cast<u16>(out_w));
    w16(ctx, enc + 6, static_cast<u16>(out_h));
    // The pixel aspect: the profile's unspecified or the feeder's.
    const u32 want_sar = aspect_of(ctx, want), sar = aspect_of(ctx, fmt);
    if (!want_sar || !sar || (want_sar != 0xFFFFFFFF && want_sar != sar)) return unsupported;
    set_sar(ctx, params, sar_info(sar, c == 0x21 || c == 0x24, true));
    s8 matrix = static_cast<s8>(r8(ctx, want + 0x10));
    w32(ctx, enc + 0x14, sar);
    w8(ctx, enc + 0x11, r8(ctx, fmt + 0x11));
    const s8 fmt_matrix = static_cast<s8>(r8(ctx, fmt + 0x10));
    // The color matrix: the profile's, or (but for RGB and Bayer pictures) the feeder's when the profile has none.
    bool rgb = false;
    if (coding <= 12) {
        const u64 bit = u64{1} << coding;
        rgb = !(bit & 0x614) && (bit & 0x1003);
    }
    if (!rgb && matrix == -1) matrix = fmt_matrix;
    w8(ctx, enc + 0x10, static_cast<u8>(matrix));
    // The encoder's configuration.
    const u32 config = params + 0xA8, pipeline = slot + vs::kPipeline;
    s32 out_profile = want_profile, out_level = want_level, max_rate = want_max;
    u32 gop = static_cast<u32>(want_gop);
    if (c == 0x24) {
        if (want_profile != -1) return unsupported;
        if (out_level == -1) out_level = 2;
        if (r32(ctx, params + 0x18)) std::memset(gb(ctx) + config, 0, 0x18);
        const u32 pic = params + 0xC0;
        w32(ctx, pic, static_cast<u32>(out_w));
        w32(ctx, pic + 8, 0x1000000);
        w32(ctx, pic + 4, static_cast<u32>(out_h));
        s32 floor_rate = want_max;
        if (want_max <= 0) {
            if (out_level == 1) floor_rate = 8000000;
            else if (out_level == 2) floor_rate = 0x1400000;
            else return unsupported;
        }
        if (bitrate <= 0) {
            const u64 px = u64{static_cast<u32>(out_w)} * u64{static_cast<u32>(out_h)} * u64{ts};
            const s64 q = divs64(static_cast<s64>(px), static_cast<s64>(u64{(dur << 2) + dur}));
            u32 v = round_rate(static_cast<u64>(q));
            if (v > 20000000) v = 20000000;
            bitrate = static_cast<s32>(v);
        }
        w32(ctx, params + 0xD4, ts);
        w32(ctx, params + 0xCC, static_cast<u32>(bitrate));
        w32(ctx, params + 0xD0, dur);
        if (want_gop <= 0) gop = ts << 1;
        max_rate = bitrate >= floor_rate ? bitrate : floor_rate;
        const u32 g2 = params + 0xD8;
        w32(ctx, g2 + 0xC, 1);
        w32(ctx, g2 + 8, 0xFFFFFFFF);
        w32(ctx, config + 8, static_cast<u32>(out_level));
        w32(ctx, config, pic);
        w32(ctx, config + 4, g2);
        const u32 q = divu(gop, ts);
        w32(ctx, g2, q);
        w32(ctx, g2 + 4, gop - q * ts);
        std::memmove(gb(ctx) + config + 0xC, gb(ctx) + params + 0x1F8, 12);
        out_profile = -1;
    } else if (c == 0x21 || c == 0x22 || c == 0x23) {
        const bool m4 = c == 0x21;
        if (want_profile == -1) out_profile = 0x111;
        // The level from the size, the frame rate and the bit rate (estimated when the profile gives none).
        if (out_level == -1 && m4) {
            if (bitrate <= 0) {
                const u64 px = u64{static_cast<u32>(out_w)} * u64{static_cast<u32>(out_h)} * u64{ts};
                bitrate = static_cast<s32>(round_rate(static_cast<u64>(
                    divs64(static_cast<s64>(px), static_cast<s64>(u64{(dur << 3) - dur})))));
            }
            const u32 br = static_cast<u32>(bitrate), w = static_cast<u32>(out_w), h = static_cast<u32>(out_h);
            if (w > 720 || h > 576 || br > 2999999) out_level = 4;
            else if (w > 640 || h > 480 || br > 2000000) out_level = 0x12;
            else if (w > 352 || h > 288 || br > 384000) out_level = 0x11;
            else if (w > 176 || h > 144) out_level = 0x10;
            else out_level = br > 128000 ? 0x10 : 0x13;
        } else if (out_level == -1) {
            const u32 mbs = ((static_cast<u32>(out_w) + 15) >> 4) * ((static_cast<u32>(out_h) + 15) >> 4);
            const u32 mbps = static_cast<u32>(divu64(u64{mbs} * ts, dur));
            if (bitrate <= 0) {
                if (mbs > 3600 || mbps > 108000) {
                    const u64 px = u64{static_cast<u32>(out_w)} * u64{static_cast<u32>(out_h)} * u64{ts};
                    const s64 q = divs64(static_cast<s64>(px), static_cast<s64>(u64{dur << 2}));
                    bitrate = static_cast<s32>(divu(static_cast<u32>(q + 999999), 1000000) * 1000000);
                } else if (mbs > 1620 || mbps > 40500) {
                    bitrate = 8000000;
                } else if (mbs > 792 || mbps > 19800) {
                    bitrate = 2000000;
                } else if (mbs > 396 || mbps > 11880 || mbps > 6000) {
                    bitrate = 768000;
                } else {
                    bitrate = mbps > 3000 ? 384000 : 128000;
                }
            }
            const u32 br = static_cast<u32>(bitrate);
            if (mbs > 3600 || mbps > 108000 || br > 14000000) out_level = 0x200;
            else if (mbs > 1620 || mbps > 40500 || br > 10000000) out_level = 0x50;
            else if (mbs > 792 || mbps > 19800 || br > 4000000) out_level = 0x100;
            else if (mbs > 396 || mbps > 11880 || br > 768000) out_level = 7;
            else if (mbps > 6000 || br > 384000) out_level = 1;
            else if (mbps > 3000 || br > 192000) out_level = 5;
            else out_level = 0x31;
        }
        // The firmware's defaults (its configuration query) in a frame laid out as libsail_rec's: the GOP (+0x70 H.264,
        // +0x84 MPEG-4), the picture's settings (+0xCC, +0xB4), the query (+0x100, +0xE4), the options (+0x11C,
        // +0x21C) with the places two of them point to.
        StackVar frame(ctx, gb(ctx), 0x400);
        const u32 base = frame.addr();
        const u32 t = tables(ctx);
        const u32 gop_at = base + (m4 ? 0x84 : 0x70), pic = base + (m4 ? 0xB4 : 0xCC), q = base + (m4 ? 0xE4 : 0x100);
        const u32 opts = base + (m4 ? 0x21C : 0x11C), n = m4 ? kM4vOptionCount : kAvcOptionCount;
        const u32 ids = t + (m4 ? kM4vOptions : kAvcOptions);
        for (u32 i = 0; i < n; i++) w32(ctx, opts + 8 * i, r32(ctx, ids + 4 * i));
        if (m4) {
            w32(ctx, q + 0x14, n);
            w32(ctx, base + 0x248, base + 0x90);
            w32(ctx, q + 0x18, r32(ctx, slot + vs::kP76));
        } else {
            w32(ctx, base + 0x1C8, base + 0x9C);
            w32(ctx, q + 0x14, n);
            w32(ctx, base + 0x198, base + 0x78);
            w32(ctx, q + 0x18, r32(ctx, slot + vs::kP77));
        }
        w32(ctx, q, static_cast<u32>(out_profile));
        w32(ctx, q + 4, static_cast<u32>(out_level));
        w32(ctx, q + 8, pic);
        w32(ctx, q + 0xC, gop_at);
        w32(ctx, q + 0x10, opts);
        const s32 r = config_query(ctx, m4 ? 0x21 : 0x22, q);
        if (r < 0) return r;
        if (r32(ctx, params + 0x18)) std::memset(gb(ctx) + config, 0, 0x1C);
        const s32 frac = static_cast<s32>(r32(ctx, gop_at + 4)), query_ts = static_cast<s32>(r32(ctx, pic + 0x14));
        const u32 game_pic = r32(ctx, config + 8);
        w32(ctx, config, static_cast<u32>(out_profile));
        w32(ctx, pic, static_cast<u32>(out_w));
        w32(ctx, config + 4, static_cast<u32>(out_level));
        w32(ctx, pic + 4, static_cast<u32>(out_h));
        w32(ctx, pic + 0xC, static_cast<u32>(bitrate));
        w32(ctx, pic + 0x10, dur);
        w32(ctx, pic + 0x14, ts);
        w32(ctx, gop_at + 4, static_cast<u32>(divu64(static_cast<u64>(static_cast<s64>(frac)) * u64{ts},
                                  static_cast<u64>(static_cast<s64>(query_ts)))));
        if (game_pic && static_cast<s32>(r32(ctx, game_pic + 8)) > 0) w32(ctx, pic + 8, r32(ctx, game_pic + 8));
        std::memmove(gb(ctx) + params + 0xC4, gb(ctx) + pic, 0x18);
        if (want_gop > 0) {
            const u32 sec = divu(gop, ts);
            w32(ctx, gop_at, sec);
            w32(ctx, gop_at + 4, divu((gop - sec * ts) * ts, ts));
        }
        if (const u32 game_gop = r32(ctx, config + 0xC)) {
            const s32 g0 = static_cast<s32>(r32(ctx, game_gop)), g1 = static_cast<s32>(r32(ctx, game_gop + 4));
            if (g0 > 0 || g1 > 0) {
                w32(ctx, gop_at, static_cast<u32>(g0));
                w32(ctx, gop_at + 4, static_cast<u32>(g1));
            }
            if (m4 && static_cast<s32>(r32(ctx, game_gop + 8)) > 0) w32(ctx, gop_at + 8, r32(ctx, game_gop + 8));
        }
        // The GOP's length in the timescale: its seconds and its remainder (libsail_rec's arithmetic: scaled by the
        // timescale over itself).
        const u64 fraction = static_cast<u64>(static_cast<s64>(static_cast<s32>(r32(ctx, gop_at + 4))));
        gop = r32(ctx, gop_at) * ts +
              static_cast<u32>(divu64(u64{ts} * fraction, static_cast<u64>(static_cast<s64>(static_cast<s32>(ts)))));
        std::memmove(gb(ctx) + params + 0xDC, gb(ctx) + gop_at, m4 ? 0xC : 8);
        const u32 rate_value = m4 ? base + 0x228 : base + 0x148;
        if (want_max > 0) w32(ctx, rate_value, static_cast<u32>(want_max));
        w32(ctx, m4 ? base + 0x248 : base + 0x198, params + 0x1F8);
        // The game's configuration's options over the defaults (but the pixel aspect's).
        if (const u32 list = r32(ctx, config + 0x10); list && r32(ctx, config + 0x14)) {
            for (u32 k = 0; r32(ctx, config + 0x14) > k; k++) {
                const u32 id = r32(ctx, list + 8 * k);
                for (u32 i = 0; i < n; i++) {
                    if (r32(ctx, ids + 4 * i) != id || id == kAspectOption) continue;
                    w32(ctx, opts + 8 * i + 4, r32(ctx, list + 8 * k + 4));
                    break;
                }
            }
        }
        u32 count = n;
        u32 list_at;
        if (m4) {
            list_at = params + 0xE8;
            std::memmove(gb(ctx) + list_at, gb(ctx) + opts, 0x100);
        } else {
            // Options 0x3D94 and 0x154E 0, option 0x413 the slot's parameter 77 when option 0x412 is 1; H.264's video
            // usability information (option 0x345F): the color description by the matrix (BT.601 or BT.709) unless
            // it is unspecified (then the option left out).
            w32(ctx, base + 0x1B8, 0);
            w32(ctx, base + 0x1B0, 0);
            if (r32(ctx, base + 0x188) == 1) w32(ctx, base + 0x1C0, r32(ctx, slot + vs::kP77));
            const u32 vui = params + 0x204;
            std::memmove(gb(ctx) + vui, gb(ctx) + base + 0x9C, 0x18);
            w32(ctx, base + 0x1C8, vui);
            if (matrix == -1) {
                count = 21;
            } else {
                w32(ctx, vui + 8, 1);
                const u32 v = matrix == 0 ? 6 : 1;
                w32(ctx, vui + 0x14, v);
                w32(ctx, vui + 0xC, v);
                w32(ctx, vui + 0x10, v);
            }
            list_at = params + 0xE4;
            std::memmove(gb(ctx) + list_at, gb(ctx) + opts, 0x100);
        }
        max_rate = static_cast<s32>(r32(ctx, rate_value));
        w32(ctx, config + 8, params + 0xC4);
        w32(ctx, config + 0x14, count);
        w32(ctx, config + 0xC, params + 0xDC);
        w32(ctx, config + 0x10, list_at);
    } else {
        return error(sail::CELL_SAIL_ERROR_FATAL);
    }
    w32(ctx, params + 0x20, static_cast<u32>(out_profile));
    w32(ctx, params + 0x24, static_cast<u32>(out_level));
    w32(ctx, params + 0x30, static_cast<u32>(bitrate));
    w32(ctx, params + 0x2C, static_cast<u32>(max_rate));
    w32(ctx, params + 0x38, gop);
    w32(ctx, params + 0x34, ts);
    w32(ctx, params + 0xA0, dur);
    open(ctx, pipeline, slot, params, r32(ctx, profile + pf::kDelay), r32(ctx, profile + 0x458), handle);
    composer_add(ctx, rec + rc::kComposers, slot + vs::kComposerStream, params, pipeline, 0, handle, true);
    return 2;
}

void video_message(PPUContext& ctx, u32 rec, u32 code, u32 a, u32 b)
{
    const u32 cs = rec + rc::kComposers;
    switch (code) {
    case 0x1A: {
        // Frames out of the feeder `a`.
        const u32 p = r32(ctx, a + fd::kPipeline);
        feeder_frames_out(ctx, a, b);
        const u32 st = streams(ctx, rec);
        if (st == 4 || st == 6) pump_if(ctx, rec, a, p);
        return;
    }
    case 0x1B: {
        // The feeder's session ended (its end time): a recording slot stopped, outside Stop and the streams' stop.
        const u32 st = streams(ctx, rec);
        if (st - 5 <= 1 || st == 2) return;
        const u32 slot = r32(ctx, a + fd::kSlot);
        if (r8(ctx, slot + vs::kState) != 5) return;
        const u32 r = slot + 0x30;
        w8(ctx, slot + vs::kState, 6);
        std::memset(gb(ctx) + r, 0, 0x18);
        w32(ctx, r + 4, 1);
        w32(ctx, r + 0x10, slot);
        w64(ctx, r + 8, 0);
        w32(ctx, r, r32(ctx, r) | 2);
        feeder_stop(ctx, a, 0, 0, u64{r} << 32 | internal("_pspocket_cellSailRec_VideoFeederStopped"));
        return;
    }
    case 0x1E:
        // A pump asked by the pipeline `a`.
        if (streams(ctx, rec) - 4 > 2) return;
        pump_if(ctx, rec, feeder_of(ctx, a), a);
        return;
    case 0x1F: converted_for_scaler(ctx, a); return;
    case 0x20: converted(ctx, a); return;
    case 0x21: output(ctx, a); return;
    case 0x22:
        // The pipeline `a` gives the feeder's frame `b` back.
        feeder_release(ctx, feeder_of(ctx, a), b);
        return;
    case 0x23:
        // Units ready in the pipeline `a`.
        if (streams(ctx, rec) - 4 <= 2) deliver(ctx, a, cs, g(ctx, a, vp::kSlot) + vs::kComposerStream);
        return;
    case 0x2C: {
        // The composer gave `b` units of the stream entry `a` back.
        composer_released(ctx, cs, a, b);
        const u32 p = r32(ctx, a + se::kPipeline);
        released(ctx, p, b);
        const u32 f = feeder_of(ctx, p), st = streams(ctx, rec);
        if (st - 4 <= 2) deliver(ctx, p, cs, a);
        if (st == 4 || st == 6) pump_if(ctx, rec, f, p);
        return;
    }
    default: return;
    }
}

void register_video(Registry& r) { r.add_imports(kChan, kImports); }

} // namespace pspocket::hle::sailrec
