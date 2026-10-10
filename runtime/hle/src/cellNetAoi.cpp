// cellNetAoi: the ad hoc link between a PS3 game and PSP peers (a PSP in ad hoc mode, reached through the PS3's
// wireless LAN), pspocket's own as a console with no ad hoc wireless and no PSN (D-635). Firmware-first (D-563): no Sony
// module was at hand to compare with, so where the system software is installed a game gets Sony's module whole.
//
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellNetAoi.cpp registers the nine names only (each a todo answering CELL_OK,
// no arguments), and nothing public describes the library. So the model is the smallest one the names describe, every
// choice recorded in D-635 with what to check against Sony's module on the Mac:
// - Init and Term bracket the library: everything else before Init (or after Term) is NOT_INITIALIZED, a second Init
//   NOT_TERMINATED. Their arguments are not read.
// - The link: Start would bring the ad hoc link up on the wireless LAN. pspocket's network is disconnected (cellNetCtl,
//   D-024) and an iPhone has no ad hoc link to a PSP, so Start answers NOT_CONNECTED and the link is never up: Stop
//   answers NOT_CONNECTED as well, and so do the calls that read what the link sees (the local station's information,
//   a peer's, the PSP's title ID), each after its arguments are checked, writing nothing.
// - Peers: AddPeer and DeletePeer keep the set of peers the game names, by their 6-byte MAC address (the identity of a
//   station in a PSP's ad hoc mode), at most 16 (a PSP ad hoc group's size); the 17th is HANDLER_MAX, adding one again
//   changes nothing, deleting or asking after one not added is ID_NOT_FOUND. Term forgets them.
// - Error codes: the library's own are not known; these are cellNetCtl's (rpcs3/Emu/Cell/Modules/cellNetCtl.h), the
//   network facility's documented codes, each used for its own meaning. A null pointer, or one that is not guest
//   memory, is INVALID_ADDR, before any state but Init's is looked at.
// Each process its own library.

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "cellNetAoi";
constexpr const char* kChan = "cellNetAoi";

// cellNetCtl's codes (RPCS3 cellNetCtl.h), the closest documented ones (D-635).
enum : u32 {
    CELL_NET_CTL_ERROR_NOT_INITIALIZED = 0x80130101,
    CELL_NET_CTL_ERROR_NOT_TERMINATED = 0x80130102,
    CELL_NET_CTL_ERROR_HANDLER_MAX = 0x80130103,
    CELL_NET_CTL_ERROR_ID_NOT_FOUND = 0x80130104,
    CELL_NET_CTL_ERROR_INVALID_ADDR = 0x80130107,
    CELL_NET_CTL_ERROR_NOT_CONNECTED = 0x80130108,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kMacSize = 6;   // a station's MAC address (a PSP's SceNetEtherAddr)
constexpr usize kMaxPeers = 16; // a PSP ad hoc group

using Mac = std::array<u8, kMacSize>;

struct State {
    std::mutex mutex;
    bool initialized = false;
    std::vector<Mac> peers;            // the peers the game named, in the order it added them
    std::set<std::string_view> logged; // the calls whose refusal was logged once
};

State& state() { return HleProcess::current()->state<State>(); }

// A pointer the call would read or write: not null, and guest memory (`size` bytes; 1 where the size is not known).
bool usable(PPUContext& ctx, u32 addr, u32 size) { return addr && guest_readable(thread_of(ctx), addr, size); }

s32 invalid_addr(PPUContext& ctx, std::string_view call, u32 addr)
{
    log_call_site(ctx, guest_base(ctx), std::format("{}: {:#x} is null or not guest memory", call, addr));
    return error(CELL_NET_CTL_ERROR_INVALID_ADDR);
}

// What the link would answer, logged once a call: there is none here.
s32 not_connected(State& s, std::string_view call)
{
    if (s.logged.insert(call).second)
        PS_LOG_INFO(kChan, "{}: no ad hoc link here (no ad hoc wireless, the network disconnected) -> NOT_CONNECTED",
            call);
    return error(CELL_NET_CTL_ERROR_NOT_CONNECTED);
}

Mac read_mac(PPUContext& ctx, u32 addr)
{
    Mac m{};
    std::memcpy(m.data(), guest_base(ctx) + addr, kMacSize);
    return m;
}

// int cellNetAoiInit(...): its arguments are not read (D-635).
s32 cellNetAoiInit(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(CELL_NET_CTL_ERROR_NOT_TERMINATED);
    s.initialized = true;
    PS_LOG_INFO(kChan, "cellNetAoiInit: up; the ad hoc link never comes up here (no ad hoc wireless)");
    return lv2::CELL_OK;
}

// int cellNetAoiTerm(void): forgets the peers.
s32 cellNetAoiTerm(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    s.initialized = false;
    s.peers.clear();
    return lv2::CELL_OK;
}

// int cellNetAoiStart(...): the ad hoc link on the wireless LAN, which cannot come up here. Its arguments are not read.
s32 cellNetAoiStart(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    return not_connected(s, "cellNetAoiStart");
}

// int cellNetAoiStop(void): no link was started.
s32 cellNetAoiStop(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    return not_connected(s, "cellNetAoiStop");
}

// int cellNetAoiAddPeer(const u8 mac[6])
s32 cellNetAoiAddPeer(PPUContext& ctx, u32 peer)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!usable(ctx, peer, kMacSize)) return invalid_addr(ctx, "cellNetAoiAddPeer", peer);
    const Mac mac = read_mac(ctx, peer);
    if (std::find(s.peers.begin(), s.peers.end(), mac) != s.peers.end()) return lv2::CELL_OK;
    if (s.peers.size() >= kMaxPeers) {
        PS_LOG_WARN(kChan, "cellNetAoiAddPeer: {} peers kept already -> HANDLER_MAX", kMaxPeers);
        return error(CELL_NET_CTL_ERROR_HANDLER_MAX);
    }
    s.peers.push_back(mac);
    PS_LOG_INFO(kChan, "cellNetAoiAddPeer({:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}): {} peer(s)", mac[0], mac[1],
        mac[2], mac[3], mac[4], mac[5], s.peers.size());
    return lv2::CELL_OK;
}

// int cellNetAoiDeletePeer(const u8 mac[6])
s32 cellNetAoiDeletePeer(PPUContext& ctx, u32 peer)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!usable(ctx, peer, kMacSize)) return invalid_addr(ctx, "cellNetAoiDeletePeer", peer);
    const auto it = std::find(s.peers.begin(), s.peers.end(), read_mac(ctx, peer));
    if (it == s.peers.end()) return error(CELL_NET_CTL_ERROR_ID_NOT_FOUND);
    s.peers.erase(it);
    return lv2::CELL_OK;
}

// int cellNetAoiGetLocalInfo(info *): what the link knows of this station; none is up.
s32 cellNetAoiGetLocalInfo(PPUContext& ctx, u32 info)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!usable(ctx, info, 1)) return invalid_addr(ctx, "cellNetAoiGetLocalInfo", info);
    return not_connected(s, "cellNetAoiGetLocalInfo");
}

// int cellNetAoiGetRemotePeerInfo(const u8 mac[6], info *): a peer the game added; the link never saw it.
s32 cellNetAoiGetRemotePeerInfo(PPUContext& ctx, u32 peer, u32 info)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!usable(ctx, peer, kMacSize)) return invalid_addr(ctx, "cellNetAoiGetRemotePeerInfo", peer);
    if (!usable(ctx, info, 1)) return invalid_addr(ctx, "cellNetAoiGetRemotePeerInfo", info);
    if (std::find(s.peers.begin(), s.peers.end(), read_mac(ctx, peer)) == s.peers.end())
        return error(CELL_NET_CTL_ERROR_ID_NOT_FOUND);
    return not_connected(s, "cellNetAoiGetRemotePeerInfo");
}

// int cellNetAoiGetPspTitleId(char *titleId): the title ID of the game the PSP on the link plays; no PSP is there.
s32 cellNetAoiGetPspTitleId(PPUContext& ctx, u32 title_id)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    if (!usable(ctx, title_id, 1)) return invalid_addr(ctx, "cellNetAoiGetPspTitleId", title_id);
    return not_connected(s, "cellNetAoiGetPspTitleId");
}

} // namespace

void register_cellNetAoi(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x480886AF, cellNetAoiInit),
        PS_HLE_FUNC(0x86F19F82, cellNetAoiTerm),
        PS_HLE_FUNC(0xBDACA3AF, cellNetAoiStart),
        PS_HLE_FUNC(0x8AA68668, cellNetAoiStop),
        PS_HLE_FUNC(0xDC3AA14B, cellNetAoiAddPeer),
        PS_HLE_FUNC(0x222FD339, cellNetAoiDeletePeer),
        PS_HLE_FUNC(0xC9151906, cellNetAoiGetLocalInfo),
        PS_HLE_FUNC(0xB4335A81, cellNetAoiGetRemotePeerInfo),
        PS_HLE_FUNC(0x62FEC3B7, cellNetAoiGetPspTitleId),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
