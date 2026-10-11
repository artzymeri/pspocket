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
//
// pspocket's own where RPCS3 has todos with no arguments (D-670), unverified (D-594; libnetctl is always native, so the
// marker only records that no Sony module was compared): the game-internal forms (cellNetCtl*GameInt) answer as the
// public forms do for this offline network. GetState, GetInfo, AddHandler and DelHandler take the public forms'
// arguments; their interface is disconnected, its handlers kept (four, IDs 0 to 3, written to *hid as the SDK's
// AddHandler does) and never called. Connect answers NOT_CONNECTED (no link comes up), Disconnect CELL_OK (it is not
// connected), Scan CELL_OK with nothing found and GetScanInfo CELL_OK with nothing written: their arguments are not
// known, so they are logged, not read. The game update's Init and Term bracket it (ALREADY_INITIALIZED,
// NOT_INITIALIZED), and CheckAbort aborts a check started and not yet answered (its callback then hears ABORTED),
// NOT_START with none.

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <set>
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
    CELL_NET_CTL_ERROR_HANDLER_MAX = 0x80130103,
    CELL_NET_CTL_ERROR_ID_NOT_FOUND = 0x80130104,
    CELL_NET_CTL_ERROR_INVALID_ID = 0x80130105,
    CELL_NET_CTL_ERROR_INVALID_ADDR = 0x80130107,
    CELL_NET_CTL_ERROR_NOT_CONNECTED = 0x80130108,
    CELL_NET_CTL_ERROR_INVALID_TYPE = 0x8013010a,
    CELL_NET_CTL_ERROR_INVALID_SIZE = 0x8013010b,
    CELL_NET_CTL_ERROR_DIALOG_CANCELED = 0x80130190,

    CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED = 0x8002cc01,
    CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED = 0x8002cc02,
    CELL_GAMEUPDATE_ERROR_NOT_START = 0x8002cc08,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }
constexpr s32 CELL_NET_CTL_STATE_Disconnected = 0;

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case CELL_NET_CTL_ERROR_NOT_INITIALIZED: return "CELL_NET_CTL_ERROR_NOT_INITIALIZED";
    case CELL_NET_CTL_ERROR_HANDLER_MAX: return "CELL_NET_CTL_ERROR_HANDLER_MAX";
    case CELL_NET_CTL_ERROR_ID_NOT_FOUND: return "CELL_NET_CTL_ERROR_ID_NOT_FOUND";
    case CELL_NET_CTL_ERROR_INVALID_ID: return "CELL_NET_CTL_ERROR_INVALID_ID";
    case CELL_NET_CTL_ERROR_INVALID_ADDR: return "CELL_NET_CTL_ERROR_INVALID_ADDR";
    case CELL_NET_CTL_ERROR_NOT_CONNECTED: return "CELL_NET_CTL_ERROR_NOT_CONNECTED";
    case CELL_NET_CTL_ERROR_INVALID_TYPE: return "CELL_NET_CTL_ERROR_INVALID_TYPE";
    case CELL_NET_CTL_ERROR_INVALID_SIZE: return "CELL_NET_CTL_ERROR_INVALID_SIZE";
    case CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED: return "CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED";
    case CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED: return "CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED";
    case CELL_GAMEUPDATE_ERROR_NOT_START: return "CELL_GAMEUPDATE_ERROR_NOT_START";
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

// The game-internal interface's handler slots (D-670): four, IDs 0 to 3, as cellNetCtlDelHandler's check (RPCS3: an
// ID above 3 is INVALID_ID) has the public ones.
constexpr u32 kHandlerSlots = 4;

// The game-internal forms whose calls are logged by count (D-670), each's first kInfoCalls at info.
enum class GameInt : u8 { AddHandler, DelHandler, Connect, Disconnect, GetInfo, GetScanInfo, GetState, Scan, Count };
constexpr u32 kInfoCalls = 4;

struct NetCtlState {
    std::atomic<bool> initialized{false};
    // cellNetCtlGetInfo codes already logged at info (RPCS3 logs most codes once).
    std::array<std::atomic<bool>, kInfoNames.size()> info_logged{};
    std::array<std::atomic<u32>, static_cast<usize>(GameInt::Count)> gameint_calls{};
    // The game-internal interface's handlers: {handler, arg} where `used`. Never called: the interface stays
    // disconnected. cellNetCtlTerm forgets them.
    struct Handler {
        bool used = false;
        u32 handler = 0, arg = 0;
    };
    std::mutex mutex;
    std::array<Handler, kHandlerSlots> gameint_handlers{};
};

NetCtlState& netctl() { return HleProcess::current()->state<NetCtlState>(); }

// A game-internal form's log line: its first calls at info (a warning for an error), later ones at debug.
s32 gameint_logged(GameInt fn, s32 r, std::string_view call)
{
    const u32 n = netctl().gameint_calls[static_cast<usize>(fn)].fetch_add(1, std::memory_order_relaxed);
    const log::Level level = n >= kInfoCalls ? log::Level::Debug : r < 0 ? log::Level::Warn : log::Level::Info;
    log::print(level, kChan, "{}: {}", call, result_name(r));
    return r;
}

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

// The game-internal interface's handlers go with the library (D-670).
void cellNetCtlTerm(PPUContext&)
{
    NetCtlState& s = netctl();
    s.initialized = false;
    std::lock_guard lock(s.mutex);
    s.gameint_handlers = {};
}

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
// logged at info, later ones at debug. The game-internal form (cellNetCtlGetInfoGameInt, D-670) answers the same.
s32 get_info(PPUContext& ctx, std::string_view name, s32 code, u32 info)
{
    NetCtlState& s = netctl();
    const bool known = code >= 0 && static_cast<usize>(code) < kInfoNames.size();
    const bool first = !known || !s.info_logged[static_cast<usize>(code)].exchange(true);
    const auto done = [&](s32 r) {
        log::print(first ? log::Level::Info : log::Level::Debug, kChan, "{}({:#x} {}, info {:#x}): {}", name, code,
            known ? kInfoNames[static_cast<usize>(code)] : "?", info, result_name(r));
        return r;
    };
    if (!s.initialized) return done(error(CELL_NET_CTL_ERROR_NOT_INITIALIZED));
    if (!info) return done(error(CELL_NET_CTL_ERROR_INVALID_ADDR));
    if (code != CELL_NET_CTL_INFO_ETHER_ADDR) return done(error(CELL_NET_CTL_ERROR_NOT_CONNECTED));
    constexpr u32 kEtherAddrSize = static_cast<u32>(kEtherAddr.size());
    if (!usable(ctx, info, kEtherAddrSize, std::format("{}: info", name)))
        return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
    u8* base = guest_base(ctx);
    for (u32 i = 0; i < kEtherAddrSize; i++) ps_write8(base, info + i, kEtherAddr[i]);
    return done(lv2::CELL_OK);
}

s32 cellNetCtlGetInfo(PPUContext& ctx, s32 code, u32 info) { return get_info(ctx, "cellNetCtlGetInfo", code, info); }

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

// ---- the game-internal forms (D-670): the public forms' answers for this offline network

// cellNetCtlGetStateGameInt(s32* state): Disconnected, as cellNetCtlGetState.
s32 cellNetCtlGetStateGameInt(PPUContext& ctx, u32 state)
{
    const std::string call = std::format("cellNetCtlGetStateGameInt({:#x})", state);
    if (!netctl().initialized)
        return gameint_logged(GameInt::GetState, error(CELL_NET_CTL_ERROR_NOT_INITIALIZED), call);
    if (!state || !usable(ctx, state, 4, "cellNetCtlGetStateGameInt: state"))
        return gameint_logged(GameInt::GetState, error(CELL_NET_CTL_ERROR_INVALID_ADDR), call);
    ps_write32(guest_base(ctx), state, static_cast<u32>(CELL_NET_CTL_STATE_Disconnected));
    return gameint_logged(GameInt::GetState, lv2::CELL_OK, call + ": Disconnected");
}

s32 cellNetCtlGetInfoGameInt(PPUContext& ctx, s32 code, u32 info)
{
    return get_info(ctx, "cellNetCtlGetInfoGameInt", code, info);
}

// cellNetCtlAddHandlerGameInt(handler, arg, s32* hid): the lowest free of four slots, its ID written; HANDLER_MAX when
// all four are taken. The handler is kept and never called: the interface never changes state.
s32 cellNetCtlAddHandlerGameInt(PPUContext& ctx, u32 handler, u32 arg, u32 hid)
{
    const std::string call =
        std::format("cellNetCtlAddHandlerGameInt(handler {:#x}, arg {:#x}, hid {:#x})", handler, arg, hid);
    NetCtlState& s = netctl();
    if (!s.initialized) return gameint_logged(GameInt::AddHandler, error(CELL_NET_CTL_ERROR_NOT_INITIALIZED), call);
    if (!hid || !usable(ctx, hid, 4, "cellNetCtlAddHandlerGameInt: hid"))
        return gameint_logged(GameInt::AddHandler, error(CELL_NET_CTL_ERROR_INVALID_ADDR), call);
    std::lock_guard lock(s.mutex);
    for (u32 id = 0; id < kHandlerSlots; id++) {
        NetCtlState::Handler& h = s.gameint_handlers[id];
        if (h.used) continue;
        h = {true, handler, arg};
        ps_write32(guest_base(ctx), hid, id);
        return gameint_logged(GameInt::AddHandler, lv2::CELL_OK,
            call + std::format(": handler {} (never called: the interface stays disconnected)", id));
    }
    return gameint_logged(GameInt::AddHandler, error(CELL_NET_CTL_ERROR_HANDLER_MAX), call);
}

// cellNetCtlDelHandlerGameInt(hid): an ID outside 0 to 3 is INVALID_ID (the public form's check, which a negative ID
// fails here too), a free slot ID_NOT_FOUND.
s32 cellNetCtlDelHandlerGameInt(PPUContext&, s32 hid)
{
    const std::string call = std::format("cellNetCtlDelHandlerGameInt({})", hid);
    NetCtlState& s = netctl();
    if (!s.initialized) return gameint_logged(GameInt::DelHandler, error(CELL_NET_CTL_ERROR_NOT_INITIALIZED), call);
    if (hid < 0 || hid >= static_cast<s32>(kHandlerSlots))
        return gameint_logged(GameInt::DelHandler, error(CELL_NET_CTL_ERROR_INVALID_ID), call);
    std::lock_guard lock(s.mutex);
    NetCtlState::Handler& h = s.gameint_handlers[static_cast<usize>(hid)];
    if (!h.used) return gameint_logged(GameInt::DelHandler, error(CELL_NET_CTL_ERROR_ID_NOT_FOUND), call);
    h = {};
    return gameint_logged(GameInt::DelHandler, lv2::CELL_OK, call);
}

// The calls whose arguments are not known: logged as the registers the game passed, not read.
std::string registers(std::string_view name, const PPUContext& ctx)
{
    return std::format("{}(r3 {:#x}, r4 {:#x}, r5 {:#x})", name, ctx.gpr[3], ctx.gpr[4], ctx.gpr[5]);
}

// No link comes up: the network is disconnected (D-024), as cellNetAoiStart answers (D-635).
s32 cellNetCtlConnectGameInt(PPUContext& ctx)
{
    const std::string call = registers("cellNetCtlConnectGameInt", ctx);
    if (!netctl().initialized) return gameint_logged(GameInt::Connect, error(CELL_NET_CTL_ERROR_NOT_INITIALIZED), call);
    return gameint_logged(GameInt::Connect, error(CELL_NET_CTL_ERROR_NOT_CONNECTED),
        call + ": no network to connect to");
}

// Not connected, so there is nothing to disconnect: the interface is as the call asks.
s32 cellNetCtlDisconnectGameInt(PPUContext& ctx)
{
    const std::string call = registers("cellNetCtlDisconnectGameInt", ctx);
    if (!netctl().initialized)
        return gameint_logged(GameInt::Disconnect, error(CELL_NET_CTL_ERROR_NOT_INITIALIZED), call);
    return gameint_logged(GameInt::Disconnect, lv2::CELL_OK, call + ": not connected");
}

// A scan for networks finds none at once (no wireless LAN); its results are none, and nothing is written.
s32 cellNetCtlScanGameInt(PPUContext& ctx)
{
    const std::string call = registers("cellNetCtlScanGameInt", ctx);
    if (!netctl().initialized) return gameint_logged(GameInt::Scan, error(CELL_NET_CTL_ERROR_NOT_INITIALIZED), call);
    return gameint_logged(GameInt::Scan, lv2::CELL_OK, call + ": no networks");
}

s32 cellNetCtlGetScanInfoGameInt(PPUContext& ctx)
{
    const std::string call = registers("cellNetCtlGetScanInfoGameInt", ctx);
    if (!netctl().initialized)
        return gameint_logged(GameInt::GetScanInfo, error(CELL_NET_CTL_ERROR_NOT_INITIALIZED), call);
    return gameint_logged(GameInt::GetScanInfo, lv2::CELL_OK, call + ": no networks, nothing written");
}

// ---- game update checks (RPCS3: they need no cellGameUpdateInit and check nothing)
//
// D-670: cellGameUpdateInit and Term bracket the utility. A check (StartAsync, StartWithoutDialogAsync and their Ex
// forms) is in progress from its start until its callback is called; cellGameUpdateCheckAbort aborts the checks in
// progress, whose callbacks then hear ABORTED rather than NO_UPDATE. The checks themselves still need no Init (RPCS3's,
// which games run on today).

constexpr s32 CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE = 0, CELL_GAMEUPDATE_RESULT_STATUS_FINISHED = 5,
              CELL_GAMEUPDATE_RESULT_STATUS_ABORTED = 6;
constexpr u32 kGameUpdateResultSize = 16; // CellGameUpdateResult: be s32 status, error_code; char app_ver[6]; pad[2]

struct GameUpdateState {
    std::mutex mutex;
    bool initialized = false;
    u64 next_check = 1;
    std::set<u64> running; // checks started whose callbacks have not been called
    std::set<u64> aborted; // those of them cellGameUpdateCheckAbort aborted
};

GameUpdateState& game_update_state() { return HleProcess::current()->state<GameUpdateState>(); }

// The callback, from the next cellSysutilCheckCallback: (status, error 0, userdata), or for the Ex functions
// (CellGameUpdateResult*, userdata). A start's status is NO_UPDATE (ABORTED once aborted), a finish's FINISHED.
s32 game_update(const char* fn, u32 func, u32 userdata, bool start, bool ex)
{
    const s32 status = start ? CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE : CELL_GAMEUPDATE_RESULT_STATUS_FINISHED;
    PS_LOG_INFO(kChan, "{}(func {:#x}, userdata {:#x}): no update server; status {}", fn, func, userdata, status);
    if (!func) return lv2::CELL_OK; // RPCS3 would call address 0
    u64 check = 0;
    if (start) {
        GameUpdateState& u = game_update_state();
        std::lock_guard lock(u.mutex);
        check = u.next_check++;
        u.running.insert(check);
    }
    sysutil_queue_call([=](PPUContext& ctx) -> s32 {
        s32 answer = status;
        if (check) {
            GameUpdateState& u = game_update_state();
            std::lock_guard lock(u.mutex);
            u.running.erase(check);
            if (u.aborted.erase(check)) answer = CELL_GAMEUPDATE_RESULT_STATUS_ABORTED;
        }
        if (!ex) {
            call_guest(ctx, func, {static_cast<u64>(answer), 0, userdata});
            return lv2::CELL_OK;
        }
        u8* base = guest_base(ctx);
        StackVar result(ctx, base, kGameUpdateResultSize);
        ps_write32(base, result.addr(), static_cast<u32>(answer));
        call_guest(ctx, func, {result.addr(), userdata});
        return lv2::CELL_OK;
    });
    return lv2::CELL_OK;
}

s32 cellGameUpdateCheckStartAsync(PPUContext&, u32, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckStartAsync", func, userdata, true, false);
}

s32 cellGameUpdateCheckStartAsyncEx(PPUContext&, u32, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckStartAsyncEx", func, userdata, true, true);
}

s32 cellGameUpdateCheckStartWithoutDialogAsync(PPUContext&, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckStartWithoutDialogAsync", func, userdata, true, false);
}

s32 cellGameUpdateCheckStartWithoutDialogAsyncEx(PPUContext&, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckStartWithoutDialogAsyncEx", func, userdata, true, true);
}

s32 cellGameUpdateCheckFinishAsync(PPUContext&, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckFinishAsync", func, userdata, false, false);
}

s32 cellGameUpdateCheckFinishAsyncEx(PPUContext&, u32 func, u32 userdata)
{
    return game_update("cellGameUpdateCheckFinishAsyncEx", func, userdata, false, true);
}

s32 game_update_logged(s32 r, std::string_view call)
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, kChan, "{}: {}", call, result_name(r));
    return r;
}

// cellGameUpdateInit(): once until cellGameUpdateTerm (ALREADY_INITIALIZED again before it). Its arguments, if any,
// are not read.
s32 cellGameUpdateInit(PPUContext&)
{
    GameUpdateState& u = game_update_state();
    std::lock_guard lock(u.mutex);
    if (u.initialized)
        return game_update_logged(error(CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED), "cellGameUpdateInit()");
    u.initialized = true;
    return game_update_logged(lv2::CELL_OK, "cellGameUpdateInit()");
}

s32 cellGameUpdateTerm(PPUContext&)
{
    GameUpdateState& u = game_update_state();
    std::lock_guard lock(u.mutex);
    if (!u.initialized) return game_update_logged(error(CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED), "cellGameUpdateTerm()");
    u.initialized = false;
    return game_update_logged(lv2::CELL_OK, "cellGameUpdateTerm()");
}

// cellGameUpdateCheckAbort(): the checks in progress hear ABORTED; with none, NOT_START (a check answered already, as
// every check here is at the cellSysutilCheckCallback after its start).
s32 cellGameUpdateCheckAbort(PPUContext&)
{
    GameUpdateState& u = game_update_state();
    std::lock_guard lock(u.mutex);
    if (!u.initialized)
        return game_update_logged(error(CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED), "cellGameUpdateCheckAbort()");
    if (u.running.empty())
        return game_update_logged(error(CELL_GAMEUPDATE_ERROR_NOT_START), "cellGameUpdateCheckAbort()");
    u.aborted.insert(u.running.begin(), u.running.end());
    return game_update_logged(lv2::CELL_OK,
        std::format("cellGameUpdateCheckAbort(): {} check(s) aborted, their callbacks hear ABORTED", u.running.size()));
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
        // D-670: RPCS3's todos, pspocket's own; unverified (D-594).
        PS_HLE_UNVERIFIED(0xB23D5A13, cellNetCtlAddHandlerGameInt),
        PS_HLE_UNVERIFIED(0x21D67EEF, cellNetCtlConnectGameInt),
        PS_HLE_UNVERIFIED(0x793BBF51, cellNetCtlDelHandlerGameInt),
        PS_HLE_UNVERIFIED(0xF08BA0D0, cellNetCtlDisconnectGameInt),
        PS_HLE_UNVERIFIED(0x11F777B7, cellNetCtlGetInfoGameInt),
        PS_HLE_UNVERIFIED(0xF8C8B9FF, cellNetCtlGetScanInfoGameInt),
        PS_HLE_UNVERIFIED(0x3BFA5B37, cellNetCtlGetStateGameInt),
        PS_HLE_UNVERIFIED(0x71DB688D, cellNetCtlScanGameInt),
        PS_HLE_UNVERIFIED(0x99AB1A26, cellGameUpdateInit),
        PS_HLE_UNVERIFIED(0x10DAE56D, cellGameUpdateTerm),
        PS_HLE_UNVERIFIED(0x3E359AB6, cellGameUpdateCheckAbort),
    });
}

} // namespace pspocket::hle
