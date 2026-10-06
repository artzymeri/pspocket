// cellSysmodule: firmware module loading. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellSysmodule.cpp/.h (the module
// table, dependencies, default modules and error codes).
//
// Model: every firmware function the executable imports is already linked to its HLE implementation (or the
// UNIMPLEMENTED stub) at load time, so "loading" a module is bookkeeping: a per-module load count, dependencies loaded
// first and unloaded last, and the modules lv2 loads at process start (DEFAULT_MODULES) counted as loaded. That is
// RPCS3's logic with its sys_prx_load_module / start / stop / unload calls removed.
//
// Internal module IDs (0xF000 + index, D-179) as RPCS3 has them: its 87 internal modules with their dependencies, the
// ones the public functions may load (LOADABLE_INTERNAL_MODULES: 0xF035 is CELL_SYSMODULE_SYSUTIL_NP_TROPHY, which
// many games load; Sniper: Ghost Warrior on the phone got CELL_SYSMODULE_ERROR_UNKNOWN for it) and the ones the Ex
// functions may. 0xF020, 0xF024 and 0xF03E (VC-1, WMA, the ASF parser) need flags in the process's PARAM.SFO buffer,
// which lv2 leaves zero, as RPCS3's: they are refused, as there.
//
// Deviations: the debug/retail variants of libfs, libgcm_sys and liblv2dbg are one module each.

#include <array>
#include <initializer_list>
#include <mutex>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSysmodule";

enum : u32 {
    CELL_SYSMODULE_ERROR_DUPLICATED = 0x80012001,
    CELL_SYSMODULE_ERROR_UNKNOWN = 0x80012002,
    CELL_SYSMODULE_ERROR_UNLOADED = 0x80012003,
    CELL_SYSMODULE_ERROR_INVALID_MEMCONTAINER = 0x80012004,
    CELL_SYSMODULE_ERROR_FATAL = 0x800120FF,
};

constexpr u32 kNoContainer = 0xFFFFFFFF; // SYS_MEMORY_CONTAINER_ID_INVALID
constexpr u32 kContainerMinFree = 0x60000; // what a container needs free to take module loads (RPCS3)

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

// RPCS3 MODULE_INFOS' 87 internal modules (IDs 0xF000 + index): the file and its dependencies (public IDs, or internal
// ones from 0xF000), generated from cellSysmodule.cpp.
struct InternalInfo {
    const char* name;
    std::initializer_list<u16> dependencies;
};
const InternalInfo kInternal[] = {
    {"libat3enc", {}}, // 0xf000
    {"libatxenc", {}}, // 0xf001
    {"libvdec", {}}, // 0xf002
    {"libvpost", {}}, // 0xf003
    {"libmp3enc", {0x000e}}, // 0xf004
    {"libaacenc", {}}, // 0xf005
    {"libadec_internal", {}}, // 0xf006
    {"libapostsrc", {}}, // 0xf007
    {"libaudio_internal", {}}, // 0xf008
    {"libavchatjpgdec", {}}, // 0xf009
    {"libcelpenc", {}}, // 0xf00a
    {"libddlenc", {}}, // 0xf00b
    {"libdmux", {}}, // 0xf00c
    {"libexif", {0x000e}}, // 0xf00d
    {"libft2d", {}}, // 0xf00e
    {"libgcm_osd", {}}, // 0xf00f
    {"libgifdec", {0x000e}}, // 0xf010
    {"libjpgdec", {0x000e}}, // 0xf011
    {"libjpgenc", {0x000e}}, // 0xf012
    {"libm4venc", {}}, // 0xf013
    {"libpamf", {}}, // 0xf014
    {"libpngdec", {0x000e}}, // 0xf015
    {"libpngenc", {0x000e}}, // 0xf016
    {"libps2savedata", {}}, // 0xf017
    {"libtiffdec", {0x000e}}, // 0xf018
    {"libcelpdec", {0x002c}}, // 0xf019
    {"libdtsdec", {0x002c}}, // 0xf01a
    {"libm2bcdec", {0x002c}}, // 0xf01b
    {"libm2aacdec", {0x002c}}, // 0xf01c
    {"libm4aacdec", {0x002c}}, // 0xf01d
    {"libmp3dec", {0x002c}}, // 0xf01e
    {"libtrhddec", {0x002c}}, // 0xf01f
    {"libsvc1d", {0x002b}}, // 0xf020
    {"libsmvd4", {0x002b}}, // 0xf021
    {"libsysutil_remoteplay", {}}, // 0xf022
    {"libsysutil_imejp", {}}, // 0xf023
    {"libwmadec", {0x002c}}, // 0xf024
    {"libasfparser", {}}, // 0xf025
    {"libddpdec", {0x002c}}, // 0xf026
    {"libdtslbrdec", {0x002c}}, // 0xf027
    {"libsysutil_music", {}}, // 0xf028
    {"libsysutil_photo_export", {}}, // 0xf029
    {"libsysutil_print", {}}, // 0xf02a
    {"libsysutil_photo_import", {}}, // 0xf02b
    {"libsysutil_music_export", {}}, // 0xf02c
    {"libavcenc_small", {}}, // 0xf02d
    {"libsysutil_photo_decode", {}}, // 0xf02e
    {"libsysutil_search", {}}, // 0xf02f
    {"libsysutil_avc2", {}}, // 0xf030
    {"libmp4", {}}, // 0xf031
    {"libsysutil_rtcalarm", {}}, // 0xf032
    {"libavcenc_small", {}}, // 0xf033
    {"libsail_rec", {}}, // 0xf034
    {"libsysutil_np_trophy", {}}, // 0xf035
    {"libsjvtd", {0x002b}}, // 0xf036
    {"libdtshddec", {0x002c}}, // 0xf037
    {"libmp3sdec", {0x002c}}, // 0xf038
    {"libsail_avi", {}}, // 0xf039
    {"libavcenc", {}}, // 0xf03a
    {"libapostsrc_mini", {}}, // 0xf03b
    {"libvoice_internal", {}}, // 0xf03c
    {"libmpl1dec", {0x002c}}, // 0xf03d
    {"libasfparser2_astd", {}}, // 0xf03e
    {"libadec2", {}}, // 0xf03f
    {"libac3dec2", {0xf03f}}, // 0xf040
    {"libatxdec2", {0xf03f}}, // 0xf041
    {"libdivx311dec", {0x002b}}, // 0xf042
    {"libvpost2", {}}, // 0xf043
    {"libsysutil_np_eula", {}}, // 0xf044
    {"libsysutil_storagedata", {}}, // 0xf045
    {"libm4hdenc", {}}, // 0xf046
    {"libsysutil_savedata_psp", {}}, // 0xf047
    {"libsysutil_video_player", {}}, // 0xf048
    {"libmvcdec", {0x002b}}, // 0xf049
    {"libaacenc_spurs", {}}, // 0xf04a
    {"libdtshdcoredec", {0x002c}}, // 0xf04b
    {"libpidvd", {}}, // 0xf04c
    {"libsysutil_dtcp_ip", {}}, // 0xf04d
    {"libsysutil_syschat", {}}, // 0xf04e
    {"libsysutil_np_installer", {}}, // 0xf04f
    {"libbemp2sys", {}}, // 0xf050
    {"libbeisobmf", {}}, // 0xf051
    {"libsysutil_photo_export2", {}}, // 0xf052
    {"libat3multidec", {0x002c}}, // 0xf053
    {"libatrac3multi", {0xf053}}, // 0xf054
    {"libsysutil_dec_psnvideo", {}}, // 0xf055
    {"libdtslbrdec", {0x002c}}, // 0xf056
};
constexpr u16 kInternalBase = 0xF000;
constexpr u16 kInternalMask = 0x0FFF;
constexpr u16 kInternalCount = 87;
static_assert(std::size(kInternal) == kInternalCount);
// RPCS3 LOADABLE_INTERNAL_MODULES (through cellSysmoduleLoadModule and the rest) and LOADABLE_INTERNAL_MODULES_EX
// (through the Ex functions), by index.
constexpr std::string_view kLoadable = "000000000010000010000000010101100001000011111011100011000000000000001000000000000000100";
constexpr std::string_view kLoadableEx = "000001000010000010010000010101101111101011111011101111100110011111011111111001111110111";
constexpr u16 kNpTrophy = 0xF035; // CELL_SYSMODULE_SYSUTIL_NP_TROPHY: loaded by lv2 from SDK 3.3 on (DEFAULT_MODULES)

bool internal_id(u16 id) { return id >= kInternalBase && (id & kInternalMask) < kInternalCount; }

// RPCS3 DEFAULT_MODULES: SYSUTIL, GCM_SYS, AUDIO, IO, SPURS, and from SDK 3.3 on FS and the internal NP trophy
// module (not tracked here).
constexpr u16 kDefaultModules[] = {0x15, 0x10, 0x11, 0x17, 0x0A, 0x0E};

struct SysmoduleState {
    std::mutex mutex;
    std::array<s32, kExternalCount + kInternalCount> loaded{}; // external IDs, then internal ones by index
    u32 container = kNoContainer; // cellSysmoduleSetMemcontainer's: where later loads would put their PRX images

    explicit SysmoduleState(HleProcess& p)
    {
        const bool sdk_330 = p.kernel().config().sdk_version >= 0x330000;
        for (usize i = 0; i < std::size(kDefaultModules); i++)
            if (i < 5 || sdk_330) loaded[kDefaultModules[i]] = 1;
        if (sdk_330) loaded[kExternalCount + (kNpTrophy & kInternalMask)] = 1;
    }
};

SysmoduleState& state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSysmodule called without an HLE process");
    return p->state<SysmoduleState>();
}

const char* name_of(u16 id)
{
    return id < kExternalCount ? kModules[id].name : internal_id(id) ? kInternal[id & kInternalMask].name : "unknown";
}

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

s32 unknown(const char* what, u16 id)
{
    PS_LOG_WARN(kChan, "{}({:#06x}): not a module this call takes -> CELL_SYSMODULE_ERROR_UNKNOWN", what, id);
    return error(CELL_SYSMODULE_ERROR_UNKNOWN);
}

// RPCS3 cellSysmoduleLoadModuleInternal / UnloadModuleInternal / IsLoadedEx, without the PRX calls.
s32 load_internal(SysmoduleState& s, u16 id)
{
    if (!internal_id(id)) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    if (id == 0xF024 || id == 0xF03E || id == 0xF020) return error(CELL_SYSMODULE_ERROR_UNKNOWN); // PARAM.SFO flags
    for (const u16 dep : kInternal[id & kInternalMask].dependencies) {
        const s32 r = dep < kInternalBase ? load(s, dep) : load_internal(s, dep);
        if (r != lv2::CELL_OK && r != error(CELL_SYSMODULE_ERROR_DUPLICATED)) return r;
    }
    std::lock_guard lock(s.mutex);
    s32& count = s.loaded[kExternalCount + (id & kInternalMask)];
    if (count == 0x7FFFFFFF) return error(CELL_SYSMODULE_ERROR_FATAL);
    count++;
    return lv2::CELL_OK;
}

s32 unload_internal(SysmoduleState& s, u16 id)
{
    if (!internal_id(id)) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    {
        std::lock_guard lock(s.mutex);
        s32& count = s.loaded[kExternalCount + (id & kInternalMask)];
        if (count > 0) count--;
    }
    const auto& deps = kInternal[id & kInternalMask].dependencies;
    for (auto it = std::rbegin(deps); it != std::rend(deps); ++it) {
        if (const s32 r = *it < kInternalBase ? unload(s, *it) : unload_internal(s, *it); r != lv2::CELL_OK) return r;
    }
    return lv2::CELL_OK;
}

s32 is_loaded_internal(SysmoduleState& s, u16 id)
{
    if (!internal_id(id)) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    std::lock_guard lock(s.mutex);
    return s.loaded[kExternalCount + (id & kInternalMask)] > 0 ? lv2::CELL_OK : error(CELL_SYSMODULE_ERROR_UNLOADED);
}

// Through the public functions, the internal modules RPCS3 lets them load.
bool publicly_loadable(u16 id) { return internal_id(id) && kLoadable[id & kInternalMask] == '1'; }
bool ex_loadable(u16 id) { return internal_id(id) && kLoadableEx[id & kInternalMask] == '1'; }

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
    if (id >= kExternalCount)
        return publicly_loadable(id) ? load_internal(state(), id) : unknown("cellSysmoduleLoadModule", id);
    if (id == kSyschat) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    return load(state(), id);
}

s32 cellSysmoduleUnloadModule(PPUContext&, u16 id)
{
    PS_LOG_INFO(kChan, "cellSysmoduleUnloadModule({:#06x} {})", id, name_of(id));
    if (id >= kExternalCount)
        return publicly_loadable(id) ? unload_internal(state(), id) : unknown("cellSysmoduleUnloadModule", id);
    if (id == kSyschat) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    return unload(state(), id);
}

s32 cellSysmoduleIsLoaded(PPUContext&, u16 id)
{
    if (id >= kExternalCount)
        return publicly_loadable(id) ? is_loaded_internal(state(), id) : unknown("cellSysmoduleIsLoaded", id);
    if (id == kSyschat) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    // A virtual module is loaded when its last dependency is (RPCS3).
    const u16 m = kModules[id].has_image ? id : *(kModules[id].dependencies.end() - 1);
    SysmoduleState& s = state();
    std::lock_guard lock(s.mutex);
    const bool loaded = s.loaded[m] > 0;
    PS_LOG_DEBUG(kChan, "cellSysmoduleIsLoaded({:#06x} {}) -> {}", id, name_of(id), loaded ? "loaded" : "unloaded");
    return loaded ? lv2::CELL_OK : error(CELL_SYSMODULE_ERROR_UNLOADED);
}

// cellSysmoduleSetMemcontainer(ct_id), as RPCS3: SYS_MEMORY_CONTAINER_ID_INVALID goes back to no container; any other
// ID must be a memory container with 0x60000 bytes free (sys_memory_container_get_size), or the call fails with
// INVALID_MEMCONTAINER and no container is used. The container would hold the PRX images of later loads; HLE modules
// have none, so it is only remembered. GTA V calls it with a container it made, and asserts (tw 31,r1,r1) on an error
// (phone run of 2026-10-05: unimplemented, the game stopped at 0xa3a16c).
s32 cellSysmoduleSetMemcontainer(PPUContext& ctx, u32 ct_id)
{
    PS_LOG_INFO(kChan, "cellSysmoduleSetMemcontainer({:#x})", ct_id);
    SysmoduleState& s = state();
    bool usable = ct_id == kNoContainer;
    if (!usable) {
        u8* base = guest_base(ctx);
        StackVar info(ctx, base, 8); // sys_memory_info_t {total_user_memory, available_user_memory}
        usable = lv2::sys_memory_container_get_size(thread_of(ctx), info.addr(), ct_id) == lv2::CELL_OK &&
            ps_read32(base, info.addr() + 4) >= kContainerMinFree;
    }
    std::lock_guard lock(s.mutex);
    s.container = usable ? ct_id : kNoContainer;
    return usable ? lv2::CELL_OK : error(CELL_SYSMODULE_ERROR_INVALID_MEMCONTAINER);
}

// The internal functions, as RPCS3: any internal module by its ID; the Ex ones only those LOADABLE_INTERNAL_MODULES_EX
// lists (else as an invalid ID).
s32 cellSysmoduleLoadModuleInternal(PPUContext&, u16 id)
{
    PS_LOG_INFO(kChan, "cellSysmoduleLoadModuleInternal({:#06x} {})", id, name_of(id));
    return load_internal(state(), id);
}

s32 cellSysmoduleUnloadModuleInternal(PPUContext&, u16 id)
{
    PS_LOG_INFO(kChan, "cellSysmoduleUnloadModuleInternal({:#06x} {})", id, name_of(id));
    return unload_internal(state(), id);
}

s32 cellSysmoduleIsLoadedEx(PPUContext&, u16 id) { return is_loaded_internal(state(), id); }

s32 cellSysmoduleLoadModuleEx(PPUContext&, u16 id)
{
    PS_LOG_INFO(kChan, "cellSysmoduleLoadModuleEx({:#06x} {})", id, name_of(id));
    return ex_loadable(id) ? load_internal(state(), id) : error(CELL_SYSMODULE_ERROR_UNKNOWN);
}

s32 cellSysmoduleUnloadModuleEx(PPUContext&, u16 id)
{
    PS_LOG_INFO(kChan, "cellSysmoduleUnloadModuleEx({:#06x} {})", id, name_of(id));
    return ex_loadable(id) ? unload_internal(state(), id) : error(CELL_SYSMODULE_ERROR_UNKNOWN);
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
        PS_HLE_FUNC(0xA193143C, cellSysmoduleSetMemcontainer),
        PS_HLE_FUNC(0xD9B8C0EE, cellSysmoduleLoadModuleInternal),
        PS_HLE_FUNC(0xB498BF77, cellSysmoduleUnloadModuleInternal),
        PS_HLE_FUNC(0x70E319B5, cellSysmoduleIsLoadedEx),
        PS_HLE_FUNC(0x1A267F98, cellSysmoduleLoadModuleEx),
        PS_HLE_FUNC(0xFD0E1CAC, cellSysmoduleUnloadModuleEx),
    });
}

} // namespace pspocket::hle
