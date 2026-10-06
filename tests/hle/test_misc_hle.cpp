// Smaller offline functions GTA V calls at startup (synthetic: no game data): cellSysutil's background music state,
// cellNetCtl's information, NAT info and network start dialog without a network (its events delivered to a guest
// sysutil callback), and sys_net_free_thread_context.

#include "hle_test_util.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

// Guest functions without lifted code run in the interpreter while this lives (the default policy aborts).
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// A guest sysutil callback(status, param, userdata) behind the descriptor kOpd. Each call appends the low words of its
// three arguments to a log at kLog + 16 (16 bytes an entry) and counts the entries at kLog.
constexpr u32 kCode = kData + 0x10000, kOpd = kData + 0x10800, kLog = kData + 0x11000, kToc = 0x00123400;

void write_callback(u8* base)
{
    const std::vector<u32> code = {
        as::lis(6, static_cast<s32>(kLog >> 16)),
        as::ori(6, 6, kLog & 0xFFFF),
        as::lwz(7, 0, 6),
        as::slwi(8, 7, 4),
        as::addi(8, 8, 16),
        as::stwx(3, 6, 8),
        as::addi(9, 8, 4),
        as::stwx(4, 6, 9),
        as::addi(9, 8, 8),
        as::stwx(5, 6, 9),
        as::addi(7, 7, 1),
        as::stw(7, 0, 6),
        as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(base, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(base, kOpd, kCode);
    ps_write32(base, kOpd + 4, kToc);
    std::memset(base + kLog, 0, 0x100);
}

u32 callback_calls(u8* b) { return ps_read32(b, kLog); }
u32 callback_status(u8* b, u32 i) { return ps_read32(b, kLog + 16 + 16 * i); }
u32 callback_param(u8* b, u32 i) { return ps_read32(b, kLog + 20 + 16 * i); }
u32 callback_userdata(u8* b, u32 i) { return ps_read32(b, kLog + 24 + 16 * i); }

bool all_bytes(const u8* p, usize n, u8 value)
{
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

constexpr u64 kNetNotInit = 0xFFFFFFFF80130101ull, kNetBadAddr = 0xFFFFFFFF80130107ull,
              kNetNotConnected = 0xFFFFFFFF80130108ull, kNetBadType = 0xFFFFFFFF8013010Aull,
              kNetBadSize = 0xFFFFFFFF8013010Bull;

} // namespace

TEST_CASE("cellSysutil BGM, cellNetCtl, sys_net: the functions GTA V imports, in their modules", "[hle][net]")
{
    struct Expected {
        std::string_view module, name;
        u32 nid;
    };
    const Expected expected[] = {
        {"cellSysutil", "cellSysutilEnableBgmPlayback", 0x220894E3},
        {"cellSysutil", "cellSysutilDisableBgmPlayback", 0xCFDD8E87},
        {"cellSysutil", "cellSysutilGetBgmPlaybackStatus", 0xA11552F6},
        {"cellNetCtl", "cellNetCtlGetInfo", 0x1E585B5D},
        {"cellNetCtl", "cellNetCtlGetNatInfo", 0x3A12865F},
        {"cellNetCtl", "cellNetCtlNetStartDialogLoadAsync", 0x04459230},
        {"cellNetCtl", "cellNetCtlNetStartDialogUnloadAsync", 0x0F1F13D3},
        {"sys_net", "sys_net_free_thread_context", 0xFDB8F926},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.module << "::" << e.name);
        const hle::Function* fn = hle::registry().find(e.module, e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
    }
}

TEST_CASE("cellSysutil background music: RPCS3's state, disabled and stopped at first", "[hle][sysutil]")
{
    Fixture f;
    GuestThread g(f, 0, "bgm");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    constexpr u32 kStatus = kData + 0x100;
    const u64 kParam = r3_of_code(0x8002b101); // CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM

    // CellSysutilBgmPlaybackStatus (32 bytes): playerState, enableState, then the content ID, fade ratio and reserved
    // bytes, all written (zero).
    CHECK(g.call("cellSysutilGetBgmPlaybackStatus", 0) == kParam);
    CHECK(g.call("cellSysutilGetBgmPlaybackStatus", kUnmapped) == kParam); // RPCS3 faults
    std::memset(b + kStatus, 0xAA, 40);
    REQUIRE(g.call("cellSysutilGetBgmPlaybackStatus", kStatus) == 0);
    CHECK(b[kStatus + 0] == 1); // CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP
    CHECK(b[kStatus + 1] == 1); // CELL_SYSUTIL_BGMPLAYBACK_STATUS_DISABLE
    CHECK(all_bytes(b + kStatus + 2, 30, 0));
    CHECK(all_bytes(b + kStatus + 32, 8, 0xAA));

    // GTA V enables it twice around a status read. Enabling and disabling change only the enable state.
    REQUIRE(g.call("cellSysutilEnableBgmPlayback") == 0);
    std::memset(b + kStatus, 0xAA, 32);
    REQUIRE(g.call("cellSysutilGetBgmPlaybackStatus", kStatus) == 0);
    CHECK(b[kStatus + 0] == 1);
    CHECK(b[kStatus + 1] == 0); // CELL_SYSUTIL_BGMPLAYBACK_STATUS_ENABLE
    CHECK(all_bytes(b + kStatus + 2, 30, 0));
    CHECK(g.call("cellSysutilEnableBgmPlayback") == 0);
    REQUIRE(g.call("cellSysutilDisableBgmPlayback") == 0);
    REQUIRE(g.call("cellSysutilGetBgmPlaybackStatus", kStatus) == 0);
    CHECK(b[kStatus + 0] == 1);
    CHECK(b[kStatus + 1] == 1);
    CHECK(g.call("cellSysutilDisableBgmPlayback") == 0);
}

TEST_CASE("cellNetCtl without a network: only the Ethernet address, RPCS3's NAT info", "[hle][net]")
{
    Fixture f;
    GuestThread g(f, 0, "net");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    constexpr u32 kInfo = kData + 0x100, kNat = kData + 0x300;
    constexpr u32 kInfoSize = 0x104; // CellNetCtlInfo: the largest member is a 256-byte host name (+ a be32)

    std::memset(b + kInfo, 0xAA, kInfoSize);
    CHECK(g.call("cellNetCtlGetInfo", 2, kInfo) == kNetNotInit);
    CHECK(g.call("cellNetCtlGetInfo", 2, 0) == kNetNotInit);
    CHECK(g.call("cellNetCtlGetNatInfo", kNat) == kNetNotInit);
    REQUIRE(g.call("cellNetCtlInit") == 0);
    CHECK(g.call("cellNetCtlGetInfo", 2, 0) == kNetBadAddr);
    CHECK(g.call("cellNetCtlGetInfo", 4, 0) == kNetBadAddr); // the address is checked before the connection
    CHECK(g.call("cellNetCtlGetInfo", 2, kUnmapped) == kNetBadAddr); // RPCS3 faults

    // GTA V asks for the link (4), the Ethernet address (2) and the IP address (16). The Ethernet address is what
    // RPCS3 has before it discovers one (only on a network): FF:FF:FF:FF:FF:FF. The union's padding isn't written.
    REQUIRE(g.call("cellNetCtlGetInfo", 2, kInfo) == 0);
    CHECK(all_bytes(b + kInfo, 6, 0xFF));
    CHECK(all_bytes(b + kInfo + 6, kInfoSize - 6, 0xAA));
    std::memset(b + kInfo, 0xAA, kInfoSize);
    for (s32 code = -1; code <= 27; code++) {
        if (code == 2) continue;
        INFO("code " << code);
        CHECK(g.call("cellNetCtlGetInfo", code, kInfo) == kNetNotConnected);
    }
    CHECK(g.call("cellNetCtlGetInfo", 0x100, kInfo) == kNetNotConnected);
    CHECK(all_bytes(b + kInfo, kInfoSize, 0xAA));

    // NAT info (CellNetCtlNatInfo: size, upnp_status, stun_status, nat_type, mapped_addr): the size must be 16 or 20;
    // RPCS3 answers NAT type 2 and STUN OK even without a network, and UPnP not in use; the mapped address isn't
    // written.
    CHECK(g.call("cellNetCtlGetNatInfo", 0) == kNetBadAddr);
    CHECK(g.call("cellNetCtlGetNatInfo", kUnmapped) == kNetBadAddr); // RPCS3 faults
    for (const u32 size : {0u, 12u, 15u, 17u, 24u, 0xFFFFFFFFu}) {
        std::memset(b + kNat, 0xAA, 20);
        ps_write32(b, kNat, size);
        CHECK(g.call("cellNetCtlGetNatInfo", kNat) == kNetBadSize);
        CHECK(all_bytes(b + kNat + 4, 16, 0xAA));
    }
    for (const u32 size : {16u, 20u}) {
        std::memset(b + kNat, 0xAA, 20);
        ps_write32(b, kNat, size);
        REQUIRE(g.call("cellNetCtlGetNatInfo", kNat) == 0);
        CHECK(ps_read32(b, kNat + 0) == size);
        CHECK(ps_read32(b, kNat + 4) == 1);  // SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_INVALID, = UPNP_NO
        CHECK(ps_read32(b, kNat + 8) == 2);  // CELL_NET_CTL_NATINFO_STUN_OK
        CHECK(ps_read32(b, kNat + 12) == 2); // CELL_NET_CTL_NATINFO_NAT_TYPE_2
        CHECK(ps_read32(b, kNat + 16) == 0xAAAAAAAA);
    }

    g.call("cellNetCtlTerm");
    CHECK(g.call("cellNetCtlGetInfo", 2, kInfo) == kNetNotInit);
    CHECK(g.call("cellNetCtlGetNatInfo", kNat) == kNetNotInit);
}

TEST_CASE("cellNetCtl network start dialog: loaded and finished 500 ms after the load, canceled at unload",
    "[hle][net]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread g(f, 0, "net");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    write_callback(b);
    constexpr u32 kParam = kData + 0x100, kResult = kData + 0x200;

    // CellNetCtlNetStartDialogParam {size 12, type, cid}; CellNetCtlNetStartDialogResult {size 8, result}.
    ps_write32(b, kParam + 0, 12);
    ps_write32(b, kParam + 4, 0); // CELL_NET_CTL_NETSTART_TYPE_NET
    ps_write32(b, kParam + 8, 0);
    ps_write32(b, kResult + 0, 8);
    ps_write32(b, kResult + 4, 0xAAAAAAAA);
    CHECK(g.call("cellNetCtlNetStartDialogLoadAsync", kParam) == kNetNotInit);
    CHECK(g.call("cellNetCtlNetStartDialogUnloadAsync", kResult) == kNetNotInit);
    REQUIRE(g.call("cellNetCtlInit") == 0);

    // The load: the pointer, the type (a signed compare), then the size.
    CHECK(g.call("cellNetCtlNetStartDialogLoadAsync", 0) == kNetBadAddr);
    CHECK(g.call("cellNetCtlNetStartDialogLoadAsync", kUnmapped) == kNetBadAddr); // RPCS3 faults
    ps_write32(b, kParam + 0, 16);
    ps_write32(b, kParam + 4, 2); // CELL_NET_CTL_NETSTART_TYPE_MAX
    CHECK(g.call("cellNetCtlNetStartDialogLoadAsync", kParam) == kNetBadType);
    ps_write32(b, kParam + 4, 1); // CELL_NET_CTL_NETSTART_TYPE_NP
    CHECK(g.call("cellNetCtlNetStartDialogLoadAsync", kParam) == kNetBadSize);

    // A callback registered after the load still gets LOADED and FINISHED (RPCS3's hack for Diva F 2nd), at the
    // check after the first one at least 500 ms after the load.
    ps_write32(b, kParam + 0, 12);
    ps_write32(b, kParam + 4, 0xFFFFFFFF); // negative: below TYPE_MAX
    const auto before = std::chrono::steady_clock::now();
    REQUIRE(g.call("cellNetCtlNetStartDialogLoadAsync", kParam) == 0);
    const auto after = std::chrono::steady_clock::now();
    REQUIRE(g.call("cellSysutilRegisterCallback", 1, kOpd, 0x5150) == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    // Both checks ran less than 500 ms after the load: nothing sent yet (on a host slow enough to take that long, the
    // events may already be queued; that is checked below).
    if (std::chrono::steady_clock::now() < before + std::chrono::milliseconds(500)) CHECK(callback_calls(b) == 0);
    std::this_thread::sleep_until(after + std::chrono::milliseconds(510));
    REQUIRE(g.call("cellSysutilCheckCallback") == 0); // sends them (if an earlier check didn't)
    REQUIRE(g.call("cellSysutilCheckCallback") == 0); // delivers them
    REQUIRE(callback_calls(b) == 2);
    CHECK(callback_status(b, 0) == 0x0801); // CELL_SYSUTIL_NET_CTL_NETSTART_LOADED
    CHECK(callback_status(b, 1) == 0x0802); // CELL_SYSUTIL_NET_CTL_NETSTART_FINISHED
    for (u32 i = 0; i < 2; i++) {
        CHECK(callback_param(b, i) == 0);
        CHECK(callback_userdata(b, i) == 0x5150);
    }
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(callback_calls(b) == 2); // once

    // The unload: the pointer, then the size. No IP address was obtained, so the dialog was canceled; UNLOADED goes
    // to the callbacks at once.
    CHECK(g.call("cellNetCtlNetStartDialogUnloadAsync", 0) == kNetBadAddr);
    CHECK(g.call("cellNetCtlNetStartDialogUnloadAsync", kUnmapped) == kNetBadAddr); // RPCS3 faults
    ps_write32(b, kResult + 0, 12);
    CHECK(g.call("cellNetCtlNetStartDialogUnloadAsync", kResult) == kNetBadSize);
    CHECK(ps_read32(b, kResult + 4) == 0xAAAAAAAA);
    ps_write32(b, kResult + 0, 8);
    REQUIRE(g.call("cellNetCtlNetStartDialogUnloadAsync", kResult) == 0);
    CHECK(ps_read32(b, kResult + 0) == 8);
    CHECK(ps_read32(b, kResult + 4) == 0x80130190); // CELL_NET_CTL_ERROR_DIALOG_CANCELED
    CHECK(callback_calls(b) == 2);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    REQUIRE(callback_calls(b) == 3);
    CHECK(callback_status(b, 2) == 0x0803); // CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED
    CHECK(callback_userdata(b, 2) == 0x5150);
    g.call("cellNetCtlTerm");
}

TEST_CASE("sys_net_free_thread_context: there is no thread context to free", "[hle][net]")
{
    Fixture f;
    GuestThread g(f, 0, "net");
    CHECK(g.call("sys_net_free_thread_context", 0, 1) == 0); // GTA V's call
    CHECK(g.call("sys_net_free_thread_context", 0x0100000000000001ull, 0) == 0);
}

TEST_CASE("cellNetCtl: game update checks find no update; the start dialog abort", "[hle][net]")
{
    InterpretMisses interpret;
    Fixture f;
    GuestThread g(f, 0, "update");
    u8* b = f.base();
    write_callback(b);
    constexpr u32 kParam = kData + 0x100;

    CHECK(g.call("cellNetCtlNetStartDialogAbortAsync") == kNetNotInit);
    REQUIRE(g.call("cellNetCtlInit") == 0);
    CHECK(g.call("cellNetCtlNetStartDialogAbortAsync") == 0);

    // (status, error, userdata) from the next cellSysutilCheckCallback: NO_UPDATE (0) for a start, FINISHED (5) at the
    // finish; no cellGameUpdateInit needed (RPCS3).
    REQUIRE(g.call("cellGameUpdateCheckStartAsync", kParam, kOpd, 0x1111) == 0);
    REQUIRE(g.call("cellGameUpdateCheckStartWithoutDialogAsync", kOpd, 0x2222) == 0);
    REQUIRE(g.call("cellGameUpdateCheckFinishAsync", kOpd, 0x3333) == 0);
    CHECK(callback_calls(b) == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    REQUIRE(callback_calls(b) == 3);
    CHECK(callback_status(b, 0) == 0);
    CHECK(callback_param(b, 0) == 0);
    CHECK(callback_userdata(b, 0) == 0x1111);
    CHECK(callback_status(b, 1) == 0);
    CHECK(callback_userdata(b, 1) == 0x2222);
    CHECK(callback_status(b, 2) == 5);
    CHECK(callback_userdata(b, 2) == 0x3333);

    // The Ex callbacks get (CellGameUpdateResult*, userdata): {status, error_code, app_ver}.
    for (const auto& [fn, status] : {std::pair<const char*, u32>{"cellGameUpdateCheckStartWithoutDialogAsyncEx", 0},
             {"cellGameUpdateCheckFinishAsyncEx", 5}}) {
        INFO(fn);
        std::memset(b + kLog, 0, 0x100);
        REQUIRE(g.call(fn, kOpd, 0x4444) == 0);
        REQUIRE(g.call("cellSysutilCheckCallback") == 0);
        REQUIRE(callback_calls(b) == 1);
        const u32 result = callback_status(b, 0);
        CHECK(callback_param(b, 0) == 0x4444);
        CHECK(ps_read32(b, result) == status);
        CHECK(ps_read32(b, result + 4) == 0);
    }
    std::memset(b + kLog, 0, 0x100);
    REQUIRE(g.call("cellGameUpdateCheckStartAsyncEx", kParam, kOpd, 0x5555) == 0);
    REQUIRE(g.call("cellSysutilCheckCallback") == 0);
    CHECK(callback_param(b, 0) == 0x5555);
    REQUIRE(g.call("cellNetCtlTerm") == 0);
}
