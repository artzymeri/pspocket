// sceNpCommerce2 and sceNpSns offline (synthetic: no game data): RPCS3's checks and results with no user signed in to
// PSN. The store library's init on top of NP, its one context and the session it creates at once (the handler's event
// delivered to guest code by cellSysutilCheckCallback), requests, results and store dialogs that check their arguments
// and do nothing; Facebook handles in idm order, and access tokens refused because nobody is signed in.

#include "hle_test_util.hpp"

#include <cstring>
#include <string_view>
#include <vector>

#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

constexpr u64 kNpNotInit = 0xFFFFFFFF8002AA01ull;
constexpr u64 kNotInit = 0xFFFFFFFF80023001ull, kAlready = 0xFFFFFFFF80023002ull, kInval = 0xFFFFFFFF80023003ull,
              kBadVersion = 0xFFFFFFFF80023004ull, kCtxMax = 0xFFFFFFFF80023005ull,
              kBadSkuId = 0xFFFFFFFF80023007ull, kBadSkuNum = 0xFFFFFFFF80023008ull,
              kCtxNotFound = 0xFFFFFFFF8002300Cull, kBadResultData = 0xFFFFFFFF80023014ull,
              kMaintenance = 0xFFFFFFFF80023016ull, kBufTooSmall = 0xFFFFFFFF80023018ull,
              kBadTargetType = 0xFFFFFFFF8002301Aull, kBadTargetId = 0xFFFFFFFF8002301Bull,
              kBadSize = 0xFFFFFFFF8002301Cull;
constexpr u64 kSnsNotSignIn = 0xFFFFFFFF80024502ull, kSnsInval = 0xFFFFFFFF80024503ull,
              kFbAlready = 0xFFFFFFFF80024511ull, kFbNotInit = 0xFFFFFFFF80024512ull,
              kFbExceeds = 0xFFFFFFFF80024513ull, kFbUnknown = 0xFFFFFFFF80024514ull;

constexpr u32 kPool = kData + 0x40000;   // NP's 128 KB pool
constexpr u32 kRecvBufSize = 256 * 1024; // SCE_NP_COMMERCE2_RECV_BUF_SIZE

void np_up(GuestThread& g) { REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0); }

void put(u8* b, u32 addr, std::string_view s)
{
    std::memcpy(b + addr, s.data(), s.size());
    b[addr + s.size()] = 0;
}

bool all_bytes(const u8* p, usize n, u8 value)
{
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

// Guest functions without lifted code run in the interpreter while this lives (the default policy aborts).
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// A guest commerce handler(ctx_id, subject_id, event, error_code, arg) behind the descriptor kOpd. Each call appends
// r3..r7 (doublewords) to a log at kLog + 16, 48 bytes an entry, and counts the entries at kLog.
constexpr u32 kCode = kData + 0x10000, kOpd = kData + 0x10800, kLog = kData + 0x11000, kToc = 0x00123400;

void write_handler(u8* base)
{
    const std::vector<u32> code = {
        as::lis(9, static_cast<s32>(kLog >> 16)),
        as::ori(9, 9, kLog & 0xFFFF),
        as::lwz(10, 0, 9),
        as::mulli(11, 10, 48),
        as::addi(11, 11, 16),
        as::add(11, 11, 9),
        as::std_(3, 0, 11),
        as::std_(4, 8, 11),
        as::std_(5, 16, 11),
        as::std_(6, 24, 11),
        as::std_(7, 32, 11),
        as::addi(10, 10, 1),
        as::stw(10, 0, 9),
        as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(base, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(base, kOpd, kCode);
    ps_write32(base, kOpd + 4, kToc);
    std::memset(base + kLog, 0, 0x200);
}

u32 handler_calls(u8* b) { return ps_read32(b, kLog); }
// Argument `reg` (0 = r3 .. 4 = r7) of handler call `call`.
u64 handler_arg(u8* b, u32 call, u32 reg) { return ps_read64(b, kLog + 16 + 48 * call + 8 * reg); }

// A SceNpCommerce2ProductCodeParam (32 bytes): size, inputMode, three char[5] blocks at 8, 16 and 24.
void put_code_param(u8* b, u32 addr, u32 size, u32 mode, std::string_view c1, std::string_view c2, std::string_view c3)
{
    std::memset(b + addr, 0, 32);
    ps_write32(b, addr, size);
    ps_write32(b, addr + 4, mode);
    put(b, addr + 8, c1);
    put(b, addr + 16, c2);
    put(b, addr + 24, c3);
}

} // namespace

TEST_CASE("sceNpCommerce2, sceNpSns: the functions GTA V imports, in their modules", "[hle][np]")
{
    struct Expected {
        std::string_view module, name;
        u32 nid;
    };
    const Expected expected[] = {
        {"sceNpCommerce2", "sceNpCommerce2DoCheckoutStartAsync", 0x104551A6},
        {"sceNpCommerce2", "sceNpCommerce2DestroyReq", 0x2A910F05},
        {"sceNpCommerce2", "sceNpCommerce2Init", 0x3539D233},
        {"sceNpCommerce2", "sceNpCommerce2GetCategoryContentsStart", 0x371A2EDD},
        {"sceNpCommerce2", "sceNpCommerce2DoDlListFinishAsync", 0x410D42BE},
        {"sceNpCommerce2", "sceNpCommerce2Term", 0x4D4A094C},
        {"sceNpCommerce2", "sceNpCommerce2DoDlListStartAsync", 0x6CA9EFD4},
        {"sceNpCommerce2", "sceNpCommerce2DestroyCtx", 0x6F67EA80},
        {"sceNpCommerce2", "sceNpCommerce2GetCategoryContentsCreateReq", 0x7370D8D0},
        {"sceNpCommerce2", "sceNpCommerce2AbortReq", 0x8DF0057F},
        {"sceNpCommerce2", "sceNpCommerce2GetProductInfoStart", 0x8F46325B},
        {"sceNpCommerce2", "sceNpCommerce2CreateSessionFinish", 0x91F8843D},
        {"sceNpCommerce2", "sceNpCommerce2GetContentInfo", 0x972AB46C},
        {"sceNpCommerce2", "sceNpCommerce2DestroyGetCategoryContentsResult", 0x9D9CB96B},
        {"sceNpCommerce2", "sceNpCommerce2GetProductInfoCreateReq", 0xA975EBB4},
        {"sceNpCommerce2", "sceNpCommerce2DoProductCodeFinishAsync", 0xA9F945B3},
        {"sceNpCommerce2", "sceNpCommerce2GetCategoryInfo", 0xBD49EAB2},
        {"sceNpCommerce2", "sceNpCommerce2GetProductInfoGetResult", 0xBF5F58EA},
        {"sceNpCommerce2", "sceNpCommerce2GetCategoryContentsGetResult", 0xCA0EA996},
        {"sceNpCommerce2", "sceNpCommerce2CreateSessionStart", 0xCC18CD2C},
        {"sceNpCommerce2", "sceNpCommerce2DoCheckoutFinishAsync", 0xD43A130E},
        {"sceNpCommerce2", "sceNpCommerce2InitGetCategoryContentsResult", 0xD8A473A3},
        {"sceNpCommerce2", "sceNpCommerce2CreateCtx", 0xD9FDCEC2},
        {"sceNpCommerce2", "sceNpCommerce2GetPrice", 0xDA8E322D},
        {"sceNpCommerce2", "sceNpCommerce2GetGameSkuInfoFromGameProductInfo", 0xDB19194C},
        {"sceNpCommerce2", "sceNpCommerce2DoProductCodeStartAsync", 0xDE7AB33D},
        {"sceNpCommerce2", "sceNpCommerce2GetGameProductInfoFromContentInfo", 0xE51A4944},
        {"sceNpCommerce2", "sceNpCommerce2GetGameProductInfo", 0xEF645654},
        {"sceNpCommerce2", "sceNpCommerce2DestroyGetProductInfoResult", 0xEF8EAFCD},
        {"sceNpCommerce2", "sceNpCommerce2InitGetProductInfoResult", 0xF798F5E3},
        {"sceNpSns", "sceNpSnsFbInit", 0x2C0F3548},
        {"sceNpSns", "sceNpSnsFbAbortHandle", 0x3711833F},
        {"sceNpSns", "sceNpSnsFbDestroyHandle", 0x6E42C0DD},
        {"sceNpSns", "sceNpSnsFbCreateHandle", 0x8FD1D549},
        {"sceNpSns", "sceNpSnsFbGetLongAccessToken", 0xBA8C569E},
        {"sceNpSns", "sceNpSnsFbTerm", 0xC05545FD},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.module << "::" << e.name);
        const hle::Function* fn = hle::registry().find(e.module, e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
    }
}

TEST_CASE("sceNpCommerce2: init needs NP; nothing else works before it", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "store");
    constexpr u32 kNpId = kData + 0x100, kCtx = kData + 0x140, kReq = kData + 0x144, kFill = kData + 0x148,
                  kStr = kData + 0x180, kOut1 = kData + 0x200, kOut2 = kData + 0x400, kParam = kData + 0x600;

    CHECK(g.call("sceNpCommerce2Init") == kNpNotInit); // NP isn't up (RPCS3 is_NP_init)

    // Before init, everything but Term and the result destructors.
    CHECK(g.call("sceNpCommerce2CreateCtx", 2, kNpId, 0, 0, kCtx) == kNotInit);
    CHECK(g.call("sceNpCommerce2DestroyCtx", 0x5001) == kNotInit);
    CHECK(g.call("sceNpCommerce2CreateSessionStart", 0x5001) == kNotInit);
    CHECK(g.call("sceNpCommerce2CreateSessionFinish", 0x5001, kOut1) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsCreateReq", 0x5001, kReq) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsStart", 1, kStr, 0, 10) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsGetResult", 1, kOut1, kRecvBufSize, kFill) == kNotInit);
    CHECK(g.call("sceNpCommerce2InitGetCategoryContentsResult", kOut1, kOut2, 100) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetCategoryInfo", kOut1, kOut2) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetContentInfo", kOut1, 0, kOut2) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetGameProductInfoFromContentInfo", kOut1, kOut2) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetProductInfoCreateReq", 0x5001, kReq) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetProductInfoStart", 1, kStr, kStr) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetProductInfoGetResult", 1, kOut1, kRecvBufSize, kFill) == kNotInit);
    CHECK(g.call("sceNpCommerce2InitGetProductInfoResult", kOut1, kOut2, 100) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetGameProductInfo", kOut1, kOut2) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetGameSkuInfoFromGameProductInfo", kOut1, 0, kOut2) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetPrice", 0x5001, kOut1, 16, 100) == kNotInit);
    CHECK(g.call("sceNpCommerce2AbortReq", 1) == kNotInit);
    CHECK(g.call("sceNpCommerce2DestroyReq", 1) == kNotInit);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, 0, 0, 0) == kNotInit);
    CHECK(g.call("sceNpCommerce2DoCheckoutFinishAsync", 0x5001) == kNotInit);
    CHECK(g.call("sceNpCommerce2DoDlListStartAsync", 0x5001, kStr, 0, 0, 0) == kNotInit);
    CHECK(g.call("sceNpCommerce2DoDlListFinishAsync", 0x5001) == kNotInit);
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == kNotInit);
    CHECK(g.call("sceNpCommerce2DoProductCodeFinishAsync", 0x5001) == kNotInit);
    CHECK(g.call("sceNpCommerce2DestroyGetCategoryContentsResult", kOut1) == 0);
    CHECK(g.call("sceNpCommerce2DestroyGetProductInfoResult", kOut1) == 0);
    CHECK(g.call("sceNpCommerce2Term") == 0); // in any state

    np_up(g);
    REQUIRE(g.call("sceNpCommerce2Init") == 0);
    CHECK(g.call("sceNpCommerce2Init") == kAlready);
    REQUIRE(g.call("sceNpCommerce2Term") == 0);
    CHECK(g.call("sceNpCommerce2AbortReq", 1) == kNotInit);
    REQUIRE(g.call("sceNpCommerce2Init") == 0);

    // NP's term leaves the library up (already initialized is checked before NP), but it can't start again without NP.
    REQUIRE(g.call("sceNp2Term") == 0);
    CHECK(g.call("sceNpCommerce2AbortReq", 1) == 0);
    CHECK(g.call("sceNpCommerce2Init") == kAlready);
    REQUIRE(g.call("sceNpCommerce2Term") == 0);
    CHECK(g.call("sceNpCommerce2Init") == kNpNotInit);
}

TEST_CASE("sceNpCommerce2: one context; the session is created at once, with no currency", "[hle][np]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread g(f, 0, "store");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    write_handler(b);
    constexpr u32 kNpId = kData + 0x100, kCtx = kData + 0x140, kInfo = kData + 0x200;
    constexpr u32 kArg = 0xCAFE0000u;
    std::memset(b + kNpId, 0, 0x24);
    put(b, kNpId, "player-one");

    np_up(g);
    REQUIRE(g.call("sceNpCommerce2Init") == 0);

    // The version (at most 2) and the NP ID are checked; without ctx_id no context is made, and that is CELL_OK.
    ps_write32(b, kCtx, 0xAAAAAAAA);
    CHECK(g.call("sceNpCommerce2CreateCtx", 3, kNpId, kOpd, kArg, kCtx) == kBadVersion);
    CHECK(g.call("sceNpCommerce2CreateCtx", 2, 0, kOpd, kArg, kCtx) == kInval);
    CHECK(g.call("sceNpCommerce2CreateCtx", 2, kUnmapped, kOpd, kArg, kCtx) == kInval); // RPCS3 faults
    CHECK(g.call("sceNpCommerce2CreateCtx", 2, kNpId, kOpd, kArg, kUnmapped) == kInval); // RPCS3 faults
    CHECK(g.call("sceNpCommerce2CreateCtx", 2, kNpId, kOpd, kArg, 0) == 0);
    CHECK(ps_read32(b, kCtx) == 0xAAAAAAAA);

    // The one context is 0x5001. Another is refused while it exists (RPCS3: ID 0 and CELL_OK), before the NP ID.
    REQUIRE(g.call("sceNpCommerce2CreateCtx", 2, kNpId, kOpd, kArg, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == 0x5001);
    ps_write32(b, kCtx, 0xAAAAAAAA);
    CHECK(g.call("sceNpCommerce2CreateCtx", 1, kNpId, 0, 0, kCtx) == kCtxMax);
    CHECK(g.call("sceNpCommerce2CreateCtx", 2, 0, 0, 0, kCtx) == kCtxMax);
    CHECK(ps_read32(b, kCtx) == 0xAAAAAAAA);

    // The session: the handler gets (ctx, 0, CREATE_SESSION_DONE, error 0, arg) at the next cellSysutilCheckCallback.
    CHECK(g.call("sceNpCommerce2CreateSessionStart", 0x5002) == kCtxNotFound);
    REQUIRE(g.call("sceNpCommerce2CreateSessionStart", 0x5001) == 0);
    CHECK(handler_calls(b) == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    REQUIRE(handler_calls(b) == 1);
    CHECK(handler_arg(b, 0, 0) == 0x5001);
    CHECK(handler_arg(b, 0, 1) == 0);
    CHECK(handler_arg(b, 0, 2) == 0x0011); // SCE_NP_COMMERCE2_EVENT_CREATE_SESSION_DONE
    CHECK(handler_arg(b, 0, 3) == 0);
    CHECK(handler_arg(b, 0, 4) == kArg);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(handler_calls(b) == 1); // once

    // Lookups see only the ID's low 16 bits (RPCS3 get_commerce2_context(u16)); the handler gets the ID as given.
    // Destroying compares all 32.
    REQUIRE(g.call("sceNpCommerce2CreateSessionStart", 0x10005001) == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    REQUIRE(handler_calls(b) == 2);
    CHECK(handler_arg(b, 1, 0) == 0x10005001);
    CHECK(g.call("sceNpCommerce2DestroyCtx", 0x10005001) == kCtxNotFound);

    // The session info (48 bytes): RPCS3 zeroes the first 44 for a game built with SDK 3.0 or later (3.6 here).
    std::memset(b + kInfo, 0xAA, 64);
    CHECK(g.call("sceNpCommerce2CreateSessionFinish", 0x5002, kInfo) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2CreateSessionFinish", 0x5001, kUnmapped) == kInval); // RPCS3 faults
    CHECK(all_bytes(b + kInfo, 64, 0xAA));
    REQUIRE(g.call("sceNpCommerce2CreateSessionFinish", 0x5001, kInfo) == 0);
    CHECK(all_bytes(b + kInfo, 44, 0));
    CHECK(all_bytes(b + kInfo + 44, 20, 0xAA));

    // Destroyed, the context's ID comes back. A context without a handler gets no event.
    REQUIRE(g.call("sceNpCommerce2DestroyCtx", 0x5001) == 0);
    CHECK(g.call("sceNpCommerce2DestroyCtx", 0x5001) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2CreateSessionStart", 0x5001) == kCtxNotFound);
    REQUIRE(g.call("sceNpCommerce2CreateCtx", 2, kNpId, 0, 0, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == 0x5001);
    REQUIRE(g.call("sceNpCommerce2CreateSessionStart", 0x5001) == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(handler_calls(b) == 2);

    // Term keeps the context, as RPCS3.
    REQUIRE(g.call("sceNpCommerce2Term") == 0);
    REQUIRE(g.call("sceNpCommerce2Init") == 0);
    CHECK(g.call("sceNpCommerce2CreateCtx", 2, kNpId, 0, 0, kCtx) == kCtxMax);
    CHECK(g.call("sceNpCommerce2DestroyCtx", 0x5001) == 0);
}

TEST_CASE("sceNpCommerce2CreateSessionFinish: a game built before SDK 3.0 gets 8 bytes cleared", "[hle][np]")
{
    lv2::Config old_sdk;
    old_sdk.sdk_version = 0x00270001; // SDK 2.70
    Fixture f({}, old_sdk);
    GuestThread g(f, 0, "store");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kCtx = kData + 0x140, kInfo = kData + 0x200;
    std::memset(b + kNpId, 0, 0x24);
    np_up(g);
    REQUIRE(g.call("sceNpCommerce2Init") == 0);
    REQUIRE(g.call("sceNpCommerce2CreateCtx", 2, kNpId, 0, 0, kCtx) == 0);
    std::memset(b + kInfo, 0xAA, 64);
    REQUIRE(g.call("sceNpCommerce2CreateSessionFinish", ps_read32(b, kCtx), kInfo) == 0);
    CHECK(all_bytes(b + kInfo, 8, 0)); // currencyCode, decimals
    CHECK(all_bytes(b + kInfo + 8, 56, 0xAA));
}

TEST_CASE("sceNpCommerce2: requests, results and store dialogs check their arguments, then do nothing", "[hle][np]")
{
    InterpretMisses interpret; // in case a dialog sent the handler an event (none may)
    Fixture f;
    GuestThread g(f, 0, "store");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    write_handler(b);
    constexpr u32 kNpId = kData + 0x100, kCtx = kData + 0x140, kReq = kData + 0x144, kFill = kData + 0x148,
                  kCategory = kData + 0x180, kProduct = kData + 0x1C0, kService = kData + 0x200,
                  kSkus = kData + 0x240, kSku1 = kData + 0x280, kSku2 = kData + 0x2C0, kEmpty = kData + 0x2FF,
                  kResult = kData + 0x300, kResultData = kData + 0x400, kOut = kData + 0x500,
                  kParam = kData + 0x600, kBuf = kData + 0x20000;
    std::memset(b + kNpId, 0, 0x24);
    put(b, kCategory, "CATEGORY_01");
    put(b, kProduct, "UP0000-NPXX00000_00-PRODUCT0000001");
    put(b, kService, "UP0000-NPXX00000_00");
    put(b, kSku1, "SKU-ONE");
    put(b, kSku2, "SKU-TWO");
    b[kEmpty] = 0;
    std::memset(b + kResult, 0xAA, 64);
    std::memset(b + kOut, 0xAA, 64);

    np_up(g);
    REQUIRE(g.call("sceNpCommerce2Init") == 0);
    REQUIRE(g.call("sceNpCommerce2CreateCtx", 2, kNpId, kOpd, 0x1234, kCtx) == 0);
    REQUIRE(ps_read32(b, kCtx) == 0x5001);

    // Category contents. The request ID is never written and the context isn't looked at; the category is needed, and
    // at most 60 results.
    ps_write32(b, kReq, 0xAAAAAAAA);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsCreateReq", 0x5001, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsCreateReq", 0x7777, kReq) == 0);
    CHECK(ps_read32(b, kReq) == 0xAAAAAAAA);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsStart", 1, 0, 0, 10) == kInval);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsStart", 1, kCategory, 0, 61) == kInval);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsStart", 1, kCategory, 20, 60) == 0);
    // The result: a 256 KB buffer first, then every pointer; nothing is received.
    ps_write32(b, kFill, 0xAAAAAAAA);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsGetResult", 1, 0, kRecvBufSize - 1, 0) == kBufTooSmall);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsGetResult", 1, 0, kRecvBufSize, kFill) == kInval);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsGetResult", 1, kBuf, kRecvBufSize, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetCategoryContentsGetResult", 1, kBuf, kRecvBufSize, kFill) == 0);
    CHECK(ps_read32(b, kFill) == 0xAAAAAAAA);
    // Result data can't be parsed (a TODO in RPCS3): invalid whatever its size, and the result isn't written.
    CHECK(g.call("sceNpCommerce2InitGetCategoryContentsResult", 0, kResultData, 100) == kInval);
    CHECK(g.call("sceNpCommerce2InitGetCategoryContentsResult", kResult, 0, 100) == kInval);
    CHECK(g.call("sceNpCommerce2InitGetCategoryContentsResult", kResult, kResultData, 0) == kInval);
    CHECK(g.call("sceNpCommerce2InitGetCategoryContentsResult", kResult, kResultData, 59) == kBadResultData);
    CHECK(g.call("sceNpCommerce2InitGetCategoryContentsResult", kResult, kResultData, 60) == kBadResultData);
    // Category info is RPCS3's "server maintenance"; content info and product info from it aren't written.
    CHECK(g.call("sceNpCommerce2GetCategoryInfo", 0, kOut) == kInval);
    CHECK(g.call("sceNpCommerce2GetCategoryInfo", kResult, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetCategoryInfo", kResult, kOut) == kMaintenance);
    CHECK(g.call("sceNpCommerce2GetContentInfo", 0, 0, kOut) == kInval);
    CHECK(g.call("sceNpCommerce2GetContentInfo", kResult, 0, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetContentInfo", kResult, 5, kOut) == 0);
    CHECK(g.call("sceNpCommerce2GetGameProductInfoFromContentInfo", 0, kOut) == kInval);
    CHECK(g.call("sceNpCommerce2GetGameProductInfoFromContentInfo", kResult, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetGameProductInfoFromContentInfo", kResult, kOut) == 0);
    CHECK(g.call("sceNpCommerce2DestroyGetCategoryContentsResult", 0) == 0);

    // Product info: the request needs the context; the start needs a product ID, not a category.
    CHECK(g.call("sceNpCommerce2GetProductInfoCreateReq", 0x5001, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetProductInfoCreateReq", 0x5002, kReq) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2GetProductInfoCreateReq", 0x5001, kReq) == 0);
    CHECK(ps_read32(b, kReq) == 0xAAAAAAAA);
    CHECK(g.call("sceNpCommerce2GetProductInfoStart", 1, kCategory, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetProductInfoStart", 1, 0, kProduct) == 0);
    CHECK(g.call("sceNpCommerce2GetProductInfoGetResult", 1, kBuf, kRecvBufSize - 1, kFill) == kBufTooSmall);
    CHECK(g.call("sceNpCommerce2GetProductInfoGetResult", 1, kBuf, kRecvBufSize, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetProductInfoGetResult", 1, kBuf, kRecvBufSize, kFill) == 0);
    CHECK(ps_read32(b, kFill) == 0xAAAAAAAA);
    CHECK(g.call("sceNpCommerce2InitGetProductInfoResult", kResult, kResultData, 0) == kInval);
    CHECK(g.call("sceNpCommerce2InitGetProductInfoResult", kResult, kResultData, 4096) == kBadResultData);
    CHECK(g.call("sceNpCommerce2GetGameProductInfo", kResult, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetGameProductInfo", kResult, kOut) == 0);
    CHECK(g.call("sceNpCommerce2GetGameSkuInfoFromGameProductInfo", 0, 0, kOut) == kInval);
    CHECK(g.call("sceNpCommerce2GetGameSkuInfoFromGameProductInfo", kResult, 0, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetGameSkuInfoFromGameProductInfo", kResult, 0, kOut) == 0);
    CHECK(g.call("sceNpCommerce2DestroyGetProductInfoResult", kResult) == 0);

    // A price: only the context is checked, nothing is written.
    CHECK(g.call("sceNpCommerce2GetPrice", 0x5002, kOut, 16, 499) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2GetPrice", 0x5001, kOut, 16, 499) == 0);
    CHECK(all_bytes(b + kResult, 64, 0xAA));
    CHECK(all_bytes(b + kOut, 64, 0xAA));

    // Aborting and destroying a request need only the library.
    CHECK(g.call("sceNpCommerce2AbortReq", 0x7777) == 0);
    CHECK(g.call("sceNpCommerce2DestroyReq", 0x7777) == 0);

    // Checkout: at most 16 SKUs, each a non-empty string, then the context.
    ps_write32(b, kSkus + 0, kSku1);
    ps_write32(b, kSkus + 4, kSku2);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, kSkus, 17, 0) == kBadSkuNum);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, 0, 1, 0) == kBadSkuId);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, kUnmapped, 1, 0) == kBadSkuId); // RPCS3 faults
    ps_write32(b, kSkus + 8, 0);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, kSkus, 3, 0) == kBadSkuId);
    ps_write32(b, kSkus + 8, kEmpty);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, kSkus, 3, 0) == kBadSkuId);
    ps_write32(b, kSkus + 8, kUnmapped);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, kSkus, 3, 0) == kBadSkuId); // RPCS3 faults
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5002, kSkus, 2, 0) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, kSkus, 2, 0) == 0);
    CHECK(g.call("sceNpCommerce2DoCheckoutStartAsync", 0x5001, 0, 0, 0) == 0); // no SKUs: the list isn't looked at
    CHECK(g.call("sceNpCommerce2DoCheckoutFinishAsync", 0x5002) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2DoCheckoutFinishAsync", 0x5001) == 0);

    // The download list of a service (no SKUs: its ID is needed) or of up to 16 SKUs.
    CHECK(g.call("sceNpCommerce2DoDlListStartAsync", 0x5001, kService, kSkus, 17, 0) == kBadSkuNum);
    CHECK(g.call("sceNpCommerce2DoDlListStartAsync", 0x5001, 0, 0, 0, 0) == kInval);
    CHECK(g.call("sceNpCommerce2DoDlListStartAsync", 0x5001, kService, 0, 0, 0) == 0);
    CHECK(g.call("sceNpCommerce2DoDlListStartAsync", 0x5001, kService, kSkus, 3, 0) == kBadSkuId);
    CHECK(g.call("sceNpCommerce2DoDlListStartAsync", 0x5001, 0, kSkus, 2, 0) == 0); // SKUs: no service needed
    CHECK(g.call("sceNpCommerce2DoDlListStartAsync", 0x5002, 0, kSkus, 2, 0) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2DoDlListFinishAsync", 0x5002) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2DoDlListFinishAsync", 0x5001) == 0);

    // Product codes: the struct's size is 32; a code the game gives is three blocks of four ASCII letters or digits.
    put_code_param(b, kParam, 32, 1, "AB12", "CD34", "EF56");
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, 0) == kInval);
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kUnmapped) == kInval); // RPCS3 faults
    ps_write32(b, kParam, 28);
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == kBadSize);
    ps_write32(b, kParam, 32);
    ps_write32(b, kParam + 4, 2);
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == kBadTargetType);
    ps_write32(b, kParam + 4, 1);
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5002, 0, kParam) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == 0);
    put(b, kParam + 16, "");
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == kBadTargetId);
    put(b, kParam + 16, "CD3"); // three characters
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == kBadTargetId);
    put(b, kParam + 16, "CD-4");
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == kBadTargetId);
    put(b, kParam + 16, "cd34");
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == 0);
    b[kParam + 25] = 0xC9; // not ASCII
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == kBadTargetId);
    ps_write32(b, kParam + 4, 0); // the user types the code: the blocks aren't looked at
    CHECK(g.call("sceNpCommerce2DoProductCodeStartAsync", 0x5001, 0, kParam) == 0);
    CHECK(g.call("sceNpCommerce2DoProductCodeFinishAsync", 0x5002) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2DoProductCodeFinishAsync", 0x5001) == 0);

    // No dialog sent the handler an event.
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(handler_calls(b) == 0);
}

TEST_CASE("sceNpSns: Facebook handles in idm order; no access token without a signed-in user", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "fb");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    constexpr u32 kParams = kData + 0x100, kHandle = kData + 0x110, kParam = kData + 0x200, kResult = kData + 0x400;
    constexpr u64 kAppId = 123456789012345ull;
    // SceNpSnsFbInitParams {pool, poolSize}; SceNpSnsFbAccessTokenParam {be64 fb_app_id, char permissions[256],
    // be32 options}; SceNpSnsFbLongAccessTokenResult {be64 expiration, char access_token[4097]} (4112 bytes).
    constexpr u32 kResultSize = 4112;
    ps_write32(b, kParams, kPool);
    ps_write32(b, kParams + 4, 0x10000);
    std::memset(b + kParam, 0, 272);
    ps_write64(b, kParam, kAppId);
    put(b, kParam + 8, "publish_actions");
    std::memset(b + kResult, 0xAA, kResultSize);

    // Before init. A token request checks its arguments first (RPCS3's order).
    CHECK(g.call("sceNpSnsFbTerm") == kFbNotInit);
    CHECK(g.call("sceNpSnsFbCreateHandle", kHandle) == kFbNotInit);
    CHECK(g.call("sceNpSnsFbDestroyHandle", 1) == kFbNotInit);
    CHECK(g.call("sceNpSnsFbAbortHandle", 1) == kFbNotInit);
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 1, 0, kResult) == kSnsInval);
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 1, kParam, kResult) == kFbNotInit);

    // Init needs its parameters, not NP; a second init is refused before they are looked at.
    CHECK(g.call("sceNpSnsFbInit", 0) == kSnsInval);
    REQUIRE(g.call("sceNpSnsFbInit", kParams) == 0);
    CHECK(g.call("sceNpSnsFbInit", 0) == kFbAlready);

    // Handles: IDs 1 to 5 in idm's order (RPCS3's id_count is SCE_NP_SNS_FB_HANDLE_SLOT_MAX + 1): a destroyed ID comes
    // back only once all were handed out. The ID is written before it is checked, so with all in use 0 is written.
    CHECK(g.call("sceNpSnsFbCreateHandle", 0) == kSnsInval);
    CHECK(g.call("sceNpSnsFbCreateHandle", kUnmapped) == kSnsInval); // RPCS3 faults
    REQUIRE(g.call("sceNpSnsFbCreateHandle", kHandle) == 0);
    CHECK(ps_read32(b, kHandle) == 1);
    REQUIRE(g.call("sceNpSnsFbDestroyHandle", 1) == 0);
    for (u32 id = 2; id <= 5; id++) {
        REQUIRE(g.call("sceNpSnsFbCreateHandle", kHandle) == 0);
        CHECK(ps_read32(b, kHandle) == id);
    }
    REQUIRE(g.call("sceNpSnsFbCreateHandle", kHandle) == 0);
    CHECK(ps_read32(b, kHandle) == 1);
    ps_write32(b, kHandle, 0xAAAAAAAA);
    CHECK(g.call("sceNpSnsFbCreateHandle", kHandle) == kFbExceeds);
    CHECK(ps_read32(b, kHandle) == 0);

    // Only IDs 1 to 4 can be destroyed or aborted (so handle 5 stays, as in RPCS3).
    CHECK(g.call("sceNpSnsFbDestroyHandle", 0) == kSnsInval);
    CHECK(g.call("sceNpSnsFbDestroyHandle", 5) == kSnsInval);
    CHECK(g.call("sceNpSnsFbAbortHandle", 0) == kSnsInval);
    CHECK(g.call("sceNpSnsFbAbortHandle", 5) == kSnsInval);
    REQUIRE(g.call("sceNpSnsFbDestroyHandle", 3) == 0);
    CHECK(g.call("sceNpSnsFbDestroyHandle", 3) == kFbUnknown);
    CHECK(g.call("sceNpSnsFbAbortHandle", 3) == kFbUnknown);
    CHECK(g.call("sceNpSnsFbAbortHandle", 2) == 0); // nothing runs on it

    // A long access token: param, result and an app ID, then a known handle; nobody is signed in to PSN, so it is
    // refused and nothing is written.
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 2, kParam, 0) == kSnsInval);
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 2, kUnmapped, kResult) == kSnsInval); // RPCS3 faults
    ps_write64(b, kParam, 0);
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 2, kParam, kResult) == kSnsInval);
    ps_write64(b, kParam, kAppId);
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 0, kParam, kResult) == kSnsInval);
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 5, kParam, kResult) == kSnsInval);
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 3, kParam, kResult) == kFbUnknown);
    CHECK(g.call("sceNpSnsFbGetLongAccessToken", 2, kParam, kResult) == kSnsNotSignIn);
    CHECK(all_bytes(b + kResult, kResultSize, 0xAA));

    // Term leaves the handles (RPCS3's idm keeps them).
    REQUIRE(g.call("sceNpSnsFbTerm") == 0);
    CHECK(g.call("sceNpSnsFbTerm") == kFbNotInit);
    CHECK(g.call("sceNpSnsFbDestroyHandle", 2) == kFbNotInit);
    REQUIRE(g.call("sceNpSnsFbInit", kParams) == 0);
    REQUIRE(g.call("sceNpSnsFbDestroyHandle", 2) == 0);
    REQUIRE(g.call("sceNpSnsFbCreateHandle", kHandle) == 0);
    CHECK(ps_read32(b, kHandle) == 2); // the lowest free (2 and 3 are)
}

TEST_CASE("sceNpCommerce2's store browsing, empty store check, product lists and rating getters, as RPCS3", "[hle][np]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread g(f, 0, "store");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    write_handler(b);
    constexpr u32 kNpId = kData + 0x100, kCtx = kData + 0x140, kId = kData + 0x200, kParam = kData + 0x300,
                  kOut = kData + 0x400, kBuf = kData + 0x40000 + 0x30000, kEmpty = kData + 0x500;
    constexpr u32 kArg = 0xCAFE0000u;
    std::memset(b + kNpId, 0, 0x24);
    put(b, kId, "UP0001-NPXX00001_00");
    b[kEmpty] = 0;

    CHECK(g.call("sceNpCommerce2EmptyStoreCheckAbort", 0x5001) == kNotInit);
    CHECK(g.call("sceNpCommerce2GetBGDLAvailability", kOut) == kNotInit);
    np_up(g);
    REQUIRE(g.call("sceNpCommerce2Init") == 0);
    REQUIRE(g.call("sceNpCommerce2CreateCtx", 2, kNpId, kOpd, kArg, kCtx) == 0);
    const u32 ctx = ps_read32(b, kCtx);

    // The session can't be aborted where there is no context.
    CHECK(g.call("sceNpCommerce2CreateSessionAbort", 0x5002) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2CreateSessionAbort", ctx) == 0);

    // Browsing a product: a product ID, a param of 4 bytes when given, then the context.
    ps_write32(b, kParam, 8);
    CHECK(g.call("sceNpCommerce2DoProductBrowseStartAsync", ctx, 0, 0, 0) == kInval);
    CHECK(g.call("sceNpCommerce2DoProductBrowseStartAsync", ctx, kEmpty, 0, 0) == kInval);
    CHECK(g.call("sceNpCommerce2DoProductBrowseStartAsync", ctx, kId, 0, kParam) == kBadSize);
    ps_write32(b, kParam, 4);
    CHECK(g.call("sceNpCommerce2DoProductBrowseStartAsync", 0x5002, kId, 0, kParam) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2DoProductBrowseStartAsync", ctx, kId, 0, kParam) == 0);
    CHECK(g.call("sceNpCommerce2DoProductBrowseStartAsync", ctx, kId, 0, 0) == 0);
    CHECK(g.call("sceNpCommerce2DoProductBrowseFinishAsync", ctx) == 0);
    // The service list: both needed, the param's size, a service ID.
    CHECK(g.call("sceNpCommerce2DoServiceListStartAsync", ctx, kId, 0, 0) == kInval);
    ps_write32(b, kParam, 5);
    CHECK(g.call("sceNpCommerce2DoServiceListStartAsync", ctx, kId, 0, kParam) == kBadSize);
    ps_write32(b, kParam, 4);
    CHECK(g.call("sceNpCommerce2DoServiceListStartAsync", ctx, kEmpty, 0, kParam) == kBadTargetId);
    CHECK(g.call("sceNpCommerce2DoServiceListStartAsync", ctx, kId, 0, kParam) == 0);
    CHECK(g.call("sceNpCommerce2DoServiceListFinishAsync", 0x5002) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2DoServiceListFinishAsync", ctx) == 0);
    CHECK(handler_calls(b) == 0);

    // The empty store check: a target and the category type; the handler hears it is done; the store isn't empty.
    CHECK(g.call("sceNpCommerce2EmptyStoreCheckStart", ctx, 1, kEmpty) == kBadTargetId);
    CHECK(g.call("sceNpCommerce2EmptyStoreCheckStart", ctx, 2, kId) == kInval);
    CHECK(g.call("sceNpCommerce2EmptyStoreCheckStart", 0x5002, 1, kId) == kCtxNotFound);
    REQUIRE(g.call("sceNpCommerce2EmptyStoreCheckStart", ctx, 1, kId) == 0);
    CHECK(handler_calls(b) == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    REQUIRE(handler_calls(b) == 1);
    CHECK(handler_arg(b, 0, 0) == ctx);
    CHECK(handler_arg(b, 0, 1) == 0);
    CHECK(handler_arg(b, 0, 2) == 0x61); // SCE_NP_COMMERCE2_EVENT_EMPTY_STORE_CHECK_DONE
    CHECK(handler_arg(b, 0, 3) == 0);
    CHECK(handler_arg(b, 0, 4) == kArg);
    CHECK(g.call("sceNpCommerce2EmptyStoreCheckFinish", ctx, 0) == kInval);
    ps_write32(b, kOut, 0x55);
    REQUIRE(g.call("sceNpCommerce2EmptyStoreCheckFinish", ctx, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0); // SCE_NP_COMMERCE2_STORE_IS_NOT_EMPTY
    CHECK(g.call("sceNpCommerce2EmptyStoreCheckAbort", 0x5002) == 0);

    // The store itself: category, product or product code (no ID needed for a code).
    CHECK(g.call("sceNpCommerce2ExecuteStoreBrowse", 0, kId, 7) == kBadTargetType);
    CHECK(g.call("sceNpCommerce2ExecuteStoreBrowse", 4, kId, 7) == kBadTargetType);
    CHECK(g.call("sceNpCommerce2ExecuteStoreBrowse", 2, kEmpty, 7) == kBadTargetId);
    CHECK(g.call("sceNpCommerce2ExecuteStoreBrowse", 1, kId, 7) == 0);
    CHECK(g.call("sceNpCommerce2ExecuteStoreBrowse", 3, 0, 7) == 0);
    ps_write32(b, kOut, 0x55);
    CHECK(g.call("sceNpCommerce2GetStoreBrowseUserdata", 0) == kInval);
    REQUIRE(g.call("sceNpCommerce2GetStoreBrowseUserdata", kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0);
    b[kOut] = 1;
    REQUIRE(g.call("sceNpCommerce2GetBGDLAvailability", kOut) == 0);
    CHECK(b[kOut] == 0);
    CHECK(g.call("sceNpCommerce2SetBGDLAvailability", 1) == 0);

    // Product lists: the request needs the context; 1 to 60 products; a 256 KB buffer; result data isn't parsed.
    CHECK(g.call("sceNpCommerce2GetProductInfoListCreateReq", ctx, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetProductInfoListCreateReq", 0x5002, kOut) == kCtxNotFound);
    CHECK(g.call("sceNpCommerce2GetProductInfoListCreateReq", ctx, kOut) == 0);
    CHECK(g.call("sceNpCommerce2GetProductInfoListStart", 1, kParam, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetProductInfoListStart", 1, kParam, 61) == kInval);
    CHECK(g.call("sceNpCommerce2GetProductInfoListStart", 1, kParam, 60) == 0);
    CHECK(g.call("sceNpCommerce2GetProductInfoListGetResult", 1, kBuf, kRecvBufSize - 1, kOut) == kBufTooSmall);
    CHECK(g.call("sceNpCommerce2GetProductInfoListGetResult", 1, kBuf, kRecvBufSize, kOut) == 0);
    CHECK(g.call("sceNpCommerce2InitGetProductInfoListResult", kOut, kBuf, 0) == kInval);
    CHECK(g.call("sceNpCommerce2InitGetProductInfoListResult", kOut, kBuf, 100) == kBadResultData);

    // The getters: both pointers, nothing written.
    for (const char* name : {"sceNpCommerce2GetCategoryInfoFromContentInfo",
             "sceNpCommerce2GetContentRatingInfoFromCategoryInfo",
             "sceNpCommerce2GetContentRatingInfoFromGameProductInfo"}) {
        INFO(name);
        CHECK(g.call(name, kParam, 0) == kInval);
        CHECK(g.call(name, kParam, kOut) == 0);
    }
    CHECK(g.call("sceNpCommerce2GetContentRatingDescriptor", kParam, 0, 0) == kInval);
    CHECK(g.call("sceNpCommerce2GetContentRatingDescriptor", kParam, 0, kOut) == 0);
    CHECK(g.call("sceNpCommerce2GetGameProductInfoFromGetProductInfoListResult", 0, 0, kOut) == kInval);
    CHECK(g.call("sceNpCommerce2GetGameProductInfoFromGetProductInfoListResult", kParam, 0, kOut) == 0);
}

TEST_CASE("sceNpSns: the short access token, throttles, config and stream publish, as RPCS3's TODOs", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "fb");
    u8* b = f.base();
    constexpr u32 kParams = kData + 0x100, kHandle = kData + 0x110, kParam = kData + 0x200, kResult = kData + 0x400,
                  kArg = kData + 0x1800;
    // SceNpSnsFbAccessTokenResult {be64 expiration, char access_token[129]} (144 bytes).
    constexpr u32 kResultSize = 144;
    ps_write32(b, kParams, kPool);
    ps_write32(b, kParams + 4, 0x10000);
    std::memset(b + kParam, 0, 272);
    ps_write64(b, kParam, 123456789012345ull);
    std::memset(b + kResult, 0xAA, kResultSize);
    std::memset(b + kArg, 0xAA, 64);

    // Before init: the config and throttle checks need the library (a null throttle argument is refused first); a
    // token request checks its arguments first.
    CHECK(g.call("sceNpSnsFbCheckConfig", kArg) == kFbNotInit);
    CHECK(g.call("sceNpSnsFbCheckThrottle", 0) == kSnsInval);
    CHECK(g.call("sceNpSnsFbCheckThrottle", kArg) == kFbNotInit);
    CHECK(g.call("sceNpSnsFbGetAccessToken", 1, kParam, 0) == kSnsInval);
    CHECK(g.call("sceNpSnsFbGetAccessToken", 1, kParam, kResult) == kFbNotInit);

    REQUIRE(g.call("sceNpSnsFbInit", kParams) == 0);
    CHECK(g.call("sceNpSnsFbCheckConfig", 0) == 0); // arg0 isn't looked at
    CHECK(g.call("sceNpSnsFbCheckConfig", kArg) == 0);
    CHECK(g.call("sceNpSnsFbCheckThrottle", 0) == kSnsInval);
    CHECK(g.call("sceNpSnsFbCheckThrottle", kArg) == 0);
    CHECK(all_bytes(b + kArg, 64, 0xAA));

    REQUIRE(g.call("sceNpSnsFbCreateHandle", kHandle) == 0);
    const u32 handle = ps_read32(b, kHandle);
    REQUIRE(handle == 1);

    // The access token: as the long one, refused because nobody is signed in to PSN; nothing written.
    CHECK(g.call("sceNpSnsFbGetAccessToken", handle, 0, kResult) == kSnsInval);
    CHECK(g.call("sceNpSnsFbGetAccessToken", handle, kUnmapped, kResult) == kSnsInval); // RPCS3 faults
    ps_write64(b, kParam, 0);
    CHECK(g.call("sceNpSnsFbGetAccessToken", handle, kParam, kResult) == kSnsInval); // no app ID
    ps_write64(b, kParam, 123456789012345ull);
    CHECK(g.call("sceNpSnsFbGetAccessToken", 0, kParam, kResult) == kSnsInval);
    CHECK(g.call("sceNpSnsFbGetAccessToken", 5, kParam, kResult) == kSnsInval);
    CHECK(g.call("sceNpSnsFbGetAccessToken", 2, kParam, kResult) == kFbUnknown);
    CHECK(g.call("sceNpSnsFbGetAccessToken", handle, kParam, kResult) == kSnsNotSignIn);
    CHECK(all_bytes(b + kResult, kResultSize, 0xAA));

    // Loading the throttle and publishing to the stream: the handle (1 to 4, known), then CELL_OK.
    for (const std::string_view name : {"sceNpSnsFbLoadThrottle", "sceNpSnsFbStreamPublish"}) {
        INFO(name);
        CHECK(g.call(name, 0) == kSnsInval);
        CHECK(g.call(name, 5) == kSnsInval);
        CHECK(g.call(name, 2) == kFbUnknown);
        CHECK(g.call(name, handle) == 0);
    }

    // They look only at the handle, which outlives the library's term (RPCS3's idm keeps it).
    REQUIRE(g.call("sceNpSnsFbTerm") == 0);
    CHECK(g.call("sceNpSnsFbCheckConfig", kArg) == kFbNotInit);
    CHECK(g.call("sceNpSnsFbCheckThrottle", kArg) == kFbNotInit);
    CHECK(g.call("sceNpSnsFbGetAccessToken", handle, kParam, kResult) == kFbNotInit);
    CHECK(g.call("sceNpSnsFbLoadThrottle", handle) == 0);
    CHECK(g.call("sceNpSnsFbStreamPublish", handle) == 0);
    CHECK(g.call("sceNpSnsFbLoadThrottle", 3) == kFbUnknown);
}
