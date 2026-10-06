// sceNpTrophy: trophies. Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNpTrophy.cpp/.h (sce_np_trophy_manager,
// trophy_context_t, trophy_handle_t) and the ID manager they use (rpcs3/Emu/IdManager.h/.cpp); the files are read and
// written by trophy_files.cpp (RPCS3 Loader/TRP.cpp, TROPUSR.cpp).
//
// A context names the game's trophy set: its communication ID ("NPWR01234", number 0) gives the set's directory,
// "NPWR01234_00", under the game's TROPDIR and the user's trophy directory /dev_hdd0/home/00000001/trophy (RPCS3's
// default user, as cellSysutil and cellSaveData use). Creating one checks the communication signature's header and
// opens no file. Registering it (sceNpTrophyRegisterContext) reads the trophy pack TROPDIR/<set>/TROPHY.TRP through
// lv2's file system (the disc image or a host directory), installs it into the user's directory as RPCS3 lays it out
// and loads or creates TROPUSR.DAT there; the game and trophy details, the progress, the unlock flags, the icons and
// unlocking work on that installed set:
//   <set>/TROPCONF.SFM   the configuration in the system language: TROP_01.SFM (English, cellSysutil's language), or
//                        else TROP.SFM, or else the pack's TROPCONF.SFM; the other TROP*.SFM are left out
//   <set>/*.PNG ...      every other file of the pack (ICON0.PNG, TROP000.PNG, ...), as the pack names them
//   <set>/TROPUSR.DAT    the user's progress: one entry per trophy with its grade and platinum link, and its state
//                        and unlock time (an RTC tick); kept across installations
// RPCS3 creates no other file (no TROPTRNS.DAT, no TROPSYS.DAT), and neither does this.
//
// Registering calls the game's status callback once directly, with the set's state (installed or not); then, as
// RPCS3 does in place of the system software, it queues the SETUP, PROGRESS (one per trophy), FINALIZE and COMPLETE
// steps for the game's cellSysutilCheckCallback one at a time, and waits: at least 2 s over all steps, each step for
// at most 50 ms more if no check has taken it yet. The calling thread stays in sceNpTrophyRegisterContext meanwhile, as
// in RPCS3.
//
// Contexts and handles get IDs as RPCS3's idm hands them out for id_base 1, id_step 1, id_count 4 (IdTable below).
// sceNpTrophyDestroyHandle and sceNpTrophyAbortHandle don't need the library initialized (RPCS3: unknown whether a
// PS3 checks).
//
// Differences from RPCS3:
// - Pointers are checked before use (D-042): one that is not guest memory is logged with the call site and refused
//   with SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT before anything changes (RPCS3 faults, in CreateContext and CreateHandle
//   after making the object, in UnlockTrophy after unlocking).
// - A TROPUSR.DAT that can't be created (RPCS3 aborts on its `ensure`) fails the registration with
//   SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE, like the other installation failures.
// - The error dialog of SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT can't be shown (no UI yet): the game is
//   sent CELL_SYSUTIL_REQUEST_EXITGAME at once, which RPCS3 does when its dialog can't open, and the error is logged.
// - The user's trophy directory is created when it is missing (RPCS3 creates the home directories at startup).
// - No trophy notification, trophy list overlay or RPCN synchronization.

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "psf.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "sysutil.hpp"
#include "trophy_files.hpp"

namespace pspocket::hle {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kChan = "sceNpTrophy";

enum : u32 {
    SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED = 0x80022901,
    SCE_NP_TROPHY_ERROR_NOT_INITIALIZED = 0x80022902,
    SCE_NP_TROPHY_ERROR_NOT_SUPPORTED = 0x80022903,
    SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED = 0x80022904,
    SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT = 0x80022906,
    SCE_NP_TROPHY_ERROR_EXCEEDS_MAX = 0x80022907,
    SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT = 0x8002290a,
    SCE_NP_TROPHY_ERROR_INVALID_CONTEXT = 0x8002290e,
    SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED = 0x8002290f,
    SCE_NP_TROPHY_ERROR_ABORT = 0x80022910,
    SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE = 0x80022911,
    SCE_NP_TROPHY_ERROR_LOCKED = 0x80022912,
    SCE_NP_TROPHY_ERROR_HIDDEN = 0x80022913,
    SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM = 0x80022914,
    SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED = 0x80022915,
    SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID = 0x80022918,
    SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST = 0x8002291b,
    SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID = 0x80022920,
    SCE_NP_TROPHY_ERROR_UNKNOWN_FILE = 0x80022923,
    SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE = 0x8002292a,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED: return "SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED";
    case SCE_NP_TROPHY_ERROR_NOT_INITIALIZED: return "SCE_NP_TROPHY_ERROR_NOT_INITIALIZED";
    case SCE_NP_TROPHY_ERROR_NOT_SUPPORTED: return "SCE_NP_TROPHY_ERROR_NOT_SUPPORTED";
    case SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED: return "SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED";
    case SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT: return "SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT";
    case SCE_NP_TROPHY_ERROR_EXCEEDS_MAX: return "SCE_NP_TROPHY_ERROR_EXCEEDS_MAX";
    case SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT: return "SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT";
    case SCE_NP_TROPHY_ERROR_INVALID_CONTEXT: return "SCE_NP_TROPHY_ERROR_INVALID_CONTEXT";
    case SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED: return "SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED";
    case SCE_NP_TROPHY_ERROR_ABORT: return "SCE_NP_TROPHY_ERROR_ABORT";
    case SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE: return "SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE";
    case SCE_NP_TROPHY_ERROR_LOCKED: return "SCE_NP_TROPHY_ERROR_LOCKED";
    case SCE_NP_TROPHY_ERROR_HIDDEN: return "SCE_NP_TROPHY_ERROR_HIDDEN";
    case SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM: return "SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM";
    case SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED: return "SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED";
    case SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID: return "SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID";
    case SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST: return "SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST";
    case SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID: return "SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID";
    case SCE_NP_TROPHY_ERROR_UNKNOWN_FILE: return "SCE_NP_TROPHY_ERROR_UNKNOWN_FILE";
    case SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE: return "SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

// "name(arguments): result". Games make these calls a few times at startup and when a trophy is earned, so every one
// is logged at info.
template <class... A>
s32 logged(s32 result, std::format_string<A...> fmt, A&&... args)
{
    PS_LOG_INFO(kChan, "{}: {}", std::format(fmt, std::forward<A>(args)...), result_name(result));
    return result;
}

constexpr u64 SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY = 1;
constexpr u64 SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT = 1;
constexpr u32 NP_TROPHY_COMM_SIGN_MAGIC = 0xB9DDE13B;
constexpr u16 kCommSignVersion = 0x100; // 1.00, the only version accepted
// SceNpCommunicationId (12 bytes): char data[9], term; u8 num; char dummy.
constexpr u32 kCommIdSize = 12, kCommIdNum = 10;
// SceNpCommunicationSignature: u8 data[160]. Its header: be32 magic, be16 version, six zero bytes.
constexpr u32 kCommSignSize = 160;

// SceNpTrophyStatus values the status callback gets.
enum : u32 {
    SCE_NP_TROPHY_STATUS_NOT_INSTALLED = 1,
    SCE_NP_TROPHY_STATUS_INSTALLED = 3,
    SCE_NP_TROPHY_STATUS_PROCESSING_SETUP = 5,
    SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS = 6,
    SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE = 7,
    SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE = 8,
};
constexpr u32 SCE_NP_TROPHY_GRADE_PLATINUM = 1;
constexpr s32 kMaxTrophyId = 127; // RPCS3: at most 128 trophies

// The system language, as cellSysutil reports it (CELL_SYSUTIL_LANG_ENGLISH_US, RPCS3's default): it picks the
// configuration file, TROP_01.SFM.
constexpr s32 kSystemLanguage = 1;
// The user's trophy directory (RPCS3: /dev_hdd0/home/<Emu.GetUsr()>/trophy/).
constexpr std::string_view kTrophyHome = "/dev_hdd0/home/00000001/trophy/";

// The status steps after the direct call (RPCS3): at least 2 s over all of them, then 50 ms more for each step no
// check has taken.
constexpr auto kRegisterMinimum = std::chrono::microseconds(2'000'000);
constexpr auto kRegisterExtra = std::chrono::microseconds(50'000);

// Guest structures (RPCS3 sceNpTrophy.h).
// SceNpTrophyGameDetails (1176 bytes): be32 numTrophies, numPlatinum, numGold, numSilver, numBronze; char title[128]
// at 20, description[1024] at 148; reserved[4].
constexpr u32 kGameDetailsSize = 1176, kGameDetailsTitle = 20, kGameDetailsDescription = 148;
constexpr u32 kTitleSize = 128, kDescriptionSize = 1024;
// SceNpTrophyGameData (20 bytes): be32 unlockedTrophies, unlockedPlatinum, unlockedGold, unlockedSilver,
// unlockedBronze.
constexpr u32 kGameDataSize = 20;
// SceNpTrophyDetails (1164 bytes): be s32 trophyId; be32 trophyGrade; char name[128] at 8, description[1024] at 136;
// b8 hidden at 1160; reserved[3].
constexpr u32 kDetailsSize = 1164, kDetailsName = 8, kDetailsDescription = 136, kDetailsHidden = 1160;
constexpr u32 kNameSize = 128;
// SceNpTrophyData (16 bytes): be64 timestamp (an RTC tick); be s32 trophyId at 8; b8 unlocked at 12; reserved[3].
constexpr u32 kDataSize = 16;
// SceNpTrophyFlagArray (16 bytes): be32 flag_bits[4], trophy n at bit n % 32 of word n / 32 (SCE_NP_TROPHY_FLAG_SETSIZE
// 128).
constexpr u32 kFlagArraySize = 16, kFlagSetSize = 128;

// RPCS3 cellRtcGetCurrentTick, for unlock times: microseconds since 0001-01-01 (RTC_MAGIC_OFFSET is 1970-01-01).
u64 current_rtc_tick()
{
    s64 sec = 0, nsec = 0;
    lv2::host_current_time(sec, nsec);
    return static_cast<u64>(nsec / 1000) + static_cast<u64>(sec) * 1'000'000ull + 62135596800000000ull;
}

// The IDs of one kind of object, as RPCS3's idm hands them out (IdManager.cpp idm::allocate_id): Base, Base + 1, ...
// in order until each has been used once, then the lowest free one; 0, idm's invalid ID for a base of 1, when all are
// in use. Removing an ID or clearing the table never rewinds the order (idm keeps its highest index). The same table
// as sceNp.cpp's.
template <class T, u32 Base, u32 Count>
class IdTable {
public:
    u32 make(T value)
    {
        u32 i = next_;
        if (next_ < Count) next_++;
        else i = static_cast<u32>(std::find(slots_.begin(), slots_.end(), std::nullopt) - slots_.begin());
        if (i == Count) return 0;
        slots_[i] = std::move(value);
        return Base + i;
    }

    T* find(u32 id)
    {
        const u32 i = id - Base; // an ID below Base wraps past Count, as in idm's get_index
        return i < Count && slots_[i] ? &*slots_[i] : nullptr;
    }

    bool remove(u32 id)
    {
        if (!find(id)) return false;
        slots_[id - Base].reset();
        return true;
    }

    void clear()
    {
        for (std::optional<T>& s : slots_) s.reset();
    }

private:
    std::array<std::optional<T>, Count> slots_{};
    u32 next_ = 0; // idm's highest_index
};

constexpr u32 kIdBase = 1, kIdCount = 4; // trophy_context_t and trophy_handle_t: id_base 1, id_step 1, id_count 4

// An ID outside 1..4 is an invalid argument, whether or not it was handed out.
bool in_range(u32 id) { return id - kIdBase < kIdCount; }

// RPCS3 trophy_context_t.
struct TrophyContext {
    std::string trp_name;                  // "NPWR01234_00": the trophy set's directory
    std::array<u8, kCommIdSize> comm_id{}; // RPCS3 keeps it for its RPCN trophy sync
    bool read_only = false;                // SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY
    u64 serial = 0;                        // tells this context from a later one with the same ID
    std::shared_ptr<trophy::TropUsr> tropusr; // set by sceNpTrophyRegisterContext (RPCS3 tropusr)
};

// RPCS3 trophy_handle_t: once aborted, a handle refuses the calls that take it (SCE_NP_TROPHY_ERROR_ABORT).
struct TrophyHandle {
    bool aborted = false;
    u64 serial = 0;
};

// RPCS3 sce_np_trophy_manager and the two idm tables (per process, as g_fxo and idm are per emulation).
struct TrophyState {
    std::mutex mutex;
    bool initialized = false;
    u64 next_serial = 1;
    IdTable<TrophyContext, kIdBase, kIdCount> contexts;
    IdTable<TrophyHandle, kIdBase, kIdCount> handles;
};

TrophyState& trophy()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNpTrophy called without an HLE process");
    return p->state<TrophyState>();
}

// RPCS3 sce_np_trophy_manager::get_context_ex, with TrophyState::mutex held: the context (1..4, known, and writable
// when asked), then the handle (1..4, known, not aborted). The context is set whenever it was found.
struct Lookup {
    TrophyContext* context = nullptr;
    TrophyHandle* handle = nullptr;
    s32 error = 0;
};

Lookup get_context(TrophyState& s, u32 context, u32 handle, bool writable = false)
{
    Lookup l;
    if (!in_range(context)) return {nullptr, nullptr, error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT)};
    l.context = s.contexts.find(context);
    if (!l.context) return {nullptr, nullptr, error(SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT)};
    if (writable && l.context->read_only) l.error = error(SCE_NP_TROPHY_ERROR_INVALID_CONTEXT);
    else if (!in_range(handle)) l.error = error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    else if (!(l.handle = s.handles.find(handle))) l.error = error(SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE);
    else if (l.handle->aborted) l.error = error(SCE_NP_TROPHY_ERROR_ABORT);
    return l;
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// RPCS3 strcpy_trunc into a guest char array: at most capacity - 1 bytes of `s`, the rest of the array zeroed.
void put_string(u8* base, u32 addr, u32 capacity, std::string_view s)
{
    const usize n = std::min<usize>(s.size(), capacity - 1);
    std::memcpy(base + addr, s.data(), n);
    std::memset(base + addr + n, 0, capacity - n);
}

// --------------------------------------------------------------------------------------------- files of the set

// The whole file at the guest path `vpath`, read through lv2's file system (a host directory or the disc image, as
// mounted), with the syscall arguments on the caller's guest stack. Nullopt when it can't be opened or read.
std::optional<std::vector<u8>> read_vfs_file(PPUContext& ctx, const std::string& vpath)
{
    constexpr u32 kPathMax = 1024, kChunk = 0x1000; // CELL_FS_MAX_FS_PATH_LENGTH
    if (vpath.size() >= kPathMax) return std::nullopt;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const StackVar var(ctx, base, kPathMax + 16 + kChunk); // path, fd, nread, buffer (zeroed: the path ends there)
    const u32 path = var.addr(), fd = path + kPathMax, nread = fd + 8, buf = nread + 8;
    std::memcpy(base + path, vpath.data(), vpath.size());
    if (lv2::sys_fs_open(t, path, 0 /* CELL_FS_O_RDONLY */, fd, 0, 0, 0) != lv2::CELL_OK) return std::nullopt;
    const u32 file = ps_read32(base, fd);
    std::vector<u8> data;
    for (;;) {
        if (lv2::sys_fs_read(t, file, buf, kChunk, nread) != lv2::CELL_OK) {
            lv2::sys_fs_close(t, file);
            return std::nullopt;
        }
        const u64 n = ps_read64(base, nread);
        if (!n) break;
        data.insert(data.end(), base + buf, base + buf + n);
    }
    lv2::sys_fs_close(t, file);
    return data;
}

// The booted game's PARAM.SFO category ("DG", "HG", "GD").
std::string game_category()
{
    const GameInfo& g = HleProcess::current()->config().game;
    const std::optional<psf::Registry> sfo = psf::parse(g.param_sfo);
    return sfo ? psf::get_string(*sfo, "CATEGORY") : std::string();
}

// The trophy pack of `name`: TROPDIR/<name>/TROPHY.TRP in the booted game's directory (RPCS3 Emu.GetDir():
// /dev_bdvd/PS3_GAME/ for a disc game, /dev_hdd0/game/<title ID>/ for an HDD game), or, for a game booted from game
// data (category GD: a patch), on the disc. `path` is set to the last path tried.
std::optional<std::vector<u8>> read_trophy_pack(PPUContext& ctx, const std::string& name, std::string& path)
{
    const GameInfo& g = HleProcess::current()->config().game;
    const std::string dir =
        g.boot == GameInfo::Boot::Disc ? std::string("/dev_bdvd/PS3_GAME/") : "/dev_hdd0/game/" + g.title_id + "/";
    path = dir + "TROPDIR/" + name + "/TROPHY.TRP";
    std::optional<std::vector<u8>> pack = read_vfs_file(ctx, path);
    if (!pack && game_category() == "GD") {
        PS_LOG_WARN(kChan, "no trophy pack at {}; the game was booted from game data, so the disc's is used", path);
        path = "/dev_bdvd/PS3_GAME/TROPDIR/" + name + "/TROPHY.TRP";
        pack = read_vfs_file(ctx, path);
    }
    return pack;
}

// The host directory of the user's copy of the set (/dev_hdd0/home/00000001/trophy/<name>), or empty when /dev_hdd0
// isn't a host directory.
std::string installed_dir(PPUContext& ctx, const std::string& name)
{
    return thread_of(ctx).kernel.vfs_host_path(std::string(kTrophyHome) + name);
}

bool is_installed(const std::string& dir)
{
    std::error_code ec;
    return !dir.empty() && fs::is_directory(dir, ec);
}

// The installed configuration, parsed; nullopt when it can't be read (`missing` set) or isn't well-formed XML.
std::optional<trophy::XmlNode> read_installed_config(const std::string& dir, bool& missing)
{
    const fs::path conf = fs::path(dir) / "TROPCONF.SFM";
    missing = false;
    std::error_code ec;
    if (dir.empty() || !fs::is_regular_file(conf, ec)) {
        missing = true;
        return std::nullopt;
    }
    std::ifstream in(conf, std::ios::binary);
    if (!in) {
        missing = true;
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::optional<trophy::XmlNode> doc = trophy::parse_xml(text);
    if (!doc) PS_LOG_ERROR(kChan, "{} is not a well-formed XML document", conf.string());
    return doc;
}

u32 grade_of(std::string_view ttype)
{
    switch (ttype.empty() ? '\0' : ttype[0]) {
    case 'B': return trophy::TropUsr::Bronze;
    case 'S': return trophy::TropUsr::Silver;
    case 'G': return trophy::TropUsr::Gold;
    case 'P': return trophy::TropUsr::Platinum;
    }
    return trophy::TropUsr::Unknown;
}

// ------------------------------------------------------------------------------------------------- init and term

// sceNpTrophyInit(pool, poolSize, containerId, u64 options): the pool and container are not used (RPCS3); no option
// exists.
s32 sceNpTrophyInit(PPUContext&, u32 pool, u32 pool_size, u32 container_id, u64 options)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) {
        return logged(r, "sceNpTrophyInit(pool {:#x}, poolSize {:#x}, containerId {:#x}, options {:#x})", pool,
            pool_size, container_id, options);
    };
    if (s.initialized) return done(error(SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED));
    if (options > 0) return done(error(SCE_NP_TROPHY_ERROR_NOT_SUPPORTED));
    s.initialized = true;
    return done(lv2::CELL_OK);
}

// Every context and handle goes (RPCS3 idm::clear); their IDs go on from where they were.
s32 sceNpTrophyTerm(PPUContext&)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return logged(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED), "sceNpTrophyTerm()");
    s.contexts.clear();
    s.handles.clear();
    s.initialized = false;
    return logged(lv2::CELL_OK, "sceNpTrophyTerm()");
}

// ----------------------------------------------------------------------------------------------------- contexts

// sceNpTrophyCreateContext(u32* context, const SceNpCommunicationId* commId, const SceNpCommunicationSignature*
// commSign, u64 options), with RPCS3's order of checks: the signature pointer before the library's state, the ID's
// number (at most 99), then the signature's header (magic, six zero bytes, version 1.00).
s32 sceNpTrophyCreateContext(PPUContext& ctx, u32 context, u32 comm_id, u32 comm_sign, u64 options)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyCreateContext(context {:#x}, commId {:#x}, commSign {:#x}, options {:#x}){}",
            context, comm_id, comm_sign, options, what);
    };
    if (!comm_sign) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    if (!context || !comm_id) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (options > SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY) return done(error(SCE_NP_TROPHY_ERROR_NOT_SUPPORTED));
    if (!usable(ctx, comm_id, kCommIdSize, "sceNpTrophyCreateContext: commId") ||
        !usable(ctx, comm_sign, kCommSignSize, "sceNpTrophyCreateContext: commSign") ||
        !usable(ctx, context, 4, "sceNpTrophyCreateContext: context")) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    TrophyContext c;
    std::memcpy(c.comm_id.data(), base + comm_id, kCommIdSize);
    const u32 num = c.comm_id[kCommIdNum];
    if (num > 99) return done(error(SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID), std::format(": number {}", num));
    // The name: the ID's nine characters, or those before a NUL (the terminator byte isn't checked).
    std::string name(reinterpret_cast<const char*>(c.comm_id.data()), 9);
    if (const usize nul = name.find('\0'); nul != std::string::npos) name.resize(nul);
    std::array<u8, kCommSignSize> sign{};
    std::memcpy(sign.data(), base + comm_sign, kCommSignSize);
    if (ps_ld32(sign.data()) != NP_TROPHY_COMM_SIGN_MAGIC ||
        std::any_of(sign.begin() + 6, sign.begin() + 12, [](u8 b) { return b != 0; }) ||
        ps_ld16(sign.data() + 4) != kCommSignVersion) {
        const std::string header = std::format("{:08x} {:04x}", ps_ld32(sign.data()), ps_ld16(sign.data() + 4));
        return done(error(SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID), std::format(": \"{}\", signature {}", name, header));
    }
    c.trp_name = std::format("{}_{:02}", name, num);
    c.read_only = (options & SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY) != 0;
    c.serial = s.next_serial++;
    const std::string trp_name = c.trp_name;
    const bool read_only = c.read_only;
    const u32 id = s.contexts.make(std::move(c));
    if (!id) {
        return done(error(SCE_NP_TROPHY_ERROR_EXCEEDS_MAX), std::format(": \"{}\", {} contexts", trp_name, kIdCount));
    }
    ps_write32(base, context, id);
    return done(lv2::CELL_OK, std::format(": context {} for trophy set \"{}\"{}", id, trp_name,
        read_only ? " (read only)" : ""));
}

s32 sceNpTrophyDestroyContext(PPUContext&, u32 context)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, "sceNpTrophyDestroyContext({})", context); };
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    if (!in_range(context)) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (!s.contexts.remove(context)) return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT));
    return done(lv2::CELL_OK);
}

// ------------------------------------------------------------------------------------------------------ handles

// sceNpTrophyCreateHandle(u32* handle).
s32 sceNpTrophyCreateHandle(PPUContext& ctx, u32 handle)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyCreateHandle({:#x}){}", handle, what);
    };
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    if (!handle) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (!usable(ctx, handle, 4, "sceNpTrophyCreateHandle: handle")) return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    const u32 id = s.handles.make({false, s.next_serial});
    if (!id) return done(error(SCE_NP_TROPHY_ERROR_EXCEEDS_MAX), std::format(": {} handles", kIdCount));
    s.next_serial++;
    ps_write32(guest_base(ctx), handle, id);
    return done(lv2::CELL_OK, std::format(": handle {}", id));
}

s32 sceNpTrophyDestroyHandle(PPUContext&, u32 handle)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, "sceNpTrophyDestroyHandle({})", handle); };
    if (!in_range(handle)) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (!s.handles.remove(handle)) return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE));
    return done(lv2::CELL_OK);
}

// Marks the handle aborted (RPCS3, a TODO: nothing that runs with it is interrupted); it stays until destroyed.
s32 sceNpTrophyAbortHandle(PPUContext&, u32 handle)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, "sceNpTrophyAbortHandle({})", handle); };
    if (!in_range(handle)) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    TrophyHandle* h = s.handles.find(handle);
    if (!h) return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE));
    h->aborted = true;
    return done(lv2::CELL_OK);
}

// ------------------------------------------------------------------------------------------------- registration

// sceNpTrophyGetRequiredDiskSpace(context, handle, u64* reqspace, u64 options): what installing the set takes (the
// pack's files), or 0 when it is installed already. Unlike most functions, the pointer and options come first.
s32 sceNpTrophyGetRequiredDiskSpace(PPUContext& ctx, u32 context, u32 handle, u32 reqspace, u64 options)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyGetRequiredDiskSpace(context {}, handle {}, reqspace {:#x}, options {:#x}){}",
            context, handle, reqspace, options, what);
    };
    if (!reqspace) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (options > 0) return done(error(SCE_NP_TROPHY_ERROR_NOT_SUPPORTED));
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup l = get_context(s, context, handle);
    if (l.error) return done(l.error);
    u64 space = 0;
    const std::string dir = installed_dir(ctx, l.context->trp_name);
    if (!is_installed(dir)) {
        std::string path;
        std::optional<std::vector<u8>> pack = read_trophy_pack(ctx, l.context->trp_name, path);
        if (!pack) return done(error(SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST), std::format(": no file {}", path));
        std::string why;
        if (const std::optional<trophy::TrpArchive> trp = trophy::TrpArchive::parse(std::move(*pack), &why)) {
            space = trp->required_space();
        } else {
            PS_LOG_ERROR(kChan, "sceNpTrophyGetRequiredDiskSpace: {} is not a trophy pack ({}); 0 bytes", path, why);
        }
    }
    if (!usable(ctx, reqspace, 8, "sceNpTrophyGetRequiredDiskSpace: reqspace")) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    ps_write64(guest_base(ctx), reqspace, space);
    return done(lv2::CELL_OK, std::format(": {} bytes{}", space, is_installed(dir) ? " (installed already)" : ""));
}

// What RPCS3 does on an error when the game asked for SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT: an
// error dialog whose OK ends the game. Without a dialog, the game is asked to exit right away (RPCS3, when its dialog
// can't open).
void on_register_error(u64 options)
{
    if (!(options & SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT)) return;
    PS_LOG_ERROR(kChan, "sceNpTrophyRegisterContext: trophy registration failed and the game asked for an error dialog "
                        "that ends it; there is no dialog, so the game is asked to exit");
    sysutil_send_event(CELL_SYSUTIL_REQUEST_EXITGAME, 0);
}

// The status steps still to be taken by a cellSysutilCheckCallback (RPCS3's `queued` counter).
struct PendingSteps {
    std::mutex mutex;
    std::condition_variable cv;
    u32 queued = 0;
};

// RPCS3's stand-in for the system software's registration events: each step (status, completed, total) goes to the
// game's status callback through cellSysutilCheckCallback, one at a time. After queueing a step this waits until
// 2 s / steps have passed if a check took it, or 50 ms longer if none did yet (then the next one is queued anyway).
// False when the process exited meanwhile (RPCS3 stops when the emulation is stopping).
bool send_status_steps(lv2::PpuThread& t, u32 status_cb, u32 context, u32 arg, u32 trophy_count)
{
    using Clock = std::chrono::steady_clock;
    const s32 progress = static_cast<s32>(trophy_count) - 1;
    const std::array<std::pair<u32, s32>, 4> statuses = {{
        {SCE_NP_TROPHY_STATUS_PROCESSING_SETUP, 3},
        {SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS, progress},
        {SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE, std::max<s32>(progress, 9) - 5}, // RPCS3: a bit less than progress
        {SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE, 0},
    }};
    u32 total_steps = 0;
    for (const auto& [status, total] : statuses) total_steps += static_cast<u32>(total + 1);
    const auto pending = std::make_shared<PendingSteps>();
    for (const auto& [status, total] : statuses) {
        for (s32 completed = 0; completed <= total; completed++) {
            {
                std::lock_guard lock(pending->mutex);
                pending->queued++;
            }
            sysutil_queue_call([=, st = status, tot = total](PPUContext& cb_ctx) -> s32 {
                // RPCS3 ignores the callback's result here. (completed and total are s32 arguments: sign-extended.)
                const u64 done_steps = static_cast<u64>(static_cast<s64>(completed));
                const u64 all_steps = static_cast<u64>(static_cast<s64>(tot));
                call_guest(cb_ctx, status_cb, {context, st, done_steps, all_steps, arg});
                std::lock_guard lock(pending->mutex);
                if (pending->queued-- == 1) pending->cv.notify_one();
                return lv2::CELL_OK;
            });
            const auto until_min = Clock::now() + kRegisterMinimum / total_steps;
            const auto until_max = until_min + kRegisterExtra;
            std::unique_lock lock(pending->mutex);
            for (;;) {
                const auto until = pending->queued ? until_max : until_min;
                const auto now = Clock::now();
                if (now >= until) break;
                if (t.kernel.exited()) return false;
                // Waits in slices so an exiting process is noticed.
                const auto wake = std::min(until, now + std::chrono::milliseconds(20));
                if (pending->queued) pending->cv.wait_until(lock, wake);
                else {
                    lock.unlock();
                    std::this_thread::sleep_until(wake);
                    lock.lock();
                }
            }
        }
    }
    return true;
}

// sceNpTrophyRegisterContext(context, handle, SceNpTrophyStatusCallback statusCb, void* arg, u64 options), as RPCS3:
// checks, the pack opened, the direct call (without the library's lock: the callback may call sceNpTrophy), the checks
// again, the pack installed and TROPUSR.DAT loaded, then the status steps.
s32 sceNpTrophyRegisterContext(PPUContext& ctx, u32 context, u32 handle, u32 status_cb, u32 arg, u64 options)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r,
            "sceNpTrophyRegisterContext(context {}, handle {}, statusCb {:#x}, arg {:#x}, options {:#x}){}", context,
            handle, status_cb, arg, options, what);
    };
    lv2::PpuThread& t = thread_of(ctx);
    TrophyState& s = trophy();
    std::unique_lock lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup first = get_context(s, context, handle, true);
    if (first.error) return done(first.error);
    const u64 context_serial = first.context->serial, handle_serial = first.handle->serial;
    if (!status_cb) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (options > SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT) {
        return done(error(SCE_NP_TROPHY_ERROR_NOT_SUPPORTED));
    }
    const std::string name = first.context->trp_name;
    std::string trp_path;
    std::optional<std::vector<u8>> pack = read_trophy_pack(ctx, name, trp_path);
    if (!pack) return done(error(SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST), std::format(": cannot open {}", trp_path));
    const std::string dir = installed_dir(ctx, name);
    const u32 trp_status = is_installed(dir) ? SCE_NP_TROPHY_STATUS_INSTALLED : SCE_NP_TROPHY_STATUS_NOT_INSTALLED;
    lock.unlock();

    // "Ask permission" to install the set.
    PS_LOG_INFO(kChan, "sceNpTrophyRegisterContext: status callback {:#x}({}, {}, 0, 0, {:#x}) ({})", status_cb,
        context, trp_status, arg, trp_status == SCE_NP_TROPHY_STATUS_INSTALLED ? "installed" : "not installed");
    const s32 answer = static_cast<s32>(call_guest(ctx, status_cb, {context, trp_status, 0, 0, arg}));
    if (answer < 0) {
        on_register_error(options);
        return done(error(SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED), std::format(": the callback answered {}", answer));
    }

    // The callback may have called sceNpTrophy itself: everything is checked again.
    lock.lock();
    if (!s.initialized) {
        on_register_error(options);
        return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    }
    const Lookup again = get_context(s, context, handle);
    if (again.error) return done(again.error);
    if (again.context->serial != context_serial) {
        on_register_error(options);
        return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT), ": a new context has the ID");
    }
    if (again.handle->serial != handle_serial) {
        on_register_error(options);
        return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE), ": a new handle has the ID");
    }
    const auto fail = [&](std::string why) {
        PS_LOG_ERROR(kChan, "sceNpTrophyRegisterContext: {}", why);
        on_register_error(options);
        return done(error(SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE));
    };
    std::string why;
    std::optional<trophy::TrpArchive> trp = trophy::TrpArchive::parse(std::move(*pack), &why);
    if (!trp) return fail(std::format("{} is not a trophy pack: {}", trp_path, why));
    // The configuration in the system language becomes TROPCONF.SFM; the other languages' files are left out.
    const std::string language_conf = std::format("TROP_{:02}.SFM", kSystemLanguage);
    if (trp->contains(language_conf)) {
        trp->remove("TROPCONF.SFM");
        trp->remove("TROP.SFM");
        trp->rename(language_conf, "TROPCONF.SFM");
    } else if (trp->contains("TROP.SFM")) {
        trp->remove("TROPCONF.SFM");
        trp->rename("TROP.SFM", "TROPCONF.SFM");
    } else if (!trp->contains("TROPCONF.SFM")) {
        return fail(std::format("{} has no trophy configuration", trp_path));
    }
    for (s32 i = 0; i <= 18; i++)
        if (i != kSystemLanguage) trp->remove(std::format("TROP_{:02}.SFM", i));
    if (dir.empty()) return fail(std::format("{}{} is not on a host directory", kTrophyHome, name));
    if (!trp->install(dir, &why)) return fail(std::format("cannot install {} into {}: {}", trp_path, dir, why));
    auto tropusr = std::make_shared<trophy::TropUsr>();
    const trophy::TropUsr::LoadResult loaded =
        tropusr->load(fs::path(dir) / "TROPUSR.DAT", fs::path(dir) / "TROPCONF.SFM", &why);
    if (!loaded.success) return fail(std::format("no TROPUSR.DAT for {}: {}", name, why));
    again.context->tropusr = tropusr;
    const u32 count = tropusr->trophy_count();
    PS_LOG_INFO(kChan, "sceNpTrophyRegisterContext: trophy set {} (pack version {}, {} files) installed in {}; "
                       "{} trophies, {} unlocked{}",
        name, trp->version(), trp->entries().size(), dir, count, tropusr->unlocked_count(),
        loaded.discarded_existing ? " (an invalid TROPUSR.DAT was replaced)" : "");
    lock.unlock();

    const auto start = std::chrono::steady_clock::now();
    if (!send_status_steps(t, status_cb, context, arg, count)) {
        return done(lv2::CELL_OK, ": the process exited during the status steps");
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return done(lv2::CELL_OK, std::format(": status steps sent in {} ms", ms.count()));
}

// ------------------------------------------------------------------------------------------------ the set's details

// sceNpTrophyGetGameInfo(context, handle, SceNpTrophyGameDetails* details, SceNpTrophyGameData* data): the set's title
// and description and its trophies by grade, and how many of them are unlocked, from the installed configuration and
// TROPUSR.DAT. What is asked for is cleared first; a configuration that isn't well-formed leaves it cleared (CELL_OK).
s32 sceNpTrophyGetGameInfo(PPUContext& ctx, u32 context, u32 handle, u32 details, u32 data)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyGetGameInfo(context {}, handle {}, details {:#x}, data {:#x}){}", context, handle,
            details, data, what);
    };
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup l = get_context(s, context, handle);
    if (l.error) return done(l.error);
    if (!l.context->tropusr) return done(error(SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED));
    if (!details && !data) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    const std::string dir = installed_dir(ctx, l.context->trp_name);
    bool missing = false;
    const std::optional<trophy::XmlNode> doc = read_installed_config(dir, missing);
    if (missing) return done(error(SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST), std::format(": in {}", dir));
    if ((details && !usable(ctx, details, kGameDetailsSize, "sceNpTrophyGetGameInfo: details")) ||
        (data && !usable(ctx, data, kGameDataSize, "sceNpTrophyGetGameInfo: data"))) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    if (details) std::memset(base + details, 0, kGameDetailsSize);
    if (data) std::memset(base + data, 0, kGameDataSize);
    if (!doc) return done(lv2::CELL_OK, ": the configuration is unreadable; cleared");
    // [0] all, then by grade: platinum, gold, silver, bronze (the structures' order).
    std::array<u32, 5> total{}, unlocked{};
    const auto count = [](std::array<u32, 5>& c, std::string_view ttype) {
        c[0]++;
        switch (ttype.empty() ? '\0' : ttype[0]) {
        case 'P': c[1]++; break;
        case 'G': c[2]++; break;
        case 'S': c[3]++; break;
        case 'B': c[4]++; break;
        }
    };
    for (const trophy::XmlNode& n : trophy::trophy_root(*doc).children) {
        if (n.text_node) continue;
        if (details && n.name == "title-name") {
            put_string(base, details + kGameDetailsTitle, kTitleSize, n.content());
        } else if (details && n.name == "title-detail") {
            put_string(base, details + kGameDetailsDescription, kDescriptionSize, n.content());
        } else if (n.name == "trophy") {
            const std::string ttype = n.attribute("ttype");
            if (details) count(total, ttype);
            if (data && l.context->tropusr->unlocked(static_cast<u32>(trophy::atoi(n.attribute("id"))))) {
                count(unlocked, ttype);
            }
        }
    }
    for (u32 i = 0; i < 5; i++) {
        if (details) ps_write32(base, details + 4 * i, total[i]);
        if (data) ps_write32(base, data + 4 * i, unlocked[i]);
    }
    return done(lv2::CELL_OK, std::format(": {} trophies, {} unlocked", total[0], unlocked[0]));
}

// sceNpTrophyGetTrophyInfo(context, handle, trophyId, SceNpTrophyDetails* details, SceNpTrophyData* data): one
// trophy's grade, name and description, and whether and when it was unlocked. A hidden trophy is refused until it is
// unlocked. The ID is checked first (0..127), before the library's state.
s32 sceNpTrophyGetTrophyInfo(PPUContext& ctx, u32 context, u32 handle, s32 trophy_id, u32 details, u32 data)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyGetTrophyInfo(context {}, handle {}, trophyId {}, details {:#x}, data {:#x}){}",
            context, handle, trophy_id, details, data, what);
    };
    if (trophy_id < 0 || trophy_id > kMaxTrophyId) return done(error(SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID));
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup l = get_context(s, context, handle);
    if (l.error) return done(l.error);
    // RPCS3 NpTrophyGetTrophyInfo.
    if (!details && !data) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (!l.context->tropusr) return done(error(SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED));
    const std::string dir = installed_dir(ctx, l.context->trp_name);
    bool missing = false;
    const std::optional<trophy::XmlNode> doc = read_installed_config(dir, missing);
    if (missing) return done(error(SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST), std::format(": in {}", dir));
    const trophy::XmlNode* found = nullptr;
    if (doc) {
        for (const trophy::XmlNode& n : trophy::trophy_root(*doc).children) {
            if (!n.text_node && n.name == "trophy" && trophy::atoi(n.attribute("id")) == trophy_id) {
                found = &n;
                break;
            }
        }
    }
    if (!found) return done(error(SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID));
    const u32 id = static_cast<u32>(trophy_id);
    const bool hidden = found->attribute("hidden").starts_with('y');
    const bool unlocked = l.context->tropusr->unlocked(id);
    if (hidden && !unlocked) return done(error(SCE_NP_TROPHY_ERROR_HIDDEN));
    if ((details && !usable(ctx, details, kDetailsSize, "sceNpTrophyGetTrophyInfo: details")) ||
        (data && !usable(ctx, data, kDataSize, "sceNpTrophyGetTrophyInfo: data"))) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    std::string trophy_name;
    if (details) {
        std::string description;
        for (const trophy::XmlNode& n : found->children) {
            if (n.text_node) continue;
            if (n.name == "name") trophy_name = n.content();
            else if (n.name == "detail") description = n.content();
        }
        std::memset(base + details, 0, kDetailsSize);
        ps_write32(base, details + 0, id);
        ps_write32(base, details + 4, grade_of(found->attribute("ttype")));
        put_string(base, details + kDetailsName, kNameSize, trophy_name);
        put_string(base, details + kDetailsDescription, kDescriptionSize, description);
        ps_write8(base, details + kDetailsHidden, hidden ? 1 : 0);
    }
    if (data) {
        std::memset(base + data, 0, kDataSize);
        ps_write64(base, data + 0, l.context->tropusr->timestamp(id));
        ps_write32(base, data + 8, id);
        ps_write8(base, data + 12, unlocked ? 1 : 0);
    }
    return done(lv2::CELL_OK, std::format(": \"{}\", {}", trophy_name, unlocked ? "unlocked" : "locked"));
}

// sceNpTrophyUnlockTrophy(context, handle, trophyId, u32* platinumId): unlocks the trophy at the current RTC tick and,
// when that completes the platinum trophy it links to, the platinum one too (its ID in *platinumId, else
// SCE_NP_TROPHY_INVALID_TROPHY_ID); TROPUSR.DAT is saved. A platinum trophy can't be unlocked directly.
s32 sceNpTrophyUnlockTrophy(PPUContext& ctx, u32 context, u32 handle, s32 trophy_id, u32 platinum_id)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyUnlockTrophy(context {}, handle {}, trophyId {}, platinumId {:#x}){}", context,
            handle, trophy_id, platinum_id, what);
    };
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup l = get_context(s, context, handle, true);
    if (l.error) return done(l.error);
    trophy::TropUsr* usr = l.context->tropusr.get();
    if (!usr) return done(error(SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED));
    if (trophy_id < 0 || trophy_id >= static_cast<s32>(usr->trophy_count())) {
        return done(error(SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID));
    }
    const u32 id = static_cast<u32>(trophy_id);
    if (usr->grade(id) == SCE_NP_TROPHY_GRADE_PLATINUM) return done(error(SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM));
    if (usr->unlocked(id)) return done(error(SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED));
    if (platinum_id && !usable(ctx, platinum_id, 4, "sceNpTrophyUnlockTrophy: platinumId")) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    const u64 tick = current_rtc_tick();
    static_cast<void>(usr->unlock(id, tick, tick));
    const std::string dir = installed_dir(ctx, l.context->trp_name);
    const u32 platinum = usr->unlocked_platinum_id(id, fs::path(dir) / "TROPCONF.SFM");
    if (platinum != trophy::TropUsr::kInvalidTrophyId) {
        PS_LOG_INFO(kChan, "sceNpTrophyUnlockTrophy: every trophy of platinum trophy {} is unlocked", platinum);
        static_cast<void>(usr->unlock(platinum, tick, tick));
    }
    if (platinum_id) ps_write32(guest_base(ctx), platinum_id, platinum);
    if (!usr->save(fs::path(dir) / "TROPUSR.DAT")) {
        PS_LOG_ERROR(kChan, "sceNpTrophyUnlockTrophy: cannot save {}/TROPUSR.DAT", dir);
    }
    return done(lv2::CELL_OK, platinum != trophy::TropUsr::kInvalidTrophyId
                                  ? std::format(": unlocked, with platinum trophy {}", platinum)
                                  : std::string(": unlocked"));
}

// ------------------------------------------------------------------------------------- progress, flags and icons

// sceNpTrophyGetGameProgress(context, handle, s32* percentage): the share of the set's trophies that are unlocked, in
// percent, rounded as RPCS3's utils::rounded_div does (to the nearest, a half down: 1 of 8 is 12); 0 for a set without
// trophies.
s32 sceNpTrophyGetGameProgress(PPUContext& ctx, u32 context, u32 handle, u32 percentage)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyGetGameProgress(context {}, handle {}, percentage {:#x}){}", context, handle,
            percentage, what);
    };
    if (!percentage) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup l = get_context(s, context, handle);
    if (l.error) return done(l.error);
    if (!l.context->tropusr) return done(error(SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED));
    if (!usable(ctx, percentage, 4, "sceNpTrophyGetGameProgress: percentage")) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    const u32 unlocked = l.context->tropusr->unlocked_count(), count = l.context->tropusr->trophy_count();
    const u32 hundredfold = unlocked * 100;
    const u32 percent = count ? hundredfold / count + (hundredfold % count > count / 2 ? 1 : 0) : 0;
    if (count == 0 || count > kFlagSetSize) {
        PS_LOG_WARN(kChan, "sceNpTrophyGetGameProgress: {} trophies in the set (RPCS3: may be invalid)", count);
    }
    ps_write32(guest_base(ctx), percentage, percent);
    return done(lv2::CELL_OK, std::format(": {}% ({} of {} trophies)", percent, unlocked, count));
}

// sceNpTrophyGetTrophyUnlockState(context, handle, SceNpTrophyFlagArray* flags, u32* count): which of the first 128
// trophies are unlocked, and how many the set has. A read-only context (which can't be registered) reads the user's
// installed copy of the set, as RPCS3 does: its TROPUSR.DAT, made from the configuration when it is missing; when the
// set isn't installed, no trophies and CELL_OK (RPCS3: "TODO: confirm").
s32 sceNpTrophyGetTrophyUnlockState(PPUContext& ctx, u32 context, u32 handle, u32 flags, u32 count)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyGetTrophyUnlockState(context {}, handle {}, flags {:#x}, count {:#x}){}", context,
            handle, flags, count, what);
    };
    if (!flags || !count) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup l = get_context(s, context, handle);
    if (l.error) return done(l.error);
    trophy::TropUsr installed;
    const trophy::TropUsr* usr = nullptr;
    if (l.context->read_only) {
        const std::string dir = installed_dir(ctx, l.context->trp_name);
        std::string why;
        if (is_installed(dir) &&
            installed.load(fs::path(dir) / "TROPUSR.DAT", fs::path(dir) / "TROPCONF.SFM", &why).success) {
            usr = &installed;
        } else {
            PS_LOG_INFO(kChan, "sceNpTrophyGetTrophyUnlockState: no installed trophy set {}{}", l.context->trp_name,
                why.empty() ? std::string() : std::format(" ({})", why));
        }
    } else if (!(usr = l.context->tropusr.get())) {
        return done(error(SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED));
    }
    if (!usable(ctx, flags, kFlagArraySize, "sceNpTrophyGetTrophyUnlockState: flags") ||
        !usable(ctx, count, 4, "sceNpTrophyGetTrophyUnlockState: count")) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    std::memset(base + flags, 0, kFlagArraySize);
    const u32 trophies = usr ? usr->trophy_count() : 0;
    ps_write32(base, count, trophies);
    if (trophies > kFlagSetSize) {
        PS_LOG_ERROR(kChan, "sceNpTrophyGetTrophyUnlockState: {} trophies; flags for the first {}", trophies,
            kFlagSetSize);
    }
    u32 unlocked = 0;
    for (u32 id = 0; id < std::min(trophies, kFlagSetSize); id++) {
        if (!usr->unlocked(id)) continue;
        const u32 word = flags + 4 * (id / 32);
        ps_write32(base, word, ps_read32(base, word) | 1u << (id % 32));
        unlocked++;
    }
    return done(lv2::CELL_OK, std::format(": {} trophies, {} of them unlocked", trophies, unlocked));
}

// The file `name` of the user's installed copy of a set, or nullopt when there is none.
std::optional<std::vector<u8>> read_installed_file(const std::string& dir, const std::string& name)
{
    std::error_code ec;
    if (dir.empty() || !fs::is_regular_file(fs::path(dir) / name, ec)) return std::nullopt;
    return trophy::read_host_file(fs::path(dir) / name);
}

// RPCS3's answer with an icon: the file into `buffer` when there is one and *size is at least the file's size, then
// the file's size into *size. False when a pointer is not guest memory (logged; nothing written).
bool put_icon(PPUContext& ctx, std::string_view who, const std::vector<u8>& icon, u32 buffer, u32 size)
{
    if (!usable(ctx, size, 4, std::format("{}: size", who))) return false;
    u8* base = guest_base(ctx);
    const u32 icon_size = static_cast<u32>(icon.size());
    if (buffer && icon_size && ps_read32(base, size) >= icon_size) {
        if (!usable(ctx, buffer, icon_size, std::format("{}: buffer", who))) return false;
        std::memcpy(base + buffer, icon.data(), icon_size);
    }
    ps_write32(base, size, icon_size);
    return true;
}

// sceNpTrophyGetGameIcon(context, handle, void* buffer, u32* size): the set's icon from the user's installed copy,
// ICON0_<language>.PNG (the system language's, ICON0_01.PNG) or else ICON0.PNG; see put_icon. The context needn't be
// registered (RPCS3).
s32 sceNpTrophyGetGameIcon(PPUContext& ctx, u32 context, u32 handle, u32 buffer, u32 size)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyGetGameIcon(context {}, handle {}, buffer {:#x}, size {:#x}){}", context, handle,
            buffer, size, what);
    };
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup l = get_context(s, context, handle);
    if (l.error) return done(l.error);
    if (!size) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    const std::string dir = installed_dir(ctx, l.context->trp_name);
    std::string name = std::format("ICON0_{:02}.PNG", kSystemLanguage);
    std::optional<std::vector<u8>> icon = read_installed_file(dir, name);
    if (!icon) icon = read_installed_file(dir, name = "ICON0.PNG");
    if (!icon) return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_FILE), std::format(": no game icon for {}", dir));
    if (!put_icon(ctx, "sceNpTrophyGetGameIcon", *icon, buffer, size)) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    return done(lv2::CELL_OK, std::format(": {}, {} bytes", name, icon->size()));
}

// sceNpTrophyGetTrophyIcon(context, handle, trophyId, void* buffer, u32* size): an unlocked trophy's icon,
// TROP<id>.PNG of the user's installed copy (see put_icon). A locked trophy is refused, a hidden one with
// SCE_NP_TROPHY_ERROR_HIDDEN, the others with SCE_NP_TROPHY_ERROR_LOCKED.
s32 sceNpTrophyGetTrophyIcon(PPUContext& ctx, u32 context, u32 handle, s32 trophy_id, u32 buffer, u32 size)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyGetTrophyIcon(context {}, handle {}, trophyId {}, buffer {:#x}, size {:#x}){}",
            context, handle, trophy_id, buffer, size, what);
    };
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    const Lookup l = get_context(s, context, handle);
    if (l.error) return done(l.error);
    if (!size) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    const trophy::TropUsr* usr = l.context->tropusr.get();
    if (!usr) return done(error(SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED));
    const u32 id = static_cast<u32>(trophy_id); // a negative ID is beyond any set
    if (id >= usr->trophy_count()) return done(error(SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID));
    const std::string dir = installed_dir(ctx, l.context->trp_name);
    if (!usr->unlocked(id)) {
        // Hidden when the configuration says so (RPCS3: an unreadable configuration makes it only locked).
        bool missing = false;
        if (const std::optional<trophy::XmlNode> doc = read_installed_config(dir, missing)) {
            for (const trophy::XmlNode& n : trophy::trophy_root(*doc).children) {
                if (!n.text_node && n.name == "trophy" && trophy::atoi(n.attribute("id")) == trophy_id &&
                    n.attribute("hidden").starts_with('y')) {
                    return done(error(SCE_NP_TROPHY_ERROR_HIDDEN));
                }
            }
        }
        return done(error(SCE_NP_TROPHY_ERROR_LOCKED));
    }
    const std::string name = std::format("TROP{:03}.PNG", trophy_id);
    const std::optional<std::vector<u8>> icon = read_installed_file(dir, name);
    if (!icon) return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_FILE), std::format(": no {} in {}", name, dir));
    if (!put_icon(ctx, "sceNpTrophyGetTrophyIcon", *icon, buffer, size)) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    return done(lv2::CELL_OK, std::format(": {}, {} bytes", name, icon->size()));
}

// sceNpTrophySetSoundLevel(context, handle, level, u64 options): the volume of the unlock notification's sound, 20 to
// 100. There is no notification (nor in RPCS3), so only the arguments are checked.
s32 sceNpTrophySetSoundLevel(PPUContext&, u32 context, u32 handle, u32 level, u64 options)
{
    const auto done = [&](s32 r) {
        return logged(r, "sceNpTrophySetSoundLevel(context {}, handle {}, level {}, options {:#x})", context, handle,
            level, options);
    };
    if (level > 100 || level < 20) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (options > 0) return done(error(SCE_NP_TROPHY_ERROR_NOT_SUPPORTED));
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    return done(get_context(s, context, handle).error);
}

} // namespace

void register_sceNpTrophy(Registry& r)
{
    r.add("sceNpTrophy", {
        PS_HLE_FUNC(0x39567781, sceNpTrophyInit),
        PS_HLE_FUNC(0xA7FABF4D, sceNpTrophyTerm),
        PS_HLE_FUNC(0xE3BF9A28, sceNpTrophyCreateContext),
        PS_HLE_FUNC(0x3741ECC7, sceNpTrophyDestroyContext),
        PS_HLE_FUNC(0x1C25470D, sceNpTrophyCreateHandle),
        PS_HLE_FUNC(0x623CD2DC, sceNpTrophyDestroyHandle),
        PS_HLE_FUNC(0x48BD97C7, sceNpTrophyAbortHandle),
        PS_HLE_FUNC(0x370136FE, sceNpTrophyGetRequiredDiskSpace),
        PS_HLE_FUNC(0x1197B52C, sceNpTrophyRegisterContext),
        PS_HLE_FUNC(0x49D18217, sceNpTrophyGetGameInfo),
        PS_HLE_FUNC(0xFCE6D30A, sceNpTrophyGetTrophyInfo),
        PS_HLE_FUNC(0x8CEEDD21, sceNpTrophyUnlockTrophy),
        PS_HLE_FUNC(0x079F0E87, sceNpTrophyGetGameProgress),
        PS_HLE_FUNC(0xB3AC3478, sceNpTrophyGetTrophyUnlockState),
        PS_HLE_FUNC(0xFF299E03, sceNpTrophyGetGameIcon),
        PS_HLE_FUNC(0xBAEDF689, sceNpTrophyGetTrophyIcon),
        PS_HLE_FUNC(0x27DEDA93, sceNpTrophySetSoundLevel),
    });
}

} // namespace pspocket::hle
