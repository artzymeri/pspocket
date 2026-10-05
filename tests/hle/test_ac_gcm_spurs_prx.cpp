// Assassin's Creed's (BLES00158) cellGcmSys, cellSpurs and sysPrxForUser imports that GTA V does not use, on real lv2
// threads over synthetic guest memory (no game data). GCM: the exact words cellGcmSetFlip and cellGcmSetWaitFlip write
// through the game's context (and through its callback when a segment is full) and the RSX thread running them, the
// local report area, flip mode and tiles as lv2 takes them. SPURS: cellSpursCreateTaskset, _cellSpursSendSignal and
// cellSpursAddUrgentCommand on the guest structures, and with the native workers running a task and a job chain.
// sysPrxForUser: spinlocks across host threads and against reservations, the sys_mmapper wrappers,
// sys_process_is_stack, sys_spu_image_close and sys_game_process_exitspawn's parameter block.

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "internal.hpp" // runtime/lv2/src (white-box): the RSX device's display sync and tiles
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/core/reservation.h"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/rsx.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u64 kSecond = 1'000'000'000ull;

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The log lines written while it lives.
struct LogLines {
    struct Line {
        log::Level level;
        std::string channel, message;
    };
    std::mutex mutex;
    std::vector<Line> lines;

    LogLines()
    {
        log::set_sink(
            [](void* user, log::Level level, std::string_view channel, std::string_view message) {
                auto* self = static_cast<LogLines*>(user);
                std::lock_guard lock(self->mutex);
                self->lines.push_back({level, std::string(channel), std::string(message)});
            },
            this);
    }
    ~LogLines() { log::set_sink(nullptr, nullptr); }
    LogLines(const LogLines&) = delete;
    LogLines& operator=(const LogLines&) = delete;

    // The lines at `level` on `channel` that contain `text`.
    usize count(log::Level level, std::string_view channel, std::string_view text)
    {
        std::lock_guard lock(mutex);
        usize n = 0;
        for (const Line& l : lines)
            if (l.level == level && l.channel == channel && l.message.find(text) != std::string::npos) n++;
        return n;
    }
    // The first such line, or "".
    std::string find(log::Level level, std::string_view channel, std::string_view text)
    {
        std::lock_guard lock(mutex);
        for (const Line& l : lines)
            if (l.level == level && l.channel == channel && l.message.find(text) != std::string::npos) return l.message;
        return {};
    }
};

// Guest code the HLE calls back into (a GCM context's callback) runs on the PPU interpreter while this lives.
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// Calls an HLE function with one argument as guest code does, without Catch assertions (for other host threads).
u64 call1(GuestThread& g, const hle::Function& fn, u64 arg)
{
    g.ctx.gpr[3] = arg;
    fn.fn(g.ctx, g.f.base());
    return g.ctx.gpr[3];
}

// ------------------------------------------------------------------------------------------------- GCM

constexpr u32 kIo = 0x00200000;   // the "game's" IO buffer: 1 MB of main memory mapped at IO offset 0
constexpr u32 kCtxPtr = kData;    // CellGcmContextData* written by _cellGcmInitBody
constexpr u32 kCmdSize = 0x40000; // 8 segments of 32 KB
constexpr u32 CELL_GCM_ERROR_FAILURE = 0x802100ff, CELL_GCM_ERROR_INVALID_ENUM = 0x80210002,
              CELL_GCM_ERROR_INVALID_VALUE = 0x80210003, CELL_GCM_ERROR_INVALID_ALIGNMENT = 0x80210004;

lv2::Config rsx_test_config()
{
    lv2::Config c;
    c.rsx_log_failures = true;
    return c;
}

// What the loader and the game's CRT do first: link the module's internal functions, initialize TLS.
struct GcmFixture : Fixture {
    GcmFixture() : Fixture({}, rsx_test_config())
    {
        REQUIRE(hle->link_internal_functions({"cellGcmSys"}));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

// ------------------------------------------------------------------------------------------------- SPURS

const u64 kTaskInval = r3_of_code(0x80410902), kTaskSrch = r3_of_code(0x80410905), kTaskFault = r3_of_code(0x8041090D),
          kTaskStat = r3_of_code(0x8041090F), kTaskAlign = r3_of_code(0x80410910), kTaskNull = r3_of_code(0x80410911);
const u64 kJobInval = r3_of_code(0x80410A02), kJobBusy = r3_of_code(0x80410A0A), kJobAlign = r3_of_code(0x80410A10),
          kJobNull = r3_of_code(0x80410A11);

// A task ELF at `ea`: one read-only executable PT_LOAD at 0x3000 holding `code` (what libsre's loader takes).
void write_task_elf(u8* base, u32 ea, const std::vector<u32>& code)
{
    const u32 size = static_cast<u32>(code.size() * 4);
    std::memset(base + ea, 0, 0x100 + size + 16);
    static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(base + ea, kIdent, 16);
    ps_write16(base, ea + 0x10, 2);    // ET_EXEC
    ps_write16(base, ea + 0x12, 0x17); // EM_SPU
    ps_write32(base, ea + 0x14, 1);
    ps_write32(base, ea + 0x18, 0x3000); // entry
    ps_write32(base, ea + 0x1C, 0x34);   // phoff
    ps_write16(base, ea + 0x28, 0x34);
    ps_write16(base, ea + 0x2A, 0x20);
    ps_write16(base, ea + 0x2C, 1);
    const u32 ph = ea + 0x34;
    ps_write32(base, ph + 0x00, 1);      // PT_LOAD
    ps_write32(base, ph + 0x04, 0x100);  // offset
    ps_write32(base, ph + 0x08, 0x3000); // vaddr
    ps_write32(base, ph + 0x0C, 0x3000); // paddr
    ps_write32(base, ph + 0x10, size);
    ps_write32(base, ph + 0x14, (size + 15) & ~15u);
    ps_write32(base, ph + 0x18, 5); // R X
    ps_write32(base, ph + 0x1C, 0x80);
    for (usize i = 0; i < code.size(); i++) ps_write32(base, ea + 0x100 + 4 * static_cast<u32>(i), code[i]);
}

} // namespace

TEST_CASE("cellGcmSys (AC): flip mode through lv2, time stamps and reports in local memory, cellGcmSetTile",
    "[hle][gcm][ac]")
{
    LogLines logs;
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    lv2::RsxDevice& dev = f.kernel->impl().rsx;
    const auto vsync = [&] {
        std::lock_guard lock(dev.mutex);
        return dev.vsync;
    };
    const auto tile = [&](u32 i) {
        std::lock_guard lock(dev.mutex);
        return dev.tiles[i];
    };

    // Before cellGcmInit there is no RSX context to take the flip mode: logged.
    g.call("cellGcmSetFlipMode", 2);
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "cellGcmSetFlipMode(2): lv2 refused") == 1);

    // Flip mode: lv2's display sync (0x101) takes it, vsync for CELL_GCM_DISPLAY_VSYNC (2) only, as RPCS3.
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    CHECK_FALSE(vsync());
    g.call("cellGcmSetFlipMode", 2);
    CHECK(vsync());
    g.call("cellGcmSetFlipMode", 1); // CELL_GCM_DISPLAY_HSYNC
    CHECK_FALSE(vsync());

    // Reports: CellGcmReportData {u64 timer, u32 value, u32 pad}, 16 bytes each from the reports area's 0x1400.
    const u32 label0 = static_cast<u32>(g.call("cellGcmGetLabelAddress", 0));
    const u32 report5 = label0 + 0x1400 + 5 * 0x10;
    CHECK(g.call("cellGcmGetReportDataAddress", 5) == report5);
    CHECK(g.call("cellGcmGetReportDataAddress", 5) == g.call("cellGcmGetReportDataAddressLocation", 5, 0));
    ps_write64(b, report5, 0x0123456789ABCDEFull);
    ps_write32(b, report5 + 8, 0x4242);
    CHECK(g.call("cellGcmGetTimeStamp", 5) == 0x0123456789ABCDEFull);
    for (u32 type = 1; type <= 5; type++) CHECK(g.call("cellGcmGetReport", type, 5) == 0x4242);
    CHECK(g.call("cellGcmGetReport", 0, 5) == 0xFFFFFFFFull); // not a report type: -1 (a u32)
    CHECK(g.call("cellGcmGetReport", 6, 5) == 0xFFFFFFFFull);
    // An index past 2047 is logged and used anyway (RPCS3).
    CHECK(g.call("cellGcmGetReportDataAddress", 2048) == label0 + 0x1400 + 2048 * 0x10);
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "report index 2048 out of range (local)") == 1);

    // cellGcmSetTile: cellGcmSetTileInfo's checks, in RPCS3's order.
    CHECK(g.call("cellGcmSetTile", 15, 0, 0, 0x10000, 0x100, 0, 0, 0) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    CHECK(g.call("cellGcmSetTile", 0, 0, 0, 0x10000, 0x100, 0, 2048, 0) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    CHECK(g.call("cellGcmSetTile", 0, 0, 0, 0x10000, 0x100, 0, 0, 4) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    CHECK(g.call("cellGcmSetTile", 0, 0, 0x100, 0x10000, 0x100, 0, 0, 0) ==
          r3_of_code(CELL_GCM_ERROR_INVALID_ALIGNMENT));
    CHECK(g.call("cellGcmSetTile", 0, 0, 0, 0x10000, 0x180, 0, 0, 0) == r3_of_code(CELL_GCM_ERROR_INVALID_ALIGNMENT));
    CHECK(g.call("cellGcmSetTile", 0, 2, 0, 0x10000, 0x100, 0, 0, 0) == r3_of_code(CELL_GCM_ERROR_INVALID_ENUM));
    CHECK(g.call("cellGcmSetTile", 0, 0, 0, 0x10000, 0x100, 6, 0, 0) == r3_of_code(CELL_GCM_ERROR_INVALID_ENUM));
    CHECK_FALSE(tile(0).bound);

    // A pitch binds the tile, its packed words decoded by lv2; pitch 0 unbinds it.
    CHECK_FALSE(tile(3).bound);
    REQUIRE(g.call("cellGcmSetTile", 3, 1, 0x200000, 0x300000, 0x1400, 0, 7, 2) == 0);
    {
        const lv2::RsxDevice::Tile t = tile(3);
        CHECK(t.bound);
        CHECK(t.location == 1);
        CHECK(t.offset == 0x200000);
        CHECK(t.size == 0x300000);
        CHECK(t.pitch == 0x1400);
        CHECK(t.comp == 0);
        CHECK(t.base == 7);
    }
    REQUIRE(g.call("cellGcmSetTile", 3, 1, 0x200000, 0x300000, 0, 0, 7, 2) == 0);
    CHECK_FALSE(tile(3).bound);
    // A compression mode is logged (not emulated, as RPCS3) and kept.
    REQUIRE(g.call("cellGcmSetTile", 4, 0, 0, 0x10000, 0x100, 8, 0, 0) == 0);
    CHECK(tile(4).bound);
    CHECK(tile(4).comp == 8);
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "cellGcmSetTile: compression mode 8") == 1);
}

TEST_CASE("cellGcmSys (AC): cellGcmSetFlip and cellGcmSetWaitFlip write their commands; the RSX flips and waits",
    "[hle][gcm][ac]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    const u32 label0 = static_cast<u32>(g.call("cellGcmGetLabelAddress", 0));
    rsx::Rsx* rsx = f.kernel->rsx();
    REQUIRE(rsx != nullptr);
    std::atomic<u32> flipped{0xFFFFFFFF};
    f.kernel->set_rsx_flip_listener([&](u32 id) { flipped = id; });
    REQUIRE(g.call("cellGcmSetDisplayBuffer", 0, 0x00100000, 5120, 1280, 720) == 0);
    REQUIRE(g.call("cellGcmSetDisplayBuffer", 1, 0x00500000, 5120, 1280, 720) == 0);

    const auto current = [&] { return ps_read32(b, ctx + 0x8); };
    const auto put = [&] { return ps_read32(b, control); };
    const auto flush = [&] {
        ps_write32(b, control, current() - kIo);
        rsx->notify();
    };
    const auto method = [&](u32 reg, u32 value) { // what libgcm's inline command functions write
        const u32 c = current();
        ps_write32(b, c, rsx::cmd::method_header(reg, 1));
        ps_write32(b, c + 4, value);
        ps_write32(b, ctx + 0x8, c + 8);
    };
    // The semaphore context of the waits and label releases below (libgcm's initial state sets it).
    method(rsx::m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, rsx::loc::kDmaSemaphoreRW);
    flush();
    REQUIRE(rsx->wait_idle(2 * kSecond));

    // cellGcmSetFlip on the default context: GCM_FLIP_COMMAND and the buffer id at current, and put moves past them.
    const u32 c0 = current();
    REQUIRE(g.call("cellGcmSetFlip", ctx, 1) == 0);
    CHECK(ps_read32(b, c0) == rsx::cmd::method_header(rsx::m::GCM_FLIP_COMMAND, 1));
    CHECK(ps_read32(b, c0 + 4) == 1);
    CHECK(current() == c0 + 8);
    CHECK(put() == c0 + 8 - kIo);
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(rsx->flip_count() == 1);
    CHECK(flipped == 1);
    // A buffer id past 7: CELL_GCM_ERROR_FAILURE, nothing written.
    CHECK(g.call("cellGcmSetFlip", ctx, 8) == r3_of_code(CELL_GCM_ERROR_FAILURE));
    CHECK(current() == c0 + 8);
    CHECK(put() == c0 + 8 - kIo);

    // cellGcmSetWaitFlip: NV406E_SEMAPHORE_OFFSET 0x10, then SEMAPHORE_ACQUIRE 0; put stays (the game flushes).
    ps_write32(b, label0 + 0x10, 1); // a flip is pending: label 1 is not 0
    ps_write32(b, label0 + 0x20, 0);
    const u32 c1 = current();
    g.call("cellGcmSetWaitFlip", ctx);
    CHECK(ps_read32(b, c1) == rsx::cmd::method_header(rsx::m::NV406E_SEMAPHORE_OFFSET, 2));
    CHECK(ps_read32(b, c1 + 4) == 0x10);
    CHECK(ps_read32(b, c1 + 8) == 0);
    CHECK(current() == c1 + 12);
    CHECK(put() == c1 - kIo);
    // A label release behind it runs once label 1 is 0.
    method(rsx::m::NV406E_SEMAPHORE_OFFSET, 0x20);
    method(rsx::m::NV406E_SEMAPHORE_RELEASE, 0xBEEF);
    flush();
    REQUIRE(wait_for([&] { return rsx->status().block == rsx::Rsx::BlockReason::Semaphore; }));
    const rsx::Rsx::Status waiting = rsx->status();
    CHECK(waiting.semaphore_offset == 0x10);
    CHECK(waiting.block_value == 0);
    CHECK(ps_read32(b, label0 + 0x20) == 0);
    ps_write32(b, label0 + 0x10, 0);
    rsx->notify();
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(ps_read32(b, label0 + 0x20) == 0xBEEF);

    // A flip clears label 1, so a wait behind it passes.
    ps_write32(b, label0 + 0x10, 1);
    REQUIRE(g.call("cellGcmSetFlip", ctx, 0) == 0);
    g.call("cellGcmSetWaitFlip", ctx);
    method(rsx::m::NV406E_SEMAPHORE_OFFSET, 0x20);
    method(rsx::m::NV406E_SEMAPHORE_RELEASE, 0xCAFE);
    flush();
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(rsx->flip_count() == 2);
    CHECK(flipped == 0);
    CHECK(ps_read32(b, label0 + 0x10) == 0);
    CHECK(ps_read32(b, label0 + 0x20) == 0xCAFE);
}

TEST_CASE("cellGcmSys (AC): cellGcmSetFlip and cellGcmSetWaitFlip make room through the context's callback",
    "[hle][gcm][ac]")
{
    InterpretMisses interpret; // the game's own callbacks below are guest code
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    rsx::Rsx* rsx = f.kernel->rsx();
    REQUIRE(rsx != nullptr);

    // The default context's first segment is full: cellGcmCallback (through the context's descriptor) flushes and
    // jumps to the next segment, where the flip goes. put then moves 8 bytes on from where the callback left it, as
    // RPCS3 adds the command's size to put.
    const u32 end = ps_read32(b, ctx + 0x4);
    ps_write32(b, ctx + 0x8, end);
    REQUIRE(g.call("cellGcmSetFlip", ctx, 2) == 0);
    CHECK(ps_read32(b, end) == rsx::cmd::old_jump(0x8000));
    CHECK(ps_read32(b, ctx + 0x0) == kIo + 0x8000);
    CHECK(ps_read32(b, ctx + 0x4) == kIo + 0x10000 - 4);
    CHECK(ps_read32(b, kIo + 0x8000) == rsx::cmd::method_header(rsx::m::GCM_FLIP_COMMAND, 1));
    CHECK(ps_read32(b, kIo + 0x8004) == 2);
    CHECK(ps_read32(b, ctx + 0x8) == kIo + 0x8008);
    CHECK(ps_read32(b, control) == end - kIo + 8);
    ps_write32(b, control, 0x8008); // the game's next flush
    rsx->notify();
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(rsx->flip_count() == 1);

    // cellGcmSetWaitFlip with two words left in the segment: the callback first, then the command in the next one.
    const u32 end1 = ps_read32(b, ctx + 0x4);
    ps_write32(b, ctx + 0x8, end1 - 4);
    g.call("cellGcmSetWaitFlip", ctx);
    CHECK(ps_read32(b, end1 - 4) == rsx::cmd::old_jump(0x10000));
    CHECK(ps_read32(b, ctx + 0x0) == kIo + 0x10000);
    CHECK(ps_read32(b, kIo + 0x10000) == rsx::cmd::method_header(rsx::m::NV406E_SEMAPHORE_OFFSET, 2));
    CHECK(ps_read32(b, kIo + 0x10004) == 0x10);
    CHECK(ps_read32(b, kIo + 0x10008) == 0);
    CHECK(ps_read32(b, ctx + 0x8) == kIo + 0x1000C);
    CHECK(ps_read32(b, control) == end1 - 4 - kIo); // the callback's flush only

    // A game's own context: its own command buffer and callback, and put is not touched.
    constexpr u32 kOwn = kData + 0x200, kOwnBuf = kIo + 0x80000, kOwnEnd = kOwnBuf + 0x100 - 4;
    constexpr u32 kFailNeg = kData + 0x1000, kFailPos = kData + 0x1010; // code
    constexpr u32 kOpdNeg = kData + 0x1100, kOpdPos = kData + 0x1108;   // their descriptors
    ps_write32(b, kFailNeg, 0x3860FFFF);     // li r3,-1
    ps_write32(b, kFailNeg + 4, 0x4E800020); // blr
    ps_write32(b, kFailPos, 0x38600005);     // li r3,5
    ps_write32(b, kFailPos + 4, 0x4E800020); // blr
    ps_write32(b, kOpdNeg, kFailNeg);
    ps_write32(b, kOpdNeg + 4, 0);
    ps_write32(b, kOpdPos, kFailPos);
    ps_write32(b, kOpdPos + 4, 0);
    const auto own_context = [&](u32 current, u32 callback) {
        ps_write32(b, kOwn + 0x0, kOwnBuf);
        ps_write32(b, kOwn + 0x4, kOwnEnd);
        ps_write32(b, kOwn + 0x8, current);
        ps_write32(b, kOwn + 0xC, callback);
    };
    const u32 put_before = ps_read32(b, control);
    own_context(kOwnBuf, kOpdNeg);
    REQUIRE(g.call("cellGcmSetFlip", kOwn, 3) == 0);
    CHECK(ps_read32(b, kOwnBuf) == rsx::cmd::method_header(rsx::m::GCM_FLIP_COMMAND, 1));
    CHECK(ps_read32(b, kOwnBuf + 4) == 3);
    CHECK(ps_read32(b, kOwn + 0x8) == kOwnBuf + 8);
    CHECK(ps_read32(b, control) == put_before);

    // Its callback fails: a negative result is CELL_GCM_ERROR_FAILURE, a positive one CELL_OK (RPCS3's not_an_error);
    // nothing is written either way, and cellGcmSetWaitFlip writes nothing.
    own_context(kOwnEnd, kOpdNeg);
    CHECK(g.call("cellGcmSetFlip", kOwn, 3) == r3_of_code(CELL_GCM_ERROR_FAILURE));
    CHECK(ps_read32(b, kOwn + 0x8) == kOwnEnd);
    CHECK(ps_read32(b, kOwnEnd) == 0);
    g.call("cellGcmSetWaitFlip", kOwn);
    CHECK(ps_read32(b, kOwn + 0x8) == kOwnEnd);
    CHECK(ps_read32(b, kOwnEnd) == 0);
    own_context(kOwnEnd, kOpdPos);
    CHECK(g.call("cellGcmSetFlip", kOwn, 3) == 0);
    CHECK(ps_read32(b, kOwn + 0x8) == kOwnEnd);
    CHECK(ps_read32(b, kOwnEnd) == 0);

    // A context, or a callback descriptor, that is not guest memory is refused (D-042).
    CHECK(g.call("cellGcmSetFlip", 0x70000000, 0) == r3_of_code(CELL_GCM_ERROR_FAILURE));
    own_context(kOwnEnd, 0x70000000);
    CHECK(g.call("cellGcmSetFlip", kOwn, 3) == r3_of_code(CELL_GCM_ERROR_FAILURE));
    g.call("cellGcmSetWaitFlip", kOwn);
    CHECK(ps_read32(b, kOwn + 0x8) == kOwnEnd);
    CHECK(ps_read32(b, kOwnEnd) == 0);
}

TEST_CASE("cellSpurs (AC): cellSpursCreateTaskset, _cellSpursSendSignal and cellSpursAddUrgentCommand on the guest "
          "structures",
    "[hle][spurs][ac]")
{
    hle::ProcessConfig config;
    config.spurs_workers = false; // the PPU API alone
    Fixture f(config);
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kJcAttr = kData + 0x4000;
    constexpr u32 kPrio = kData + 0x4400, kJc = kData + 0x5000, kCmds = kData + 0x5400, kTs = kData + 0x6000;
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 5, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    const u8 prio[8] = {1, 1, 1, 1, 1, 0, 0, 0};
    std::memcpy(b + kPrio, prio, 8);

    // cellSpursCreateTaskset: create_taskset's checks, then a CellSpursTaskset with no name and enable_clear_ls 0.
    CHECK(a.call("cellSpursCreateTaskset", 0, kTs, 0x1234, kPrio, 5) == kTaskNull);
    CHECK(a.call("cellSpursCreateTaskset", kSpurs, 0, 0x1234, kPrio, 5) == kTaskNull);
    CHECK(a.call("cellSpursCreateTaskset", kSpurs + 0x40, kTs, 0x1234, kPrio, 5) == kTaskAlign);
    CHECK(a.call("cellSpursCreateTaskset", kSpurs, kTs + 0x40, 0x1234, kPrio, 5) == kTaskAlign);
    CHECK(a.call("cellSpursCreateTaskset", kSpurs, kTs, 0x1234, 0x70000000, 5) == kTaskFault); // unreadable priorities
    std::memset(b + kTs, 0xEE, 6400);
    REQUIRE(a.call("cellSpursCreateTaskset", kSpurs, kTs, 0x1234, kPrio, 5) == 0);
    CHECK(ps_read64(b, kTs + 0x60) == kSpurs);
    CHECK(ps_read64(b, kTs + 0x68) == 0x1234);
    CHECK(ps_read8(b, kTs + 0x70) == 0);    // enable_clear_ls
    CHECK(ps_read8(b, kTs + 0x72) == 0x80); // wkl_flag_wait_task
    CHECK(ps_read32(b, kTs + 0x74) == 0);   // workload 0
    CHECK(ps_read32(b, kTs + 0x1890) == 6400);
    CHECK(ps_read32(b, kTs + 6400 - 4) == 0); // the whole CellSpursTaskset cleared
    CHECK(ps_read64(b, kSpurs + 0xB00) == 0x200); // wklInfo1[0].addr: the taskset policy marker
    CHECK(ps_read64(b, kSpurs + 0xB08) == kTs);
    CHECK(ps_read32(b, kSpurs + 0xB10) == 0x1E40);
    CHECK(ps_read8(b, kSpurs + 0xB18) == 1);  // priority[0]
    CHECK(ps_read8(b, kSpurs + 0x40) == 8);   // min contention
    CHECK(ps_read8(b, kSpurs + 0x50) == 5);   // max contention
    CHECK(ps_read64(b, kSpurs + 0xE08) == 0); // no name
    CHECK(ps_read8(b, kSpurs + 0x80) == 2);   // runnable

    // _cellSpursSendSignal: NULL, ALIGN, a task ID past 127, an unreadable taskset, a workload ID past 31, a task that
    // is not enabled.
    CHECK(a.call("_cellSpursSendSignal", 0, 0) == kTaskNull);
    CHECK(a.call("_cellSpursSendSignal", kTs + 0x40, 0) == kTaskAlign);
    CHECK(a.call("_cellSpursSendSignal", kTs, 128) == kTaskInval);
    CHECK(a.call("_cellSpursSendSignal", 0x70000000, 0) == kTaskFault);
    ps_write32(b, kTs + 0x74, 32);
    CHECK(a.call("_cellSpursSendSignal", kTs, 0) == kTaskInval);
    ps_write32(b, kTs + 0x74, 0);
    CHECK(a.call("_cellSpursSendSignal", kTs, 37) == kTaskSrch);

    // Task 37 (word 1, bit 5), enabled and waiting, as the taskset policy leaves a task that waits for a signal: its
    // signalled bit, the workload's signal, SPURS woken (handler dirty), in one update that a guest reservation on the
    // line sees.
    constexpr u32 kWord = 4, kBit = 0x80000000u >> 5;
    const auto set = [&](u32 bitset, u32 v) { ps_write32(b, kTs + bitset + kWord, v); };
    const auto get = [&](u32 bitset) { return ps_read32(b, kTs + bitset + kWord); };
    set(0x30, kBit);
    set(0x50, kBit);
    ps_write8(b, kSpurs + 0xD64, 0);
    u8 snapshot[PS_RES_LINE_SIZE];
    u64 version = ps_res_acquire(b, kTs, snapshot);
    REQUIRE(a.call("_cellSpursSendSignal", kTs, 37) == 0);
    CHECK_FALSE(ps_res_store_conditional(b, kTs + 0x40 + kWord, 4, 0, version, snapshot));
    CHECK(get(0x40) == kBit);
    CHECK(ps_read16(b, kSpurs + 0x70) == 0x8000); // wklSignal1, workload 0
    CHECK(ps_read8(b, kSpurs + 0xD64) == 1);
    // Signalled already: the bit stays and the workload gets no new signal.
    ps_write16(b, kSpurs + 0x70, 0);
    REQUIRE(a.call("_cellSpursSendSignal", kTs, 37) == 0);
    CHECK(get(0x40) == kBit);
    CHECK(ps_read16(b, kSpurs + 0x70) == 0);
    // Not waiting: only the bit.
    set(0x40, 0);
    set(0x50, 0);
    REQUIRE(a.call("_cellSpursSendSignal", kTs, 37) == 0);
    CHECK(get(0x40) == kBit);
    CHECK(ps_read16(b, kSpurs + 0x70) == 0);
    // Inconsistent bits (running and waiting): SRCH, nothing changed.
    set(0x40, 0);
    set(0x00, kBit);
    set(0x50, kBit);
    CHECK(a.call("_cellSpursSendSignal", kTs, 37) == kTaskSrch);
    CHECK(get(0x40) == 0);
    set(0x00, 0);
    // A workload that is not runnable: cellSpursSendWorkloadSignal refuses it (ignored), SPURS is woken: CELL_OK.
    ps_write8(b, kSpurs + 0x80, 3); // SPURS_WKL_STATE_SHUTTING_DOWN
    ps_write8(b, kSpurs + 0xD64, 0);
    REQUIRE(a.call("_cellSpursSendSignal", kTs, 37) == 0);
    CHECK(get(0x40) == kBit);
    CHECK(ps_read16(b, kSpurs + 0x70) == 0);
    CHECK(ps_read8(b, kSpurs + 0xD64) == 1);
    ps_write8(b, kSpurs + 0x80, 2);
    // SPURS in exception state: the bit is set, the workload signal refused, and cellSpursWakeUp's STAT becomes
    // TASK_ERROR_STAT.
    set(0x40, 0);
    ps_write32(b, kSpurs + 0xD6C, 1);
    CHECK(a.call("_cellSpursSendSignal", kTs, 37) == kTaskStat);
    CHECK(get(0x40) == kBit);
    CHECK(ps_read16(b, kSpurs + 0x70) == 0);
    ps_write32(b, kSpurs + 0xD6C, 0);

    // cellSpursAddUrgentCommand: NULL, ALIGN, an unreadable chain (INVAL, as cellSpursRunJobChain), a workload ID past
    // 31; then the first free slot of four, in order, as one reservation operation, and BUSY when all four are taken.
    REQUIRE(a.call("_cellSpursJobChainAttributeInitialize", 3, 0x330000, kJcAttr, kCmds, 0x80, 4, kPrio, 1, 1, 4, 5, 0,
                0x100, 0) == 0);
    REQUIRE(a.call("cellSpursCreateJobChainWithAttribute", kSpurs, kJc, kJcAttr) == 0);
    const u32 wid = ps_read32(b, kJc + 0x74);
    CHECK(a.call("cellSpursAddUrgentCommand", 0, 0x1000) == kJobNull);
    CHECK(a.call("cellSpursAddUrgentCommand", kJc + 0x40, 0x1000) == kJobAlign);
    CHECK(a.call("cellSpursAddUrgentCommand", 0x70000000, 0x1000) == kJobInval);
    ps_write32(b, kJc + 0x74, 32);
    CHECK(a.call("cellSpursAddUrgentCommand", kJc, 0x1000) == kJobInval);
    ps_write32(b, kJc + 0x74, wid);
    const u16 signals = ps_read16(b, kSpurs + 0x70);
    version = ps_res_acquire(b, kJc, snapshot);
    for (u32 k = 0; k < 4; k++) REQUIRE(a.call("cellSpursAddUrgentCommand", kJc, 0x1000 + 0x100 * k) == 0);
    CHECK_FALSE(ps_res_store_conditional(b, kJc + 0x30, 8, 0, version, snapshot));
    for (u32 k = 0; k < 4; k++) CHECK(ps_read64(b, kJc + 0x30 + 8 * k) == 0x1000 + 0x100 * k);
    CHECK(a.call("cellSpursAddUrgentCommand", kJc, 0x2000) == kJobBusy);
    // The policy takes slot 0 and moves the others up: the next command goes into the slot that freed.
    for (u32 k = 0; k < 3; k++) ps_write64(b, kJc + 0x30 + 8 * k, ps_read64(b, kJc + 0x38 + 8 * k));
    ps_write64(b, kJc + 0x48, 0);
    REQUIRE(a.call("cellSpursAddUrgentCommand", kJc, 0x2000) == 0);
    CHECK(ps_read64(b, kJc + 0x30) == 0x1100);
    CHECK(ps_read64(b, kJc + 0x48) == 0x2000);
    CHECK(ps_read16(b, kSpurs + 0x70) == signals); // nothing signalled: the chain's next visit takes the commands
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs (AC) with workers: a cellSpursCreateTaskset taskset runs a task that _cellSpursSendSignal resumes",
    "[hle][spurs][ac]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kPrio = kData + 0x4400, kTs = kData + 0x6000;
    constexpr u32 kId = kData + 0x8000, kPattern = kData + 0x8200, kCtx = kData + 0xA000, kElf = kData + 0x10000;
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 1, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    const u8 prio[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(b + kPrio, prio, 8);
    REQUIRE(a.call("cellSpursCreateTaskset", kSpurs, kTs, 0x1234, kPrio, 1) == 0);

    // The task: its stack in the top block, which its LS pattern keeps; it waits for a signal (the taskset policy's
    // syscall 2), then exits (syscall 0).
    write_task_elf(b, kElf,
        {as::ila(1, 0x3FFD0), as::il(3, 2), as::ila(5, 0xA70), as::bisl(0, 5), as::il(3, 0), as::ila(5, 0xA70),
            as::bisl(0, 5)});
    std::memset(b + kPattern, 0, 16);
    ps_write8(b, kPattern + 15, 1);
    REQUIRE(a.call("cellSpursCreateTask", kTs, kId, kElf, kCtx, 0xC00, kPattern, 0) == 0);
    const u32 id = ps_read32(b, kId);
    const auto task_bit = [&](u32 bitset) {
        return (ps_read32(b, kTs + bitset + 4 * (id / 32)) & (0x80000000u >> (id % 32))) != 0;
    };
    REQUIRE(wait_for([&] { return task_bit(0x50) && !task_bit(0x00); })); // waiting, its context saved

    REQUIRE(a.call("_cellSpursSendSignal", kTs, id) == 0);
    REQUIRE(wait_for([&] { return !task_bit(0x30); })); // resumed on a worker, then exited
    CHECK(a.call("_cellSpursSendSignal", kTs, id) == kTaskSrch); // gone
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("cellSpurs (AC) with workers: the job chain policy runs the urgent call cellSpursAddUrgentCommand added",
    "[hle][spurs][ac]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kJcAttr = kData + 0x4000;
    constexpr u32 kPrio = kData + 0x4400, kJc = kData + 0x5000, kGuard = kData + 0x5400, kCmds = kData + 0x6000;
    constexpr u32 kCalled = kData + 0x6800;
    REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 1, 200, 2016, 0) == 0);
    REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    std::memset(b + kPrio, 1, 8);
    REQUIRE(a.call("_cellSpursJobChainAttributeInitialize", 3, 0x330000, kJcAttr, kCmds, 0x80, 4, kPrio, 1, 1, 4, 5, 0,
                0x100, 0) == 0);
    REQUIRE(a.call("cellSpursCreateJobChainWithAttribute", kSpurs, kJc, kJcAttr) == 0);
    const u32 wid = ps_read32(b, kJc + 0x74);

    // The chain: END. The urgent call: a GUARD that passes (count 0; autoReset sets it back to 7), then RET.
    ps_write64(b, kCmds, 0x7F);
    std::memset(b + kGuard, 0, 0x80);
    ps_write32(b, kGuard + 0x04, 7); // the count autoReset restores
    ps_write32(b, kGuard + 0x10, 1); // requestSpuCount
    ps_write32(b, kGuard + 0x20, 1); // autoReset
    ps_write64(b, kCalled, kGuard | 0x0F); // GUARD
    ps_write64(b, kCalled + 8, 0x77);      // RET
    REQUIRE(a.call("cellSpursAddUrgentCommand", kJc, kCalled | 4) == 0); // CALL
    CHECK(ps_read64(b, kJc + 0x30) == (kCalled | 4));
    REQUIRE(a.call("cellSpursRunJobChain", kJc) == 0);

    // An SPU takes the urgent CALL from slot 0, passes the guard, returns, and leaves at the chain's END.
    REQUIRE(wait_for([&] {
        return ps_read32(b, kGuard) == 7 && ps_read64(b, kJc + 0x00) == kCmds && ps_read8(b, kJc + 0x29) == 0 &&
               ps_read8(b, kSpurs + 0x20 + wid) == 0 && !(ps_read16(b, kSpurs + 0x70) & (0x8000u >> wid));
    }));
    for (u32 k = 0; k < 4; k++) CHECK(ps_read64(b, kJc + 0x30 + 8 * k) == 0); // the urgent slots
    for (u32 k = 0; k < 3; k++) CHECK(ps_read64(b, kJc + 0x08 + 8 * k) == 0); // the call stack
    CHECK(ps_read8(b, kJc + 0x20) == 0);  // no urgent call active
    CHECK(ps_read32(b, kJc + 0x80) == 0); // no error
    REQUIRE(a.call("cellSpursFinalize", kSpurs) == 0);
}

TEST_CASE("sys_spinlock: initialize, lock, trylock and unlock on one guest word, through the reservation system",
    "[hle][spinlock][ac]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    constexpr u32 kLock = kData + 0x204; // a word inside a 128-byte line
    const u64 kBusy = r3_of(lv2::CELL_EBUSY);

    // initialize clears a word that is not 0 (RPCS3 stores only then).
    ps_write32(b, kLock, 0x12345678);
    a.call("sys_spinlock_initialize", kLock);
    CHECK(ps_read32(b, kLock) == 0);

    REQUIRE(a.call("sys_spinlock_trylock", kLock) == 0);
    CHECK(ps_read32(b, kLock) == 0xABADCAFE);
    CHECK(a.call("sys_spinlock_trylock", kLock) == kBusy); // not recursive
    a.call("sys_spinlock_unlock", kLock);
    CHECK(ps_read32(b, kLock) == 0);
    REQUIRE(a.call("sys_spinlock_lock", kLock) == 0);
    CHECK(ps_read32(b, kLock) == 0xABADCAFE);
    a.call("sys_spinlock_initialize", kLock);
    CHECK(ps_read32(b, kLock) == 0);

    // Reservations: a guest lwarx/stwcx. pair across an HLE lock or unlock fails, even an unlock of a free lock (the
    // store goes through the reservation system, not around it).
    u8 snapshot[PS_RES_LINE_SIZE];
    u64 version = ps_res_acquire(b, kLock, snapshot);
    REQUIRE(a.call("sys_spinlock_trylock", kLock) == 0);
    CHECK_FALSE(ps_res_store_conditional(b, kLock, 4, 1, version, snapshot));
    a.call("sys_spinlock_unlock", kLock);
    version = ps_res_acquire(b, kLock, snapshot);
    a.call("sys_spinlock_unlock", kLock);
    CHECK_FALSE(ps_res_store_conditional(b, kLock, 4, 1, version, snapshot));
    // A guest that took the lock with its own value: trylock is BUSY and leaves the word alone (RPCS3 exchanges only
    // over a word it read as 0).
    version = ps_res_acquire(b, kLock, snapshot);
    REQUIRE(ps_res_store_conditional(b, kLock, 4, 7, version, snapshot));
    CHECK(a.call("sys_spinlock_trylock", kLock) == kBusy);
    CHECK(ps_read32(b, kLock) == 7);
    a.call("sys_spinlock_unlock", kLock);
    CHECK(ps_read32(b, kLock) == 0);

    // A word that is not aligned, or not guest memory, is refused (D-042).
    const u64 kFault = r3_of(lv2::CELL_EFAULT);
    CHECK(a.call("sys_spinlock_lock", kLock + 2) == kFault);
    CHECK(a.call("sys_spinlock_trylock", 0x70000000) == kFault);
    ps_write32(b, kLock, 5);
    a.call("sys_spinlock_unlock", kLock + 1);
    a.call("sys_spinlock_initialize", kLock + 1);
    CHECK(ps_read32(b, kLock) == 5);
}

TEST_CASE("sys_spinlock: the lock excludes other host threads; a waiter sleeps, then gets it on unlock",
    "[hle][spinlock][ac]")
{
    Fixture f;
    u8* b = f.base();
    constexpr u32 kLock = kData + 0x300, kCounter = kData + 0x400, kInside = kData + 0x480;
    const hle::Function& lock = GuestThread::find("sys_spinlock_lock");
    const hle::Function& unlock = GuestThread::find("sys_spinlock_unlock");

    // Four threads add to a plain guest counter under the lock.
    constexpr u32 kThreads = 4, kRounds = 1000;
    std::atomic<u32> failures{0}, overlaps{0};
    std::vector<std::thread> threads;
    for (u32 i = 0; i < kThreads; i++) {
        threads.emplace_back([&, i] {
            GuestThread g(f, i, "spin");
            for (u32 r = 0; r < kRounds; r++) {
                if (call1(g, lock, kLock) != 0) failures++;
                if (ps_read32(b, kInside) != 0) overlaps++;
                ps_write32(b, kInside, 1);
                ps_write32(b, kCounter, ps_read32(b, kCounter) + 1);
                if (r % 64 == 0) std::this_thread::yield(); // hold it across a host yield now and then
                ps_write32(b, kInside, 0);
                call1(g, unlock, kLock);
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(failures == 0);
    CHECK(overlaps == 0);
    CHECK(ps_read32(b, kCounter) == kThreads * kRounds);
    CHECK(ps_read32(b, kLock) == 0);

    // A waiter on a held lock spins briefly, then sleeps between looks (lv2's yield, D-104); it gets the lock on
    // unlock.
    GuestThread a(f, 0, "holder");
    REQUIRE(a.call("sys_spinlock_lock", kLock) == 0);
    std::atomic<lv2::PpuThread*> waiter{nullptr};
    std::atomic<u64> result{~0ull};
    std::thread tb([&] {
        GuestThread g(f, 1, "waiter");
        waiter = g.t;
        result = call1(g, lock, kLock);
        call1(g, unlock, kLock);
        waiter = nullptr;
    });
    REQUIRE(wait_for([&] { return waiter.load() != nullptr; }));
    bool slept = false;
    for (const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
         !slept && std::chrono::steady_clock::now() < end;) {
        if (lv2::PpuThread* t = waiter.load(); t && t->waiter.waiting_on().kind == lv2::WaitKind::Sleep) slept = true;
    }
    CHECK(slept);
    CHECK(result == ~0ull); // still waiting
    a.call("sys_spinlock_unlock", kLock);
    tb.join();
    CHECK(result == 0);
    CHECK(ps_read32(b, kLock) == 0);
}

TEST_CASE("sys_spinlock: a thread waiting on a held lock unwinds when the process ends", "[hle][spinlock][ac]")
{
    Fixture f;
    GuestThread a(f, 0, "holder");
    constexpr u32 kLock = kData + 0x300;
    REQUIRE(a.call("sys_spinlock_lock", kLock) == 0);
    const hle::Function& lock = GuestThread::find("sys_spinlock_lock");
    std::atomic<int> outcome{0}; // 1: it got the lock, 2: it unwound
    std::thread tb([&] {
        GuestThread g(f, 1, "waiter");
        try {
            call1(g, lock, kLock);
            outcome = 1;
        } catch (const lv2::ThreadExit&) {
            outcome = 2;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(outcome == 0);
    CHECK_THROWS_AS(lv2::sys_process_exit3(*a.t, 0), lv2::ThreadExit);
    tb.join();
    CHECK(outcome == 2);
}

TEST_CASE("sys_mmapper wrappers: allocate, map, unmap and free shared memory through lv2", "[hle][mmapper][ac]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    constexpr u32 kArea = kData + 0x100, kMemId = kData + 0x104, kGot = kData + 0x108;
    constexpr u64 k1M = 0x400, k64K = 0x200;
    REQUIRE(lv2::sys_mmapper_allocate_address(*a.t, 0x10000000, k1M, 0x10000000, kArea) == lv2::CELL_OK);
    const u32 area = ps_read32(b, kArea);

    // lv2's checks: a size of 0 or not whole pages, an unknown page size.
    CHECK(a.call("sys_mmapper_allocate_memory", 0, k1M, kMemId) == r3_of(lv2::CELL_EALIGN));
    CHECK(a.call("sys_mmapper_allocate_memory", 0x10000, k1M, kMemId) == r3_of(lv2::CELL_EALIGN));
    CHECK(a.call("sys_mmapper_allocate_memory", 0x100000, 0x300, kMemId) == r3_of(lv2::CELL_EINVAL));
    REQUIRE(a.call("sys_mmapper_allocate_memory", 0x200000, k1M, kMemId) == 0);
    const u32 mem = ps_read32(b, kMemId);
    CHECK((mem >> 24) == 0x08); // a memory object ID
    // No IPC key: a second allocation of the same size is another object.
    REQUIRE(a.call("sys_mmapper_allocate_memory", 0x200000, k1M, kMemId) == 0);
    const u32 other = ps_read32(b, kMemId);
    CHECK(other != mem);
    REQUIRE(a.call("sys_mmapper_free_memory", other) == 0);

    CHECK(a.call("sys_mmapper_map_memory", 0x70000000, mem, 0) == r3_of(lv2::CELL_EINVAL)); // not an mmapper area
    CHECK(a.call("sys_mmapper_map_memory", area, other, 0) == r3_of(lv2::CELL_ESRCH));      // freed
    REQUIRE(a.call("sys_mmapper_map_memory", area, mem, 0) == 0);
    CHECK(f.mem.is_committed(area));
    CHECK(f.mem.is_committed(area + 0x1FFFFF));
    ps_write64(b, area + 0x1000, 0x0123456789ABCDEFull);
    CHECK(a.call("sys_mmapper_free_memory", mem) == r3_of(lv2::CELL_EBUSY)); // still mapped
    // A second mapping of the object shows the same memory.
    REQUIRE(a.call("sys_mmapper_map_memory", area + 0x200000, mem, 0) == 0);
    CHECK(ps_read64(b, area + 0x201000) == 0x0123456789ABCDEFull);

    REQUIRE(a.call("sys_mmapper_unmap_memory", area + 0x200000, kGot) == 0);
    CHECK(ps_read32(b, kGot) == mem);
    REQUIRE(a.call("sys_mmapper_unmap_memory", area, kGot) == 0);
    CHECK(ps_read32(b, kGot) == mem);
    CHECK_FALSE(f.mem.is_committed(area));
    CHECK(a.call("sys_mmapper_unmap_memory", area, kGot) == r3_of(lv2::CELL_EINVAL));
    REQUIRE(a.call("sys_mmapper_free_memory", mem) == 0);
    CHECK(a.call("sys_mmapper_free_memory", mem) == r3_of(lv2::CELL_ESRCH));

    // A 64 KB-page object does not go into a 1 MB-page area.
    REQUIRE(a.call("sys_mmapper_allocate_memory", 0x10000, k64K, kMemId) == 0);
    const u32 small = ps_read32(b, kMemId);
    CHECK(a.call("sys_mmapper_map_memory", area, small, 0) == r3_of(lv2::CELL_EINVAL));
    REQUIRE(a.call("sys_mmapper_free_memory", small) == 0);
    REQUIRE(lv2::sys_mmapper_free_address(*a.t, area) == lv2::CELL_OK);
}

TEST_CASE("sys_process_is_stack and sys_spu_image_close", "[hle][ac]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();

    // The PPU stack area: addresses whose top four bits are 0xD.
    CHECK(a.call("sys_process_is_stack", PS_MEM_STACK_BASE) == 1);
    CHECK(a.call("sys_process_is_stack", PS_MEM_STACK_BASE + PS_MEM_STACK_SIZE - 1) == 1);
    CHECK(a.call("sys_process_is_stack", PS_MEM_STACK_BASE - 1) == 0);
    CHECK(a.call("sys_process_is_stack", PS_MEM_SPU_BASE) == 0);
    CHECK(a.call("sys_process_is_stack", kData) == 0);

    // sys_spu_image {type, entry_point, segs, nsegs}: a user image's segment table is freed in the main area.
    constexpr u32 kImg = kData + 0x500;
    const u32 segs = f.hle->main_memory().alloc(0x100);
    REQUIRE(segs != 0);
    ps_write32(b, kImg + 0x0, 0); // SYS_SPU_IMAGE_TYPE_USER
    ps_write32(b, kImg + 0x4, 0x3000);
    ps_write32(b, kImg + 0x8, segs);
    ps_write32(b, kImg + 0xC, 2);
    REQUIRE(a.call("sys_spu_image_close", kImg) == 0);
    CHECK_FALSE(f.mem.is_committed(segs));
    // A kernel image goes to the syscall _sys_spu_image_close (158), which lv2 does not implement: reported, ENOSYS.
    ps_write32(b, kImg + 0x0, 1); // SYS_SPU_IMAGE_TYPE_KERNEL
    ps_write32(b, kImg + 0x4, 0x22000001);
    const u64 unimplemented = f.kernel->unimplemented_count();
    CHECK(a.call("sys_spu_image_close", kImg) == r3_of(lv2::CELL_ENOSYS));
    CHECK(f.kernel->unimplemented_count() == unimplemented + 1);
    // Any other type: EINVAL; an image that is not guest memory: EFAULT (D-042).
    ps_write32(b, kImg + 0x0, 2);
    CHECK(a.call("sys_spu_image_close", kImg) == r3_of(lv2::CELL_EINVAL));
    CHECK(a.call("sys_spu_image_close", 0x70000000) == r3_of(lv2::CELL_EFAULT));
}

TEST_CASE("sys_game_process_exitspawn: RPCS3's parameter block, the request logged, then the process ends",
    "[hle][ac]")
{
    constexpr u32 kPath = kData + 0x600, kArgv = kData + 0x700, kEnvp = kData + 0x720, kStrings = kData + 0x740;
    constexpr u32 kBlob = kData + 0x800, kManyArgv = kData + 0x1000, kManyStrings = kData + 0x2000;
    const std::string path = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"; // 35 characters
    const auto put_string = [](u8* b, u32 at, std::string_view s) {
        std::memcpy(b + at, s.data(), s.size());
        b[at + s.size()] = 0;
    };
    // The block's address, from the error line that reports the request.
    const auto block_of = [](LogLines& logs) {
        const std::string line = logs.find(log::Level::Error, "sysPrxForUser", "sys_game_process_exitspawn(");
        const auto at = line.find("parameter block ");
        REQUIRE(at != std::string::npos);
        return static_cast<u32>(std::stoul(line.substr(at + 16), nullptr, 16));
    };

    {
        LogLines logs;
        Fixture f;
        GuestThread a(f, 0, "a");
        u8* b = f.base();
        put_string(b, kPath, path);
        put_string(b, kStrings + 0x00, "-arg1");
        put_string(b, kStrings + 0x10, "x");
        put_string(b, kStrings + 0x20, "E=1");
        ps_write32(b, kArgv + 0, kStrings);
        ps_write32(b, kArgv + 4, kStrings + 0x10);
        ps_write32(b, kArgv + 8, 0);
        ps_write32(b, kEnvp + 0, kStrings + 0x20);
        ps_write32(b, kEnvp + 4, 0);
        for (u32 i = 0; i < 0x20; i++) b[kBlob + i] = static_cast<u8>(0xA0 + i);

        CHECK_THROWS_AS(a.call("sys_game_process_exitspawn", kPath, kArgv, kEnvp, kBlob, 0x20, 1001, 0x12345),
            lv2::ThreadExit);
        CHECK(f.kernel->exited());
        CHECK(f.kernel->exit_status() == 0);
        CHECK(logs.count(log::Level::Error, "sysPrxForUser",
                  "sys_game_process_exitspawn(path \"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN\", argv [\"-arg1\", \"x\"], "
                  "envp [\"E=1\"], data") == 1);

        // sys_exit2_param: 0x85, 0x30, the size after it (strings 0x90 + data 0x1030), prio, (flags & 0xf0) | 1 << 63,
        // the strings' EA.
        const u32 block = block_of(logs);
        REQUIRE(block != 0);
        constexpr u32 kSize = 0x30 + 0x90 + 0x1030;
        CHECK(ps_read64(b, block + 0x00) == 0x85);
        CHECK(ps_read64(b, block + 0x08) == 0x30);
        CHECK(ps_read64(b, block + 0x10) == kSize - 0x30);
        CHECK(ps_read64(b, block + 0x18) == 1001);
        CHECK(ps_read64(b, block + 0x20) == (0x40 | (1ull << 63)));
        CHECK(ps_read64(b, block + 0x28) == block + 0x30);
        // The pointers (path, argv, 0, envp, 0; six slots, even), then the strings 16-byte rounded.
        const u32 out = block + 0x30;
        CHECK(ps_read64(b, out + 0x00) == out + 0x30);
        CHECK(ps_read64(b, out + 0x08) == out + 0x60);
        CHECK(ps_read64(b, out + 0x10) == out + 0x70);
        CHECK(ps_read64(b, out + 0x18) == 0);
        CHECK(ps_read64(b, out + 0x20) == out + 0x80);
        CHECK(ps_read64(b, out + 0x28) == 0);
        CHECK(std::string(reinterpret_cast<const char*>(b + out + 0x30)) == path);
        CHECK(std::string(reinterpret_cast<const char*>(b + out + 0x60)) == "-arg1");
        CHECK(std::string(reinterpret_cast<const char*>(b + out + 0x70)) == "x");
        CHECK(std::string(reinterpret_cast<const char*>(b + out + 0x80)) == "E=1");
        // The game's data in the last 0x1000 bytes.
        CHECK(std::memcmp(b + block + kSize - 0x1000, b + kBlob, 0x20) == 0);
        CHECK(logs.count(log::Level::Warn, "lv2", "the next program is not launched") == 1);
    }
    {
        // Argument strings that would make the block pass 0x1000 bytes are left out, with the environment.
        LogLines logs;
        Fixture f;
        GuestThread a(f, 0, "a");
        u8* b = f.base();
        put_string(b, kPath, path);
        for (u32 i = 0; i < 200; i++) {
            put_string(b, kManyStrings + 0x10 * i, "argument");
            ps_write32(b, kManyArgv + 4 * i, kManyStrings + 0x10 * i);
        }
        ps_write32(b, kManyArgv + 4 * 200, 0);
        CHECK_THROWS_AS(a.call("sys_game_process_exitspawn", kPath, kManyArgv, 0, 0, 0, 1001, 0), lv2::ThreadExit);
        CHECK(f.kernel->exited());
        CHECK(logs.count(log::Level::Error, "sysPrxForUser", "argv and envp left out") == 1);
        const u32 block = block_of(logs);
        REQUIRE(block != 0);
        // The path's 0x30 + 8, the two terminators, 8 for an odd count, the header; no data.
        CHECK(ps_read64(b, block + 0x10) == 0x30 + 8 + 8 + 8 + 8);
        CHECK(ps_read64(b, block + 0x20) == (1ull << 63));
        const u32 out = block + 0x30;
        CHECK(ps_read64(b, out + 0x00) == out + 0x20);
        CHECK(ps_read64(b, out + 0x08) == 0);
        CHECK(ps_read64(b, out + 0x10) == 0);
        CHECK(std::string(reinterpret_cast<const char*>(b + out + 0x20)) == path);
    }
}
