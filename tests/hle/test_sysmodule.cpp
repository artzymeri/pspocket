// cellSysmodule's two names past the module calls (D-639): the export 0x59521326 (the OpenPSID, its halves swapped,
// decrypted under libsysmodule's key from the build, as RPCS3 makes it) and RPCS3's variable s_sysmodule_context (its
// layout, kept in step with the module states once linked). Synthetic: no game data, and no key in the source: the
// expected bytes are computed here from the build's key.

#include <array>
#include <cstring>
#include <string_view>

#include "hle_test_util.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/crypto.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/sysmodule_key.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kMinus22 = 0xFFFFFFFFFFFFFFEAull; // RPCS3's -22, sign-extended into r3

lv2::Config psid_config(u64 high, u64 low)
{
    lv2::Config c;
    c.console_psid_high = high;
    c.console_psid_low = low;
    return c;
}

// What the export gives for this OpenPSID: the swapped halves P (low first), decrypted as one CBC block whose IV is P,
// so D(P) XOR P, with the AES block functions (crypto::Aes) rather than the CBC the library calls.
std::array<u8, 16> expected(u64 high, u64 low)
{
    std::array<u8, 16> p{}, d{};
    ps_st64(p.data(), low);
    ps_st64(p.data() + 8, high);
    const recomp::crypto::Aes aes(recomp::builtin_sysmodule_key().key);
    REQUIRE(aes.valid());
    aes.decrypt_block(p.data(), d.data());
    for (usize i = 0; i < 16; i++) d[i] ^= p[i];
    return d;
}

std::array<u8, 16> output_of(Fixture& f, GuestThread& g, u32 at)
{
    std::array<u8, 16> out{};
    REQUIRE(g.call("cellSysmodule_0x59521326", at) == 0);
    std::memcpy(out.data(), f.base() + at, out.size());
    return out;
}

// s_sysmodule_context's fields (RPCS3 cellSysmodule.cpp sysmodule_context).
constexpr u32 kUnkAddr = 0x18, kContainerId = 0x1C, kUseContainer = 0x20, kStates = 0x24, kBasePath = 0x60C,
              kBasePathSize = 0x61C;
u32 count_at(u8* b, u32 ctx, u32 state) { return ps_read32(b, ctx + kStates + 8 * state); }
u32 prx_at(u8* b, u32 ctx, u32 state) { return ps_read32(b, ctx + kStates + 8 * state + 4); }
std::string_view text_at(u8* b, u32 addr, usize size) { return {reinterpret_cast<const char*>(b + addr), size}; }

} // namespace

TEST_CASE("cellSysmodule_0x59521326: the OpenPSID, halves swapped, decrypted as RPCS3 does (D-639)", "[hle][sysmodule]")
{
    if (!recomp::builtin_sysmodule_key().present) {
        // A build without RPCS3's source has no key: the export is left unimplemented.
        CHECK(GuestThread::lookup("cellSysmodule_0x59521326") == nullptr);
        SKIP("no libsysmodule key in this build (configured without RPCS3's source)");
    }
    CHECK(registry().is_unverified("cellSysmodule", 0x59521326));

    constexpr u64 kHigh = 0x0123456789ABCDEFull, kLow = 0xFEDCBA9876543210ull;
    constexpr u32 kOut = kData + 0x100;
    std::array<u8, 16> first{};
    {
        Fixture f({}, psid_config(kHigh, kLow));
        GuestThread g(f, 0, "game");
        u8* b = f.base();
        std::memset(b + kOut - 16, 0xEE, 48);
        first = output_of(f, g, kOut);
        CHECK(first == expected(kHigh, kLow));
        for (u32 i = 0; i < 16; i++) {
            INFO(i);
            CHECK(ps_read8(b, kOut - 16 + i) == 0xEE); // nothing before
            CHECK(ps_read8(b, kOut + 16 + i) == 0xEE); // nor after
        }
        // Encrypting it back under the same IV gives the swapped OpenPSID.
        std::array<u8, 16> p{}, back{};
        ps_st64(p.data(), kLow);
        ps_st64(p.data() + 8, kHigh);
        REQUIRE(recomp::crypto::aes_cbc_encrypt(recomp::builtin_sysmodule_key().key, p.data(), first.data(), 16,
            back.data()));
        CHECK(back == p);
        CHECK(output_of(f, g, kOut) == first); // the same each time

        // No output, or one that is not guest memory: -22, nothing written.
        CHECK(g.call("cellSysmodule_0x59521326", 0) == kMinus22);
        CHECK(g.call("cellSysmodule_0x59521326", 0xF0000000u) == kMinus22);
    }
    // Another console: another answer; the halves are swapped first, so the swapped PSID is not the same console.
    {
        Fixture f({}, psid_config(kLow, kHigh));
        GuestThread g(f, 0, "game");
        const std::array<u8, 16> swapped = output_of(f, g, kOut);
        CHECK(swapped == expected(kLow, kHigh));
        CHECK(swapped != first);
    }
    {
        Fixture f({}, psid_config(kHigh, kLow + 1));
        GuestThread g(f, 0, "game");
        CHECK(output_of(f, g, kOut) != first);
    }
}

TEST_CASE("s_sysmodule_context: RPCS3's layout, kept in step with the module states once linked (D-639)",
    "[hle][sysmodule]")
{
    Fixture f; // SDK 0x00360001: lv2 loaded the seven default modules
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    const u32 nid = recomp::nid_of("s_sysmodule_context");
    const Variable* v = registry().find_variable("cellSysmodule", nid);
    REQUIRE(v != nullptr);
    CHECK(v->size == 0x620);
    CHECK(v->align == 8);

    // Loaded before anything links the variable: there once it is laid out.
    REQUIRE(a.call("cellSysmoduleLoadModule", 0x09) == 0); // RTC
    const u32 ctx = f.hle->link_variable("cellSysmodule", nid);
    REQUIRE(ctx != 0);
    CHECK(ctx % 8 == 0);
    CHECK(f.hle->link_variable("cellSysmodule", nid) == ctx);

    for (u32 i = 0; i < 0x18; i++) CHECK(ps_read8(b, ctx + i) == 0); // the lwmutex: none made here
    CHECK(ps_read32(b, ctx + kUnkAddr) == 0);
    CHECK(ps_read32(b, ctx + kContainerId) == 0);
    CHECK(ps_read8(b, ctx + kUseContainer) == 0);
    // Counts: the seven defaults (SYSUTIL, GCM_SYS, AUDIO, IO, SPURS, FS; NP trophy, internal 0xF035 at 93 + 0x35),
    // RTC, and the retail variants of GCM_SYS and FS in step (libgcm_sys at 183, libfs at 184); no PRX for any.
    for (u32 i = 0; i < 189; i++) {
        INFO(i);
        const bool loaded = i == 0x15 || i == 0x10 || i == 0x11 || i == 0x17 || i == 0x0A || i == 0x0E ||
            i == 93 + 0x35 || i == 0x09 || i == 183 || i == 184;
        CHECK(count_at(b, ctx, i) == (loaded ? 1u : 0u));
        CHECK(prx_at(b, ctx, i) == 0xFFFFFFFFu);
    }
    CHECK(text_at(b, ctx + kBasePath, 16) == std::string_view("/dev_flash/sys/\0", 16));
    CHECK(ps_read8(b, ctx + kBasePathSize) == 15);

    // Loads and unloads, with their dependencies (NET needs NETCTL).
    REQUIRE(a.call("cellSysmoduleLoadModule", 0x00) == 0);
    CHECK(count_at(b, ctx, 0x00) == 1);
    CHECK(count_at(b, ctx, 0x14) == 1);
    REQUIRE(a.call("cellSysmoduleUnloadModule", 0x00) == 0);
    CHECK(count_at(b, ctx, 0x00) == 0);
    CHECK(count_at(b, ctx, 0x14) == 0);
    REQUIRE(a.call("cellSysmoduleUnloadModule", 0x09) == 0);
    CHECK(count_at(b, ctx, 0x09) == 0);
    // LV2DBG with its retail variant (liblv2dbg_for_cex at 181).
    REQUIRE(a.call("cellSysmoduleLoadModule", 0x2E) == 0);
    CHECK(count_at(b, ctx, 0x2E) == 1);
    CHECK(count_at(b, ctx, 181) == 1);
    CHECK(count_at(b, ctx, 180) == 0);
    // An internal module (0xF018, libtiffdec, at 93 + 0x18) and its dependency FS, with libfs in step.
    REQUIRE(a.call("cellSysmoduleLoadModuleInternal", 0xF018) == 0);
    CHECK(count_at(b, ctx, 93 + 0x18) == 1);
    CHECK(count_at(b, ctx, 0x0E) == 2);
    CHECK(count_at(b, ctx, 184) == 2);
    REQUIRE(a.call("cellSysmoduleUnloadModuleInternal", 0xF018) == 0);
    CHECK(count_at(b, ctx, 93 + 0x18) == 0);
    CHECK(count_at(b, ctx, 0x0E) == 1);
    CHECK(count_at(b, ctx, 184) == 1);

    // The debug mode's path over the retail one, as RPCS3 copies it (the tail of the longer path stays), and back.
    REQUIRE(a.call("cellSysmoduleSetDebugmode", 1) == 0);
    CHECK(text_at(b, ctx + kBasePath, 16) == std::string_view("/app_home/\0sys/\0", 16));
    CHECK(ps_read8(b, ctx + kBasePathSize) == 10);
    REQUIRE(a.call("cellSysmoduleSetDebugmode", 0) == 0);
    CHECK(text_at(b, ctx + kBasePath, 16) == std::string_view("/dev_flash/sys/\0", 16));
    CHECK(ps_read8(b, ctx + kBasePathSize) == 15);

    // The memory container: taken, given up (its ID stays), refused (likewise).
    constexpr u32 kId = kData + 0x40;
    REQUIRE(lv2::sys_memory_container_create(*a.t, kId, 0x100000) == lv2::CELL_OK);
    const u32 ct = ps_read32(b, kId);
    REQUIRE(a.call("cellSysmoduleSetMemcontainer", ct) == 0);
    CHECK(ps_read32(b, ctx + kContainerId) == ct);
    CHECK(ps_read8(b, ctx + kUseContainer) == 1);
    REQUIRE(a.call("cellSysmoduleSetMemcontainer", 0xFFFFFFFFu) == 0);
    CHECK(ps_read32(b, ctx + kContainerId) == ct);
    CHECK(ps_read8(b, ctx + kUseContainer) == 0);
    REQUIRE(a.call("cellSysmoduleSetMemcontainer", ct) == 0);
    CHECK(a.call("cellSysmoduleSetMemcontainer", ct + 1) == 0xFFFFFFFF80012004ull);
    CHECK(ps_read32(b, ctx + kContainerId) == ct);
    CHECK(ps_read8(b, ctx + kUseContainer) == 0);
}
