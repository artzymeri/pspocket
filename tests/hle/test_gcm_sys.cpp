// cellGcmSys complete, as RPCS3 has it (D-170): every function RPCS3's cellGcmSys.cpp implements, each against RPCS3's
// behaviour and error codes, over lv2 sys_rsx and the RSX thread. Synthetic guest memory and guest code only (no game
// data, CLAUDE.md rule 1).

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "internal.hpp" // runtime/lv2/src (white-box): the RSX device's Z-cull regions and display sync
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/rsx.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kIo = 0x00200000;   // the "game's" IO buffer: 1 MB of main memory mapped at IO offset 0
constexpr u32 kCtxPtr = kData;    // CellGcmContextData* written by _cellGcmInitBody
constexpr u32 kOut = kData + 0x100;
constexpr u32 kCmdSize = 0x40000; // 8 segments of 32 KB
constexpr u32 kBadPointer = 0x70000000;
constexpr u64 kSecond = 1'000'000'000ull;
constexpr u32 CELL_GCM_ERROR_FAILURE = 0x802100ff, CELL_GCM_ERROR_NO_IO_PAGE_TABLE = 0x80210001,
              CELL_GCM_ERROR_INVALID_VALUE = 0x80210003, CELL_GCM_ERROR_INVALID_ALIGNMENT = 0x80210004;
const u64 kFailure = r3_of_code(CELL_GCM_ERROR_FAILURE);
const u64 kLv1IllegalParameter = r3_of_code(static_cast<u32>(-17)); // lv2's LV1_ILLEGAL_PARAMETER_VALUE
const u64 kEinval = r3_of(lv2::CELL_EINVAL);
// RsxDriverInfo (RPCS3 sys_rsx.h): driver information = DMA control + 1 MB = control register - 0x40 + 1 MB.
constexpr u32 kHead0 = 0x10B8, kHead1 = 0x10B8 + 0x40, kFlipFlags = 0x8, kLastQueuedBufferId = 0x14;

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

// Guest code the HLE calls back into (handlers, a context's callback) runs on the PPU interpreter while this lives.
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

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

    usize count(log::Level level, std::string_view channel, std::string_view text)
    {
        std::lock_guard lock(mutex);
        usize n = 0;
        for (const Line& l : lines)
            if (l.level == level && l.channel == channel && l.message.find(text) != std::string::npos) n++;
        return n;
    }
};

// The game's first flush after cellGcmInit: put at current, the default command buffer's first command, so the commands
// written next run once put moves past them (cellGcmSetFlip adds to put, as RPCS3's).
void flush_to_current(Fixture& f, GuestThread& g)
{
    u8* b = f.base();
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    ps_write32(b, control, ps_read32(b, ps_read32(b, kCtxPtr) + 0x8) - kIo);
    f.kernel->rsx()->notify();
    REQUIRE(f.kernel->rsx()->wait_idle(2 * kSecond));
}

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A guest function at `code` with its descriptor at `opd`: *seq += 1, then slot[0] = *seq and slot[1] = r3. The
// handlers of one test share `seq`, so their slots show the order they ran in.
void write_sequence_function(u8* b, u32 code, u32 opd, u32 seq, u32 slot)
{
    const u32 words[] = {
        0x3CA00000u | (seq >> 16),     // lis r5, seq@h
        0x60A50000u | (seq & 0xFFFF),  // ori r5, r5, seq@l
        0x80850000u,                   // lwz r4, 0(r5)
        0x38840001u,                   // addi r4, r4, 1
        0x90850000u,                   // stw r4, 0(r5)
        0x3CC00000u | (slot >> 16),    // lis r6, slot@h
        0x60C60000u | (slot & 0xFFFF), // ori r6, r6, slot@l
        0x90860000u,                   // stw r4, 0(r6)
        0x90660004u,                   // stw r3, 4(r6)
        0x4E800020u,                   // blr
    };
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, code + 4 * i, words[i]);
    ps_write32(b, opd, code);
    ps_write32(b, opd + 4, 0);
}

// Context callbacks for a game's own context: one that fails with -1, one that returns 5 (both make no room).
constexpr u32 kFailNeg = kData + 0x3000, kFailPos = kData + 0x3010, kOpdNeg = kData + 0x3100, kOpdPos = kData + 0x3108;

void write_failing_callbacks(u8* b)
{
    ps_write32(b, kFailNeg, 0x3860FFFF);     // li r3,-1
    ps_write32(b, kFailNeg + 4, 0x4E800020); // blr
    ps_write32(b, kFailPos, 0x38600005);     // li r3,5
    ps_write32(b, kFailPos + 4, 0x4E800020); // blr
    ps_write32(b, kOpdNeg, kFailNeg);
    ps_write32(b, kOpdNeg + 4, 0);
    ps_write32(b, kOpdPos, kFailPos);
    ps_write32(b, kOpdPos + 4, 0);
}

} // namespace

TEST_CASE("cellGcmSys (D-170): every function RPCS3 implements is registered under its NID", "[hle][gcm]")
{
    for (const char* name : {"_cellGcmInitBody", "_cellGcmFunc12", "_cellGcmFunc15", "_cellGcmSetFlipCommand",
             "_cellGcmSetFlipCommandWithWaitLabel", "cellGcmAddressToOffset", "cellGcmBindTile", "cellGcmBindZcull",
             "cellGcmGetConfiguration", "cellGcmGetControlRegister", "cellGcmGetCurrentDisplayBufferId",
             "cellGcmGetDefaultCommandWordSize", "cellGcmGetDefaultSegmentWordSize", "cellGcmGetDisplayInfo",
             "cellGcmGetFlipStatus", "cellGcmGetLabelAddress", "cellGcmGetLastFlipTime", "cellGcmGetMaxIoMapSize",
             "cellGcmGetNotifyDataAddress", "cellGcmGetOffsetTable", "cellGcmGetReport", "cellGcmGetReportDataAddress",
             "cellGcmGetReportDataAddressLocation", "cellGcmGetReportDataLocation", "cellGcmGetTileInfo",
             "cellGcmGetTiledPitchSize", "cellGcmGetTimeStamp", "cellGcmGetTimeStampLocation", "cellGcmGetVBlankCount",
             "cellGcmGetZcullInfo", "cellGcmInitDefaultFifoMode", "cellGcmInitSystemMode", "cellGcmIoOffsetToAddress",
             "cellGcmMapEaIoAddress", "cellGcmMapEaIoAddressWithFlags", "cellGcmMapLocalMemory",
             "cellGcmMapMainMemory", "cellGcmReserveIoMapSize", "cellGcmResetFlipStatus", "cellGcmSetDebugOutputLevel",
             "cellGcmSetDefaultCommandBuffer", "cellGcmSetDefaultCommandBufferAndSegmentWordSize",
             "cellGcmSetDefaultFifoSize", "cellGcmSetDisplayBuffer", "cellGcmSetFlip", "cellGcmSetFlipHandler",
             "cellGcmSetFlipImmediate", "cellGcmSetFlipMode", "cellGcmSetFlipStatus", "cellGcmSetPrepareFlip",
             "cellGcmSetQueueHandler", "cellGcmSetSecondVFrequency", "cellGcmSetTile", "cellGcmSetTileInfo",
             "cellGcmSetUserCommand", "cellGcmSetUserHandler", "cellGcmSetVBlankHandler", "cellGcmSetWaitFlip",
             "cellGcmSetWaitFlipUnsafe", "cellGcmSetZcull", "cellGcmTerminate", "cellGcmUnbindTile",
             "cellGcmUnbindZcull", "cellGcmUnmapEaIoAddress", "cellGcmUnmapIoAddress", "cellGcmUnreserveIoMapSize"}) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellGcmSys", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
}

TEST_CASE("cellGcmSys: before cellGcmInit, as RPCS3; cellGcmMapLocalMemory maps local memory once", "[hle][gcm]")
{
    LogLines logs;
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();

    // The IO address space is 0 until cellGcmInit (RPCS3's renderer): nothing to reserve or map.
    CHECK(g.call("cellGcmGetMaxIoMapSize") == 0);
    CHECK(g.call("cellGcmReserveIoMapSize", 0x1234) == r3_of_code(CELL_GCM_ERROR_INVALID_ALIGNMENT));
    CHECK(g.call("cellGcmReserveIoMapSize", 0x100000) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    CHECK(g.call("cellGcmReserveIoMapSize", 0) == 0);
    CHECK(g.call("cellGcmMapMainMemory", kIo, 0x100000, kOut) == r3_of_code(CELL_GCM_ERROR_NO_IO_PAGE_TABLE));
    CHECK(g.call("cellGcmMapEaIoAddress", kIo, 0, 0x100000) == kEinval); // lv2 has no RSX context
    CHECK(g.call("cellGcmUnmapIoAddress", 0) == kFailure);
    CHECK(g.call("cellGcmUnmapEaIoAddress", kIo) == kFailure);
    CHECK(g.call("cellGcmGetNotifyDataAddress", 0) == 0);

    // No arrays, no offset table yet: 0.
    CHECK(g.call("cellGcmGetDisplayInfo") == 0);
    CHECK(g.call("cellGcmGetTileInfo") == 0);
    CHECK(g.call("cellGcmGetZcullInfo") == 0);
    ps_write32(b, kOut, 0xDEADBEEF);
    ps_write32(b, kOut + 4, 0xDEADBEEF);
    g.call("cellGcmGetOffsetTable", kOut);
    CHECK(ps_read32(b, kOut) == 0);
    CHECK(ps_read32(b, kOut + 4) == 0);

    // RPCS3's initial flip state: done, buffer 0, no vblanks; the default sizes.
    CHECK(g.call("cellGcmGetFlipStatus") == 0);
    CHECK(g.call("cellGcmGetLastFlipTime") == 0);
    CHECK(g.call("cellGcmGetVBlankCount") == 0);
    ps_write8(b, kOut, 0xAA);
    CHECK(g.call("cellGcmGetCurrentDisplayBufferId", kOut) == 0);
    CHECK(ps_read8(b, kOut) == 0);
    CHECK(g.call("cellGcmGetDefaultCommandWordSize") == 0x400);
    CHECK(g.call("cellGcmGetDefaultSegmentWordSize") == 0x100);

    // What needs the RSX context or the default context is refused or logged.
    CHECK(g.call("cellGcmSetDefaultCommandBufferAndSegmentWordSize", 0x800, 0x400) == kFailure);
    ps_write32(b, kCtxPtr, 0x12345678);
    g.call("cellGcmSetDefaultCommandBuffer");
    CHECK(ps_read32(b, kCtxPtr) == 0x12345678);
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "cellGcmSetDefaultCommandBuffer before cellGcmInit") == 1);
    g.call("cellGcmResetFlipStatus");
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "cellGcmResetFlipStatus: lv2 refused") == 1);
    CHECK(g.call("cellGcmGetFlipStatus") == 0);
    CHECK(g.call("cellGcmBindZcull", 0, 0x300000, 1280, 720, 0, 2, 0, 0, 1, 0, 0x80, 0xff) == kEinval);
    CHECK(g.call("cellGcmUnbindZcull", 8) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    g.call("cellGcmSetQueueHandler", kData + 0x1000);
    CHECK(logs.count(log::Level::Warn, "cellGcmSys", "cellGcmSetQueueHandler(0x101000) before cellGcmInit") == 1);

    // cellGcmMapLocalMemory: once, before cellGcmInit, whose local memory it then is; its pointers are checked.
    CHECK(g.call("cellGcmMapLocalMemory", kOut, kBadPointer) == kFailure);
    CHECK_FALSE(f.mem.is_committed(PS_MEM_RSX_LOCAL_BASE));
    REQUIRE(g.call("cellGcmMapLocalMemory", kOut, kOut + 4) == 0);
    CHECK(ps_read32(b, kOut) == PS_MEM_RSX_LOCAL_BASE);
    CHECK(ps_read32(b, kOut + 4) == 0x0F900000);
    CHECK(f.mem.is_committed(PS_MEM_RSX_LOCAL_BASE + 0x0F8FFFFF));
    CHECK(g.call("cellGcmMapLocalMemory", kOut, kOut + 4) == kFailure);
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    CHECK(g.call("cellGcmMapLocalMemory", kOut, kOut + 4) == kFailure);
    g.call("cellGcmGetConfiguration", kOut);
    CHECK(ps_read32(b, kOut + 0x00) == PS_MEM_RSX_LOCAL_BASE);
    CHECK(ps_read32(b, kOut + 0x08) == 0x0F900000);
}

TEST_CASE("cellGcmSys: cellGcmInit's own local memory is not mapped twice", "[hle][gcm]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    CHECK(g.call("cellGcmMapLocalMemory", kOut, kOut + 4) == kFailure);
}

TEST_CASE("cellGcmSys: IO mapping as RPCS3: the offset table, first fit, reserves, unmapping", "[hle][gcm]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    REQUIRE(f.mem.commit(0x04000000, 0x1000000)); // 16 MB more main memory: EA MBs 0x40..0x4F
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    rsx::Rsx* rsx = f.kernel->rsx();
    REQUIRE(rsx != nullptr);

    // CellGcmOffsetTable: ioAddress[3072] by EA MB, then eaAddress[512] by IO MB.
    g.call("cellGcmGetOffsetTable", kOut);
    const u32 io_table = ps_read32(b, kOut), ea_table = ps_read32(b, kOut + 4);
    REQUIRE(io_table != 0);
    CHECK(ea_table == io_table + 3072 * 2);
    const auto io_of_ea_mb = [&](u32 mb) { return ps_read16(b, io_table + 2 * mb); };
    const auto ea_of_io_mb = [&](u32 mb) { return ps_read16(b, ea_table + 2 * mb); };
    CHECK(io_of_ea_mb(kIo >> 20) == 0); // cellGcmInit's command buffer at IO 0
    CHECK(ea_of_io_mb(0) == kIo >> 20);
    CHECK(ea_of_io_mb(1) == 0xFFFF);
    CHECK(g.call("cellGcmGetMaxIoMapSize") == 0x10000000);

    // cellGcmMapEaIoAddress: RPCS3's checks (whole MBs, a size), then lv2's (memory that is there, the IO space).
    const auto map = [&](u32 ea, u32 io, u32 size) { return g.call("cellGcmMapEaIoAddress", ea, io, size); };
    CHECK(map(0x04000000, 0x00100000, 0) == kFailure);
    CHECK(map(0x04000800, 0x00100000, 0x100000) == kFailure);
    CHECK(map(0x04000000, 0x00100800, 0x100000) == kFailure);
    CHECK(map(0x04000000, 0x00100000, 0x100800) == kFailure);
    CHECK(map(0x08000000, 0x00100000, 0x100000) == kEinval); // not committed
    CHECK(map(0x04000000, 0x0FF00000, 0x200000) == kEinval); // past the 256 MB IO space
    CHECK(map(0xC0000000, 0x00100000, 0x100000) == kFailure); // past the offset table (RPCS3 writes past it)
    CHECK(ea_of_io_mb(1) == 0xFFFF);
    REQUIRE(map(0x04200000, 0x00400000, 0x100000) == 0);
    CHECK(io_of_ea_mb(0x42) == 4);
    CHECK(ea_of_io_mb(4) == 0x42);
    REQUIRE(g.call("cellGcmAddressToOffset", 0x04200010, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00400010);
    REQUIRE(g.call("cellGcmIoOffsetToAddress", 0x00400010, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x04200010);

    // cellGcmMapMainMemory: the first IO pages free for the size, from 0; *offset gets the IO offset.
    REQUIRE(g.call("cellGcmMapMainMemory", 0x04000000, 0x200000, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00100000); // MBs 1-2 (0 is the command buffer)
    REQUIRE(g.call("cellGcmMapMainMemory", 0x04300000, 0x200000, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00500000); // MB 3 alone is too small and 4 is taken
    REQUIRE(g.call("cellGcmMapMainMemory", 0x04500000, 0x100000, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00300000); // the hole
    CHECK(rsx->io_map().io_to_ea(0x00500010) == 0x04300010);
    CHECK(io_of_ea_mb(0x44) == 6);
    CHECK(ea_of_io_mb(6) == 0x44);
    CHECK(g.call("cellGcmMapMainMemory", 0x04600000, 0, kOut) == kFailure);
    CHECK(g.call("cellGcmMapMainMemory", 0x04600800, 0x100000, kOut) == kFailure);
    CHECK(g.call("cellGcmMapMainMemory", 0x04600000, 0x100800, kOut) == kFailure);
    CHECK(g.call("cellGcmMapMainMemory", 0x04600000, 0x100000, kBadPointer) == kFailure);
    CHECK(ea_of_io_mb(7) == 0xFFFF);

    // Reserving the top of the IO space: the maximum shrinks and cellGcmMapMainMemory stays below it.
    CHECK(g.call("cellGcmReserveIoMapSize", 0x0F900001) == r3_of_code(CELL_GCM_ERROR_INVALID_ALIGNMENT));
    CHECK(g.call("cellGcmReserveIoMapSize", 0x10100000) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    REQUIRE(g.call("cellGcmReserveIoMapSize", 0x0F900000) == 0); // 7 MB left: MBs 0..6, all taken
    CHECK(g.call("cellGcmGetMaxIoMapSize") == 0x00700000);
    CHECK(g.call("cellGcmMapMainMemory", 0x04600000, 0x100000, kOut) == r3_of_code(CELL_GCM_ERROR_NO_IO_PAGE_TABLE));
    CHECK(g.call("cellGcmReserveIoMapSize", 0x00800000) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    CHECK(g.call("cellGcmUnreserveIoMapSize", 0x00100001) == r3_of_code(CELL_GCM_ERROR_INVALID_ALIGNMENT));
    CHECK(g.call("cellGcmUnreserveIoMapSize", 0x0FA00000) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    REQUIRE(g.call("cellGcmUnreserveIoMapSize", 0x0F900000) == 0);
    CHECK(g.call("cellGcmGetMaxIoMapSize") == 0x10000000);

    // Unmapping by IO offset: only where a mapping starts; it goes from lv2's map and both halves of the table.
    CHECK(g.call("cellGcmUnmapIoAddress", 0x00200000) == kFailure); // the second MB of a mapping
    REQUIRE(g.call("cellGcmUnmapIoAddress", 0x00100000) == 0);
    CHECK(io_of_ea_mb(0x40) == 0xFFFF);
    CHECK(io_of_ea_mb(0x41) == 0xFFFF);
    CHECK(ea_of_io_mb(1) == 0xFFFF);
    CHECK(ea_of_io_mb(2) == 0xFFFF);
    CHECK_FALSE(rsx->io_map().io_to_ea(0x00100000));
    CHECK_FALSE(rsx->io_map().io_to_ea(0x00200000));
    CHECK(g.call("cellGcmAddressToOffset", 0x04000000, kOut) == kFailure);
    CHECK(g.call("cellGcmUnmapIoAddress", 0x00100000) == kFailure); // gone already
    CHECK(g.call("cellGcmUnmapIoAddress", 0x00700000) == kFailure); // never mapped
    CHECK(g.call("cellGcmUnmapIoAddress", 0x20000000) == kFailure); // past the IO space

    // By EA: the mapping of the EA's MB, which must be its first (the low bits are ignored); 3 GB on is refused.
    CHECK(g.call("cellGcmUnmapEaIoAddress", 0x04400000) == kFailure); // the second MB of 0x04300000's
    REQUIRE(g.call("cellGcmUnmapEaIoAddress", 0x04200000 + 0x1234) == 0);
    CHECK(io_of_ea_mb(0x42) == 0xFFFF);
    CHECK(ea_of_io_mb(4) == 0xFFFF);
    CHECK_FALSE(rsx->io_map().io_to_ea(0x00400000));
    CHECK(g.call("cellGcmUnmapEaIoAddress", 0x04200000) == kFailure);
    CHECK(g.call("cellGcmUnmapEaIoAddress", 0xC0000000) == kFailure);
    CHECK(g.call("cellGcmUnmapEaIoAddress", 0x04600000) == kFailure); // never mapped

    // The pages freed are the first fit again: MBs 1-2 and 4 are free, 3 MB first fit from 7.
    REQUIRE(g.call("cellGcmMapMainMemory", 0x04600000, 0x300000, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00700000);
    REQUIRE(g.call("cellGcmMapMainMemory", 0x04900000, 0x200000, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00100000);
    REQUIRE(g.call("cellGcmMapMainMemory", 0x04B00000, 0x100000, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00400000);

    // cellGcmGetNotifyDataAddress: 0x40-byte entries in what IO offset 241 MB maps.
    CHECK(g.call("cellGcmGetNotifyDataAddress", 3) == 0);
    REQUIRE(map(0x04C00000, 0x0F100000, 0x100000) == 0);
    CHECK(g.call("cellGcmGetNotifyDataAddress", 3) == 0x04C00000 + 3 * 0x40);
}

TEST_CASE("cellGcmSys: the 512 MB IO mode raises the maximum IO map size", "[hle][gcm]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    REQUIRE(f.mem.commit(0x04000000, 0x100000));
    REQUIRE(g.call("cellGcmInitSystemMode", 1) == 0); // CELL_GCM_SYSTEM_MODE_IOMAP_512MB
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    CHECK(g.call("cellGcmGetMaxIoMapSize") == 0x20000000);
    REQUIRE(g.call("cellGcmReserveIoMapSize", 0x1FF00000) == 0);
    CHECK(g.call("cellGcmGetMaxIoMapSize") == 0x00100000);
    REQUIRE(g.call("cellGcmUnreserveIoMapSize", 0x1FF00000) == 0);
    REQUIRE(g.call("cellGcmMapEaIoAddress", 0x04000000, 0x1FF00000, 0x100000) == 0); // the last MB of 512
    REQUIRE(g.call("cellGcmAddressToOffset", 0x04000020, kOut) == 0);
    CHECK(ps_read32(f.base(), kOut) == 0x1FF00020);
    REQUIRE(g.call("cellGcmUnmapIoAddress", 0x1FF00000) == 0);
    CHECK(g.call("cellGcmAddressToOffset", 0x04000020, kOut) == kFailure);
}

TEST_CASE("cellGcmSys: flip status, current display buffer and last flip time come from lv2's driver information",
    "[hle][gcm]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    rsx::Rsx* rsx = f.kernel->rsx();
    REQUIRE(rsx != nullptr);
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 info = static_cast<u32>(g.call("cellGcmGetControlRegister")) - 0x40 + 0x100000;
    REQUIRE(g.call("cellGcmSetDisplayBuffer", 0, 0x00100000, 5120, 1280, 720) == 0);
    REQUIRE(g.call("cellGcmSetDisplayBuffer", 1, 0x00500000, 5120, 1280, 720) == 0);

    // DONE (0) from cellGcmInit on (RPCS3 rsx::thread::init): head 1's flip-done bit.
    CHECK(g.call("cellGcmGetFlipStatus") == 0);
    CHECK((ps_read32(b, info + kHead1 + kFlipFlags) & 0x80000000) != 0);
    g.call("cellGcmResetFlipStatus");
    CHECK(g.call("cellGcmGetFlipStatus") == 1); // WAITING
    CHECK((ps_read32(b, info + kHead1 + kFlipFlags) & 0x80000000) == 0);
    g.call("cellGcmSetFlipStatus");
    CHECK(g.call("cellGcmGetFlipStatus") == 0);
    g.call("cellGcmResetFlipStatus");
    CHECK(g.call("cellGcmGetFlipStatus") == 1);

    // No flip yet: buffer 0, time 0.
    CHECK(g.call("cellGcmGetLastFlipTime") == 0);
    ps_write8(b, kOut, 0xAA);
    REQUIRE(g.call("cellGcmGetCurrentDisplayBufferId", kOut) == 0);
    CHECK(ps_read8(b, kOut) == 0);

    // A flip: DONE again, its buffer, its time on the guest's system-time clock (sys_time_get_system_time).
    flush_to_current(f, g);
    const u64 before = lv2::guest_system_time_us();
    REQUIRE(g.call("cellGcmSetFlip", ctx, 1) == 0);
    REQUIRE(rsx->wait_idle(2 * kSecond));
    const u64 after = lv2::guest_system_time_us();
    CHECK(rsx->flip_count() == 1);
    CHECK(g.call("cellGcmGetFlipStatus") == 0);
    REQUIRE(g.call("cellGcmGetCurrentDisplayBufferId", kOut) == 0);
    CHECK(ps_read8(b, kOut) == 1);
    const u64 when = g.call("cellGcmGetLastFlipTime");
    CHECK(when >= before);
    CHECK(when <= after);
    CHECK(g.call("cellGcmGetCurrentDisplayBufferId", kBadPointer) == kFailure);

    // The vblanks since cellGcmInit, which the interrupt thread counts.
    const u64 v0 = g.call("cellGcmGetVBlankCount");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const u64 v1 = g.call("cellGcmGetVBlankCount");
    CHECK(v1 >= v0 + 3); // ~12 expected at 59.94 Hz; loose for a loaded test machine
    CHECK(v1 <= v0 + 30);
}

TEST_CASE("sys_rsx: flip status (0x10a), a buffer queued on a head (0x103's driver information), as RPCS3's lv2",
    "[hle][gcm]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    lv2::PpuThread& t = *g.t;
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    rsx::Rsx* rsx = f.kernel->rsx();
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    const u32 info = control - 0x40 + 0x100000;

    // 0x10a: flipFlags = (flipFlags & a4) | a5, heads 0 and 1 only.
    CHECK(lv2::sys_rsx_context_attribute(t, 0x55555555, 0x10a, 8, 0, 0, 0) == -17);
    CHECK(lv2::sys_rsx_context_attribute(t, 0x55555555, 0x10a, 2, 0, 0, 0) == lv2::err(lv2::CELL_EINVAL));
    REQUIRE(lv2::sys_rsx_context_attribute(t, 0x55555555, 0x10a, 0, 0xFFFFFFFF, 0x1200, 0) == lv2::CELL_OK);
    CHECK(ps_read32(b, info + kHead0 + kFlipFlags) == 0x1200);
    REQUIRE(lv2::sys_rsx_context_attribute(t, 0x55555555, 0x10a, 0, 0xFFFFF0FF, 0x34, 0) == lv2::CELL_OK);
    CHECK(ps_read32(b, info + kHead0 + kFlipFlags) == 0x1034);

    // GCM_DRIVER_QUEUE on head 0 with buffer 2: the head's last queued buffer, the queued bit and the buffer's bit.
    const u32 c = ps_read32(b, ctx + 0x8);
    ps_write32(b, c, rsx::cmd::method_header(rsx::m::GCM_DRIVER_QUEUE + 0, 1));
    ps_write32(b, c + 4, 2);
    ps_write32(b, ctx + 0x8, c + 8);
    ps_write32(b, control, c + 8 - kIo);
    rsx->notify();
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(ps_read32(b, info + kHead0 + kLastQueuedBufferId) == 2);
    CHECK(ps_read32(b, info + kHead0 + kFlipFlags) == (0x1034 | 0x40000000 | 0x4));
}

TEST_CASE("cellGcmSys: the queue handler runs on the interrupt thread with the head, before the flip's", "[hle][gcm]")
{
    InterpretMisses interpret;
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    constexpr u32 kSeq = kData + 0x2000, kQueueSlot = kData + 0x2010, kFlipSlot = kData + 0x2020;
    constexpr u32 kQueueCode = kData + 0x1000, kQueueOpd = kData + 0x1100;
    constexpr u32 kFlipCode = kData + 0x1200, kFlipOpd = kData + 0x1300;
    write_sequence_function(b, kQueueCode, kQueueOpd, kSeq, kQueueSlot);
    write_sequence_function(b, kFlipCode, kFlipOpd, kSeq, kFlipSlot);
    const auto seq = [&] { return ps_read32(b, kSeq); };

    g.call("cellGcmSetQueueHandler", kQueueOpd); // before cellGcmInit: ignored (RPCS3)
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    rsx::Rsx* rsx = f.kernel->rsx();
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    REQUIRE(g.call("cellGcmSetDisplayBuffer", 0, 0x00100000, 5120, 1280, 720) == 0);
    flush_to_current(f, g);
    REQUIRE(g.call("cellGcmSetFlip", ctx, 0) == 0);
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(rsx->flip_count() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(seq() == 0);

    // A flip command queues on head 1, then flips: the queue handler, then the flip handler, both with head 1.
    g.call("cellGcmSetQueueHandler", kQueueOpd);
    g.call("cellGcmSetFlipHandler", kFlipOpd);
    REQUIRE(g.call("cellGcmSetFlip", ctx, 0) == 0);
    REQUIRE(wait_for([&] { return seq() == 2; }));
    CHECK(ps_read32(b, kQueueSlot) == 1);
    CHECK(ps_read32(b, kQueueSlot + 4) == 1);
    CHECK(ps_read32(b, kFlipSlot) == 2);
    CHECK(ps_read32(b, kFlipSlot + 4) == 1);

    // A GCM_DRIVER_QUEUE method: the queue handler with its head, and no flip.
    const u32 c = ps_read32(b, ctx + 0x8);
    ps_write32(b, c, rsx::cmd::method_header(rsx::m::GCM_DRIVER_QUEUE + 0, 1));
    ps_write32(b, c + 4, 1);
    ps_write32(b, ctx + 0x8, c + 8);
    ps_write32(b, control, c + 8 - kIo);
    rsx->notify();
    REQUIRE(wait_for([&] { return seq() == 3; }));
    CHECK(ps_read32(b, kQueueSlot) == 3);
    CHECK(ps_read32(b, kQueueSlot + 4) == 0);
    CHECK(rsx->flip_count() == 2);
    // cellGcmSetFlip's put + 8 lands where the next flip is written: put is at current again.
    REQUIRE(ps_read32(b, control) == ps_read32(b, ctx + 0x8) - kIo);

    // Without a queue handler, a flip calls the flip handler alone.
    g.call("cellGcmSetQueueHandler", 0);
    REQUIRE(g.call("cellGcmSetFlip", ctx, 0) == 0);
    REQUIRE(wait_for([&] { return seq() == 4; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(seq() == 4);
    CHECK(ps_read32(b, kFlipSlot) == 4);
    CHECK(ps_read32(b, kQueueSlot) == 3);
}

TEST_CASE("cellGcmSys: cellGcmSetPrepareFlip, cellGcmSetUserCommand and cellGcmSetWaitFlipUnsafe write RPCS3's words",
    "[hle][gcm]")
{
    InterpretMisses interpret; // the user handler and the contexts' callbacks are guest code
    LogLines logs;
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    write_failing_callbacks(b);
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    rsx::Rsx* rsx = f.kernel->rsx();
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    const auto current = [&] { return ps_read32(b, ctx + 0x8); };
    const auto put = [&] { return ps_read32(b, control); };

    // cellGcmSetPrepareFlip returns the id; on the default context put moves past the command, so the RSX flips.
    flush_to_current(f, g);
    const u32 c0 = current();
    CHECK(g.call("cellGcmSetPrepareFlip", ctx, 3) == 3);
    CHECK(ps_read32(b, c0) == rsx::cmd::method_header(rsx::m::GCM_FLIP_COMMAND, 1));
    CHECK(ps_read32(b, c0 + 4) == 3);
    CHECK(current() == c0 + 8);
    CHECK(put() == c0 + 8 - kIo);
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(rsx->flip_count() == 1);
    CHECK(g.call("cellGcmSetPrepareFlip", ctx, 8) == kFailure);
    CHECK(g.call("cellGcmSetPrepareFlip", kBadPointer, 0) == kFailure);
    CHECK(current() == c0 + 8);

    // A game's own context: put stays; a callback that makes no room has its result returned as it is.
    constexpr u32 kOwn = kData + 0x200, kOwnBuf = kIo + 0x80000, kOwnEnd = kOwnBuf + 0x100 - 4;
    const auto own_context = [&](u32 cur, u32 callback) {
        ps_write32(b, kOwn + 0x0, kOwnBuf);
        ps_write32(b, kOwn + 0x4, kOwnEnd);
        ps_write32(b, kOwn + 0x8, cur);
        ps_write32(b, kOwn + 0xC, callback);
    };
    own_context(kOwnBuf, kOpdNeg);
    CHECK(g.call("cellGcmSetPrepareFlip", kOwn, 2) == 2);
    CHECK(ps_read32(b, kOwnBuf + 4) == 2);
    CHECK(put() == c0 + 8 - kIo);
    own_context(kOwnEnd, kOpdNeg);
    CHECK(g.call("cellGcmSetPrepareFlip", kOwn, 2) == r3_of_code(0xFFFFFFFF)); // -1
    own_context(kOwnEnd, kOpdPos);
    CHECK(g.call("cellGcmSetPrepareFlip", kOwn, 2) == 5);
    CHECK(ps_read32(b, kOwn + 0x8) == kOwnEnd);
    CHECK(ps_read32(b, kOwnEnd) == 0);

    // cellGcmSetUserCommand: GCM_SET_USER_COMMAND with the cause, put untouched; the RSX hands the cause to the user
    // handler on the interrupt thread.
    constexpr u32 kSeq = kData + 0x2000, kUserSlot = kData + 0x2010, kUserCode = kData + 0x1000,
                  kUserOpd = kData + 0x1100;
    write_sequence_function(b, kUserCode, kUserOpd, kSeq, kUserSlot);
    g.call("cellGcmSetUserHandler", kUserOpd);
    const u32 c1 = current();
    g.call("cellGcmSetUserCommand", ctx, 0x77);
    CHECK(ps_read32(b, c1) == rsx::cmd::method_header(rsx::m::GCM_SET_USER_COMMAND, 1));
    CHECK(ps_read32(b, c1 + 4) == 0x77);
    CHECK(current() == c1 + 8);
    CHECK(put() == c0 + 8 - kIo);
    ps_write32(b, control, current() - kIo);
    rsx->notify();
    REQUIRE(wait_for([&] { return ps_read32(b, kSeq) == 1; }));
    CHECK(ps_read32(b, kUserSlot + 4) == 0x77);

    // No room: the default context's callback moves to the next segment first.
    const u32 end = ps_read32(b, ctx + 0x4);
    ps_write32(b, ctx + 0x8, end - 4);
    g.call("cellGcmSetUserCommand", ctx, 0x78);
    CHECK(ps_read32(b, end - 4) == rsx::cmd::old_jump(0x8000));
    CHECK(ps_read32(b, kIo + 0x8000) == rsx::cmd::method_header(rsx::m::GCM_SET_USER_COMMAND, 1));
    CHECK(ps_read32(b, kIo + 0x8004) == 0x78);
    CHECK(current() == kIo + 0x8008);
    // A callback that fails: logged, nothing written.
    own_context(kOwnEnd, kOpdNeg);
    g.call("cellGcmSetUserCommand", kOwn, 0x79);
    CHECK(ps_read32(b, kOwnEnd) == 0);
    CHECK(ps_read32(b, kOwn + 0x8) == kOwnEnd);
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "cellGcmSetUserCommand(): callback failed (0xffffffff)") == 1);

    // cellGcmSetWaitFlipUnsafe: cellGcmSetWaitFlip's words without the room check (past end, no callback).
    own_context(kOwnEnd, kOpdNeg);
    g.call("cellGcmSetWaitFlipUnsafe", kOwn);
    CHECK(ps_read32(b, kOwnEnd) == rsx::cmd::method_header(rsx::m::NV406E_SEMAPHORE_OFFSET, 2));
    CHECK(ps_read32(b, kOwnEnd + 4) == 0x10);
    CHECK(ps_read32(b, kOwnEnd + 8) == 0);
    CHECK(ps_read32(b, kOwn + 0x8) == kOwnEnd + 12);
    g.call("cellGcmSetWaitFlipUnsafe", kBadPointer);
    CHECK(logs.count(log::Level::Error, "hle", "cellGcmSetWaitFlipUnsafe: the context 0x70000000") == 1);
}

TEST_CASE("cellGcmSys: the default command buffer's word sizes and cellGcmSetDefaultCommandBuffer", "[hle][gcm]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    rsx::Rsx* rsx = f.kernel->rsx();
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    const auto set_sizes = [&](u32 buffer, u32 segment) {
        return g.call("cellGcmSetDefaultCommandBufferAndSegmentWordSize", buffer, segment);
    };

    CHECK(g.call("cellGcmGetDefaultCommandWordSize") == 0x400);
    CHECK(g.call("cellGcmGetDefaultSegmentWordSize") == 0x100);
    // Only while put and get are both 0x1000, the default buffer's first command: not yet (both 0 after cellGcmInit).
    CHECK(set_sizes(0x800, 0x400) == kFailure);
    ps_write32(b, control, 0x1000);
    rsx->notify();
    REQUIRE(rsx->wait_idle(2 * kSecond));
    REQUIRE(ps_read32(b, control + 4) == 0x1000);
    CHECK(set_sizes(0x7FF, 0x400) == kFailure);        // less than two segments
    CHECK(set_sizes(0xFFFFFFFF, 0x80000000) == kFailure); // a segment of 2^31 words
    REQUIRE(set_sizes(0x800, 0x400) == 0);
    CHECK(g.call("cellGcmGetDefaultCommandWordSize") == 0x800);
    CHECK(g.call("cellGcmGetDefaultSegmentWordSize") == 0x400);
    // Once commands ran, no more.
    ps_write32(b, control, 0x1008);
    rsx->notify();
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(set_sizes(0x1000, 0x400) == kFailure);
    CHECK(g.call("cellGcmGetDefaultCommandWordSize") == 0x800);

    // cellGcmSetDefaultCommandBuffer: the game's context variable points at the default context again.
    ps_write32(b, kCtxPtr, kData + 0x200);
    g.call("cellGcmSetDefaultCommandBuffer");
    CHECK(ps_read32(b, kCtxPtr) == ctx);
}

TEST_CASE("cellGcmSys: Z-cull binding as RPCS3, and the arrays the Get*Info functions return", "[hle][gcm]")
{
    LogLines logs;
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    lv2::RsxDevice& dev = f.kernel->impl().rsx;
    const auto zcull = [&](u32 i) {
        std::lock_guard lock(dev.mutex);
        return dev.zculls[i];
    };

    // CellGcmDisplayInfo[8], CellGcmTileInfo[15] (RPCS3 GcmTileInfo::pack), CellGcmZcullInfo[8].
    const u32 display = static_cast<u32>(g.call("cellGcmGetDisplayInfo"));
    const u32 tiles = static_cast<u32>(g.call("cellGcmGetTileInfo"));
    const u32 zculls = static_cast<u32>(g.call("cellGcmGetZcullInfo"));
    REQUIRE(display != 0);
    REQUIRE(tiles != 0);
    REQUIRE(zculls != 0);
    REQUIRE(g.call("cellGcmSetDisplayBuffer", 2, 0x00300000, 5120, 1280, 720) == 0);
    CHECK(ps_read32(b, display + 32 + 0x0) == 0x00300000);
    CHECK(ps_read32(b, display + 32 + 0x4) == 5120);
    CHECK(ps_read32(b, display + 32 + 0x8) == 1280);
    CHECK(ps_read32(b, display + 32 + 0xC) == 720);
    REQUIRE(g.call("cellGcmSetTileInfo", 1, 0, 0x20000, 0x40000, 0x1400, 0, 5, 1) == 0);
    CHECK(ps_read32(b, tiles + 16 + 0x0) == 0x00020011); // (location + 1) | bank << 4 | offset / 64K << 16
    CHECK(ps_read32(b, tiles + 16 + 0x4) == 0x00050000); // (offset + size - 1) / 64K << 16
    CHECK(ps_read32(b, tiles + 16 + 0x8) == 0x00001400);
    CHECK(ps_read32(b, tiles + 16 + 0xC) == 0x40010005); // base | (base + (size - 1) / 64K) << 13 | 1 << 30

    // cellGcmBindZcull: lv2's result, and the packed words in the array only when lv2 took them.
    CHECK(g.call("cellGcmBindZcull", 8, 0x300000, 1280, 720, 0x1000, 2, 0, 1, 1, 7, 0x80, 0xff) ==
          kLv1IllegalParameter);
    REQUIRE(g.call("cellGcmBindZcull", 0, 0x300000, 1280, 720, 0x1000, 2, 0, 1, 1, 7, 0x80, 0xff) == 0);
    const u32 words[6] = {0x21, 0x050002C0, 0x1000, 0x300000, 0xFF807006, 0x00202000};
    for (u32 i = 0; i < 6; i++) CHECK(ps_read32(b, zculls + 4 * i) == words[i]);
    {
        const lv2::RsxDevice::Zcull z = zcull(0);
        CHECK(z.bound);
        CHECK(z.width == 1280);
        CHECK(z.height == 704); // whole 64-pixel blocks
        CHECK(z.cull_start == 0x1000);
        CHECK(z.offset == 0x300000);
    }
    // RPCS3's lv2 refuses a cull range outside the 3 MB of Z-cull RAM, an empty one, and an offset outside local
    // memory; the array keeps what it had.
    ps_write32(b, zculls + 24, 0xDEADBEEF);
    CHECK(g.call("cellGcmBindZcull", 1, 0x300000, 1280, 720, 0x2F0000, 2, 0, 1, 1, 7, 0x80, 0xff) == kEinval);
    CHECK(g.call("cellGcmBindZcull", 1, 0x300000, 0, 720, 0, 2, 0, 1, 1, 7, 0x80, 0xff) == kEinval);
    CHECK(g.call("cellGcmBindZcull", 1, 0x0F900000, 1280, 720, 0, 2, 0, 1, 1, 7, 0x80, 0xff) == kEinval);
    CHECK(ps_read32(b, zculls + 24) == 0xDEADBEEF);
    CHECK_FALSE(zcull(1).bound);
    // cellGcmSetZcull is the same without a result (RPCS3); a refusal is logged.
    g.call("cellGcmSetZcull", 1, 0x300000, 1280, 720, 0x2F0000, 2, 0, 1, 1, 7, 0x80, 0xff);
    CHECK(ps_read32(b, zculls + 24) == 0xDEADBEEF);
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "cellGcmSetZcull: lv2 refused Z-cull region 1") == 1);
    g.call("cellGcmSetZcull", 1, 0x400000, 640, 360, 0x100000, 2, 0, 1, 1, 7, 0x80, 0xff);
    CHECK(ps_read32(b, zculls + 24 + 12) == 0x400000);
    CHECK(zcull(1).bound);

    // cellGcmUnbindZcull: CELL_GCM_ERROR_INVALID_VALUE past 7; else unbound, its settings kept (RPCS3 clears the bound
    // flag only), and the array as it was.
    CHECK(g.call("cellGcmUnbindZcull", 8) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    REQUIRE(g.call("cellGcmUnbindZcull", 0) == 0);
    {
        const lv2::RsxDevice::Zcull z = zcull(0);
        CHECK_FALSE(z.bound);
        CHECK(z.width == 1280);
        CHECK(z.offset == 0x300000);
        CHECK(z.cull_start == 0x1000);
    }
    for (u32 i = 0; i < 6; i++) CHECK(ps_read32(b, zculls + 4 * i) == words[i]);
    REQUIRE(g.call("cellGcmUnbindZcull", 5) == 0); // never bound
    CHECK_FALSE(zcull(5).bound);
}

TEST_CASE("cellGcmSys: _cellGcmFunc12, debug level, second V frequency, immediate flip, default FIFO, terminate",
    "[hle][gcm]")
{
    LogLines logs;
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    rsx::Rsx* rsx = f.kernel->rsx();
    lv2::RsxDevice& dev = f.kernel->impl().rsx;
    const auto vsync = [&] {
        std::lock_guard lock(dev.mutex);
        return dev.vsync;
    };

    // _cellGcmFunc12: the local report area.
    CHECK(g.call("_cellGcmFunc12") == g.call("cellGcmGetLabelAddress", 0) + 0x1400);
    CHECK(g.call("_cellGcmFunc12") == g.call("cellGcmGetReportDataAddress", 0));

    // cellGcmSetDebugOutputLevel: CELL_GCM_DEBUG_LEVEL0..2 (1..3) kept, anything else ignored.
    g.call("cellGcmSetDebugOutputLevel", 2);
    CHECK(logs.count(log::Level::Info, "cellGcmSys", "cellGcmSetDebugOutputLevel(2)") == 1);
    g.call("cellGcmSetDebugOutputLevel", 0);
    g.call("cellGcmSetDebugOutputLevel", 4);
    CHECK(logs.count(log::Level::Warn, "cellGcmSys", "not a debug level, ignored") == 2);

    // cellGcmSetSecondVFrequency: 59.94 Hz taken; scanout and disabled logged (RPCS3 TODOs); anything else an error.
    g.call("cellGcmSetSecondVFrequency", 1);
    CHECK(logs.count(log::Level::Warn, "cellGcmSys", "cellGcmSetSecondVFrequency") == 0);
    g.call("cellGcmSetSecondVFrequency", 2);
    CHECK(logs.count(log::Level::Warn, "cellGcmSys", "the scanout frequency is not implemented") == 1);
    g.call("cellGcmSetSecondVFrequency", 3);
    CHECK(logs.count(log::Level::Warn, "cellGcmSys", "disabling it is not implemented") == 1);
    g.call("cellGcmSetSecondVFrequency", 4);
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "4 is not a display frequency") == 1);

    // cellGcmSetFlipImmediate: a buffer id past 7 is CELL_GCM_ERROR_FAILURE; otherwise nothing changes, logged once
    // (RPCS3 would set the flip mode to the id: vsync for 2).
    CHECK(g.call("cellGcmSetFlipImmediate", 8) == kFailure);
    CHECK(g.call("cellGcmSetFlipImmediate", 2) == 0);
    CHECK_FALSE(vsync());
    CHECK(rsx->flip_count() == 0);
    CHECK(g.call("cellGcmSetFlipImmediate", 1) == 0);
    CHECK(logs.count(log::Level::Error, "cellGcmSys", "cellGcmSetFlipImmediate(") == 1);

    // RPCS3's plain CELL_OKs.
    CHECK(g.call("cellGcmInitDefaultFifoMode", 0) == 0);
    CHECK(g.call("cellGcmSetDefaultFifoSize", 0x100000, 0x8000) == 0);
    CHECK(g.call("cellGcmTerminate") == 0);
}
