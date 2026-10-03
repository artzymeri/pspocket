// cellSysmodule: firmware module loading. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellSysmodule.cpp/.h (the module
// table, dependencies, default modules and error codes).
//
// Model: every firmware function the executable imports is already linked to its HLE implementation (or the
// UNIMPLEMENTED stub) at load time, so "loading" a module is bookkeeping: a per-module load count, dependencies loaded
// first and unloaded last, and the modules lv2 loads at process start (DEFAULT_MODULES) counted as loaded. That is
// RPCS3's logic with its sys_prx_load_module / start / stop / unload calls removed.
//
// Deviations: internal module IDs (0xF000 and up) return CELL_SYSMODULE_ERROR_UNKNOWN with an error log until a game
// is seen loading one; the debug/retail variants of libfs, libgcm_sys and liblv2dbg are one module each;
// cellSysmoduleSetMemcontainer is not implemented.

#include <array>
#include <initializer_list>
#include <mutex>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSysmodule";

enum : u32 {
    CELL_SYSMODULE_ERROR_DUPLICATED = 0x80012001,
    CELL_SYSMODULE_ERROR_UNKNOWN = 0x80012002,
    CELL_SYSMODULE_ERROR_UNLOADED = 0x80012003,
    CELL_SYSMODULE_ERROR_FATAL = 0x800120FF,
};

constexpr s32 error(u32 code) { return static_cast<s32>(code); }

struct ModuleInfo {
    const char* name;
    bool has_image; // false: a virtual module that stands for its dependencies (RPCS3: empty path)
    std::initializer_list<u16> dependencies;
};

// RPCS3 MODULE_INFOS, the 93 external modules (IDs 0x00..0x5C), generated from cellSysmodule.cpp.
const ModuleInfo kModules[] = {
    {"NET", true, {0x14}}, // 0x00
    {"HTTP", true, {0x00, 0x09}}, // 0x01
    {"HTTP_UTIL", false, {0x01}}, // 0x02
    {"SSL", true, {0x00, 0x09, 0x0E}}, // 0x03
    {"HTTPS", false, {0x03, 0x01}}, // 0x04
    {"VDEC", false, {0x24, 0x25, 0x2B}}, // 0x05
    {"ADEC", false, {0x26, 0x27, 0x29, 0x28, 0x2C}}, // 0x06
    {"DMUX", false, {0x2A, 0x2D}}, // 0x07
    {"VPOST", true, {}}, // 0x08
    {"RTC", true, {}}, // 0x09
    {"SPURS", true, {}}, // 0x0A
    {"OVIS", false, {0x0A}}, // 0x0B
    {"SHEAP", false, {0x0A}}, // 0x0C
    {"SYNC", false, {0x0A}}, // 0x0D
    {"FS", true, {}}, // 0x0E
    {"JPGDEC", true, {0x0E}}, // 0x0F
    {"GCM_SYS", true, {}}, // 0x10
    {"AUDIO", true, {}}, // 0x11
    {"PAMF", true, {}}, // 0x12
    {"ATRAC3PLUS", true, {0x29, 0x28}}, // 0x13
    {"NETCTL", true, {}}, // 0x14
    {"SYSUTIL", true, {}}, // 0x15
    {"SYSUTIL_NP", true, {}}, // 0x16
    {"IO", true, {}}, // 0x17
    {"PNGDEC", true, {0x0E}}, // 0x18
    {"FONT", true, {0x0E}}, // 0x19
    {"FONTFT", true, {}}, // 0x1A
    {"FREETYPE", true, {0x0E}}, // 0x1B
    {"USBD", true, {}}, // 0x1C
    {"SAIL", true, {}}, // 0x1D
    {"L10N", true, {}}, // 0x1E
    {"RESC", true, {}}, // 0x1F
    {"DAISY", false, {0x0A}}, // 0x20
    {"KEY2CHAR", true, {}}, // 0x21
    {"MIC", true, {}}, // 0x22
    {"CAMERA", true, {}}, // 0x23
    {"VDEC_MPEG2", true, {0x2B}}, // 0x24
    {"VDEC_AVC", true, {0x2B}}, // 0x25
    {"ADEC_LPCM", false, {0x2C}}, // 0x26
    {"ADEC_AC3", true, {0x2C}}, // 0x27
    {"ADEC_ATX", true, {0x2C}}, // 0x28
    {"ADEC_AT3", true, {0x2C}}, // 0x29
    {"DMUX_PAMF", true, {0x2D}}, // 0x2A
    {"VDEC_AL", true, {}}, // 0x2B
    {"ADEC_AL", true, {}}, // 0x2C
    {"DMUX_AL", true, {}}, // 0x2D
    {"LV2DBG", true, {}}, // 0x2E
    {"libsysutil_avc_ext", true, {}}, // 0x2F
    {"USBPSPCM", true, {0x1C}}, // 0x30
    {"AVCONF_EXT", true, {}}, // 0x31
    {"SYSUTIL_USERINFO", true, {}}, // 0x32
    {"SYSUTIL_SAVEDATA", true, {}}, // 0x33
    {"SUBDISPLAY", true, {}}, // 0x34
    {"SYSUTIL_REC", true, {}}, // 0x35
    {"VIDEO_EXPORT", true, {}}, // 0x36
    {"SYSUTIL_GAME_EXEC", true, {}}, // 0x37
    {"SYSUTIL_NP2", true, {0x16}}, // 0x38
    {"SYSUTIL_AP", true, {}}, // 0x39
    {"SYSUTIL_NP_CLANS", true, {0x16, 0x04}}, // 0x3A
    {"SYSUTIL_OSK_EXT", true, {}}, // 0x3B
    {"VDEC_DIVX", true, {0x2B}}, // 0x3C
    {"JPGENC", true, {0x0E}}, // 0x3D
    {"SYSUTIL_GAME", true, {}}, // 0x3E
    {"BGDL", true, {}}, // 0x3F
    {"FREETYPE_TT", true, {0x0E}}, // 0x40
    {"SYSUTIL_VIDEO_UPLOAD", true, {}}, // 0x41
    {"SYSUTIL_SYSCONF_EXT", true, {}}, // 0x42
    {"FIBER", true, {}}, // 0x43
    {"SYSUTIL_NP_COMMERCE2", true, {0x38, 0x04}}, // 0x44
    {"SYSUTIL_NP_TUS", true, {0x38}}, // 0x45
    {"VOICE", true, {}}, // 0x46
    {"ADEC_CELP8", true, {0x2C}}, // 0x47
    {"CELP8ENC", true, {}}, // 0x48
    {"SYSUTIL_LICENSEAREA", true, {}}, // 0x49
    {"SYSUTIL_MUSIC2", true, {}}, // 0x4A
    {"libad_core", true, {0x0E, 0x00, 0x16}}, // 0x4B
    {"libad_async", true, {0x4B}}, // 0x4C
    {"libad_billboard_util", true, {0x0E, 0x00, 0x16}}, // 0x4D
    {"SYSUTIL_SCREENSHOT", true, {}}, // 0x4E
    {"SYSUTIL_MUSIC_DECODE", true, {}}, // 0x4F
    {"SPURS_JQ", true, {0x43}}, // 0x50
    {"libsysutil_authdialog", true, {}}, // 0x51
    {"PNGENC", true, {0x0E}}, // 0x52
    {"SYSUTIL_MUSIC_DECODE2", true, {}}, // 0x53
    {"libmedi", true, {0x00, 0x16}}, // 0x54
    {"SYNC2", true, {0x43}}, // 0x55
    {"SYSUTIL_NP_UTIL", true, {}}, // 0x56
    {"RUDP", true, {}}, // 0x57
    {"libsysutil_syschat", true, {}}, // 0x58
    {"SYSUTIL_NP_SNS", true, {}}, // 0x59
    {"GEM", true, {}}, // 0x5A
    {"libsysutil_photo_network_sharing", true, {}}, // 0x5B
    {"SYSUTIL_CROSS_CONTROLLER", true, {}}, // 0x5C
};
constexpr u16 kExternalCount = 93;
static_assert(std::size(kModules) == kExternalCount);

constexpr u16 kSyschat = 0x58; // listed, but load / unload / is-loaded refuse it (RPCS3)

// RPCS3 DEFAULT_MODULES: SYSUTIL, GCM_SYS, AUDIO, IO, SPURS, and from SDK 3.3 on FS and the internal NP trophy
// module (not tracked here).
constexpr u16 kDefaultModules[] = {0x15, 0x10, 0x11, 0x17, 0x0A, 0x0E};

struct SysmoduleState {
    std::mutex mutex;
    std::array<s32, kExternalCount> loaded{};

    explicit SysmoduleState(HleProcess& p)
    {
        const bool sdk_330 = p.kernel().config().sdk_version >= 0x330000;
        for (usize i = 0; i < std::size(kDefaultModules); i++)
            if (i < 5 || sdk_330) loaded[kDefaultModules[i]] = 1;
    }
};

SysmoduleState& state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSysmodule called without an HLE process");
    return p->state<SysmoduleState>();
}

const char* name_of(u16 id) { return id < kExternalCount ? kModules[id].name : "internal"; }

// RPCS3 load_module / unload_module without the PRX calls.
s32 load_one(SysmoduleState& s, u16 id)
{
    if (!kModules[id].has_image) return lv2::CELL_OK;
    std::lock_guard lock(s.mutex);
    if (s.loaded[id] == 0x7FFFFFFF) return error(CELL_SYSMODULE_ERROR_FATAL);
    s.loaded[id]++;
    return lv2::CELL_OK;
}

void unload_one(SysmoduleState& s, u16 id)
{
    if (!kModules[id].has_image) return;
    std::lock_guard lock(s.mutex);
    if (s.loaded[id] > 0) s.loaded[id]--;
}

s32 load(SysmoduleState& s, u16 id)
{
    for (const u16 dep : kModules[id].dependencies) {
        const s32 r = load(s, dep);
        if (r != lv2::CELL_OK && r != error(CELL_SYSMODULE_ERROR_DUPLICATED)) return r;
    }
    return load_one(s, id);
}

s32 unload(SysmoduleState& s, u16 id)
{
    // A default module loaded only by lv2 stays loaded.
    for (const u16 d : kDefaultModules) {
        std::lock_guard lock(s.mutex);
        if (d == id && s.loaded[id] == 1) return lv2::CELL_OK;
    }
    unload_one(s, id);
    const auto& deps = kModules[id].dependencies;
    for (auto it = std::rbegin(deps); it != std::rend(deps); ++it) {
        if (const s32 r = unload(s, *it); r != lv2::CELL_OK) return r;
    }
    return lv2::CELL_OK;
}

s32 unknown_internal(const char* what, u16 id)
{
    PS_LOG_ERROR(kChan, "{}({:#06x}): internal module IDs are not implemented -> CELL_SYSMODULE_ERROR_UNKNOWN", what, id);
    return error(CELL_SYSMODULE_ERROR_UNKNOWN);
}

s32 cellSysmoduleInitialize(PPUContext&)
{
    PS_LOG_INFO(kChan, "cellSysmoduleInitialize()");
    return lv2::CELL_OK;
}

s32 cellSysmoduleFinalize(PPUContext&)
{
    PS_LOG_INFO(kChan, "cellSysmoduleFinalize()");
    return lv2::CELL_OK;
}

s32 cellSysmoduleLoadModule(PPUContext&, u16 id)
{
    PS_LOG_INFO(kChan, "cellSysmoduleLoadModule({:#06x} {})", id, name_of(id));
    if (id >= kExternalCount) return unknown_internal("cellSysmoduleLoadModule", id);
    if (id == kSyschat) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    return load(state(), id);
}

s32 cellSysmoduleUnloadModule(PPUContext&, u16 id)
{
    PS_LOG_INFO(kChan, "cellSysmoduleUnloadModule({:#06x} {})", id, name_of(id));
    if (id >= kExternalCount) return unknown_internal("cellSysmoduleUnloadModule", id);
    if (id == kSyschat) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    return unload(state(), id);
}

s32 cellSysmoduleIsLoaded(PPUContext&, u16 id)
{
    if (id >= kExternalCount) return unknown_internal("cellSysmoduleIsLoaded", id);
    if (id == kSyschat) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    // A virtual module is loaded when its last dependency is (RPCS3).
    const u16 m = kModules[id].has_image ? id : *(kModules[id].dependencies.end() - 1);
    SysmoduleState& s = state();
    std::lock_guard lock(s.mutex);
    const bool loaded = s.loaded[m] > 0;
    PS_LOG_DEBUG(kChan, "cellSysmoduleIsLoaded({:#06x} {}) -> {}", id, name_of(id), loaded ? "loaded" : "unloaded");
    return loaded ? lv2::CELL_OK : error(CELL_SYSMODULE_ERROR_UNLOADED);
}

} // namespace

void register_cellSysmodule(Registry& r)
{
    r.add("cellSysmodule", {
        PS_HLE_FUNC(0x63FF6FF9, cellSysmoduleInitialize),
        PS_HLE_FUNC(0x96C07ADF, cellSysmoduleFinalize),
        PS_HLE_FUNC(0x32267A31, cellSysmoduleLoadModule),
        PS_HLE_FUNC(0x112A5EE9, cellSysmoduleUnloadModule),
        PS_HLE_FUNC(0x5A59E258, cellSysmoduleIsLoaded),
    });
}

} // namespace pspocket::hle
