// cellNetCtl: network connection control. Online play is a non-goal (D-024): the library initializes, reports the
// connection as disconnected, and accepts handler registrations that never fire.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellNetCtl.cpp/.h (with RPCS3's network set to "disconnected"), and the
// connection details of rpcs3/Emu/NP/np_handler.cpp.
//
// Disconnected, as RPCS3's np_handler: the state is Disconnected, so cellNetCtlGetInfo answers only the Ethernet
// address, which np_handler discovers only on a connected network and otherwise leaves at FF:FF:FF:FF:FF:FF; every
// other code is CELL_NET_CTL_ERROR_NOT_CONNECTED. The NAT info is RPCS3's fixed answer (NAT type 2, STUN OK) with UPnP
// not in use. The network start dialog has no UI: as in RPCS3 it reports itself loaded and finished 500 ms after the
// load, and the unload reports it canceled (no IP address was obtained). A game update check (cellGameUpdateCheck*,
// also in this library) finds no update and finishes, from the next cellSysutilCheckCallback, as in RPCS3.
//
// Differences from RPCS3: pointers are checked before use (D-042): one that is not guest memory is logged with the call
// site and refused with CELL_NET_CTL_ERROR_INVALID_ADDR (RPCS3 faults). RPCS3 sends the dialog's LOADED and FINISHED
// events from a helper thread 500 ms after the load; here the first cellSysutilCheckCallback at least 500 ms after it
// sends them (sysutil_queue_call), so they reach the callbacks registered by then at the check after that, and no
// host thread outlives the process.

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "sysutil.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellNetCtl";

enum : u32 {
    CELL_NET_CTL_ERROR_NOT_INITIALIZED = 0x80130101,
    CELL_NET_CTL_ERROR_NOT_TERMINATED = 0x80130102,
    CELL_NET_CTL_ERROR_INVALID_ID = 0x80130105,
    CELL_NET_CTL_ERROR_INVALID_ADDR = 0x80130107,
    CELL_NET_CTL_ERROR_NOT_CONNECTED = 0x80130108,
    CELL_NET_CTL_ERROR_INVALID_TYPE = 0x8013010a,
    CELL_NET_CTL_ERROR_INVALID_SIZE = 0x8013010b,
    CELL_NET_CTL_ERROR_DIALOG_CANCELED = 0x80130190,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }
constexpr s32 CELL_NET_CTL_STATE_Disconnected = 0;

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case CELL_NET_CTL_ERROR_NOT_INITIALIZED: return "CELL_NET_CTL_ERROR_NOT_INITIALIZED";
    case CELL_NET_CTL_ERROR_INVALID_ADDR: return "CELL_NET_CTL_ERROR_INVALID_ADDR";
    case CELL_NET_CTL_ERROR_NOT_CONNECTED: return "CELL_NET_CTL_ERROR_NOT_CONNECTED";
    case CELL_NET_CTL_ERROR_INVALID_TYPE: return "CELL_NET_CTL_ERROR_INVALID_TYPE";
    case CELL_NET_CTL_ERROR_INVALID_SIZE: return "CELL_NET_CTL_ERROR_INVALID_SIZE";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

// cellNetCtlGetInfo codes (RPCS3 InfoCodeToName), for logs.
constexpr std::array<std::string_view, 27> kInfoNames = {"?", "DEVICE", "ETHER_ADDR", "MTU", "LINK", "LINK_TYPE",
    "BSSID", "SSID", "WLAN_SECURITY", "8021X_TYPE", "8021X_AUTH_NAME", "RSSI", "CHANNEL", "IP_CONFIG", "DHCP_HOSTNAME",
    "PPPOE_AUTH_NAME", "IP_ADDRESS", "NETMASK", "DEFAULT_ROUTE", "PRIMARY_DNS", "SECONDARY_DNS", "HTTP_PROXY_CONFIG",
    "HTTP_PROXY_SERVER", "HTTP_PROXY_PORT", "UPNP_CONFIG", "RESERVED1", "RESERVED2"};
constexpr s32 CELL_NET_CTL_INFO_ETHER_ADDR = 2;
// RPCS3 np_handler::ether_address before discover_ether_address, which runs only on a connected network.
constexpr std::array<u8, 6> kEtherAddr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// CellNetCtlNatInfo (20 bytes): be32 size (16 or 20); be s32 upnp_status, stun_status, nat_type; be32 mapped_addr.
constexpr u32 kNatInfoWritten = 16;
constexpr s32 CELL_NET_CTL_NATINFO_STUN_OK = 2;
constexpr s32 CELL_NET_CTL_NATINFO_NAT_TYPE_2 = 2;
constexpr s32 SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_INVALID = 1; // np_handler::get_upnp_status without UPnP

// CellNetCtlNetStartDialogParam (12 bytes): be32 size (12), be s32 type, be32 cid. CellNetCtlNetStartDialogResult
// (8 bytes): be32 size (8), be s32 result.
constexpr u32 kDialogParamSize = 12, kDialogResultSize = 8;
constexpr s32 CELL_NET_CTL_NETSTART_TYPE_MAX = 2; // NET, NP
// The dialog's system events, the statuses the game's sysutil callbacks get.
constexpr u64 CELL_SYSUTIL_NET_CTL_NETSTART_LOADED = 0x0801;
constexpr u64 CELL_SYSUTIL_NET_CTL_NETSTART_FINISHED = 0x0802;
constexpr u64 CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED = 0x0803;
// RPCS3 netstart_hack: how long after the load the dialog reports itself loaded and finished (for Diva F 2nd, which
// registers its sysutil callback after cellNetCtlNetStartDialogLoadAsync).
constexpr auto kNetStartDelay = std::chrono::milliseconds(500);

struct NetCtlState {
    std::atomic<bool> initialized{false};
    // cellNetCtlGetInfo codes already logged at info (RPCS3 logs most codes once).
    std::array<std::atomic<bool>, kInfoNames.size()> info_logged{};
};

NetCtlState& netctl() { return HleProcess::current()->state<NetCtlState>(); }

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

s32 cellNetCtlInit(PPUContext&)
{
    if (netctl().initialized.exchange(true)) return error(CELL_NET_CTL_ERROR_NOT_TERMINATED);
    PS_LOG_INFO(kChan, "cellNetCtlInit(): no network, the connection state is Disconnected");
    return lv2::CELL_OK;
}

void cellNetCtlTerm(PPUContext&) { netctl().initialized = false; }

s32 cellNetCtlGetState(PPUContext& ctx, u32 state)
{
    if (!netctl().initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!state) return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    ps_write32(thread_of(ctx).kernel.base(), state, static_cast<u32>(CELL_NET_CTL_STATE_Disconnected));
    return lv2::CELL_OK;
}

// RPCS3: a TODO that validates and returns CELL_OK without writing a handler ID.
s32 cellNetCtlAddHandler(PPUContext&, u32 handler, u32 arg, u32 hid)
{
    PS_LOG_DEBUG(kChan, "cellNetCtlAddHandler(handler {:#x}, arg {:#x}, hid {:#x}): never called (no network)", handler,
        arg, hid);
    if (!netctl().initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!hid) return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    return lv2::CELL_OK;
}

s32 cellNetCtlDelHandler(PPUContext&, s32 hid)
{
    if (!netctl().initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (hid > 3) return error(CELL_NET_CTL_ERROR_INVALID_ID);
    return lv2::CELL_OK;
}

// cellNetCtlGetInfo(code, CellNetCtlInfo* info): the Ethernet address (6 bytes; the union's 2 padding bytes are not
// written) whatever the state; every other code, known or not, needs a connection. The first call with each code is
// logged at info, later ones at debug.
s32 cellNetCtlGetInfo(PPUContext& ctx, s32 code, u32 info)
{
    NetCtlState& s = netctl();
    const bool known = code >= 0 && static_cast<usize>(code) < kInfoNames.size();
    const bool first = !known || !s.info_logged[static_cast<usize>(code)].exchange(true);
    const auto done = [&](s32 r) {
        log::print(first ? log::Level::Info : log::Level::Debug, kChan, "cellNetCtlGetInfo({:#x} {}, info {:#x}): {}",
            code, known ? kInfoNames[static_cast<usize>(code)] : "?", info, result_name(r));
        return r;
    };
    if (!s.initialized) return done(error(CELL_NET_CTL_ERROR_NOT_INITIALIZED));
    if (!info) return done(error(CELL_NET_CTL_ERROR_INVALID_ADDR));
    if (code != CELL_NET_CTL_INFO_ETHER_ADDR) return done(error(CELL_NET_CTL_ERROR_NOT_CONNECTED));
    constexpr u32 kEtherAddrSize = static_cast<u32>(kEtherAddr.size());
    if (!usable(ctx, info, kEtherAddrSize, "cellNetCtlGetInfo: info")) return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    u8* base = guest_base(ctx);
    for (u32 i = 0; i < kEtherAddrSize; i++) ps_write8(base, info + i, kEtherAddr[i]);
    return done(lv2::CELL_OK);
}

// cellNetCtlGetNatInfo(CellNetCtlNatInfo*): RPCS3 answers NAT type 2 with STUN OK even without a network, and its UPnP
// status (not in use). The mapped address is not written.
s32 cellNetCtlGetNatInfo(PPUContext& ctx, u32 nat_info)
{
    const auto done = [&](s32 r) {
        PS_LOG_INFO(kChan, "cellNetCtlGetNatInfo({:#x}): {}", nat_info, result_name(r));
        return r;
    };
    if (!netctl().initialized) return done(error(CELL_NET_CTL_ERROR_NOT_INITIALIZED));
    if (!nat_info) return done(error(CELL_NET_CTL_ERROR_INVALID_ADDR));
    if (!usable(ctx, nat_info, kNatInfoWritten, "cellNetCtlGetNatInfo: natInfo")) {
        return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    }
    u8* base = guest_base(ctx);
    const u32 size = ps_read32(base, nat_info);
    if (size != 16 && size != 20) return done(error(CELL_NET_CTL_ERROR_INVALID_SIZE));
    ps_write32(base, nat_info + 12, static_cast<u32>(CELL_NET_CTL_NATINFO_NAT_TYPE_2));
    ps_write32(base, nat_info + 8, static_cast<u32>(CELL_NET_CTL_NATINFO_STUN_OK));
    ps_write32(base, nat_info + 4, static_cast<u32>(SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_INVALID));
    return done(lv2::CELL_OK);
}

// Sends the dialog's LOADED and FINISHED events from the first cellSysutilCheckCallback at or after `due`; an earlier
// check puts this back in the queue for the next one.
void queue_netstart_events(std::chrono::steady_clock::time_point due)
{
    sysutil_queue_call([due](PPUContext&) -> s32 {
        if (std::chrono::steady_clock::now() < due) {
            queue_netstart_events(due);
            return lv2::CELL_OK;
        }
        PS_LOG_INFO(kChan, "network start dialog: LOADED and FINISHED (no dialog, no network)");
        sysutil_send_event(CELL_SYSUTIL_NET_CTL_NETSTART_LOADED, 0);
        sysutil_send_event(CELL_SYSUTIL_NET_CTL_NETSTART_FINISHED, 0);
        return lv2::CELL_OK;
    });
}

// cellNetCtlNetStartDialogLoadAsync(const CellNetCtlNetStartDialogParam*): the type is checked (a signed compare, as
// RPCS3), then the size; the dialog then finishes by itself (queue_netstart_events).
s32 cellNetCtlNetStartDialogLoadAsync(PPUContext& ctx, u32 param)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        PS_LOG_INFO(kChan, "cellNetCtlNetStartDialogLoadAsync({:#x}){}: {}", param, what, result_name(r));
        return r;
    };
    if (!netctl().initialized) return done(error(CELL_NET_CTL_ERROR_NOT_INITIALIZED));
    if (!param) return done(error(CELL_NET_CTL_ERROR_INVALID_ADDR));
    if (!usable(ctx, param, kDialogParamSize, "cellNetCtlNetStartDialogLoadAsync: param")) {
        return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    }
    u8* base = guest_base(ctx);
    const u32 size = ps_read32(base, param);
    const s32 type = static_cast<s32>(ps_read32(base, param + 4));
    const std::string what = std::format(" {{size {}, type {}}}", size, type);
    if (type >= CELL_NET_CTL_NETSTART_TYPE_MAX) return done(error(CELL_NET_CTL_ERROR_INVALID_TYPE), what);
    if (size != kDialogParamSize) return done(error(CELL_NET_CTL_ERROR_INVALID_SIZE), what);
    queue_netstart_events(std::chrono::steady_clock::now() + kNetStartDelay);
    return done(lv2::CELL_OK, what + ", no dialog: LOADED and FINISHED in 500 ms");
}

// cellNetCtlNetStartDialogUnloadAsync(CellNetCtlNetStartDialogResult*): the result is DIALOG_CANCELED because no IP
// address was obtained, and the UNLOADED event goes to the game's callbacks at once.
s32 cellNetCtlNetStartDialogUnloadAsync(PPUContext& ctx, u32 result)
{
    const auto done = [&](s32 r) {
        PS_LOG_INFO(kChan, "cellNetCtlNetStartDialogUnloadAsync({:#x}): {}", result, result_name(r));
        return r;
    };
    if (!netctl().initialized) return done(error(CELL_NET_CTL_ERROR_NOT_INITIALIZED));
    if (!result) return done(error(CELL_NET_CTL_ERROR_INVALID_ADDR));
    if (!usable(ctx, result, kDialogResultSize, "cellNetCtlNetStartDialogUnloadAsync: result")) {
        return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    }
    u8* base = guest_base(ctx);
    if (ps_read32(base, result) != kDialogResultSize) return done(error(CELL_NET_CTL_ERROR_INVALID_SIZE));
    ps_write32(base, result + 4, CELL_NET_CTL_ERROR_DIALOG_CANCELED);
    sysutil_send_event(CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED, 0);
    return done(lv2::CELL_OK);
}

// RPCS3 accepts an abort whenever the library is initialised.
s32 cellNetCtlNetStartDialogAbortAsync(PPUContext&)
{
    const s32 r = netctl().initialized ? lv2::CELL_OK : error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "cellNetCtlNetStartDialogAbortAsync(): {}", result_name(r));
    return r;
}

// ---- game update checks (RPCS3: they need no cellGameUpdateInit and check nothing)

constexpr s32 CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE = 0, CELL_GAMEUPDATE_RESULT_STATUS_FINISHED = 5;
constexpr u32 kGameUpdateResultSize = 16; // CellGameUpdateResult: be s32 status, error_code; char app_ver[6]; pad[2]

// The callback, from the next cellSysutilCheckCallback: (status, error 0, userdata), or for the Ex functions
// (CellGameUpdateResult*, userdata).
s32 game_update(const char* fn, u32 func, u32 userdata, s32 status, bool ex)
{
    PS_LOG_INFO(kChan, "{}(func {:#x}, userdata {:#x}): no update server; status {}", fn, func, userdata, status);
    if (!func) return lv2::CELL_OK; // RPCS3 would call address 0
    sysutil_queue_call([=](PPUContext& ctx) -> s32 {
        if (!ex) {
            call_guest(ctx, func, {static_cast<u64>(status), 0, userdata});
            return lv2::CELL_OK;
        }
        u8* base = guest_base(ctx);
        StackVar result(ctx, base, kGameUpdateResultSize);
        ps_write32(base, result.addr(), static_cast<u32>(status));
        call_guest(ctx, func, {result.addr(), userdata});
        return lv2::CELL_OK;
    });
    return lv2::CELL_OK;
}

s32 cellGameUpdateCheckStartAsync(PPUContext&, u32, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckStartAsync", func, userdata, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE, false);
}

s32 cellGameUpdateCheckStartAsyncEx(PPUContext&, u32, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckStartAsyncEx", func, userdata, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE, true);
}

s32 cellGameUpdateCheckStartWithoutDialogAsync(PPUContext&, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckStartWithoutDialogAsync", func, userdata,
        CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE, false);
}

s32 cellGameUpdateCheckStartWithoutDialogAsyncEx(PPUContext&, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckStartWithoutDialogAsyncEx", func, userdata,
        CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE, true);
}

s32 cellGameUpdateCheckFinishAsync(PPUContext&, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckFinishAsync", func, userdata, CELL_GAMEUPDATE_RESULT_STATUS_FINISHED, false);
}

s32 cellGameUpdateCheckFinishAsyncEx(PPUContext&, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckFinishAsyncEx", func, userdata, CELL_GAMEUPDATE_RESULT_STATUS_FINISHED, true);
}

} // namespace

void register_cellNetCtl(Registry& r)
{
    r.add("cellNetCtl", {
        PS_HLE_FUNC(0xBD5A59FC, cellNetCtlInit),
        PS_HLE_FUNC(0x105EE2CB, cellNetCtlTerm),
        PS_HLE_FUNC(0x8B3EBA69, cellNetCtlGetState),
        PS_HLE_FUNC(0x0CE13C6B, cellNetCtlAddHandler),
        PS_HLE_FUNC(0x901815C3, cellNetCtlDelHandler),
        PS_HLE_FUNC(0x1E585B5D, cellNetCtlGetInfo),
        PS_HLE_FUNC(0x3A12865F, cellNetCtlGetNatInfo),
        PS_HLE_FUNC(0x04459230, cellNetCtlNetStartDialogLoadAsync),
        PS_HLE_FUNC(0x0F1F13D3, cellNetCtlNetStartDialogUnloadAsync),
        PS_HLE_FUNC(0x71D53210, cellNetCtlNetStartDialogAbortAsync),
        PS_HLE_FUNC(0xD0A5D727, cellGameUpdateCheckStartAsync),
        PS_HLE_FUNC(0x0791015F, cellGameUpdateCheckStartAsyncEx),
        PS_HLE_FUNC(0xA5E1FA60, cellGameUpdateCheckStartWithoutDialogAsync),
        PS_HLE_FUNC(0x558700F6, cellGameUpdateCheckStartWithoutDialogAsyncEx),
        PS_HLE_FUNC(0xFFA3D791, cellGameUpdateCheckFinishAsync),
        PS_HLE_FUNC(0xF463981C, cellGameUpdateCheckFinishAsyncEx),
    });
}

} // namespace pspocket::hle
