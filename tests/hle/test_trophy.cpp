// sceNpTrophy (synthetic: no game data): init and term, contexts and handles with RPCS3's checks in RPCS3's order, and
// IDs as RPCS3's idm hands them out (1 to 4, in order, then the lowest free one; term doesn't rewind them).

#include "hle_test_util.hpp"

#include <cstring>
#include <string_view>
#include <utility>

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kAlreadyInit = 0xFFFFFFFF80022901ull, kNotInit = 0xFFFFFFFF80022902ull,
              kNotSupported = 0xFFFFFFFF80022903ull, kInvalArg = 0xFFFFFFFF80022906ull,
              kExceedsMax = 0xFFFFFFFF80022907ull, kUnknownContext = 0xFFFFFFFF8002290Aull,
              kUnknownHandle = 0xFFFFFFFF80022911ull, kInvalCommId = 0xFFFFFFFF80022918ull;

constexpr u32 kCommId = kData + 0x100, kSign = kData + 0x200, kCtx = kData + 0x300, kHandle = kData + 0x310;
constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

// A SceNpCommunicationId (12 bytes) at kCommId: up to nine characters, the terminator, num, dummy.
void put_comm_id(u8* b, std::string_view data, u8 num, u8 term = 0)
{
    std::memset(b + kCommId, 0, 12);
    std::memcpy(b + kCommId, data.data(), data.size());
    b[kCommId + 9] = term;
    b[kCommId + 10] = num;
}

// A SceNpCommunicationSignature (160 bytes) at kSign whose header is valid: magic, version 1.00, six zero bytes. The
// rest stands for the signature proper (not checked).
void put_sign(u8* b)
{
    for (u32 i = 0; i < 160; i++) b[kSign + i] = static_cast<u8>(0x40 + i);
    ps_write32(b, kSign, 0xB9DDE13B);
    ps_write16(b, kSign + 4, 0x100);
    std::memset(b + kSign + 6, 0, 6);
}

void trophy_up(GuestThread& g) { REQUIRE(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 0) == 0); }

// Creates a context for the communication ID and signature in place and returns its ID.
u32 create_context(GuestThread& g, u8* b, u64 options = 0)
{
    ps_write32(b, kCtx, 0xEEEEEEEE);
    REQUIRE(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, options) == 0);
    return ps_read32(b, kCtx);
}

u32 create_handle(GuestThread& g, u8* b)
{
    ps_write32(b, kHandle, 0xEEEEEEEE);
    REQUIRE(g.call("sceNpTrophyCreateHandle", kHandle) == 0);
    return ps_read32(b, kHandle);
}

} // namespace

TEST_CASE("sceNpTrophy: init, term and the context and handle functions are registered", "[hle][trophy]")
{
    const std::pair<std::string_view, u32> expected[] = {
        {"sceNpTrophyInit", 0x39567781},
        {"sceNpTrophyTerm", 0xA7FABF4D},
        {"sceNpTrophyCreateContext", 0xE3BF9A28},
        {"sceNpTrophyDestroyContext", 0x3741ECC7},
        {"sceNpTrophyCreateHandle", 0x1C25470D},
        {"sceNpTrophyDestroyHandle", 0x623CD2DC},
        {"sceNpTrophyAbortHandle", 0x48BD97C7},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const auto& [name, nid] : expected) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("sceNpTrophy", nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
}

TEST_CASE("sceNpTrophy: init once, without options, until term", "[hle][trophy]")
{
    Fixture f;
    GuestThread g(f, 0, "trophy");
    CHECK(g.call("sceNpTrophyTerm") == kNotInit);
    CHECK(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 1) == kNotSupported);
    CHECK(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 0x100000000ull) == kNotSupported); // options is a u64
    trophy_up(g); // GTA V's call: no pool, no container
    CHECK(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 0) == kAlreadyInit);
    CHECK(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 1) == kAlreadyInit); // checked before the options
    REQUIRE(g.call("sceNpTrophyTerm") == 0); // GTA V terminates right away
    CHECK(g.call("sceNpTrophyTerm") == kNotInit);
    REQUIRE(g.call("sceNpTrophyInit", kData, 0x10000, 0x1234, 0) == 0); // the pool and container aren't used
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
}

TEST_CASE("sceNpTrophy contexts: RPCS3's checks in its order, IDs 1 to 4, destroyed, cleared by term",
    "[hle][trophy]")
{
    Fixture f;
    GuestThread g(f, 0, "trophy");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    put_comm_id(b, "NPWR01234", 0);
    put_sign(b);
    ps_write32(b, kCtx, 0xEEEEEEEE);

    // The signature pointer is checked before the library's state.
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, 0, 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kNotInit);
    CHECK(g.call("sceNpTrophyDestroyContext", 1) == kNotInit);
    trophy_up(g);
    CHECK(g.call("sceNpTrophyDestroyContext", 1) == kUnknownContext);

    // The pointers, the options (only READ_ONLY), then the ID's number (at most 99) and the signature's header.
    CHECK(g.call("sceNpTrophyCreateContext", 0, kCommId, kSign, 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, 0, kSign, 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", 0, kCommId, kSign, 2) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 2) == kNotSupported);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0x100000000ull) == kNotSupported);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kUnmapped, kSign, 0) == kInvalArg); // RPCS3 faults
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kUnmapped, 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", kUnmapped, kCommId, kSign, 0) == kInvalArg);
    put_comm_id(b, "NPWR01234", 100);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
    put_comm_id(b, "NPWR01234", 0xFF);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
    put_comm_id(b, "NPWR01234", 99);
    ps_write32(b, kSign, 0xB9DDE13C); // magic
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
    put_sign(b);
    for (const u32 i : {6u, 11u}) { // the six zero bytes
        b[kSign + i] = 1;
        CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
        b[kSign + i] = 0;
    }
    for (const u16 version : {u16{0x0001}, u16{0x0101}, u16{0x0200}}) {
        ps_write16(b, kSign + 4, version);
        CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
    }
    put_sign(b);
    CHECK(ps_read32(b, kCtx) == 0xEEEEEEEE); // nothing created

    // IDs 1 to 4. The characters and the terminator aren't checked; read only is an option.
    CHECK(create_context(g, b) == 1);
    CHECK(create_context(g, b, 1) == 2);
    put_comm_id(b, "npwr", 7);
    CHECK(create_context(g, b) == 3);
    put_comm_id(b, "NPWR98765", 0, 'X');
    CHECK(create_context(g, b) == 4);
    ps_write32(b, kCtx, 0xEEEEEEEE);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kExceedsMax);
    CHECK(ps_read32(b, kCtx) == 0xEEEEEEEE);

    // Destroying: the ID must be 1 to 4, then known.
    for (const u32 id : {0u, 5u, 0xFFFFFFFFu}) CHECK(g.call("sceNpTrophyDestroyContext", id) == kInvalArg);
    REQUIRE(g.call("sceNpTrophyDestroyContext", 2) == 0);
    CHECK(g.call("sceNpTrophyDestroyContext", 2) == kUnknownContext);
    CHECK(create_context(g, b) == 2); // all four were handed out: the lowest free one
    REQUIRE(g.call("sceNpTrophyDestroyContext", 3) == 0);
    REQUIRE(g.call("sceNpTrophyDestroyContext", 1) == 0);
    CHECK(create_context(g, b) == 1);

    // Term clears every context.
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
    CHECK(g.call("sceNpTrophyDestroyContext", 1) == kNotInit);
    trophy_up(g);
    for (u32 id = 1; id <= 4; id++) CHECK(g.call("sceNpTrophyDestroyContext", id) == kUnknownContext);
    CHECK(create_context(g, b) == 1);
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
}

TEST_CASE("sceNpTrophy handles: IDs 1 to 4 that term doesn't rewind; abort and destroy without init",
    "[hle][trophy]")
{
    Fixture f;
    GuestThread g(f, 0, "trophy");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));

    // Creating needs the library; destroying and aborting don't (RPCS3: unknown whether a PS3 checks).
    ps_write32(b, kHandle, 0xEEEEEEEE);
    CHECK(g.call("sceNpTrophyCreateHandle", kHandle) == kNotInit);
    CHECK(g.call("sceNpTrophyCreateHandle", 0) == kNotInit);
    CHECK(ps_read32(b, kHandle) == 0xEEEEEEEE);
    CHECK(g.call("sceNpTrophyDestroyHandle", 1) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyAbortHandle", 1) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyDestroyHandle", 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyAbortHandle", 5) == kInvalArg);

    trophy_up(g);
    CHECK(g.call("sceNpTrophyCreateHandle", 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateHandle", kUnmapped) == kInvalArg); // RPCS3 faults
    CHECK(create_handle(g, b) == 1);
    CHECK(create_handle(g, b) == 2);

    // Term clears the handles, but their IDs go on from where they were (idm keeps its highest index).
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
    trophy_up(g);
    CHECK(g.call("sceNpTrophyDestroyHandle", 1) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyAbortHandle", 2) == kUnknownHandle);
    CHECK(create_handle(g, b) == 3);
    CHECK(create_handle(g, b) == 4);
    CHECK(create_handle(g, b) == 1);
    CHECK(create_handle(g, b) == 2);
    ps_write32(b, kHandle, 0xEEEEEEEE);
    CHECK(g.call("sceNpTrophyCreateHandle", kHandle) == kExceedsMax);
    CHECK(ps_read32(b, kHandle) == 0xEEEEEEEE);

    // Contexts have IDs of their own.
    put_comm_id(b, "NPWR01234", 0);
    put_sign(b);
    CHECK(create_context(g, b) == 1);

    // An aborted handle stays until it is destroyed.
    for (const u32 id : {0u, 5u, 0xFFFFFFFFu}) {
        CHECK(g.call("sceNpTrophyAbortHandle", id) == kInvalArg);
        CHECK(g.call("sceNpTrophyDestroyHandle", id) == kInvalArg);
    }
    REQUIRE(g.call("sceNpTrophyAbortHandle", 3) == 0);
    CHECK(g.call("sceNpTrophyAbortHandle", 3) == 0);
    REQUIRE(g.call("sceNpTrophyDestroyHandle", 3) == 0);
    CHECK(g.call("sceNpTrophyAbortHandle", 3) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyDestroyHandle", 3) == kUnknownHandle);
    CHECK(create_handle(g, b) == 3);

    // After term, destroying and aborting still answer (the handles are gone).
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
    CHECK(g.call("sceNpTrophyDestroyHandle", 1) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyAbortHandle", 4) == kUnknownHandle);
}
