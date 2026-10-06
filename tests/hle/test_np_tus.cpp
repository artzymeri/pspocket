// sceNpTus offline (synthetic: no game data): the library on top of NP, title contexts from 0x7001, transactions
// refused; every variable and data request with RPCS3's checks in RPCS3's order, then offline
// (SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID); the calls that take only a transaction find none.

#include "hle_test_util.hpp"

#include <cstring>
#include <string>
#include <string_view>

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kNpNotInit = 0xFFFFFFFF8002AA01ull, kAlready = 0xFFFFFFFF8002A101ull, kNotInit = 0xFFFFFFFF8002A102ull,
              kInval = 0xFFFFFFFF8002A104ull, kInsufficient = 0xFFFFFFFF8002A1A1ull, kInvalId = 0xFFFFFFFF8002A1A3ull,
              kOffline = 0xFFFFFFFF8002A1A4ull, kAlignment = 0xFFFFFFFF8002A1ACull, kTooManyNpId = 0xFFFFFFFF8002A1ADull,
              kTooManySlots = 0xFFFFFFFF8002A1B1ull;
constexpr u32 kPool = kData + 0x40000;
constexpr u32 kUnmapped = 0x20000000;

// The four forms of a call: an NP ID or a virtual user ID as the target, synchronous or not.
std::string form(std::string_view stem, u32 i)
{
    static constexpr std::string_view kSuffix[] = {"", "VUser", "Async", "VUserAsync"};
    return std::string("sceNpTus") + std::string(stem) + std::string(kSuffix[i]);
}

} // namespace

TEST_CASE("sceNpTus offline: title contexts kept, transactions refused, RPCS3's checks before going online",
    "[hle][np]")
{
    Fixture f;
    GuestThread g(f, 0, "tus");
    u8* b = f.base();
    constexpr u32 kComm = kData + 0x100, kPass = kData + 0x200, kNpId = kData + 0x300, kSlots = kData + 0x400,
                  kValues = kData + 0x800, kOut = kData + 0x1000, kOpt = kData + 0x2000, kData0 = kData + 0x3000;
    std::memset(b + kComm, 0, 12);
    std::memcpy(b + kComm, "NPWR01234", 9);
    std::memset(b + kNpId, 0, 0x24);
    for (u32 i = 0; i < 65; i++) ps_write32(b, kSlots + 4 * i, i);

    // The library needs NP; once.
    CHECK(g.call("sceNpTusInit", 1000) == kNpNotInit);
    CHECK(g.call("sceNpTusCreateTitleCtx", kComm, kPass, kNpId) == kNotInit);
    REQUIRE(g.call("sceNp2Init", 0x20000, kPool) == 0);
    CHECK(g.call("sceNpTusTerm") == kNotInit);
    REQUIRE(g.call("sceNpTusInit", 1000) == 0);
    CHECK(g.call("sceNpTusInit", 1000) == kAlready);

    // Title contexts from 0x7001; transactions refused.
    CHECK(g.call("sceNpTusCreateTitleCtx", 0, kPass, kNpId) == kInsufficient);
    CHECK(g.call("sceNpTusCreateTitleCtx", kUnmapped, kPass, kNpId) == kInval);
    b[kComm] = 'n';
    CHECK(g.call("sceNpTusCreateTitleCtx", kComm, kPass, kNpId) == kInval); // RPCS3 aborts
    b[kComm] = 'N';
    CHECK(g.call("sceNpTusCreateTitleCtx", kComm, kPass, kNpId) == 0x7001);
    CHECK(g.call("sceNpTusCreateTitleCtx", kComm, kPass, kNpId) == 0x7002);
    CHECK(g.call("sceNpTusCreateTransactionCtx", 0x7001) == kOffline);
    for (const char* name : {"sceNpTusDestroyTransactionCtx", "sceNpTusAbortTransaction"}) {
        CHECK(g.call(name, 0x8001) == kInvalId);
    }
    CHECK(g.call("sceNpTusWaitAsync", 0x8001, kOut) == kInvalId);
    CHECK(g.call("sceNpTusPollAsync", 0x8001, kOut) == kInvalId);
    CHECK(g.call("sceNpTusSetTimeout", 0x7001, 9'999'999) == kInval);
    CHECK(g.call("sceNpTusSetTimeout", 0x7001, 10'000'000) == 0);
    CHECK(g.call("sceNpTusSetTimeout", 0x8001, 10'000'000) == kInvalId);

    for (u32 i = 0; i < 4; i++) {
        // Setting several slots' variables: 1 to 64 slots, none negative, no option; then the target.
        const std::string set = form("SetMultiSlotVariable", i);
        INFO(set);
        CHECK(g.call(set, 0x8001, kNpId, 0, kValues, 1, 0) == kInsufficient);
        CHECK(g.call(set, 0x8001, kNpId, kSlots, kValues, 0, 0) == kInval);
        CHECK(g.call(set, 0x8001, kNpId, kSlots, kValues, 1, kOpt) == kInval);
        CHECK(g.call(set, 0x8001, kNpId, kSlots, kValues, 65, 0) == kTooManySlots);
        ps_write32(b, kSlots + 4, 0xFFFFFFFF);
        CHECK(g.call(set, 0x8001, kNpId, kSlots, kValues, 2, 0) == kInval);
        ps_write32(b, kSlots + 4, 1);
        CHECK(g.call(set, 0x8001, kNpId, kUnmapped, kValues, 2, 0) == kInval); // RPCS3 faults
        CHECK(g.call(set, 0x8001, 0, kSlots, kValues, 64, 0) == kInsufficient);
        CHECK(g.call(set, 0x8001, kNpId, kSlots, kValues, 64, 0) == kOffline);

        // Getting them: the output array's size (120 bytes each) after the option.
        const std::string get = form("GetMultiSlotVariable", i);
        CHECK(g.call(get, 0x8001, kNpId, kSlots, kOut, 119, 1, 0) == kAlignment);
        CHECK(g.call(get, 0x8001, kNpId, kSlots, kOut, 120 * 65, 65, 0) == kTooManySlots);
        CHECK(g.call(get, 0x8001, kNpId, kSlots, kOut, 240, 2, 0) == kOffline);

        // Several users' variables: 1 to 101 users, a slot.
        const std::string users = form("GetMultiUserVariable", i);
        CHECK(g.call(users, 0x8001, kNpId, 1, kOut, 120, 0, 0) == kInval);
        CHECK(g.call(users, 0x8001, kNpId, 1, kOut, 120 * 102, 102, 0) == kTooManyNpId);
        CHECK(g.call(users, 0x8001, kNpId, -1, kOut, 120, 1, 0) == kInval);
        CHECK(g.call(users, 0x8001, 0, 1, kOut, 120, 1, 0) == kInsufficient);
        CHECK(g.call(users, 0x8001, kNpId, 1, kOut, 120, 1, 0) == kOffline);

        // Add and get, try and set: the library first, the option's and the output's sizes.
        const std::string add = form("AddAndGetVariable", i);
        CHECK(g.call(add, 0x8001, kNpId, -1, 5, kOut, 120, 0) == kInval);
        ps_write32(b, kOpt, 16);
        CHECK(g.call(add, 0x8001, kNpId, 1, 5, kOut, 120, kOpt) == kAlignment);
        CHECK(g.call(add, 0x8001, kNpId, 1, 5, kOut, 112, 0) == kAlignment);
        ps_write32(b, kOpt, 12);
        CHECK(g.call(add, 0x8001, kNpId, 1, 5, kOut, 120, kOpt) == kOffline);
        const std::string tryset = form("TryAndSetVariable", i);
        CHECK(g.call(tryset, 0x8001, kNpId, 1, 1, 5, 0, 120, 0) == kInsufficient);
        CHECK(g.call(tryset, 0x8001, kNpId, 1, 1, 5, kOut, 120, kOpt) == kAlignment); // its option is 16 bytes
        ps_write32(b, kOpt, 16);
        CHECK(g.call(tryset, 0x8001, kNpId, 1, 1, 5, kOut, 120, kOpt) == kOffline);

        // Deleting slots: no target check.
        for (const char* stem : {"DeleteMultiSlotVariable", "DeleteMultiSlotData"}) {
            const std::string del = form(stem, i);
            CHECK(g.call(del, 0x8001, 0, 0, 1, 0) == kInsufficient);
            CHECK(g.call(del, 0x8001, 0, kSlots, 1, kOpt) == kInval);
            CHECK(g.call(del, 0x8001, 0, kSlots, 1, 0) == kOffline);
        }

        // Data: setting needs a slot, data and its size; the info is 392 bytes; getting checks the target first.
        const std::string setd = form("SetData", i);
        CHECK(g.call(setd, 0x8001, kNpId, -1, 16, 16, kData0, 0, 0, 0) == kInsufficient);
        CHECK(g.call(setd, 0x8001, kNpId, 1, 0, 0, kData0, 0, 0, 0) == kInsufficient);
        CHECK(g.call(setd, 0x8001, kNpId, 1, 16, 16, kData0, kOut, 391, 0) == kAlignment);
        CHECK(g.call(setd, 0x8001, 0, 1, 16, 16, kData0, kOut, 392, 0) == kInsufficient);
        ps_write32(b, kOpt, 12);
        CHECK(g.call(setd, 0x8001, kNpId, 1, 16, 16, kData0, kOut, 392, kOpt) == kOffline);
        const std::string getd = form("GetData", i);
        CHECK(g.call(getd, 0x8001, 0, 1, kOut, 488, kData0, 16, 0) == kInsufficient);
        CHECK(g.call(getd, 0x8001, kNpId, -1, kOut, 488, kData0, 16, 0) == kInval);
        CHECK(g.call(getd, 0x8001, kNpId, 1, kOut, 480, kData0, 16, 0) == kAlignment);
        CHECK(g.call(getd, 0x8001, kNpId, 1, kOut, 488, kData0, 16, 0) == kOffline);

        // Data statuses: 488 bytes each.
        const std::string slots_status = form("GetMultiSlotDataStatus", i);
        CHECK(g.call(slots_status, 0x8001, kNpId, kSlots, kOut, 488, 2, 0) == kAlignment);
        CHECK(g.call(slots_status, 0x8001, kNpId, kSlots, kOut, 976, 2, 0) == kOffline);
        const std::string users_status = form("GetMultiUserDataStatus", i);
        CHECK(g.call(users_status, 0x8001, kNpId, 1, kOut, 488 * 102, 102, 0) == kTooManyNpId);
        CHECK(g.call(users_status, 0x8001, kNpId, 1, kOut, 488, 1, 0) == kOffline);
    }

    // Friends: an option starting with 0xC or none, sort types, at most 100.
    ps_write32(b, kOpt, 0xD);
    CHECK(g.call("sceNpTusGetFriendsVariable", 0x8001, 1, 0, 1, kOut, 120, 1, kOpt) == kInval);
    ps_write32(b, kOpt, 0xC);
    CHECK(g.call("sceNpTusGetFriendsVariable", 0x8001, 1, 0, 5, kOut, 120, 1, kOpt) == kInval);
    CHECK(g.call("sceNpTusGetFriendsVariable", 0x8001, 1, 0, 4, kOut, 120 * 101, 101, kOpt) == kTooManyNpId);
    CHECK(g.call("sceNpTusGetFriendsVariable", 0x8001, 1, 0, 4, kOut, 120, 1, kOpt) == kOffline);
    CHECK(g.call("sceNpTusGetFriendsVariableAsync", 0x8001, 1, 0, 1, kOut, 120, 1, 0) == kOffline);
    CHECK(g.call("sceNpTusGetFriendsDataStatus", 0x8001, 1, 0, 3, kOut, 488, 1, 0) == kInval);
    CHECK(g.call("sceNpTusGetFriendsDataStatus", 0x8001, 1, 0, 2, kOut, 488, 1, 0) == kOffline);
    CHECK(g.call("sceNpTusGetFriendsDataStatusAsync", 0x8001, 1, 1, 1, kOut, 488, 1, 0) == kOffline);

    // TSS: online before anything else.
    CHECK(g.call("sceNpTssGetData", 0x8001, 1, 0, 16, kData0, 16, 0) == kOffline);
    CHECK(g.call("sceNpTssGetDataAsync", 0x8001, 1, kOut, 16, kData0, 16, 0) == kOffline);

    // Destroying a title context; term leaves the others.
    REQUIRE(g.call("sceNpTusDestroyTitleCtx", 0x7001) == 0);
    CHECK(g.call("sceNpTusDestroyTitleCtx", 0x7001) == kInvalId);
    REQUIRE(g.call("sceNpTusTerm") == 0);
    CHECK(g.call("sceNpTusDestroyTitleCtx", 0x7002) == kNotInit);
    CHECK(g.call("sceNpTusGetData", 0x8001, kNpId, 1, kOut, 488, kData0, 16, 0) == kNotInit);
    REQUIRE(g.call("sceNpTusInit", 1000) == 0);
    CHECK(g.call("sceNpTusDestroyTitleCtx", 0x7002) == 0);
}
