// sceNpTrophy: trophies. Implemented: init and term, and the life cycle of contexts and handles, which needs no files.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNpTrophy.cpp/.h (sce_np_trophy_manager, trophy_context_t,
// trophy_handle_t) and the ID manager they use (rpcs3/Emu/IdManager.h/.cpp).
//
// A context names the game's trophy set: its communication ID ("NPWR01234", number 0) gives the set's directory,
// "NPWR01234_00", under the disc's PS3_GAME/TROPDIR and the user's /dev_hdd0/home/<user>/trophy. Creating one checks
// the communication signature's header and opens no file. Not implemented yet, for when the game's log shows them:
// sceNpTrophyRegisterContext (installs TROPHY.TRP from the disc into the user's trophy directory and calls the game's
// status callback through sysutil), and what reads or writes the installed set: GetRequiredDiskSpace, GetGameInfo,
// GetTrophyInfo, UnlockTrophy.
//
// Contexts and handles get IDs as RPCS3's idm hands them out for id_base 1, id_step 1, id_count 4 (IdTable below).
// sceNpTrophyDestroyHandle and sceNpTrophyAbortHandle don't need the library initialized (RPCS3: unknown whether a
// PS3 checks).
//
// Differences from RPCS3: pointers are checked before use (D-042): a context, communication ID, signature or handle
// pointer that is not guest memory is logged with the call site and refused with SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT
// before anything is created (RPCS3 faults, after creating the context or handle). RPCS3 also hands a writable
// context's name to its trophy list overlay; there is no overlay here.

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNpTrophy";

enum : u32 {
    SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED = 0x80022901,
    SCE_NP_TROPHY_ERROR_NOT_INITIALIZED = 0x80022902,
    SCE_NP_TROPHY_ERROR_NOT_SUPPORTED = 0x80022903,
    SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT = 0x80022906,
    SCE_NP_TROPHY_ERROR_EXCEEDS_MAX = 0x80022907,
    SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT = 0x8002290a,
    SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE = 0x80022911,
    SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID = 0x80022918,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED: return "SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED";
    case SCE_NP_TROPHY_ERROR_NOT_INITIALIZED: return "SCE_NP_TROPHY_ERROR_NOT_INITIALIZED";
    case SCE_NP_TROPHY_ERROR_NOT_SUPPORTED: return "SCE_NP_TROPHY_ERROR_NOT_SUPPORTED";
    case SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT: return "SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT";
    case SCE_NP_TROPHY_ERROR_EXCEEDS_MAX: return "SCE_NP_TROPHY_ERROR_EXCEEDS_MAX";
    case SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT: return "SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT";
    case SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE: return "SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE";
    case SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID: return "SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

// "name(arguments): result". Games make these calls a few times at startup, so every one is logged at info.
template <class... A>
s32 logged(s32 result, std::format_string<A...> fmt, A&&... args)
{
    PS_LOG_INFO(kChan, "{}: {}", std::format(fmt, std::forward<A>(args)...), result_name(result));
    return result;
}

constexpr u64 SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY = 1;
constexpr u32 NP_TROPHY_COMM_SIGN_MAGIC = 0xB9DDE13B;
constexpr u16 kCommSignVersion = 0x100; // 1.00, the only version accepted
// SceNpCommunicationId (12 bytes): char data[9], term; u8 num; char dummy.
constexpr u32 kCommIdSize = 12, kCommIdNum = 10;
// SceNpCommunicationSignature: u8 data[160]. Its header: be32 magic, be16 version, six zero bytes.
constexpr u32 kCommSignSize = 160;

// The IDs of one kind of object, as RPCS3's idm hands them out (IdManager.cpp idm::allocate_id): Base, Base + 1, ...
// in order until each has been used once, then the lowest free one; 0, idm's invalid ID for a base of 1, when all are
// in use. Removing an ID or clearing the table never rewinds the order (idm keeps its highest index). The same table
// as sceNp.cpp's.
template <class T, u32 Base, u32 Count>
class IdTable {
public:
    u32 make(T value)
    {
        u32 i = next_;
        if (next_ < Count) next_++;
        else i = static_cast<u32>(std::find(slots_.begin(), slots_.end(), std::nullopt) - slots_.begin());
        if (i == Count) return 0;
        slots_[i] = std::move(value);
        return Base + i;
    }

    T* find(u32 id)
    {
        const u32 i = id - Base; // an ID below Base wraps past Count, as in idm's get_index
        return i < Count && slots_[i] ? &*slots_[i] : nullptr;
    }

    bool remove(u32 id)
    {
        if (!find(id)) return false;
        slots_[id - Base].reset();
        return true;
    }

    void clear()
    {
        for (std::optional<T>& s : slots_) s.reset();
    }

private:
    std::array<std::optional<T>, Count> slots_{};
    u32 next_ = 0; // idm's highest_index
};

constexpr u32 kIdBase = 1, kIdCount = 4; // trophy_context_t and trophy_handle_t: id_base 1, id_step 1, id_count 4

// An ID outside 1..4 is an invalid argument, whether or not it was handed out.
bool in_range(u32 id) { return id - kIdBase < kIdCount; }

// RPCS3 trophy_context_t, without what sceNpTrophyRegisterContext adds (the installed set's TROPUSR.DAT).
struct TrophyContext {
    std::string trp_name;                  // "NPWR01234_00": the trophy set's directory
    std::array<u8, kCommIdSize> comm_id{}; // RPCS3 keeps it for its RPCN trophy sync
    bool read_only = false;                // SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY
};

// RPCS3 trophy_handle_t: once aborted, a handle refuses the calls that take it (SCE_NP_TROPHY_ERROR_ABORT in RPCS3's
// get_context_ex; none of them is implemented yet).
struct TrophyHandle {
    bool aborted = false;
};

// RPCS3 sce_np_trophy_manager and the two idm tables (per process, as g_fxo and idm are per emulation).
struct TrophyState {
    std::mutex mutex;
    bool initialized = false;
    IdTable<TrophyContext, kIdBase, kIdCount> contexts;
    IdTable<TrophyHandle, kIdBase, kIdCount> handles;
};

TrophyState& trophy()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNpTrophy called without an HLE process");
    return p->state<TrophyState>();
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// ------------------------------------------------------------------------------------------------- init and term

// sceNpTrophyInit(pool, poolSize, containerId, u64 options): the pool and container are not used (RPCS3); no option
// exists.
s32 sceNpTrophyInit(PPUContext&, u32 pool, u32 pool_size, u32 container_id, u64 options)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) {
        return logged(r, "sceNpTrophyInit(pool {:#x}, poolSize {:#x}, containerId {:#x}, options {:#x})", pool,
            pool_size, container_id, options);
    };
    if (s.initialized) return done(error(SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED));
    if (options > 0) return done(error(SCE_NP_TROPHY_ERROR_NOT_SUPPORTED));
    s.initialized = true;
    return done(lv2::CELL_OK);
}

// Every context and handle goes (RPCS3 idm::clear); their IDs go on from where they were.
s32 sceNpTrophyTerm(PPUContext&)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return logged(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED), "sceNpTrophyTerm()");
    s.contexts.clear();
    s.handles.clear();
    s.initialized = false;
    return logged(lv2::CELL_OK, "sceNpTrophyTerm()");
}

// ----------------------------------------------------------------------------------------------------- contexts

// sceNpTrophyCreateContext(u32* context, const SceNpCommunicationId* commId, const SceNpCommunicationSignature*
// commSign, u64 options), with RPCS3's order of checks: the signature pointer before the library's state, the ID's
// number (at most 99), then the signature's header (magic, six zero bytes, version 1.00).
s32 sceNpTrophyCreateContext(PPUContext& ctx, u32 context, u32 comm_id, u32 comm_sign, u64 options)
{
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyCreateContext(context {:#x}, commId {:#x}, commSign {:#x}, options {:#x}){}",
            context, comm_id, comm_sign, options, what);
    };
    if (!comm_sign) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    if (!context || !comm_id) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (options > SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY) return done(error(SCE_NP_TROPHY_ERROR_NOT_SUPPORTED));
    if (!usable(ctx, comm_id, kCommIdSize, "sceNpTrophyCreateContext: commId") ||
        !usable(ctx, comm_sign, kCommSignSize, "sceNpTrophyCreateContext: commSign") ||
        !usable(ctx, context, 4, "sceNpTrophyCreateContext: context")) {
        return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    }
    u8* base = guest_base(ctx);
    TrophyContext c;
    std::memcpy(c.comm_id.data(), base + comm_id, kCommIdSize);
    const u32 num = c.comm_id[kCommIdNum];
    if (num > 99) return done(error(SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID), std::format(": number {}", num));
    // The name: the ID's nine characters, or those before a NUL (the terminator byte isn't checked).
    std::string name(reinterpret_cast<const char*>(c.comm_id.data()), 9);
    if (const usize nul = name.find('\0'); nul != std::string::npos) name.resize(nul);
    std::array<u8, kCommSignSize> sign{};
    std::memcpy(sign.data(), base + comm_sign, kCommSignSize);
    if (ps_ld32(sign.data()) != NP_TROPHY_COMM_SIGN_MAGIC ||
        std::any_of(sign.begin() + 6, sign.begin() + 12, [](u8 b) { return b != 0; }) ||
        ps_ld16(sign.data() + 4) != kCommSignVersion) {
        const std::string header = std::format("{:08x} {:04x}", ps_ld32(sign.data()), ps_ld16(sign.data() + 4));
        return done(error(SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID), std::format(": \"{}\", signature {}", name, header));
    }
    c.trp_name = std::format("{}_{:02}", name, num);
    c.read_only = (options & SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY) != 0;
    const std::string trp_name = c.trp_name;
    const bool read_only = c.read_only;
    const u32 id = s.contexts.make(std::move(c));
    if (!id) {
        return done(error(SCE_NP_TROPHY_ERROR_EXCEEDS_MAX), std::format(": \"{}\", {} contexts", trp_name, kIdCount));
    }
    ps_write32(base, context, id);
    return done(lv2::CELL_OK, std::format(": context {} for trophy set \"{}\"{}", id, trp_name,
        read_only ? " (read only)" : ""));
}

s32 sceNpTrophyDestroyContext(PPUContext&, u32 context)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, "sceNpTrophyDestroyContext({})", context); };
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    if (!in_range(context)) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (!s.contexts.remove(context)) return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT));
    return done(lv2::CELL_OK);
}

// ------------------------------------------------------------------------------------------------------ handles

// sceNpTrophyCreateHandle(u32* handle).
s32 sceNpTrophyCreateHandle(PPUContext& ctx, u32 handle)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r, std::string_view what = {}) {
        return logged(r, "sceNpTrophyCreateHandle({:#x}){}", handle, what);
    };
    if (!s.initialized) return done(error(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED));
    if (!handle) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (!usable(ctx, handle, 4, "sceNpTrophyCreateHandle: handle")) return error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    const u32 id = s.handles.make({});
    if (!id) return done(error(SCE_NP_TROPHY_ERROR_EXCEEDS_MAX), std::format(": {} handles", kIdCount));
    ps_write32(guest_base(ctx), handle, id);
    return done(lv2::CELL_OK, std::format(": handle {}", id));
}

s32 sceNpTrophyDestroyHandle(PPUContext&, u32 handle)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, "sceNpTrophyDestroyHandle({})", handle); };
    if (!in_range(handle)) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    if (!s.handles.remove(handle)) return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE));
    return done(lv2::CELL_OK);
}

// Marks the handle aborted (RPCS3, a TODO: nothing that runs with it is interrupted); it stays until destroyed.
s32 sceNpTrophyAbortHandle(PPUContext&, u32 handle)
{
    TrophyState& s = trophy();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, "sceNpTrophyAbortHandle({})", handle); };
    if (!in_range(handle)) return done(error(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT));
    TrophyHandle* h = s.handles.find(handle);
    if (!h) return done(error(SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE));
    h->aborted = true;
    return done(lv2::CELL_OK);
}

} // namespace

void register_sceNpTrophy(Registry& r)
{
    r.add("sceNpTrophy", {
        PS_HLE_FUNC(0x39567781, sceNpTrophyInit),
        PS_HLE_FUNC(0xA7FABF4D, sceNpTrophyTerm),
        PS_HLE_FUNC(0xE3BF9A28, sceNpTrophyCreateContext),
        PS_HLE_FUNC(0x3741ECC7, sceNpTrophyDestroyContext),
        PS_HLE_FUNC(0x1C25470D, sceNpTrophyCreateHandle),
        PS_HLE_FUNC(0x623CD2DC, sceNpTrophyDestroyHandle),
        PS_HLE_FUNC(0x48BD97C7, sceNpTrophyAbortHandle),
    });
}

} // namespace pspocket::hle
