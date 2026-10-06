// cellSearch: the PS3's media library, searched for lists and contents of music, photos and videos, and for a video's
// scenes. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellSearch.cpp/.h (search_info, search_object_t, content_id_map).
//
// pspocket has no media library: nothing puts music, photos or videos on its /dev_hdd0 (RPCS3 scans /dev_hdd0/music,
// photo and video, probing each file with FFmpeg; pspocket finds them empty, D-186). Every call makes RPCS3's checks
// in RPCS3's order and goes through RPCS3's states: cellSearchInitialize and Finalize, and each search, finish at the
// game's next cellSysutilCheckCallback, where its callback hears the result: no content found (resultNum 0). Every
// lookup by offset or content ID finds nothing (CELL_SEARCH_ERROR_OUT_OF_RANGE, CONTENT_NOT_FOUND). Search IDs are
// handed out as RPCS3's idm does (1 on, 1024 of them) and stay until cellSearchEnd.
//
// Differences from RPCS3:
// - A search refused after the library was marked busy (an unknown list or video, a sort RPCS3 doesn't support) leaves
//   it idle again. RPCS3 leaves it busy, so that every later call would answer CELL_SEARCH_ERROR_BUSY.
// - With all 1024 search IDs in use, a search is refused with CELL_SEARCH_ERROR_NO_MEMORY (RPCS3 crashes).
// - Pointers are checked before use (D-042): memory that isn't guest memory is logged with the call site and refused
//   with CELL_SEARCH_ERROR_PARAM before anything is written (RPCS3 faults).

#include <cstring>
#include <format>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "guest.hpp"
#include "idm.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "sysutil.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellSearch";

enum : u32 {
    CELL_SEARCH_ERROR_PARAM = 0x8002C801,
    CELL_SEARCH_ERROR_BUSY = 0x8002C802,
    CELL_SEARCH_ERROR_NO_MEMORY = 0x8002C803,
    CELL_SEARCH_ERROR_UNKNOWN_MODE = 0x8002C804,
    CELL_SEARCH_ERROR_ALREADY_INITIALIZED = 0x8002C805,
    CELL_SEARCH_ERROR_NOT_INITIALIZED = 0x8002C806,
    CELL_SEARCH_ERROR_FINALIZING = 0x8002C807,
    CELL_SEARCH_ERROR_NOT_SUPPORTED_SEARCH = 0x8002C808,
    CELL_SEARCH_ERROR_CONTENT_NOT_FOUND = 0x8002C80A,
    CELL_SEARCH_ERROR_OUT_OF_RANGE = 0x8002C80C,
    CELL_SEARCH_ERROR_INVALID_SEARCHID = 0x8002C80D,
    CELL_SEARCH_ERROR_ALREADY_GOT_RESULT = 0x8002C80E,
    CELL_SEARCH_ERROR_TAG = 0x8002C812,
    CELL_SEARCH_ERROR_GENERIC = 0x8002C8FF,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case CELL_SEARCH_ERROR_PARAM: return "CELL_SEARCH_ERROR_PARAM";
    case CELL_SEARCH_ERROR_BUSY: return "CELL_SEARCH_ERROR_BUSY";
    case CELL_SEARCH_ERROR_NO_MEMORY: return "CELL_SEARCH_ERROR_NO_MEMORY";
    case CELL_SEARCH_ERROR_UNKNOWN_MODE: return "CELL_SEARCH_ERROR_UNKNOWN_MODE";
    case CELL_SEARCH_ERROR_ALREADY_INITIALIZED: return "CELL_SEARCH_ERROR_ALREADY_INITIALIZED";
    case CELL_SEARCH_ERROR_NOT_INITIALIZED: return "CELL_SEARCH_ERROR_NOT_INITIALIZED";
    case CELL_SEARCH_ERROR_FINALIZING: return "CELL_SEARCH_ERROR_FINALIZING";
    case CELL_SEARCH_ERROR_NOT_SUPPORTED_SEARCH: return "CELL_SEARCH_ERROR_NOT_SUPPORTED_SEARCH";
    case CELL_SEARCH_ERROR_CONTENT_NOT_FOUND: return "CELL_SEARCH_ERROR_CONTENT_NOT_FOUND";
    case CELL_SEARCH_ERROR_OUT_OF_RANGE: return "CELL_SEARCH_ERROR_OUT_OF_RANGE";
    case CELL_SEARCH_ERROR_INVALID_SEARCHID: return "CELL_SEARCH_ERROR_INVALID_SEARCHID";
    case CELL_SEARCH_ERROR_ALREADY_GOT_RESULT: return "CELL_SEARCH_ERROR_ALREADY_GOT_RESULT";
    case CELL_SEARCH_ERROR_TAG: return "CELL_SEARCH_ERROR_TAG";
    case CELL_SEARCH_ERROR_GENERIC: return "CELL_SEARCH_ERROR_GENERIC";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

// A call's log line with its result: info, a warning for an error.
s32 finish(s32 r, const std::string& call)
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, kChan, "{}: {}", call, result_name(r));
    return r;
}

constexpr s32 CELL_SEARCH_MODE_NORMAL = 0;
// CellSearchEvent (CONTENTSEARCH_INLIST_RESULT 4 and SCENESEARCH_INVIDEO_RESULT 6 need content found first).
constexpr s32 CELL_SEARCH_EVENT_INITIALIZE_RESULT = 1, CELL_SEARCH_EVENT_FINALIZE_RESULT = 2,
              CELL_SEARCH_EVENT_LISTSEARCH_RESULT = 3, CELL_SEARCH_EVENT_CONTENTSEARCH_RESULT = 5,
              CELL_SEARCH_EVENT_SCENESEARCH_RESULT = 7;
// CellSearchSortKey (NONE 0, DEFAULT 1 .. MODIFIEDDATE 10) and CellSearchSortOrder.
constexpr s32 CELL_SEARCH_SORTKEY_DEFAULT = 1, CELL_SEARCH_SORTKEY_TITLE = 2, CELL_SEARCH_SORTKEY_ALBUMTITLE = 3,
              CELL_SEARCH_SORTKEY_GENRENAME = 4, CELL_SEARCH_SORTKEY_ARTISTNAME = 5,
              CELL_SEARCH_SORTKEY_IMPORTEDDATE = 6, CELL_SEARCH_SORTKEY_TAKENDATE = 8,
              CELL_SEARCH_SORTKEY_MODIFIEDDATE = 10;
constexpr s32 CELL_SEARCH_SORTORDER_ASCENDING = 1, CELL_SEARCH_SORTORDER_DESCENDING = 2;
// CellSearchListSearchType MUSIC_ALBUM 1 .. MUSIC_PLAYLIST 9; CellSearchContentSearchType MUSIC_ALL 1, PHOTO_ALL 2,
// VIDEO_ALL 3; CellSearchSceneSearchType CHAPTER 1 .. ALL 5.
constexpr s32 CELL_SEARCH_CONTENTSEARCHTYPE_MUSIC_ALL = 1, CELL_SEARCH_CONTENTSEARCHTYPE_PHOTO_ALL = 2,
              CELL_SEARCH_CONTENTSEARCHTYPE_VIDEO_ALL = 3;
constexpr u32 CELL_SEARCH_TAG_NUM_MAX = 6, CELL_SEARCH_TAG_LEN_MAX = 63;
// Sizes: CellSearchContentId 16, the info buffer CELL_SEARCH_CONTENT_BUFFER_SIZE_MAX 2048, CellSearchTimeInfo (three be
// s64: taken, imported, modified) 24, CellSearchContentInfoPath and CellSearchContentInfoPathMovieThumb 128,
// CellSearchResultParam (be s32 searchId, be u32 resultNum) 8.
constexpr u32 kContentIdSize = 16, kInfoBufferSize = 2048, kTimeInfoSize = 24, kInfoPathSize = 128,
              kResultParamSize = 8;
constexpr u32 kSearchIdCount = 1024; // RPCS3 search_object_t::id_count

bool valid_sort_key(s32 key) { return key >= CELL_SEARCH_SORTKEY_DEFAULT && key <= CELL_SEARCH_SORTKEY_MODIFIEDDATE; }
bool valid_sort_order(s32 order)
{
    return order == CELL_SEARCH_SORTORDER_ASCENDING || order == CELL_SEARCH_SORTORDER_DESCENDING;
}

// RPCS3 search_state.
enum class State : u8 { NotInitialized, Idle, InProgress, Initializing, Canceling, Finalizing };

struct Search {}; // RPCS3 search_object_t: the content a search found, never any here

struct SearchLib {
    std::mutex mutex;
    State state = State::NotInitialized;
    u32 func = 0, user_data = 0; // CellSearchSystemCallback(event, result, param, userData) and its argument
    IdTable<Search, 1, kSearchIdCount> searches;
};

SearchLib& lib()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSearch called without an HLE process");
    return p->state<SearchLib>();
}

// RPCS3 check_search_state: whether the library, in `state`, may start `action` (InProgress also stands for every
// call that needs it idle).
s32 check_state(State state, State action)
{
    switch (action) {
    case State::Initializing:
        switch (state) {
        case State::NotInitialized: return lv2::CELL_OK;
        case State::Initializing: return error(CELL_SEARCH_ERROR_BUSY);
        case State::Finalizing: return error(CELL_SEARCH_ERROR_FINALIZING);
        default: return error(CELL_SEARCH_ERROR_ALREADY_INITIALIZED);
        }
    case State::Finalizing:
        switch (state) {
        case State::Idle: return lv2::CELL_OK;
        case State::NotInitialized: return error(CELL_SEARCH_ERROR_NOT_INITIALIZED);
        case State::Finalizing: return error(CELL_SEARCH_ERROR_FINALIZING);
        case State::InProgress:
        case State::Initializing:
        case State::Canceling: return error(CELL_SEARCH_ERROR_BUSY);
        }
        return error(CELL_SEARCH_ERROR_GENERIC);
    case State::Canceling:
        switch (state) {
        case State::InProgress: return lv2::CELL_OK;
        case State::NotInitialized:
        case State::Initializing: return error(CELL_SEARCH_ERROR_NOT_INITIALIZED);
        case State::Finalizing: return error(CELL_SEARCH_ERROR_FINALIZING);
        case State::Canceling: return error(CELL_SEARCH_ERROR_BUSY);
        case State::Idle: return error(CELL_SEARCH_ERROR_ALREADY_GOT_RESULT);
        }
        return error(CELL_SEARCH_ERROR_GENERIC);
    default:
        switch (state) {
        case State::Idle: return lv2::CELL_OK;
        case State::NotInitialized:
        case State::Initializing: return error(CELL_SEARCH_ERROR_NOT_INITIALIZED);
        case State::Finalizing: return error(CELL_SEARCH_ERROR_FINALIZING);
        case State::InProgress:
        case State::Canceling: return error(CELL_SEARCH_ERROR_BUSY);
        }
        return error(CELL_SEARCH_ERROR_GENERIC);
    }
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// Each non-null pointer of `ptrs` ({address, size}) usable; logs the first that isn't.
bool all_usable(PPUContext& ctx, std::string_view call, std::initializer_list<std::pair<u32, u32>> ptrs)
{
    for (const auto& [addr, size] : ptrs) {
        if (addr && !usable(ctx, addr, size, call)) return false;
    }
    return true;
}

// The game's callback, at its next cellSysutilCheckCallback, after the library moves to `next` (RPCS3 stores the state
// in its sysutil_register_cb). A search's result comes with a CellSearchResultParam on the stack: its ID and no
// content.
void queue_callback(s32 event, State next, u32 search_id = 0)
{
    sysutil_queue_call([event, next, search_id](PPUContext& ctx) -> s32 {
        SearchLib& s = lib();
        u32 func = 0, user_data = 0;
        {
            std::lock_guard lock(s.mutex);
            s.state = next;
            func = s.func;
            user_data = s.user_data;
        }
        if (!search_id) {
            call_guest(ctx, func, {static_cast<u64>(event), lv2::CELL_OK, 0, user_data});
            return lv2::CELL_OK;
        }
        u8* base = guest_base(ctx);
        StackVar param(ctx, base, kResultParamSize);
        ps_write32(base, param.addr(), search_id);
        ps_write32(base, param.addr() + 4, 0); // resultNum: the library is empty
        call_guest(ctx, func, {static_cast<u64>(event), lv2::CELL_OK, param.addr(), user_data});
        return lv2::CELL_OK;
    });
}

// RPCS3's compare_and_swap(idle, in_progress) for a new search.
s32 begin_search(SearchLib& s)
{
    const s32 r = check_state(s.state, State::InProgress);
    if (r == lv2::CELL_OK) s.state = State::InProgress;
    return r;
}

// The new search's ID (idm::make<search_object_t>) to *out_search_id, and its result queued for the game.
s32 start_search(PPUContext& ctx, SearchLib& s, s32 event, u32 out_search_id)
{
    const u32 id = s.searches.make({});
    if (!id) {
        s.state = State::Idle;
        return error(CELL_SEARCH_ERROR_NO_MEMORY);
    }
    ps_write32(guest_base(ctx), out_search_id, id);
    queue_callback(event, State::Idle, id);
    return lv2::CELL_OK;
}

// RPCS3's "Reset values first" of a content ID: 4 zero bytes, then 12 of 0xFF.
void reset_content_id(u8* base, u32 addr)
{
    std::memset(base + addr, 0, 4);
    std::memset(base + addr + 4, 0xFF, kContentIdSize - 4);
}

s32 cellSearchInitialize(PPUContext&, s32 mode, u32 container, u32 func, u32 user_data)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchInitialize(mode {}, container {:#x}, func {:#x}, userData {:#x})",
                             mode, container, func, user_data));
    };
    if (mode != CELL_SEARCH_MODE_NORMAL) return done(error(CELL_SEARCH_ERROR_UNKNOWN_MODE));
    if (!func) return done(error(CELL_SEARCH_ERROR_PARAM));
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_state(s.state, State::Initializing)) return done(r);
    s.state = State::Initializing;
    s.func = func;
    s.user_data = user_data;
    queue_callback(CELL_SEARCH_EVENT_INITIALIZE_RESULT, State::Idle);
    return done(lv2::CELL_OK);
}

s32 cellSearchFinalize(PPUContext&)
{
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_state(s.state, State::Finalizing)) return finish(r, "cellSearchFinalize()");
    s.state = State::Finalizing;
    queue_callback(CELL_SEARCH_EVENT_FINALIZE_RESULT, State::NotInitialized);
    return finish(lv2::CELL_OK, "cellSearchFinalize()");
}

// RPCS3 cellSearchStartListSearch: a list type and ascending order (descending is valid but not supported).
s32 cellSearchStartListSearch(PPUContext& ctx, s32 type, s32 sort_order, u32 out_search_id)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchStartListSearch(type {}, sortOrder {}, outSearchId {:#x})", type,
                             sort_order, out_search_id));
    };
    if (!out_search_id) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (!usable(ctx, out_search_id, 4, "cellSearchStartListSearch: outSearchId")) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    ps_write32(guest_base(ctx), out_search_id, 0);
    if (type < 1 || type > 9) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (!valid_sort_order(sort_order)) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (sort_order != CELL_SEARCH_SORTORDER_ASCENDING) return done(error(CELL_SEARCH_ERROR_NOT_SUPPORTED_SEARCH));
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = begin_search(s)) return done(r);
    return done(start_search(ctx, s, CELL_SEARCH_EVENT_LISTSEARCH_RESULT, out_search_id));
}

// RPCS3 cellSearchStartContentSearchInList: the contents of a list a list search found; none ever is.
s32 cellSearchStartContentSearchInList(PPUContext& ctx, u32 list_id, s32 sort_key, s32 sort_order, u32 out_search_id)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchStartContentSearchInList(listId {:#x}, sortKey {}, sortOrder {}, "
                                     "outSearchId {:#x})",
                             list_id, sort_key, sort_order, out_search_id));
    };
    if (!all_usable(ctx, "cellSearchStartContentSearchInList: outSearchId", {{out_search_id, 4}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    if (out_search_id) ps_write32(guest_base(ctx), out_search_id, 0);
    if (!list_id || !out_search_id) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (!valid_sort_key(sort_key) || !valid_sort_order(sort_order)) return done(error(CELL_SEARCH_ERROR_PARAM));
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = begin_search(s)) return done(r);
    s.state = State::Idle; // RPCS3 leaves it in progress
    if (!usable(ctx, list_id, kContentIdSize, "cellSearchStartContentSearchInList: listId")) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    return done(error(CELL_SEARCH_ERROR_CONTENT_NOT_FOUND));
}

// RPCS3 cellSearchStartContentSearch: every music, photo or video in an order RPCS3 supports.
s32 cellSearchStartContentSearch(PPUContext& ctx, s32 type, s32 sort_key, s32 sort_order, u32 out_search_id)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchStartContentSearch(type {}, sortKey {}, sortOrder {}, outSearchId "
                                     "{:#x})",
                             type, sort_key, sort_order, out_search_id));
    };
    if (!all_usable(ctx, "cellSearchStartContentSearch: outSearchId", {{out_search_id, 4}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    if (out_search_id) ps_write32(guest_base(ctx), out_search_id, 0);
    if (!out_search_id) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (!valid_sort_key(sort_key) || !valid_sort_order(sort_order)) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (type < CELL_SEARCH_CONTENTSEARCHTYPE_MUSIC_ALL || type > CELL_SEARCH_CONTENTSEARCHTYPE_VIDEO_ALL) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = begin_search(s)) return done(r);
    s32 key = sort_key;
    if (key == CELL_SEARCH_SORTKEY_DEFAULT) {
        key = type == CELL_SEARCH_CONTENTSEARCHTYPE_MUSIC_ALL   ? CELL_SEARCH_SORTKEY_ARTISTNAME
              : type == CELL_SEARCH_CONTENTSEARCHTYPE_PHOTO_ALL ? CELL_SEARCH_SORTKEY_TAKENDATE
                                                                : CELL_SEARCH_SORTKEY_TITLE;
    }
    bool supported = true;
    if (key != CELL_SEARCH_SORTKEY_IMPORTEDDATE && key != CELL_SEARCH_SORTKEY_MODIFIEDDATE) {
        const bool ascending = sort_order == CELL_SEARCH_SORTORDER_ASCENDING;
        switch (type) {
        case CELL_SEARCH_CONTENTSEARCHTYPE_MUSIC_ALL:
            supported = ascending && (key == CELL_SEARCH_SORTKEY_ARTISTNAME || key == CELL_SEARCH_SORTKEY_ALBUMTITLE ||
                                         key == CELL_SEARCH_SORTKEY_GENRENAME || key == CELL_SEARCH_SORTKEY_TITLE);
            break;
        case CELL_SEARCH_CONTENTSEARCHTYPE_PHOTO_ALL:
            supported = key == CELL_SEARCH_SORTKEY_TAKENDATE || (ascending && key == CELL_SEARCH_SORTKEY_TITLE);
            break;
        default: supported = ascending && key == CELL_SEARCH_SORTKEY_TITLE; break;
        }
    }
    if (!supported) {
        s.state = State::Idle; // RPCS3 leaves it in progress
        return done(error(CELL_SEARCH_ERROR_NOT_SUPPORTED_SEARCH));
    }
    return done(start_search(ctx, s, CELL_SEARCH_EVENT_CONTENTSEARCH_RESULT, out_search_id));
}

// RPCS3 cellSearchStartSceneSearchInVideo: the scenes of a video a search found; none ever is.
s32 cellSearchStartSceneSearchInVideo(PPUContext& ctx, u32 video_id, s32 search_type, s32 sort_order,
    u32 out_search_id)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchStartSceneSearchInVideo(videoId {:#x}, searchType {}, sortOrder {}, "
                                     "outSearchId {:#x})",
                             video_id, search_type, sort_order, out_search_id));
    };
    if (!all_usable(ctx, "cellSearchStartSceneSearchInVideo: outSearchId", {{out_search_id, 4}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    if (out_search_id) ps_write32(guest_base(ctx), out_search_id, 0);
    if (!video_id || !out_search_id) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (search_type < 1 || search_type > 5 || !valid_sort_order(sort_order)) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = begin_search(s)) return done(r);
    s.state = State::Idle; // RPCS3 leaves it in progress
    if (!usable(ctx, video_id, kContentIdSize, "cellSearchStartSceneSearchInVideo: videoId")) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    return done(error(CELL_SEARCH_ERROR_CONTENT_NOT_FOUND));
}

// RPCS3 cellSearchStartSceneSearch(searchType, gameTitle, tags, tagNum, sortKey, sortOrder, outSearchId): the scenes
// a game tagged; up to six tags, each NUL-terminated within 63 bytes.
s32 cellSearchStartSceneSearch(PPUContext& ctx, s32 search_type, u32 game_title, u32 tags, u32 tag_num, s32 sort_key,
    s32 sort_order, u32 out_search_id)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchStartSceneSearch(searchType {}, gameTitle \"{}\", tags {:#x}, tagNum "
                                     "{}, sortKey {}, sortOrder {}, outSearchId {:#x})",
                             search_type, read_cstr(t, game_title, 128).text, tags, tag_num, sort_key, sort_order,
                             out_search_id));
    };
    if (!all_usable(ctx, "cellSearchStartSceneSearch: outSearchId", {{out_search_id, 4}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    if (out_search_id) ps_write32(base, out_search_id, 0);
    if (!game_title || !out_search_id) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (search_type < 1 || search_type > 5 || !valid_sort_key(sort_key)) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (tag_num) {
        if (tag_num > CELL_SEARCH_TAG_NUM_MAX || !tags) return done(error(CELL_SEARCH_ERROR_TAG));
        if (!usable(ctx, tags, 4 * tag_num, "cellSearchStartSceneSearch: tags")) {
            return done(error(CELL_SEARCH_ERROR_PARAM));
        }
        for (u32 n = 0; n < tag_num; n++) {
            const u32 tag = ps_read32(base, tags + 4 * n);
            if (!tag) return done(error(CELL_SEARCH_ERROR_TAG));
            const GuestString text = read_cstr(t, tag, CELL_SEARCH_TAG_LEN_MAX);
            if (text.faulted) {
                log_call_site(ctx, base, std::format("cellSearchStartSceneSearch: tag {} at {:#x} leaves guest memory",
                                             n, tag));
                return done(error(CELL_SEARCH_ERROR_PARAM));
            }
            if (!text.terminated) return done(error(CELL_SEARCH_ERROR_TAG));
        }
    }
    if (sort_key != CELL_SEARCH_SORTKEY_DEFAULT && sort_key != CELL_SEARCH_SORTKEY_IMPORTEDDATE &&
        sort_key != CELL_SEARCH_SORTKEY_MODIFIEDDATE &&
        (sort_key != CELL_SEARCH_SORTKEY_TITLE || sort_order != CELL_SEARCH_SORTORDER_ASCENDING)) {
        return done(error(CELL_SEARCH_ERROR_NOT_SUPPORTED_SEARCH));
    }
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = begin_search(s)) return done(r);
    return done(start_search(ctx, s, CELL_SEARCH_EVENT_SCENESEARCH_RESULT, out_search_id));
}

// RPCS3 cellSearchGetContentInfoByOffset(searchId, offset, infoBuffer, outContentType, outContentId): no search ever
// found anything, so every offset is out of range.
s32 cellSearchGetContentInfoByOffset(PPUContext& ctx, s32 search_id, s32 offset, u32 info_buffer,
    u32 out_content_type, u32 out_content_id)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchGetContentInfoByOffset(searchId {}, offset {}, infoBuffer {:#x}, "
                                     "outContentType {:#x}, outContentId {:#x})",
                             search_id, offset, info_buffer, out_content_type, out_content_id));
    };
    if (!all_usable(ctx, "cellSearchGetContentInfoByOffset",
            {{out_content_type, 4}, {info_buffer, kInfoBufferSize}, {out_content_id, kContentIdSize}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    u8* base = guest_base(ctx);
    if (out_content_type) ps_write32(base, out_content_type, 0); // CELL_SEARCH_CONTENTTYPE_NONE
    if (info_buffer) std::memset(base + info_buffer, 0, kInfoBufferSize);
    if (out_content_id) reset_content_id(base, out_content_id);
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (!s.searches.find(static_cast<u32>(search_id))) return done(error(CELL_SEARCH_ERROR_INVALID_SEARCHID));
    if (!out_content_type || (!out_content_id && !info_buffer)) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (const s32 r = check_state(s.state, State::InProgress)) return done(r);
    return done(error(CELL_SEARCH_ERROR_OUT_OF_RANGE));
}

// RPCS3 cellSearchGetContentInfoByContentId(contentId, infoBuffer, outContentType).
s32 cellSearchGetContentInfoByContentId(PPUContext& ctx, u32 content_id, u32 info_buffer, u32 out_content_type)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchGetContentInfoByContentId(contentId {:#x}, infoBuffer {:#x}, "
                                     "outContentType {:#x})",
                             content_id, info_buffer, out_content_type));
    };
    if (!all_usable(ctx, "cellSearchGetContentInfoByContentId",
            {{out_content_type, 4}, {info_buffer, kInfoBufferSize}, {content_id, kContentIdSize}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    u8* base = guest_base(ctx);
    if (out_content_type) ps_write32(base, out_content_type, 0);
    if (info_buffer) std::memset(base + info_buffer, 0, kInfoBufferSize);
    if (!out_content_type || !content_id) return done(error(CELL_SEARCH_ERROR_PARAM));
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_state(s.state, State::InProgress)) return done(r);
    return done(error(CELL_SEARCH_ERROR_CONTENT_NOT_FOUND));
}

// RPCS3 cellSearchGetOffsetByContentId(searchId, contentId, outOffset): the state, then the search, then the pointers.
s32 cellSearchGetOffsetByContentId(PPUContext& ctx, s32 search_id, u32 content_id, u32 out_offset)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchGetOffsetByContentId(searchId {}, contentId {:#x}, outOffset {:#x})",
                             search_id, content_id, out_offset));
    };
    if (!all_usable(ctx, "cellSearchGetOffsetByContentId", {{out_offset, 4}, {content_id, kContentIdSize}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    if (out_offset) ps_write32(guest_base(ctx), out_offset, 0xFFFFFFFFu); // -1
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_state(s.state, State::InProgress)) return done(r);
    if (!s.searches.find(static_cast<u32>(search_id))) return done(error(CELL_SEARCH_ERROR_INVALID_SEARCHID));
    if (!out_offset || !content_id) return done(error(CELL_SEARCH_ERROR_PARAM));
    return done(error(CELL_SEARCH_ERROR_CONTENT_NOT_FOUND));
}

// RPCS3 cellSearchGetContentIdByOffset(searchId, offset, outContentType, outContentId, outTimeInfo).
s32 cellSearchGetContentIdByOffset(PPUContext& ctx, s32 search_id, s32 offset, u32 out_content_type,
    u32 out_content_id, u32 out_time_info)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchGetContentIdByOffset(searchId {}, offset {}, outContentType {:#x}, "
                                     "outContentId {:#x}, outTimeInfo {:#x})",
                             search_id, offset, out_content_type, out_content_id, out_time_info));
    };
    if (!all_usable(ctx, "cellSearchGetContentIdByOffset",
            {{out_time_info, kTimeInfoSize}, {out_content_type, 4}, {out_content_id, kContentIdSize}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    u8* base = guest_base(ctx);
    if (out_time_info) {
        for (u32 i = 0; i < 3; i++) ps_write64(base, out_time_info + 8 * i, ~u64{0}); // -1: no date
    }
    if (out_content_type) ps_write32(base, out_content_type, 0);
    if (out_content_id) reset_content_id(base, out_content_id);
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (!s.searches.find(static_cast<u32>(search_id))) return done(error(CELL_SEARCH_ERROR_INVALID_SEARCHID));
    if (!out_content_type || !out_content_id) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (const s32 r = check_state(s.state, State::InProgress)) return done(r);
    return done(error(CELL_SEARCH_ERROR_OUT_OF_RANGE));
}

// The content lookups RPCS3 makes after clearing their output: the pointers, the state, then the content, never found.
// `reset` bytes at `out` are cleared first (RPCS3's "Reset values first").
s32 content_lookup(PPUContext& ctx, std::string_view name, u32 content_id, u32 out, u32 reset)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("{}(contentId {:#x}, {:#x})", name, content_id, out));
    };
    if (!all_usable(ctx, name, {{out, reset}, {content_id, kContentIdSize}})) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    if (out) std::memset(guest_base(ctx) + out, 0, reset);
    if (!content_id || !out) return done(error(CELL_SEARCH_ERROR_PARAM));
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_state(s.state, State::InProgress)) return done(r);
    return done(error(CELL_SEARCH_ERROR_CONTENT_NOT_FOUND));
}

// gameComment[0] = 0 first.
s32 cellSearchGetContentInfoGameComment(PPUContext& ctx, u32 content_id, u32 game_comment)
{
    return content_lookup(ctx, "cellSearchGetContentInfoGameComment", content_id, game_comment, 1);
}

// The selection context's first four bytes (its magic) cleared first.
s32 cellSearchGetMusicSelectionContextOfSingleTrack(PPUContext& ctx, u32 content_id, u32 out_context)
{
    return content_lookup(ctx, "cellSearchGetMusicSelectionContextOfSingleTrack", content_id, out_context, 4);
}

s32 cellSearchGetContentInfoPath(PPUContext& ctx, u32 content_id, u32 info_path)
{
    return content_lookup(ctx, "cellSearchGetContentInfoPath", content_id, info_path, kInfoPathSize);
}

s32 cellSearchGetContentInfoPathMovieThumb(PPUContext& ctx, u32 content_id, u32 info_mt)
{
    return content_lookup(ctx, "cellSearchGetContentInfoPathMovieThumb", content_id, info_mt, kInfoPathSize);
}

s32 cellSearchGetContentInfoDeveloperData(PPUContext& ctx, u32 content_id, u32 developer_data)
{
    return content_lookup(ctx, "cellSearchGetContentInfoDeveloperData", content_id, developer_data, 1);
}

// *sharable = CELL_SEARCH_SHARABLETYPE_PROHIBITED (0) first.
s32 cellSearchGetContentInfoSharable(PPUContext& ctx, u32 content_id, u32 sharable)
{
    return content_lookup(ctx, "cellSearchGetContentInfoSharable", content_id, sharable, 4);
}

// RPCS3 cellSearchGetMusicSelectionContext(searchId, contentId, repeatMode, option, outContext): a playlist of what
// the search found, which is nothing.
s32 cellSearchGetMusicSelectionContext(PPUContext& ctx, s32 search_id, u32 content_id, s32 repeat_mode, s32 option,
    u32 out_context)
{
    const auto done = [&](s32 r) {
        return finish(r, std::format("cellSearchGetMusicSelectionContext(searchId {}, contentId {:#x}, repeatMode {}, "
                                     "option {}, outContext {:#x})",
                             search_id, content_id, repeat_mode, option, out_context));
    };
    if (!out_context) return done(error(CELL_SEARCH_ERROR_PARAM));
    if (!usable(ctx, out_context, 4, "cellSearchGetMusicSelectionContext: outContext")) {
        return done(error(CELL_SEARCH_ERROR_PARAM));
    }
    std::memset(guest_base(ctx) + out_context, 0, 4);
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (!s.searches.find(static_cast<u32>(search_id))) return done(error(CELL_SEARCH_ERROR_INVALID_SEARCHID));
    if (const s32 r = check_state(s.state, State::InProgress)) return done(r);
    return done(error(CELL_SEARCH_ERROR_CONTENT_NOT_FOUND));
}

// RPCS3 cellSearchPrepareFile(path): makes a long content path reachable through a short link (RPCS3 mounts it). No
// content, so no link: nothing to prepare.
s32 cellSearchPrepareFile(PPUContext& ctx, u32 path)
{
    const std::string text = path ? read_cstr(thread_of(ctx), path, 1056).text : std::string();
    const auto done = [&](s32 r) { return finish(r, std::format("cellSearchPrepareFile(\"{}\")", text)); };
    if (!path) return done(error(CELL_SEARCH_ERROR_PARAM));
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_state(s.state, State::InProgress)) return done(r);
    return done(lv2::CELL_OK);
}

// RPCS3 cellSearchCancel: a TODO after its checks; the search still finishes (with nothing found).
s32 cellSearchCancel(PPUContext&, s32 search_id)
{
    const std::string call = std::format("cellSearchCancel({})", search_id);
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (!s.searches.find(static_cast<u32>(search_id))) return finish(error(CELL_SEARCH_ERROR_INVALID_SEARCHID), call);
    if (const s32 r = check_state(s.state, State::Canceling)) return finish(r, call);
    return finish(lv2::CELL_OK, call + "; RPCS3 todo: the search still reports");
}

// RPCS3 cellSearchEnd: the ID first, then the library's state, then the search, which goes.
s32 cellSearchEnd(PPUContext&, s32 search_id)
{
    const std::string call = std::format("cellSearchEnd({})", search_id);
    if (!search_id) return finish(error(CELL_SEARCH_ERROR_INVALID_SEARCHID), call);
    SearchLib& s = lib();
    std::lock_guard lock(s.mutex);
    if (const s32 r = check_state(s.state, State::Finalizing)) return finish(r, call);
    if (!s.searches.remove(static_cast<u32>(search_id))) return finish(error(CELL_SEARCH_ERROR_INVALID_SEARCHID), call);
    return finish(lv2::CELL_OK, call);
}

} // namespace

void register_cellSearch(Registry& r)
{
    r.add("cellSearchUtility", {
        PS_HLE_FUNC(0xC81CCF8A, cellSearchInitialize),
        PS_HLE_FUNC(0xBFAB7616, cellSearchFinalize),
        PS_HLE_FUNC(0x0A4C8295, cellSearchStartListSearch),
        PS_HLE_FUNC(0x64FB0B76, cellSearchStartContentSearchInList),
        PS_HLE_FUNC(0x0591826F, cellSearchStartContentSearch),
        PS_HLE_FUNC(0xC0ED0522, cellSearchStartSceneSearchInVideo),
        PS_HLE_FUNC(0x13524FAA, cellSearchStartSceneSearch),
        PS_HLE_FUNC(0x3B210319, cellSearchGetContentInfoByOffset),
        PS_HLE_FUNC(0x9663A44B, cellSearchGetContentInfoByContentId),
        PS_HLE_FUNC(0x540D9068, cellSearchGetOffsetByContentId),
        PS_HLE_FUNC(0x94E21701, cellSearchGetContentIdByOffset),
        PS_HLE_FUNC(0xD7A7A433, cellSearchGetContentInfoGameComment),
        PS_HLE_FUNC(0x025CE169, cellSearchGetMusicSelectionContext),
        PS_HLE_FUNC(0xED20E079, cellSearchGetMusicSelectionContextOfSingleTrack),
        PS_HLE_FUNC(0xFFB28491, cellSearchGetContentInfoPath),
        PS_HLE_FUNC(0x37B5BA0C, cellSearchGetContentInfoPathMovieThumb),
        PS_HLE_FUNC(0xE73CB0D2, cellSearchPrepareFile),
        PS_HLE_FUNC(0x35CDA406, cellSearchGetContentInfoDeveloperData),
        PS_HLE_FUNC(0x1E989496, cellSearchGetContentInfoSharable),
        PS_HLE_FUNC(0x8FE376A6, cellSearchCancel),
        PS_HLE_FUNC(0x774033D6, cellSearchEnd),
    });
}

} // namespace pspocket::hle
