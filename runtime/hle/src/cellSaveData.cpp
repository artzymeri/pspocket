// cellSaveData: the PS3's save data, on host directories under /dev_hdd0. Reference: RPCS3
// rpcs3/Emu/Cell/Modules/cellSaveData.cpp/.h (savedata_check_args, savedata_op, savedata_get_list_item,
// select_and_delete), cellSysutil.cpp (sysutil_check_name_string), cellMsgDialog.cpp (open_msg_dialog),
// cellUserInfo.h, Loader/PSF.cpp (psf::load's category check), Utilities/File.cpp (fs::get_stat's times).
//
// Implemented: what GTA V imports. From cellSysutil: cellSaveDataAutoLoad2, cellSaveDataAutoSave2,
// cellSaveDataListAutoLoad, cellSaveDataDelete2, cellSaveDataFixedDelete; from cellSaveData: cellSaveDataGetListItem.
//
// Layout, as RPCS3's: the current user's saves are the directories /dev_hdd0/home/00000001/savedata/<dir name>/. Each
// holds PARAM.SFO (category SD, TITLE, SUB_TITLE, DETAIL, SAVEDATA_LIST_PARAM, SAVEDATA_DIRECTORY, ATTRIBUTE,
// PARENTAL_LEVEL, and RPCS3's own entries: an integer "*<file>" per file, 1 for a secure file, and RPCS3_BLIST, the
// files in the order they were first written) and the game's files under their own names. Saves move between RPCS3 and
// pspocket as they are.
//
// One operation (savedata_op) calls the game's callbacks on the calling thread (call_guest), with their structures in
// one guest block, as RPCS3's g_savedata_context. The list variants list the saves whose names start with the game's
// prefixes and call funcFixed, which picks one. funcStat sees the save's state (new or not, PARAM.SFO's fields, its
// files) and may set PARAM.SFO's fields; funcFile is then called until it returns OK_LAST, reading, writing or deleting
// one file per call. The files are read into memory first. A modified save is written whole to .working_<dir>, the old
// one becomes .backup_<dir>, the new one takes its place and the backup goes, so a save interrupted at any point
// leaves the old save or the new one.
//
// No dialogs exist yet. The auto and list-auto variants need none. cellSaveDataDelete2 shows the system's list of the
// game's saves to delete from: without one it returns CELL_CANCEL, as RPCS3 does without a save dialog.
// cellSaveDataFixedDelete asks the user to confirm unless the game passes CELL_SAVEDATA_OPTION_NOCONFIRM: without a
// message dialog that is CELL_SAVEDATA_ERROR_INTERNAL, as in RPCS3 when open_msg_dialog can't open one. Errors the PS3
// would show in an error dialog (errDialog) are logged.
//
// Deviations from RPCS3:
// - RPCS3's sleeps, which imitate the PS3's slow save data calls, are left out (as cellGame's, D-044), except the
//   delay after funcFile's OK_LAST (100 ms, 500 ms when the save changed), which RPCS3 notes games rely on.
// - The current user's savedata directory is created at the first call; RPCS3 creates it when it boots a game.
// - Host failures RPCS3 throws on (creating, writing or renaming the directories) log and return
//   CELL_SAVEDATA_ERROR_ACCESS_ERROR, leaving the old save in place.
// - cellSaveDataFixedDelete naming a save that isn't listed calls funcDone with excResult CELL_SAVEDATA_ERROR_NODATA
//   (RPCS3 throws "out of range" there; its delete step reports NODATA for a missing directory). A null funcDone is
//   CELL_SAVEDATA_ERROR_PARAM before anything is deleted (RPCS3 throws after deleting).
// - Directories are listed in name order (RPCS3: the host's order). Only matters when a list holds fewer entries than
//   there are saves.
// - Guest pointers into memory that isn't committed are parameter errors with an error log (RPCS3 faults).
// - The auto save/load indicator (CellSaveDataStatSet::indicator) is not shown: there is no overlay.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "psf.hpp"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kChan = "cellSaveData";

enum : u32 {
    CELL_SAVEDATA_ERROR_CBRESULT = 0x8002b401,
    CELL_SAVEDATA_ERROR_ACCESS_ERROR = 0x8002b402,
    CELL_SAVEDATA_ERROR_INTERNAL = 0x8002b403,
    CELL_SAVEDATA_ERROR_PARAM = 0x8002b404,
    CELL_SAVEDATA_ERROR_NOSPACE = 0x8002b405,
    CELL_SAVEDATA_ERROR_BROKEN = 0x8002b406,
    CELL_SAVEDATA_ERROR_FAILURE = 0x8002b407,
    CELL_SAVEDATA_ERROR_BUSY = 0x8002b408,
    CELL_SAVEDATA_ERROR_NOUSER = 0x8002b409,
    CELL_SAVEDATA_ERROR_SIZEOVER = 0x8002b40a,
    CELL_SAVEDATA_ERROR_NODATA = 0x8002b40b,
    CELL_SAVEDATA_ERROR_NOTSUPPORTED = 0x8002b40c,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// What a callback puts in CellSaveDataCBResult::result.
enum : s32 {
    CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM = 2,
    CELL_SAVEDATA_CBRESULT_OK_LAST = 1,
    CELL_SAVEDATA_CBRESULT_OK_NEXT = 0,
    CELL_SAVEDATA_CBRESULT_ERR_NOSPACE = -1,
    CELL_SAVEDATA_CBRESULT_ERR_FAILURE = -2,
    CELL_SAVEDATA_CBRESULT_ERR_BROKEN = -3,
    CELL_SAVEDATA_CBRESULT_ERR_NODATA = -4,
    CELL_SAVEDATA_CBRESULT_ERR_INVALID = -5,
};

enum : u32 {
    CELL_SAVEDATA_DIRNAME_SIZE = 32,
    CELL_SAVEDATA_FILENAME_SIZE = 13,
    CELL_SAVEDATA_SECUREFILEID_SIZE = 16,
    CELL_SAVEDATA_PREFIX_SIZE = 256,
    CELL_SAVEDATA_DIRLIST_MAX = 2048,
    CELL_SAVEDATA_INVALIDMSG_MAX = 256,
    CELL_SAVEDATA_SYSP_TITLE_SIZE = 128,
    CELL_SAVEDATA_SYSP_SUBTITLE_SIZE = 128,
    CELL_SAVEDATA_SYSP_DETAIL_SIZE = 1024,
    CELL_SAVEDATA_SYSP_LPARAM_SIZE = 8,

    CELL_SAVEDATA_SORTTYPE_MODIFIEDTIME = 0,
    CELL_SAVEDATA_SORTTYPE_SUBTITLE = 1,
    CELL_SAVEDATA_SORTORDER_DESCENT = 0,
    CELL_SAVEDATA_SORTORDER_ASCENT = 1,

    CELL_SAVEDATA_FILEOP_READ = 0,
    CELL_SAVEDATA_FILEOP_WRITE = 1,
    CELL_SAVEDATA_FILEOP_DELETE = 2,
    CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC = 3,

    CELL_SAVEDATA_FILETYPE_SECUREFILE = 0,
    CELL_SAVEDATA_FILETYPE_NORMALFILE = 1,
    CELL_SAVEDATA_FILETYPE_CONTENT_ICON0 = 2,
    CELL_SAVEDATA_FILETYPE_CONTENT_ICON1 = 3,
    CELL_SAVEDATA_FILETYPE_CONTENT_PIC1 = 4,
    CELL_SAVEDATA_FILETYPE_CONTENT_SND0 = 5,

    CELL_SAVEDATA_ATTR_NODUPLICATE = 1,

    CELL_SAVEDATA_RECREATE_NO = 0,
    CELL_SAVEDATA_RECREATE_NO_NOBROKEN = 1,
    CELL_SAVEDATA_RECREATE_YES = 2,
    CELL_SAVEDATA_RECREATE_YES_RESET_OWNER = 3,
    CELL_SAVEDATA_RECREATE_MASK = 0xfffeffff, // bit 16: CELL_SAVEDATA_DISABLE_TROPHY_OWNERSHIP_CHECK

    CELL_SAVEDATA_VERSION_420 = 1,

    CELL_SAVEDATA_ERRDIALOG_NONE = 0,
    CELL_SAVEDATA_ERRDIALOG_ALWAYS = 1,
    CELL_SAVEDATA_ERRDIALOG_NOREPEAT = 2,

    CELL_SAVEDATA_OPTION_NONE = 0,
    CELL_SAVEDATA_OPTION_NOCONFIRM = 1,
};

// RPCS3's operations (savedata_op). The import and export ones (9 to 12) belong to functions GTA V doesn't import.
enum : u32 {
    SAVEDATA_OP_AUTO_SAVE = 0,
    SAVEDATA_OP_AUTO_LOAD = 1,
    SAVEDATA_OP_LIST_AUTO_SAVE = 2,
    SAVEDATA_OP_LIST_AUTO_LOAD = 3,
    SAVEDATA_OP_LIST_SAVE = 4,
    SAVEDATA_OP_LIST_LOAD = 5,
    SAVEDATA_OP_FIXED_SAVE = 6,
    SAVEDATA_OP_FIXED_LOAD = 7,
    SAVEDATA_OP_LIST_DELETE = 13,
    SAVEDATA_OP_FIXED_DELETE = 14,
};

// RPCS3 reports 40 GB free less 256 KB (see its cellHddGameCheck), and 35 KB for PARAM.SFO and PARAM.PFD.
constexpr s32 kHddFreeSizeKB = 40 * 1024 * 1024 - 256;
constexpr s32 kSysSizeKB = 35;

// ---- guest structures (cellSaveData.h): big-endian, 32-bit pointers; offsets in bytes ----

namespace set_list { // CellSaveDataSetList
enum : u32 { sortType = 0, sortOrder = 4, dirNamePrefix = 8, reserved = 12, kSize = 16 };
}
namespace set_buf { // CellSaveDataSetBuf
enum : u32 { dirListMax = 0, fileListMax = 4, reserved = 8, kReservedWords = 6, bufSize = 32, buf = 36, kSize = 40 };
}
namespace dir_list { // CellSaveDataDirList
enum : u32 { dirName = 0, listParam = 32, kSize = 48 };
}
namespace list_get { // CellSaveDataListGet
enum : u32 { dirNum = 0, dirListNum = 4, dirList = 8 };
}
namespace fixed_set { // CellSaveDataFixedSet
enum : u32 { dirName = 0, newIcon = 4, option = 8 };
}
namespace sys_param { // CellSaveDataSystemFileParam
enum : u32 {
    title = 0x000,
    subTitle = 0x080,
    detail = 0x100,
    attribute = 0x500,
    parental_level = 0x504, // reserved2[4] from firmware 3.70
    listParam = 0x508,
    reserved = 0x510,
    kReservedSize = 256,
    kSize = 0x610,
};
}
namespace dir_stat { // CellSaveDataDirStat
enum : u32 { atime = 0, mtime = 8, ctime = 16, dirName = 24, kSize = 56 };
}
namespace file_stat { // CellSaveDataFileStat
enum : u32 { fileType = 0, size = 8, atime = 16, mtime = 24, ctime = 32, fileName = 40, kSize = 56 };
}
namespace stat_get { // CellSaveDataStatGet
enum : u32 {
    hddFreeSizeKB = 0,
    isNewData = 4,
    dir = 8, // CellSaveDataDirStat
    getParam = 64, // CellSaveDataSystemFileParam
    bind = 1616,
    sizeKB = 1620,
    sysSizeKB = 1624,
    fileNum = 1628,
    fileListNum = 1632,
    fileList = 1636,
};
}
namespace stat_set { // CellSaveDataStatSet
enum : u32 { setParam = 0, reCreateMode = 4, indicator = 8 };
}
namespace file_get { // CellSaveDataFileGet
enum : u32 { excSize = 0, reserved = 4, kReservedSize = 64 };
}
namespace file_set { // CellSaveDataFileSet (secureFileId is unaligned: be_t<u128, 1> in RPCS3)
enum : u32 {
    fileOperation = 0,
    reserved = 4,
    fileType = 8,
    secureFileId = 12,
    fileName = 28,
    fileOffset = 32,
    fileSize = 36,
    fileBufSize = 40,
    fileBuf = 44,
    kSize = 48,
};
}
namespace cb_result { // CellSaveDataCBResult
enum : u32 { result = 0, progressBarInc = 4, errNeedSizeKB = 8, invalidMsg = 12, userdata = 16 };
}
namespace done_get { // CellSaveDataDoneGet
enum : u32 { excResult = 0, dirName = 4, sizeKB = 36, hddFreeSizeKB = 40 };
}

// The callbacks' structures in one guest block, as RPCS3's savedata_context (a gvar there): each 16-byte aligned.
namespace context {
enum : u32 {
    result = 0,      // CellSaveDataCBResult, 20 bytes
    listGet = 32,    // CellSaveDataListGet, 76
    listSet = 112,   // CellSaveDataListSet, 24 (for the funcList variants)
    fixedSet = 144,  // CellSaveDataFixedSet, 12
    statGet = 160,   // CellSaveDataStatGet, 1704
    statSet = 1872,  // CellSaveDataStatSet, 12
    fileGet = 1888,  // CellSaveDataFileGet, 68
    fileSet = 1968,  // CellSaveDataFileSet, 48
    doneGet = 2016,  // CellSaveDataDoneGet, 108
    kSize = 2128,
};
}

// ---- per-process state ----

// RPCS3 savedata_manager, and the guest block of g_savedata_context.
struct SaveDataState {
    explicit SaveDataState(HleProcess& p) : process(p) {}

    HleProcess& process;
    // RPCS3's mutex, taken with try_lock: a second operation while one runs is CELL_SAVEDATA_ERROR_BUSY. A flag rather
    // than a mutex, because a callback may call back into save data on the same thread.
    std::atomic<bool> busy{false};
    std::atomic<s32> last_cbresult_error_dialog{0}; // for CELL_SAVEDATA_ERRDIALOG_NOREPEAT
    u32 context = 0; // allocated by the first operation; only the busy holder touches it
};

SaveDataState& save_state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSaveData called without an HLE process");
    return p->state<SaveDataState>();
}

class BusyGuard {
public:
    explicit BusyGuard(std::atomic<bool>& flag)
        : flag_(flag), owned_(!flag.exchange(true, std::memory_order_acquire))
    {
    }
    ~BusyGuard()
    {
        if (owned_) flag_.store(false, std::memory_order_release);
    }
    BusyGuard(const BusyGuard&) = delete;
    BusyGuard& operator=(const BusyGuard&) = delete;
    bool owned() const { return owned_; }

private:
    std::atomic<bool>& flag_;
    const bool owned_;
};

// ---- results and logs ----

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case 1: return "CELL_CANCEL";
    case CELL_SAVEDATA_ERROR_CBRESULT: return "CELL_SAVEDATA_ERROR_CBRESULT";
    case CELL_SAVEDATA_ERROR_ACCESS_ERROR: return "CELL_SAVEDATA_ERROR_ACCESS_ERROR";
    case CELL_SAVEDATA_ERROR_INTERNAL: return "CELL_SAVEDATA_ERROR_INTERNAL";
    case CELL_SAVEDATA_ERROR_PARAM: return "CELL_SAVEDATA_ERROR_PARAM";
    case CELL_SAVEDATA_ERROR_NOSPACE: return "CELL_SAVEDATA_ERROR_NOSPACE";
    case CELL_SAVEDATA_ERROR_BROKEN: return "CELL_SAVEDATA_ERROR_BROKEN";
    case CELL_SAVEDATA_ERROR_FAILURE: return "CELL_SAVEDATA_ERROR_FAILURE";
    case CELL_SAVEDATA_ERROR_BUSY: return "CELL_SAVEDATA_ERROR_BUSY";
    case CELL_SAVEDATA_ERROR_NOUSER: return "CELL_SAVEDATA_ERROR_NOUSER";
    case CELL_SAVEDATA_ERROR_SIZEOVER: return "CELL_SAVEDATA_ERROR_SIZEOVER";
    case CELL_SAVEDATA_ERROR_NODATA: return "CELL_SAVEDATA_ERROR_NODATA";
    case CELL_SAVEDATA_ERROR_NOTSUPPORTED: return "CELL_SAVEDATA_ERROR_NOTSUPPORTED";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

std::string cbresult_name(s32 r)
{
    switch (r) {
    case CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM: return "OK_LAST_NOCONFIRM";
    case CELL_SAVEDATA_CBRESULT_OK_LAST: return "OK_LAST";
    case CELL_SAVEDATA_CBRESULT_OK_NEXT: return "OK_NEXT";
    case CELL_SAVEDATA_CBRESULT_ERR_NOSPACE: return "ERR_NOSPACE";
    case CELL_SAVEDATA_CBRESULT_ERR_FAILURE: return "ERR_FAILURE";
    case CELL_SAVEDATA_CBRESULT_ERR_BROKEN: return "ERR_BROKEN";
    case CELL_SAVEDATA_CBRESULT_ERR_NODATA: return "ERR_NODATA";
    case CELL_SAVEDATA_CBRESULT_ERR_INVALID: return "ERR_INVALID";
    }
    return std::format("{} (not a callback result)", r);
}

// A function's result, logged when it returns: errors as warnings.
s32 finish(std::string_view fn, s32 r)
{
    if (r < 0) PS_LOG_WARN(kChan, "{}: {}", fn, result_name(r));
    else PS_LOG_INFO(kChan, "{}: {}", fn, result_name(r));
    return r;
}

// RPCS3's "****** sysutil savedata parameter error : N ******": the number a PS3 logs for each failed check.
s32 param_error(u32 n, std::string_view detail = {})
{
    PS_LOG_ERROR(kChan, "sysutil savedata parameter error : {}{}{}", n, detail.empty() ? "" : " ", detail);
    return error(CELL_SAVEDATA_ERROR_PARAM);
}

// CELL_SAVEDATA_ERROR_PARAM for a check of pspocket's own (RPCS3 faults or throws there), with why.
s32 param_refused(std::string_view why)
{
    PS_LOG_ERROR(kChan, "{}: CELL_SAVEDATA_ERROR_PARAM", why);
    return error(CELL_SAVEDATA_ERROR_PARAM);
}

// ---- guest memory ----

// Whether all of [addr, addr + size) is committed guest memory (checked per host page).
bool guest_range_ok(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (addr + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < addr + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

// A structure the game passed, checked before it is read or written: an error log when it isn't all in memory.
bool struct_ok(lv2::PpuThread& t, u32 addr, u64 size, std::string_view what)
{
    if (guest_range_ok(t, addr, size)) return true;
    PS_LOG_ERROR(kChan, "{} at {:#010x} ({:#x} bytes) is outside committed guest memory", what, addr, size);
    return false;
}

struct GuestString {
    std::string text;        // up to the NUL, or the first `max` bytes
    bool terminated = false; // a NUL within `max` bytes
    bool faulted = false;    // ran into memory that isn't committed first
};

GuestString read_cstr(lv2::PpuThread& t, u32 addr, u32 max)
{
    GuestString s;
    u8* base = t.kernel.base();
    for (u32 i = 0; i < max; i++) {
        if (!guest_readable(t, addr + i, 1)) {
            s.faulted = true;
            break;
        }
        const char c = static_cast<char>(ps_read8(base, addr + i));
        if (!c) {
            s.terminated = true;
            break;
        }
        s.text += c;
    }
    return s;
}

// A char array of `size` bytes in a structure already checked: up to its first NUL (RPCS3's psf::string(char[N])).
std::string read_chars(u8* base, u32 addr, u32 size)
{
    std::string s;
    for (u32 i = 0; i < size; i++) {
        const char c = static_cast<char>(ps_read8(base, addr + i));
        if (!c) break;
        s += c;
    }
    return s;
}

// RPCS3 strcpy_trunc into a char array of `capacity` bytes: at most capacity - 1 bytes, the rest zeroed.
void put_string(u8* base, u32 addr, u32 capacity, std::string_view s)
{
    if (!capacity) return;
    const usize n = std::min<usize>(s.size(), capacity - 1);
    std::memcpy(base + addr, s.data(), n);
    std::memset(base + addr + n, 0, capacity - n);
}

// For logs: a guest string argument.
std::string describe(lv2::PpuThread& t, u32 addr, u32 max)
{
    if (!addr) return "null";
    const GuestString s = read_cstr(t, addr, max);
    return std::format("\"{}\"{}", s.text, s.faulted ? " (outside memory)" : s.terminated ? "" : "...");
}

// ---- names (RPCS3 sysutil_check_name_string, check_filename, is_valid_dir_name) ----

// [A-Z0-9_-], not starting with '_', minlen to maxlen characters; 0, -1 for a bad character, -2 for a bad length.
// Games built with SDK 3.7 or newer must end the string inside its maxlen bytes; older ones may fill them. `s` is the
// string up to its NUL (or a guest string's first maxlen + 1 bytes): past its end reads as NUL, as RPCS3's pointer
// would at the terminator.
s32 check_name_string(std::string_view s, s32 minlen, s32 maxlen, u32 sdk)
{
    const s32 lastpos = sdk > 0x36FFFF ? std::max(maxlen - 1, 0) : maxlen;
    const auto at = [&](s32 i) { return i < static_cast<s32>(s.size()) ? s[static_cast<usize>(i)] : '\0'; };
    char cur = at(0);
    if (cur == '_') return -1;
    for (s32 index = 0;; cur = at(++index)) {
        if (cur == '\0' || index == maxlen) return minlen > index || (maxlen == index && at(lastpos)) ? -2 : 0;
        if ((cur >= 'A' && cur <= 'Z') || (cur >= '0' && cur <= '9') || cur == '-' || cur == '_') continue;
        return -1;
    }
}

// RPCS3 check_filename: 0, or the parameter error number (70, 71) a file name gets. 8.3 names in [A-Z0-9_-] (more
// dots allowed in the name, '_' allowed first). PARAM.SFO and PARAM.PFD only pass with `account_sfo_pfd`; names
// starting PARAM, ICON, PIC or SND fail with `disallow_system_files`.
u32 check_filename(std::string_view file_path, bool disallow_system_files, bool account_sfo_pfd, u32 sdk)
{
    if (file_path.size() >= CELL_SAVEDATA_FILENAME_SIZE) return 71;
    usize dotpos = file_path.find_last_of('.');
    if (dotpos == std::string_view::npos) dotpos = file_path.size();
    if (file_path.empty() || dotpos > 8 || file_path.size() - dotpos > 4) return 70;
    if (file_path == "." || (!account_sfo_pfd && (file_path == "PARAM.SFO" || file_path == "PARAM.PFD"))) return 70;
    if (dotpos) {
        std::string name(file_path.substr(0, dotpos));
        std::replace(name.begin(), name.end(), '.', '-');
        if (name[0] == '_') name[0] = '-';
        const bool system_name = name.starts_with("PARAM") || name.starts_with("ICON") || name.starts_with("PIC") ||
            name.starts_with("SND");
        if (disallow_system_files && system_name) return 70;
        if (check_name_string(name, 1, 9, sdk) == -1) return 70;
    }
    if (file_path.size() > dotpos + 1) {
        std::string ext(file_path.substr(dotpos + 1));
        if (ext[0] == '_') ext[0] = '-';
        if (check_name_string(ext, 1, 4, sdk) == -1) return 70;
    }
    return 0;
}

bool is_valid_dir_name(std::string_view name, u32 sdk)
{
    return !name.empty() && check_name_string(name, 1, CELL_SAVEDATA_DIRNAME_SIZE, sdk) == 0;
}

// ---- host files ----

struct HostEntry {
    std::string name;
    bool is_directory = false;
    u64 size = 0;
    s64 atime = 0, mtime = 0, ctime = 0;
};

// RPCS3 fs::get_stat on POSIX: ctime is reported as mtime, and atime is at least mtime.
bool host_stat(const std::string& path, HostEntry& out)
{
    struct ::stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    out.is_directory = S_ISDIR(st.st_mode);
    out.size = static_cast<u64>(st.st_size);
    out.atime = static_cast<s64>(st.st_atime);
    out.mtime = static_cast<s64>(st.st_mtime);
    out.ctime = out.mtime;
    out.atime = std::max(out.atime, out.mtime);
    return true;
}

// A host directory's entries (RPCS3 fs::dir), in name order; none when it doesn't exist.
std::vector<HostEntry> host_dir(const std::string& path)
{
    std::vector<HostEntry> out;
    std::error_code ec;
    fs::directory_iterator it(path, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        HostEntry e;
        e.name = it->path().filename().string();
        if (host_stat(it->path().string(), e)) out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const HostEntry& a, const HostEntry& b) { return a.name < b.name; });
    return out;
}

bool host_is_dir(const std::string& path)
{
    std::error_code ec;
    return fs::is_directory(path, ec);
}

std::optional<std::vector<u8>> read_host_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return bytes;
}

// Written and synced before the directory renames that commit a save (RPCS3 fs::write_file, then fs::sync).
bool write_host_file(const std::string& path, std::span<const u8> data)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    usize done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        done += static_cast<usize>(n);
    }
    const bool synced = ::fsync(fd) == 0;
    return ::close(fd) == 0 && synced;
}

// RPCS3 fs::utime.
void set_host_times(const std::string& path, s64 atime, s64 mtime)
{
    const struct ::timeval times[2] = {{static_cast<time_t>(atime), 0}, {static_cast<time_t>(mtime), 0}};
    if (::utimes(path.c_str(), times) != 0) PS_LOG_WARN(kChan, "cannot restore the times of {}", path);
}

// The current user's save data directory on the host, with a trailing '/': /dev_hdd0/home/00000001/savedata/ (RPCS3:
// user 0 is the current user, 00000001). RPCS3 creates it when it boots a game; here the first save data call does.
// Empty (logged) when /dev_hdd0 is not mounted on a host directory.
std::string savedata_dir(lv2::Kernel& kernel)
{
    const std::string host = kernel.vfs_host_path("/dev_hdd0/home/00000001/savedata");
    if (host.empty()) {
        PS_LOG_ERROR(kChan, "/dev_hdd0 is not mounted on a host directory: there is no place for save data");
        return {};
    }
    std::error_code ec;
    fs::create_directories(host, ec);
    if (ec) {
        PS_LOG_ERROR(kChan, "cannot create {} ({})", host, ec.message());
        return {};
    }
    return host + "/";
}

// ---- PARAM.SFO ----

// RPCS3 psf::load_object: empty when the file is missing, isn't a PSF, or has no known category.
psf::Registry load_sfo(const std::string& path)
{
    const auto bytes = read_host_file(path);
    if (!bytes) return {};
    std::optional<psf::Registry> parsed = psf::parse(*bytes);
    if (!parsed) {
        PS_LOG_ERROR(kChan, "{} is not a valid PARAM.SFO", path);
        return {};
    }
    static constexpr std::string_view kCategories[] = {"GD", "DG", "HG", "AM", "AP", "AS", "AT", "AV", "BV", "WT", "HM",
        "CB", "SF", "2P", "2G", "1P", "PP", "MN", "PE", "2D", "SD", "MS"};
    const std::string category = psf::get_string(*parsed, "CATEGORY");
    if (std::find(std::begin(kCategories), std::end(kCategories), category) == std::end(kCategories)) {
        PS_LOG_ERROR(kChan, "{}: unknown category \"{}\"", path, category);
        return {};
    }
    return std::move(*parsed);
}

u32 get_integer(const psf::Registry& sfo, std::string_view key)
{
    const auto it = sfo.find(key);
    return it != sfo.end() && it->second.format == psf::Format::Integer ? it->second.integer : 0;
}

psf::Entry integer_entry(u32 value) { return {psf::Format::Integer, 4, {}, value}; }
psf::Entry array_entry(u32 max, std::string_view value) { return {psf::Format::Array, max, std::string(value), 0}; }

// RPCS3 fmt::split and fmt::merge.
std::vector<std::string> split(std::string_view source, char separator, bool skip_empty)
{
    std::vector<std::string> out;
    for (usize index = 0; index < source.size();) {
        const usize pos = source.find(separator, index);
        if (pos == std::string_view::npos) {
            out.emplace_back(source.substr(index));
            return out;
        }
        const std::string_view piece = source.substr(index, pos - index);
        index = pos + 1;
        if (piece.empty() && skip_empty) continue;
        out.emplace_back(piece);
    }
    if (out.empty() && !skip_empty) out.emplace_back();
    return out;
}

std::string merge(const std::vector<std::string>& parts, std::string_view separator)
{
    std::string out;
    for (usize i = 0; i < parts.size(); i++) {
        if (i) out += separator;
        out += parts[i];
    }
    return out;
}

// RPCS3 SaveDataEntry. `dir_name` is PARAM.SFO's SAVEDATA_DIRECTORY, `folder` the host directory's name.
struct SaveEntry {
    std::string folder;
    std::string dir_name;
    std::string list_param;
    std::string title;
    std::string subtitle;
    std::string details;
    u64 size = 0;
    s64 atime = 0, mtime = 0, ctime = 0;
    bool is_new = false;
};

// ---- savedata_op ----

struct OpArgs {
    u32 operation = 0;
    u32 version = 0;
    u32 dir_name = 0;
    u32 err_dialog = 0;
    u32 set_list = 0;
    u32 set_buf = 0;
    u32 func_list = 0; // the list dialog variants (none implemented): always 0 here
    u32 func_fixed = 0;
    u32 func_stat = 0;
    u32 func_file = 0;
    u32 unk_op_flags = 0;
    u32 userdata = 0;
    u32 func_done = 0;
};

// RPCS3 savedata_check_args: 0, or the parameter error number. User IDs are always the current user's (no
// cellSaveDataUser* function is implemented), so RPCS3's user ID check (91) can't fail.
u32 savedata_check_args(lv2::PpuThread& t, const OpArgs& a)
{
    u8* base = t.kernel.base();
    const u32 sdk = t.kernel.config().sdk_version;
    const u32 op = a.operation;
    if (a.version > CELL_SAVEDATA_VERSION_420) return 1;
    if (a.err_dialog > CELL_SAVEDATA_ERRDIALOG_NOREPEAT) return 5;

    if (op <= SAVEDATA_OP_AUTO_LOAD) {
        if (!a.dir_name) return 2;
        const GuestString dir = read_cstr(t, a.dir_name, CELL_SAVEDATA_DIRNAME_SIZE + 1);
        if (dir.faulted) {
            PS_LOG_ERROR(kChan, "dirName at {:#010x} is outside committed guest memory", a.dir_name);
            return 2;
        }
        switch (check_name_string(dir.text, 1, CELL_SAVEDATA_DIRNAME_SIZE, sdk)) {
        case -1: return 3;
        case -2: return 4;
        default: break;
        }
    }

    const bool listed = (op >= SAVEDATA_OP_LIST_AUTO_SAVE && op <= SAVEDATA_OP_FIXED_LOAD) ||
        op == SAVEDATA_OP_LIST_DELETE || op == SAVEDATA_OP_FIXED_DELETE;
    if (listed) {
        if (!a.set_list || !struct_ok(t, a.set_list, set_list::kSize, "setList")) return 11;
        if (ps_read32(base, a.set_list + set_list::sortType) > CELL_SAVEDATA_SORTTYPE_SUBTITLE) return 12;
        if (ps_read32(base, a.set_list + set_list::sortOrder) > CELL_SAVEDATA_SORTORDER_ASCENT) return 13;
        const u32 prefix_addr = ps_read32(base, a.set_list + set_list::dirNamePrefix);
        if (!prefix_addr) return 15;
        const GuestString prefix = read_cstr(t, prefix_addr, CELL_SAVEDATA_PREFIX_SIZE);
        if (!prefix.terminated || (sdk > 0x3FFFFF && prefix.text.empty())) return 17;
        const bool allow_asterisk = op == SAVEDATA_OP_LIST_DELETE;
        if (!allow_asterisk || prefix.text != "*") {
            // Each '|'-separated prefix, through a buffer that is not cleared between them (as RPCS3's).
            char buf[CELL_SAVEDATA_DIRNAME_SIZE + 1] = {};
            for (usize pos = 0, posprefix = 0;;) {
                const char cur = pos < prefix.text.size() ? prefix.text[pos] : '\0';
                pos++;
                if (cur == '\0' || cur == '|') {
                    if (posprefix) {
                        switch (check_name_string(buf, 1, CELL_SAVEDATA_DIRNAME_SIZE, sdk)) {
                        case -1: return 16;
                        case -2: return 17;
                        default: break;
                        }
                    }
                    if (cur == '\0') break;
                    posprefix = 0;
                    continue;
                }
                if (posprefix == CELL_SAVEDATA_DIRNAME_SIZE) return 17;
                buf[posprefix++] = cur;
            }
        }
        if (ps_read32(base, a.set_list + set_list::reserved)) return 14;
    }

    if (!a.set_buf || !struct_ok(t, a.set_buf, set_buf::kSize, "setBuf")) return 74;
    const u64 dir_list_max = ps_read32(base, a.set_buf + set_buf::dirListMax);
    const u64 file_list_max = ps_read32(base, a.set_buf + set_buf::fileListMax);
    const u64 buf_size = ps_read32(base, a.set_buf + set_buf::bufSize);
    if (listed) {
        if (dir_list_max > CELL_SAVEDATA_DIRLIST_MAX) return 8;
        if (dir_list_max * dir_list::kSize > buf_size) return 7;
    }
    if (op == SAVEDATA_OP_LIST_DELETE || op == SAVEDATA_OP_FIXED_DELETE) {
        if (file_list_max != 0) return 9;
    } else if (file_list_max * file_stat::kSize > buf_size) {
        return 7;
    }
    if (buf_size && !ps_read32(base, a.set_buf + set_buf::buf)) return 6;
    for (u32 i = 0; i < set_buf::kReservedWords; i++)
        if (ps_read32(base, a.set_buf + set_buf::reserved + 4 * i)) return 10;

    if ((op == SAVEDATA_OP_LIST_SAVE || op == SAVEDATA_OP_LIST_LOAD || op == SAVEDATA_OP_LIST_DELETE) && !a.func_list)
        return 18;
    if ((op == SAVEDATA_OP_FIXED_SAVE || op == SAVEDATA_OP_FIXED_LOAD || op == SAVEDATA_OP_LIST_AUTO_LOAD ||
            op == SAVEDATA_OP_LIST_AUTO_SAVE || op == SAVEDATA_OP_FIXED_DELETE) &&
        !a.func_fixed) {
        return 19;
    }
    // funcStat and funcFile are not part of the delete functions.
    if (op != SAVEDATA_OP_LIST_DELETE && op != SAVEDATA_OP_FIXED_DELETE &&
        (!(a.unk_op_flags & 0x2) || op == SAVEDATA_OP_AUTO_SAVE || op == SAVEDATA_OP_AUTO_LOAD)) {
        if (!a.func_stat) return 20;
        if (!(a.unk_op_flags & 0x2) && !a.func_file) return 18;
    }
    return 0;
}

// RPCS3 display_callback_result_error_message: a callback's error result. The PS3 shows it in an error dialog unless
// errDialog is NONE (or NOREPEAT and the same error was the last one shown); no dialog exists here, so it is logged.
s32 callback_error(lv2::PpuThread& t, u32 result, u32 err_dialog, std::string_view callback)
{
    u8* base = t.kernel.base();
    const s32 res = static_cast<s32>(ps_read32(base, result + cb_result::result));
    std::string message;
    switch (res) {
    case CELL_SAVEDATA_CBRESULT_ERR_NOSPACE:
        message = std::format("Not enough free space. {} KB more are needed.",
            static_cast<s32>(ps_read32(base, result + cb_result::errNeedSizeKB)));
        break;
    case CELL_SAVEDATA_CBRESULT_ERR_FAILURE: message = "The operation failed."; break;
    case CELL_SAVEDATA_CBRESULT_ERR_BROKEN: message = "The save data is corrupted."; break;
    case CELL_SAVEDATA_CBRESULT_ERR_NODATA: message = "There is no save data."; break;
    case CELL_SAVEDATA_CBRESULT_ERR_INVALID:
        if (const u32 msg = ps_read32(base, result + cb_result::invalidMsg))
            message = read_cstr(t, msg, CELL_SAVEDATA_INVALIDMSG_MAX).text;
        break;
    default: return param_error(22, std::format("({} returned {})", callback, cbresult_name(res)));
    }
    SaveDataState& s = save_state();
    if (err_dialog == CELL_SAVEDATA_ERRDIALOG_NONE ||
        (err_dialog == CELL_SAVEDATA_ERRDIALOG_NOREPEAT && res == s.last_cbresult_error_dialog.exchange(res))) {
        PS_LOG_INFO(kChan, "  {} returned {}", callback, cbresult_name(res));
        return error(CELL_SAVEDATA_ERROR_CBRESULT);
    }
    PS_LOG_WARN(kChan, "  {} returned {}; the PS3's error dialog (not shown): \"{}\"", callback, cbresult_name(res),
        message);
    return error(CELL_SAVEDATA_ERROR_CBRESULT);
}

// Calls a callback with its structures, the result cleared up to the userdata the game may change (RPCS3 memsets
// CellSaveDataCBResult up to userdata before each call). Returns what it put in the result.
s32 call_callback(PPUContext& ctx, u32 func, u32 result, u32 get, std::optional<u32> set)
{
    u8* base = guest_base(ctx);
    std::memset(base + result, 0, cb_result::userdata);
    if (set) call_guest(ctx, func, {result, get, *set});
    else call_guest(ctx, func, {result, get});
    return static_cast<s32>(ps_read32(base, result + cb_result::result));
}

// The guest addresses of the context's structures.
struct Context {
    u32 result, listGet, fixedSet, statGet, statSet, fileGet, fileSet, doneGet;

    explicit Context(u32 c)
        : result(c + context::result), listGet(c + context::listGet), fixedSet(c + context::fixedSet),
          statGet(c + context::statGet), statSet(c + context::statSet), fileGet(c + context::fileGet),
          fileSet(c + context::fileSet), doneGet(c + context::doneGet)
    {
    }
};

// RPCS3's delete_save (FIXED_DELETE): the save goes (renamed to .backup_<dir>, then removed), and funcDone hears how
// it went. `entry` is null when funcFixed named a save that isn't listed.
s32 delete_save(PPUContext& ctx, const OpArgs& a, const Context& c, const std::string& base_dir,
    const SaveEntry* entry, const std::string& dir_name)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!a.func_done) return param_refused("funcDone is null (RPCS3 throws, after deleting)");
    put_string(base, c.doneGet + done_get::dirName, CELL_SAVEDATA_DIRNAME_SIZE, entry ? entry->dir_name : dir_name);
    ps_write32(base, c.doneGet + done_get::hddFreeSizeKB, static_cast<u32>(kHddFreeSizeKB));
    ps_write32(base, c.doneGet + done_get::excResult, lv2::CELL_OK);
    ps_write32(base, c.doneGet + done_get::sizeKB, 0);
    if (!entry) {
        PS_LOG_WARN(kChan, "  funcFixed chose \"{}\", which is not listed: nothing to delete", dir_name);
        ps_write32(base, c.doneGet + done_get::excResult, CELL_SAVEDATA_ERROR_NODATA);
    } else {
        const std::string del_path = base_dir + entry->folder, old_path = base_dir + ".backup_" + entry->folder;
        u64 size_bytes = 0;
        for (const HostEntry& file : host_dir(del_path))
            if (!file.is_directory) size_bytes += (file.size + 1023) & ~u64{1023};
        ps_write32(base, c.doneGet + done_get::sizeKB, static_cast<u32>(size_bytes / 1024));
        if (host_is_dir(del_path)) {
            std::error_code ec;
            fs::remove_all(old_path, ec);
            fs::rename(del_path, old_path, ec);
            if (ec) {
                PS_LOG_ERROR(kChan, "cannot move {} to {} ({})", del_path, old_path, ec.message());
                return error(CELL_SAVEDATA_ERROR_ACCESS_ERROR);
            }
            fs::remove_all(old_path, ec);
            PS_LOG_INFO(kChan, "  deleted {} ({} KB)", del_path, size_bytes / 1024);
        } else {
            ps_write32(base, c.doneGet + done_get::excResult, CELL_SAVEDATA_ERROR_NODATA);
        }
    }
    const s32 res = call_callback(ctx, a.func_done, c.result, c.doneGet, std::nullopt);
    if (res != CELL_SAVEDATA_CBRESULT_OK_NEXT) {
        if (res == CELL_SAVEDATA_CBRESULT_OK_LAST || res == CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM)
            return lv2::CELL_OK;
        return callback_error(t, c.result, a.err_dialog, "funcDone");
    }
    return lv2::CELL_OK;
}

// RPCS3's last step of savedata_op: every file of the save, with the new PARAM.SFO, goes to .working_<dir>; the old
// save becomes .backup_<dir>, the new one takes its place, and the backup goes.
s32 commit_save(const std::string& dir_path, const std::string& new_path, const std::string& old_path,
    psf::Registry& sfo, const std::vector<std::string>& blist, std::map<std::string, std::vector<u8>>& files,
    const std::map<std::string, std::pair<s64, s64>>& times)
{
    std::error_code ec;
    fs::create_directory(new_path, ec);
    if (!ec) {
        // An earlier attempt may have left files in it.
        for (const HostEntry& e : host_dir(new_path)) fs::remove_all(new_path + "/" + e.name, ec);
    }
    if (ec) {
        PS_LOG_ERROR(kChan, "cannot create {} ({})", new_path, ec.message());
        return error(CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    }
    const std::string order = merge(blist, "/");
    sfo["RPCS3_BLIST"] = psf::string(static_cast<u32>((order.size() + 1 + 3) & ~usize{3}), order);
    files["PARAM.SFO"] = psf::save(sfo);
    for (const auto& [name, data] : files) {
        if (!write_host_file(new_path + "/" + name, data)) {
            PS_LOG_ERROR(kChan, "cannot write {}/{} ({})", new_path, name, std::strerror(errno));
            return error(CELL_SAVEDATA_ERROR_ACCESS_ERROR);
        }
    }
    // The files that were not written keep their times.
    for (const auto& [name, t] : times) set_host_times(new_path + "/" + name, t.first, t.second);
    fs::remove_all(old_path, ec);
    fs::rename(dir_path, old_path, ec);
    if (ec) {
        PS_LOG_ERROR(kChan, "cannot move {} to {} ({})", dir_path, old_path, ec.message());
        return error(CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    }
    fs::rename(new_path, dir_path, ec);
    if (ec) {
        PS_LOG_ERROR(kChan, "cannot move {} to {} ({}); the old save stays", new_path, dir_path, ec.message());
        std::error_code ignored;
        fs::rename(old_path, dir_path, ignored);
        return error(CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    }
    fs::remove_all(old_path, ec);
    return lv2::CELL_OK;
}

std::string_view file_op_name(u32 op)
{
    switch (op) {
    case CELL_SAVEDATA_FILEOP_READ: return "READ";
    case CELL_SAVEDATA_FILEOP_WRITE: return "WRITE";
    case CELL_SAVEDATA_FILEOP_DELETE: return "DELETE";
    case CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC: return "WRITE_NOTRUNC";
    }
    return "?";
}

// RPCS3 savedata_op, without its list dialog (funcList: no function GTA V imports has one).
s32 savedata_op(PPUContext& ctx, const OpArgs& a)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 sdk = t.kernel.config().sdk_version;
    const u32 op = a.operation;

    if (const u32 e = savedata_check_args(t, a)) return param_error(e);

    SaveDataState& s = save_state();
    BusyGuard busy(s.busy);
    if (!busy.owned()) return error(CELL_SAVEDATA_ERROR_BUSY);
    if (!s.context) s.context = s.process.main_memory().alloc(context::kSize, 0x100);
    if (!s.context) {
        PS_LOG_ERROR(kChan, "no main-area memory for the callbacks' structures");
        return error(CELL_SAVEDATA_ERROR_INTERNAL);
    }
    std::memset(base + s.context, 0, context::kSize);
    const Context c(s.context);

    const std::string base_dir = savedata_dir(t.kernel);
    if (base_dir.empty()) return error(CELL_SAVEDATA_ERROR_ACCESS_ERROR);

    const u32 buf = ps_read32(base, a.set_buf + set_buf::buf);
    const u32 buf_size = ps_read32(base, a.set_buf + set_buf::bufSize);
    if (!struct_ok(t, buf, buf_size, "setBuf->buf")) return param_refused("setBuf->buf");

    ps_write32(base, c.result + cb_result::userdata, a.userdata);

    SaveEntry save_entry;

    if (a.set_list) {
        std::vector<SaveEntry> save_entries;
        const u32 dir_list_max = ps_read32(base, a.set_buf + set_buf::dirListMax);
        ps_write32(base, c.listGet + list_get::dirList, buf);

        const std::string prefixes =
            read_cstr(t, ps_read32(base, a.set_list + set_list::dirNamePrefix), CELL_SAVEDATA_PREFIX_SIZE).text;
        std::vector<std::string> prefix_list = split(prefixes, '|', true);
        // An empty prefix lists every save for these (and, with RPCS3's '*', for LIST_DELETE: not implemented).
        if (prefix_list.empty() && (op == SAVEDATA_OP_LIST_LOAD || op == SAVEDATA_OP_FIXED_LOAD)) prefix_list = {""};

        // The saves matching the prefixes: all are counted (dirNum), the first dirListMax are listed (dirListNum,
        // which counts directories without a valid PARAM.SFO too, as RPCS3's does).
        u32 dir_num = 0, dir_list_num = 0;
        for (const HostEntry& entry : host_dir(base_dir)) {
            if (!entry.is_directory || check_name_string(entry.name, 1, CELL_SAVEDATA_DIRNAME_SIZE, sdk) != 0) continue;
            for (const std::string& prefix : prefix_list) {
                if (!entry.name.starts_with(prefix)) continue;
                if (!dir_num++) std::memset(base + buf, 0, buf_size); // cleared once a save matches
                if (dir_list_num < dir_list_max) {
                    dir_list_num++;
                    const psf::Registry sfo = load_sfo(base_dir + entry.name + "/PARAM.SFO");
                    if (sfo.empty()) break;
                    SaveEntry e;
                    e.dir_name = psf::get_string(sfo, "SAVEDATA_DIRECTORY");
                    e.list_param = psf::get_string(sfo, "SAVEDATA_LIST_PARAM");
                    e.title = psf::get_string(sfo, "TITLE");
                    e.subtitle = psf::get_string(sfo, "SUB_TITLE");
                    e.details = psf::get_string(sfo, "DETAIL");
                    if (!is_valid_dir_name(e.dir_name, sdk)) {
                        PS_LOG_ERROR(kChan, "save data '{}' has an invalid SAVEDATA_DIRECTORY entry ('{}')", entry.name,
                            e.dir_name);
                        break;
                    }
                    for (const HostEntry& file : host_dir(base_dir + entry.name))
                        if (!file.is_directory && !check_filename(file.name, false, true, sdk)) e.size += file.size;
                    e.atime = entry.atime;
                    e.mtime = entry.mtime;
                    e.ctime = entry.ctime;
                    e.folder = entry.name;
                    save_entries.push_back(std::move(e));
                }
                break;
            }
        }
        ps_write32(base, c.listGet + list_get::dirNum, dir_num);
        ps_write32(base, c.listGet + list_get::dirListNum, dir_list_num);

        const u32 sort_type = ps_read32(base, a.set_list + set_list::sortType);
        const auto less = [sort_type](const SaveEntry& x, const SaveEntry& y) {
            if (sort_type == CELL_SAVEDATA_SORTTYPE_MODIFIEDTIME)
                return x.mtime == y.mtime ? x.subtitle < y.subtitle : x.mtime < y.mtime;
            return x.subtitle == y.subtitle ? x.mtime < y.mtime : x.subtitle < y.subtitle;
        };
        if (ps_read32(base, a.set_list + set_list::sortOrder) == CELL_SAVEDATA_SORTORDER_ASCENT)
            std::sort(save_entries.begin(), save_entries.end(), less);
        else
            std::sort(save_entries.rbegin(), save_entries.rend(), less);
        for (usize i = 0; i < save_entries.size(); i++) {
            const u32 at = buf + static_cast<u32>(i) * dir_list::kSize;
            put_string(base, at + dir_list::dirName, CELL_SAVEDATA_DIRNAME_SIZE, save_entries[i].dir_name);
            put_string(base, at + dir_list::listParam, CELL_SAVEDATA_SYSP_LPARAM_SIZE, save_entries[i].list_param);
        }
        PS_LOG_INFO(kChan, "  {} save(s) match \"{}\", {} listed", dir_num, prefixes, save_entries.size());

        s32 selected = -1;
        if (a.func_fixed) {
            const s32 res = call_callback(ctx, a.func_fixed, c.result, c.listGet, c.fixedSet);
            if (res != CELL_SAVEDATA_CBRESULT_OK_NEXT) {
                // OK_LAST skips the rest (NOCONFIRM is not allowed here).
                if (res == CELL_SAVEDATA_CBRESULT_OK_LAST) {
                    PS_LOG_INFO(kChan, "  funcFixed returned OK_LAST");
                    return lv2::CELL_OK;
                }
                return callback_error(t, c.result, a.err_dialog, "funcFixed");
            }
            const u32 fixed_dir = ps_read32(base, c.fixedSet + fixed_set::dirName);
            if (!fixed_dir) return param_error(26);
            const std::string dir_str = read_cstr(t, fixed_dir, CELL_SAVEDATA_DIRNAME_SIZE + 1).text;
            switch (check_name_string(dir_str, 1, CELL_SAVEDATA_DIRNAME_SIZE, sdk)) {
            case -1: return param_error(27);
            case -2: return param_error(28);
            default: break;
            }
            for (usize i = 0; i < save_entries.size(); i++) {
                if (save_entries[i].dir_name == dir_str) {
                    selected = static_cast<s32>(i);
                    break;
                }
            }
            const u32 option = ps_read32(base, c.fixedSet + fixed_set::option);
            PS_LOG_INFO(kChan, "  funcFixed chose \"{}\" ({}), option {}", dir_str, selected < 0 ? "new" : "listed",
                option);
            switch (option) {
            case CELL_SAVEDATA_OPTION_NONE:
                // The fixed variants ask the user to confirm (RPCS3 open_msg_dialog). Without a message dialog that
                // fails, and RPCS3 returns CELL_SAVEDATA_ERROR_INTERNAL.
                if (op == SAVEDATA_OP_FIXED_SAVE || op == SAVEDATA_OP_FIXED_LOAD || op == SAVEDATA_OP_FIXED_DELETE) {
                    PS_LOG_ERROR(kChan, "  the user must confirm \"{}\", but there is no message dialog", dir_str);
                    return error(CELL_SAVEDATA_ERROR_INTERNAL);
                }
                break;
            case CELL_SAVEDATA_OPTION_NOCONFIRM: break;
            default: return param_error(81, std::format("(option={:#x})", option));
            }
            if (selected == -1) save_entry.dir_name = dir_str;
            if (op == SAVEDATA_OP_FIXED_DELETE) {
                const SaveEntry* listed = selected >= 0 ? &save_entries[static_cast<usize>(selected)] : nullptr;
                return delete_save(ctx, a, c, base_dir, listed, dir_str);
            }
        }
        // Cleared again after funcFixed, for funcStat.
        if (dir_num) std::memset(base + buf, 0, buf_size);
        if (selected >= 0) save_entry.dir_name = save_entries[static_cast<usize>(selected)].dir_name;
    }

    if (a.dir_name) save_entry.dir_name = read_cstr(t, a.dir_name, CELL_SAVEDATA_DIRNAME_SIZE + 1).text;
    if (!is_valid_dir_name(save_entry.dir_name, sdk)) {
        PS_LOG_ERROR(kChan, "invalid save data directory name \"{}\"", save_entry.dir_name);
        return error(CELL_SAVEDATA_ERROR_BROKEN);
    }

    const std::string dir_path = base_dir + save_entry.dir_name;
    const std::string old_path = base_dir + ".backup_" + save_entry.dir_name;
    const std::string new_path = base_dir + ".working_" + save_entry.dir_name;
    psf::Registry sfo = load_sfo(dir_path + "/PARAM.SFO");
    bool has_modified = false, recreated = false;

    // RPCS3_BLIST: the files in the order the game first wrote them (a BSD file system's directory order on a PS3).
    std::vector<std::string> blist;
    if (const auto it = sfo.find("RPCS3_BLIST"); it != sfo.end()) blist = split(it->second.text, '/', false);

    // ---- funcStat ----
    if (!a.func_stat) return param_error(20);
    HostEntry dir_info;
    host_stat(dir_path, dir_info); // funcStat is called even if the directory doesn't exist
    save_entry.is_new = sfo.empty();
    ps_write32(base, c.statGet + stat_get::hddFreeSizeKB, static_cast<u32>(kHddFreeSizeKB));
    ps_write32(base, c.statGet + stat_get::isNewData, save_entry.is_new ? 1 : 0);
    ps_write64(base, c.statGet + stat_get::dir + dir_stat::atime, static_cast<u64>(dir_info.atime));
    ps_write64(base, c.statGet + stat_get::dir + dir_stat::mtime, static_cast<u64>(dir_info.mtime));
    ps_write64(base, c.statGet + stat_get::dir + dir_stat::ctime, static_cast<u64>(dir_info.ctime));
    put_string(base, c.statGet + stat_get::dir + dir_stat::dirName, CELL_SAVEDATA_DIRNAME_SIZE, save_entry.dir_name);
    if (!sfo.empty()) {
        const u32 p = c.statGet + stat_get::getParam;
        ps_write32(base, p + sys_param::parental_level, get_integer(sfo, "PARENTAL_LEVEL"));
        ps_write32(base, p + sys_param::attribute, get_integer(sfo, "ATTRIBUTE"));
        put_string(base, p + sys_param::title, CELL_SAVEDATA_SYSP_TITLE_SIZE, psf::get_string(sfo, "TITLE"));
        put_string(base, p + sys_param::subTitle, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE, psf::get_string(sfo, "SUB_TITLE"));
        put_string(base, p + sys_param::detail, CELL_SAVEDATA_SYSP_DETAIL_SIZE, psf::get_string(sfo, "DETAIL"));
        put_string(base, p + sys_param::listParam, CELL_SAVEDATA_SYSP_LPARAM_SIZE,
            psf::get_string(sfo, "SAVEDATA_LIST_PARAM"));
    }
    ps_write32(base, c.statGet + stat_get::bind, 0);
    ps_write32(base, c.statGet + stat_get::fileList, buf);
    if (!save_entry.is_new) std::memset(base + buf, 0, buf_size); // cleared whatever fileNum is

    // The files (not PARAM.SFO / PARAM.PFD), in RPCS3_BLIST's order, then by name.
    std::vector<HostEntry> files_sorted;
    for (HostEntry& entry : host_dir(dir_path)) {
        if (!entry.is_directory && !check_filename(entry.name, false, false, sdk))
            files_sorted.push_back(std::move(entry));
    }
    const auto blist_pos = [&](const std::string& name) {
        return static_cast<usize>(std::find(blist.begin(), blist.end(), name) - blist.begin());
    };
    std::sort(files_sorted.begin(), files_sorted.end(), [&](const HostEntry& x, const HostEntry& y) {
        const usize px = blist_pos(x.name), py = blist_pos(y.name);
        if (px == blist.size() && py == blist.size()) return x.name < y.name;
        return px < py;
    });
    const u32 file_list_max = ps_read32(base, a.set_buf + set_buf::fileListMax);
    u32 file_num = 0, file_list_num = 0;
    u64 size_bytes = 0;
    for (const HostEntry& entry : files_sorted) {
        file_num++;
        size_bytes += (entry.size + 1023) & ~u64{1023}; // the firmware rounds each file up
        if (file_list_num >= file_list_max) continue;
        const u32 at = buf + file_list_num++ * file_stat::kSize;
        u32 type = CELL_SAVEDATA_FILETYPE_NORMALFILE;
        if (entry.name == "ICON0.PNG") type = CELL_SAVEDATA_FILETYPE_CONTENT_ICON0;
        else if (entry.name == "ICON1.PAM") type = CELL_SAVEDATA_FILETYPE_CONTENT_ICON1;
        else if (entry.name == "PIC1.PNG") type = CELL_SAVEDATA_FILETYPE_CONTENT_PIC1;
        else if (entry.name == "SND0.AT3") type = CELL_SAVEDATA_FILETYPE_CONTENT_SND0;
        else if (get_integer(sfo, "*" + entry.name)) type = CELL_SAVEDATA_FILETYPE_SECUREFILE;
        ps_write32(base, at + file_stat::fileType, type);
        ps_write64(base, at + file_stat::size, entry.size);
        ps_write64(base, at + file_stat::atime, static_cast<u64>(entry.atime));
        ps_write64(base, at + file_stat::mtime, static_cast<u64>(entry.mtime));
        ps_write64(base, at + file_stat::ctime, static_cast<u64>(entry.ctime));
        put_string(base, at + file_stat::fileName, CELL_SAVEDATA_FILENAME_SIZE, entry.name);
    }
    ps_write32(base, c.statGet + stat_get::fileNum, file_num);
    ps_write32(base, c.statGet + stat_get::fileListNum, file_list_num);
    ps_write32(base, c.statGet + stat_get::sysSizeKB, static_cast<u32>(kSysSizeKB)); // always 35
    ps_write32(base, c.statGet + stat_get::sizeKB,
        save_entry.is_new ? 0 : static_cast<u32>(size_bytes / 1024 + kSysSizeKB));
    PS_LOG_INFO(kChan, "  funcStat: \"{}\" {}, {} file(s), {} KB", save_entry.dir_name,
        save_entry.is_new ? "is new" : "exists", file_num, size_bytes / 1024);

    {
        const s32 res = call_callback(ctx, a.func_stat, c.result, c.statGet, c.statSet);
        if (res != CELL_SAVEDATA_CBRESULT_OK_NEXT) {
            // OK_LAST skips the rest without an error (NOCONFIRM is not allowed here).
            if (res == CELL_SAVEDATA_CBRESULT_OK_LAST) {
                PS_LOG_INFO(kChan, "  funcStat returned OK_LAST");
                return lv2::CELL_OK;
            }
            return callback_error(t, c.result, a.err_dialog, "funcStat");
        }
    }

    const u32 set_param = ps_read32(base, c.statSet + stat_set::setParam);
    if (set_param) {
        if (!struct_ok(t, set_param, sys_param::kSize, "statSet->setParam")) return param_refused("statSet->setParam");
        const u32 attribute = ps_read32(base, set_param + sys_param::attribute);
        const u32 parental_level = ps_read32(base, set_param + sys_param::parental_level);
        if (attribute > CELL_SAVEDATA_ATTR_NODUPLICATE)
            return param_error(57, std::format("(attribute={:#x})", attribute));
        if (parental_level > 11) return param_error(58, std::format("(parental_level={})", parental_level));
        // listParam: [A-Z0-9_-], NUL-terminated within its 8 bytes for SDK 3.7 or newer.
        const usize lp_max = sdk > 0x36FFFF ? CELL_SAVEDATA_SYSP_LPARAM_SIZE - 1 : CELL_SAVEDATA_SYSP_LPARAM_SIZE;
        for (usize index = 0;; index++) {
            const u8 ch = ps_read8(base, set_param + sys_param::listParam + static_cast<u32>(index));
            if (ch == 0 || index >= lp_max) {
                if (ch) return param_error(76, "(listParam)");
                break;
            }
            if ((ch < 'A' || ch > 'Z') && (ch < '0' || ch > '9') && ch != '-' && ch != '_')
                return param_error(77, "(listParam)");
        }
        for (u32 i = 0; i < sys_param::kReservedSize; i++)
            if (ps_read8(base, set_param + sys_param::reserved + i)) return param_error(59);

        sfo["ACCOUNT_ID"] = array_entry(16, "0000000000000000");
        sfo["ATTRIBUTE"] = integer_entry(attribute);
        sfo["CATEGORY"] = psf::string(4, "SD");
        sfo["PARAMS"] = psf::string(1024, "");
        sfo["PARAMS2"] = psf::string(12, "");
        sfo["PARENTAL_LEVEL"] = integer_entry(parental_level);
        sfo["DETAIL"] = psf::string(CELL_SAVEDATA_SYSP_DETAIL_SIZE,
            read_chars(base, set_param + sys_param::detail, CELL_SAVEDATA_SYSP_DETAIL_SIZE));
        sfo["SAVEDATA_DIRECTORY"] = psf::string(CELL_SAVEDATA_DIRNAME_SIZE, save_entry.dir_name);
        sfo["SAVEDATA_LIST_PARAM"] = psf::string(CELL_SAVEDATA_SYSP_LPARAM_SIZE,
            read_chars(base, set_param + sys_param::listParam, CELL_SAVEDATA_SYSP_LPARAM_SIZE));
        sfo["SUB_TITLE"] = psf::string(CELL_SAVEDATA_SYSP_SUBTITLE_SIZE,
            read_chars(base, set_param + sys_param::subTitle, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE));
        sfo["TITLE"] = psf::string(CELL_SAVEDATA_SYSP_TITLE_SIZE,
            read_chars(base, set_param + sys_param::title, CELL_SAVEDATA_SYSP_TITLE_SIZE));
        has_modified = true;
    } else if (save_entry.is_new) {
        return param_error(50, "(new save data without setParam)");
    }

    const u32 recreate = ps_read32(base, c.statSet + stat_set::reCreateMode);
    switch (recreate & CELL_SAVEDATA_RECREATE_MASK) {
    case CELL_SAVEDATA_RECREATE_NO:
    case CELL_SAVEDATA_RECREATE_NO_NOBROKEN: break;
    case CELL_SAVEDATA_RECREATE_YES:
    case CELL_SAVEDATA_RECREATE_YES_RESET_OWNER:
        if (!set_param) return param_error(50, "(recreate without setParam)");
        PS_LOG_WARN(kChan, "  recreating \"{}\" (mode {})", save_entry.dir_name, recreate);
        // The secure file flags and the file order go with the old files.
        for (auto it = sfo.begin(); it != sfo.end();) {
            if (it->first.starts_with('*')) it = sfo.erase(it);
            else ++it;
        }
        blist.clear();
        recreated = true;
        has_modified = true;
        break;
    default: return param_error(48, std::format("(reCreateMode={:#x})", recreate));
    }

    // A new save's directory.
    if (!sfo.empty() && save_entry.is_new) {
        std::error_code ec;
        fs::create_directory(dir_path, ec);
        if (ec) {
            PS_LOG_WARN(kChan, "cannot create {} ({})", dir_path, ec.message());
            return error(CELL_SAVEDATA_ERROR_ACCESS_ERROR);
        }
    }

    // Every file (PARAM.SFO and PARAM.PFD too) into memory first, with its times; none when recreating.
    std::map<std::string, std::pair<s64, s64>> all_times;
    std::map<std::string, std::vector<u8>> all_files;
    if (!recreated) {
        for (const HostEntry& entry : host_dir(dir_path)) {
            if (entry.is_directory || check_filename(entry.name, false, true, sdk)) continue;
            std::optional<std::vector<u8>> data = read_host_file(dir_path + "/" + entry.name);
            if (!data) {
                PS_LOG_ERROR(kChan, "cannot read {}/{}", dir_path, entry.name);
                return error(CELL_SAVEDATA_ERROR_ACCESS_ERROR);
            }
            all_times.emplace(entry.name, std::make_pair(entry.atime, entry.mtime));
            all_files.emplace(entry.name, std::move(*data));
        }
    }

    ps_write32(base, c.fileGet + file_get::excSize, 0);
    if (ps_read32(base, c.statSet + stat_set::indicator))
        PS_LOG_DEBUG(kChan, "  the game asked for the auto save/load indicator (not shown: no overlay)");

    // ---- funcFile, until OK_LAST ----
    s32 savedata_result = lv2::CELL_OK;
    std::optional<std::chrono::steady_clock::time_point> delay_until;
    const auto add_to_blist = [&](const std::string& name) {
        if (std::find(blist.begin(), blist.end(), name) != blist.end()) return;
        if (const auto hole = std::find(blist.begin(), blist.end(), ""); hole != blist.end()) *hole = name;
        else blist.push_back(name);
    };
    const auto del_from_blist = [&](const std::string& name) {
        if (const auto it = std::find(blist.begin(), blist.end(), name); it != blist.end()) it->clear();
    };
    while (a.func_file) {
        std::memset(base + c.fileSet, 0, file_set::kSize);
        std::memset(base + c.fileGet + file_get::reserved, 0, file_get::kReservedSize);
        const s32 res = call_callback(ctx, a.func_file, c.result, c.fileGet, c.fileSet);
        if (res != CELL_SAVEDATA_CBRESULT_OK_NEXT) {
            if (res == CELL_SAVEDATA_CBRESULT_OK_LAST || res == CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM) {
                // Some games (RPCS3: Jak II) rely on the time a PS3 takes here.
                delay_until = std::chrono::steady_clock::now() +
                    std::chrono::microseconds(has_modified ? 500'000 : 100'000);
                break;
            }
            PS_LOG_WARN(kChan, "  funcFile returned {}", cbresult_name(res));
            if (res < CELL_SAVEDATA_CBRESULT_ERR_INVALID || res > CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM) {
                savedata_result = param_error(22, std::format("(funcFile returned {})", res));
                break;
            }
            savedata_result = error(CELL_SAVEDATA_ERROR_CBRESULT);
            break;
        }

        const u32 file_op = ps_read32(base, c.fileSet + file_set::fileOperation);
        const u32 type = ps_read32(base, c.fileSet + file_set::fileType);
        const u32 file_offset = ps_read32(base, c.fileSet + file_set::fileOffset);
        const u32 file_size = ps_read32(base, c.fileSet + file_set::fileSize);
        const u32 file_buf_size = ps_read32(base, c.fileSet + file_set::fileBufSize);
        const u32 file_buf = ps_read32(base, c.fileSet + file_set::fileBuf);

        std::string file_path;
        switch (type) {
        case CELL_SAVEDATA_FILETYPE_SECUREFILE:
        case CELL_SAVEDATA_FILETYPE_NORMALFILE: {
            const u32 name = ps_read32(base, c.fileSet + file_set::fileName);
            if (!name) {
                savedata_result = param_error(69);
                break;
            }
            const GuestString n = read_cstr(t, name, CELL_SAVEDATA_FILENAME_SIZE);
            if (!n.terminated) {
                savedata_result = param_error(71, std::format("(file name {})", describe(t, name, 32)));
                break;
            }
            file_path = n.text;
            if (const u32 e = check_filename(file_path, true, false, sdk)) {
                savedata_result = param_error(e, std::format("(file name \"{}\")", file_path));
                break;
            }
            if (type == CELL_SAVEDATA_FILETYPE_SECUREFILE) {
                std::string id;
                for (u32 i = 0; i < CELL_SAVEDATA_SECUREFILEID_SIZE; i++)
                    id += std::format("{:02x}", ps_read8(base, c.fileSet + file_set::secureFileId + i));
                PS_LOG_DEBUG(kChan, "  secure file {}: id {}", file_path, id);
            }
            break;
        }
        case CELL_SAVEDATA_FILETYPE_CONTENT_ICON0: file_path = "ICON0.PNG"; break;
        case CELL_SAVEDATA_FILETYPE_CONTENT_ICON1: file_path = "ICON1.PAM"; break;
        case CELL_SAVEDATA_FILETYPE_CONTENT_PIC1: file_path = "PIC1.PNG"; break;
        case CELL_SAVEDATA_FILETYPE_CONTENT_SND0: file_path = "SND0.AT3"; break;
        default: savedata_result = param_error(61, std::format("(fileType={:#x})", type)); break;
        }
        if (savedata_result) break;

        PS_LOG_INFO(kChan, "  funcFile: {} {} (type {}, offset {:#x}, size {:#x}, buffer {:#x} bytes at {:#010x})",
            file_op_name(file_op), file_path, type, file_offset, file_size, file_buf_size, file_buf);

        if ((file_path == "." || file_path == "..") && file_op <= CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC) {
            savedata_result = error(CELL_SAVEDATA_ERROR_BROKEN);
            break;
        }

        // READ, WRITE and WRITE_NOTRUNC check the buffer first.
        if (file_op == CELL_SAVEDATA_FILEOP_READ || file_op == CELL_SAVEDATA_FILEOP_WRITE ||
            file_op == CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC) {
            if (file_buf_size < file_size) {
                savedata_result = param_error(72);
                break;
            }
            if (!file_buf && file_buf_size) {
                savedata_result = param_error(73);
                break;
            }
        }

        switch (file_op) {
        case CELL_SAVEDATA_FILEOP_READ: {
            const auto file = all_files.find(file_path);
            if (file == all_files.end() || file->second.size() <= file_offset) {
                PS_LOG_ERROR(kChan, "  cannot read {}/{} (size {}, offset {:#x})", dir_path, file_path,
                    file == all_files.end() ? -1 : static_cast<s64>(file->second.size()), file_offset);
                savedata_result = error(CELL_SAVEDATA_ERROR_FAILURE);
                break;
            }
            const usize n = std::min<usize>(file_size, file->second.size() - file_offset);
            if (!struct_ok(t, file_buf, n, "fileSet->fileBuf")) {
                savedata_result = param_refused("fileSet->fileBuf");
                break;
            }
            std::memcpy(base + file_buf, file->second.data() + file_offset, n);
            ps_write32(base, c.fileGet + file_get::excSize, static_cast<u32>(n));
            break;
        }
        case CELL_SAVEDATA_FILEOP_WRITE:
        case CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC: {
            if (!struct_ok(t, file_buf, file_size, "fileSet->fileBuf")) {
                savedata_result = param_refused("fileSet->fileBuf");
                break;
            }
            // Written at the offset (a gap reads as zeros); WRITE then cuts the file at the end of what it wrote.
            std::vector<u8>& file = all_files[file_path];
            const usize end = usize{file_offset} + file_size;
            if (file.size() < end) file.resize(end);
            std::memcpy(file.data() + file_offset, base + file_buf, file_size);
            if (file_op == CELL_SAVEDATA_FILEOP_WRITE) file.resize(end);
            ps_write32(base, c.fileGet + file_get::excSize, file_size);
            all_times.erase(file_path);
            add_to_blist(file_path);
            has_modified = true;
            break;
        }
        case CELL_SAVEDATA_FILEOP_DELETE:
            if (all_files.erase(file_path) == 0) {
                PS_LOG_ERROR(kChan, "  cannot delete {}/{}: no such file", dir_path, file_path);
                savedata_result = error(CELL_SAVEDATA_ERROR_FAILURE);
                break;
            }
            sfo.erase("*" + file_path);
            ps_write32(base, c.fileGet + file_get::excSize, 0);
            all_times.erase(file_path);
            del_from_blist(file_path);
            has_modified = true;
            break;
        default: savedata_result = param_error(60, std::format("(fileOperation={:#x})", file_op)); break;
        }
        if (savedata_result) break;
        if (file_op != CELL_SAVEDATA_FILEOP_DELETE)
            sfo.try_emplace("*" + file_path, integer_entry(type == CELL_SAVEDATA_FILETYPE_SECUREFILE ? 1 : 0));
    }

    // Written whole when anything changed, also after a callback error (RPCS3).
    if (!sfo.empty() && has_modified) {
        const s32 r = commit_save(dir_path, new_path, old_path, sfo, blist, all_files, all_times);
        if (r != lv2::CELL_OK) return r;
        PS_LOG_INFO(kChan, "  wrote {} ({} file(s) and PARAM.SFO)", dir_path, all_files.size() - 1);
    }

    if (savedata_result == error(CELL_SAVEDATA_ERROR_CBRESULT))
        return callback_error(t, c.result, a.err_dialog, "funcFile");

    if (delay_until) {
        const auto now = std::chrono::steady_clock::now();
        if (now < *delay_until) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(*delay_until - now).count();
            lv2::sys_timer_usleep(t, static_cast<u64>(us));
        }
    }
    return savedata_result;
}

// ---- functions ----

// cellSaveDataAutoSave2 / cellSaveDataAutoLoad2: the save named dirName, no list, no dialog (errDialog chooses whether
// callback errors show one).
s32 auto_op(PPUContext& ctx, std::string_view fn, u32 operation, u32 version, u32 dir_name, u32 err_dialog,
    u32 set_buf, u32 func_stat, u32 func_file, u32 container, u32 userdata)
{
    lv2::PpuThread& t = thread_of(ctx);
    PS_LOG_INFO(kChan,
        "{}(version {}, dirName {}, errDialog {}, setBuf {:#x}, funcStat {:#x}, funcFile {:#x}, container {:#x}, "
        "userdata {:#x})",
        fn, version, describe(t, dir_name, CELL_SAVEDATA_DIRNAME_SIZE + 1), err_dialog, set_buf, func_stat, func_file,
        container, userdata);
    OpArgs a;
    a.operation = operation;
    a.version = version;
    a.dir_name = dir_name;
    a.err_dialog = err_dialog;
    a.set_buf = set_buf;
    a.func_stat = func_stat;
    a.func_file = func_file;
    a.unk_op_flags = 2;
    a.userdata = userdata;
    return finish(fn, savedata_op(ctx, a));
}

s32 cellSaveDataAutoSave2(PPUContext& ctx, u32 version, u32 dir_name, u32 err_dialog, u32 set_buf, u32 func_stat,
    u32 func_file, u32 container, u32 userdata)
{
    return auto_op(ctx, "cellSaveDataAutoSave2", SAVEDATA_OP_AUTO_SAVE, version, dir_name, err_dialog, set_buf,
        func_stat, func_file, container, userdata);
}

s32 cellSaveDataAutoLoad2(PPUContext& ctx, u32 version, u32 dir_name, u32 err_dialog, u32 set_buf, u32 func_stat,
    u32 func_file, u32 container, u32 userdata)
{
    return auto_op(ctx, "cellSaveDataAutoLoad2", SAVEDATA_OP_AUTO_LOAD, version, dir_name, err_dialog, set_buf,
        func_stat, func_file, container, userdata);
}

// The saves whose names start with setList's prefixes are listed for funcFixed, which picks one; no dialog.
s32 cellSaveDataListAutoLoad(PPUContext& ctx, u32 version, u32 err_dialog, u32 set_list, u32 set_buf, u32 func_fixed,
    u32 func_stat, u32 func_file, u32 container, u32 userdata)
{
    PS_LOG_INFO(kChan,
        "cellSaveDataListAutoLoad(version {}, errDialog {}, setList {:#x}, setBuf {:#x}, funcFixed {:#x}, funcStat "
        "{:#x}, funcFile {:#x}, container {:#x}, userdata {:#x})",
        version, err_dialog, set_list, set_buf, func_fixed, func_stat, func_file, container, userdata);
    OpArgs a;
    a.operation = SAVEDATA_OP_LIST_AUTO_LOAD;
    a.version = version;
    a.err_dialog = err_dialog;
    a.set_list = set_list;
    a.set_buf = set_buf;
    a.func_fixed = func_fixed;
    a.func_stat = func_stat;
    a.func_file = func_file;
    a.unk_op_flags = 2;
    a.userdata = userdata;
    return finish("cellSaveDataListAutoLoad", savedata_op(ctx, a));
}

// funcFixed names the save to delete; funcDone hears how it went. With CELL_SAVEDATA_OPTION_NONE the PS3 asks the
// user first: no dialog exists, so that is CELL_SAVEDATA_ERROR_INTERNAL (RPCS3 without a message dialog).
s32 cellSaveDataFixedDelete(PPUContext& ctx, u32 set_list, u32 set_buf, u32 func_fixed, u32 func_done, u32 container,
    u32 userdata)
{
    PS_LOG_INFO(kChan,
        "cellSaveDataFixedDelete(setList {:#x}, setBuf {:#x}, funcFixed {:#x}, funcDone {:#x}, container {:#x}, "
        "userdata {:#x})",
        set_list, set_buf, func_fixed, func_done, container, userdata);
    OpArgs a;
    a.operation = SAVEDATA_OP_FIXED_DELETE;
    a.err_dialog = CELL_SAVEDATA_ERRDIALOG_ALWAYS;
    a.set_list = set_list;
    a.set_buf = set_buf;
    a.func_fixed = func_fixed;
    a.unk_op_flags = 2;
    a.userdata = userdata;
    a.func_done = func_done;
    return finish("cellSaveDataFixedDelete", savedata_op(ctx, a));
}

// RPCS3 select_and_delete: the system's list of this game's saves, to delete them one by one until the user leaves
// it. There is no save data dialog, so the user leaves it at once: CELL_CANCEL, as RPCS3 without a save dialog.
s32 cellSaveDataDelete2(PPUContext& ctx, u32 container)
{
    PS_LOG_INFO(kChan, "cellSaveDataDelete2(container {:#x})", container);
    lv2::PpuThread& t = thread_of(ctx);
    SaveDataState& s = save_state();
    BusyGuard busy(s.busy);
    if (!busy.owned()) return finish("cellSaveDataDelete2", error(CELL_SAVEDATA_ERROR_BUSY));
    std::string title = s.process.config().game.title_id;
    if (title.empty()) title = t.kernel.config().title_id;
    usize count = 0;
    if (const std::string base_dir = savedata_dir(t.kernel); !base_dir.empty() && !title.empty()) {
        for (const HostEntry& e : host_dir(base_dir)) {
            if (e.is_directory && e.name.starts_with(title) &&
                !load_sfo(base_dir + e.name + "/PARAM.SFO").empty()) {
                count++;
            }
        }
    }
    PS_LOG_WARN(kChan, "cellSaveDataDelete2: the PS3 would list the {} save(s) of {} to delete; there is no save data "
                       "dialog, so the user cancels",
        count, title.empty() ? "this game" : title);
    return finish("cellSaveDataDelete2", lv2::CELL_CANCEL);
}

// RPCS3 savedata_get_list_item, for the current user.
s32 cellSaveDataGetListItem(PPUContext& ctx, u32 dir_name, u32 dir, u32 sys_file_param, u32 bind, u32 size_kb)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 sdk = t.kernel.config().sdk_version;
    constexpr std::string_view fn = "cellSaveDataGetListItem";
    PS_LOG_INFO(kChan, "{}(dirName {}, dir {:#x}, sysFileParam {:#x}, bind {:#x}, sizeKB {:#x})", fn,
        describe(t, dir_name, CELL_SAVEDATA_DIRNAME_SIZE + 1), dir, sys_file_param, bind, size_kb);
    if (!dir_name) return finish(fn, param_error(107));
    const GuestString name = read_cstr(t, dir_name, CELL_SAVEDATA_DIRNAME_SIZE + 1);
    if (name.faulted) return finish(fn, param_refused("dirName is outside committed guest memory"));
    switch (check_name_string(name.text, 1, CELL_SAVEDATA_DIRNAME_SIZE, sdk)) {
    case -1: return finish(fn, param_error(108));
    case -2: return finish(fn, param_error(109));
    default: break;
    }
    if ((dir && !struct_ok(t, dir, dir_stat::kSize, "dir")) ||
        (sys_file_param && !struct_ok(t, sys_file_param, sys_param::kSize, "sysFileParam")) ||
        (bind && !struct_ok(t, bind, 4, "bind")) || (size_kb && !struct_ok(t, size_kb, 4, "sizeKB"))) {
        return finish(fn, param_refused("an output"));
    }

    const std::string base_dir = savedata_dir(t.kernel);
    if (base_dir.empty() || !host_is_dir(base_dir)) return finish(fn, error(CELL_SAVEDATA_ERROR_NOUSER));
    const std::string save_path = base_dir + name.text, sfo_path = save_path + "/PARAM.SFO";
    std::error_code ec;
    if (!host_is_dir(save_path) && !fs::is_regular_file(sfo_path, ec)) {
        PS_LOG_INFO(kChan, "  no save data \"{}\"", name.text);
        return finish(fn, error(CELL_SAVEDATA_ERROR_NODATA));
    }
    const psf::Registry sfo = load_sfo(sfo_path);
    if (sys_file_param) {
        put_string(base, sys_file_param + sys_param::listParam, CELL_SAVEDATA_SYSP_LPARAM_SIZE,
            psf::get_string(sfo, "SAVEDATA_LIST_PARAM"));
        put_string(base, sys_file_param + sys_param::title, CELL_SAVEDATA_SYSP_TITLE_SIZE,
            psf::get_string(sfo, "TITLE"));
        put_string(base, sys_file_param + sys_param::subTitle, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE,
            psf::get_string(sfo, "SUB_TITLE"));
        put_string(base, sys_file_param + sys_param::detail, CELL_SAVEDATA_SYSP_DETAIL_SIZE,
            psf::get_string(sfo, "DETAIL"));
    }
    if (dir) {
        HostEntry info;
        if (!host_stat(save_path, info)) return finish(fn, error(CELL_SAVEDATA_ERROR_INTERNAL));
        put_string(base, dir + dir_stat::dirName, CELL_SAVEDATA_DIRNAME_SIZE, name.text);
        ps_write64(base, dir + dir_stat::atime, static_cast<u64>(info.atime));
        ps_write64(base, dir + dir_stat::ctime, static_cast<u64>(info.ctime));
        ps_write64(base, dir + dir_stat::mtime, static_cast<u64>(info.mtime));
    }
    if (size_kb) {
        u32 kbytes = 0;
        for (const HostEntry& e : host_dir(save_path)) {
            if (!e.is_directory && !check_filename(e.name, false, false, sdk))
                kbytes += static_cast<u32>((e.size + 1023) / 1024); // the firmware rounds each file up
        }
        ps_write32(base, size_kb, kbytes + kSysSizeKB); // PARAM.SFO and PARAM.PFD: a constant 35 KB
    }
    if (bind) ps_write32(base, bind, 0);
    return finish(fn, lv2::CELL_OK);
}

} // namespace

void register_cellSaveData(Registry& r)
{
    r.add("cellSysutil", {
        PS_HLE_FUNC(0xFBD5C856, cellSaveDataAutoLoad2),
        PS_HLE_FUNC(0x8B7ED64B, cellSaveDataAutoSave2),
        PS_HLE_FUNC(0x21425307, cellSaveDataListAutoLoad),
        PS_HLE_FUNC(0xEDADD797, cellSaveDataDelete2),
        PS_HLE_FUNC(0xCED17573, cellSaveDataFixedDelete),
    });
    r.add("cellSaveData", {
        PS_HLE_FUNC(0x04C06FC2, cellSaveDataGetListItem),
    });
}

} // namespace pspocket::hle
