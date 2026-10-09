// sceNp offline (synthetic: no game data): RPCS3's checks and results with no user signed in to PSN, for lookups,
// the manager's user queries and tickets, basic events and presence, messages, the players' history and its event for
// the basic handler, the profile dialog, signaling contexts and connections, NP ID comparison, and the DRM check of
// files on mounted directories.

#include "hle_test_util.hpp"

#include <unistd.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "pspocket/hle/npdrm.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "synthetic.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace fsys = std::filesystem;

namespace {

constexpr u64 kNotInit = 0xFFFFFFFF8002AA01ull, kInval = 0xFFFFFFFF8002AA03ull, kIdNotFound = 0xFFFFFFFF8002AA06ull,
              kInvalState = 0xFFFFFFFF8002AA0Aull, kOffline = 0xFFFFFFFF8002AA0Cull, kAuthInval = 0xFFFFFFFF8002A015ull;
constexpr u64 kBasicNotInit = 0xFFFFFFFF8002A662ull, kBasicInval = 0xFFFFFFFF8002A665ull,
              kBasicNoEvent = 0xFFFFFFFF8002A66Aull, kBasicExceeds = 0xFFFFFFFF8002A66Bull,
              kBasicNotRegistered = 0xFFFFFFFF8002A66Dull, kBasicInvalDataId = 0xFFFFFFFF8002A673ull;
constexpr u64 kComAlready = 0xFFFFFFFF8002A101ull, kComNotInit = 0xFFFFFFFF8002A102ull,
              kComInval = 0xFFFFFFFF8002A104ull, kComTooMany = 0xFFFFFFFF8002A107ull,
              kComInsufficient = 0xFFFFFFFF8002A1A1ull, kComInvalId = 0xFFFFFFFF8002A1A3ull,
              kComInvalOnlineId = 0xFFFFFFFF8002A1A4ull;
constexpr u64 kSigNotInit = 0xFFFFFFFF8002A801ull, kSigCtxNotFound = 0xFFFFFFFF8002A805ull,
              kSigConnNotFound = 0xFFFFFFFF8002A80Eull, kSigCtxMax = 0xFFFFFFFF8002A812ull,
              kSigInval = 0xFFFFFFFF8002A815ull, kSigOwnNpId = 0xFFFFFFFF8002A816ull;
constexpr u64 kUtilInval = 0xFFFFFFFF8002AB01ull, kUtilNotMatch = 0xFFFFFFFF8002AB0Full;
constexpr u64 kBasicNotSupported = 0xFFFFFFFF8002A663ull, kBasicCancel = 0xFFFFFFFF8002A671ull,
              kBasicNotConnected = 0xFFFFFFFF8002A678ull, kUtilInvalChar = 0xFFFFFFFF8002AB0Bull;
constexpr u64 kProfileNotInit = 0xFFFFFFFF8002AB41ull, kProfileInval = 0xFFFFFFFF8002AB48ull;
constexpr u64 kSigPeerUnreachable = 0xFFFFFFFF8002A80Full;
constexpr u64 kDrmInvalParam = 0xFFFFFFFF80029502ull, kDrmNoEntitlement = 0xFFFFFFFF80029513ull,
              kDrmBadFormat = 0xFFFFFFFF80029515ull, kDrmLicenseNotFound = 0xFFFFFFFF80029521ull;

constexpr u32 kPool = kData + 0x40000; // NP's 128 KB pool
constexpr u32 kUnmapped = 0x20000000;  // no guest memory there

void np_up(GuestThread& g) { REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0); }

void put(u8* b, u32 addr, std::string_view s)
{
    std::memcpy(b + addr, s.data(), s.size());
    b[addr + s.size()] = 0;
}

// A SceNpId (0x24 bytes) with this online ID and every other byte 0.
void put_np_id(u8* b, u32 addr, std::string_view online_id)
{
    std::memset(b + addr, 0, 0x24);
    std::memcpy(b + addr, online_id.data(), online_id.size());
}

// A SceNpCommunicationId (12 bytes): nine characters, the terminator, num.
void put_comm_id(u8* b, u32 addr, std::string_view data, u8 num)
{
    std::memset(b + addr, 0, 12);
    std::memcpy(b + addr, data.data(), data.size());
    b[addr + 10] = num;
}

// A SceNpBasicMessageDetails (40 bytes): msgId (u64), mainType, subType (u16), msgFeatures, npids, count, subject,
// body, data, size.
struct Message {
    u64 msg_id = 0;
    u16 main_type = 0, sub_type = 0;
    u32 features = 0, npids = 0, count = 0, subject = 0, body = 0, data = 0, size = 0;
};

void put_message(u8* b, u32 addr, const Message& m)
{
    ps_write64(b, addr, m.msg_id);
    ps_write16(b, addr + 8, m.main_type);
    ps_write16(b, addr + 10, m.sub_type);
    ps_write32(b, addr + 12, m.features);
    ps_write32(b, addr + 16, m.npids);
    ps_write32(b, addr + 20, m.count);
    ps_write32(b, addr + 24, m.subject);
    ps_write32(b, addr + 28, m.body);
    ps_write32(b, addr + 32, m.data);
    ps_write32(b, addr + 36, m.size);
}

// Guest functions without lifted code run in the interpreter while this lives (the default policy aborts).
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// A guest basic event handler (SceNpBasicEventHandler: event, retCode, reqId, arg) behind the descriptor kEventOpd. It
// stores its arguments' low words at kEventOut (+0 event, +4 retCode, +8 reqId, +12 arg), counts its calls at
// kEventOut + 16 and returns 0.
constexpr u32 kEventCode = kData + 0x20000, kEventOpd = kData + 0x20800, kEventOut = kData + 0x21000;

void write_event_handler(u8* b)
{
    const std::vector<u32> code = {
        as::lis(9, static_cast<s32>(kEventOut >> 16)),
        as::ori(9, 9, kEventOut & 0xFFFF),
        as::stw(3, 0, 9),
        as::stw(4, 4, 9),
        as::stw(5, 8, 9),
        as::stw(6, 12, 9),
        as::lwz(10, 16, 9),
        as::addi(10, 10, 1),
        as::stw(10, 16, 9),
        as::li(3, 0),
        as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kEventCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kEventOpd, kEventCode);
    ps_write32(b, kEventOpd + 4, 0x00123400);
    std::memset(b + kEventOut, 0, 32);
}

// The host directories the DRM test mounts: a disc at /dev_bdvd (read only), the user's drive at /dev_hdd0.
struct DrmDirs {
    fsys::path root, bdvd, hdd0;

    DrmDirs()
        : root(fsys::temp_directory_path() / std::format("pspocket-hle-np-drm-{}", ::getpid())), bdvd(root / "bdvd"),
          hdd0(root / "hdd0")
    {
        fsys::remove_all(root);
        fsys::create_directories(bdvd);
        fsys::create_directories(hdd0);
    }
    ~DrmDirs()
    {
        std::error_code ec;
        fsys::remove_all(root, ec);
    }
    DrmDirs(const DrmDirs&) = delete;
    DrmDirs& operator=(const DrmDirs&) = delete;

    static void write(const fsys::path& p, const std::vector<u8>& bytes)
    {
        fsys::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    void mount(Fixture& f) const
    {
        REQUIRE(f.kernel->vfs_mount("/dev_bdvd", bdvd.string(), true));
        REQUIRE(f.kernel->vfs_mount("/dev_hdd0", hdd0.string(), false));
    }
};

// The start of a synthetic EDAT: an NPD header (magic "NPD\0", version 3, the licence type, the content ID; no
// hashes), then the EDAT header's flags.
std::vector<u8> npd_file(u32 license, std::string_view content_id, u32 flags)
{
    std::vector<u8> v(0x100, 0);
    std::memcpy(v.data(), "NPD", 4);
    ps_write32(v.data(), 4, 3);
    ps_write32(v.data(), 8, license);
    std::memcpy(v.data() + 0x10, content_id.data(), content_id.size());
    ps_write32(v.data(), 0x80, flags);
    return v;
}

} // namespace

TEST_CASE("sceNpLookup: init needs NP, title contexts in idm order, transactions refused offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kBadComm = kData + 0x110, kSelf = kData + 0x120, kOnline = kData + 0x160,
                  kNpId = kData + 0x180, kOpt = kData + 0x1C0, kBuf = kData + 0x200, kLen = kData + 0x300,
                  kResult = kData + 0x304;
    put_comm_id(b, kComm, "NPWR01234", 0);
    put_np_id(b, kSelf, "");
    put(b, kOnline, "somebody");

    // Before NP: lookup init needs NP; everything else needs lookup init first.
    CHECK(g.call("sceNpLookupInit") == kNotInit);
    CHECK(g.call("sceNpLookupTerm") == kComNotInit);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == kComNotInit);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3001) == kComNotInit);
    CHECK(g.call("sceNpLookupCreateTransactionCtx", 0x3001) == kComNotInit);
    CHECK(g.call("sceNpLookupDestroyTransactionCtx", 0x4001) == kComNotInit);
    CHECK(g.call("sceNpLookupSetTimeout", 0x3001, 20'000'000) == kComNotInit);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, kOnline, kNpId, 0, 0) == kComNotInit);
    CHECK(g.call("sceNpLookupTitleSmallStorageAsync", 0x4001, kBuf, 0x100, kLen, 0, 0) == kComNotInit);
    CHECK(g.call("sceNpLookupPollAsync", 0x4001, kResult) == kComNotInit);

    np_up(g);
    REQUIRE(g.call("sceNpLookupInit") == 0);
    CHECK(g.call("sceNpLookupInit") == kComAlready);

    // Title contexts: the ID is the result, from 0x3001; a valid communication ID and an NP ID pointer are needed.
    CHECK(g.call("sceNpLookupCreateTitleCtx", 0, kSelf) == kComInsufficient);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, 0) == kComInsufficient);
    put_comm_id(b, kBadComm, "npwr01234", 0);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kBadComm, kSelf) == kComInval); // RPCS3 aborts
    put_comm_id(b, kBadComm, "NPWR01234", 100);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kBadComm, kSelf) == kComInval);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == 0x3001);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == 0x3002);
    REQUIRE(g.call("sceNpLookupDestroyTitleCtx", 0x3001) == 0);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3001) == kComInvalId);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3000) == kComInvalId);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", -1) == kComInvalId);
    // As RPCS3's idm: new IDs until each of the 32 was handed out once, then the lowest free one, then none.
    for (u32 id = 0x3003; id <= 0x3020; id++) REQUIRE(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == id);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == 0x3001);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == kComTooMany);
    REQUIRE(g.call("sceNpLookupDestroyTitleCtx", 0x3010) == 0);
    CHECK(g.call("sceNpLookupCreateTitleCtx", kComm, kSelf) == 0x3010);

    // Transactions need PSN: none is made (whatever the title context), so none can be destroyed.
    CHECK(g.call("sceNpLookupCreateTransactionCtx", 0x3002) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupCreateTransactionCtx", 0x7777) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupDestroyTransactionCtx", 0x4001) == kComInvalId);

    // The timeout is a 64-bit number of microseconds, at least 10 s; the context isn't checked.
    CHECK(g.call("sceNpLookupSetTimeout", 0x3002, 9'999'999) == kComInval);
    CHECK(g.call("sceNpLookupSetTimeout", 0x3002, 10'000'000) == 0);
    CHECK(g.call("sceNpLookupSetTimeout", 0x7777, 0x100000000ull) == 0);

    // Lookups: arguments checked, then refused offline with nothing written.
    std::memset(b + kNpId, 0xAA, 0x24);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, 0, kNpId, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, kOnline, 0, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, kOnline, kNpId, 0, kOpt) == kComInval);
    CHECK(g.call("sceNpLookupNpIdAsync", 0x4001, kOnline, kNpId, 0, 0) == kComInvalOnlineId);
    CHECK(b[kNpId] == 0xAA);
    std::memset(b + kBuf, 0xAA, 0x100);
    ps_write32(b, kLen, 0x55);
    CHECK(g.call("sceNpLookupTitleSmallStorageAsync", 0x4001, 0, 0x100, kLen, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupTitleSmallStorageAsync", 0x4001, kBuf, 0x100, kLen, 0, kOpt) == kComInval);
    CHECK(g.call("sceNpLookupTitleSmallStorageAsync", 0x4001, kBuf, 0x100, kLen, 0, 0) == kComInvalOnlineId);
    CHECK(b[kBuf] == 0xAA);
    CHECK(ps_read32(b, kLen) == 0x55);
    // Polling reports any transaction finished with result 0.
    ps_write32(b, kResult, 0xFFFFFFFF);
    REQUIRE(g.call("sceNpLookupPollAsync", 0x4001, kResult) == 0);
    CHECK(ps_read32(b, kResult) == 0);

    // NP term leaves the lookup library and its title contexts, as in RPCS3; lookup term itself needs NP.
    REQUIRE(g.call("sceNp2Term") == 0);
    CHECK(g.call("sceNpLookupTerm") == kNotInit);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3002) == 0);
    np_up(g);
    CHECK(g.call("sceNpLookupInit") == kComAlready);
    REQUIRE(g.call("sceNpLookupTerm") == 0);
    CHECK(g.call("sceNpLookupTerm") == kComNotInit);
    CHECK(g.call("sceNpLookupDestroyTitleCtx", 0x3003) == kComNotInit);
}

TEST_CASE("sceNpManager: the user's details and tickets are not available offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kOut1 = kData + 0x100, kOut2 = kData + 0x110, kNpId = kData + 0x120, kService = kData + 0x160,
                  kBuf = kData + 0x200, kSize = kData + 0x300, kParam = kData + 0x400, kEnt = kData + 0x600,
                  kEntId = kData + 0x700;
    put(b, kService, "UP0001-NPWR01234_00");
    put(b, kEntId, "SOMEDLC");

    CHECK(g.call("sceNpManagerGetNpId", kNpId) == kNotInit);
    CHECK(g.call("sceNpManagerGetAccountRegion", kOut1, kOut2) == kNotInit);
    CHECK(g.call("sceNpManagerGetAccountAge", kOut1) == kNotInit);
    CHECK(g.call("sceNpManagerGetContentRatingFlag", kOut1, kOut2) == kNotInit);
    CHECK(g.call("sceNpManagerGetChatRestrictionFlag", kOut1) == kNotInit);
    CHECK(g.call("sceNpManagerGetNetworkTime", kOut1) == kNotInit);
    CHECK(g.call("sceNpManagerRequestTicket2", kNpId, 0, kService, 0, 0, 0, 0) == kNotInit);
    CHECK(g.call("sceNpManagerGetTicket", kBuf, kSize) == kNotInit);
    CHECK(g.call("sceNpManagerGetTicketParam", 0, kParam) == kNotInit);
    CHECK(g.call("sceNpManagerGetEntitlementIdList", kEnt, 4) == kNotInit);
    CHECK(g.call("sceNpManagerGetEntitlementById", kEntId, kEnt) == kNotInit);

    np_up(g);
    // Questions about the signed-in user: every output is needed, then the answer is offline and nothing is written.
    std::memset(b + kOut1, 0xAA, 0x20);
    std::memset(b + kNpId, 0xAA, 0x24);
    CHECK(g.call("sceNpManagerGetNpId", 0) == kInval);
    CHECK(g.call("sceNpManagerGetNpId", kNpId) == kOffline);
    CHECK(g.call("sceNpManagerGetAccountRegion", 0, kOut2) == kInval);
    CHECK(g.call("sceNpManagerGetAccountRegion", kOut1, 0) == kInval);
    CHECK(g.call("sceNpManagerGetAccountRegion", kOut1, kOut2) == kOffline);
    CHECK(g.call("sceNpManagerGetAccountAge", 0) == kInval);
    CHECK(g.call("sceNpManagerGetAccountAge", kOut1) == kOffline);
    CHECK(g.call("sceNpManagerGetContentRatingFlag", 0, kOut2) == kInval);
    CHECK(g.call("sceNpManagerGetContentRatingFlag", kOut1, 0) == kInval);
    CHECK(g.call("sceNpManagerGetContentRatingFlag", kOut1, kOut2) == kOffline);
    CHECK(g.call("sceNpManagerGetChatRestrictionFlag", 0) == kInval);
    CHECK(g.call("sceNpManagerGetChatRestrictionFlag", kOut1) == kOffline);
    CHECK(g.call("sceNpManagerGetNetworkTime", 0) == kInval);
    CHECK(g.call("sceNpManagerGetNetworkTime", kOut1) == kOffline);
    CHECK(ps_read64(b, kOut1) == 0xAAAAAAAAAAAAAAAAull);
    CHECK(ps_read64(b, kOut2) == 0xAAAAAAAAAAAAAAAAull);
    CHECK(ps_read32(b, kNpId) == 0xAAAAAAAA);

    // Ticket requests are refused offline (after their own argument checks), so there is never a ticket to read.
    CHECK(g.call("sceNpManagerRequestTicket2", 0, 0, kService, 0, 0, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket2", kNpId, 0, 0, 0, 0, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket2", kNpId, 0, kService, kBuf, 1025, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket2", kNpId, 0, kService, kBuf, 1024, 0, 0) == kOffline);
    CHECK(g.call("sceNpManagerGetTicket", kBuf, 0) == kInval);
    ps_write32(b, kSize, 0x1234);
    REQUIRE(g.call("sceNpManagerGetTicket", 0, kSize) == 0); // no buffer: the ticket's size
    CHECK(ps_read32(b, kSize) == 0);
    ps_write32(b, kSize, 0x100);
    std::memset(b + kBuf, 0xAA, 0x100);
    REQUIRE(g.call("sceNpManagerGetTicket", kBuf, kSize) == 0); // a buffer: nothing copied
    CHECK(ps_read32(b, kSize) == 0);
    CHECK(b[kBuf] == 0xAA);
    CHECK(g.call("sceNpManagerGetTicketParam", 0, 0) == kInval);
    CHECK(g.call("sceNpManagerGetTicketParam", -1, kParam) == kInval);
    CHECK(g.call("sceNpManagerGetTicketParam", 12, kParam) == kInval);
    CHECK(g.call("sceNpManagerGetTicketParam", 0, kParam) == kInvalState);
    CHECK(g.call("sceNpManagerGetTicketParam", 11, kParam) == kInvalState);

    // No entitlements: the list's count is 0 and no ID is found.
    CHECK(g.call("sceNpManagerGetEntitlementIdList", kEnt, 4) == 0);
    CHECK(g.call("sceNpManagerGetEntitlementIdList", 0, 0) == 0);
    CHECK(g.call("sceNpManagerGetEntitlementById", 0, kEnt) == kInval);
    CHECK(g.call("sceNpManagerGetEntitlementById", kEntId, kEnt) == kIdNotFound);

    REQUIRE(g.call("sceNp2Term") == 0);
    CHECK(g.call("sceNpManagerGetNpId", kNpId) == kNotInit);
    CHECK(g.call("sceNpManagerGetTicket", 0, kSize) == kNotInit);
}

TEST_CASE("sceNpBasic: no events, presence goes nowhere, no block list or messages offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kHandler = kData + 0x120, kEvent = kData + 0x140, kFrom = kData + 0x200,
                  kBuf = kData + 0x400, kSize = kData + 0x500, kPres = kData + 0x600, kCount = kData + 0x900,
                  kNpId = kData + 0x920;
    put_comm_id(b, kComm, "NPWR01234", 0);

    // Before NP; the message type is checked even before that.
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, kSize) == kBasicNotInit);
    CHECK(g.call("sceNpBasicSetPresence", kBuf, 16) == kBasicNotInit);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 3) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetBlockListEntryCount", kCount) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetBlockListEntry", 0, kNpId) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetMessageEntryCount", 8, kCount) == kBasicInval);
    CHECK(g.call("sceNpBasicGetMessageEntryCount", 0, kCount) == kBasicNotInit);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, kBuf, kSize) == kBasicNotInit);

    // Events, presence and attachments need the basic handler; the block list and messages don't.
    np_up(g);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, kSize) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicSetPresence", kBuf, 16) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 3) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, kBuf, kSize) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicGetBlockListEntryCount", kCount) == kIdNotFound);
    REQUIRE(g.call("sceNpBasicRegisterContextSensitiveHandler", kComm, kHandler, 0) == 0);

    // No event ever comes offline.
    CHECK(g.call("sceNpBasicGetEvent", 0, kFrom, kBuf, kSize) == kBasicInval);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, 0, kBuf, kSize) == kBasicInval);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, 0, kSize) == kBasicInval);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, 0) == kBasicInval);
    ps_write32(b, kEvent, 0x55);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, kSize) == kBasicNoEvent);
    CHECK(ps_read32(b, kEvent) == 0x55);

    // Presence data: at most 128 bytes, readable.
    CHECK(g.call("sceNpBasicSetPresence", kBuf, 129) == kBasicExceeds);
    CHECK(g.call("sceNpBasicSetPresence", kBuf, 128) == 0);
    CHECK(g.call("sceNpBasicSetPresence", 0, 0) == 0);
    CHECK(g.call("sceNpBasicSetPresence", 0, 16) == kBasicInval); // RPCS3 ensure()s it

    // Presence details 2: options 1 to 3, struct_size 524, data size at most 128.
    std::memset(b + kPres, 0, 524);
    ps_write32(b, kPres, 524);
    put(b, kPres + 136, "Story mode");
    ps_write32(b, kPres + 520, 128);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 4) == kBasicInval);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", 0, 3) == kBasicInval); // RPCS3 reads it unchecked
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 3) == 0);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 1) == 0);
    ps_write32(b, kPres + 520, 129);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 2) == kBasicExceeds);
    ps_write32(b, kPres + 520, 0);
    ps_write32(b, kPres, 520);
    CHECK(g.call("sceNpBasicSetPresenceDetails2", kPres, 2) == kBasicInval);

    // The block list and messages are the PSN account's: not found, nothing written.
    ps_write32(b, kCount, 0x55);
    CHECK(g.call("sceNpBasicGetBlockListEntryCount", 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetBlockListEntryCount", kCount) == kIdNotFound);
    CHECK(g.call("sceNpBasicGetBlockListEntry", 0, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetBlockListEntry", 0, kNpId) == kIdNotFound);
    CHECK(g.call("sceNpBasicGetMessageEntryCount", 7, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetMessageEntryCount", 7, kCount) == kIdNotFound);
    CHECK(ps_read32(b, kCount) == 0x55);

    // No invitation or message can be selected offline.
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, 0, kSize) == kBasicInval);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, kBuf, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 0, kBuf, kSize) == kBasicInvalDataId);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 1, kBuf, kSize) == kBasicInvalDataId);
    CHECK(g.call("sceNpBasicRecvMessageAttachmentLoad", 2, kBuf, kSize) == kBasicInvalDataId);

    // NP term drops the handler.
    REQUIRE(g.call("sceNp2Term") == 0);
    np_up(g);
    CHECK(g.call("sceNpBasicGetEvent", kEvent, kFrom, kBuf, kSize) == kBasicNotRegistered);
}

TEST_CASE("sceNpSignaling: contexts in idm order, cleared by NP term; no network, no connections", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kOwn = kData + 0x140, kCtx = kData + 0x180, kInfo = kData + 0x200,
                  kStatus = kData + 0x300, kAddr = kData + 0x304, kPort = kData + 0x308, kConn = kData + 0x30C,
                  kConnInfo = kData + 0x400;
    constexpr u32 kHandler = 0x00012340, kArg = 0x5678;
    put_np_id(b, kNpId, "somebody");
    put_np_id(b, kOwn, "");

    CHECK(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == kSigNotInit);
    CHECK(g.call("sceNpSignalingDestroyCtx", 0x6001) == kSigNotInit);
    CHECK(g.call("sceNpSignalingAddExtendedHandler", 0x6001, kHandler, kArg) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetLocalNetInfo", 0x6001, kInfo) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetConnectionStatus", 0x6001, 1, kStatus, kAddr, kPort) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetConnectionInfo", 0x6001, 1, 1, kConnInfo) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kNpId, kConn) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetConnectionFromPeerAddress", 0x6001, 0x7F000001, 3658, kConn) == kSigNotInit);
    CHECK(g.call("sceNpSignalingTerminateConnection", 0x6001, 1) == kSigNotInit);

    // Contexts: IDs from 0x6001; extended handlers are kept per context.
    np_up(g);
    CHECK(g.call("sceNpSignalingCreateCtx", 0, kHandler, kArg, kCtx) == kSigInval);
    CHECK(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, 0) == kSigInval);
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == 0x6001);
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, 0, 0, kCtx) == 0); // a null handler is accepted
    CHECK(ps_read32(b, kCtx) == 0x6002);
    CHECK(g.call("sceNpSignalingAddExtendedHandler", 0x6001, kHandler, kArg) == 0);
    CHECK(g.call("sceNpSignalingAddExtendedHandler", 0x6003, kHandler, kArg) == kSigCtxNotFound);
    REQUIRE(g.call("sceNpSignalingDestroyCtx", 0x6001) == 0);
    CHECK(g.call("sceNpSignalingDestroyCtx", 0x6001) == kSigCtxNotFound);
    CHECK(g.call("sceNpSignalingAddExtendedHandler", 0x6001, kHandler, kArg) == kSigCtxNotFound);

    // NP term clears them but, as RPCS3's idm, doesn't rewind the IDs: 0x6003 is next. Once all 8 were handed out,
    // the lowest free one, then none.
    REQUIRE(g.call("sceNp2Term") == 0);
    np_up(g);
    CHECK(g.call("sceNpSignalingDestroyCtx", 0x6002) == kSigCtxNotFound);
    for (u32 id = 0x6003; id <= 0x6008; id++) {
        REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == 0);
        CHECK(ps_read32(b, kCtx) == id);
    }
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == 0x6001);
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == 0x6002);
    ps_write32(b, kCtx, 0);
    CHECK(g.call("sceNpSignalingCreateCtx", kNpId, kHandler, kArg, kCtx) == kSigCtxMax);
    CHECK(ps_read32(b, kCtx) == 0);

    // Local network info: no addresses; the struct's size picks the layout (28 bytes with the NP port, 24 without).
    // The context isn't checked.
    std::memset(b + kInfo, 0xAA, 32);
    ps_write32(b, kInfo, 20);
    CHECK(g.call("sceNpSignalingGetLocalNetInfo", 0x6001, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetLocalNetInfo", 0x6001, kInfo) == kSigInval);
    ps_write32(b, kInfo, 28);
    REQUIRE(g.call("sceNpSignalingGetLocalNetInfo", 0x7777, kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 4) == 0);  // local_addr
    CHECK(ps_read32(b, kInfo + 8) == 0);  // mapped_addr
    CHECK(ps_read32(b, kInfo + 12) == 2); // nat_status: type 2
    CHECK(ps_read32(b, kInfo + 16) == 1); // upnp_status: invalid
    CHECK(ps_read32(b, kInfo + 20) == 1); // npport_status: open
    CHECK(ps_read16(b, kInfo + 24) == 3658);
    CHECK(ps_read16(b, kInfo + 26) == 0xAAAA);
    std::memset(b + kInfo, 0xAA, 32);
    ps_write32(b, kInfo, 24);
    REQUIRE(g.call("sceNpSignalingGetLocalNetInfo", 0x6001, kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 4) == 0);
    CHECK(ps_read32(b, kInfo + 20) == 1);
    CHECK(ps_read16(b, kInfo + 24) == 0xAAAA);

    // No connection exists offline.
    CHECK(g.call("sceNpSignalingGetConnectionStatus", 0x6001, 1, 0, kAddr, kPort) == kSigInval);
    ps_write32(b, kStatus, 2);
    ps_write32(b, kAddr, 0x55);
    ps_write16(b, kPort, 0x55);
    CHECK(g.call("sceNpSignalingGetConnectionStatus", 0x6001, 1, kStatus, kAddr, kPort) == kSigConnNotFound);
    CHECK(ps_read32(b, kStatus) == 0); // inactive
    CHECK(ps_read32(b, kAddr) == 0x55);
    CHECK(ps_read16(b, kPort) == 0x55);
    CHECK(g.call("sceNpSignalingGetConnectionInfo", 0x6001, 1, 1, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetConnectionInfo", 0x6001, 1, 1, kConnInfo) == kSigConnNotFound);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, 0, kConn) == kSigInval);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kNpId, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kOwn, kConn) == kSigOwnNpId); // the user's: empty
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kNpId, kConn) == kSigConnNotFound);
    CHECK(g.call("sceNpSignalingGetConnectionFromPeerAddress", 0x6001, 0x7F000001, 3658, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetConnectionFromPeerAddress", 0x6001, 0x7F000001, 3658, kConn) == kSigConnNotFound);
    CHECK(g.call("sceNpSignalingTerminateConnection", 0x6001, 1) == 0);
}

TEST_CASE("sceNpUtilCmpNpId: the online IDs' first 16 characters, without NP", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kA = kData + 0x100, kB = kData + 0x140;
    put_np_id(b, kA, "player-one");
    put_np_id(b, kB, "player-one");
    CHECK(g.call("sceNpUtilCmpNpId", 0, kB) == kUtilInval);
    CHECK(g.call("sceNpUtilCmpNpId", kA, 0) == kUtilInval);
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == 0);
    b[kB + 12] = 'x'; // after the terminator
    b[kB + 20] = 1;   // opt[0]
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == 0);
    put_np_id(b, kB, "player-two");
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == kUtilNotMatch);
    put_np_id(b, kB, "player");
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == kUtilNotMatch);
    put_np_id(b, kA, "sixteen-chars-id");
    put_np_id(b, kB, "sixteen-chars-id");
    b[kB + 16] = 'x'; // the 17th character isn't compared
    CHECK(g.call("sceNpUtilCmpNpId", kA, kB) == 0);
}

TEST_CASE("sceNpBasic messages: RPCS3's checks, then not connected offline; nothing to receive, no event", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kHandler = kData + 0x120, kTo = kData + 0x140, kNobody = kData + 0x180,
                  kMsg = kData + 0x200, kAttachment = kData + 0x300, kSubject = kData + 0x400, kBody = kData + 0x500,
                  kText = kData + 0x600, kNpIds = kData + 0xA00;
    put_comm_id(b, kComm, "NPWR01234", 0);
    put_np_id(b, kTo, "somebody");
    put_np_id(b, kNobody, "");
    put(b, kSubject, "Hi");
    put(b, kBody, "Hello there");
    put_np_id(b, kNpIds, "player-one");
    put_np_id(b, kNpIds + 0x24, "player-two");

    // NP first, then the basic handler.
    CHECK(g.call("sceNpBasicSendMessage", kTo, kAttachment, 16) == kBasicNotInit);
    CHECK(g.call("sceNpBasicSendMessageGui", kMsg, 0) == kBasicNotInit);
    CHECK(g.call("sceNpBasicAddFriend", kTo, kBody, 0) == kBasicNotInit);
    CHECK(g.call("sceNpBasicRecvMessageCustom", 3, 0, 0) == kBasicNotInit);
    np_up(g);
    CHECK(g.call("sceNpBasicSendMessage", kTo, kAttachment, 16) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicSendMessageGui", kMsg, 0) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicAddFriend", kTo, kBody, 0) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicRecvMessageCustom", 3, 0, 0) == kBasicNotRegistered);
    REQUIRE(g.call("sceNpBasicRegisterContextSensitiveHandler", kComm, kHandler, 0) == 0);

    // A message to one user: a recipient with an online ID, data, at most 512 bytes; then it can't be sent offline.
    CHECK(g.call("sceNpBasicSendMessage", 0, kAttachment, 16) == kBasicInval);
    CHECK(g.call("sceNpBasicSendMessage", kNobody, kAttachment, 16) == kBasicInval);
    CHECK(g.call("sceNpBasicSendMessage", kTo, 0, 16) == kBasicInval);
    CHECK(g.call("sceNpBasicSendMessage", kTo, kAttachment, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicSendMessage", kTo, kAttachment, 513) == kBasicExceeds);
    CHECK(g.call("sceNpBasicSendMessage", kTo, kAttachment, 512) == kBasicNotConnected);
    CHECK(g.call("sceNpBasicSendMessage", kUnmapped, kAttachment, 16) == kBasicInval); // RPCS3 faults

    // The send dialog: RPCS3's checks of the message, then, offline, no dialog. The base message: an attachment
    // (main type 0, sub type ACTION_USE) for two users, with a subject and a body.
    const auto send = [&](const Message& m) {
        put_message(b, kMsg, m);
        return g.call("sceNpBasicSendMessageGui", kMsg, 0);
    };
    const auto attachment = [&] {
        Message m;
        m.npids = kNpIds;
        m.count = 2;
        m.subject = kSubject;
        m.body = kBody;
        m.data = kAttachment;
        m.size = 100;
        return m;
    };
    const auto send_changed = [&](auto change) {
        Message m = attachment();
        change(m);
        return send(m);
    };
    CHECK(g.call("sceNpBasicSendMessageGui", 0, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicSendMessageGui", kUnmapped, 0) == kBasicInval); // RPCS3 faults
    CHECK(send(attachment()) == kBasicNotConnected);

    // The header: no message ID, at most 12 recipients, known features and main types.
    CHECK(send_changed([](Message& m) { m.msg_id = 1; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.count = 13; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.features = 8; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.features = 0x10000; }) == kBasicNotConnected); // an EXP_MIN bit
    CHECK(send_changed([](Message& m) { m.main_type = 6; }) == kBasicInval);

    // Sub types: 0 for every main type, and ACTION_ACCEPT (1) for invitations.
    CHECK(send_changed([](Message& m) { m.sub_type = 1; }) == kBasicNotSupported);
    CHECK(send_changed([](Message& m) { m.main_type = 4, m.sub_type = 1; }) == kBasicNotSupported);
    CHECK(send_changed([](Message& m) { m.main_type = 5, m.sub_type = 1; }) == kBasicNotSupported);
    CHECK(send_changed([](Message& m) { m.main_type = 3, m.sub_type = 2; }) == kBasicNotSupported);
    CHECK(send_changed([](Message& m) { m.main_type = 3, m.sub_type = 1; }) == kBasicNotConnected);
    // A general message takes no data, but RPCS3 wants data in every message but a friend request.
    CHECK(send_changed([](Message& m) { m.main_type = 1, m.sub_type = 1; }) == kBasicNotSupported);
    CHECK(send_changed([](Message& m) { m.main_type = 1; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.main_type = 1, m.data = 0; }) == kBasicInval);
    // Bootable: an accepted invitation or custom data only.
    CHECK(send_changed([](Message& m) { m.features = 2, m.main_type = 3; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.features = 2, m.main_type = 3, m.sub_type = 1; }) == kBasicNotConnected);
    CHECK(send_changed([](Message& m) { m.features = 2, m.main_type = 4; }) == kBasicNotConnected);
    CHECK(send_changed([](Message& m) { m.features = 2, m.main_type = 4, m.sub_type = 1; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.features = 2; }) == kBasicInval);
    // Data and its size.
    CHECK(send_changed([](Message& m) { m.data = 0; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.size = 0; }) == kBasicInval);

    // A friend request: one recipient, no data, no subject; RPCS3 still wants a size.
    const auto friend_request = [](Message& m) {
        m.main_type = 2;
        m.count = 1;
        m.data = 0;
        m.subject = 0;
    };
    CHECK(send_changed([&](Message& m) { friend_request(m), m.size = 0; }) == kBasicInval);
    CHECK(send_changed([&](Message& m) { friend_request(m), m.size = 1; }) == kBasicNotConnected);
    CHECK(send_changed([&](Message& m) { friend_request(m), m.size = 1, m.count = 2; }) == kBasicInval);
    CHECK(send_changed([&](Message& m) { friend_request(m), m.size = 1, m.subject = kSubject; }) == kBasicInval);
    CHECK(send_changed([&](Message& m) { friend_request(m), m.size = 1, m.data = kAttachment; }) == kBasicInval);

    // Every recipient given needs an online ID.
    put_np_id(b, kNpIds + 0x24, "");
    CHECK(send(attachment()) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.count = 1; }) == kBasicNotConnected);
    put_np_id(b, kNpIds + 0x24, "player-two");
    CHECK(send_changed([](Message& m) { m.npids = 0; }) == kBasicNotConnected);
    CHECK(send_changed([](Message& m) { m.npids = kUnmapped; }) == kBasicInval); // RPCS3 faults

    // The subject: at most 18 characters, the body at most 512, of UTF-8 whose lead bytes RPCS3 checks.
    const auto with_subject = [&](std::string_view text) {
        put(b, kText, text);
        return send_changed([](Message& m) { m.subject = kText; });
    };
    const auto with_body = [&](std::string_view text) {
        put(b, kText, text);
        return send_changed([](Message& m) { m.body = kText; });
    };
    CHECK(with_subject(std::string(18, 's')) == kBasicNotConnected);
    CHECK(with_subject(std::string(19, 's')) == kBasicExceeds);
    std::string two_bytes; // 18 characters in 36 bytes
    for (int i = 0; i < 18; i++) two_bytes += "\xC3\xA9";
    CHECK(with_subject(two_bytes) == kBasicNotConnected);
    CHECK(with_subject(two_bytes + "\xC3\xA9") == kBasicExceeds);
    CHECK(with_subject("\xE3\x81\x82\xF0\x9F\x98\x80") == kBasicNotConnected); // three bytes, four bytes
    CHECK(with_subject("") == kBasicNotConnected);
    CHECK(with_subject("\x80") == kUtilInvalChar);     // a continuation byte first
    CHECK(with_subject("\xC0\x80") == kUtilInvalChar); // an overlong two-byte lead
    CHECK(with_subject("\xE3\x81") == kUtilInvalChar); // cut short by the terminator
    CHECK(with_subject("\xF8\x80\x80\x80\x80") == kUtilInvalChar);
    CHECK(with_body(std::string(512, 'b')) == kBasicNotConnected);
    CHECK(with_body(std::string(513, 'b')) == kBasicExceeds);
    CHECK(with_body("ok\xFF") == kUtilInvalChar);
    CHECK(send_changed([](Message& m) { m.subject = kUnmapped; }) == kBasicInval); // RPCS3 faults
    // A body that runs into memory that isn't guest memory.
    constexpr u32 kEnd = kData + 0x300000; // the end of the fixture's memory
    REQUIRE_FALSE(f.mem.is_committed(kEnd));
    std::memset(b + kEnd - 4, 'z', 4);
    CHECK(send_changed([](Message& m) { m.body = kEnd - 4; }) == kBasicInval);

    // Data sizes per main type: attachments and custom data 1 MB, invitations 1 KB, URLs 2 KB.
    CHECK(send_changed([](Message& m) { m.size = 1048576; }) == kBasicNotConnected);
    CHECK(send_changed([](Message& m) { m.size = 1048577; }) == kBasicExceeds);
    CHECK(send_changed([](Message& m) { m.main_type = 4, m.size = 1048577; }) == kBasicExceeds);
    CHECK(send_changed([](Message& m) { m.main_type = 3, m.size = 1024; }) == kBasicNotConnected);
    CHECK(send_changed([](Message& m) { m.main_type = 3, m.size = 1025; }) == kBasicExceeds);
    CHECK(send_changed([](Message& m) { m.main_type = 5, m.size = 2048; }) == kBasicNotConnected);
    CHECK(send_changed([](Message& m) { m.main_type = 5, m.size = 2049; }) == kBasicExceeds);

    // "Assume send" needs recipients, a body and, but for a friend request, a subject; no URLs.
    CHECK(send_changed([](Message& m) { m.features = 4; }) == kBasicNotConnected);
    CHECK(send_changed([](Message& m) { m.features = 4, m.count = 0; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.features = 4, m.body = 0; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.features = 4, m.subject = 0; }) == kBasicInval);
    CHECK(send_changed([](Message& m) { m.features = 4, m.main_type = 5; }) == kBasicInval);
    CHECK(send_changed([&](Message& m) { friend_request(m), m.size = 1, m.features = 4; }) == kBasicNotConnected);

    // sceNpBasicAddFriend sends a friend request, which has no data: RPCS3's checks refuse its size of 0.
    CHECK(g.call("sceNpBasicAddFriend", kTo, kBody, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicAddFriend", 0, 0, 0) == kBasicInval);

    // Receiving: invitations or custom data (a 16-bit main type), known options; offline the list is empty, so the
    // user can only cancel.
    CHECK(g.call("sceNpBasicRecvMessageCustom", 0, 0, 0) == kBasicNotSupported);
    CHECK(g.call("sceNpBasicRecvMessageCustom", 2, 0, 0) == kBasicNotSupported);
    CHECK(g.call("sceNpBasicRecvMessageCustom", 3, 8, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicRecvMessageCustom", 3, 7, 0) == kBasicCancel);
    CHECK(g.call("sceNpBasicRecvMessageCustom", 4, 0, 0) == kBasicCancel);
    CHECK(g.call("sceNpBasicRecvMessageCustom", 0x10003, 0, 0) == kBasicCancel);

    // None of this queued an event for the basic handler (whose address here holds no code).
    CHECK(g.call("cellSysutilCheckCallback") == 0);
}

TEST_CASE("sceNpBasicAddPlayersHistoryAsync: RPCS3's checks, request IDs, the result for the basic handler", "[hle][np]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    write_event_handler(b);
    constexpr u32 kComm = kData + 0x100, kNpIds = kData + 0x200, kDesc = kData + 0x400, kReqId = kData + 0x600;
    put_comm_id(b, kComm, "NPWR01234", 0);
    for (u32 i = 0; i < 9; i++) put_np_id(b, kNpIds + i * 0x24, std::format("player-{}", i));
    const auto add = [&](u32 npids, u32 count, u32 description, u32 req_id) {
        return g.call("sceNpBasicAddPlayersHistoryAsync", npids, count, description, req_id);
    };
    const auto events = [&] { return ps_read32(b, kEventOut + 16); };

    CHECK(add(kNpIds, 2, 0, kReqId) == kBasicNotInit);
    np_up(g);
    // 1 to 8 players, each with an online ID, and a description of at most 341 bytes; no basic handler is needed.
    CHECK(add(kNpIds, 0, 0, kReqId) == kBasicInval);
    CHECK(add(kNpIds, 9, 0, kReqId) == kBasicExceeds);
    CHECK(add(0, 1, 0, kReqId) == kBasicInval);
    CHECK(add(kUnmapped, 1, 0, kReqId) == kBasicInval); // RPCS3 faults
    put_np_id(b, kNpIds + 0x24, "");
    CHECK(add(kNpIds, 2, 0, kReqId) == kBasicInval);
    put_np_id(b, kNpIds + 0x24, "player-1");
    put(b, kDesc, std::string(342, 'd'));
    CHECK(add(kNpIds, 2, kDesc, kReqId) == kBasicExceeds);
    CHECK(add(kNpIds, 2, kUnmapped, kReqId) == kBasicInval); // RPCS3 faults
    CHECK(add(kNpIds, 2, 0, kUnmapped) == kBasicInval);      // RPCS3 faults writing the request ID

    // Without a basic handler: a request ID (REQUEST_ID_HIGH::MISC, then a count from 1) and no event.
    put(b, kDesc, std::string(341, 'd'));
    ps_write32(b, kReqId, 0);
    REQUIRE(add(kNpIds, 8, kDesc, kReqId) == 0);
    CHECK(ps_read32(b, kReqId) == 0x33330001);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(events() == 0);

    // With one: the result (event ADD_PLAYERS_HISTORY_RESULT 18, result 0, the request ID, the handler's argument) at
    // the game's next check, once.
    REQUIRE(g.call("sceNpBasicRegisterContextSensitiveHandler", kComm, kEventOpd, 0xABCD) == 0);
    REQUIRE(add(kNpIds, 2, 0, kReqId) == 0);
    CHECK(ps_read32(b, kReqId) == 0x33330002);
    CHECK(events() == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(events() == 1);
    CHECK(ps_read32(b, kEventOut) == 18);
    CHECK(ps_read32(b, kEventOut + 4) == 0);
    CHECK(ps_read32(b, kEventOut + 8) == 0x33330002);
    CHECK(ps_read32(b, kEventOut + 12) == 0xABCD);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(events() == 1);

    // Without an address for the request ID: the request and its result all the same.
    REQUIRE(add(kNpIds + 7 * 0x24, 1, 0, 0) == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(events() == 2);
    CHECK(ps_read32(b, kEventOut + 8) == 0x33330003);

    // NP's term drops the handler but doesn't rewind the count.
    REQUIRE(g.call("sceNp2Term") == 0);
    np_up(g);
    REQUIRE(add(kNpIds, 1, 0, kReqId) == 0);
    CHECK(ps_read32(b, kReqId) == 0x33330004);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(events() == 2);
}

TEST_CASE("sceNpProfileCallGui: NP and a handler, then the user's NP ID, which isn't there offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    constexpr u32 kNpId = kData + 0x100, kHandler = 0x00012340;
    CHECK(g.call("sceNpProfileCallGui", kNpId, kHandler, 0, 0) == kProfileNotInit);
    np_up(g);
    CHECK(g.call("sceNpProfileCallGui", kNpId, 0, 0, 0) == kProfileInval);
    CHECK(g.call("sceNpProfileCallGui", kNpId, kHandler, 0x55, 0) == kOffline);
    CHECK(g.call("sceNpProfileCallGui", 0, kHandler, 0, ~0ull) == kOffline); // the NP ID and options aren't checked
}

TEST_CASE("sceNpSignalingActivateConnection: RPCS3's checks, then no connection offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kOwn = kData + 0x140, kCtx = kData + 0x180, kConn = kData + 0x190,
                  kStatus = kData + 0x1A0;
    put_np_id(b, kNpId, "somebody");
    put_np_id(b, kOwn, "");

    CHECK(g.call("sceNpSignalingActivateConnection", 0x6001, kNpId, kConn) == kSigNotInit);
    np_up(g);
    // The context first, then both pointers, then not the user's own NP ID (offline: an empty online ID).
    CHECK(g.call("sceNpSignalingActivateConnection", 0x6001, kNpId, kConn) == kSigCtxNotFound);
    CHECK(g.call("sceNpSignalingActivateConnection", 0x6001, 0, 0) == kSigCtxNotFound);
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, 0x00012340, 0x5678, kCtx) == 0);
    REQUIRE(ps_read32(b, kCtx) == 0x6001);
    CHECK(g.call("sceNpSignalingActivateConnection", 0x6001, 0, kConn) == kSigInval);
    CHECK(g.call("sceNpSignalingActivateConnection", 0x6001, kNpId, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingActivateConnection", 0x6001, kOwn, kConn) == kSigOwnNpId);
    CHECK(g.call("sceNpSignalingActivateConnection", 0x6001, kUnmapped, kConn) == kSigInval); // RPCS3 faults

    // RPCS3 would ask RPCN for the peer's address now (and aborts without it): the peer can't be reached, and no
    // connection is made or written.
    ps_write32(b, kConn, 0xAAAA);
    CHECK(g.call("sceNpSignalingActivateConnection", 0x6001, kNpId, kConn) == kSigPeerUnreachable);
    CHECK(ps_read32(b, kConn) == 0xAAAA);
    CHECK(g.call("sceNpSignalingGetConnectionFromNpId", 0x6001, kNpId, kConn) == kSigConnNotFound);
    CHECK(g.call("sceNpSignalingGetConnectionStatus", 0x6001, 1, kStatus, 0, 0) == kSigConnNotFound);
}

TEST_CASE("sceNpDrmIsAvailable: the file at the path as RPCS3 checks it, and the key of content that is available",
    "[hle][np]")
{
    DrmDirs dirs;
    const std::string id = "UP0001-BLES01807_00-PSPOCKETTEST0001";
    const fsys::path usrdir = dirs.hdd0 / "game" / "BLES01807" / "USRDIR";
    DrmDirs::write(dirs.bdvd / "PS3_GAME" / "USRDIR" / "plain.dat", {1, 2, 3, 4, 5});
    DrmDirs::write(dirs.bdvd / "PS3_GAME" / "USRDIR" / "empty.dat", {});
    DrmDirs::write(dirs.bdvd / "PS3_GAME" / "USRDIR" / "sce3", {'S', 'C', 'E'});
    DrmDirs::write(usrdir / "data.sdat", npd_file(0, id, 0x01000000));
    DrmDirs::write(usrdir / "short.edat", {'N', 'P', 'D', 0, 0, 0});
    std::vector<u8> sce(0x200, 0);
    std::memcpy(sce.data(), "SCE", 4);
    DrmDirs::write(usrdir / "module.sprx", sce);
    // EDATs with the hashes RPCS3 checks, made with the build's keys (in memory and the test's folders only).
    const recomp::NpdKeys& npd_keys = recomp::builtin_npd_keys();
    const recomp::NpdrmKeys& keys = recomp::builtin_npdrm_keys();
    recomp::Key128 klicensee{}, rap{};
    klicensee.fill(0x11);
    rap.fill(0x22);
    const auto edat = [&](s32 license, std::string_view content_id, std::string_view name, const recomp::Key128& key) {
        test::NpdOptions o{.version = 3, .extra_flags = 0, .block_size = 0x400};
        o.edata = true;
        o.license = license;
        o.content_id = std::string(content_id);
        o.file_name = std::string(name);
        o.key = key;
        o.npdrm = &keys;
        return test::build_npd(std::vector<u8>(3000, 0x5A), npd_keys, o);
    };
    if (keys.present) {
        const std::vector<u8> dlc = edat(2, id, "dlc.edat", recomp::rap_to_rif(rap, keys));
        DrmDirs::write(usrdir / "dlc.edat", dlc);
        DrmDirs::write(usrdir / "renamed.edat", dlc);
        DrmDirs::write(usrdir / "network.edat", edat(1, "UP0001-BLES01807_00-PSPOCKETTEST0002", "network.edat", rap));
        DrmDirs::write(usrdir / "free.edat", edat(3, id, "free.edat", klicensee));
        DrmDirs::write(usrdir / "badid.edat", edat(2, "../x", "badid.edat", rap));
    }

    Fixture f;
    dirs.mount(f);
    GuestThread g(f, 0, "drm");
    u8* b = f.base();
    constexpr u32 kKlicensee = kData + 0x100, kOtherKlicensee = kData + 0x110, kPath = kData + 0x1000;
    std::memcpy(b + kKlicensee, klicensee.data(), 16);
    std::memset(b + kOtherKlicensee, 0x12, 16);
    const auto check = [&](std::string_view path, u32 klicensee_addr = kKlicensee) {
        put(b, kPath, path);
        return g.call("sceNpDrmIsAvailable", klicensee_addr, kPath);
    };
    const std::string game = "/dev_hdd0/game/BLES01807/USRDIR/";

    // NP need not be up. A path is needed; RPCS3 faults on memory that isn't guest memory.
    CHECK(g.call("sceNpDrmIsAvailable", kKlicensee, 0) == kDrmInvalParam);
    CHECK(g.call("sceNpDrmIsAvailable", kKlicensee, kUnmapped) == kDrmInvalParam);
    CHECK(check("/dev_bdvd/PS3_GAME/USRDIR/plain.dat", kUnmapped) == kDrmInvalParam);

    // A file that can't be opened: lv2's error.
    CHECK(check("/dev_bdvd/PS3_GAME/USRDIR/missing.edat") == r3_of(lv2::CELL_ENOENT));
    CHECK(check("/dev_bdvd/PS3_GAME/USRDIR") == r3_of(lv2::CELL_EISDIR));
    CHECK(check("/dev_usb000/dlc.edat") == r3_of(lv2::CELL_ENOTMOUNTED));
    CHECK(check("dlc.edat") == r3_of(lv2::CELL_ENOENT));

    // Neither a SELF nor an EDAT: unencrypted, available, with or without a klicensee.
    CHECK(check("/dev_bdvd/PS3_GAME/USRDIR/plain.dat") == 0);
    CHECK(g.call("sceNpDrmIsAvailable2", kKlicensee, kPath) == 0); // the same check (RPCS3)
    CHECK(g.call("sceNpDrmIsAvailable2", kKlicensee, 0) == kDrmInvalParam);
    CHECK(check("/dev_bdvd/PS3_GAME/USRDIR/plain.dat", 0) == 0);
    CHECK(check("/dev_bdvd/PS3_GAME/USRDIR/empty.dat") == 0);
    CHECK(check("/dev_bdvd/PS3_GAME/USRDIR/sce3") == 0); // too short for a magic

    // An SDAT: RPCS3's EDAT check refuses it. A SELF must open with the build's keys: this one is no SELF.
    CHECK(check(game + "data.sdat") == kDrmNoEntitlement);
    CHECK(check(game + "module.sprx") == kDrmNoEntitlement);

    // At most 0x100 characters of the path are read.
    const std::string name(0x100 - std::string_view("/dev_hdd0/").size(), 'a');
    DrmDirs::write(dirs.hdd0 / name, {1});
    CHECK(check("/dev_hdd0/" + name + "xyz") == 0);
    CHECK(check("/dev_hdd0/" + name.substr(1) + "xyz") == r3_of(lv2::CELL_ENOENT));

    if (!keys.present) SKIP("this build has no NPDRM keys: EDATs are not checked");
    hle::NpdrmKeyRing* ring = hle::npdrm_key_ring();
    REQUIRE(ring);
    REQUIRE_FALSE(ring->newest()); // nothing above installed a key

    // EDATs. The header checks first (RPCS3 VerifyEDATHeaderWithKLicense): one renamed since it was made, or too
    // short to have its hashes, is no entitlement.
    CHECK(check(game + "renamed.edat") == kDrmNoEntitlement);
    CHECK(check(game + "short.edat") == kDrmNoEntitlement);
    // One that needs a licence (types 1 and 2) the player hasn't put in exdata (none looked for when the content ID
    // isn't valid).
    CHECK(check(game + "dlc.edat") == kDrmLicenseNotFound);
    CHECK(check(game + "network.edat") == kDrmLicenseNotFound);
    CHECK(check(game + "badid.edat") == kDrmLicenseNotFound);
    // A RAP under 16 bytes is a bad format; with the player's RAP, available, and its RIF key installed.
    const fsys::path rap_file = dirs.hdd0 / "home" / "00000001" / "exdata" / (id + ".rap");
    DrmDirs::write(rap_file, std::vector<u8>(8, 0x22));
    CHECK(check(game + "dlc.edat") == kDrmBadFormat);
    DrmDirs::write(rap_file, std::vector<u8>(rap.begin(), rap.end()));
    CHECK(check(game + "dlc.edat") == 0);
    REQUIRE(ring->newest());
    CHECK(*ring->newest() == recomp::rap_to_rif(rap, keys));
    // Free content: its dev hash must match the klicensee the game gives (none is zeros).
    CHECK(check(game + "free.edat", kOtherKlicensee) == kDrmNoEntitlement);
    CHECK(check(game + "free.edat", 0) == kDrmNoEntitlement);
    CHECK(check(game + "free.edat") == 0);
    CHECK(*ring->newest() == klicensee);
    CHECK(ring->newest_first().size() == 2);
}

TEST_CASE("EDATA files: lv2 opens them with the keys sceNpDrmIsAvailable installed, newest first (D-581)",
    "[hle][np][fs]")
{
    const recomp::NpdKeys& npd_keys = recomp::builtin_npd_keys();
    const recomp::NpdrmKeys& keys = recomp::builtin_npdrm_keys();
    if (!keys.present || !npd_keys.present) SKIP("this build has no NPD keys");
    DrmDirs dirs;
    const std::string id = "UP0001-NPXX00000_00-PSPOCKETTEST0001";
    const fsys::path usrdir = dirs.hdd0 / "game" / "NPXX00000" / "USRDIR";
    std::vector<u8> plain(70000);
    for (usize i = 0; i < plain.size(); i++) plain[i] = static_cast<u8>(i * 13 + (i >> 8));
    recomp::Key128 klicensee{}, other{}, rap{};
    klicensee.fill(0x31);
    other.fill(0x32);
    rap.fill(0x41);
    const auto edat = [&](s32 license, std::string_view name, const recomp::Key128& key, u32 flags) {
        test::NpdOptions o{.version = 4, .extra_flags = flags, .block_size = 0x4000};
        o.edata = true;
        o.license = license;
        o.content_id = id;
        o.file_name = std::string(name);
        o.key = key;
        o.npdrm = &keys;
        DrmDirs::write(usrdir / std::string(name), test::build_npd(plain, npd_keys, o));
    };
    edat(3, "free.edat", klicensee, recomp::kNpdEncryptedKeyFlag | recomp::kNpdFlag10);
    edat(2, "local.edat", recomp::rap_to_rif(rap, keys), recomp::kNpdCompressedFlag);
    edat(3, "other.edat", other, 0);
    DrmDirs::write(dirs.hdd0 / "home" / "00000001" / "exdata" / (id + ".rap"), std::vector<u8>(rap.begin(), rap.end()));

    Fixture f;
    dirs.mount(f);
    GuestThread g(f, 0, "edat");
    u8* b = f.base();
    constexpr u32 kKlicensee = kData + 0x100, kArg = kData + 0x200, kFd = kData + 0x300, kNread = kData + 0x308,
                  kStat = kData + 0x400, kPath = kData + 0x1000, kBuf = kData + 0x10000;
    std::memcpy(b + kKlicensee, klicensee.data(), 16);
    ps_write64(b, kArg, 2); // sys_fs_open's type argument: EDATA
    const std::string game = "/dev_hdd0/game/NPXX00000/USRDIR/";
    const auto open_edata = [&](const std::string& name) {
        put(b, kPath, game + name);
        return g.call("cellFsOpen", kPath, 0, kFd, kArg, 8);
    };
    const auto read_all = [&](const std::string& name) {
        REQUIRE(open_edata(name) == 0);
        const u32 fd = ps_read32(b, kFd);
        REQUIRE(g.call("cellFsFstat", fd, kStat) == 0);
        CHECK(ps_read64(b, kStat + 36) == plain.size()); // the plain size
        REQUIRE(g.call("cellFsRead", fd, kBuf, 0x20000, kNread) == 0);
        CHECK(ps_read64(b, kNread) == plain.size());
        CHECK(std::memcmp(b + kBuf, plain.data(), plain.size()) == 0);
        CHECK(g.call("cellFsClose", fd) == 0);
    };

    // Before the game shows a key, nothing opens it (RPCS3: none of the 0 keys); as SDATA it never does.
    CHECK(open_edata("free.edat") == r3_of(lv2::CELL_EFSSPECIFIC));
    put(b, kPath, game + "free.edat");
    CHECK(g.call("cellFsSdataOpen", kPath, 0, kFd, 0, 0) == r3_of(lv2::CELL_EFSSPECIFIC));

    // The free file with the game's klicensee, the local one with the player's RAP; each key stays for the other.
    put(b, kPath, game + "free.edat");
    REQUIRE(g.call("sceNpDrmIsAvailable", kKlicensee, kPath) == 0);
    read_all("free.edat");
    put(b, kPath, game + "local.edat");
    REQUIRE(g.call("sceNpDrmIsAvailable2", 0, kPath) == 0);
    read_all("local.edat");
    read_all("free.edat");
    // One whose klicensee the game never showed still does not open.
    CHECK(open_edata("other.edat") == r3_of(lv2::CELL_EFSSPECIFIC));
}

TEST_CASE("sceNpDrmVerifyUpgradeLicense: an unlock licence RPCS3's checks accept makes the trial the full game (D-581)",
    "[hle][np]")
{
    const recomp::NpdKeys& npd_keys = recomp::builtin_npd_keys();
    const recomp::NpdrmKeys& keys = recomp::builtin_npdrm_keys();
    if (!keys.present || !npd_keys.present) SKIP("this build has no NPD keys");
    DrmDirs dirs;
    const fsys::path exdata = dirs.hdd0 / "home" / "00000001" / "exdata";
    // An unlock licence (RPCS3 verify_c00_unlock_edat): a free EDAT made with NP_KLIC_FREE and named after its
    // content ID, holding "GOMA" and the content ID at 0x10.
    const auto unlock = [&](const std::string& id, std::string_view magic, const std::string& inner_id) {
        std::vector<u8> plain(0x100, 0);
        std::memcpy(plain.data(), magic.data(), 4);
        std::memcpy(plain.data() + 0x10, inner_id.data(), std::min<usize>(inner_id.size(), 0x30));
        test::NpdOptions o{.version = 3, .extra_flags = 0, .block_size = 0x400};
        o.edata = true;
        o.license = 3;
        o.content_id = id;
        o.file_name = id + ".edat";
        o.key = keys.klic_free;
        o.npdrm = &keys;
        DrmDirs::write(exdata / (id + ".edat"), test::build_npd(plain, npd_keys, o));
    };
    const std::string full = "UP0001-NPXX00000_00-FULLGAME00000001";
    const std::string wrong_magic = "UP0001-NPXX00000_00-FULLGAME00000002";
    const std::string wrong_inner = "UP0001-NPXX00000_00-FULLGAME00000003";
    unlock(full, "GOMA", full);
    unlock(wrong_magic, "GOMB", wrong_magic);
    unlock(wrong_inner, "GOMA", full);

    Fixture f;
    dirs.mount(f);
    GuestThread g(f, 0, "upgrade");
    u8* b = f.base();
    constexpr u32 kId = kData + 0x100;
    for (const char* name : {"sceNpDrmVerifyUpgradeLicense", "sceNpDrmVerifyUpgradeLicense2"}) {
        INFO(name);
        put(b, kId, full);
        CHECK(g.call(name, kId) == 0);
        put(b, kId, wrong_magic);
        CHECK(g.call(name, kId) == kDrmLicenseNotFound);
        put(b, kId, wrong_inner);
        CHECK(g.call(name, kId) == kDrmLicenseNotFound);
    }
    // A licence copied under another content's name fails RPCS3's header checks.
    fsys::copy_file(exdata / (full + ".edat"), exdata / "UP0001-NPXX00000_00-FULLGAME00000004.edat");
    put(b, kId, "UP0001-NPXX00000_00-FULLGAME00000004");
    CHECK(g.call("sceNpDrmVerifyUpgradeLicense", kId) == kDrmLicenseNotFound);
}

TEST_CASE("NpdrmKeyRing: sixteen keys, the newest first, the oldest given up (RPCS3 loaded_npdrm_keys)", "[hle][np]")
{
    hle::NpdrmKeyRing ring;
    CHECK_FALSE(ring.newest());
    CHECK(ring.newest_first().empty());
    for (u8 i = 0; i < 20; i++) {
        recomp::Key128 k{};
        k.fill(i);
        ring.install(k);
    }
    const auto keys = ring.newest_first();
    REQUIRE(keys.size() == 16);
    CHECK(keys.front()[0] == 19);
    CHECK(keys.back()[0] == 4);
    CHECK((*ring.newest())[0] == 19);
}

namespace {

constexpr u64 kBasicBusy = 0xFFFFFFFF8002A66Full;
constexpr u64 kFriendlistNotInit = 0xFFFFFFFF8002AB21ull, kFriendlistInval = 0xFFFFFFFF8002AB28ull;
constexpr u64 kMenuNotInit = 0xFFFFFFFF80023B02ull, kMenuInval = 0xFFFFFFFF80023B09ull,
              kMenuNotRegistered = 0xFFFFFFFF80023B0Bull, kMenuExceeds = 0xFFFFFFFF80023B0Cull;
constexpr u64 kUtilUnknownPlatform = 0xFFFFFFFF8002AB10ull;
constexpr u64 kDrmTimeLimit = 0xFFFFFFFF8002951Bull, kDrmNotStarted = 0xFFFFFFFF8002951Eull;

// A guest function that returns 0 at once, behind the descriptor kNopOpd (a handler RPCS3 never calls offline).
constexpr u32 kNopCode = kData + 0x22000, kNopOpd = kData + 0x22100;

void write_nop(u8* b)
{
    ps_write32(b, kNopCode, as::li(3, 0));
    ps_write32(b, kNopCode + 4, as::blr());
    ps_write32(b, kNopOpd, kNopCode);
    ps_write32(b, kNopOpd + 4, 0x00123400);
}

// The milliseconds since 1970 now, as sceNpDrmGetTimelimit reads the clock.
s64 now_ms()
{
    s64 sec = 0, nsec = 0;
    lv2::host_current_time(sec, nsec);
    return sec * 1000 + nsec / 1'000'000;
}

// An EDAT's start whose NPD header has this validity window.
std::vector<u8> npd_window(s64 activate, s64 expire)
{
    std::vector<u8> v = npd_file(2, "UP0001-BLES01807_00-PSPOCKETTEST0001", 0);
    ps_write64(v.data(), 0x70, static_cast<u64>(activate));
    ps_write64(v.data(), 0x78, static_cast<u64>(expire));
    return v;
}

// A SELF's headers: the extended header's supplemental headers at 0x100 (a type 1 one of 0x30 bytes, then, when
// `npdrm`, a type 3 one holding an NPD header with this expiry), and `key_revision`.
std::vector<u8> self_with_npd(bool npdrm, s64 expire, u16 key_revision = 0)
{
    std::vector<u8> v(0x400, 0);
    std::memcpy(v.data(), "SCE", 4);
    ps_write16(v.data(), 8, key_revision);
    ps_write64(v.data(), 0x58, 0x100);
    ps_write64(v.data(), 0x60, npdrm ? 0x30 + 0x90 : 0x30);
    ps_write32(v.data(), 0x100, 1);
    ps_write32(v.data(), 0x104, 0x30);
    if (npdrm) {
        ps_write32(v.data(), 0x130, 3);
        ps_write32(v.data(), 0x134, 0x90);
        std::memcpy(v.data() + 0x140, "NPD", 4);
        ps_write64(v.data(), 0x140 + 0x78, static_cast<u64>(expire));
    }
    return v;
}

} // namespace

TEST_CASE("sceNpBasic offline: lists not found, presence not connected, friends busy, players' history; RPCS3's checks",
    "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kNpId = kData + 0x200, kOut = kData + 0x300, kText = kData + 0x400,
                  kPres = kData + 0x800, kBody = kData + 0xC00, kAttach = kData + 0xD00;
    write_nop(b);
    put_comm_id(b, kComm, "NPWR01234", 0);
    put_np_id(b, kNpId, "friend");
    std::memset(b + kOut, 0xAA, 0x40);
    const auto out_untouched = [&] { return ps_read32(b, kOut) == 0xAAAAAAAAu; };

    // Without NP. The friend presence functions check the details pointer first, the message entry its type.
    CHECK(g.call("sceNpBasicRegisterHandler", kComm, kNopOpd, 0) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetClanMessageEntryCount", kOut) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetFriendListEntry", 0, kOut) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetFriendPresenceByIndex", 0, kOut, 0, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetFriendPresenceByNpId2", kNpId, kOut, 0) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetMessageEntry", 8, 0, kOut) == kBasicInval);
    CHECK(g.call("sceNpBasicGetMessageEntry", 7, 0, kOut) == kBasicNotInit);
    CHECK(g.call("sceNpBasicAddPlayersHistory", kNpId, 0) == kBasicNotInit);
    CHECK(g.call("sceNpBasicGetPlayersHistoryEntryCount", 2, kOut) == kBasicInval);
    CHECK(g.call("sceNpBasicGetPlayersHistoryEntryCount", 1, kOut) == kBasicNotInit);
    np_up(g);

    // Before a handler: those that need one refuse; the friend list and messages don't need one.
    for (const char* name : {"sceNpBasicGetClanMessageEntryCount", "sceNpBasicGetCustomInvitationEntryCount",
             "sceNpBasicGetMatchingInvitationEntryCount", "sceNpBasicGetMessageAttachmentEntryCount"}) {
        INFO(name);
        CHECK(g.call(name, kOut) == kBasicNotRegistered);
    }
    CHECK(g.call("sceNpBasicAbortGui") == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicMarkMessageAsUsed", 1) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicRecvMessageAttachment", 0) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicGetPlayersHistoryEntry", 0, 0, kOut) == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicGetFriendListEntryCount", kOut) == kIdNotFound);
    CHECK(g.call("sceNpBasicGetMessageEntry", 0, 0, kOut) == kIdNotFound);
    // A player for the history needs no handler; it isn't kept (only listed online).
    CHECK(g.call("sceNpBasicAddPlayersHistory", 0, 0) == kBasicInval);
    put_np_id(b, kNpId, "");
    CHECK(g.call("sceNpBasicAddPlayersHistory", kNpId, 0) == kBasicInval);
    put_np_id(b, kNpId, "friend");
    CHECK(g.call("sceNpBasicAddPlayersHistory", kUnmapped, 0) == kBasicInval); // RPCS3 faults
    put(b, kText, std::string(342, 'd'));
    CHECK(g.call("sceNpBasicAddPlayersHistory", kNpId, kText) == kBasicExceeds);
    put(b, kText, std::string(341, 'd'));
    CHECK(g.call("sceNpBasicAddPlayersHistory", kNpId, kText) == 0);
    CHECK(g.call("sceNpBasicAddPlayersHistory", kNpId, 0) == 0);

    // One handler, from either registration function.
    REQUIRE(g.call("sceNpBasicRegisterHandler", kComm, kNopOpd, 0x1234) == 0);
    CHECK(g.call("sceNpBasicRegisterHandler", kComm, kNopOpd, 0) == kBasicExceeds);
    CHECK(g.call("sceNpBasicRegisterContextSensitiveHandler", kComm, kNopOpd, 0) == kBasicExceeds);

    // The lists from PSN: a pointer is needed, then nothing is found offline and nothing is written.
    for (const char* name : {"sceNpBasicGetClanMessageEntryCount", "sceNpBasicGetCustomInvitationEntryCount",
             "sceNpBasicGetMatchingInvitationEntryCount", "sceNpBasicGetMessageAttachmentEntryCount",
             "sceNpBasicGetFriendListEntryCount"}) {
        INFO(name);
        CHECK(g.call(name, 0) == kBasicInval);
        CHECK(g.call(name, kOut) == kIdNotFound);
    }
    for (const char* name : {"sceNpBasicGetClanMessageEntry", "sceNpBasicGetCustomInvitationEntry",
             "sceNpBasicGetMatchingInvitationEntry", "sceNpBasicGetMessageAttachmentEntry"}) {
        INFO(name);
        CHECK(g.call(name, 0, 0) == kBasicInval);
        CHECK(g.call(name, 3, kOut) == kIdNotFound);
    }
    CHECK(g.call("sceNpBasicGetMessageEntry", 1, 0, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetMessageEntry", 1, 0, kOut) == kIdNotFound);
    for (const u32 options : {0u, 1u}) {
        CHECK(g.call("sceNpBasicGetPlayersHistoryEntryCount", options, 0) == kBasicInval);
        CHECK(g.call("sceNpBasicGetPlayersHistoryEntryCount", options, kOut) == kIdNotFound);
        CHECK(g.call("sceNpBasicGetPlayersHistoryEntry", options, 0, kOut) == kIdNotFound);
    }
    CHECK(g.call("sceNpBasicGetPlayersHistoryEntry", 2, 0, kOut) == kBasicInval);
    CHECK(g.call("sceNpBasicGetFriendListEntry", 0, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicGetFriendListEntry", 0, kOut) == kBasicBusy);
    CHECK(out_untouched());

    // Friends' presence comes from RPCN: not connected.
    for (const char* name : {"sceNpBasicGetFriendPresenceByIndex", "sceNpBasicGetFriendPresenceByIndex2"}) {
        INFO(name);
        CHECK(g.call(name, 0, kOut, 0, 0) == kBasicInval);
        CHECK(g.call(name, 0, 0, kPres, 0) == kBasicInval);
        CHECK(g.call(name, 0, kOut, kPres, 0) == kBasicNotConnected);
    }
    for (const char* name : {"sceNpBasicGetFriendPresenceByNpId", "sceNpBasicGetFriendPresenceByNpId2"}) {
        INFO(name);
        CHECK(g.call(name, 0, kPres, 0) == kBasicInval);
        CHECK(g.call(name, kNpId, kPres, 3) == kBasicNotConnected);
    }

    // The block list is PSN's: an NP ID with an online ID, then not connected.
    CHECK(g.call("sceNpBasicAddBlockListEntry", 0) == kBasicInval);
    CHECK(g.call("sceNpBasicAddBlockListEntry", kUnmapped) == kBasicInval); // RPCS3 faults
    put_np_id(b, kNpId, "");
    CHECK(g.call("sceNpBasicAddBlockListEntry", kNpId) == kBasicInval);
    put_np_id(b, kNpId, "friend");
    CHECK(g.call("sceNpBasicAddBlockListEntry", kNpId) == kBasicNotConnected);

    // Presence details (the older struct): the options, then the data's size (at most 128).
    std::memset(b + kPres, 0, 392);
    put(b, kPres + 128, "Racing");
    CHECK(g.call("sceNpBasicSetPresenceDetails", kPres, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicSetPresenceDetails", kPres, 4) == kBasicInval);
    CHECK(g.call("sceNpBasicSetPresenceDetails", kUnmapped, 3) == kBasicInval); // RPCS3 ensure()s
    ps_write32(b, kPres + 384, 129);
    CHECK(g.call("sceNpBasicSetPresenceDetails", kPres, 1) == kBasicExceeds);
    ps_write32(b, kPres + 384, 128);
    CHECK(g.call("sceNpBasicSetPresenceDetails", kPres, 3) == 0);
    CHECK(g.call("sceNpBasicSetPresenceDetails", kPres, 2) == 0);

    // Nothing to abort or mark; the attachments to receive: none (cancelled); one to send: not connected.
    CHECK(g.call("sceNpBasicAbortGui") == 0);
    CHECK(g.call("sceNpBasicMarkMessageAsUsed", 0x123456789ull) == 0);
    CHECK(g.call("sceNpBasicRecvMessageAttachment", 0x1234) == kBasicCancel);
    put(b, kText, "Subject");
    put(b, kBody, "A body");
    std::memset(b + kAttach, 7, 16);
    CHECK(g.call("sceNpBasicSendMessageAttachment", kNpId, kText, kBody, kAttach, 16, 0) == kBasicNotConnected);
    CHECK(g.call("sceNpBasicSendMessageAttachment", kNpId, kText, kBody, 0, 16, 0) == kBasicInval); // no data
    CHECK(g.call("sceNpBasicSendMessageAttachment", kNpId, kText, kBody, kAttach, 0, 0) == kBasicInval);
    CHECK(g.call("sceNpBasicSendMessageAttachment", kNpId, kText, kBody, kAttach, 1048577, 0) == kBasicExceeds);

    // Unregistered again: the checks that want a handler refuse.
    REQUIRE(g.call("sceNpBasicUnregisterHandler") == 0);
    CHECK(g.call("sceNpBasicAbortGui") == kBasicNotRegistered);
    CHECK(g.call("sceNpBasicGetClanMessageEntry", 0, kOut) == kBasicNotRegistered);
}

TEST_CASE("sceNpManager offline: the user's IDs and languages, tickets; cached info, sign-in ID, sub sign-in",
    "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kOut = kData + 0x100, kNpId = kData + 0x400, kService = kData + 0x500;
    const char* const queries[] = {"sceNpManagerGetOnlineId", "sceNpManagerGetPsHandle", "sceNpManagerGetOnlineName",
        "sceNpManagerGetAvatarUrl", "sceNpManagerGetMyLanguages"};
    for (const char* name : queries) CHECK(g.call(name, kOut) == kNotInit);
    CHECK(g.call("sceNpManagerGetCachedInfo", 0, kOut) == kNotInit);
    CHECK(g.call("sceNpManagerGetSigninId", kOut) == kNotInit);
    CHECK(g.call("sceNpManagerSubSignin", 1, 0, 0, 0) == kNotInit);
    CHECK(g.call("sceNpManagerSubSigninAbortGui") == kNotInit);
    CHECK(g.call("sceNpManagerSubSignout", 0) == kNotInit);
    np_up(g);

    // The signed-in user's details: offline, and nothing written.
    std::memset(b + kOut, 0xAA, 0x200);
    for (const char* name : queries) {
        INFO(name);
        CHECK(g.call(name, 0) == kInval);
        CHECK(g.call(name, kOut) == kOffline);
    }
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAAu);

    // A ticket (version 2.1): RPCS3's checks, then offline.
    put_np_id(b, kNpId, "");
    put(b, kService, "IV0001-NPXS00001_00");
    CHECK(g.call("sceNpManagerRequestTicket", 0, kService, 0, 0, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket", kNpId, 0, 0, 0, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket", kNpId, kService, 0, 1025, 0, 0) == kAuthInval);
    CHECK(g.call("sceNpManagerRequestTicket", kNpId, kService, 0, 1024, 0, 0) == kOffline);

    // The cached account details: the current user (0) or user 1, with every field empty; others don't exist.
    CHECK(g.call("sceNpManagerGetCachedInfo", 0, 0) == kInval);
    CHECK(g.call("sceNpManagerGetCachedInfo", 2, kOut) == r3_of(lv2::CELL_ENOENT));
    CHECK(g.call("sceNpManagerGetCachedInfo", 0, kUnmapped) == kInval); // RPCS3 faults
    for (const u32 user : {0u, 1u}) {
        std::memset(b + kOut, 0xAA, 0x200);
        REQUIRE(g.call("sceNpManagerGetCachedInfo", user, kOut) == 0);
        CHECK(ps_read32(b, kOut) == 240);
        CHECK(std::all_of(b + kOut + 4, b + kOut + 240, [](u8 x) { return x == 0; }));
        CHECK(b[kOut + 240] == 0xAA);
    }

    // The sign-in ID: 64 zero bytes.
    CHECK(g.call("sceNpManagerGetSigninId", 0) == kInval);
    std::memset(b + kOut, 0xAA, 0x200);
    REQUIRE(g.call("sceNpManagerGetSigninId", kOut) == 0);
    CHECK(std::all_of(b + kOut, b + kOut + 64, [](u8 x) { return x == 0; }));
    CHECK(b[kOut + 64] == 0xAA);

    // Sub sign-in: RPCS3's TODOs answer CELL_OK.
    CHECK(g.call("sceNpManagerSubSignin", 1, 0x1000, 0, 0) == 0);
    CHECK(g.call("sceNpManagerSubSigninAbortGui") == 0);
    CHECK(g.call("sceNpManagerSubSignout", kNpId) == 0);
}

TEST_CASE("sceNpFriendlist and the custom menu: RPCS3's checks; the user's NP ID isn't there offline", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kMenu = kData + 0x100, kActions = kData + 0x200, kNames = kData + 0x400, kArray = kData + 0x800,
                  kExceptions = kData + 0x1000;
    write_nop(b);
    CHECK(g.call("sceNpFriendlist", kNopOpd, 0, 0) == kFriendlistNotInit);
    CHECK(g.call("sceNpFriendlistCustom", 0, kNopOpd, 0, 0) == kFriendlistNotInit);
    CHECK(g.call("sceNpFriendlistAbortGui") == kFriendlistNotInit);
    CHECK(g.call("sceNpProfileAbortGui") == kFriendlistNotInit); // RPCS3: the friend list's error
    CHECK(g.call("sceNpCustomMenuRegisterActions", kMenu, kNopOpd, 0, 0) == kMenuNotInit);
    CHECK(g.call("sceNpCustomMenuActionSetActivation", kArray, 0) == kMenuNotInit);
    CHECK(g.call("sceNpCustomMenuRegisterExceptionList", kExceptions, 1, 0) == kMenuNotInit);
    np_up(g);

    // The friend list dialog: a handler, then the user's NP ID, which isn't there offline.
    CHECK(g.call("sceNpFriendlist", 0, 0, 0) == kFriendlistInval);
    CHECK(g.call("sceNpFriendlist", kNopOpd, 0, 0) == kOffline);
    CHECK(g.call("sceNpFriendlistCustom", 1, 0, 0, 0) == kFriendlistInval);
    CHECK(g.call("sceNpFriendlistCustom", 1, kNopOpd, 0, 0) == kOffline);
    CHECK(g.call("sceNpFriendlistAbortGui") == 0);
    CHECK(g.call("sceNpProfileAbortGui") == 0);

    // The custom menu: nothing before its actions are registered.
    CHECK(g.call("sceNpCustomMenuActionSetActivation", kArray, 0) == kMenuNotRegistered);
    CHECK(g.call("sceNpCustomMenuRegisterExceptionList", kExceptions, 1, 0) == kMenuNotRegistered);
    // A menu of two actions: names of at most 20 characters, at most 16 actions.
    const auto put_menu = [&](u32 actions, u32 count) {
        std::memset(b + kMenu, 0, 16);
        ps_write32(b, kMenu + 8, actions);
        ps_write32(b, kMenu + 12, count);
    };
    const auto put_action = [&](u32 i, u32 name, u32 mask) {
        ps_write32(b, kActions + 12 * i, 0);
        ps_write32(b, kActions + 12 * i + 4, name);
        ps_write32(b, kActions + 12 * i + 8, mask);
    };
    put(b, kNames, "Invite");
    put(b, kNames + 0x40, std::string(20, 'n'));
    put_action(0, kNames, 2);
    put_action(1, kNames + 0x40, 7);
    CHECK(g.call("sceNpCustomMenuRegisterActions", 0, kNopOpd, 0, 0) == kMenuInval);
    put_menu(kActions, 2);
    CHECK(g.call("sceNpCustomMenuRegisterActions", kMenu, 0, 0, 0) == kMenuInval);
    CHECK(g.call("sceNpCustomMenuRegisterActions", kUnmapped, kNopOpd, 0, 0) == kMenuInval); // RPCS3 faults
    put_menu(kActions, 17);
    CHECK(g.call("sceNpCustomMenuRegisterActions", kMenu, kNopOpd, 0, 0) == kMenuExceeds);
    put_menu(0, 2);
    CHECK(g.call("sceNpCustomMenuRegisterActions", kMenu, kNopOpd, 0, 0) == kMenuInval);
    put_menu(kActions, 2);
    put_action(1, 0, 7);
    CHECK(g.call("sceNpCustomMenuRegisterActions", kMenu, kNopOpd, 0, 0) == kMenuInval);
    put(b, kNames + 0x80, std::string(21, 'n'));
    put_action(1, kNames + 0x80, 7);
    CHECK(g.call("sceNpCustomMenuRegisterActions", kMenu, kNopOpd, 0, 0) == kMenuExceeds);
    CHECK(g.call("sceNpCustomMenuActionSetActivation", kArray, 0) == kMenuNotRegistered); // nothing registered yet
    put_action(1, kNames + 0x40, 7);
    REQUIRE(g.call("sceNpCustomMenuRegisterActions", kMenu, kNopOpd, 0x55, 0) == 0);
    put_menu(0, 0); // no actions at all is a menu too
    CHECK(g.call("sceNpCustomMenuRegisterActions", kMenu, kNopOpd, 0, 0) == 0);

    // Which actions are active, and the exceptions (at most 256).
    CHECK(g.call("sceNpCustomMenuActionSetActivation", 0, 0) == kMenuInval);
    CHECK(g.call("sceNpCustomMenuActionSetActivation", kUnmapped, 0) == kMenuInval); // RPCS3 faults
    std::memset(b + kArray, 0xFF, 16);
    CHECK(g.call("sceNpCustomMenuActionSetActivation", kArray, 0) == 0);
    CHECK(g.call("sceNpCustomMenuRegisterExceptionList", kExceptions, 257, 0) == kMenuExceeds);
    CHECK(g.call("sceNpCustomMenuRegisterExceptionList", 0, 1, 0) == kMenuInval);
    CHECK(g.call("sceNpCustomMenuRegisterExceptionList", 0, 0, 0) == kMenuInval);
    CHECK(g.call("sceNpCustomMenuRegisterExceptionList", kExceptions, 256, 0) == 0);
    CHECK(g.call("sceNpCustomMenuRegisterExceptionList", kExceptions, 0, 0) == 0);
}

TEST_CASE("sceNpUtil: online IDs compared, NP IDs ordered, platform types", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kId1 = kData + 0x100, kId2 = kData + 0x200, kOrder = kData + 0x300;
    // NP need not be up.
    put_np_id(b, kId1, "alice");
    put_np_id(b, kId2, "alice");
    b[kId2 + 20] = 9; // another opt word: still the same online ID
    CHECK(g.call("sceNpUtilCmpOnlineId", 0, kId2) == kUtilInval);
    CHECK(g.call("sceNpUtilCmpOnlineId", kId1, kId2) == 0);
    put_np_id(b, kId2, "bob");
    CHECK(g.call("sceNpUtilCmpOnlineId", kId1, kId2) == kUtilNotMatch);

    // The order: online IDs, then the first opt word, then the platforms.
    const auto order = [&](u32 a, u32 c) {
        ps_write32(b, kOrder, 0x55555555);
        REQUIRE(g.call("sceNpUtilCmpNpIdInOrder", a, c, kOrder) == 0);
        return static_cast<s32>(ps_read32(b, kOrder));
    };
    CHECK(g.call("sceNpUtilCmpNpIdInOrder", kId1, 0, kOrder) == kUtilInval);
    CHECK(order(kId1, kId2) == -1);
    CHECK(order(kId2, kId1) == 1);
    put_np_id(b, kId2, "alice");
    CHECK(order(kId1, kId2) == 0);
    b[kId1 + 23] = 1; // the first opt word
    CHECK(order(kId1, kId2) == 1);
    b[kId1 + 23] = 0;
    std::memcpy(b + kId1 + 24, "ps3", 4); // one platform: against "ps3"
    CHECK(order(kId1, kId2) == 0);
    std::memcpy(b + kId1 + 24, "ps4", 4);
    CHECK(order(kId1, kId2) == 1);
    CHECK(order(kId2, kId1) == 1);
    std::memcpy(b + kId2 + 24, "psp2", 4); // both: compared to each other
    CHECK(order(kId1, kId2) == -1);
    CHECK(order(kId2, kId1) == 1);

    // Canonicalizing changes nothing (RPCS3 keeps its checks commented out).
    CHECK(g.call("sceNpUtilCanonicalizeNpIdForPs3", 0) == kUtilInval);
    CHECK(g.call("sceNpUtilCanonicalizeNpIdForPsp", 0) == kUtilInval);
    CHECK(g.call("sceNpUtilCanonicalizeNpIdForPs3", kId1) == 0);
    CHECK(g.call("sceNpUtilCanonicalizeNpIdForPsp", kId1) == 0);
    CHECK(std::memcmp(b + kId1 + 24, "ps4", 4) == 0);

    // Platform types: the word at +24.
    CHECK(g.call("sceNpUtilGetPlatformType", 0) == kUtilInval);
    CHECK(g.call("sceNpUtilSetPlatformType", 0, 1) == kUtilInval);
    CHECK(g.call("sceNpUtilGetPlatformType", kId1) == 3);
    CHECK(g.call("sceNpUtilGetPlatformType", kId2) == 2);
    REQUIRE(g.call("sceNpUtilSetPlatformType", kId1, 1) == 0);
    CHECK(std::memcmp(b + kId1 + 24, "ps3", 4) == 0);
    CHECK(g.call("sceNpUtilGetPlatformType", kId1) == 1);
    CHECK(g.call("sceNpUtilSetPlatformType", kId1, 4) == kUtilUnknownPlatform);
    REQUIRE(g.call("sceNpUtilSetPlatformType", kId1, 0) == 0);
    CHECK(ps_read32(b, kId1 + 24) == 0);
    CHECK(g.call("sceNpUtilGetPlatformType", kId1) == 0);
    std::memcpy(b + kId1 + 24, "abcd", 4);
    CHECK(g.call("sceNpUtilGetPlatformType", kId1) == kUtilUnknownPlatform);
}

TEST_CASE("sceNpDrm: time limits from EDAT and SELF headers, exitspawn's check, upgrade licences", "[hle][np]")
{
    DrmDirs dirs;
    const fsys::path usrdir = dirs.hdd0 / "game" / "BLES01807" / "USRDIR";
    const s64 now = now_ms();
    DrmDirs::write(usrdir / "endless.edat", npd_window(0, 0));
    DrmDirs::write(usrdir / "started.edat", npd_window(now - 60'000, now + 3'600'000));
    DrmDirs::write(usrdir / "expired.edat", npd_window(0, now - 1000));
    DrmDirs::write(usrdir / "future.edat", npd_window(now + 3'600'000, 0));
    DrmDirs::write(usrdir / "rental.self", self_with_npd(true, now + 7'200'000));
    DrmDirs::write(usrdir / "plain.self", self_with_npd(false, 0));
    DrmDirs::write(usrdir / "debug.self", self_with_npd(true, 0, 0x8000));
    DrmDirs::write(usrdir / "plain.dat", {1, 2, 3, 4});
    const std::string id = "UP0001-BLES01807_00-PSPOCKETTEST0001";
    DrmDirs::write(dirs.hdd0 / "home" / "00000001" / "exdata" / (id + ".edat"), npd_file(3, id, 0));

    Fixture f;
    dirs.mount(f);
    GuestThread g(f, 0, "drm");
    u8* b = f.base();
    constexpr u32 kPath = kData + 0x1000, kRemain = kData + 0x100;
    const std::string game = "/dev_hdd0/game/BLES01807/USRDIR/";
    const auto limit = [&](const std::string& name) {
        put(b, kPath, game + name);
        ps_write64(b, kRemain, 0x5555);
        return g.call("sceNpDrmGetTimelimit", kPath, kRemain);
    };

    // The arguments, then the file.
    put(b, kPath, game + "endless.edat");
    CHECK(g.call("sceNpDrmGetTimelimit", 0, kRemain) == kDrmInvalParam);
    CHECK(g.call("sceNpDrmGetTimelimit", kPath, 0) == kDrmInvalParam);
    CHECK(g.call("sceNpDrmGetTimelimit", kUnmapped, kRemain) == kDrmInvalParam); // RPCS3 aborts
    CHECK(g.call("sceNpDrmGetTimelimit", kPath, kUnmapped) == kDrmInvalParam);
    CHECK(limit("missing.edat") == r3_of(lv2::CELL_ENOENT));
    CHECK(limit("plain.dat") == kDrmBadFormat);

    // An EDAT's window.
    REQUIRE(limit("endless.edat") == 0);
    CHECK(ps_read64(b, kRemain) == 0x7FFFFFFFFFFFFFFFull);
    REQUIRE(limit("started.edat") == 0);
    CHECK(ps_read64(b, kRemain) <= 3'600'000'000ull);
    CHECK(ps_read64(b, kRemain) > 3'500'000'000ull);
    CHECK(limit("expired.edat") == kDrmTimeLimit);
    CHECK(limit("future.edat") == kDrmNotStarted);
    CHECK(ps_read64(b, kRemain) == 0x5555); // nothing written on an error

    // A SELF's, from its NPDRM supplemental header; without one (or a debug SELF), a bad format.
    REQUIRE(limit("rental.self") == 0);
    CHECK(ps_read64(b, kRemain) <= 7'200'000'000ull);
    CHECK(ps_read64(b, kRemain) > 7'100'000'000ull);
    CHECK(limit("plain.self") == kDrmBadFormat);
    CHECK(limit("debug.self") == kDrmBadFormat);

    // Exitspawn checks the content first; a failed check is the result and the game goes on.
    put(b, kPath, game + "missing.self");
    CHECK(g.call("sceNpDrmProcessExitSpawn", 0, kPath, 0, 0, 0, 0, 1001, 0) == r3_of(lv2::CELL_ENOENT));
    CHECK(g.call("sceNpDrmProcessExitSpawn2", 0, kPath, 0, 0, 0, 0, 1001, 0) == r3_of(lv2::CELL_ENOENT));
    CHECK(g.call("sceNpDrmProcessExitSpawn", 0, 0, 0, 0, 0, 0, 1001, 0) == kDrmInvalParam);
    // (This EDAT has no hashes: RPCS3's header check refuses it, where the build has the keys to make that check;
    // without them, the licence it needs is missing.)
    put(b, kPath, game + "endless.edat");
    CHECK(g.call("sceNpDrmProcessExitSpawn", 0, kPath, 0, 0, 0, 0, 1001, 0) ==
        (recomp::builtin_npdrm_keys().present ? kDrmNoEntitlement : kDrmLicenseNotFound));

    // The upgrade licence: the user's unlock EDAT, which can't be verified here.
    const auto upgrade = [&](const char* name, std::string_view content_id) {
        put(b, kPath, content_id);
        return g.call(name, kPath);
    };
    for (const char* name : {"sceNpDrmVerifyUpgradeLicense", "sceNpDrmVerifyUpgradeLicense2"}) {
        INFO(name);
        CHECK(g.call(name, 0) == kDrmInvalParam);
        CHECK(g.call(name, kUnmapped) == kDrmInvalParam); // RPCS3 aborts
        CHECK(upgrade(name, "not/valid") == kDrmLicenseNotFound);
        CHECK(upgrade(name, "UP0001-BLES01807_00-PSPOCKETTEST0002") == kDrmLicenseNotFound);
        CHECK(upgrade(name, id) == kDrmLicenseNotFound);
    }
}

TEST_CASE("sceNpTerm: NP down, its signaling contexts gone; their IDs go on", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kCtx = kData + 0x200;
    CHECK(g.call("sceNpTerm") == kNotInit);
    REQUIRE(g.call("sceNpInit", 0x20000, kPool) == 0);
    put_np_id(b, kNpId, "");
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, 0, 0, kCtx) == 0);
    const u32 first = ps_read32(b, kCtx);
    REQUIRE(g.call("sceNpTerm") == 0);
    CHECK(g.call("sceNpTerm") == kNotInit);
    CHECK(g.call("sceNpSignalingDestroyCtx", first) == kSigNotInit);
    CHECK(g.call("sceNpManagerGetStatus", kCtx) == kNotInit);
    REQUIRE(g.call("sceNpInit", 0x20000, kPool) == 0);
    CHECK(g.call("sceNpSignalingDestroyCtx", first) == kSigCtxNotFound);
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, 0, 0, kCtx) == 0);
    CHECK(ps_read32(b, kCtx) == first + 1);
    REQUIRE(g.call("sceNpTerm") == 0);
}

TEST_CASE("sceNpLookup's PSN calls refused offline; sceNpScore: title contexts kept, every transaction refused",
    "[hle][np]")
{
    constexpr u64 kComTooManyNpId = 0xFFFFFFFF8002A1ADull, kComTooLargeRange = 0xFFFFFFFF8002A1AEull;
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kPass = kData + 0x200, kNpId = kData + 0x300, kOut = kData + 0x400,
                  kText = kData + 0x800, kOpt = kData + 0xC00;
    put_comm_id(b, kComm, "NPWR01234", 0);
    put_np_id(b, kNpId, "");
    std::memset(b + kOut, 0xAA, 0x40);

    // Lookup: the library first.
    CHECK(g.call("sceNpLookupNpId", 1, kNpId, kOut, 0) == kComNotInit);
    CHECK(g.call("sceNpLookupWaitAsync", 1, kOut) == kComNotInit);
    CHECK(g.call("sceNpLookupAbortTransaction", 1) == kComNotInit);
    CHECK(g.call("sceNpLookupTitleStorage") == kComNotInit);
    CHECK(g.call("sceNpScoreInit") == kNotInit); // the score library needs NP
    np_up(g);
    REQUIRE(g.call("sceNpLookupInit") == 0);

    // The pointers each needs, a null option, then offline.
    CHECK(g.call("sceNpLookupNpId", 1, 0, kOut, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupNpId", 1, kNpId, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupNpId", 1, kNpId, kOut, kOpt) == kComInval);
    CHECK(g.call("sceNpLookupNpId", 1, kNpId, kOut, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupTitleSmallStorage", 1, 0, 16, kOut, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupTitleSmallStorage", 1, kOut, 16, kOut, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupAvatarImage", 1, kText, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupAvatarImage", 1, kText, kOut, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupAvatarImageAsync", 1, kText, kOut, 1000, kOpt) == kComInval);
    CHECK(g.call("sceNpLookupAvatarImageAsync", 1, kText, kOut, 1000, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupUserProfile", 1, 0, kOut, 0, 0, 0, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupUserProfile", 1, kNpId, 0, 0, 0, 0, 0, kOpt) == kComInval);
    CHECK(g.call("sceNpLookupUserProfile", 1, kNpId, 0, 0, 0, 0, 0, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupUserProfileAsync", 1, kNpId, kOut, 0, 0, 0, 0, 1000, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpLookupUserProfileWithAvatarSize", 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpLookupUserProfileWithAvatarSize", 1, 0, kNpId, 0, 0, 0, 0, kOut, 64, kOut, kOpt) == kComInval);
    CHECK(g.call("sceNpLookupUserProfileWithAvatarSize", 1, 0, kNpId, 0, 0, 0, 0, kOut, 64, kOut, 0) ==
          kComInvalOnlineId);
    CHECK(g.call("sceNpLookupUserProfileWithAvatarSizeAsync", 1, 2, kNpId, 0, 0, 0, 0, kOut, 64, kOut, 1000, 0) ==
          kComInvalOnlineId);
    // RPCS3's TODOs: CELL_OK, nothing written.
    CHECK(g.call("sceNpLookupWaitAsync", 1, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAAu);
    CHECK(g.call("sceNpLookupAbortTransaction", 7) == 0);
    CHECK(g.call("sceNpLookupTitleStorage") == 0);
    CHECK(g.call("sceNpLookupTitleStorageAsync") == 0);

    // Score: once.
    CHECK(g.call("sceNpScoreCreateTitleCtx", kComm, kPass, kNpId) == kComNotInit);
    CHECK(g.call("sceNpScoreTerm") == kComNotInit);
    REQUIRE(g.call("sceNpScoreInit") == 0);
    CHECK(g.call("sceNpScoreInit") == kComAlready);

    // Title contexts: 0x2001 on, with a valid communication ID.
    CHECK(g.call("sceNpScoreCreateTitleCtx", 0, kPass, kNpId) == kComInsufficient);
    CHECK(g.call("sceNpScoreCreateTitleCtx", kComm, 0, kNpId) == kComInsufficient);
    CHECK(g.call("sceNpScoreCreateTitleCtx", kComm, kPass, 0) == kComInsufficient);
    put_comm_id(b, kComm, "npwr01234", 0);
    CHECK(g.call("sceNpScoreCreateTitleCtx", kComm, kPass, kNpId) == kComInval); // RPCS3 aborts
    CHECK(g.call("sceNpScoreCreateTitleCtx", kUnmapped, kPass, kNpId) == kComInval);
    put_comm_id(b, kComm, "NPWR01234", 0);
    CHECK(g.call("sceNpScoreCreateTitleCtx", kComm, kPass, kNpId) == 0x2001);
    CHECK(g.call("sceNpScoreCreateTitleCtx", kComm, kPass, kNpId) == 0x2002);

    // No transaction offline: refused at creation, unknown everywhere else.
    CHECK(g.call("sceNpScoreCreateTransactionCtx", 0x2001) == kComInvalOnlineId);
    for (const char* name : {"sceNpScoreDestroyTransactionCtx", "sceNpScoreAbortTransaction"}) {
        INFO(name);
        CHECK(g.call(name, 0x1001) == kComInvalId);
    }
    CHECK(g.call("sceNpScorePollAsync", 0x1001, kOut) == kComInvalId);
    CHECK(g.call("sceNpScoreWaitAsync", 0x1001, kOut) == kComInvalId);

    // The character ID (checked before the library) and the timeout (at least 10 s, a u64) of a title context.
    CHECK(g.call("sceNpScoreSetPlayerCharacterId", 0x2001, -1) == kComInval);
    CHECK(g.call("sceNpScoreSetPlayerCharacterId", 0x2001, 3) == 0);
    CHECK(g.call("sceNpScoreSetPlayerCharacterId", 0x1001, 3) == kComInvalId);
    CHECK(g.call("sceNpScoreSetTimeout", 0x2001, 9'999'999) == kComInval);
    CHECK(g.call("sceNpScoreSetTimeout", 0x2001, 10'000'000) == 0);
    CHECK(g.call("sceNpScoreSetTimeout", 0x2002, 0x100000000ull) == 0);
    CHECK(g.call("sceNpScoreSetTimeout", 0x2003, 10'000'000) == kComInvalId);

    // Board info and recording a score: the online check comes before the transaction.
    CHECK(g.call("sceNpScoreGetBoardInfo", 0x1001, 1, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreGetBoardInfo", 0x1001, 1, kOut, kOpt) == kComInval);
    CHECK(g.call("sceNpScoreGetBoardInfo", 0x1001, 1, kOut, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpScoreGetBoardInfoAsync", 0x1001, 1, kOut, 1000, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpScoreRecordScore", 0x1001, 1, -5, 0, 0, kOut, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpScoreRecordScoreAsync", 0x1001, 1, 12345, 0, 0, kOut, 1000, 0) == kComInvalOnlineId);

    // Game data and rankings: their pointers and counts, then the transaction.
    CHECK(g.call("sceNpScoreRecordGameData", 0x1001, 1, 10, 16, 16, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreRecordGameData", 0x1001, 1, 10, 16, 16, kOut, 0) == kComInvalId);
    CHECK(g.call("sceNpScoreRecordGameDataAsync", 0x1001, 1, 10, 16, 16, kOut, 1000, 0) == kComInvalId);
    CHECK(g.call("sceNpScoreGetGameData", 0x1001, 1, 0, kOut, 16, kOut, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreGetGameData", 0x1001, 1, kNpId, kOut, 16, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreGetGameData", 0x1001, 1, kNpId, kOut, 16, kOut, 0) == kComInvalId);
    CHECK(g.call("sceNpScoreGetGameDataAsync", 0x1001, 1, kNpId, kOut, 16, kOut, 1000, 0) == kComInvalId);
    for (const char* name : {"sceNpScoreGetRankingByNpId", "sceNpScoreGetRankingByNpIdPcId"}) {
        INFO(name);
        CHECK(g.call(name, 0x1001, 1, 0, 36, kOut, 64, 0, 0, 0, 0, 1, kOut, kOut, 0) == kComInsufficient);
        CHECK(g.call(name, 0x1001, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 0, kOut, kOut, 0) == kComInval);
        CHECK(g.call(name, 0x1001, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 102, kOut, kOut, 0) == kComTooManyNpId);
        CHECK(g.call(name, 0x1001, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 101, kOut, kOut, 0) == kComInvalId);
    }
    for (const char* name : {"sceNpScoreGetRankingByNpIdAsync", "sceNpScoreGetRankingByNpIdPcIdAsync"}) {
        INFO(name);
        CHECK(g.call(name, 0x1001, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 102, kOut, kOut, 1000, 0) == kComTooManyNpId);
        CHECK(g.call(name, 0x1001, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 1, kOut, kOut, 1000, 0) == kComInvalId);
    }
    CHECK(g.call("sceNpScoreGetRankingByRange", 0x1001, 1, 1, kOut, 64, 0, 0, 0, 0, 1, kOut, kOut, 0) == kComInvalId);
    CHECK(g.call("sceNpScoreGetRankingByRangeAsync", 0x1001, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1000, 0) == kComInvalId);
    CHECK(g.call("sceNpScoreGetFriendsRanking", 0x1001, 1, 1, kOut, 64, 0, 0, 0, 0, 1, kOut, kOut, 0) == kComInvalId);
    CHECK(g.call("sceNpScoreGetFriendsRankingAsync", 0x1001, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1000, 0) == kComInvalId);

    // Comments: at most 255 characters. Censoring needs PSN; sanitizing checks its output before the library.
    put(b, kText, std::string(256, 'c'));
    CHECK(g.call("sceNpScoreCensorComment", 0x1001, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreCensorComment", 0x1001, kText, 0) == kComInval);
    CHECK(g.call("sceNpScoreSanitizeComment", 0x1001, kText, kOut, 0) == kComInval);
    put(b, kText, std::string(255, 'c'));
    CHECK(g.call("sceNpScoreCensorComment", 0x1001, kText, kOpt) == kComInval);
    CHECK(g.call("sceNpScoreCensorComment", 0x1001, kText, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpScoreCensorCommentAsync", 0x1001, kText, 1000, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpScoreSanitizeComment", 0x1001, kText, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreSanitizeComment", 0x1001, 0, kOut, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreSanitizeComment", 0x1001, kText, kOut, 0) == kComInvalId);
    CHECK(g.call("sceNpScoreSanitizeCommentAsync", 0x1001, kText, kOut, 1000, 0) == kComInvalId);

    // Clans: the synchronous calls end offline; RPCS3's asynchronous ones answer CELL_OK after their checks.
    CHECK(g.call("sceNpScoreGetClanMemberGameData", 0x1001, 1, 7, kNpId, kOut, 16, 0, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreGetClanMemberGameData", 0x1001, 1, 7, kNpId, kOut, 16, kOut, kOpt) == kComInval);
    CHECK(g.call("sceNpScoreGetClanMemberGameData", 0x1001, 1, 7, kNpId, kOut, 16, kOut, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpScoreGetClanMemberGameDataAsync", 0x1001, 1, 7, 0, 0, 0, 0, 1000, kOpt) == kComInval);
    CHECK(g.call("sceNpScoreGetClanMemberGameDataAsync", 0x1001, 1, 7, 0, 0, 0, 0, 1000, 0) == 0);
    for (const char* name : {"sceNpScoreGetClansMembersRankingByNpId", "sceNpScoreGetClansMembersRankingByNpIdPcId"}) {
        INFO(name);
        CHECK(g.call(name, 0x1001, 7, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 0, 0, 1, 0, kOut, 0, 0) == kComInsufficient);
        CHECK(g.call(name, 0x1001, 7, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 0, 0, 1, 0, kOut, kOut, kOpt) == kComInval);
        CHECK(g.call(name, 0x1001, 7, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 0, 0, 102, 0, kOut, kOut, 0) ==
              kComTooManyNpId);
        CHECK(g.call(name, 0x1001, 7, 1, kNpId, 36, kOut, 64, 0, 0, 0, 0, 0, 0, 1, 0, kOut, kOut, 0) ==
              kComInvalOnlineId);
    }
    for (const char* name :
        {"sceNpScoreGetClansMembersRankingByNpIdAsync", "sceNpScoreGetClansMembersRankingByNpIdPcIdAsync"}) {
        INFO(name);
        CHECK(g.call(name, 0x1001, 7, 1, kNpId, 36, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1000, 0) == kComInsufficient);
        CHECK(g.call(name, 0x1001, 7, 1, kNpId, 36, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1000, kOpt) == kComInval);
        CHECK(g.call(name, 0x1001, 7, 1, kNpId, 36, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1000, 0) == 0);
    }
    CHECK(g.call("sceNpScoreGetClansMembersRankingByRange", 0x1001, 7, 1, 0, kOut, 64, 0, 0, 0, 0, 0, 0, 1, 0, kOut,
              kOut, 0) == kComInval); // no start rank
    CHECK(g.call("sceNpScoreGetClansMembersRankingByRange", 0x1001, 7, 1, 1, kOut, 64, 0, 0, 0, 0, 0, 0, 101, 0, kOut,
              kOut, 0) == kComTooLargeRange);
    CHECK(g.call("sceNpScoreGetClansMembersRankingByRange", 0x1001, 7, 1, 1, kOut, 64, 0, 0, 0, 0, 0, 0, 100, 0, kOut,
              kOut, 0) == kComInvalOnlineId);
    CHECK(g.call("sceNpScoreGetClansMembersRankingByRangeAsync", 0x1001, 7, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
              1000, 0) == kComInsufficient);
    CHECK(g.call("sceNpScoreGetClansMembersRankingByRangeAsync", 0x1001, 7, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0,
              1000, 0) == 0);
    CHECK(g.call("sceNpScoreGetClansRankingByClanId", 0x1001, 1, kOut, 4, kOut, 64, 0, 1, 0, 0, 1, kOut, kOut, 0) ==
          kComInval); // a reserved size
    CHECK(g.call("sceNpScoreGetClansRankingByClanId", 0x1001, 1, kOut, 4, kOut, 64, 0, 0, 0, 0, 102, kOut, kOut, 0) ==
          kComTooManyNpId);
    CHECK(g.call("sceNpScoreGetClansRankingByClanId", 0x1001, 1, kOut, 4, kOut, 64, 0, 0, 0, 0, 101, kOut, kOut, 0) ==
          kComInvalOnlineId);
    CHECK(g.call("sceNpScoreGetClansRankingByClanIdAsync", 0x1001, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1000, 0) == 0);
    CHECK(g.call("sceNpScoreGetClansRankingByRange", 0x1001, 1, 1, kOut, 64, 0, 0, 0, 0, 102, kOut, kOut, 0) ==
          kComTooLargeRange);
    CHECK(g.call("sceNpScoreGetClansRankingByRange", 0x1001, 1, 1, kOut, 64, 0, 0, 0, 0, 101, kOut, kOut, 0) ==
          kComInvalOnlineId);
    CHECK(g.call("sceNpScoreGetClansRankingByRangeAsync", 0x1001, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1000, 0) ==
          kComInval); // no start rank
    CHECK(g.call("sceNpScoreGetClansRankingByRangeAsync", 0x1001, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1000, 0) == 0);

    // Destroying a title context; term leaves the others.
    REQUIRE(g.call("sceNpScoreDestroyTitleCtx", 0x2001) == 0);
    CHECK(g.call("sceNpScoreDestroyTitleCtx", 0x2001) == kComInvalId);
    REQUIRE(g.call("sceNpScoreTerm") == 0);
    CHECK(g.call("sceNpScoreDestroyTitleCtx", 0x2002) == kComNotInit);
    REQUIRE(g.call("sceNpScoreInit") == 0);
    CHECK(g.call("sceNpScoreDestroyTitleCtx", 0x2002) == 0);
    CHECK(g.call("sceNpScoreCreateTitleCtx", kComm, kPass, kNpId) == 0x2003); // IDs go on
}

namespace {

constexpr u64 kMatchNotInit = 0xFFFFFFFF8002A701ull, kMatchInval = 0xFFFFFFFF8002A703ull,
              kMatchCtxNotFound = 0xFFFFFFFF8002A709ull, kMatchRoomNotFound = 0xFFFFFFFF8002A70Bull,
              kMatchInvalAttrId = 0xFFFFFFFF8002A70Full, kMatchCtxMax = 0xFFFFFFFF8002A716ull,
              kMatchInvalReqId = 0xFFFFFFFF8002A717ull, kMatchNoResult = 0xFFFFFFFF8002A718ull,
              kMatchBusy = 0xFFFFFFFF8002A719ull, kMatchCondMax = 0xFFFFFFFF8002A71Dull,
              kMatchInvalCond = 0xFFFFFFFF8002A71Eull, kMatchInvalAttr = 0xFFFFFFFF8002A71Full,
              kMatchInvalCompOp = 0xFFFFFFFF8002A725ull, kMatchInvalCompType = 0xFFFFFFFF8002A726ull,
              kMatchAttrNotSpecified = 0xFFFFFFFF8002A72Aull, kMatchInvalAttrValue = 0xFFFFFFFF8002A72Dull,
              kMatchDuplicate = 0xFFFFFFFF8002A72Eull, kMatchUnavailable = 0xFFFFFFFF8002A733ull;
constexpr u32 kMatchCanceled = 0x8002A723;

// A guest GUI handler (SceNpMatchingGUIHandler: ctx_id, event, error, arg) behind kGuiOpd: each call appends r3..r6
// (low words) to a log at kGuiLog + 16 (16 bytes an entry) and counts the calls at kGuiLog.
constexpr u32 kGuiCode = kData + 0x24000, kGuiOpd = kData + 0x24800, kGuiLog = kData + 0x25000;

void write_gui_handler(u8* b)
{
    const std::vector<u32> code = {
        as::lis(8, static_cast<s32>(kGuiLog >> 16)),
        as::ori(8, 8, kGuiLog & 0xFFFF),
        as::lwz(9, 0, 8),
        as::slwi(10, 9, 4),
        as::addi(10, 10, 16),
        as::stwx(3, 8, 10),
        as::addi(11, 10, 4),
        as::stwx(4, 8, 11),
        as::addi(11, 10, 8),
        as::stwx(5, 8, 11),
        as::addi(11, 10, 12),
        as::stwx(6, 8, 11),
        as::addi(9, 9, 1),
        as::stw(9, 0, 8),
        as::li(3, 0),
        as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kGuiCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kGuiOpd, kGuiCode);
    ps_write32(b, kGuiOpd + 4, 0x00123400);
    std::memset(b + kGuiLog, 0, 0x1000);
}

struct GuiCall {
    u32 ctx_id, event, error, arg;
    bool operator==(const GuiCall&) const = default;
};

// The calls logged since the last take, after the sysutil callbacks were checked.
std::vector<GuiCall> take_gui_calls(GuestThread& g, u8* b)
{
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    std::vector<GuiCall> calls;
    for (u32 i = 0; i < ps_read32(b, kGuiLog); i++) {
        const u32 e = kGuiLog + 16 + 16 * i;
        calls.push_back({ps_read32(b, e), ps_read32(b, e + 4), ps_read32(b, e + 8), ps_read32(b, e + 12)});
    }
    std::memset(b + kGuiLog, 0, 0x1000);
    return calls;
}

// A SceNpMatchingAttr (20 bytes) at `at`: next, type, id, then num (or a pointer and size).
void put_attr(u8* b, u32 at, u32 next, s32 type, u32 id, u32 num, u32 size = 0)
{
    ps_write32(b, at, next);
    ps_write32(b, at + 4, static_cast<u32>(type));
    ps_write32(b, at + 8, id);
    ps_write32(b, at + 12, num);
    ps_write32(b, at + 16, size);
}

// A SceNpMatchingSearchCondition (40 bytes) at `at`.
void put_cond(u8* b, u32 at, u32 next, s32 type, u32 id, s32 op, s32 comp_type = 0)
{
    std::memset(b + at, 0, 40);
    ps_write32(b, at, next);
    ps_write32(b, at + 4, static_cast<u32>(type));
    ps_write32(b, at + 8, id);
    ps_write32(b, at + 12, static_cast<u32>(op));
    ps_write32(b, at + 16, static_cast<u32>(comp_type));
}

} // namespace

TEST_CASE("sceNpMatching offline: one context; dialogs answered as cancelled after RPCS3's checks; no room is found",
    "[hle][np]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kCtxOut = kData + 0x200, kComm = kData + 0x300, kRoom = kData + 0x400,
                  kLobby = kData + 0x500, kReq = kData + 0x600, kSize = kData + 0x610, kRange = kData + 0x620,
                  kAttrs = kData + 0x1000, kConds = kData + 0x2000, kText = kData + 0x3000, kText2 = kData + 0x3200;
    constexpr u32 kArg = 0x00C0FFEE;
    write_gui_handler(b);
    write_nop(b);
    put_np_id(b, kNpId, "");
    put_comm_id(b, kComm, "NPWR01234", 0);
    std::memset(b + kRoom, 1, 36);
    std::memset(b + kLobby, 2, 36);

    // The result of a dialog needs no NP; a context does.
    CHECK(g.call("sceNpMatchingGetResultGUI", 0, 0, 0) == kMatchInval);
    CHECK(g.call("sceNpMatchingGetResultGUI", 0, kSize, 0) == kMatchNoResult);
    CHECK(g.call("sceNpMatchingCreateCtx", kNpId, kNopOpd, 0, kCtxOut) == kMatchNotInit);
    np_up(g);

    // One context, 0x9001.
    CHECK(g.call("sceNpMatchingCreateCtx", 0, kNopOpd, 0, kCtxOut) == kMatchInval);
    CHECK(g.call("sceNpMatchingCreateCtx", kNpId, 0, 0, kCtxOut) == kMatchInval);
    CHECK(g.call("sceNpMatchingCreateCtx", kNpId, kNopOpd, 0, 0) == kMatchInval);
    CHECK(g.call("sceNpMatchingCreateCtx", kUnmapped, kNopOpd, 0, kCtxOut) == kMatchInval); // RPCS3 faults
    REQUIRE(g.call("sceNpMatchingCreateCtx", kNpId, kNopOpd, 0, kCtxOut) == 0);
    const u32 ctx = ps_read32(b, kCtxOut);
    CHECK(ctx == 0x9001);
    CHECK(g.call("sceNpMatchingCreateCtx", kNpId, kNopOpd, 0, kCtxOut) == kMatchCtxMax);
    CHECK(g.call("sceNpMatchingGetResult", ctx, 1, 0, 0, 0) == kMatchInval);
    CHECK(g.call("sceNpMatchingGetResult", ctx + 1, 1, 0, kSize, 0) == kMatchCtxNotFound);
    CHECK(g.call("sceNpMatchingGetResult", ctx, 1, 0, kSize, 0) == kMatchInvalReqId);

    // Creating a room: the room's attributes as RPCS3 checks them, then the dialog, cancelled.
    const auto create = [&](u32 attrs) { return g.call("sceNpMatchingCreateRoomGUI", ctx, kComm, attrs, kGuiOpd, kArg); };
    CHECK(g.call("sceNpMatchingCreateRoomGUI", ctx, 0, kAttrs, kGuiOpd, kArg) == kMatchInval);
    CHECK(g.call("sceNpMatchingCreateRoomGUI", ctx, kComm, kAttrs, 0, kArg) == kMatchInval);
    CHECK(create(0) == kMatchAttrNotSpecified);
    put_attr(b, kAttrs, kAttrs + 0x20, 2, 1, 4); // total slots 4
    put_attr(b, kAttrs + 0x20, 0, 2, 2, 1);      // private slots 1
    put_attr(b, kAttrs + 0x40, 0, 1, 1, 0);      // a basic binary attribute
    CHECK(create(kAttrs + 0x40) == kMatchInvalAttrId);
    put_attr(b, kAttrs + 0x40, 0, 2, 3, 0); // a current count
    CHECK(create(kAttrs + 0x40) == kMatchInvalAttrId);
    put_attr(b, kAttrs + 0x40, 0, 2, 6, 2); // the privilege type is a flag
    CHECK(create(kAttrs + 0x40) == kMatchInvalAttrValue);
    put_attr(b, kAttrs + 0x40, 0, 3, 1, kText, 257); // game data 1 and 2: at most 256 bytes
    CHECK(create(kAttrs + 0x40) == kMatchInvalAttr);
    put_attr(b, kAttrs + 0x40, 0, 3, 3, kText, 65); // the others: 64
    CHECK(create(kAttrs + 0x40) == kMatchInvalAttr);
    put_attr(b, kAttrs + 0x40, kAttrs + 0x60, 4, 5, 9);
    put_attr(b, kAttrs + 0x60, 0, 4, 5, 9);
    CHECK(create(kAttrs + 0x40) == kMatchDuplicate);
    put_attr(b, kAttrs + 0x60, kAttrs + 0x40, 4, 6, 9); // a cycle
    CHECK(create(kAttrs + 0x40) == kMatchInval);
    CHECK(create(kUnmapped) == kMatchInval); // RPCS3 faults
    CHECK(create(kAttrs + 0x20) == kMatchAttrNotSpecified); // no total
    put_attr(b, kAttrs, kAttrs + 0x20, 2, 1, 17);
    CHECK(create(kAttrs) == kMatchInval); // at most 16 slots
    put_attr(b, kAttrs, kAttrs + 0x20, 2, 1, 1);
    CHECK(create(kAttrs) == kMatchInval); // no public slot left
    put_attr(b, kAttrs, kAttrs + 0x20, 2, 1, 4);
    CHECK(g.call("sceNpMatchingCreateRoomGUI", ctx + 1, kComm, kAttrs, kGuiOpd, kArg) == kMatchCtxNotFound);
    REQUIRE(create(kAttrs) == 0);
    CHECK(take_gui_calls(g, b) == std::vector<GuiCall>{{ctx, 8, 0, kArg}, {ctx, 1, kMatchCanceled, kArg},
                                      {ctx, 9, 0, kArg}});
    CHECK(g.call("sceNpMatchingGetResultGUI", 0, kSize, 0) == kMatchNoResult);

    // Joining a room.
    CHECK(g.call("sceNpMatchingJoinRoomGUI", ctx, 0, kGuiOpd, kArg) == kMatchInval);
    REQUIRE(g.call("sceNpMatchingJoinRoomGUI", ctx, kRoom, kGuiOpd, kArg) == 0);
    CHECK(take_gui_calls(g, b) == std::vector<GuiCall>{{ctx, 8, 0, kArg}, {ctx, 2, kMatchCanceled, kArg},
                                      {ctx, 9, 0, kArg}});

    // Quick match: 2 players or more, a timeout, game number conditions only, EQ or NE.
    const auto quick = [&](u32 conds) {
        return g.call("sceNpMatchingQuickMatchGUI", ctx, kComm, conds, 2, 10, kGuiOpd, kArg);
    };
    CHECK(g.call("sceNpMatchingQuickMatchGUI", ctx, kComm, 0, 1, 10, kGuiOpd, kArg) == kMatchInval);
    CHECK(g.call("sceNpMatchingQuickMatchGUI", ctx, kComm, 0, 2, 0, kGuiOpd, kArg) == kMatchInval);
    put_cond(b, kConds, 0, 4, 1, 2);
    CHECK(quick(kConds) == kMatchInvalCompOp);
    put_cond(b, kConds, 0, 4, 1, 1, 1);
    CHECK(quick(kConds) == kMatchInvalCompType);
    put_cond(b, kConds, 0, 2, 1, 0);
    CHECK(quick(kConds) == kMatchInvalCond);
    put_cond(b, kConds, 0, 4, 9, 0);
    CHECK(quick(kConds) == kMatchInvalCond);
    for (u32 i = 0; i < 10; i++) put_cond(b, kConds + 0x40 * i, i < 9 ? kConds + 0x40 * (i + 1) : 0, 4, 1, 0);
    CHECK(quick(kConds) == kMatchCondMax);
    put_cond(b, kConds + 0x40 * 8, 0, 4, 8, 1); // nine
    REQUIRE(quick(kConds) == 0);
    CHECK(take_gui_calls(g, b) == std::vector<GuiCall>{{ctx, 8, 0, kArg}, {ctx, 5, kMatchCanceled, kArg},
                                      {ctx, 9, 0, kArg}});

    // Search and join: value conditions EQ to GE; of the basic attributes only the total slots; then the attributes.
    const auto search = [&](u32 conds, u32 attrs) {
        return g.call("sceNpMatchingSearchJoinRoomGUI", ctx, kComm, conds, attrs, kGuiOpd, kArg);
    };
    put_cond(b, kConds, 0, 4, 1, 0, 1);
    CHECK(search(kConds, 0) == kMatchInvalCond);
    put_cond(b, kConds, 0, 4, 1, 6);
    CHECK(search(kConds, 0) == kMatchInvalCond);
    put_cond(b, kConds, 0, 2, 2, 5);
    CHECK(search(kConds, 0) == kMatchInvalCond);
    put_cond(b, kConds, 0, 2, 1, 5);
    put_attr(b, kAttrs + 0x40, kAttrs + 0x60, 4, 5, 9);
    put_attr(b, kAttrs + 0x60, 0, 4, 5, 9);
    CHECK(search(kConds, kAttrs + 0x40) == kMatchDuplicate);
    REQUIRE(search(kConds, kAttrs) == 0);
    CHECK(take_gui_calls(g, b) == std::vector<GuiCall>{{ctx, 8, 0, kArg}, {ctx, 4, kMatchCanceled, kArg},
                                      {ctx, 9, 0, kArg}});

    // The limited room list: a range that starts somewhere, at most 20; the total slots can't be a condition.
    const auto list = [&](u32 range, u32 conds) {
        return g.call("sceNpMatchingGetRoomListLimitGUI", ctx, kComm, range, conds, 0, kGuiOpd, kArg);
    };
    CHECK(list(0, 0) == kMatchInval);
    ps_write32(b, kRange, 0);
    ps_write32(b, kRange + 4, 10);
    CHECK(list(kRange, 0) == kMatchInval);
    ps_write32(b, kRange, 1);
    ps_write32(b, kRange + 4, 21);
    CHECK(list(kRange, 0) == kMatchInval);
    ps_write32(b, kRange + 4, 20);
    put_cond(b, kConds, 0, 2, 1, 0);
    CHECK(list(kRange, kConds) == kMatchInvalCond);
    put_cond(b, kConds, 0, 2, 2, 0);
    REQUIRE(list(kRange, kConds) == 0);
    CHECK(take_gui_calls(g, b) == std::vector<GuiCall>{{ctx, 8, 0, kArg}, {ctx, 11, kMatchCanceled, kArg},
                                      {ctx, 9, 0, kArg}});

    // Room requests: their checks, the context, then no room. The limited info calls take one call per 30 s.
    CHECK(g.call("sceNpMatchingGetRoomInfo", ctx, kLobby, kRoom, kAttrs, kReq) == kMatchRoomNotFound);
    CHECK(g.call("sceNpMatchingGetRoomInfo", ctx, kLobby, kRoom, kAttrs, kReq) == kMatchBusy);
    CHECK(g.call("sceNpMatchingGetRoomInfoNoLimit", ctx, 0, kRoom, kAttrs, kReq) == kMatchInval);
    CHECK(g.call("sceNpMatchingGetRoomInfoNoLimit", ctx, kLobby, kRoom, kAttrs + 0x40, kReq) == kMatchDuplicate);
    CHECK(g.call("sceNpMatchingGetRoomInfoNoLimit", ctx + 1, kLobby, kRoom, kAttrs, kReq) == kMatchCtxNotFound);
    CHECK(g.call("sceNpMatchingGetRoomInfoNoLimit", ctx, kLobby, kRoom, kAttrs, kReq) == kMatchRoomNotFound);
    CHECK(g.call("sceNpMatchingSetRoomInfo", ctx, kLobby, kRoom, 0, kReq) == kMatchInval); // let through: checked
    CHECK(g.call("sceNpMatchingSetRoomInfo", ctx, kLobby, kRoom, kAttrs, kReq) == kMatchBusy);
    CHECK(g.call("sceNpMatchingSetRoomInfoNoLimit", ctx, kLobby, kRoom, kAttrs, kReq) == kMatchRoomNotFound);
    CHECK(g.call("sceNpMatchingGetRoomSearchFlag", ctx, kLobby, 0, kReq) == kMatchInval);
    CHECK(g.call("sceNpMatchingGetRoomSearchFlag", ctx, kLobby, kRoom, kReq) == kMatchRoomNotFound);
    CHECK(g.call("sceNpMatchingSetRoomSearchFlag", ctx, kLobby, kRoom, 2, kReq) == kMatchInval);
    CHECK(g.call("sceNpMatchingSetRoomSearchFlag", ctx, kLobby, kRoom, 1, kReq) == kMatchRoomNotFound);
    CHECK(g.call("sceNpMatchingLeaveRoom", ctx + 1, kRoom, kReq) == kMatchCtxNotFound);
    CHECK(g.call("sceNpMatchingLeaveRoom", ctx, 0, kReq) == kMatchInval); // RPCS3 aborts
    CHECK(g.call("sceNpMatchingLeaveRoom", ctx, kRoom, kReq) == kMatchRoomNotFound);
    CHECK(g.call("sceNpMatchingGetRoomMemberListLocal", ctx, kRoom, 0, 0) == kMatchInval);
    CHECK(g.call("sceNpMatchingGetRoomMemberListLocal", ctx, kRoom, kSize, 0) == kMatchRoomNotFound);

    // RPCS3's TODOs: their checks, then CELL_OK and no dialog.
    CHECK(g.call("sceNpMatchingGrantOwnership", ctx, kRoom, kNpId, 0) == kMatchInval);
    CHECK(g.call("sceNpMatchingGrantOwnership", ctx, kRoom, kNpId, kReq) == 0);
    CHECK(g.call("sceNpMatchingKickRoomMember", ctx, kRoom, kNpId, kReq) == 0);
    CHECK(g.call("sceNpMatchingKickRoomMemberWithOpt", ctx, kRoom, kNpId, kText, 17, kReq) == kMatchInval);
    CHECK(g.call("sceNpMatchingKickRoomMemberWithOpt", ctx, kRoom, kNpId, kText, 16, kReq) == 0);
    CHECK(g.call("sceNpMatchingAcceptInvitationGUI", ctx, 0, 0, kGuiOpd, kArg) == kMatchInval);
    CHECK(g.call("sceNpMatchingAcceptInvitationGUI", ctx, kComm, 0, kGuiOpd, kArg) == 0);
    const auto invite = [&](u32 dsts, s32 num, s32 slot) {
        return g.call("sceNpMatchingSendInvitationGUI", ctx, kRoom, kComm, dsts, num, slot, kText, kText2, 0, kGuiOpd,
            kArg);
    };
    put(b, kText, std::string(17, 's'));
    put(b, kText2, "Come and play");
    CHECK(invite(0, 0, 0) == kMatchInval); // a subject of at most 16 characters
    put(b, kText, std::string(16, 's'));
    CHECK(invite(kNpId, 13, 0) == kMatchInval);
    CHECK(invite(kNpId, 0, 0) == kMatchInval);
    CHECK(invite(0, 0, 2) == kMatchInval);
    put(b, kText2, "\xFF");
    CHECK(invite(0, 0, 0) == kUtilInvalChar);
    put(b, kText2, std::string(129, 'b'));
    CHECK(invite(0, 0, 0) == kMatchInval);
    put(b, kText2, std::string(128, 'b'));
    CHECK(invite(kNpId, 12, 1) == 0);
    CHECK(take_gui_calls(g, b).empty()); // no dialog for the TODOs

    // While matching 2 is in use, the older library is unavailable.
    REQUIRE(g.call("sceNpMatching2Init2", 0, 0, 0) == 0);
    CHECK(g.call("sceNpMatchingJoinRoomGUI", ctx, kRoom, kGuiOpd, kArg) == kMatchUnavailable);
    CHECK(g.call("sceNpMatchingGetResultGUI", 0, kSize, 0) == kMatchUnavailable);
    CHECK(g.call("sceNpMatchingDestroyCtx", ctx) == kMatchUnavailable);
    REQUIRE(g.call("sceNpMatching2Term2") == 0);

    // Destroyed: no context for the GUI's results either.
    REQUIRE(g.call("sceNpMatchingDestroyCtx", ctx) == 0);
    CHECK(g.call("sceNpMatchingDestroyCtx", ctx) == kMatchCtxNotFound);
    CHECK(g.call("sceNpMatchingGetResult", ctx, 1, 0, kSize, 0) == kMatchCtxNotFound);
    CHECK(g.call("sceNpMatchingJoinRoomGUI", ctx, kRoom, kGuiOpd, kArg) == kMatchCtxNotFound);
}

TEST_CASE("sceNpCommerce (version 1): RPCS3's checks and TODO answers; product categories as RPCS3 fills them",
    "[hle][np]")
{
    constexpr u64 kComUnsupported = 0xFFFFFFFF80029404ull, kComInsufficientBuffer = 0xFFFFFFFF8002940Eull,
                  kComInvalFlagNum = 0xFFFFFFFF8002941Aull, kComParse = 0xFFFFFFFF80029421ull,
                  kComCurrencyNotFound = 0xFFFFFFFF80029422ull, kComCategoryNotFound = 0xFFFFFFFF80029423ull,
                  kComChildCountNotFound = 0xFFFFFFFF80029424ull, kComChildInfoNotFound = 0xFFFFFFFF80029425ull,
                  kComSkuCountNotFound = 0xFFFFFFFF80029426ull, kComSkuInfoNotFound = 0xFFFFFFFF80029427ull,
                  kComInvalSkuNum = 0xFFFFFFFF80029429ull;
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kPc = kData + 0x100, kInfo = kData + 0x200, kPrice = kData + 0x300, kOut = kData + 0x400,
                  kData0 = kData + 0x1000;
    std::memset(b + kOut, 0xAA, 16);

    // A context (no ID written); its requests answer CELL_OK and fetch nothing.
    CHECK(g.call("sceNpCommerceCreateCtx", 2, 0, 0, 0, kOut) == kComUnsupported);
    CHECK(g.call("sceNpCommerceCreateCtx", 1, 0, 0, 0, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAAu);
    CHECK(g.call("sceNpCommerceGetProductCategoryStart", 1, kOut, 1, kOut) == 0);
    CHECK(g.call("sceNpCommerceGetProductCategoryResult", 1, kData0, 0, kOut) == kComInsufficientBuffer);
    CHECK(g.call("sceNpCommerceGetProductCategoryResult", 1, kData0, 16, 0) == kComInsufficientBuffer);
    REQUIRE(g.call("sceNpCommerceGetProductCategoryResult", 1, kData0, 16, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0);
    CHECK(g.call("sceNpCommerceGetProductCategoryFinish", 1) == 0);
    CHECK(g.call("sceNpCommerceGetProductCategoryAbort", 1) == 0);
    CHECK(g.call("sceNpCommerceDestroyCtx", 1) == 0);

    // A product category: the data and its size are recorded, then the version, then the currency (16 bytes).
    std::memset(b + kPc, 0xEE, 32);
    CHECK(g.call("sceNpCommerceInitProductCategory", 0, kData0, 100) == kComParse);
    CHECK(g.call("sceNpCommerceInitProductCategory", kPc, kData0, 3) == kComParse);
    CHECK(ps_read32(b, kPc + 4) == kData0);
    CHECK(ps_read32(b, kPc + 8) == 3);
    CHECK(ps_read32(b, kPc) == 0xEEEEEEEEu);
    CHECK(g.call("sceNpCommerceInitProductCategory", kPc, kData0, 15) == kComParse);
    CHECK(ps_read32(b, kPc) == 1);
    REQUIRE(g.call("sceNpCommerceInitProductCategory", kPc, kData0, 16) == 0);
    CHECK(ps_read32(b, kPc + 12) == 0); // dval

    // The infos and counts need data of a size; the info points at the category.
    std::memset(b + kInfo, 0, 16);
    CHECK(g.call("sceNpCommerceGetCurrencyInfo", 0, kInfo) == kComCurrencyNotFound);
    REQUIRE(g.call("sceNpCommerceGetCurrencyInfo", kPc, kInfo) == 0);
    CHECK(ps_read32(b, kInfo) == kPc);
    CHECK(g.call("sceNpCommerceGetCurrencyInfo", kPc, 0) == 0);
    CHECK(g.call("sceNpCommerceGetCategoryInfo", kPc, kInfo) == kComCategoryNotFound);
    CHECK(g.call("sceNpCommerceGetNumOfChildCategory", kPc, kOut) == kComChildCountNotFound);
    CHECK(g.call("sceNpCommerceGetNumOfChildProductSku", kPc, kOut) == kComSkuCountNotFound);
    CHECK(g.call("sceNpCommerceGetChildProductSkuInfo", kPc, 0, kInfo) == 0); // RPCS3: at most 780 bytes
    ps_write32(b, kPc + 8, 776);
    CHECK(g.call("sceNpCommerceGetCategoryInfo", kPc, kInfo) == 0);
    ps_write32(b, kPc + 8, 780);
    CHECK(g.call("sceNpCommerceGetNumOfChildCategory", kPc, kOut) == 0);
    CHECK(g.call("sceNpCommerceGetNumOfChildProductSku", kPc, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0); // not written (the result above left 0)
    ps_write32(b, kPc + 8, 781);
    CHECK(g.call("sceNpCommerceGetChildProductSkuInfo", kPc, 0, kInfo) == kComSkuInfoNotFound);
    CHECK(g.call("sceNpCommerceGetChildProductSkuInfo", kPc, 0, 0) == kComSkuInfoNotFound);
    ps_write32(b, kPc + 8, 1539);
    CHECK(g.call("sceNpCommerceGetChildCategoryInfo", kPc, 0, kInfo) == kComChildInfoNotFound);
    ps_write32(b, kPc + 8, 1540);
    CHECK(g.call("sceNpCommerceGetChildCategoryInfo", kPc, 0, kInfo) == 0);
    CHECK(g.call("sceNpCommerceGetChildCategoryInfo", kPc, 1, kInfo) == kComChildInfoNotFound);
    CHECK(g.call("sceNpCommerceGetChildCategoryInfo", kPc, 0, 0) == kComChildInfoNotFound);
    CHECK(g.call("sceNpCommerceGetCurrencyDecimals", kInfo) == 0);

    // A price: the fraction is 0 without a dval; with one, nothing is written.
    ps_write32(b, kPrice + 4, 0x55);
    ps_write32(b, kInfo, kPc);
    g.call("sceNpCommerceGetSkuPrice", kInfo, kPrice);
    CHECK(ps_read32(b, kPrice + 4) == 0);
    ps_write32(b, kPrice + 4, 0x55);
    ps_write32(b, kPc + 12, 100);
    g.call("sceNpCommerceGetSkuPrice", kInfo, kPrice);
    CHECK(ps_read32(b, kPrice + 4) == 0x55);

    // Destroying the category forgets its data.
    g.call("sceNpCommerceDestroyProductCategory", kPc);
    CHECK(ps_read32(b, kPc + 4) == 0);
    CHECK(ps_read32(b, kPc + 8) == 0);
    g.call("sceNpCommerceDestroyProductCategory", 0);

    // Checkout: 1 to 16 SKUs; data flags: 1 to 16.
    CHECK(g.call("sceNpCommerceDoCheckoutStartAsync", 1, kData0, 0, 0, kOut) == kComInvalSkuNum);
    CHECK(g.call("sceNpCommerceDoCheckoutStartAsync", 1, kData0, 17, 0, kOut) == kComInvalSkuNum);
    CHECK(g.call("sceNpCommerceDoCheckoutStartAsync", 1, kData0, 16, 0, kOut) == 0);
    CHECK(g.call("sceNpCommerceDoCheckoutFinishAsync", 1) == 0);
    for (const char* name : {"sceNpCommerceGetDataFlagStart", "sceNpCommerceSetDataFlagStart"}) {
        INFO(name);
        CHECK(g.call(name, 1, 2, 3, 0, 5) == kComInvalFlagNum);
        CHECK(g.call(name, 1, 2, 3, 17, 5) == kComInvalFlagNum);
        CHECK(g.call(name, 1, 2, 3, 16, 5) == 0);
    }
    CHECK(g.call("sceNpCommerceGetDataFlagState", 1, 2, 0) == kComInvalFlagNum);
    CHECK(g.call("sceNpCommerceGetDataFlagState", 1, 2, 16) == 0);
    CHECK(g.call("sceNpCommerceGetDataFlagAbort") == 0);
    CHECK(g.call("sceNpCommerceGetDataFlagFinish") == 0);
    CHECK(g.call("sceNpCommerceSetDataFlagFinish") == 0);
}

TEST_CASE("sceNpSignaling's context options and peer information: RPCS3's checks, then its TODO answers", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kCtx = kData + 0x200, kOut = kData + 0x300;
    CHECK(g.call("sceNpSignalingDeactivateConnection", 0x6001, 1) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetCtxOpt", 0x6001, 1, kOut) == kSigNotInit);
    CHECK(g.call("sceNpSignalingGetPeerNetInfo", 0x6001, kNpId, kOut) == kSigNotInit);
    CHECK(g.call("sceNpSignalingCancelPeerNetInfo", 0x6001, 1) == kSigNotInit);
    np_up(g);
    put_np_id(b, kNpId, "");
    REQUIRE(g.call("sceNpSignalingCreateCtx", kNpId, 0, 0, kCtx) == 0);
    const u32 ctx = ps_read32(b, kCtx);
    std::memset(b + kOut, 0xAA, 0x40);

    CHECK(g.call("sceNpSignalingGetCtxOpt", ctx, 0, kOut) == kSigInval);
    CHECK(g.call("sceNpSignalingGetCtxOpt", ctx, 1, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetCtxOpt", ctx + 1, 1, kOut) == kSigCtxNotFound);
    CHECK(g.call("sceNpSignalingGetCtxOpt", ctx, 1, kOut) == 0);
    CHECK(g.call("sceNpSignalingSetCtxOpt", ctx, 1, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingSetCtxOpt", ctx + 1, 1, 1) == kSigCtxNotFound);
    CHECK(g.call("sceNpSignalingSetCtxOpt", ctx, 1, 1) == 0);
    CHECK(g.call("sceNpSignalingGetPeerNetInfo", ctx, 0, kOut) == kSigInval);
    CHECK(g.call("sceNpSignalingGetPeerNetInfo", ctx, kNpId, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetPeerNetInfo", ctx, kNpId, kOut) == 0);
    CHECK(g.call("sceNpSignalingGetPeerNetInfoResult", ctx, 1, 0) == kSigInval);
    CHECK(g.call("sceNpSignalingGetPeerNetInfoResult", ctx, 1, kOut + 4) == 0);
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAAu); // nothing written by RPCS3's TODOs
    CHECK(ps_read32(b, kOut + 4) == 0xAAAAAAAAu);
    CHECK(g.call("sceNpSignalingCancelPeerNetInfo", ctx, 1) == 0);
    CHECK(g.call("sceNpSignalingDeactivateConnection", ctx, 1) == 0);
}

TEST_CASE("sceNpMatchingInt: the matching library's internal entry points, as sceNp's", "[hle][np]")
{
    // Three share sceNp's names and NIDs (RPCS3 registers its OLD_ functions under them) and do the same.
    for (const auto& [name, nid] : {std::pair<std::string_view, u32>{"sceNpMatchingJoinRoomGUI", 0x474B7B13},
             {"sceNpMatchingSetRoomInfoNoLimit", 0x4A18A89E}, {"sceNpMatchingGetRoomInfoNoLimit", 0xB020684E}}) {
        INFO(name);
        const hle::Function* internal = hle::registry().find("sceNpMatchingInt", nid);
        const hle::Function* np = hle::registry().find("sceNp", nid);
        REQUIRE(internal != nullptr);
        REQUIRE(np != nullptr);
        CHECK(internal->name == name);
        CHECK(internal->fn == np->fn);
    }
    for (const auto& [name, nid] : {std::pair<std::string_view, u32>{"sceNpMatchingGetRoomMemberList", 0x170829D3},
             {"sceNpMatchingJoinRoomWithoutGUI", 0x31B2B978}, {"sceNpMatchingGetRoomListWithoutGUI", 0x92D5C77B},
             {"sceNpMatchingGetRoomListGUI", 0xB00F408A}, {"sceNpMatchingCreateRoomWithoutGUI", 0xEADCB5CA}}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("sceNpMatchingInt", nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }

    InterpretMisses interpret;
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u32 kNpId = kData + 0x100, kCtxOut = kData + 0x200, kComm = kData + 0x300, kRoom = kData + 0x400,
                  kSize = kData + 0x610, kRange = kData + 0x620, kAttrs = kData + 0x1000, kConds = kData + 0x2000;
    constexpr u32 kArg = 0x00C0FFEE;
    write_gui_handler(b);
    write_nop(b);
    put_np_id(b, kNpId, "");
    put_comm_id(b, kComm, "NPWR01234", 0);
    std::memset(b + kRoom, 1, 36);

    CHECK(g.call("sceNpMatchingGetRoomMemberList", 0x9001, kRoom, kSize, 0) == kMatchNotInit);
    np_up(g);
    REQUIRE(g.call("sceNpMatchingCreateCtx", kNpId, kNopOpd, 0, kCtxOut) == 0);
    const u32 ctx = ps_read32(b, kCtxOut);

    // Creating a room without the dialog: sceNpMatchingCreateRoomGUI's checks, and the same answer.
    put_attr(b, kAttrs, kAttrs + 0x20, 2, 1, 4); // total slots 4
    put_attr(b, kAttrs + 0x20, 0, 2, 2, 1);      // private slots 1
    CHECK(g.call("sceNpMatchingCreateRoomWithoutGUI", ctx, 0, kAttrs, kGuiOpd, kArg) == kMatchInval);
    CHECK(g.call("sceNpMatchingCreateRoomWithoutGUI", ctx, kComm, 0, kGuiOpd, kArg) == kMatchAttrNotSpecified);
    CHECK(g.call("sceNpMatchingCreateRoomWithoutGUI", ctx + 1, kComm, kAttrs, kGuiOpd, kArg) == kMatchCtxNotFound);
    REQUIRE(g.call("sceNpMatchingCreateRoomWithoutGUI", ctx, kComm, kAttrs, kGuiOpd, kArg) == 0);
    CHECK(take_gui_calls(g, b) == std::vector<GuiCall>{{ctx, 8, 0, kArg}, {ctx, 1, kMatchCanceled, kArg},
                                      {ctx, 9, 0, kArg}});

    // Joining one.
    CHECK(g.call("sceNpMatchingJoinRoomWithoutGUI", ctx, 0, kGuiOpd, kArg) == kMatchInval);
    CHECK(g.call("sceNpMatchingJoinRoomWithoutGUI", ctx, kRoom, 0, kArg) == kMatchInval);
    REQUIRE(g.call("sceNpMatchingJoinRoomWithoutGUI", ctx, kRoom, kGuiOpd, kArg) == 0);
    CHECK(take_gui_calls(g, b) == std::vector<GuiCall>{{ctx, 8, 0, kArg}, {ctx, 2, kMatchCanceled, kArg},
                                      {ctx, 9, 0, kArg}});

    // The room lists without the limit: the limited list's checks, and the GET_ROOM_LIST event (10), not 11.
    for (const std::string_view name : {"sceNpMatchingGetRoomListGUI", "sceNpMatchingGetRoomListWithoutGUI"}) {
        INFO(name);
        ps_write32(b, kRange, 0);
        ps_write32(b, kRange + 4, 10);
        CHECK(g.call(name, ctx, kComm, kRange, 0, 0, kGuiOpd, kArg) == kMatchInval); // no start
        ps_write32(b, kRange, 1);
        ps_write32(b, kRange + 4, 21);
        CHECK(g.call(name, ctx, kComm, kRange, 0, 0, kGuiOpd, kArg) == kMatchInval); // more than 20
        ps_write32(b, kRange + 4, 20);
        CHECK(g.call(name, ctx, kComm, kUnmapped, 0, 0, kGuiOpd, kArg) == kMatchInval); // RPCS3 faults
        put_cond(b, kConds, 0, 2, 1, 0);
        CHECK(g.call(name, ctx, kComm, kRange, kConds, 0, kGuiOpd, kArg) == kMatchInvalCond); // the total slots
        put_cond(b, kConds, 0, 2, 2, 0);
        CHECK(g.call(name, ctx + 1, kComm, kRange, kConds, 0, kGuiOpd, kArg) == kMatchCtxNotFound);
        REQUIRE(g.call(name, ctx, kComm, kRange, kConds, 0, kGuiOpd, kArg) == 0);
        CHECK(take_gui_calls(g, b) == std::vector<GuiCall>{{ctx, 8, 0, kArg}, {ctx, 10, kMatchCanceled, kArg},
                                          {ctx, 9, 0, kArg}});
    }

    // A room's members: none offline, as sceNpMatchingGetRoomMemberListLocal.
    CHECK(g.call("sceNpMatchingGetRoomMemberList", ctx, kRoom, 0, 0) == kMatchInval);
    CHECK(g.call("sceNpMatchingGetRoomMemberList", ctx + 1, kRoom, kSize, 0) == kMatchCtxNotFound);
    CHECK(g.call("sceNpMatchingGetRoomMemberList", ctx, kRoom, kSize, 0) == kMatchRoomNotFound);

    // While matching 2 is in use, these are unavailable too.
    REQUIRE(g.call("sceNpMatching2Init2", 0, 0, 0) == 0);
    CHECK(g.call("sceNpMatchingGetRoomListGUI", ctx, kComm, kRange, 0, 0, kGuiOpd, kArg) == kMatchUnavailable);
    CHECK(g.call("sceNpMatchingCreateRoomWithoutGUI", ctx, kComm, kAttrs, kGuiOpd, kArg) == kMatchUnavailable);
    CHECK(g.call("sceNpMatchingGetRoomMemberList", ctx, kRoom, kSize, 0) == kMatchUnavailable);
    REQUIRE(g.call("sceNpMatching2Term2") == 0);
    CHECK(take_gui_calls(g, b).empty());
}

TEST_CASE("sceNpUtil's bandwidth test: a fake of about 100 ms, as RPCS3; one at a time", "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "np");
    u8* b = f.base();
    constexpr u64 kAlready = 0xFFFFFFFF8002AA02ull;
    constexpr s64 kRunning = 1, kFinished = 2;
    constexpr u32 kResult = kData + 0x100;
    // SceNpUtilBandwidthTestResult: be f64 upload_bps, be f64 download_bps, be s32 result, 4 bytes of padding.
    const auto check_result = [&] {
        CHECK(std::bit_cast<double>(ps_read64(b, kResult)) == 100'000'000.0);
        CHECK(std::bit_cast<double>(ps_read64(b, kResult + 8)) == 100'000'000.0);
        CHECK(ps_read32(b, kResult + 16) == 0);
        CHECK(ps_read32(b, kResult + 20) == 0);
    };
    const auto status = [&] { return static_cast<s64>(g.call("sceNpUtilBandwidthTestGetStatus")); };

    // Nothing before a test starts; NP isn't needed.
    CHECK(g.call("sceNpUtilBandwidthTestGetStatus") == kNotInit);
    CHECK(g.call("sceNpUtilBandwidthTestAbort") == kNotInit);
    CHECK(g.call("sceNpUtilBandwidthTestShutdown", kResult) == kNotInit);

    // A test runs for about 100 ms, then finishes by itself; one at a time, until it is shut down.
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(g.call("sceNpUtilBandwidthTestInitStart", 1000, 0x4000) == 0);
    const s64 first = status();
    if (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100)) CHECK(first == kRunning);
    CHECK(g.call("sceNpUtilBandwidthTestInitStart", 1000, 0x4000) == kAlready);
    while (status() == kRunning && std::chrono::steady_clock::now() - started < std::chrono::seconds(10)) {
        usleep(1000);
    }
    CHECK(status() == kFinished);
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(100));
    CHECK(g.call("sceNpUtilBandwidthTestInitStart", 1000, 0x4000) == kAlready);

    // Shutdown writes the result when given a buffer (one that is not guest memory is refused and the test kept) and
    // drops the test.
    std::memset(b + kResult, 0xAA, 24);
    CHECK(g.call("sceNpUtilBandwidthTestShutdown", kUnmapped) == kInval); // RPCS3 faults
    CHECK(status() == kFinished);
    REQUIRE(g.call("sceNpUtilBandwidthTestShutdown", kResult) == 0);
    check_result();
    CHECK(g.call("sceNpUtilBandwidthTestGetStatus") == kNotInit);
    CHECK(g.call("sceNpUtilBandwidthTestShutdown", kResult) == kNotInit);

    // Aborted, a test finishes at once, with the same result.
    REQUIRE(g.call("sceNpUtilBandwidthTestInitStart", 0, 0) == 0);
    REQUIRE(g.call("sceNpUtilBandwidthTestAbort") == 0);
    CHECK(status() == kFinished);
    std::memset(b + kResult, 0xAA, 24);
    REQUIRE(g.call("sceNpUtilBandwidthTestShutdown", kResult) == 0);
    check_result();

    // A shutdown while it runs ends it too; a null result is allowed and nothing is written.
    REQUIRE(g.call("sceNpUtilBandwidthTestInitStart", 0, 0) == 0);
    std::memset(b + kResult, 0xAA, 24);
    REQUIRE(g.call("sceNpUtilBandwidthTestShutdown", 0) == 0);
    CHECK(ps_read32(b, kResult) == 0xAAAAAAAAu);
    CHECK(g.call("sceNpUtilBandwidthTestGetStatus") == kNotInit);
}
