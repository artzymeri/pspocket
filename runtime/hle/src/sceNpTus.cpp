// sceNpTus: title user storage, variables and data a game keeps per user on PSN's servers, and the title's small
// storage files (TSS). Reference: RPCS3 rpcs3/Emu/Cell/Modules/sceNpTus.cpp/.h, with its title and transaction
// contexts (rpcs3/Emu/NP/np_contexts.cpp/.h: tus_ctx, tus_transaction_ctx) and the NP state of np_handler.
//
// Offline, as RPCS3 with PSN disabled (D-059): the library initializes on top of NP and title contexts are made and
// kept (IDs 0x7001 on, with their communication ID and timeout), but transactions talk to PSN, so
// sceNpTusCreateTransactionCtx refuses them and no transaction ID is ever valid. Each request makes RPCS3's checks in
// RPCS3's order (its arguments, mostly before the library's state; the target), then RPCS3's online check, which ends
// it with SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID before the transaction is looked up; nothing is written. The calls
// that only take a transaction find none (SCE_NP_COMMUNITY_ERROR_INVALID_ID).
//
// Difference from RPCS3: guest memory the checks read (slot IDs, the size field of an option, a communication ID) is
// checked first (D-042): memory that isn't guest memory is logged with the call site and refused with
// SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT (RPCS3 faults or, for a communication ID, aborts).

#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "idm.hpp"
#include "np.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNpTus";

enum : u32 {
    SCE_NP_ERROR_NOT_INITIALIZED = 0x8002AA01,
    SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED = 0x8002A101,
    SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED = 0x8002A102,
    SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT = 0x8002A104,
    SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS = 0x8002A107,
    SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT = 0x8002A1A1,
    SCE_NP_COMMUNITY_ERROR_INVALID_ID = 0x8002A1A3,
    SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID = 0x8002A1A4,
    SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT = 0x8002A1AC,
    SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID = 0x8002A1AD,
    SCE_NP_COMMUNITY_ERROR_TOO_MANY_SLOTID = 0x8002A1B1,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 SCE_NP_TUS_MAX_CTX_NUM = 32, SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS = 64;
constexpr s32 SCE_NP_SCORE_MAX_NPID_NUM_PER_TRANS = 101, SCE_NP_SCORE_MAX_SELECTED_FRIENDS_NUM = 100;
constexpr s32 SCE_NP_TUS_VARIABLE_SORTTYPE_DESCENDING_DATE = 1, SCE_NP_TUS_VARIABLE_SORTTYPE_ASCENDING_VALUE = 4;
constexpr s32 SCE_NP_TUS_DATASTATUS_SORTTYPE_DESCENDING_DATE = 1, SCE_NP_TUS_DATASTATUS_SORTTYPE_ASCENDING_DATE = 2;
constexpr u64 kTusTimeoutDefault = 60'000'000, kTimeoutMin = 10'000'000; // microseconds
// Structure sizes (RPCS3 sceNpTus.h): SceNpTusVariable 120, SceNpTusDataStatus 488 (its SceNpTusDataInfo 392), the
// option structs of add-and-get and set-data 12, of try-and-set 16. The friends' calls take an undocumented option
// whose first word must be 0xC.
constexpr u64 kVariableSize = 120, kDataStatusSize = 488;
constexpr u32 kDataInfoSize = 392, kAddAndGetOptSize = 12, kTryAndSetOptSize = 16, kSetDataOptSize = 12;
constexpr u32 kFriendsOptMagic = 0xC;

// RPCS3 tus_ctx: a title context's communication ID and the timeout its transactions would start with.
struct TusCtx {
    std::string communication_id;
    u64 timeout = kTusTimeoutDefault;
};

// RPCS3 np_handler's is_NP_TUS_init and the idm table of tus_ctx. Neither NP's term nor the library's clears the
// contexts, as in RPCS3.
struct TusState {
    std::mutex mutex;
    bool initialized = false;
    IdTable<TusCtx, 0x7001, SCE_NP_TUS_MAX_CTX_NUM> title_ctxs;
};

TusState& tus()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNpTus called without an HLE process");
    return p->state<TusState>();
}

bool tus_initialized()
{
    TusState& s = tus();
    std::lock_guard lock(s.mutex);
    return s.initialized;
}

// Whether `size` bytes at `addr` are guest memory; if not, the call site is logged (RPCS3 would fault).
bool usable(PPUContext& ctx, std::string_view name, u32 addr, u32 size)
{
    lv2::PpuThread& t = thread_of(ctx);
    if (guest_readable(t, addr, size)) return true;
    log_call_site(ctx, t.kernel.base(), std::format("{}: {:#x} ({} bytes) is not guest memory", name, addr, size));
    return false;
}

// The end of a request once its checks pass: PSN is needed.
s32 offline(std::string_view name, s32 trans_id)
{
    PS_LOG_DEBUG(kChan, "{}(transaction {:#x}): offline", name, trans_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// RPCS3 is_slot_array_valid: no negative slot ID among the first `count`. Nullopt when the array isn't guest memory.
std::optional<bool> slots_valid(PPUContext& ctx, std::string_view name, u32 slots, s32 count)
{
    if (count <= 0) return true;
    if (!usable(ctx, name, slots, 4 * static_cast<u32>(count))) return std::nullopt;
    for (s32 i = 0; i < count; i++) {
        if (static_cast<s32>(ps_read32(guest_base(ctx), slots + 4 * static_cast<u32>(i))) < 0) return false;
    }
    return true;
}

// RPCS3's size check of an array of `count` structs of `size` bytes (count * sizeof, in size_t arithmetic: a negative
// count never matches).
bool array_size_ok(u32 array_size, s32 count, u64 size)
{
    return u64{array_size} == static_cast<u64>(static_cast<s64>(count)) * size;
}

// Whether an option struct is present with a size field other than `size` (RPCS3 option->size); nullopt when it isn't
// guest memory.
std::optional<bool> option_size_wrong(PPUContext& ctx, std::string_view name, u32 option, u32 size)
{
    if (!option) return false;
    if (!usable(ctx, name, option, 4)) return std::nullopt;
    return ps_read32(guest_base(ctx), option) != size;
}

// --------------------------------------------------------------------------------------------- library, contexts

s32 sceNpTusInit(PPUContext&, s32 prio)
{
    TusState& s = tus();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED);
    if (!np_initialized()) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.initialized = true;
    PS_LOG_INFO(kChan, "sceNpTusInit(prio {}): up; offline every transaction is refused", prio);
    return lv2::CELL_OK;
}

s32 sceNpTusTerm(PPUContext&)
{
    TusState& s = tus();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!np_initialized()) return error(SCE_NP_ERROR_NOT_INITIALIZED);
    s.initialized = false;
    return lv2::CELL_OK;
}

// RPCS3 sceNpTusCreateTitleCtx: the new title context's ID; the passphrase and the user's NP ID are only checked for
// null. A communication ID that isn't valid (RPCS3 aborts in tus_ctx's constructor) is refused with a log.
s32 sceNpTusCreateTitleCtx(PPUContext& ctx, u32 comm_id, u32 passphrase, u32 self_np_id)
{
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!comm_id || !passphrase || !self_np_id) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (!usable(ctx, "sceNpTusCreateTitleCtx", comm_id, 12)) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    const std::optional<std::string> id = np_communication_id(guest_base(ctx), comm_id);
    if (!id) {
        log_call_site(ctx, guest_base(ctx), std::format("sceNpTusCreateTitleCtx: {:#x} is not a valid communication ID "
                                                        "(RPCS3 aborts)", comm_id));
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    TusState& s = tus();
    std::lock_guard lock(s.mutex);
    const u32 title_ctx = s.title_ctxs.make(TusCtx{*id});
    if (!title_ctx) {
        PS_LOG_ERROR(kChan, "sceNpTusCreateTitleCtx({}): all {} title contexts are in use", *id, SCE_NP_TUS_MAX_CTX_NUM);
        return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS);
    }
    PS_LOG_INFO(kChan, "sceNpTusCreateTitleCtx({}): title context {:#x}", *id, title_ctx);
    return static_cast<s32>(title_ctx);
}

s32 sceNpTusDestroyTitleCtx(PPUContext&, s32 title_ctx_id)
{
    TusState& s = tus();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!s.title_ctxs.remove(static_cast<u32>(title_ctx_id))) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    return lv2::CELL_OK;
}

// RPCS3 sceNpTusCreateTransactionCtx: offline refused before the title context is looked at.
s32 sceNpTusCreateTransactionCtx(PPUContext&, s32 title_ctx_id)
{
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_INFO(kChan, "sceNpTusCreateTransactionCtx(title context {:#x}): offline, refused", title_ctx_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ONLINE_ID);
}

// The calls that only take a transaction: there is none.
s32 no_transaction(std::string_view name, s32 trans_id)
{
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    PS_LOG_DEBUG(kChan, "{}({:#x}): no such transaction", name, trans_id);
    return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
}

s32 sceNpTusDestroyTransactionCtx(PPUContext&, s32 trans_id)
{
    return no_transaction("sceNpTusDestroyTransactionCtx", trans_id);
}

s32 sceNpTusAbortTransaction(PPUContext&, s32 trans_id) { return no_transaction("sceNpTusAbortTransaction", trans_id); }

s32 sceNpTusWaitAsync(PPUContext&, s32 trans_id, u32 result)
{
    PS_LOG_DEBUG(kChan, "sceNpTusWaitAsync: result {:#x}", result);
    return no_transaction("sceNpTusWaitAsync", trans_id);
}

s32 sceNpTusPollAsync(PPUContext&, s32 trans_id, u32 result)
{
    PS_LOG_DEBUG(kChan, "sceNpTusPollAsync: result {:#x}", result);
    return no_transaction("sceNpTusPollAsync", trans_id);
}

// RPCS3 sceNpTusSetTimeout: at least 10 s (a u32 here, unlike the score library's), on a transaction (none offline) or
// a title context.
s32 sceNpTusSetTimeout(PPUContext&, s32 ctx_id, u32 timeout_us)
{
    TusState& s = tus();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (timeout_us < kTimeoutMin) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    TusCtx* c = s.title_ctxs.find(static_cast<u32>(ctx_id));
    if (!c) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    c->timeout = timeout_us;
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------ variables

// RPCS3 scenp_tus_set_multislot_variable: the slots and values, 1 to 64 of them, no negative slot, no option; then the
// library and the target.
s32 set_multislot_variable(PPUContext& ctx, std::string_view name, s32 trans_id, u32 target, u32 slots, u32 values,
    s32 count, u32 option)
{
    if (!slots || !values) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (count == 0 || option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (count > static_cast<s32>(SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_SLOTID);
    const std::optional<bool> valid = slots_valid(ctx, name, slots, count);
    if (!valid || !*valid) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!target) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_get_multislot_variable: as setting, with the output array's size (count * 120) checked after the
// option.
s32 get_multislot_variable(PPUContext& ctx, std::string_view name, s32 trans_id, u32 target, u32 slots, u32 variables,
    u32 variables_size, s32 count, u32 option)
{
    if (!slots || !variables) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (count == 0 || option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!array_size_ok(variables_size, count, kVariableSize)) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (count > static_cast<s32>(SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_SLOTID);
    const std::optional<bool> valid = slots_valid(ctx, name, slots, count);
    if (!valid || !*valid) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!target) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_get_multiuser_variable: the output, 1 to 101 users, no option, the array's size, a slot; then the
// library and the targets.
s32 get_multiuser_variable(std::string_view name, s32 trans_id, u32 targets, s32 slot, u32 variables,
    u32 variables_size, s32 count, u32 option)
{
    if (!variables) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (count == 0 || option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (count > SCE_NP_SCORE_MAX_NPID_NUM_PER_TRANS) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID);
    if (!array_size_ok(variables_size, count, kVariableSize)) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (slot < 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!targets) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return offline(name, trans_id);
}

// The friends' calls: an option, when given, must start with 0xC (RPCS3: "Undocumented behaviour and structure
// unknown"). Nullopt when it isn't guest memory.
std::optional<bool> friends_option_bad(PPUContext& ctx, std::string_view name, u32 option)
{
    if (!option) return false;
    if (!usable(ctx, name, option, 4)) return std::nullopt;
    return ps_read32(guest_base(ctx), option) != kFriendsOptMagic;
}

// RPCS3 scenp_tus_get_friends_variable: the output, a count, the option, a sort type (1 to 4), at most 100 friends,
// the array's size, a slot; then the library.
s32 get_friends_variable(PPUContext& ctx, std::string_view name, s32 trans_id, s32 slot, s32 sort_type, u32 variables,
    u32 variables_size, s32 count, u32 option)
{
    if (!variables) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (count == 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    const std::optional<bool> bad_option = friends_option_bad(ctx, name, option);
    if (!bad_option || *bad_option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (sort_type < SCE_NP_TUS_VARIABLE_SORTTYPE_DESCENDING_DATE || sort_type > SCE_NP_TUS_VARIABLE_SORTTYPE_ASCENDING_VALUE) {
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    if (count > SCE_NP_SCORE_MAX_SELECTED_FRIENDS_NUM) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID);
    if (!array_size_ok(variables_size, count, kVariableSize)) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (slot < 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_add_and_get_variable: the library first, a slot, the option's and the output's sizes, the target.
s32 add_and_get_variable(PPUContext& ctx, std::string_view name, s32 trans_id, u32 target, s32 slot, u32 out_size,
    u32 option)
{
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (slot < 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    const std::optional<bool> wrong = option_size_wrong(ctx, name, option, kAddAndGetOptSize);
    if (!wrong) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (*wrong || out_size != kVariableSize) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (!target) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_try_and_set_variable: the library first, the output, a slot, the option's and the output's sizes,
// the target.
s32 try_and_set_variable(PPUContext& ctx, std::string_view name, s32 trans_id, u32 target, s32 slot, u32 result,
    u32 result_size, u32 option)
{
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!result) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (slot < 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    const std::optional<bool> wrong = option_size_wrong(ctx, name, option, kTryAndSetOptSize);
    if (!wrong) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (*wrong || result_size != kVariableSize) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (!target) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_delete_multislot_variable and scenp_tus_delete_multislot_data: the slots (1 to 64, none negative),
// no option, then the library; the target isn't checked.
s32 delete_multislot(PPUContext& ctx, std::string_view name, s32 trans_id, u32 slots, s32 count, u32 option)
{
    if (!slots) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (count == 0 || option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (count > static_cast<s32>(SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_SLOTID);
    const std::optional<bool> valid = slots_valid(ctx, name, slots, count);
    if (!valid || !*valid) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    return offline(name, trans_id);
}

// ----------------------------------------------------------------------------------------------------- data

// RPCS3 scenp_tus_set_data: a slot, the data and its total size; the option's and the info's sizes; the library, the
// target.
s32 set_data(PPUContext& ctx, std::string_view name, s32 trans_id, u32 target, s32 slot, u32 total_size, u32 data,
    u32 info, u32 info_size, u32 option)
{
    if (slot < 0 || !data || !total_size) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    const std::optional<bool> wrong = option_size_wrong(ctx, name, option, kSetDataOptSize);
    if (!wrong) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (*wrong || (info && info_size != kDataInfoSize)) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!target) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_get_data: the target first, no option, a slot, the status's size; then the library.
s32 get_data(std::string_view name, s32 trans_id, u32 target, s32 slot, u32 status_size, u32 option)
{
    if (!target) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (option || slot < 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (status_size != kDataStatusSize) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_get_multislot_data_status: as getting several slots' variables, with statuses of 488 bytes.
s32 get_multislot_data_status(PPUContext& ctx, std::string_view name, s32 trans_id, u32 target, u32 slots,
    u32 statuses, u32 statuses_size, s32 count, u32 option)
{
    if (!slots || !statuses) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (count == 0 || option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!array_size_ok(statuses_size, count, kDataStatusSize)) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (count > static_cast<s32>(SCE_NP_TUS_MAX_SLOT_NUM_PER_TRANS)) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_SLOTID);
    const std::optional<bool> valid = slots_valid(ctx, name, slots, count);
    if (!valid || !*valid) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!target) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_get_multiuser_data_status: as getting several users' variables, with statuses of 488 bytes.
s32 get_multiuser_data_status(std::string_view name, s32 trans_id, u32 targets, s32 slot, u32 statuses,
    u32 statuses_size, s32 count, u32 option)
{
    if (!statuses) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (count == 0 || option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (count > SCE_NP_SCORE_MAX_NPID_NUM_PER_TRANS) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID);
    if (!array_size_ok(statuses_size, count, kDataStatusSize)) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (slot < 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    if (!targets) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    return offline(name, trans_id);
}

// RPCS3 scenp_tus_get_friends_data_status: as the friends' variables, with sort types 1 and 2 only.
s32 get_friends_data_status(PPUContext& ctx, std::string_view name, s32 trans_id, s32 slot, s32 sort_type,
    u32 statuses, u32 statuses_size, s32 count, u32 option)
{
    if (!statuses) return error(SCE_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT);
    if (count == 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    const std::optional<bool> bad_option = friends_option_bad(ctx, name, option);
    if (!bad_option || *bad_option) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (sort_type != SCE_NP_TUS_DATASTATUS_SORTTYPE_DESCENDING_DATE &&
        sort_type != SCE_NP_TUS_DATASTATUS_SORTTYPE_ASCENDING_DATE) {
        return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    }
    if (count > SCE_NP_SCORE_MAX_SELECTED_FRIENDS_NUM) return error(SCE_NP_COMMUNITY_ERROR_TOO_MANY_NPID);
    if (!array_size_ok(statuses_size, count, kDataStatusSize)) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ALIGNMENT);
    if (slot < 0) return error(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    return offline(name, trans_id);
}

// RPCS3 sceNpTssGetData / ...Async: the library, then online (before even the status pointer).
s32 tss_get_data(std::string_view name, s32 trans_id)
{
    if (!tus_initialized()) return error(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    return offline(name, trans_id);
}

// The four forms of each call (an NP ID or a virtual user ID as the target, synchronous or not) share their checks.
#define PS_TUS_TARGET_FORMS(stem, call)                                                                                \
    s32 sceNpTus##stem(PPUContext& ctx, s32 trans_id, u32 target, PS_TUS_ARGS)                                          \
    {                                                                                                                  \
        return call("sceNpTus" #stem);                                                                                 \
    }                                                                                                                  \
    s32 sceNpTus##stem##VUser(PPUContext& ctx, s32 trans_id, u32 target, PS_TUS_ARGS)                                   \
    {                                                                                                                  \
        return call("sceNpTus" #stem "VUser");                                                                         \
    }                                                                                                                  \
    s32 sceNpTus##stem##Async(PPUContext& ctx, s32 trans_id, u32 target, PS_TUS_ARGS)                                   \
    {                                                                                                                  \
        return call("sceNpTus" #stem "Async");                                                                         \
    }                                                                                                                  \
    s32 sceNpTus##stem##VUserAsync(PPUContext& ctx, s32 trans_id, u32 target, PS_TUS_ARGS)                              \
    {                                                                                                                  \
        return call("sceNpTus" #stem "VUserAsync");                                                                    \
    }

#define PS_TUS_ARGS u32 slots, u32 values, s32 count, u32 option
#define PS_TUS_CALL(name) set_multislot_variable(ctx, name, trans_id, target, slots, values, count, option)
PS_TUS_TARGET_FORMS(SetMultiSlotVariable, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS u32 slots, u32 variables, u32 variables_size, s32 count, u32 option
#define PS_TUS_CALL(name)                                                                                              \
    get_multislot_variable(ctx, name, trans_id, target, slots, variables, variables_size, count, option)
PS_TUS_TARGET_FORMS(GetMultiSlotVariable, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS s32 slot, u32 variables, u32 variables_size, s32 count, u32 option
#define PS_TUS_CALL(name)                                                                                              \
    (static_cast<void>(ctx), get_multiuser_variable(name, trans_id, target, slot, variables, variables_size, count, option))
PS_TUS_TARGET_FORMS(GetMultiUserVariable, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS s32 slot, s64 in_variable, u32 out_variable, u32 out_size, u32 option
#define PS_TUS_CALL(name)                                                                                              \
    (PS_LOG_DEBUG(kChan, "{}: add {} into {:#x}", name, in_variable, out_variable),                                    \
        add_and_get_variable(ctx, name, trans_id, target, slot, out_size, option))
PS_TUS_TARGET_FORMS(AddAndGetVariable, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS s32 slot, s32 ope_type, s64 variable, u32 result, u32 result_size, u32 option
#define PS_TUS_CALL(name)                                                                                              \
    (PS_LOG_DEBUG(kChan, "{}: operation {}, {}", name, ope_type, variable),                                            \
        try_and_set_variable(ctx, name, trans_id, target, slot, result, result_size, option))
PS_TUS_TARGET_FORMS(TryAndSetVariable, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS u32 slots, s32 count, u32 option
#define PS_TUS_CALL(name) (static_cast<void>(target), delete_multislot(ctx, name, trans_id, slots, count, option))
PS_TUS_TARGET_FORMS(DeleteMultiSlotVariable, PS_TUS_CALL)
PS_TUS_TARGET_FORMS(DeleteMultiSlotData, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS s32 slot, u32 total_size, u32 send_size, u32 data, u32 info, u32 info_size, u32 option
#define PS_TUS_CALL(name)                                                                                              \
    (PS_LOG_DEBUG(kChan, "{}: {} of {} bytes", name, send_size, total_size),                                           \
        set_data(ctx, name, trans_id, target, slot, total_size, data, info, info_size, option))
PS_TUS_TARGET_FORMS(SetData, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS s32 slot, u32 status, u32 status_size, u32 data, u32 recv_size, u32 option
#define PS_TUS_CALL(name)                                                                                              \
    (static_cast<void>(ctx), PS_LOG_DEBUG(kChan, "{}: status {:#x}, {} bytes into {:#x}", name, status, recv_size, data), \
        get_data(name, trans_id, target, slot, status_size, option))
PS_TUS_TARGET_FORMS(GetData, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS u32 slots, u32 statuses, u32 statuses_size, s32 count, u32 option
#define PS_TUS_CALL(name)                                                                                              \
    get_multislot_data_status(ctx, name, trans_id, target, slots, statuses, statuses_size, count, option)
PS_TUS_TARGET_FORMS(GetMultiSlotDataStatus, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#define PS_TUS_ARGS s32 slot, u32 statuses, u32 statuses_size, s32 count, u32 option
#define PS_TUS_CALL(name)                                                                                              \
    (static_cast<void>(ctx), get_multiuser_data_status(name, trans_id, target, slot, statuses, statuses_size, count, option))
PS_TUS_TARGET_FORMS(GetMultiUserDataStatus, PS_TUS_CALL)
#undef PS_TUS_CALL
#undef PS_TUS_ARGS

#undef PS_TUS_TARGET_FORMS

// The friends' calls have no target and no virtual user forms.
s32 sceNpTusGetFriendsVariable(PPUContext& ctx, s32 trans_id, s32 slot, s32 include_self, s32 sort_type,
    u32 variables, u32 variables_size, s32 count, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpTusGetFriendsVariable: self {}", include_self);
    return get_friends_variable(ctx, "sceNpTusGetFriendsVariable", trans_id, slot, sort_type, variables, variables_size,
        count, option);
}

s32 sceNpTusGetFriendsVariableAsync(PPUContext& ctx, s32 trans_id, s32 slot, s32 include_self, s32 sort_type,
    u32 variables, u32 variables_size, s32 count, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpTusGetFriendsVariableAsync: self {}", include_self);
    return get_friends_variable(ctx, "sceNpTusGetFriendsVariableAsync", trans_id, slot, sort_type, variables,
        variables_size, count, option);
}

s32 sceNpTusGetFriendsDataStatus(PPUContext& ctx, s32 trans_id, s32 slot, s32 include_self, s32 sort_type,
    u32 statuses, u32 statuses_size, s32 count, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpTusGetFriendsDataStatus: self {}", include_self);
    return get_friends_data_status(ctx, "sceNpTusGetFriendsDataStatus", trans_id, slot, sort_type, statuses,
        statuses_size, count, option);
}

s32 sceNpTusGetFriendsDataStatusAsync(PPUContext& ctx, s32 trans_id, s32 slot, s32 include_self, s32 sort_type,
    u32 statuses, u32 statuses_size, s32 count, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpTusGetFriendsDataStatusAsync: self {}", include_self);
    return get_friends_data_status(ctx, "sceNpTusGetFriendsDataStatusAsync", trans_id, slot, sort_type, statuses,
        statuses_size, count, option);
}

s32 sceNpTssGetData(PPUContext&, s32 trans_id, s32 slot, u32 status, u32 status_size, u32 data, u32 recv_size,
    u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpTssGetData: slot {}, status {:#x} ({}), {} bytes into {:#x}, option {:#x}", slot, status,
        status_size, recv_size, data, option);
    return tss_get_data("sceNpTssGetData", trans_id);
}

s32 sceNpTssGetDataAsync(PPUContext&, s32 trans_id, s32 slot, u32 status, u32 status_size, u32 data, u32 recv_size,
    u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpTssGetDataAsync: slot {}, status {:#x} ({}), {} bytes into {:#x}, option {:#x}", slot,
        status, status_size, recv_size, data, option);
    return tss_get_data("sceNpTssGetDataAsync", trans_id);
}

// sceNpTssGetDataNoLimit / ...Async (D-923): libsysutil_np_tus runs GetData's code with the data's size limit off;
// offline they answer as GetData does.
s32 sceNpTssGetDataNoLimit(PPUContext&, s32 trans_id, s32 slot, u32 status, u32 status_size, u32 data, u32 recv_size,
    u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpTssGetDataNoLimit: slot {}, status {:#x} ({}), {} bytes into {:#x}, option {:#x}", slot,
        status, status_size, recv_size, data, option);
    return tss_get_data("sceNpTssGetDataNoLimit", trans_id);
}

s32 sceNpTssGetDataNoLimitAsync(PPUContext&, s32 trans_id, s32 slot, u32 status, u32 status_size, u32 data,
    u32 recv_size, u32 option)
{
    PS_LOG_DEBUG(kChan, "sceNpTssGetDataNoLimitAsync: slot {}, status {:#x} ({}), {} bytes into {:#x}, option {:#x}",
        slot, status, status_size, recv_size, data, option);
    return tss_get_data("sceNpTssGetDataNoLimitAsync", trans_id);
}

} // namespace

void register_sceNpTus(Registry& r)
{
    r.add("sceNpTus", {
        PS_HLE_FUNC(0x8F87A06B, sceNpTusInit),
        PS_HLE_FUNC(0x225AED26, sceNpTusTerm),
        PS_HLE_FUNC(0x7CAF58EE, sceNpTusCreateTitleCtx),
        PS_HLE_FUNC(0x2E162A62, sceNpTusDestroyTitleCtx),
        PS_HLE_FUNC(0x1904435E, sceNpTusCreateTransactionCtx),
        PS_HLE_FUNC(0x44ECA8B4, sceNpTusDestroyTransactionCtx),
        PS_HLE_FUNC(0x59432970, sceNpTusSetTimeout),
        PS_HLE_FUNC(0x325C6284, sceNpTusAbortTransaction),
        PS_HLE_FUNC(0xB8E8FF22, sceNpTusWaitAsync),
        PS_HLE_FUNC(0x19BCE18C, sceNpTusPollAsync),
        PS_HLE_FUNC(0xCC86A8F6, sceNpTusSetMultiSlotVariable),
        PS_HLE_FUNC(0xF819BE91, sceNpTusSetMultiSlotVariableVUser),
        PS_HLE_FUNC(0x065B610D, sceNpTusSetMultiSlotVariableAsync),
        PS_HLE_FUNC(0x96A06212, sceNpTusSetMultiSlotVariableVUserAsync),
        PS_HLE_FUNC(0x0423E622, sceNpTusGetMultiSlotVariable),
        PS_HLE_FUNC(0x2357BA9E, sceNpTusGetMultiSlotVariableVUser),
        PS_HLE_FUNC(0xBB2877F2, sceNpTusGetMultiSlotVariableAsync),
        PS_HLE_FUNC(0xFC7D346E, sceNpTusGetMultiSlotVariableVUserAsync),
        PS_HLE_FUNC(0x0D15043B, sceNpTusGetMultiUserVariable),
        PS_HLE_FUNC(0x6C511024, sceNpTusGetMultiUserVariableVUser),
        PS_HLE_FUNC(0xCC7A31CD, sceNpTusGetMultiUserVariableAsync),
        PS_HLE_FUNC(0x9549D22C, sceNpTusGetMultiUserVariableVUserAsync),
        PS_HLE_FUNC(0x94989003, sceNpTusAddAndGetVariable),
        PS_HLE_FUNC(0xF60BE06F, sceNpTusAddAndGetVariableVUser),
        PS_HLE_FUNC(0x1FA5C87D, sceNpTusAddAndGetVariableAsync),
        PS_HLE_FUNC(0xA7993BF3, sceNpTusAddAndGetVariableVUserAsync),
        PS_HLE_FUNC(0x47E9424A, sceNpTusTryAndSetVariable),
        PS_HLE_FUNC(0x3602BC80, sceNpTusTryAndSetVariableVUser),
        PS_HLE_FUNC(0xBBB244B7, sceNpTusTryAndSetVariableAsync),
        PS_HLE_FUNC(0x17DB7AA7, sceNpTusTryAndSetVariableVUserAsync),
        PS_HLE_FUNC(0xAF985783, sceNpTusDeleteMultiSlotVariable),
        PS_HLE_FUNC(0xC4E51FBF, sceNpTusDeleteMultiSlotVariableVUser),
        PS_HLE_FUNC(0xF5363608, sceNpTusDeleteMultiSlotVariableAsync),
        PS_HLE_FUNC(0xC2E18DA8, sceNpTusDeleteMultiSlotVariableVUserAsync),
        PS_HLE_FUNC(0x7D5F0F0E, sceNpTusSetData),
        PS_HLE_FUNC(0x0835DEB2, sceNpTusSetDataVUser),
        PS_HLE_FUNC(0xE847341F, sceNpTusSetDataAsync),
        PS_HLE_FUNC(0x9CC0CF44, sceNpTusSetDataVUserAsync),
        PS_HLE_FUNC(0x8DDD0D85, sceNpTusGetData),
        PS_HLE_FUNC(0xAE4E590E, sceNpTusGetDataVUser),
        PS_HLE_FUNC(0x5175ABB9, sceNpTusGetDataAsync),
        PS_HLE_FUNC(0x38F364B0, sceNpTusGetDataVUserAsync),
        PS_HLE_FUNC(0xC848D425, sceNpTusGetMultiSlotDataStatus),
        PS_HLE_FUNC(0xA3ABFADB, sceNpTusGetMultiSlotDataStatusVUser),
        PS_HLE_FUNC(0x651FD79F, sceNpTusGetMultiSlotDataStatusAsync),
        PS_HLE_FUNC(0x2AB21EA9, sceNpTusGetMultiSlotDataStatusVUserAsync),
        PS_HLE_FUNC(0x348DBCB4, sceNpTusGetMultiUserDataStatus),
        PS_HLE_FUNC(0x2D1B9F1A, sceNpTusGetMultiUserDataStatusVUser),
        PS_HLE_FUNC(0xC66BA67E, sceNpTusGetMultiUserDataStatusAsync),
        PS_HLE_FUNC(0x368FEC59, sceNpTusGetMultiUserDataStatusVUserAsync),
        PS_HLE_FUNC(0xE0719847, sceNpTusDeleteMultiSlotData),
        PS_HLE_FUNC(0x01711E81, sceNpTusDeleteMultiSlotDataVUser),
        PS_HLE_FUNC(0x3175AF23, sceNpTusDeleteMultiSlotDataAsync),
        PS_HLE_FUNC(0xC815B219, sceNpTusDeleteMultiSlotDataVUserAsync),
        PS_HLE_FUNC(0x91A34A5B, sceNpTusGetFriendsVariable),
        PS_HLE_FUNC(0x9F67BFBD, sceNpTusGetFriendsVariableAsync),
        PS_HLE_FUNC(0x479C65D0, sceNpTusGetFriendsDataStatus),
        PS_HLE_FUNC(0x3E4A2075, sceNpTusGetFriendsDataStatusAsync),
        PS_HLE_FUNC(0x56D3A5AB, sceNpTssGetData),
        PS_HLE_FUNC(0x1E0870D1, sceNpTssGetDataAsync),
        PS_HLE_FUNC(0xF46D8AE5, sceNpTssGetDataNoLimit),
        PS_HLE_FUNC(0x7240C64D, sceNpTssGetDataNoLimitAsync),
    });
}

} // namespace pspocket::hle
