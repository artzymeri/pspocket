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
//
// RPCS3's two names past the module calls (D-639): its variable s_sysmodule_context, RPCS3's copy of libsysmodule's
// own data in guest memory, exported with RPCS3's layout and kept in step with the state here once something links it;
// and the export 0x59521326, the console's OpenPSID decrypted with a key libsysmodule holds, as RPCS3 makes it.

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <initializer_list>
#include <mutex>
#include <string_view>

#include "guest.hpp"
#include "prx.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/crypto.hpp"
#include "pspocket/recomp/sysmodule_key.hpp"

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

// RPCS3's s_sysmodule_context (cellSysmodule.cpp sysmodule_context, D-639): sys_lwmutex_t mutex (24 bytes), be u32
// unk_addr at 0x18, u32 mem_container_id at 0x1C, b8 use_mem_container at 0x20, module_state module_states[189] at 0x24
// (s32 loaded_count, s32 prx_id), char module_base_path[16] at 0x60C, u8 module_base_path_size at 0x61C: 0x620 bytes,
// aligned to 8 (the lwmutex's). The 189 states are the 93 public modules, the 87 internal ones (from 93, the order of
// SysmoduleState::loaded) and RPCS3's nine variants (liblv2dbg for DEX and CEX at 180 and 181, libgcm_sys_deh and
// libgcm_sys at 182 and 183, libfs and libfs_155 at 184 and 185, libprof, liblv2coredump and libgpad at 186 to 188).
constexpr u32 kContextNid = 0xA1B6F70B, kContextSize = 0x620, kContextAlign = 8;
constexpr u32 kCtxContainerId = 0x1C, kCtxUseContainer = 0x20, kCtxStates = 0x24, kCtxBasePath = 0x60C,
              kCtxBasePathSize = 0x61C;
constexpr u32 kContextStates = 189;
constexpr u16 kFs = 0x0E, kGcmSys = 0x10, kLv2Dbg = 0x2E;
// The variants RPCS3 keeps in step with those three for a retail console's game: liblv2dbg_for_cex, libgcm_sys, libfs.
constexpr u32 kLv2DbgCex = 181, kGcmSysRetail = 183, kFsRetail = 184;
constexpr std::string_view kBasePath = "/dev_flash/sys/", kBasePathDebug = "/app_home/"; // RPCS3 MODULE_BASE_PATH*

struct SysmoduleState {
    std::mutex mutex;
    std::array<s32, kExternalCount + kInternalCount> loaded{}; // external IDs, then internal ones by index
    u32 container = kNoContainer; // cellSysmoduleSetMemcontainer's: where later loads would put their PRX images
    u32 last_container = 0;       // the last container taken, which s_sysmodule_context keeps when none is used
    bool debug_mode = false;      // cellSysmoduleSetDebugmode(1): modules from /app_home/ (recorded only)
    std::recursive_mutex file_mutex; // the module file calls (RPCS3's lwmutex; module_start may load more)
    // s_sysmodule_context's address once something has linked it (0 before: it is laid out only then), under `mutex`.
    u32 context = 0;
    u8* context_base = nullptr;

    explicit SysmoduleState(HleProcess& p)
    {
        const bool sdk_330 = p.kernel().config().sdk_version >= 0x330000;
        for (usize i = 0; i < std::size(kDefaultModules); i++)
            if (i < 5 || sdk_330) loaded[kDefaultModules[i]] = 1;
        if (sdk_330) loaded[kExternalCount + (kNpTrophy & kInternalMask)] = 1;
    }
};

// ---- s_sysmodule_context, kept in step with the state (under its mutex)

void publish_count(SysmoduleState& s, usize index)
{
    if (!s.context) return;
    const auto put = [&](u32 state) {
        ps_write32(s.context_base, s.context + kCtxStates + 8 * state, static_cast<u32>(s.loaded[index]));
    };
    put(static_cast<u32>(index));
    if (index == kLv2Dbg) put(kLv2DbgCex);
    if (index == kGcmSys) put(kGcmSysRetail);
    if (index == kFs) put(kFsRetail);
}

void publish_container(SysmoduleState& s)
{
    if (!s.context) return;
    ps_write32(s.context_base, s.context + kCtxContainerId, s.last_container);
    ps_write8(s.context_base, s.context + kCtxUseContainer, s.container != kNoContainer ? 1 : 0);
}

// As RPCS3's cellSysmoduleSetDebugmode: the path and its NUL over the one there, then its size.
void publish_base_path(SysmoduleState& s)
{
    if (!s.context) return;
    const std::string_view path = s.debug_mode ? kBasePathDebug : kBasePath;
    std::memcpy(s.context_base + s.context + kCtxBasePath, path.data(), path.size());
    ps_write8(s.context_base, s.context + kCtxBasePath + static_cast<u32>(path.size()), 0);
    ps_write8(s.context_base, s.context + kCtxBasePathSize, static_cast<u8>(path.size()));
}

// The variable's layout (HleProcess::link_variable: zeroed, then this, once): RPCS3's own (its REG_VAR init: no memory
// taken, no container, every module unloaded with no PRX, the retail modules' path), then the state as it stands.
void init_sysmodule_context(u8* base, u32 addr)
{
    for (u32 i = 0; i < kContextStates; i++) ps_write32(base, addr + kCtxStates + 8 * i + 4, 0xFFFFFFFF); // prx_id
    std::memcpy(base + addr + kCtxBasePath, kBasePath.data(), kBasePath.size());
    ps_write8(base, addr + kCtxBasePathSize, static_cast<u8>(kBasePath.size()));
    HleProcess* p = HleProcess::current();
    if (!p) return;
    SysmoduleState& s = p->state<SysmoduleState>();
    std::lock_guard lock(s.mutex);
    s.context = addr;
    s.context_base = base;
    for (usize i = 0; i < s.loaded.size(); i++) publish_count(s, i);
    publish_container(s);
    publish_base_path(s);
}

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
    publish_count(s, id);
    return lv2::CELL_OK;
}

void unload_one(SysmoduleState& s, u16 id)
{
    if (!kModules[id].has_image) return;
    std::lock_guard lock(s.mutex);
    if (s.loaded[id] > 0) s.loaded[id]--;
    publish_count(s, id);
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
    const usize index = kExternalCount + (id & kInternalMask);
    if (s.loaded[index] == 0x7FFFFFFF) return error(CELL_SYSMODULE_ERROR_FATAL);
    s.loaded[index]++;
    publish_count(s, index);
    return lv2::CELL_OK;
}

s32 unload_internal(SysmoduleState& s, u16 id)
{
    if (!internal_id(id)) return error(CELL_SYSMODULE_ERROR_UNKNOWN);
    {
        std::lock_guard lock(s.mutex);
        const usize index = kExternalCount + (id & kInternalMask);
        if (s.loaded[index] > 0) s.loaded[index]--;
        publish_count(s, index);
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

// libsysmodule's internal mode switch: it takes the mode and does nothing with it (fw 4.92's answers CELL_OK).
s32 cellSysmoduleSetInternalmode(PPUContext&, u32 mode)
{
    PS_LOG_DEBUG(kChan, "cellSysmoduleSetInternalmode({:#x})", mode);
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
    if (s.container != kNoContainer) s.last_container = ct_id;
    publish_container(s);
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

// ---- module files, images and the debug mode (RPCS3)

// SysmoduleUnk: be s32 prx_id, unk.
constexpr u32 kSysmoduleUnkSize = 8;

// cellSysmoduleLoadModuleFile(path, SysmoduleUnk*): loads and starts a module file by path (RPCS3: sys_prx_load_module
// and sys_prx_start_module under the library's mutex). The ID goes to unk->prx_id even when it is an error.
s32 cellSysmoduleLoadModuleFile(PPUContext& ctx, u32 path, u32 unk)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!unk || !guest_readable(t, unk, kSysmoduleUnkSize)) {
        log_call_site(ctx, guest_base(ctx), "cellSysmoduleLoadModuleFile: the SysmoduleUnk is not guest memory");
        return error(CELL_SYSMODULE_ERROR_FATAL);
    }
    std::lock_guard lock(state().file_mutex);
    u8* base = guest_base(ctx);
    const s32 id = prx_load_module(ctx, path, 0);
    ps_write32(base, unk, static_cast<u32>(id));
    ps_write32(base, unk + 4, 0xFFFFFFFF);
    if (id < 1) {
        PS_LOG_ERROR(kChan, "cellSysmoduleLoadModuleFile: loading failed ({:#010x})", static_cast<u32>(id));
        return error(CELL_SYSMODULE_ERROR_FATAL);
    }
    StackVar result(ctx, base, 4);
    if (const s32 r = prx_start_module(ctx, static_cast<u32>(id), 0, 0, result.addr()); r != lv2::CELL_OK) {
        prx_unload_module(ctx, static_cast<u32>(id));
        PS_LOG_ERROR(kChan, "cellSysmoduleLoadModuleFile: starting module {:#x} failed ({:#010x})", id,
            static_cast<u32>(r));
        return r;
    }
    PS_LOG_INFO(kChan, "cellSysmoduleLoadModuleFile: module {:#x} loaded and started", id);
    return lv2::CELL_OK;
}

// cellSysmoduleUnloadModuleFile(const SysmoduleUnk*): stops and unloads it; any failure is FATAL.
s32 cellSysmoduleUnloadModuleFile(PPUContext& ctx, u32 unk)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!unk || !guest_readable(t, unk, kSysmoduleUnkSize)) {
        log_call_site(ctx, guest_base(ctx), "cellSysmoduleUnloadModuleFile: the SysmoduleUnk is not guest memory");
        return error(CELL_SYSMODULE_ERROR_FATAL);
    }
    std::lock_guard lock(state().file_mutex);
    u8* base = guest_base(ctx);
    const u32 id = ps_read32(base, unk);
    StackVar result(ctx, base, 4);
    if (prx_stop_module(ctx, id, 0, 0, result.addr()) != lv2::CELL_OK || prx_unload_module(ctx, id) != lv2::CELL_OK) {
        PS_LOG_ERROR(kChan, "cellSysmoduleUnloadModuleFile({:#x}): stopping or unloading failed", id);
        return error(CELL_SYSMODULE_ERROR_FATAL);
    }
    return lv2::CELL_OK;
}

// cellSysmoduleSetDebugmode(mode): where modules load from (mode 1: /app_home/, else /dev_flash/sys/). Modules are
// linked at load time here (see the top of the file), so the path is only recorded.
s32 cellSysmoduleSetDebugmode(PPUContext&, u32 mode)
{
    SysmoduleState& s = state();
    std::lock_guard lock(s.mutex);
    s.debug_mode = mode == 1;
    publish_base_path(s);
    PS_LOG_INFO(kChan, "cellSysmoduleSetDebugmode({}): modules from {}", mode,
        s.debug_mode ? kBasePathDebug : kBasePath);
    return lv2::CELL_OK;
}

// The images of IDs 0xFF00 and up: 0xFF00 is the firmware's Flash ATRAC picture; 0xFF01 has no path (RPCS3: the
// string is empty on the PS3 as well); the rest are FATAL.
constexpr u16 kImageIdBase = 0xFF00;
constexpr char kFlashAtracImage[] = "/dev_flash/sys/external/flashATRAC.pic";
constexpr u32 kFsStatSize = 0x34, kFsStatSizeOffset = 36; // CellFsStat, its be u64 size

// Opens the image of `id` and returns its descriptor and size, or nothing (the file is missing or can't be read).
bool open_image(PPUContext& ctx, u16 id, u32& fd, u64& size)
{
    if (id < kImageIdBase || (id & 0xFF) >= 2) return false;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = guest_base(ctx);
    const std::string_view path = (id & 0xFF) == 0 ? std::string_view(kFlashAtracImage) : std::string_view();
    StackVar vars(ctx, base, 0x80 + kFsStatSize);
    const u32 path_addr = vars.addr(), fd_addr = vars.addr() + 0x40, sb = vars.addr() + 0x80;
    for (usize i = 0; i < path.size(); i++) ps_write8(base, path_addr + static_cast<u32>(i), static_cast<u8>(path[i]));
    if (lv2::sys_fs_open(t, path_addr, 0, fd_addr, 0, 0, 0) != lv2::CELL_OK) return false;
    fd = ps_read32(base, fd_addr);
    if (lv2::sys_fs_fstat(t, fd, sb) != lv2::CELL_OK) {
        lv2::sys_fs_close(t, fd);
        return false;
    }
    size = ps_read64(base, sb + kFsStatSizeOffset);
    return true;
}

s32 cellSysmoduleGetImagesize(PPUContext& ctx, u16 id, u32 image_size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (!image_size || !guest_readable(t, image_size, 4)) {
        log_call_site(ctx, guest_base(ctx), "cellSysmoduleGetImagesize: the size pointer is not guest memory");
        return error(CELL_SYSMODULE_ERROR_FATAL);
    }
    ps_write32(guest_base(ctx), image_size, 0);
    u32 fd = 0;
    u64 size = 0;
    if (!open_image(ctx, id, fd, size)) {
        PS_LOG_WARN(kChan, "cellSysmoduleGetImagesize({:#06x}): no such image", id);
        return error(CELL_SYSMODULE_ERROR_FATAL);
    }
    lv2::sys_fs_close(t, fd);
    ps_write32(guest_base(ctx), image_size, static_cast<u32>(size));
    return lv2::CELL_OK;
}

// cellSysmoduleFetchImage(id, void* buf, u32* size): at most *size bytes of the image; *size becomes what was read.
s32 cellSysmoduleFetchImage(PPUContext& ctx, u16 id, u32 buf, u32 buf_size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (id < kImageIdBase) return error(CELL_SYSMODULE_ERROR_FATAL);
    if (!buf_size || !guest_readable(t, buf_size, 4)) {
        log_call_site(ctx, guest_base(ctx), "cellSysmoduleFetchImage: the size pointer is not guest memory");
        return error(CELL_SYSMODULE_ERROR_FATAL);
    }
    u8* base = guest_base(ctx);
    u32 fd = 0;
    u64 size = 0;
    if (!open_image(ctx, id, fd, size)) {
        ps_write32(base, buf_size, 0);
        PS_LOG_WARN(kChan, "cellSysmoduleFetchImage({:#06x}): no such image", id);
        return error(CELL_SYSMODULE_ERROR_FATAL);
    }
    StackVar nread(ctx, base, 8);
    const u64 want = std::min<u64>(ps_read32(base, buf_size), size);
    if (lv2::sys_fs_read(t, fd, buf, want, nread.addr()) != lv2::CELL_OK) {
        ps_write32(base, buf_size, 0);
    } else {
        ps_write32(base, buf_size, static_cast<u32>(ps_read64(base, nread.addr())));
    }
    lv2::sys_fs_close(t, fd);
    return lv2::CELL_OK;
}

// ---- the export 0x59521326 (RPCS3 cellSysmodule_0x59521326; its name is not known)

constexpr s32 kOpenPsidError = -22; // RPCS3's answer
constexpr u32 kSysSsGetOpenPsid = 872;

// cellSysmodule_0x59521326(u8 output[16]), as RPCS3 makes it: the console's OpenPSID (lv2's sys_ss_get_open_psid),
// its two 64-bit halves swapped, decrypted with AES-128-CBC under libsysmodule's key (scripts/self-keys.py takes it
// from RPCS3's source into the build), the swapped OpenPSID being the IV too, into the caller's 16 bytes. -22 for no
// output or when sys_ss_get_open_psid fails, and here also for an output that is not guest memory (RPCS3 ensure()s).
// Unverified (D-594): not compared with libsysmodule; registered only in a build that has the key (D-639).
s32 cellSysmodule_0x59521326(PPUContext& ctx, u32 output)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = guest_base(ctx);
    if (!output) return kOpenPsidError;
    StackVar psid(ctx, base, 16); // CellSsOpenPSID: be u64 high, low
    if (lv2_syscall(ctx, kSysSsGetOpenPsid, {psid.addr()}) != 0) return kOpenPsidError;
    if (!guest_readable(t, output, 16)) {
        log_call_site(ctx, base, std::format("cellSysmodule_0x59521326: the output {:#x} is not guest memory", output));
        return kOpenPsidError;
    }
    std::array<u8, 16> swapped{}, out{};
    std::memcpy(swapped.data(), base + psid.addr() + 8, 8);
    std::memcpy(swapped.data() + 8, base + psid.addr(), 8);
    const recomp::SysmoduleKey& key = recomp::builtin_sysmodule_key();
    if (!recomp::crypto::aes_cbc_decrypt(key.key, swapped.data(), swapped.data(), swapped.size(), out.data())) {
        PS_LOG_ERROR(kChan, "cellSysmodule_0x59521326: AES-128-CBC failed");
        return kOpenPsidError;
    }
    std::memcpy(base + output, out.data(), out.size());
    return lv2::CELL_OK;
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
        PS_HLE_FUNC(0xC93200DE, cellSysmoduleLoadModuleFile),
        PS_HLE_FUNC(0x205FE2A0, cellSysmoduleUnloadModuleFile),
        PS_HLE_FUNC(0x03D90241, cellSysmoduleSetDebugmode),
        PS_HLE_FUNC(0x1EF115EF, cellSysmoduleGetImagesize),
        PS_HLE_FUNC(0x3C92BE09, cellSysmoduleFetchImage),
        PS_HLE_FUNC(0x002CD0BF, cellSysmoduleSetInternalmode),
    });
    // Without the key (a build without RPCS3's source) the export stays unimplemented: a call logs and fails (D-639).
    if (recomp::builtin_sysmodule_key().present)
        r.add("cellSysmodule", {PS_HLE_UNVERIFIED(0x59521326, cellSysmodule_0x59521326)});
    // RPCS3's variable (hidden there: no game or module imports it), laid out when something links it (D-639).
    r.add_variables("cellSysmodule",
        {PS_HLE_VAR(kContextNid, s_sysmodule_context, kContextSize, kContextAlign, &init_sysmodule_context)});
}

} // namespace pspocket::hle
