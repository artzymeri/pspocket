// sceNpUtil: the network bandwidth test a game runs before online play. Reference: RPCS3
// rpcs3/Emu/Cell/Modules/sceNpUtil.cpp/.h (bandwidth_test, sce_np_util_manager).
//
// As RPCS3: the test is a fake (RPCS3's TODO: "run proper bandwidth test, probably with cellHttp"). It runs for about
// 100 ms, or until it is aborted or shut down, then finishes with 100,000,000 bits per second up and down and CELL_OK.
// One test at a time: a second start is refused until sceNpUtilBandwidthTestShutdown, which ends the test, writes its
// result (when the game gives a buffer) and drops it. The priority and stack size of RPCS3's thread are not used
// (RPCS3 runs a host thread too). Here the test is a deadline on the host's clock instead of a thread: it is RUNNING
// until the deadline, an abort or a shutdown, then FINISHED.
//
// Difference from RPCS3: a result buffer that is not guest memory is logged with the call site and refused with
// SCE_NP_ERROR_INVALID_ARGUMENT before anything changes (D-042: RPCS3 faults).

#include <bit>
#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sceNpUtil";

enum : u32 {
    SCE_NP_ERROR_NOT_INITIALIZED = 0x8002AA01,
    SCE_NP_ERROR_ALREADY_INITIALIZED = 0x8002AA02,
    SCE_NP_ERROR_INVALID_ARGUMENT = 0x8002AA03,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr s32 SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_RUNNING = 1, SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_FINISHED = 2;
// RPCS3's thread waits 1 ms at a time and ends after its 102nd wait.
constexpr std::chrono::milliseconds kFakeTestTime{102};
constexpr double kFakeBitsPerSecond = 100'000'000.0;
// SceNpUtilBandwidthTestResult: be f64 upload_bps, be f64 download_bps, be s32 result, 4 bytes of padding.
constexpr u32 kResultSize = 24;

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case SCE_NP_ERROR_NOT_INITIALIZED: return "SCE_NP_ERROR_NOT_INITIALIZED";
    case SCE_NP_ERROR_ALREADY_INITIALIZED: return "SCE_NP_ERROR_ALREADY_INITIALIZED";
    case SCE_NP_ERROR_INVALID_ARGUMENT: return "SCE_NP_ERROR_INVALID_ARGUMENT";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

// RPCS3 sce_np_util_manager::bandwidth_test_thread: the running test, if any. It ends at `deadline`, or at once once
// aborted (RPCS3's thread checks its abort and shutdown flags every millisecond).
struct UtilState {
    std::mutex mutex;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool aborted = false;

    bool finished() const { return aborted || std::chrono::steady_clock::now() >= *deadline; }
};

UtilState& util()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "sceNpUtil called without an HLE process");
    return p->state<UtilState>();
}

// RPCS3 sceNpUtilBandwidthTestInitStart(prio, stack).
s32 sceNpUtilBandwidthTestInitStart(PPUContext&, u32 prio, u32 stack)
{
    UtilState& s = util();
    std::lock_guard lock(s.mutex);
    s32 r = error(SCE_NP_ERROR_ALREADY_INITIALIZED);
    if (!s.deadline) {
        s.deadline = std::chrono::steady_clock::now() + kFakeTestTime;
        s.aborted = false;
        r = lv2::CELL_OK;
    }
    PS_LOG_WARN(kChan, "sceNpUtilBandwidthTestInitStart(prio {}, stack {:#x}): {}{}", prio, stack, result_name(r),
        r == lv2::CELL_OK ? "; RPCS3 todo: a fake test of about 100 ms, no traffic" : "");
    return r;
}

// RPCS3 sceNpUtilBandwidthTestGetStatus: the status, returned as not_an_error.
s32 sceNpUtilBandwidthTestGetStatus(PPUContext&)
{
    UtilState& s = util();
    std::lock_guard lock(s.mutex);
    if (!s.deadline) {
        PS_LOG_INFO(kChan, "sceNpUtilBandwidthTestGetStatus(): {}", result_name(error(SCE_NP_ERROR_NOT_INITIALIZED)));
        return error(SCE_NP_ERROR_NOT_INITIALIZED);
    }
    const bool finished = s.finished();
    PS_LOG_INFO(kChan, "sceNpUtilBandwidthTestGetStatus(): {}", finished ? "FINISHED" : "RUNNING");
    return finished ? SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_FINISHED : SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_RUNNING;
}

// RPCS3 sceNpUtilBandwidthTestShutdown(SceNpUtilBandwidthTestResult* result): ends the test (RPCS3 sets the shutdown
// flag and waits for the thread to finish, which it does within a millisecond), writes the result if `result` is not
// null (RPCS3's TODO asks what a null one does) and drops the test.
s32 sceNpUtilBandwidthTestShutdown(PPUContext& ctx, u32 result)
{
    const auto done = [&](s32 r) {
        PS_LOG_WARN(kChan, "sceNpUtilBandwidthTestShutdown(result {:#x}): {}", result, result_name(r));
        return r;
    };
    UtilState& s = util();
    std::lock_guard lock(s.mutex);
    if (!s.deadline) return done(error(SCE_NP_ERROR_NOT_INITIALIZED));
    if (result) {
        if (!guest_readable(thread_of(ctx), result, kResultSize)) {
            log_call_site(ctx, guest_base(ctx),
                std::format("sceNpUtilBandwidthTestShutdown: result ({:#x}, {} bytes) is not guest memory", result,
                    kResultSize));
            return done(error(SCE_NP_ERROR_INVALID_ARGUMENT));
        }
        u8* base = guest_base(ctx);
        ps_write64(base, result, std::bit_cast<u64>(kFakeBitsPerSecond));
        ps_write64(base, result + 8, std::bit_cast<u64>(kFakeBitsPerSecond));
        ps_write32(base, result + 16, static_cast<u32>(lv2::CELL_OK));
        ps_write32(base, result + 20, 0); // RPCS3 copies its zeroed padding too
    }
    s.deadline.reset();
    s.aborted = false;
    return done(lv2::CELL_OK);
}

// RPCS3 sceNpUtilBandwidthTestAbort: sets the abort flag, so the test finishes (with the same fake result) at once.
// RPCS3's TODO would also abort the HTTP connection: there is none.
s32 sceNpUtilBandwidthTestAbort(PPUContext&)
{
    UtilState& s = util();
    std::lock_guard lock(s.mutex);
    s32 r = error(SCE_NP_ERROR_NOT_INITIALIZED);
    if (s.deadline) {
        s.aborted = true;
        r = lv2::CELL_OK;
    }
    PS_LOG_WARN(kChan, "sceNpUtilBandwidthTestAbort(): {}{}", result_name(r),
        r == lv2::CELL_OK ? "; RPCS3 todo: no connection to abort" : "");
    return r;
}

} // namespace

void register_sceNpUtil(Registry& r)
{
    r.add("sceNpUtil", {
        PS_HLE_FUNC(0xC2CED2B7, sceNpUtilBandwidthTestInitStart),
        PS_HLE_FUNC(0x432B3CBF, sceNpUtilBandwidthTestShutdown),
        PS_HLE_FUNC(0xC880F37D, sceNpUtilBandwidthTestGetStatus),
        PS_HLE_FUNC(0xC99EE313, sceNpUtilBandwidthTestAbort),
    });
}

} // namespace pspocket::hle
