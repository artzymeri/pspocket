// The media utilities that move files between a game and the PS3's media library: cellMusicExport, cellPhotoExport
// (the cellPhotoUtility module, with cellPhotoRegistFromFile), cellVideoExport, cellPhotoImport and cellPhotoDecode.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellMusicExport.cpp, cellPhotoExport.cpp, cellVideoExport.cpp,
// cellPhotoImport.cpp, cellPhotoDecode.cpp, and RPCS3's photo path (rpcs3/main_application.cpp get_photo_path).
//
// As RPCS3:
// - Initialize, Finalize and Progress answer through the game's finish callback at its next cellSysutilCheckCallback;
//   Progress reports the last export's progress (0 to 0xFFFF).
// - An export checks its path (/dev_hdd0, /dev_hdd1 or /dev_bdvd, letters, digits and "-_/." only, no "..", under
//   1055 bytes), then at the callback copies the file into the library's folder: /dev_hdd0/music/<file>,
//   /dev_hdd0/video/<file> (a "_N" suffix if taken), /dev_hdd0/photo/YYYY/MM/DD/<game title> DD-MM-YYYY HH-MM-SS<ext>
//   (a " N" suffix if taken). The plain exports move the file (music and video: unless it is on the disc; photos: only
//   from /dev_hdd0); the "WithCopy" forms keep it. A failed copy reports ERROR_MOVE. Metadata is ignored (RPCS3's TODO).
// - cellPhotoImport lets the user pick a photo in RPCS3's media list dialog, which is cancelled without RPCS3's native
//   UI; pspocket has none, so the import is cancelled at once (CELL_CANCEL, the file data cleared).
//
// pspocket has no media library (cellSearch.cpp, D-186): exported files land in the folders above, which nothing scans
// yet. cellPhotoDecodeFromFile makes RPCS3's checks but cannot decode: pspocket has no image decoder (RPCS3 uses Qt's;
// D-181's FFmpeg build has none), so it answers CELL_PHOTO_DECODE_ERROR_DECODE, logged.
//
// Difference from RPCS3: strings and structures are checked before use (D-042): memory that isn't guest memory is
// logged with the call site and refused with the library's PARAM error before anything changes (RPCS3 faults).

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "guest.hpp"
#include "psf.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil.hpp"

namespace pspocket::hle {
namespace {

namespace fs = std::filesystem;

constexpr s32 error(u32 code) { return static_cast<s32>(code); }
constexpr u32 kPathMax = 1055; // CELL_*_EXPORT_UTIL_HDD_PATH_MAX, CELL_PHOTO_IMPORT_HDD_PATH_MAX

// A call's log line with its result: info, a warning for an error.
s32 finish(std::string_view chan, s32 r, const std::string& call)
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, chan, "{}: {}", call,
        r < 0 ? std::format("{:#010x}", static_cast<u32>(r)) : std::to_string(r));
    return r;
}

// The game's finish callback(result, userdata) at its next cellSysutilCheckCallback.
void queue_finish(u32 func, s32 result, u32 user_data)
{
    sysutil_queue_call([=](PPUContext& ctx) -> s32 {
        call_guest(ctx, func, {static_cast<u64>(static_cast<s64>(result)), user_data});
        return lv2::CELL_OK;
    });
}

// A guest string for these calls: nullopt (the call site logged) when it runs out of guest memory first; the text up
// to the NUL, or the first kPathMax + 1 bytes (a path that long is refused anyway).
std::optional<std::string> guest_text(PPUContext& ctx, u32 addr, std::string_view what)
{
    const GuestString s = read_cstr(thread_of(ctx), addr, kPathMax + 1);
    if (!s.faulted) return s.text;
    log_call_site(ctx, guest_base(ctx), std::format("{} at {:#x} runs out of guest memory", what, addr));
    return std::nullopt;
}

// RPCS3 check_music_path, check_photo_path and check_movie_path (the same rule).
bool check_media_path(std::string_view path)
{
    if (path.size() >= kPathMax) return false;
    for (const char c : path) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '/' || c == '.')) {
            return false;
        }
    }
    if (!path.starts_with("/dev_hdd0") && !path.starts_with("/dev_bdvd") && !path.starts_with("/dev_hdd1")) {
        return false;
    }
    return path.find("..") == std::string_view::npos;
}

// `name` with `suffix` before its extension (RPCS3's "_N" and " N" suffixes).
std::string with_suffix(std::string_view name, std::string_view suffix)
{
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return std::string(name) + std::string(suffix);
    return std::string(name.substr(0, dot)) + std::string(suffix) + std::string(name.substr(dot));
}

// RPCS3 get_available_music_path / get_available_movie_path: `dir`/`file`, or with "_0", "_1"... while taken.
std::string available_path(const lv2::Kernel& kernel, std::string_view dir, std::string_view file)
{
    std::error_code ec;
    std::string vpath = std::string(dir) + std::string(file);
    for (u32 i = 0; fs::exists(kernel.vfs_host_path(vpath), ec); i++) {
        vpath = std::string(dir) + with_suffix(file, std::format("_{}", i));
    }
    return vpath;
}

// The game's title (PARAM.SFO TITLE), as RPCS3's Emu.GetTitle(); '/' can't be in a file name.
std::string game_title()
{
    const GameInfo& game = HleProcess::current()->config().game;
    std::string title;
    if (const auto sfo = psf::parse(game.param_sfo)) title = psf::get_string(*sfo, "TITLE");
    if (title.empty()) title = game.title_id;
    for (char& c : title) {
        if (c == '/') c = '_';
    }
    return title;
}

// RPCS3 get_available_photo_path / get_photo_path: /dev_hdd0/photo/YYYY/MM/DD/<title> DD-MM-YYYY HH-MM-SS<ext> in
// local time, the extension the file's (".png" if none), " 1", " 2"... while taken.
std::string available_photo_path(const lv2::Kernel& kernel, std::string_view file)
{
    const auto dot = file.find_last_of('.');
    const std::string ext = dot == std::string_view::npos ? std::string(".png") : std::string(file.substr(dot));
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm t{};
    localtime_r(&now, &t);
    const std::string base = std::format("/dev_hdd0/photo/{:04}/{:02}/{:02}/{} {:02}-{:02}-{:04} {:02}-{:02}-{:02}",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, game_title(), t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour,
        t.tm_min, t.tm_sec);
    std::error_code ec;
    std::string vpath = base + ext;
    for (u32 n = 1; fs::exists(kernel.vfs_host_path(vpath), ec); n++) vpath = std::format("{} {}{}", base, n, ext);
    return vpath;
}

enum class Library : u8 { Music, Photo, Video };

// The copy an export makes at its callback (RPCS3's sysutil_register_cb body): `src` (any mount, a disc image too) to
// the library's next free name, the source removed when `move` says so. False when the copy fails.
bool export_copy(lv2::Kernel& kernel, std::string_view chan, Library library, const std::string& src,
    const std::string& file, bool move)
{
    const std::string dst = library == Library::Music   ? available_path(kernel, "/dev_hdd0/music/", file)
                            : library == Library::Video ? available_path(kernel, "/dev_hdd0/video/", file)
                                                        : available_photo_path(kernel, file);
    const std::string host_dst = kernel.vfs_host_path(dst);
    std::vector<u8> data;
    std::error_code ec;
    bool ok = !host_dst.empty() && kernel.vfs_read_file(src, data);
    if (ok) {
        fs::create_directories(fs::path(host_dst).parent_path(), ec);
        std::ofstream out(host_dst, std::ios::binary);
        ok = out && out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!ok) {
        PS_LOG_ERROR(chan, "export: cannot copy {} to {}", src, dst);
        return false;
    }
    PS_LOG_INFO(chan, "export: {} copied to {}", src, dst);
    if (move) {
        if (const std::string host_src = kernel.vfs_host_path(src); host_src.empty() || !fs::remove(host_src, ec)) {
            PS_LOG_ERROR(chan, "export: cannot remove {}", src); // RPCS3 logs it and goes on
        }
    }
    return true;
}

// Each library's last export progress, 0 to 0xFFFF (RPCS3 music_export, photo_export, video_export).
struct ExportProgress {
    std::atomic<s32> music{0}, photo{0}, video{0};
};

std::atomic<s32>& progress_of(Library library)
{
    ExportProgress& p = HleProcess::current()->state<ExportProgress>();
    return library == Library::Music ? p.music : library == Library::Photo ? p.photo : p.video;
}

// An export's work at the callback: progress 0, the copy, progress 0xFFFF, the finish callback with CELL_OK or
// `move_error` (RPCS3 leaves the progress at 0 when the copy fails).
void queue_export(std::string_view chan, Library library, std::string src, std::string file, bool move,
    u32 move_error, u32 func, u32 user_data)
{
    sysutil_queue_call([=](PPUContext& ctx) -> s32 {
        std::atomic<s32>& progress = progress_of(library);
        progress = 0;
        const bool ok = export_copy(thread_of(ctx).kernel, chan, library, src, file, move);
        if (ok) progress = 0xFFFF;
        call_guest(ctx, func, {ok ? 0 : static_cast<u64>(static_cast<s64>(error(move_error))), user_data});
        return lv2::CELL_OK;
    });
}

// The FromFile exports of music, photos and videos: param and callback, the strings, the path, a file name; then the
// export is queued. `video` adds RPCS3's video checks (both strings non-empty, up front).
s32 export_from_file(PPUContext& ctx, std::string_view chan, std::string_view name, Library library, u32 param_error,
    u32 move_error, u32 src_dir, u32 src_file, u32 param, u32 func, u32 user_data, bool move)
{
    std::string call = std::format("{}(srcHddDir {:#x}, srcHddFile {:#x}, param {:#x}, funcFinish {:#x}, userdata "
                                   "{:#x})",
        name, src_dir, src_file, param, func, user_data);
    if (!param || !func || (library == Library::Video && (!src_dir || !src_file))) {
        return finish(chan, error(param_error), call);
    }
    if (!src_dir || !src_file) { // RPCS3 formats the null pointer's memory
        log_call_site(ctx, guest_base(ctx), std::format("{}: a null directory or file name", name));
        return finish(chan, error(param_error), call);
    }
    const std::optional<std::string> dir = guest_text(ctx, src_dir, std::format("{}: srcHddDir", name));
    const std::optional<std::string> file = guest_text(ctx, src_file, std::format("{}: srcHddFile", name));
    if (!dir || !file) return finish(chan, error(param_error), call);
    call = std::format("{}(\"{}\", \"{}\")", name, *dir, *file);
    if (library == Library::Video && (dir->empty() || file->empty())) return finish(chan, error(param_error), call);
    const std::string path = *dir + "/" + *file;
    if (!check_media_path(path) || file->empty()) return finish(chan, error(param_error), call);
    const bool remove = move && (library == Library::Photo ? path.starts_with("/dev_hdd0") : !path.starts_with("/dev_bdvd"));
    queue_export(chan, library, path, *file, remove, move_error, func, user_data);
    return finish(chan, lv2::CELL_OK, call);
}

// The Initialize functions: the version (0), then the callback, which hears CELL_OK (a container is not checked: an
// RPCS3 TODO).
s32 init_finish(std::string_view chan, u32 param_error, const std::string& call, u32 version, u32 func, u32 user_data)
{
    if (version != 0 || !func) return finish(chan, error(param_error), call);
    queue_finish(func, lv2::CELL_OK, user_data);
    return finish(chan, lv2::CELL_OK, call);
}

// Finalize: the callback hears CELL_OK.
s32 finalize_finish(std::string_view chan, u32 param_error, const std::string& call, u32 func, u32 user_data)
{
    if (!func) return finish(chan, error(param_error), call);
    queue_finish(func, lv2::CELL_OK, user_data);
    return finish(chan, lv2::CELL_OK, call);
}

// Progress: the callback hears the library's last export progress, read when it runs.
s32 progress_finish(std::string_view chan, u32 param_error, const std::string& call, Library library, u32 func,
    u32 user_data)
{
    if (!func) return finish(chan, error(param_error), call);
    sysutil_queue_call([=](PPUContext& ctx) -> s32 {
        call_guest(ctx, func, {static_cast<u64>(progress_of(library).load()), user_data});
        return lv2::CELL_OK;
    });
    return finish(chan, lv2::CELL_OK, call);
}

// ------------------------------------------------------------------------------------------- cellMusicExport

constexpr std::string_view kMusicExport = "cellMusicExport";
constexpr u32 CELL_MUSIC_EXPORT_UTIL_ERROR_PARAM = 0x8002C603, CELL_MUSIC_EXPORT_UTIL_ERROR_MOVE = 0x8002C609;

s32 cellMusicExportInitialize(PPUContext&, u32 version, u32 container, u32 func, u32 user_data)
{
    return init_finish(kMusicExport, CELL_MUSIC_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellMusicExportInitialize(version {}, container {:#x}, funcFinish {:#x}, userdata {:#x})",
            version, container, func, user_data),
        version, func, user_data);
}

s32 cellMusicExportInitialize2(PPUContext&, u32 version, u32 func, u32 user_data)
{
    return init_finish(kMusicExport, CELL_MUSIC_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellMusicExportInitialize2(version {}, funcFinish {:#x}, userdata {:#x})", version, func,
            user_data),
        version, func, user_data);
}

s32 cellMusicExportFinalize(PPUContext&, u32 func, u32 user_data)
{
    return finalize_finish(kMusicExport, CELL_MUSIC_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellMusicExportFinalize(funcFinish {:#x}, userdata {:#x})", func, user_data), func, user_data);
}

// RPCS3 cellMusicExportFromFile(srcHddDir, srcHddFile, CellMusicExportSetParam* param, funcFinish, userdata).
s32 cellMusicExportFromFile(PPUContext& ctx, u32 src_dir, u32 src_file, u32 param, u32 func, u32 user_data)
{
    return export_from_file(ctx, kMusicExport, "cellMusicExportFromFile", Library::Music,
        CELL_MUSIC_EXPORT_UTIL_ERROR_PARAM, CELL_MUSIC_EXPORT_UTIL_ERROR_MOVE, src_dir, src_file, param, func,
        user_data, true);
}

s32 cellMusicExportProgress(PPUContext&, u32 func, u32 user_data)
{
    return progress_finish(kMusicExport, CELL_MUSIC_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellMusicExportProgress(funcFinish {:#x}, userdata {:#x})", func, user_data), Library::Music,
        func, user_data);
}

// ------------------------------------------------------------------------------------- cellPhotoExport (cellPhotoUtility)

constexpr std::string_view kPhotoExport = "cellPhotoExport";
constexpr u32 CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM = 0x8002C203, CELL_PHOTO_EXPORT_UTIL_ERROR_MOVE = 0x8002C209;

s32 cellPhotoInitialize(PPUContext&, s32 version, u32 container, u32 func, u32 user_data)
{
    return init_finish(kPhotoExport, CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellPhotoInitialize(version {}, container {:#x}, funcFinish {:#x}, userdata {:#x})", version,
            container, func, user_data),
        static_cast<u32>(version), func, user_data);
}

s32 cellPhotoFinalize(PPUContext&, u32 func, u32 user_data)
{
    return finalize_finish(kPhotoExport, CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellPhotoFinalize(funcFinish {:#x}, userdata {:#x})", func, user_data), func, user_data);
}

// RPCS3 cellPhotoRegistFromFile(path, photo_title, game_title, game_comment, funcFinish, userdata): the file's own
// path; its name is what follows the last '/'. Moved when it is on /dev_hdd0.
s32 cellPhotoRegistFromFile(PPUContext& ctx, u32 path, u32 photo_title, u32 title, u32 comment, u32 func, u32 user_data)
{
    std::string call = std::format("cellPhotoRegistFromFile(path {:#x}, photo_title {:#x}, game_title {:#x}, "
                                   "game_comment {:#x}, funcFinish {:#x}, userdata {:#x})",
        path, photo_title, title, comment, func, user_data);
    if (!path || !func) return finish(kPhotoExport, error(CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM), call);
    const std::optional<std::string> text = guest_text(ctx, path, "cellPhotoRegistFromFile: path");
    if (!text) return finish(kPhotoExport, error(CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM), call);
    call = std::format("cellPhotoRegistFromFile(\"{}\")", *text);
    const std::string file = text->substr(text->find_last_of('/') + 1);
    if (!check_media_path(*text) || file.empty()) {
        return finish(kPhotoExport, error(CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM), call);
    }
    queue_export(kPhotoExport, Library::Photo, *text, file, text->starts_with("/dev_hdd0"),
        CELL_PHOTO_EXPORT_UTIL_ERROR_MOVE, func, user_data);
    return finish(kPhotoExport, lv2::CELL_OK, call);
}

s32 cellPhotoExportInitialize(PPUContext&, u32 version, u32 container, u32 func, u32 user_data)
{
    return init_finish(kPhotoExport, CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellPhotoExportInitialize(version {}, container {:#x}, funcFinish {:#x}, userdata {:#x})",
            version, container, func, user_data),
        version, func, user_data);
}

s32 cellPhotoExportInitialize2(PPUContext&, u32 version, u32 func, u32 user_data)
{
    return init_finish(kPhotoExport, CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellPhotoExportInitialize2(version {}, funcFinish {:#x}, userdata {:#x})", version, func,
            user_data),
        version, func, user_data);
}

s32 cellPhotoExportFinalize(PPUContext&, u32 func, u32 user_data)
{
    return finalize_finish(kPhotoExport, CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellPhotoExportFinalize(funcFinish {:#x}, userdata {:#x})", func, user_data), func, user_data);
}

// RPCS3 cellPhotoExportFromFile(srcHddDir, srcHddFile, CellPhotoExportSetParam* param, funcFinish, userdata).
s32 cellPhotoExportFromFile(PPUContext& ctx, u32 src_dir, u32 src_file, u32 param, u32 func, u32 user_data)
{
    return export_from_file(ctx, kPhotoExport, "cellPhotoExportFromFile", Library::Photo,
        CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM, CELL_PHOTO_EXPORT_UTIL_ERROR_MOVE, src_dir, src_file, param, func,
        user_data, true);
}

s32 cellPhotoExportFromFileWithCopy(PPUContext& ctx, u32 src_dir, u32 src_file, u32 param, u32 func, u32 user_data)
{
    return export_from_file(ctx, kPhotoExport, "cellPhotoExportFromFileWithCopy", Library::Photo,
        CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM, CELL_PHOTO_EXPORT_UTIL_ERROR_MOVE, src_dir, src_file, param, func,
        user_data, false);
}

s32 cellPhotoExportProgress(PPUContext&, u32 func, u32 user_data)
{
    return progress_finish(kPhotoExport, CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellPhotoExportProgress(funcFinish {:#x}, userdata {:#x})", func, user_data), Library::Photo,
        func, user_data);
}

// ------------------------------------------------------------------------------------------- cellVideoExport

constexpr std::string_view kVideoExport = "cellVideoExport";
constexpr u32 CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM = 0x8002CA03, CELL_VIDEO_EXPORT_UTIL_ERROR_MOVE = 0x8002CA09;

s32 cellVideoExportInitialize(PPUContext&, u32 version, u32 container, u32 func, u32 user_data)
{
    return init_finish(kVideoExport, CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellVideoExportInitialize(version {}, container {:#x}, funcFinish {:#x}, userdata {:#x})",
            version, container, func, user_data),
        version, func, user_data);
}

s32 cellVideoExportInitialize2(PPUContext&, u32 version, u32 func, u32 user_data)
{
    return init_finish(kVideoExport, CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellVideoExportInitialize2(version {}, funcFinish {:#x}, userdata {:#x})", version, func,
            user_data),
        version, func, user_data);
}

s32 cellVideoExportFinalize(PPUContext&, u32 func, u32 user_data)
{
    return finalize_finish(kVideoExport, CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellVideoExportFinalize(funcFinish {:#x}, userdata {:#x})", func, user_data), func, user_data);
}

// RPCS3 cellVideoExportFromFile(srcHddDir, srcHddFile, CellVideoExportSetParam* param, funcFinish, userdata).
s32 cellVideoExportFromFile(PPUContext& ctx, u32 src_dir, u32 src_file, u32 param, u32 func, u32 user_data)
{
    return export_from_file(ctx, kVideoExport, "cellVideoExportFromFile", Library::Video,
        CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM, CELL_VIDEO_EXPORT_UTIL_ERROR_MOVE, src_dir, src_file, param, func,
        user_data, true);
}

s32 cellVideoExportFromFileWithCopy(PPUContext& ctx, u32 src_dir, u32 src_file, u32 param, u32 func, u32 user_data)
{
    return export_from_file(ctx, kVideoExport, "cellVideoExportFromFileWithCopy", Library::Video,
        CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM, CELL_VIDEO_EXPORT_UTIL_ERROR_MOVE, src_dir, src_file, param, func,
        user_data, false);
}

s32 cellVideoExportProgress(PPUContext&, u32 func, u32 user_data)
{
    return progress_finish(kVideoExport, CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM,
        std::format("cellVideoExportProgress(funcFinish {:#x}, userdata {:#x})", func, user_data), Library::Video,
        func, user_data);
}

// ------------------------------------------------------------------------------------------- cellPhotoImport

constexpr std::string_view kPhotoImport = "cellPhotoImport";
constexpr u32 CELL_PHOTO_IMPORT_ERROR_BUSY = 0x8002C701, CELL_PHOTO_IMPORT_ERROR_PARAM = 0x8002C703,
              CELL_PHOTO_IMPORT_ERROR_ACCESS_ERROR = 0x8002C704;
constexpr u32 CELL_CANCEL = 1;
// CellPhotoImportSetParam {be u32 fileSizeMax, two reserved pointers}; CellPhotoImportFileData: dstFileName[255],
// photo_title[192], game_title[128], game_comment[1024], a padding byte, then the data_sub and reserved pointers (1608
// bytes); CellPhotoImportFileDataSub (16 bytes).
constexpr u32 kSetParamSize = 12, kFileDataSize = 1608, kFileDataSubSize = 16;

// RPCS3 photo_import; the file data are cellPhotoImportUtil's variables under RPCS3's names for them, g_filedata and
// g_filedata_sub (D-567).
struct PhotoImportState {
    std::mutex mutex;
    bool busy = false;
};

// RPCS3 cellPhotoImport(2) and select_photo: the arguments, the busy flag, then the destination (on /dev_hdd0 or
// /dev_hdd1, and a directory); then the dialog, cancelled: the finish callback(result, filedata, userdata) hears
// CELL_CANCEL with the file data cleared.
s32 photo_import(PPUContext& ctx, const std::string& call, u32 version, u32 dst, u32 param, u32 func, u32 user_data)
{
    if (version != 0 || !func || !param || !dst) return finish(kPhotoImport, error(CELL_PHOTO_IMPORT_ERROR_PARAM), call);
    if (!guest_readable(thread_of(ctx), param, kSetParamSize)) {
        log_call_site(ctx, guest_base(ctx), std::format("{}: param {:#x} is not guest memory", call, param));
        return finish(kPhotoImport, error(CELL_PHOTO_IMPORT_ERROR_PARAM), call);
    }
    const std::optional<std::string> dir = guest_text(ctx, dst, "cellPhotoImport: dstHddPath");
    if (!dir) return finish(kPhotoImport, error(CELL_PHOTO_IMPORT_ERROR_PARAM), call);
    HleProcess* process = HleProcess::current();
    PhotoImportState& s = process->state<PhotoImportState>();
    std::lock_guard lock(s.mutex);
    if (s.busy) return finish(kPhotoImport, error(CELL_PHOTO_IMPORT_ERROR_BUSY), call);
    std::error_code ec;
    const std::string host_dir = thread_of(ctx).kernel.vfs_host_path(*dir);
    if ((!dir->starts_with("/dev_hdd0") && !dir->starts_with("/dev_hdd1")) || host_dir.empty() ||
        !fs::is_directory(host_dir, ec)) {
        return finish(kPhotoImport, error(CELL_PHOTO_IMPORT_ERROR_ACCESS_ERROR), call + std::format(": \"{}\"", *dir));
    }
    const u32 file_data = process->link_variable("cellPhotoImportUtil", recomp::nid_of("g_filedata"));
    const u32 file_data_sub = process->link_variable("cellPhotoImportUtil", recomp::nid_of("g_filedata_sub"));
    if (!file_data || !file_data_sub) PS_FATAL(kPhotoImport, "cellPhotoImport: no main memory for the file data");
    s.busy = true;
    sysutil_queue_call([func, user_data, file_data, file_data_sub](PPUContext& cb) -> s32 {
        std::memset(guest_base(cb) + file_data, 0, kFileDataSize);
        std::memset(guest_base(cb) + file_data_sub, 0, kFileDataSubSize);
        PhotoImportState& st = HleProcess::current()->state<PhotoImportState>();
        {
            std::lock_guard l(st.mutex);
            st.busy = false;
        }
        call_guest(cb, func, {CELL_CANCEL, file_data, user_data});
        return lv2::CELL_OK;
    });
    return finish(kPhotoImport, lv2::CELL_OK, call + std::format(" to \"{}\": no media dialog, cancelled", *dir));
}

s32 cellPhotoImport(PPUContext& ctx, u32 version, u32 dst, u32 param, u32 container, u32 func, u32 user_data)
{
    return photo_import(ctx,
        std::format("cellPhotoImport(version {}, dstHddPath {:#x}, param {:#x}, container {:#x}, funcFinish {:#x}, "
                    "userdata {:#x})",
            version, dst, param, container, func, user_data),
        version, dst, param, func, user_data);
}

s32 cellPhotoImport2(PPUContext& ctx, u32 version, u32 dst, u32 param, u32 func, u32 user_data)
{
    return photo_import(ctx,
        std::format("cellPhotoImport2(version {}, dstHddPath {:#x}, param {:#x}, funcFinish {:#x}, userdata {:#x})",
            version, dst, param, func, user_data),
        version, dst, param, func, user_data);
}

// ------------------------------------------------------------------------------------------- cellPhotoDecode

constexpr std::string_view kPhotoDecode = "cellPhotoDecode";
constexpr u32 CELL_PHOTO_DECODE_ERROR_PARAM = 0x8002C903, CELL_PHOTO_DECODE_ERROR_ACCESS_ERROR = 0x8002C904,
              CELL_PHOTO_DECODE_ERROR_DECODE = 0x8002C906;
// CellPhotoDecodeSetParam {be u32 dstBuffer, be u16 width, be u16 height, two reserved pointers};
// CellPhotoDecodeReturnParam {be u16 width, be u16 height, two reserved pointers}.
constexpr u32 kDecodeSetParamSize = 16, kDecodeReturnParamSize = 12;

s32 cellPhotoDecodeInitialize(PPUContext&, u32 version, u32 container1, u32 container2, u32 func, u32 user_data)
{
    return init_finish(kPhotoDecode, CELL_PHOTO_DECODE_ERROR_PARAM,
        std::format("cellPhotoDecodeInitialize(version {}, container1 {:#x}, container2 {:#x}, funcFinish {:#x}, "
                    "userdata {:#x})",
            version, container1, container2, func, user_data),
        version, func, user_data);
}

s32 cellPhotoDecodeInitialize2(PPUContext&, u32 version, u32 container2, u32 func, u32 user_data)
{
    return init_finish(kPhotoDecode, CELL_PHOTO_DECODE_ERROR_PARAM,
        std::format("cellPhotoDecodeInitialize2(version {}, container2 {:#x}, funcFinish {:#x}, userdata {:#x})",
            version, container2, func, user_data),
        version, func, user_data);
}

s32 cellPhotoDecodeFinalize(PPUContext&, u32 func, u32 user_data)
{
    return finalize_finish(kPhotoDecode, CELL_PHOTO_DECODE_ERROR_PARAM,
        std::format("cellPhotoDecodeFinalize(funcFinish {:#x}, userdata {:#x})", func, user_data), func, user_data);
}

// RPCS3 cellPhotoDecodeFromFile(srcHddDir, srcHddFile, set_param, return_param): the return parameters cleared, the
// file (on /dev_hdd0, /dev_hdd1 or /dev_bdvd, and a file), then RPCS3 decodes it into dstBuffer, scaled down to fit
// width x height, as RGBA. pspocket has no image decoder: CELL_PHOTO_DECODE_ERROR_DECODE.
s32 cellPhotoDecodeFromFile(PPUContext& ctx, u32 src_dir, u32 src_file, u32 set_param, u32 return_param)
{
    std::string call = std::format("cellPhotoDecodeFromFile(srcHddDir {:#x}, srcHddFile {:#x}, set_param {:#x}, "
                                   "return_param {:#x})",
        src_dir, src_file, set_param, return_param);
    if (!src_dir || !src_file || !set_param || !return_param) {
        return finish(kPhotoDecode, error(CELL_PHOTO_DECODE_ERROR_PARAM), call);
    }
    lv2::PpuThread& t = thread_of(ctx);
    if (!guest_readable(t, set_param, kDecodeSetParamSize) || !guest_readable(t, return_param, kDecodeReturnParamSize)) {
        log_call_site(ctx, guest_base(ctx), call + ": a parameter block is not guest memory");
        return finish(kPhotoDecode, error(CELL_PHOTO_DECODE_ERROR_PARAM), call);
    }
    const std::optional<std::string> dir = guest_text(ctx, src_dir, "cellPhotoDecodeFromFile: srcHddDir");
    const std::optional<std::string> file = guest_text(ctx, src_file, "cellPhotoDecodeFromFile: srcHddFile");
    if (!dir || !file) return finish(kPhotoDecode, error(CELL_PHOTO_DECODE_ERROR_PARAM), call);
    std::memset(guest_base(ctx) + return_param, 0, kDecodeReturnParamSize);
    const std::string path = *dir + "/" + *file;
    call = std::format("cellPhotoDecodeFromFile(\"{}\", {}x{})", path, ps_read16(guest_base(ctx), set_param + 4),
        ps_read16(guest_base(ctx), set_param + 6));
    if (!path.starts_with("/dev_hdd0") && !path.starts_with("/dev_hdd1") && !path.starts_with("/dev_bdvd")) {
        return finish(kPhotoDecode, error(CELL_PHOTO_DECODE_ERROR_ACCESS_ERROR), call);
    }
    std::vector<u8> data;
    if (!t.kernel.vfs_read_file(path, data)) {
        return finish(kPhotoDecode, error(CELL_PHOTO_DECODE_ERROR_ACCESS_ERROR), call + ": not a file");
    }
    return finish(kPhotoDecode, error(CELL_PHOTO_DECODE_ERROR_DECODE), call + ": pspocket has no image decoder");
}

} // namespace

void register_media_export(Registry& r)
{
    r.add("cellMusicExportUtility", {
        PS_HLE_FUNC(0xB4C9B4F9, cellMusicExportInitialize),
        PS_HLE_FUNC(0xE0443A44, cellMusicExportInitialize2),
        PS_HLE_FUNC(0xE90EFFEA, cellMusicExportFinalize),
        PS_HLE_FUNC(0xB202F0E8, cellMusicExportFromFile),
        PS_HLE_FUNC(0x92B50EBC, cellMusicExportProgress),
    });
    r.add("cellPhotoUtility", {
        PS_HLE_FUNC(0x55C70783, cellPhotoInitialize),
        PS_HLE_FUNC(0x3F7FC0AF, cellPhotoFinalize),
        PS_HLE_FUNC(0x42A32983, cellPhotoRegistFromFile),
        PS_HLE_FUNC(0x4357C77F, cellPhotoExportInitialize),
        PS_HLE_FUNC(0x08CBD8E1, cellPhotoExportInitialize2),
        PS_HLE_FUNC(0xED4A0148, cellPhotoExportFinalize),
        PS_HLE_FUNC(0x09CE84AC, cellPhotoExportFromFile),
        PS_HLE_FUNC(0x8D14D09B, cellPhotoExportFromFileWithCopy),
        PS_HLE_FUNC(0xDE509EAD, cellPhotoExportProgress),
    });
    r.add("cellVideoExportUtility", {
        PS_HLE_FUNC(0x1BB79FF4, cellVideoExportProgress),
        PS_HLE_FUNC(0x2F457571, cellVideoExportInitialize2),
        PS_HLE_FUNC(0x6A24CC70, cellVideoExportInitialize),
        PS_HLE_FUNC(0x78946D1F, cellVideoExportFromFileWithCopy),
        PS_HLE_FUNC(0x81296524, cellVideoExportFromFile),
        PS_HLE_FUNC(0xC15BE817, cellVideoExportFinalize),
    });
    r.add_variables("cellPhotoImportUtil",
        {Variable{{}, "g_filedata", recomp::nid_of("g_filedata"), kFileDataSize, 8, nullptr},
            Variable{{}, "g_filedata_sub", recomp::nid_of("g_filedata_sub"), kFileDataSubSize, 8, nullptr}});
    r.add("cellPhotoImportUtil", {
        PS_HLE_FUNC(0x0783BCE0, cellPhotoImport),
        PS_HLE_FUNC(0x1AB8DF55, cellPhotoImport2),
    });
    r.add("cellPhotoDecodeUtil", {
        PS_HLE_FUNC(0x596F0A56, cellPhotoDecodeInitialize),
        PS_HLE_FUNC(0x0F424ECB, cellPhotoDecodeInitialize2),
        PS_HLE_FUNC(0xAD7D8F38, cellPhotoDecodeFinalize),
        PS_HLE_FUNC(0x28B22E44, cellPhotoDecodeFromFile),
    });
}

} // namespace pspocket::hle
