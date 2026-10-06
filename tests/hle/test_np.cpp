// sceNp offline (synthetic: no game data): RPCS3's checks and results with no user signed in to PSN, for lookups,
// the manager's user queries and tickets, basic events and presence, messages, the players' history and its event for
// the basic handler, the profile dialog, signaling contexts and connections, NP ID comparison, and the DRM check of
// files on mounted directories.

#include "hle_test_util.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"

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

TEST_CASE("sceNpDrmIsAvailable: the file at the path as RPCS3 checks it; content that needs a key isn't available",
    "[hle][np]")
{
    DrmDirs dirs;
    const std::string id = "UP0001-BLES01807_00-PSPOCKETTEST0001";
    const fsys::path usrdir = dirs.hdd0 / "game" / "BLES01807" / "USRDIR";
    DrmDirs::write(dirs.bdvd / "PS3_GAME" / "USRDIR" / "plain.dat", {1, 2, 3, 4, 5});
    DrmDirs::write(dirs.bdvd / "PS3_GAME" / "USRDIR" / "empty.dat", {});
    DrmDirs::write(dirs.bdvd / "PS3_GAME" / "USRDIR" / "sce3", {'S', 'C', 'E'});
    DrmDirs::write(usrdir / "dlc.edat", npd_file(2, id, 0));
    DrmDirs::write(usrdir / "network.edat", npd_file(1, "UP0001-BLES01807_00-PSPOCKETTEST0002", 0));
    DrmDirs::write(usrdir / "free.edat", npd_file(3, id, 0));
    DrmDirs::write(usrdir / "data.sdat", npd_file(0, id, 0x01000000));
    DrmDirs::write(usrdir / "badid.edat", npd_file(2, "../x", 0));
    DrmDirs::write(usrdir / "short.edat", {'N', 'P', 'D', 0, 0, 0});
    std::vector<u8> sce(0x200, 0);
    std::memcpy(sce.data(), "SCE", 4);
    DrmDirs::write(usrdir / "module.sprx", sce);

    Fixture f;
    dirs.mount(f);
    GuestThread g(f, 0, "drm");
    u8* b = f.base();
    constexpr u32 kKlicensee = kData + 0x100, kPath = kData + 0x1000;
    std::memset(b + kKlicensee, 0x11, 16);
    const auto check = [&](std::string_view path, u32 klicensee = kKlicensee) {
        put(b, kPath, path);
        return g.call("sceNpDrmIsAvailable", klicensee, kPath);
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

    // EDATs. An SDAT: RPCS3's EDAT check refuses it. One that needs a licence (types 1 and 2) the disc game doesn't
    // have: no RAP in the user's exdata (none looked for when the content ID isn't valid).
    CHECK(check(game + "data.sdat") == kDrmNoEntitlement);
    CHECK(check(game + "dlc.edat") == kDrmLicenseNotFound);
    CHECK(check(game + "network.edat") == kDrmLicenseNotFound);
    CHECK(check(game + "badid.edat") == kDrmLicenseNotFound);
    CHECK(check(game + "short.edat") == kDrmLicenseNotFound); // read as zeros past its end
    // A RAP under 16 bytes is a bad format.
    const fsys::path rap = dirs.hdd0 / "home" / "00000001" / "exdata" / (id + ".rap");
    DrmDirs::write(rap, std::vector<u8>(8, 0x22));
    CHECK(check(game + "dlc.edat") == kDrmBadFormat);
    // Content RPCS3 would open with a key (the RAP's, the klicensee's) can't be decrypted here: not available.
    DrmDirs::write(rap, std::vector<u8>(16, 0x22));
    CHECK(check(game + "dlc.edat") == kDrmNoEntitlement);
    CHECK(check(game + "free.edat") == kDrmNoEntitlement);
    // A SELF must open with the build's keys: this one is no SELF.
    CHECK(check(game + "module.sprx") == kDrmNoEntitlement);

    // At most 0x100 characters of the path are read.
    const std::string name(0x100 - std::string_view("/dev_hdd0/").size(), 'a');
    DrmDirs::write(dirs.hdd0 / name, {1});
    CHECK(check("/dev_hdd0/" + name + "xyz") == 0);
    CHECK(check("/dev_hdd0/" + name.substr(1) + "xyz") == r3_of(lv2::CELL_ENOENT));
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
    put(b, kPath, game + "endless.edat");
    CHECK(g.call("sceNpDrmProcessExitSpawn", 0, kPath, 0, 0, 0, 0, 1001, 0) == kDrmLicenseNotFound);

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
