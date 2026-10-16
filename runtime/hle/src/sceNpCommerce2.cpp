// sceNpCommerce2: the PlayStation Store from inside a game (categories, products, prices, and the system's checkout,
// download list and product code dialogs). The store is online only, a non-goal (D-024, docs/phase0-gta5.md).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNpCommerce2.cpp/.h, with its context (commerce2_ctx in
// rpcs3/Emu/NP/np_contexts.cpp/.h) and init flags (is_NP_init, is_NP_Com2_init in rpcs3/Emu/NP/np_handler.h).
//
// RPCS3's library never reaches the store, whether a user is signed in or not, so nothing here depends on being
// offline: each function makes RPCS3's checks in RPCS3's order, then does what RPCS3 does.
// - sceNpCommerce2Init needs NP (sceNp2Init, sceNp.cpp). sceNpCommerce2Term only clears the library's flag: the
//   context stays. NP's term leaves both.
// - One context (SCE_NP_COMMERCE2_CTX_MAX), ID 0x5001 (idm commerce2_ctx: id_base 0x5001, id_count 1), which keeps the
//   game's event handler. The functions that look a context up see only the ID's low 16 bits (RPCS3's
//   get_commerce2_context takes a u16); sceNpCommerce2DestroyCtx compares all 32.
// - sceNpCommerce2CreateSessionStart "creates" the session at once: the context's handler gets CREATE_SESSION_DONE with
//   error 0 from the game's next cellSysutilCheckCallback (RPCS3 sysutil_register_cb; sysutil_queue_call here), and
//   sceNpCommerce2CreateSessionFinish zeroes the session info: RPCS3 has no currency or number format to give.
// - The rest are TODOs in RPCS3: after the arguments are checked, no request ID is written, nothing is asked of the
//   store and nothing comes back (initializing a result fails with INVALID_RESULT_DATA; category info is
//   SERVER_MAINTENANCE, RPCS3's "hack to stop crashes in some games"), no output structure is written, memory
//   containers are not checked, and the dialogs neither open nor send their handler events.
// What RPCS3 implements is logged at info; its TODOs at warn (RPCS3's todo level), saying what was left undone.
//
// Differences from RPCS3:
// - Pointers are checked before use (D-042): one that is not guest memory is logged with the call site and refused
//   before anything changes, with SCE_NP_COMMERCE2_ERROR_INVALID_SKUID in a list of SKU IDs and
//   SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT elsewhere (RPCS3 faults).
// - With its one context in use, sceNpCommerce2CreateCtx returns SCE_NP_COMMERCE2_ERROR_CTX_MAX and writes nothing,
//   where RPCS3 has a TODO for that check; RPCS3 writes idm's invalid ID 0 and returns CELL_OK. (sceNp.cpp's lookup
//   title contexts make the same choice.)

#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "guest.hpp"
#include "idm.hpp"
#include "np.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "sysutil.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNpCommerce2";

enum : u32 {
    SCE_NP_ERROR_NOT_INITIALIZED = 0x8002AA01,
    SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED = 0x80023001,
    SCE_NP_COMMERCE2_ERROR_ALREADY_INITIALIZED = 0x80023002,
    SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT = 0x80023003,
    SCE_NP_COMMERCE2_ERROR_UNSUPPORTED_VERSION = 0x80023004,
    SCE_NP_COMMERCE2_ERROR_CTX_MAX = 0x80023005,
    SCE_NP_COMMERCE2_ERROR_INVALID_SKUID = 0x80023007,
    SCE_NP_COMMERCE2_ERROR_INVALID_SKU_NUM = 0x80023008,
    SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND = 0x8002300C,
    SCE_NP_COMMERCE2_ERROR_INVALID_RESULT_DATA = 0x80023014,
    SCE_NP_COMMERCE2_ERROR_SERVER_MAINTENANCE = 0x80023016,
    SCE_NP_COMMERCE2_ERROR_INSUFFICIENT_BUF_SIZE = 0x80023018,
    SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_TYPE = 0x8002301A,
    SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID = 0x8002301B,
    SCE_NP_COMMERCE2_ERROR_INVALID_SIZE = 0x8002301C,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case SCE_NP_ERROR_NOT_INITIALIZED: return "SCE_NP_ERROR_NOT_INITIALIZED";
    case SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED: return "SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED";
    case SCE_NP_COMMERCE2_ERROR_ALREADY_INITIALIZED: return "SCE_NP_COMMERCE2_ERROR_ALREADY_INITIALIZED";
    case SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT: return "SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT";
    case SCE_NP_COMMERCE2_ERROR_UNSUPPORTED_VERSION: return "SCE_NP_COMMERCE2_ERROR_UNSUPPORTED_VERSION";
    case SCE_NP_COMMERCE2_ERROR_CTX_MAX: return "SCE_NP_COMMERCE2_ERROR_CTX_MAX";
    case SCE_NP_COMMERCE2_ERROR_INVALID_SKUID: return "SCE_NP_COMMERCE2_ERROR_INVALID_SKUID";
    case SCE_NP_COMMERCE2_ERROR_INVALID_SKU_NUM: return "SCE_NP_COMMERCE2_ERROR_INVALID_SKU_NUM";
    case SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND: return "SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND";
    case SCE_NP_COMMERCE2_ERROR_INVALID_RESULT_DATA: return "SCE_NP_COMMERCE2_ERROR_INVALID_RESULT_DATA";
    case SCE_NP_COMMERCE2_ERROR_SERVER_MAINTENANCE: return "SCE_NP_COMMERCE2_ERROR_SERVER_MAINTENANCE";
    case SCE_NP_COMMERCE2_ERROR_INSUFFICIENT_BUF_SIZE: return "SCE_NP_COMMERCE2_ERROR_INSUFFICIENT_BUF_SIZE";
    case SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_TYPE: return "SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_TYPE";
    case SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID: return "SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID";
    case SCE_NP_COMMERCE2_ERROR_INVALID_SIZE: return "SCE_NP_COMMERCE2_ERROR_INVALID_SIZE";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

constexpr u32 SCE_NP_COMMERCE2_VERSION = 2;
constexpr u32 SCE_NP_COMMERCE2_CTX_MAX = 1;
constexpr u32 kCtxIdBase = 0x5001; // idm commerce2_ctx: id_base 0x5001, id_step 1, id_count SCE_NP_COMMERCE2_CTX_MAX
constexpr u32 SCE_NP_COMMERCE2_RECV_BUF_SIZE = 262144;
constexpr u32 SCE_NP_COMMERCE2_GETCAT_MAX_COUNT = 60;
constexpr u32 SCE_NP_COMMERCE2_SKU_CHECKOUT_MAX = 16; // RPCS3 checks the download list's SKUs against it too
constexpr u32 SCE_NP_COMMERCE2_PRODUCT_CODE_BLOCK_LEN = 4;
constexpr u32 SCE_NP_COMMERCE2_PRODUCT_CODE_INPUT_MODE_USER_INPUT = 0;
constexpr u32 SCE_NP_COMMERCE2_PRODUCT_CODE_INPUT_MODE_CODE_SPECIFIED = 1;
constexpr s32 SCE_NP_COMMERCE2_EVENT_CREATE_SESSION_DONE = 0x0011;
constexpr s32 SCE_NP_COMMERCE2_EVENT_EMPTY_STORE_CHECK_DONE = 0x0061;
constexpr u32 SCE_NP_COMMERCE2_STORE_IS_NOT_EMPTY = 0;
constexpr s32 SCE_NP_COMMERCE2_STORE_CHECK_TYPE_CATEGORY = 1;
constexpr s32 SCE_NP_COMMERCE2_STORE_BROWSE_TYPE_CATEGORY = 1, SCE_NP_COMMERCE2_STORE_BROWSE_TYPE_PRODUCT_CODE = 3;
constexpr u32 SCE_NP_COMMERCE2_GETPRODLIST_MAX_COUNT = 60;
constexpr u32 kProductBrowseParamSize = 4; // SceNpCommerce2ProductBrowseParam: be32 size

// Guest structures (RPCS3 sceNpCommerce2.h, sceNp.h).
// SceNpId (0x24 bytes), whose first 16 characters are the online ID.
constexpr u32 kNpIdSize = 0x24, kOnlineIdLength = 16;
// SceNpCommerce2SessionInfo (48 bytes): RPCS3 zeroes its first 44 bytes, or only its first 8 (currencyCode, decimals)
// for a game built with an SDK before 3.0.
constexpr u32 kSessionInfoCleared = 44, kSessionInfoClearedBefore300 = 8;
constexpr s32 kSdk300 = 0x300000;
// SceNpCommerce2ProductCodeParam (32 bytes): size, inputMode, then three blocks of char[4 + 1] at 8, 16 and 24.
constexpr u32 kProductCodeParamSize = 32, kProductCodeInputMode = 4, kProductCode1 = 8, kProductCodeStride = 8;
// The longest guest string a log line shows.
constexpr u32 kLogStringMax = 64;

// RPCS3's commerce2_ctx also keeps the version and the creator's NP ID, which nothing reads.
struct Commerce2Ctx {
    u32 handler = 0; // the SceNpCommerce2Handler's descriptor; may be 0
    u32 arg = 0;
};

struct Commerce2State {
    std::mutex mutex;
    bool initialized = false; // RPCS3 np_handler::is_NP_Com2_init
    IdTable<Commerce2Ctx, kCtxIdBase, SCE_NP_COMMERCE2_CTX_MAX> ctxs;
};

Commerce2State& commerce2()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNpCommerce2 called without an HLE process");
    return p->state<Commerce2State>();
}

bool initialized()
{
    Commerce2State& s = commerce2();
    std::lock_guard lock(s.mutex);
    return s.initialized;
}

// The context, found as RPCS3's get_commerce2_context(u16) finds it: by the ID's low 16 bits.
std::optional<Commerce2Ctx> find_ctx(u32 ctx_id)
{
    Commerce2State& s = commerce2();
    std::lock_guard lock(s.mutex);
    const Commerce2Ctx* c = s.ctxs.find(static_cast<u16>(ctx_id));
    if (!c) return std::nullopt;
    return *c;
}

// One call's log line, written with its result: at info for what RPCS3 implements; at warn for RPCS3's TODOs (its
// todo level), with what the call left undone when it returns CELL_OK.
struct Call {
    log::Level level;
    std::string_view undone;
    std::string text;

    s32 operator()(s32 r, std::string_view detail = {}) const
    {
        const bool stub = r == lv2::CELL_OK && !undone.empty();
        log::print(level, kChan, "{}: {}{}{}{}", text, result_name(r), detail, stub ? "; RPCS3 todo: " : "",
            stub ? undone : std::string_view{});
        return r;
    }
};

template <class... A>
Call implemented(std::format_string<A...> fmt, A&&... args)
{
    return {log::Level::Info, {}, std::format(fmt, std::forward<A>(args)...)};
}

template <class... A>
Call todo(std::string_view undone, std::format_string<A...> fmt, A&&... args)
{
    return {log::Level::Warn, undone, std::format(fmt, std::forward<A>(args)...)};
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// A guest string for a log line: null, or quoted (cut where guest memory ends).
std::string text_at(PPUContext& ctx, u32 addr, u32 max = kLogStringMax)
{
    if (!addr) return "null";
    lv2::PpuThread& t = thread_of(ctx);
    std::string out = "\"";
    for (u32 i = 0; i < max && guest_readable(t, addr + i, 1); i++) {
        const char c = static_cast<char>(ps_read8(t.kernel.base(), addr + i));
        if (!c) break;
        out += c;
    }
    return out + "\"";
}

// utils::isalnum in the C locale, as RPCS3 checks product codes: ASCII letters and digits.
bool alnum(u8 c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// ------------------------------------------------------------------------------------------ init and the context

// RPCS3 sceNpCommerce2Init: the library needs NP up (RPCS3 is_NP_init; sceNp2Init here).
s32 sceNpCommerce2Init(PPUContext&)
{
    const Call done = implemented("sceNpCommerce2Init()");
    Commerce2State& s = commerce2();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return done(error(SCE_NP_COMMERCE2_ERROR_ALREADY_INITIALIZED));
    if (!np_initialized()) return done(error(SCE_NP_ERROR_NOT_INITIALIZED));
    s.initialized = true;
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2Term: clears the flag in any state; the context stays.
s32 sceNpCommerce2Term(PPUContext&)
{
    Commerce2State& s = commerce2();
    std::lock_guard lock(s.mutex);
    s.initialized = false;
    return implemented("sceNpCommerce2Term()")(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2CreateCtx(version, const SceNpId* npId, SceNpCommerce2Handler handler, void* arg, u32* ctx_id):
// the context keeps the handler and its argument for the session event. Without ctx_id no context is made, and the
// result is CELL_OK all the same.
s32 sceNpCommerce2CreateCtx(PPUContext& ctx, u32 version, u32 np_id, u32 handler, u32 arg, u32 ctx_id)
{
    const Call done = implemented("sceNpCommerce2CreateCtx(version {}, npId {:#x}, handler {:#x}, arg {:#x}, "
                                  "ctx_id {:#x})", version, np_id, handler, arg, ctx_id);
    Commerce2State& s = commerce2();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (version > SCE_NP_COMMERCE2_VERSION) return done(error(SCE_NP_COMMERCE2_ERROR_UNSUPPORTED_VERSION));
    if (s.ctxs.full()) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_MAX)); // a TODO in RPCS3 (see the top)
    if (!np_id) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    if (!ctx_id) return done(lv2::CELL_OK, "; ctx_id is null, so no context is made (as RPCS3)");
    // RPCS3 copies the NP ID into the new context, then writes its ID.
    if (!usable(ctx, np_id, kNpIdSize, "sceNpCommerce2CreateCtx: npId") ||
        !usable(ctx, ctx_id, 4, "sceNpCommerce2CreateCtx: ctx_id")) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    const u32 id = s.ctxs.make({handler, arg});
    ps_write32(guest_base(ctx), ctx_id, id);
    return done(lv2::CELL_OK, std::format("; context {:#x} for {}", id, text_at(ctx, np_id, kOnlineIdLength)));
}

s32 sceNpCommerce2DestroyCtx(PPUContext&, u32 ctx_id)
{
    const Call done = implemented("sceNpCommerce2DestroyCtx({:#x})", ctx_id);
    Commerce2State& s = commerce2();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!s.ctxs.remove(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK);
}

// ------------------------------------------------------------------------------------------------------ session

// RPCS3 sceNpCommerce2CreateSessionStart: the session is "created" at once. The context's handler, if it has one, gets
// (ctx_id as the game passed it, subject 0, CREATE_SESSION_DONE, error 0, arg) from the game's next
// cellSysutilCheckCallback.
s32 sceNpCommerce2CreateSessionStart(PPUContext&, u32 ctx_id)
{
    const Call done = implemented("sceNpCommerce2CreateSessionStart({:#x})", ctx_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    const std::optional<Commerce2Ctx> c = find_ctx(ctx_id);
    if (!c) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    if (!c->handler) return done(lv2::CELL_OK, "; the context has no handler to tell");
    sysutil_queue_call([ctx_id, handler = c->handler, arg = c->arg](PPUContext& cb_ctx) -> s32 {
        PS_LOG_INFO(kChan, "cellSysutilCheckCallback: commerce handler {:#x}(ctx {:#x}, 0, CREATE_SESSION_DONE, 0, "
                           "arg {:#x})", handler, ctx_id, arg);
        call_guest(cb_ctx, handler, {ctx_id, 0, SCE_NP_COMMERCE2_EVENT_CREATE_SESSION_DONE, 0, arg});
        return lv2::CELL_OK; // the handler returns void
    });
    return done(lv2::CELL_OK, "; CREATE_SESSION_DONE goes to the handler at the next cellSysutilCheckCallback");
}

// RPCS3 sceNpCommerce2CreateSessionFinish(ctx_id, SceNpCommerce2SessionInfo* sessionInfo): the session's currency and
// number format, which RPCS3 doesn't have (a TODO), so it zeroes them.
s32 sceNpCommerce2CreateSessionFinish(PPUContext& ctx, u32 ctx_id, u32 session_info)
{
    const Call done = todo("the session info is zero: no currency or number format",
        "sceNpCommerce2CreateSessionFinish(ctx {:#x}, sessionInfo {:#x})", ctx_id, session_info);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    lv2::PpuThread& t = thread_of(ctx);
    const u32 size = static_cast<s32>(t.kernel.config().sdk_version) < kSdk300 ? kSessionInfoClearedBefore300
                                                                                 : kSessionInfoCleared;
    if (!usable(ctx, session_info, size, "sceNpCommerce2CreateSessionFinish: sessionInfo")) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    u8* base = t.kernel.base();
    for (u32 i = 0; i < size; i++) ps_write8(base, session_info + i, 0);
    return done(lv2::CELL_OK);
}

// ------------------------------------------------------------------------------ requests and results (TODOs)

// RPCS3 sceNpCommerce2GetCategoryContentsCreateReq(ctx_id, u32* req_id): the context isn't looked at.
s32 sceNpCommerce2GetCategoryContentsCreateReq(PPUContext&, u32 ctx_id, u32 req_id)
{
    const Call done = todo("no request is made and *req_id is not written",
        "sceNpCommerce2GetCategoryContentsCreateReq(ctx {:#x}, req_id {:#x})", ctx_id, req_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!req_id) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2GetCategoryContentsStart(req_id, const char* categoryId, startPosition, maxCountOfResults): at
// most 60 results (SCE_NP_COMMERCE2_GETCAT_MAX_COUNT); the request isn't looked at.
s32 sceNpCommerce2GetCategoryContentsStart(PPUContext& ctx, u32 req_id, u32 category_id, u32 start_position,
    u32 max_count)
{
    const Call done = todo("nothing is asked of the store",
        "sceNpCommerce2GetCategoryContentsStart(req {:#x}, category {}, from {}, at most {})", req_id,
        text_at(ctx, category_id), start_position, max_count);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!category_id || max_count > SCE_NP_COMMERCE2_GETCAT_MAX_COUNT) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2GetCategoryContentsGetResult / GetProductInfoGetResult(req_id, void* buf, buf_size,
// u32* fill_size): a buffer of at least 256 KB (SCE_NP_COMMERCE2_RECV_BUF_SIZE), then the pointers (RPCS3 get_result,
// whose check of buf_size can't fail by then); nothing is received.
s32 get_result(std::string_view fn, u32 req_id, u32 buf, u32 buf_size, u32 fill_size)
{
    const Call done = todo("nothing is received: the buffer and *fill_size are not written",
        "{}(req {:#x}, buf {:#x}, {} bytes, fill_size {:#x})", fn, req_id, buf, buf_size, fill_size);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (buf_size < SCE_NP_COMMERCE2_RECV_BUF_SIZE) return done(error(SCE_NP_COMMERCE2_ERROR_INSUFFICIENT_BUF_SIZE));
    if (!buf || !fill_size) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2InitGetCategoryContentsResult / InitGetProductInfoResult(result, const void* data, data_size):
// the result, the data and its size are needed. RPCS3 can't read result data (check_and_get_data: under 60 bytes it
// is invalid, and longer data isn't parsed, a TODO), so any data is invalid and the result is not written.
s32 init_result(std::string_view fn, u32 result, u32 data, u32 data_size)
{
    const Call done = todo({}, "{}(result {:#x}, data {:#x}, {} bytes)", fn, result, data, data_size);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!result || !data || !data_size) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_RESULT_DATA), "; RPCS3 todo: result data is not parsed");
}

// RPCS3's getters of result data (TODOs): the library initialized and both pointers given; nothing is written.
s32 get_info(const Call& done, u32 from, u32 to)
{
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!from || !to) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    return done(lv2::CELL_OK);
}

s32 sceNpCommerce2GetCategoryContentsGetResult(PPUContext&, u32 req_id, u32 buf, u32 buf_size, u32 fill_size)
{
    return get_result("sceNpCommerce2GetCategoryContentsGetResult", req_id, buf, buf_size, fill_size);
}

s32 sceNpCommerce2InitGetCategoryContentsResult(PPUContext&, u32 result, u32 data, u32 data_size)
{
    return init_result("sceNpCommerce2InitGetCategoryContentsResult", result, data, data_size);
}

// RPCS3 sceNpCommerce2GetCategoryInfo(result, SceNpCommerce2CategoryInfo*): SERVER_MAINTENANCE once both pointers are
// given ("hack to stop crashes in some games").
s32 sceNpCommerce2GetCategoryInfo(PPUContext&, u32 result, u32 category_info)
{
    const Call done =
        todo({}, "sceNpCommerce2GetCategoryInfo(result {:#x}, categoryInfo {:#x})", result, category_info);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!result || !category_info) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    return done(error(SCE_NP_COMMERCE2_ERROR_SERVER_MAINTENANCE), "; RPCS3's answer while category info is a todo");
}

s32 sceNpCommerce2GetContentInfo(PPUContext&, u32 result, u32 index, u32 content_info)
{
    return get_info(todo("*contentInfo is not written",
                        "sceNpCommerce2GetContentInfo(result {:#x}, index {}, contentInfo {:#x})", result, index,
                        content_info),
        result, content_info);
}

s32 sceNpCommerce2GetGameProductInfoFromContentInfo(PPUContext&, u32 content_info, u32 game_product_info)
{
    return get_info(todo("*gameProductInfo is not written",
                        "sceNpCommerce2GetGameProductInfoFromContentInfo(contentInfo {:#x}, gameProductInfo {:#x})",
                        content_info, game_product_info),
        content_info, game_product_info);
}

// RPCS3 sceNpCommerce2DestroyGetCategoryContentsResult / DestroyGetProductInfoResult: CELL_OK in any state.
s32 sceNpCommerce2DestroyGetCategoryContentsResult(PPUContext&, u32 result)
{
    return todo("nothing to free", "sceNpCommerce2DestroyGetCategoryContentsResult({:#x})", result)(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2GetProductInfoCreateReq(ctx_id, u32* req_id): unlike the category request, the context must
// exist.
s32 sceNpCommerce2GetProductInfoCreateReq(PPUContext&, u32 ctx_id, u32 req_id)
{
    const Call done = todo("no request is made and *req_id is not written",
        "sceNpCommerce2GetProductInfoCreateReq(ctx {:#x}, req_id {:#x})", ctx_id, req_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!req_id) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2GetProductInfoStart(req_id, const char* categoryId, const char* productId): the product ID is
// needed, the category isn't.
s32 sceNpCommerce2GetProductInfoStart(PPUContext& ctx, u32 req_id, u32 category_id, u32 product_id)
{
    const Call done = todo("nothing is asked of the store",
        "sceNpCommerce2GetProductInfoStart(req {:#x}, category {}, product {})", req_id, text_at(ctx, category_id),
        text_at(ctx, product_id));
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!product_id) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    return done(lv2::CELL_OK);
}

s32 sceNpCommerce2GetProductInfoGetResult(PPUContext&, u32 req_id, u32 buf, u32 buf_size, u32 fill_size)
{
    return get_result("sceNpCommerce2GetProductInfoGetResult", req_id, buf, buf_size, fill_size);
}

s32 sceNpCommerce2InitGetProductInfoResult(PPUContext&, u32 result, u32 data, u32 data_size)
{
    return init_result("sceNpCommerce2InitGetProductInfoResult", result, data, data_size);
}

s32 sceNpCommerce2GetGameProductInfo(PPUContext&, u32 result, u32 game_product_info)
{
    return get_info(todo("*gameProductInfo is not written",
                        "sceNpCommerce2GetGameProductInfo(result {:#x}, gameProductInfo {:#x})", result,
                        game_product_info),
        result, game_product_info);
}

s32 sceNpCommerce2DestroyGetProductInfoResult(PPUContext&, u32 result)
{
    return todo("nothing to free", "sceNpCommerce2DestroyGetProductInfoResult({:#x})", result)(lv2::CELL_OK);
}

// sceNpCommerce2DestroyGetProductInfoListResult(result): Sony's libsysutil_np_commerce2 answers 0 and does nothing
// (D-923).
s32 sceNpCommerce2DestroyGetProductInfoListResult(PPUContext&, u32) { return lv2::CELL_OK; }

s32 sceNpCommerce2GetGameSkuInfoFromGameProductInfo(PPUContext&, u32 game_product_info, u32 index, u32 game_sku_info)
{
    return get_info(todo("*gameSkuInfo is not written",
                        "sceNpCommerce2GetGameSkuInfoFromGameProductInfo(gameProductInfo {:#x}, index {}, "
                        "gameSkuInfo {:#x})", game_product_info, index, game_sku_info),
        game_product_info, game_sku_info);
}

// RPCS3 sceNpCommerce2GetPrice(ctx_id, char* buf, buflen, price): the price as text in the session's currency; only
// the library and the context are checked.
s32 sceNpCommerce2GetPrice(PPUContext&, u32 ctx_id, u32 buf, u32 buflen, u32 price)
{
    const Call done = todo("buf is not written", "sceNpCommerce2GetPrice(ctx {:#x}, buf {:#x}, {} bytes, price {})",
        ctx_id, buf, buflen, price);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2AbortReq / DestroyReq: only the library is checked (no request is ever made).
s32 sceNpCommerce2AbortReq(PPUContext&, u32 req_id)
{
    const Call done = todo("no request runs", "sceNpCommerce2AbortReq({:#x})", req_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    return done(lv2::CELL_OK);
}

s32 sceNpCommerce2DestroyReq(PPUContext&, u32 req_id)
{
    const Call done = todo("no request exists", "sceNpCommerce2DestroyReq({:#x})", req_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    return done(lv2::CELL_OK);
}

// --------------------------------------------------------------------------------------------- dialogs (TODOs)

// RPCS3's check of the SKU IDs given to a dialog: sku_num pointers at sku_ids, each to a non-empty string (without SKUs
// sku_ids isn't looked at). 0 or SCE_NP_COMMERCE2_ERROR_INVALID_SKUID; `list` gets the IDs for the log.
s32 check_sku_ids(PPUContext& ctx, std::string_view fn, u32 sku_ids, u32 sku_num, std::string& list)
{
    u8* base = guest_base(ctx);
    for (u32 i = 0; i < sku_num; i++) {
        if (!sku_ids) return error(SCE_NP_COMMERCE2_ERROR_INVALID_SKUID);
        const u32 entry = sku_ids + 4 * i;
        if (!usable(ctx, entry, 4, std::format("{}: sku_ids[{}]", fn, i))) {
            return error(SCE_NP_COMMERCE2_ERROR_INVALID_SKUID);
        }
        const u32 sku = ps_read32(base, entry);
        if (!sku) return error(SCE_NP_COMMERCE2_ERROR_INVALID_SKUID);
        if (!usable(ctx, sku, 1, std::format("{}: SKU ID {}", fn, i))) {
            return error(SCE_NP_COMMERCE2_ERROR_INVALID_SKUID);
        }
        if (!ps_read8(base, sku)) return error(SCE_NP_COMMERCE2_ERROR_INVALID_SKUID);
        list += (i ? ", " : "") + text_at(ctx, sku);
    }
    return lv2::CELL_OK;
}

// RPCS3 sceNpCommerce2DoCheckoutStartAsync(ctx_id, const char* const* sku_ids, sku_num, container): at most 16 SKUs
// (SCE_NP_COMMERCE2_SKU_CHECKOUT_MAX), then the context.
s32 sceNpCommerce2DoCheckoutStartAsync(PPUContext& ctx, u32 ctx_id, u32 sku_ids, u32 sku_num, u32 container)
{
    const Call done = todo("no checkout dialog opens and the handler gets no event",
        "sceNpCommerce2DoCheckoutStartAsync(ctx {:#x}, sku_ids {:#x}, {} SKUs, container {:#x})", ctx_id, sku_ids,
        sku_num, container);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (sku_num > SCE_NP_COMMERCE2_SKU_CHECKOUT_MAX) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_SKU_NUM));
    std::string skus;
    if (const s32 r = check_sku_ids(ctx, "sceNpCommerce2DoCheckoutStartAsync", sku_ids, sku_num, skus)) return done(r);
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK, std::format(" (SKUs {})", skus));
}

// RPCS3 sceNpCommerce2DoDlListStartAsync(ctx_id, const char* serviceId, const char* const* sku_ids, sku_num,
// container): the download list of a service (no SKUs: the service ID is needed) or of up to 16 SKUs.
s32 sceNpCommerce2DoDlListStartAsync(PPUContext& ctx, u32 ctx_id, u32 service_id, u32 sku_ids, u32 sku_num,
    u32 container)
{
    const Call done = todo("no download list dialog opens and the handler gets no event",
        "sceNpCommerce2DoDlListStartAsync(ctx {:#x}, service {}, sku_ids {:#x}, {} SKUs, container {:#x})", ctx_id,
        text_at(ctx, service_id), sku_ids, sku_num, container);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (sku_num > SCE_NP_COMMERCE2_SKU_CHECKOUT_MAX) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_SKU_NUM));
    std::string skus;
    if (sku_num == 0) {
        if (!service_id) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    } else if (const s32 r = check_sku_ids(ctx, "sceNpCommerce2DoDlListStartAsync", sku_ids, sku_num, skus)) {
        return done(r);
    }
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK, sku_num ? std::format(" (SKUs {})", skus) : std::string());
}

// RPCS3 sceNpCommerce2DoProductCodeStartAsync(ctx_id, container, const SceNpCommerce2ProductCodeParam* param): the
// user types a code (inputMode 0), or the game gives one (inputMode 1: three blocks of four letters or digits).
s32 sceNpCommerce2DoProductCodeStartAsync(PPUContext& ctx, u32 ctx_id, u32 container, u32 param)
{
    const Call done = todo("no product code dialog opens and the handler gets no event",
        "sceNpCommerce2DoProductCodeStartAsync(ctx {:#x}, container {:#x}, param {:#x})", ctx_id, container, param);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!param) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    if (!usable(ctx, param, kProductCodeParamSize, "sceNpCommerce2DoProductCodeStartAsync: param")) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    u8* base = guest_base(ctx);
    if (ps_read32(base, param) != kProductCodeParamSize) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_SIZE));
    const u32 mode = ps_read32(base, param + kProductCodeInputMode);
    if (mode != SCE_NP_COMMERCE2_PRODUCT_CODE_INPUT_MODE_USER_INPUT) {
        if (mode != SCE_NP_COMMERCE2_PRODUCT_CODE_INPUT_MODE_CODE_SPECIFIED) {
            return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_TYPE));
        }
        const auto code = [&](u32 block, u32 i) {
            return ps_read8(base, param + kProductCode1 + kProductCodeStride * block + i);
        };
        if (!code(0, 0) || !code(1, 0) || !code(2, 0)) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID));
        for (u32 i = 0; i < SCE_NP_COMMERCE2_PRODUCT_CODE_BLOCK_LEN; i++) {
            if (!alnum(code(0, i)) || !alnum(code(1, i)) || !alnum(code(2, i))) {
                return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID));
            }
        }
    }
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK, std::format(" (input mode {})", mode));
}

// RPCS3 sceNpCommerce2DoCheckoutFinishAsync, DoDlListFinishAsync and DoProductCodeFinishAsync: the library and the
// context are checked; no dialog is open to close.
s32 finish_dialog(std::string_view fn, u32 ctx_id)
{
    const Call done = todo("no dialog to close", "{}(ctx {:#x})", fn, ctx_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK);
}

s32 sceNpCommerce2DoCheckoutFinishAsync(PPUContext&, u32 ctx_id)
{
    return finish_dialog("sceNpCommerce2DoCheckoutFinishAsync", ctx_id);
}

s32 sceNpCommerce2DoDlListFinishAsync(PPUContext&, u32 ctx_id)
{
    return finish_dialog("sceNpCommerce2DoDlListFinishAsync", ctx_id);
}

s32 sceNpCommerce2DoProductCodeFinishAsync(PPUContext&, u32 ctx_id)
{
    return finish_dialog("sceNpCommerce2DoProductCodeFinishAsync", ctx_id);
}

s32 sceNpCommerce2DoProductBrowseFinishAsync(PPUContext&, u32 ctx_id)
{
    return finish_dialog("sceNpCommerce2DoProductBrowseFinishAsync", ctx_id);
}

// libsceNpCommerce2's unnamed 0x9684D405 (D-1066): a plain branch into sceNpCommerce2DoProductBrowseFinishAsync's body,
// with no register set (scripts/nid_audit.py): the same function.
s32 sceNpCommerce2_9684D405(PPUContext&, u32 ctx_id)
{
    return finish_dialog("sceNpCommerce2_9684D405", ctx_id);
}

s32 sceNpCommerce2DoServiceListFinishAsync(PPUContext&, u32 ctx_id)
{
    return finish_dialog("sceNpCommerce2DoServiceListFinishAsync", ctx_id);
}

// RPCS3 sceNpCommerce2CreateSessionAbort: the library and the context; the session is made at once, so there is
// nothing to abort.
s32 sceNpCommerce2CreateSessionAbort(PPUContext&, u32 ctx_id)
{
    const Call done = todo("nothing to abort", "sceNpCommerce2CreateSessionAbort(ctx {:#x})", ctx_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK);
}

// The size field of an optional SceNpCommerce2ProductBrowseParam: nullopt when it isn't guest memory (logged).
std::optional<u32> browse_param_size(PPUContext& ctx, std::string_view fn, u32 param)
{
    if (!usable(ctx, param, kProductBrowseParamSize, std::format("{}: param", fn))) return std::nullopt;
    return ps_read32(guest_base(ctx), param);
}

// RPCS3 sceNpCommerce2DoProductBrowseStartAsync(ctx_id, const char* productId, container, const
// SceNpCommerce2ProductBrowseParam* param): a product ID, a param of its size when given, then the context.
s32 sceNpCommerce2DoProductBrowseStartAsync(PPUContext& ctx, u32 ctx_id, u32 product_id, u32 container, u32 param)
{
    const Call done = todo("no product browse dialog opens and the handler gets no event",
        "sceNpCommerce2DoProductBrowseStartAsync(ctx {:#x}, product {}, container {:#x}, param {:#x})", ctx_id,
        text_at(ctx, product_id), container, param);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!product_id || !usable(ctx, product_id, 1, "sceNpCommerce2DoProductBrowseStartAsync: productId") ||
        !ps_read8(guest_base(ctx), product_id)) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    if (param) {
        const std::optional<u32> size = browse_param_size(ctx, "sceNpCommerce2DoProductBrowseStartAsync", param);
        if (!size) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
        if (*size != kProductBrowseParamSize) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_SIZE));
    }
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2DoServiceListStartAsync(ctx_id, char* serviceId, container, SceNpCommerce2ProductBrowseParam*
// param): both needed, the param of its size, a service ID, then the context.
s32 sceNpCommerce2DoServiceListStartAsync(PPUContext& ctx, u32 ctx_id, u32 service_id, u32 container, u32 param)
{
    const Call done = todo("no service list dialog opens and the handler gets no event",
        "sceNpCommerce2DoServiceListStartAsync(ctx {:#x}, service {}, container {:#x}, param {:#x})", ctx_id,
        text_at(ctx, service_id), container, param);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!param || !service_id) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    const std::optional<u32> size = browse_param_size(ctx, "sceNpCommerce2DoServiceListStartAsync", param);
    if (!size) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    if (*size != kProductBrowseParamSize) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_SIZE));
    if (!usable(ctx, service_id, 1, "sceNpCommerce2DoServiceListStartAsync: serviceId")) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    if (!ps_read8(guest_base(ctx), service_id)) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID));
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2EmptyStoreCheckStart(ctx_id, store_check_type, const char* target_id): a target, the category
// check type, the context; its handler, when there is one, gets EMPTY_STORE_CHECK_DONE (subject 0, error 0) from the
// game's next cellSysutilCheckCallback. EmptyStoreCheckFinish then reports the store not empty (RPCS3); Abort checks
// only the library.
s32 sceNpCommerce2EmptyStoreCheckStart(PPUContext& ctx, u32 ctx_id, s32 store_check_type, u32 target_id)
{
    const Call done = implemented("sceNpCommerce2EmptyStoreCheckStart(ctx {:#x}, type {}, target {})", ctx_id,
        store_check_type, text_at(ctx, target_id));
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!target_id || !usable(ctx, target_id, 1, "sceNpCommerce2EmptyStoreCheckStart: target_id") ||
        !ps_read8(guest_base(ctx), target_id)) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID));
    }
    if (store_check_type != SCE_NP_COMMERCE2_STORE_CHECK_TYPE_CATEGORY) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    const std::optional<Commerce2Ctx> c = find_ctx(ctx_id);
    if (!c) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    if (!c->handler) return done(lv2::CELL_OK, "; no handler to tell");
    sysutil_queue_call([ctx_id, handler = c->handler, arg = c->arg](PPUContext& cb_ctx) -> s32 {
        call_guest(cb_ctx, handler, {ctx_id, 0, SCE_NP_COMMERCE2_EVENT_EMPTY_STORE_CHECK_DONE, 0, arg});
        return lv2::CELL_OK;
    });
    return done(lv2::CELL_OK, "; EMPTY_STORE_CHECK_DONE goes to the handler at the next cellSysutilCheckCallback");
}

s32 sceNpCommerce2EmptyStoreCheckFinish(PPUContext& ctx, u32 ctx_id, u32 is_empty)
{
    const Call done = implemented("sceNpCommerce2EmptyStoreCheckFinish(ctx {:#x}, is_empty {:#x})", ctx_id, is_empty);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!is_empty || !usable(ctx, is_empty, 4, "sceNpCommerce2EmptyStoreCheckFinish: is_empty")) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    ps_write32(guest_base(ctx), is_empty, SCE_NP_COMMERCE2_STORE_IS_NOT_EMPTY);
    return done(lv2::CELL_OK, ": the store is not empty");
}

s32 sceNpCommerce2EmptyStoreCheckAbort(PPUContext&, u32 ctx_id)
{
    const Call done = todo("nothing to abort", "sceNpCommerce2EmptyStoreCheckAbort(ctx {:#x})", ctx_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2ExecuteStoreBrowse(targetType, const char* targetId, userdata): a target type (category, product
// or product code), with a target ID except for a product code; the store doesn't open.
s32 sceNpCommerce2ExecuteStoreBrowse(PPUContext& ctx, s32 target_type, u32 target_id, s32 userdata)
{
    const Call done = todo("the store doesn't open", "sceNpCommerce2ExecuteStoreBrowse(type {}, target {}, userdata {})",
        target_type, text_at(ctx, target_id), userdata);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (target_type < SCE_NP_COMMERCE2_STORE_BROWSE_TYPE_CATEGORY ||
        target_type > SCE_NP_COMMERCE2_STORE_BROWSE_TYPE_PRODUCT_CODE) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_TYPE));
    }
    if (target_type < SCE_NP_COMMERCE2_STORE_BROWSE_TYPE_PRODUCT_CODE &&
        (!target_id || !usable(ctx, target_id, 1, "sceNpCommerce2ExecuteStoreBrowse: targetId") ||
            !ps_read8(guest_base(ctx), target_id))) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_TARGET_ID));
    }
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2GetStoreBrowseUserdata: 0 (a TODO: no store browse ever returns).
s32 sceNpCommerce2GetStoreBrowseUserdata(PPUContext& ctx, u32 userdata)
{
    const Call done = implemented("sceNpCommerce2GetStoreBrowseUserdata({:#x})", userdata);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!userdata || !usable(ctx, userdata, 4, "sceNpCommerce2GetStoreBrowseUserdata: userdata")) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    ps_write32(guest_base(ctx), userdata, 0);
    return done(lv2::CELL_OK, ": 0");
}

// RPCS3 sceNpCommerce2GetBGDLAvailability / SetBGDLAvailability: background downloads are never available (a TODO);
// setting is only checked.
s32 sceNpCommerce2GetBGDLAvailability(PPUContext& ctx, u32 availability)
{
    const Call done = implemented("sceNpCommerce2GetBGDLAvailability({:#x})", availability);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!availability || !usable(ctx, availability, 1, "sceNpCommerce2GetBGDLAvailability: bgdlAvailability")) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    ps_write8(guest_base(ctx), availability, 0);
    return done(lv2::CELL_OK, ": not available");
}

s32 sceNpCommerce2SetBGDLAvailability(PPUContext&, bool availability)
{
    const Call done = todo("nothing changes", "sceNpCommerce2SetBGDLAvailability({})", availability);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2GetProductInfoListCreateReq(ctx_id, u32* req_id): unlike the other requests, the context is
// looked at.
s32 sceNpCommerce2GetProductInfoListCreateReq(PPUContext&, u32 ctx_id, u32 req_id)
{
    const Call done = todo("no request is made and *req_id is not written",
        "sceNpCommerce2GetProductInfoListCreateReq(ctx {:#x}, req_id {:#x})", ctx_id, req_id);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!req_id) return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    if (!find_ctx(ctx_id)) return done(error(SCE_NP_COMMERCE2_ERROR_CTX_NOT_FOUND));
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpCommerce2GetProductInfoListStart(req_id, const char** productIds, productNum): 1 to 60 products.
s32 sceNpCommerce2GetProductInfoListStart(PPUContext&, u32 req_id, u32 product_ids, u32 product_num)
{
    const Call done = todo("nothing is asked of the store",
        "sceNpCommerce2GetProductInfoListStart(req {:#x}, products {:#x}, {})", req_id, product_ids, product_num);
    if (!initialized()) return done(error(SCE_NP_COMMERCE2_ERROR_NOT_INITIALIZED));
    if (!product_ids || !product_num || product_num > SCE_NP_COMMERCE2_GETPRODLIST_MAX_COUNT) {
        return done(error(SCE_NP_COMMERCE2_ERROR_INVALID_ARGUMENT));
    }
    return done(lv2::CELL_OK);
}

s32 sceNpCommerce2GetProductInfoListGetResult(PPUContext&, u32 req_id, u32 buf, u32 buf_size, u32 fill_size)
{
    return get_result("sceNpCommerce2GetProductInfoListGetResult", req_id, buf, buf_size, fill_size);
}

s32 sceNpCommerce2InitGetProductInfoListResult(PPUContext&, u32 result, u32 data, u32 data_size)
{
    return init_result("sceNpCommerce2InitGetProductInfoListResult", result, data, data_size);
}

// RPCS3's other getters of result data (TODOs): both pointers, nothing written.
s32 sceNpCommerce2GetGameProductInfoFromGetProductInfoListResult(PPUContext&, u32 result, u32 index,
    u32 game_product_info)
{
    return get_info(todo("*gameProductInfo is not written",
                        "sceNpCommerce2GetGameProductInfoFromGetProductInfoListResult(result {:#x}, {}, {:#x})", result,
                        index, game_product_info),
        result, game_product_info);
}

s32 sceNpCommerce2GetCategoryInfoFromContentInfo(PPUContext&, u32 content_info, u32 category_info)
{
    return get_info(todo("*categoryInfo is not written", "sceNpCommerce2GetCategoryInfoFromContentInfo({:#x}, {:#x})",
                        content_info, category_info),
        content_info, category_info);
}

s32 sceNpCommerce2GetContentRatingInfoFromCategoryInfo(PPUContext&, u32 category_info, u32 rating_info)
{
    return get_info(todo("*contentRatingInfo is not written",
                        "sceNpCommerce2GetContentRatingInfoFromCategoryInfo({:#x}, {:#x})", category_info, rating_info),
        category_info, rating_info);
}

s32 sceNpCommerce2GetContentRatingInfoFromGameProductInfo(PPUContext&, u32 game_product_info, u32 rating_info)
{
    return get_info(todo("*contentRatingInfo is not written",
                        "sceNpCommerce2GetContentRatingInfoFromGameProductInfo({:#x}, {:#x})", game_product_info,
                        rating_info),
        game_product_info, rating_info);
}

s32 sceNpCommerce2GetContentRatingDescriptor(PPUContext&, u32 rating_info, u32 index, u32 descriptor)
{
    return get_info(todo("*contentRatingDescriptor is not written",
                        "sceNpCommerce2GetContentRatingDescriptor({:#x}, {}, {:#x})", rating_info, index, descriptor),
        rating_info, descriptor);
}

} // namespace

void register_sceNpCommerce2(Registry& r)
{
    r.add("sceNpCommerce2", {
        PS_HLE_FUNC(0x3539D233, sceNpCommerce2Init),
        PS_HLE_FUNC(0x4D4A094C, sceNpCommerce2Term),
        PS_HLE_FUNC(0xD9FDCEC2, sceNpCommerce2CreateCtx),
        PS_HLE_FUNC(0x6F67EA80, sceNpCommerce2DestroyCtx),
        PS_HLE_FUNC(0xCC18CD2C, sceNpCommerce2CreateSessionStart),
        PS_HLE_FUNC(0x91F8843D, sceNpCommerce2CreateSessionFinish),
        PS_HLE_FUNC(0x7370D8D0, sceNpCommerce2GetCategoryContentsCreateReq),
        PS_HLE_FUNC(0x371A2EDD, sceNpCommerce2GetCategoryContentsStart),
        PS_HLE_FUNC(0xCA0EA996, sceNpCommerce2GetCategoryContentsGetResult),
        PS_HLE_FUNC(0xD8A473A3, sceNpCommerce2InitGetCategoryContentsResult),
        PS_HLE_FUNC(0xBD49EAB2, sceNpCommerce2GetCategoryInfo),
        PS_HLE_FUNC(0x972AB46C, sceNpCommerce2GetContentInfo),
        PS_HLE_FUNC(0xE51A4944, sceNpCommerce2GetGameProductInfoFromContentInfo),
        PS_HLE_FUNC(0x9D9CB96B, sceNpCommerce2DestroyGetCategoryContentsResult),
        PS_HLE_FUNC(0xA975EBB4, sceNpCommerce2GetProductInfoCreateReq),
        PS_HLE_FUNC(0x8F46325B, sceNpCommerce2GetProductInfoStart),
        PS_HLE_FUNC(0xBF5F58EA, sceNpCommerce2GetProductInfoGetResult),
        PS_HLE_FUNC(0xF798F5E3, sceNpCommerce2InitGetProductInfoResult),
        PS_HLE_FUNC(0xEF645654, sceNpCommerce2GetGameProductInfo),
        PS_HLE_FUNC(0xEF8EAFCD, sceNpCommerce2DestroyGetProductInfoResult),
        PS_HLE_FUNC(0xF6139B58, sceNpCommerce2DestroyGetProductInfoListResult),
        PS_HLE_FUNC(0xDB19194C, sceNpCommerce2GetGameSkuInfoFromGameProductInfo),
        PS_HLE_FUNC(0xDA8E322D, sceNpCommerce2GetPrice),
        PS_HLE_FUNC(0x8DF0057F, sceNpCommerce2AbortReq),
        PS_HLE_FUNC(0x2A910F05, sceNpCommerce2DestroyReq),
        PS_HLE_FUNC(0x104551A6, sceNpCommerce2DoCheckoutStartAsync),
        PS_HLE_FUNC(0xD43A130E, sceNpCommerce2DoCheckoutFinishAsync),
        PS_HLE_FUNC(0x6CA9EFD4, sceNpCommerce2DoDlListStartAsync),
        PS_HLE_FUNC(0x410D42BE, sceNpCommerce2DoDlListFinishAsync),
        PS_HLE_FUNC(0xDE7AB33D, sceNpCommerce2DoProductCodeStartAsync),
        PS_HLE_FUNC(0xA9F945B3, sceNpCommerce2DoProductCodeFinishAsync),
        PS_HLE_FUNC(0xB23E3BD1, sceNpCommerce2DoProductBrowseFinishAsync),
        PS_HLE_FUNC(0x9684D405, sceNpCommerce2_9684D405),
        PS_HLE_FUNC(0x9050617D, sceNpCommerce2DoServiceListFinishAsync),
        PS_HLE_FUNC(0x62023E98, sceNpCommerce2CreateSessionAbort),
        PS_HLE_FUNC(0x9825A0FC, sceNpCommerce2DoProductBrowseStartAsync),
        PS_HLE_FUNC(0xC7AFC42A, sceNpCommerce2DoServiceListStartAsync),
        PS_HLE_FUNC(0x542B74CC, sceNpCommerce2EmptyStoreCheckStart),
        PS_HLE_FUNC(0xC1EB3B92, sceNpCommerce2EmptyStoreCheckFinish),
        PS_HLE_FUNC(0x1C153E5C, sceNpCommerce2EmptyStoreCheckAbort),
        PS_HLE_FUNC(0xEEF51BE0, sceNpCommerce2ExecuteStoreBrowse),
        PS_HLE_FUNC(0x1FA1B312, sceNpCommerce2GetStoreBrowseUserdata),
        PS_HLE_FUNC(0x3D627D81, sceNpCommerce2GetBGDLAvailability),
        PS_HLE_FUNC(0xA5A863FE, sceNpCommerce2SetBGDLAvailability),
        PS_HLE_FUNC(0xE1E7B5AC, sceNpCommerce2GetProductInfoListCreateReq),
        PS_HLE_FUNC(0x9CDE07CC, sceNpCommerce2GetProductInfoListStart),
        PS_HLE_FUNC(0x146618DF, sceNpCommerce2GetProductInfoListGetResult),
        PS_HLE_FUNC(0xE0F90E44, sceNpCommerce2InitGetProductInfoListResult),
        PS_HLE_FUNC(0xD9956CE7, sceNpCommerce2GetGameProductInfoFromGetProductInfoListResult),
        PS_HLE_FUNC(0xFC216890, sceNpCommerce2GetCategoryInfoFromContentInfo),
        PS_HLE_FUNC(0xAC78C1F3, sceNpCommerce2GetContentRatingInfoFromCategoryInfo),
        PS_HLE_FUNC(0xEC324C8F, sceNpCommerce2GetContentRatingInfoFromGameProductInfo),
        PS_HLE_FUNC(0x150FDCA3, sceNpCommerce2GetContentRatingDescriptor),
    });
}

} // namespace pspocket::hle
