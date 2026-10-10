// cellGcmSys over lv2 sys_rsx (synthetic: no game data). The end-to-end case writes commands the way a game's
// static libgcm does (through the CellGcmContextData pointers, then the put register) and checks that the RSX thread
// runs them: reference register, a label release, and a flip of a registered display buffer.

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include "pspocket/core/memory_map.h"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/rsx.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kIo = 0x00200000;     // the "game's" IO buffer: 1 MB of main memory mapped at IO offset 0
constexpr u32 kCtxPtr = kData;      // CellGcmContextData* written by _cellGcmInitBody
constexpr u32 kOut = kData + 0x100; // output variables
constexpr u32 kCmdSize = 0x40000;   // 8 segments of 32 KB
constexpr u32 CELL_GCM_ERROR_FAILURE = 0x802100ff;
constexpr u32 CELL_GCM_ERROR_INVALID_VALUE = 0x80210003;
constexpr u32 CELL_GCM_ERROR_INVALID_ALIGNMENT = 0x80210004;
constexpr u32 CELL_GCM_ERROR_INVALID_ENUM = 0x80210002;
constexpr u64 kSecond = 1'000'000'000ull;

lv2::Config rsx_test_config()
{
    lv2::Config c;
    c.rsx_log_failures = true;
    return c;
}

// What the loader and the game's CRT do first: link the module's internal functions, initialize TLS (the GCM
// interrupt thread gets a TLS block like any thread).
struct GcmFixture : Fixture {
    GcmFixture() : Fixture({}, rsx_test_config())
    {
        REQUIRE(hle->link_internal_functions({"cellGcmSys"}));
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
    }
};

} // namespace

TEST_CASE("sys_rsx: local memory, device, context layout and driver info, IO map, display buffers", "[hle][gcm]")
{
    Fixture f({}, rsx_test_config());
    GuestThread g(f, 0, "rsx");
    u8* b = f.base();
    lv2::PpuThread& t = *g.t;

    // A context needs local memory first, and the size must be whole MBs.
    CHECK(lv2::sys_rsx_memory_allocate(t, kOut, kOut + 8, 0x0F900001, 0, 0, 0, 0) == -17);
    REQUIRE(lv2::sys_rsx_memory_allocate(t, kOut, kOut + 8, 0x0F900000, 0, 0, 0, 0) == lv2::CELL_OK);
    CHECK(ps_read64(b, kOut + 8) == PS_MEM_RSX_LOCAL_BASE);
    CHECK(f.mem.is_committed(PS_MEM_RSX_LOCAL_BASE + 0x0F8FFFFF));

    REQUIRE(lv2::sys_rsx_device_map(t, kOut + 0x10, 0, 8) == lv2::CELL_OK);
    const u32 device = static_cast<u32>(ps_read64(b, kOut + 0x10));
    CHECK(device % 0x10000000 == 0);
    CHECK(f.kernel->rsx() == nullptr);
    REQUIRE(lv2::sys_rsx_context_allocate(t, kOut + 0x18, kOut + 0x20, kOut + 0x28, kOut + 0x30, 0, 0) == lv2::CELL_OK);
    CHECK(ps_read32(b, kOut + 0x18) == 0x55555555);
    const u32 dma = static_cast<u32>(ps_read64(b, kOut + 0x20));
    const u32 info = static_cast<u32>(ps_read64(b, kOut + 0x28));
    const u32 reports = static_cast<u32>(ps_read64(b, kOut + 0x30));
    CHECK(dma == device + 0x100000);
    CHECK(info == dma + 0x100000);
    CHECK(reports == dma + 0x200000);
    REQUIRE(f.kernel->rsx() != nullptr);
    // RsxDriverInfo as RPCS3 fills it; reports initialized; the device's flip semaphore is 1.
    CHECK(ps_read32(b, info + 0x0) == 0x211);
    CHECK(ps_read32(b, info + 0x8) == 0x0F900000);
    CHECK(ps_read32(b, info + 0x2C) == 0x1000);
    CHECK(ps_read32(b, info + 0x34) == 0x1400);
    CHECK(ps_read32(b, reports + 0) == 0x1337C0D3);
    CHECK(ps_read32(b, dma + 0x40) == 0); // put
    CHECK(ps_read32(b, device + 0x30) == 1);
    // A second context is refused.
    CHECK(lv2::sys_rsx_context_allocate(t, kOut + 0x18, kOut + 0x20, kOut + 0x28, kOut + 0x30, 0, 0) ==
          lv2::err(lv2::CELL_EBUSY));

    // IO map: 1 MB granularity, committed memory only, within the 256 MB IO space.
    CHECK(lv2::sys_rsx_context_iomap(t, 0x55555555, 0, kIo + 0x1000, 0x100000, 0) == lv2::err(lv2::CELL_EINVAL));
    CHECK(lv2::sys_rsx_context_iomap(t, 0x55555555, 0, 0x08000000, 0x100000, 0) == lv2::err(lv2::CELL_EINVAL));
    CHECK(lv2::sys_rsx_context_iomap(t, 0x1234, 0, kIo, 0x100000, 0) == lv2::err(lv2::CELL_EINVAL));
    REQUIRE(lv2::sys_rsx_context_iomap(t, 0x55555555, 0x00300000, kIo, 0x100000, 0) == lv2::CELL_OK);
    CHECK(f.kernel->rsx()->io_map().io_to_ea(0x00300010) == kIo + 0x10);
    REQUIRE(lv2::sys_rsx_context_iounmap(t, 0x55555555, 0x00300000, 0x100000) == lv2::CELL_OK);
    CHECK(!f.kernel->rsx()->io_map().io_to_ea(0x00300000));

    // Display buffer (package 0x104): width/height in a4, pitch/offset in a5.
    REQUIRE(lv2::sys_rsx_context_attribute(t, 0x55555555, 0x104, 2, (u64{1280} << 32) | 720,
                (u64{5120} << 32) | 0x100000, 0) == lv2::CELL_OK);
    const auto db = f.kernel->rsx()->display_buffer(2);
    REQUIRE(db);
    CHECK(db->width == 1280);
    CHECK(db->height == 720);
    CHECK(db->pitch == 5120);
    CHECK(db->offset == 0x100000);
    CHECK(lv2::sys_rsx_context_attribute(t, 0x55555555, 0x104, 8, 0, 0, 0) == -17);
    CHECK(lv2::sys_rsx_context_attribute(t, 0x55555555, 0x999, 0, 0, 0, 0) == lv2::err(lv2::CELL_EINVAL));
}

TEST_CASE("cellGcmSys: init, configuration, offsets, IO mapping, tiles, Z-cull, pitches", "[hle][gcm]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();

    for (const hle::Function& fn : hle::registry().functions()) {
        if (fn.module == "cellGcmSys") CHECK(fn.nid == recomp::nid_of(fn.name));
    }

    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    const u32 ctx = ps_read32(b, kCtxPtr);
    REQUIRE(ctx != 0);
    // The context: 4 KB reserved at the start, a segment end 4 bytes short of 32 KB, the callback linked at load.
    CHECK(ps_read32(b, ctx + 0x0) == kIo + 0x1000);
    CHECK(ps_read32(b, ctx + 0x4) == kIo + 0x8000 - 4);
    CHECK(ps_read32(b, ctx + 0x8) == kIo + 0x1000);
    CHECK(ps_read32(b, ctx + 0xC) == f.hle->descriptor_of("cellGcmSys", 0x8DD39C9C));
    CHECK(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == r3_of_code(CELL_GCM_ERROR_FAILURE));

    g.call("cellGcmGetConfiguration", kOut);
    CHECK(ps_read32(b, kOut + 0x00) == PS_MEM_RSX_LOCAL_BASE);
    CHECK(ps_read32(b, kOut + 0x04) == kIo);
    CHECK(ps_read32(b, kOut + 0x08) == 0x0F900000);
    CHECK(ps_read32(b, kOut + 0x0C) == 0x100000);
    CHECK(ps_read32(b, kOut + 0x10) == 650000000);

    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    CHECK(control % 0x100000 == 0x40); // RsxDmaControl + 0x40
    const u32 label0 = static_cast<u32>(g.call("cellGcmGetLabelAddress", 0));
    CHECK(g.call("cellGcmGetLabelAddress", 3) == label0 + 0x30);
    CHECK(g.call("cellGcmGetReportDataAddressLocation", 2, 0) == label0 + 0x1400 + 0x20);
    ps_write64(b, label0 + 0x1400 + 0x20, 0x1122334455667788ull);
    ps_write32(b, label0 + 0x1400 + 0x28, 77);
    CHECK(g.call("cellGcmGetTimeStampLocation", 2, 0) == 0x1122334455667788ull);
    CHECK(g.call("cellGcmGetReportDataLocation", 2, 0) == 77);

    // Offsets: local memory, IO-mapped main memory, and unmapped memory.
    REQUIRE(g.call("cellGcmAddressToOffset", PS_MEM_RSX_LOCAL_BASE + 0x1234, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x1234);
    REQUIRE(g.call("cellGcmAddressToOffset", kIo + 0x5678, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x5678);
    CHECK(g.call("cellGcmAddressToOffset", 0x00F00000, kOut) == r3_of_code(CELL_GCM_ERROR_FAILURE));
    REQUIRE(g.call("cellGcmIoOffsetToAddress", 0x5678, kOut) == 0);
    CHECK(ps_read32(b, kOut) == kIo + 0x5678);
    CHECK(g.call("cellGcmIoOffsetToAddress", 0x00500000, kOut) == r3_of_code(CELL_GCM_ERROR_FAILURE));

    // More main memory for the RSX (strict ordering is the only flag RPCS3 accepts).
    CHECK(g.call("cellGcmMapEaIoAddressWithFlags", 0x00100000, 0x00400000, 0x100000, 0) ==
          r3_of_code(CELL_GCM_ERROR_FAILURE));
    REQUIRE(g.call("cellGcmMapEaIoAddressWithFlags", 0x00100000, 0x00400000, 0x100000, 2) == 0);
    REQUIRE(g.call("cellGcmAddressToOffset", 0x00100010, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00400010);
    REQUIRE(g.call("cellGcmIoOffsetToAddress", 0x00400010, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x00100010);

    // Tiles: validation as RPCS3, packed words in the array, binding through lv2.
    CHECK(g.call("cellGcmSetTileInfo", 15, 0, 0, 0x10000, 0x100, 0, 0, 0) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));
    CHECK(g.call("cellGcmSetTileInfo", 0, 0, 0x100, 0x10000, 0x100, 0, 0, 0) ==
          r3_of_code(CELL_GCM_ERROR_INVALID_ALIGNMENT));
    CHECK(g.call("cellGcmSetTileInfo", 0, 2, 0, 0x10000, 0x100, 0, 0, 0) == r3_of_code(CELL_GCM_ERROR_INVALID_ENUM));
    REQUIRE(g.call("cellGcmSetTileInfo", 1, 0, 0x20000, 0x40000, 0x1400, 0, 5, 1) == 0);
    REQUIRE(g.call("cellGcmBindTile", 1) == 0);
    REQUIRE(g.call("cellGcmUnbindTile", 1) == 0);
    CHECK(g.call("cellGcmBindTile", 15) == r3_of_code(CELL_GCM_ERROR_INVALID_VALUE));

    // Z-cull: twelve arguments, the last four on the stack.
    g.call("cellGcmSetZcull", 0, 0x300000, 1280, 720, 0, 2, 0, 0, 1, 0, 0x80, 0xff);

    CHECK(g.call("cellGcmGetTiledPitchSize", 1280 * 4) == 0x1400);
    CHECK(g.call("cellGcmGetTiledPitchSize", 0x1401) == 0x1800);
    CHECK(g.call("cellGcmGetTiledPitchSize", 0x20000) == 0);
}

TEST_CASE("cellGcmSys end to end: commands written through the context run on the RSX thread", "[hle][gcm]")
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

    // What libgcm's inline command functions write: reference, a label release, then the flip.
    u32 cur = ps_read32(b, ctx + 0x8);
    auto put_method = [&](u32 reg, u32 value) {
        ps_write32(b, cur, rsx::cmd::method_header(reg, 1));
        ps_write32(b, cur + 4, value);
        cur += 8;
    };
    put_method(rsx::m::NV406E_SET_REFERENCE, 0x1234);
    put_method(rsx::m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, rsx::loc::kDmaSemaphoreRW);
    put_method(rsx::m::NV406E_SEMAPHORE_OFFSET, 0x20);
    put_method(rsx::m::NV406E_SEMAPHORE_RELEASE, 0xCAFE);
    ps_write32(b, ctx + 0x8, cur);
    g.call("_cellGcmSetFlipCommand", ctx, 0);
    CHECK(ps_read32(b, ctx + 0x8) == cur + 8);
    store32_release(b, control, ps_read32(b, ctx + 0x8) - kIo); // put = IO offset of current, after a sync (D-585)
    rsx->notify();

    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(load32_acquire(b, control + 8) == 0x1234);             // ref
    CHECK(load32_acquire(b, label0 + 0x20) == 0xCAFE);           // label 2
    CHECK(load32_acquire(b, control + 4) == ps_read32(b, control)); // get caught up with put
    CHECK(rsx->flip_count() == 1);
    CHECK(flipped == 0);

    // A flip that waits for a label: the RSX blocks until the game (here, the test) writes it.
    store32_release(b, label0 + 0x40, 0);
    g.call("_cellGcmSetFlipCommandWithWaitLabel", ctx, 0, 4, 7);
    store32_release(b, control, ps_read32(b, ctx + 0x8) - kIo);
    rsx->notify();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(rsx->flip_count() == 1);
    store32_release(b, label0 + 0x40, 7);
    rsx->notify();
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(rsx->flip_count() == 2);
}

TEST_CASE("cellGcmSys: the command buffer callback flushes and jumps to the next segment", "[hle][gcm]")
{
    GcmFixture f;
    GuestThread g(f, 0, "gcm");
    u8* b = f.base();
    REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
    const u32 ctx = ps_read32(b, kCtxPtr);
    const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
    rsx::Rsx* rsx = f.kernel->rsx();

    // The first segment "filled" up to its end (zero words are NOPs).
    const u32 end = ps_read32(b, ctx + 0x4);
    ps_write32(b, ctx + 0x8, end);
    REQUIRE(g.call("cellGcmCallback", ctx, 8) == 0);
    CHECK(ps_read32(b, control) == end - kIo);             // put: everything before the jump
    CHECK(ps_read32(b, end) == rsx::cmd::old_jump(0x8000)); // jump to segment 1
    CHECK(ps_read32(b, ctx + 0x0) == kIo + 0x8000);
    CHECK(ps_read32(b, ctx + 0x8) == kIo + 0x8000);
    CHECK(ps_read32(b, ctx + 0x4) == kIo + 0x10000 - 4);
    REQUIRE(rsx->wait_idle(2 * kSecond));
    CHECK(ps_read32(b, control + 4) == end - kIo);

    // From the last segment, the next one is the first again (4 KB in).
    ps_write32(b, ctx + 0x8, kIo + 7 * 0x8000 + 0x100);
    REQUIRE(g.call("cellGcmCallback", ctx, 8) == 0);
    CHECK(ps_read32(b, ctx + 0x0) == kIo + 0x1000);
    CHECK(ps_read32(b, ctx + 0x4) == kIo + 0x8000 - 4);
    REQUIRE(rsx->wait_idle(2 * kSecond));
}

TEST_CASE("cellGcmSys: the interrupt thread counts vblanks and calls the game's vblank handler", "[hle][gcm]")
{
    const ps_ppu_miss_policy policy = ps_ppu_get_miss_policy();
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); // the synthetic handler has no lifted code
    {
        GcmFixture f;
        GuestThread g(f, 0, "gcm");
        u8* b = f.base();

        // handler(head): counter += 1; last_head = head
        constexpr u32 kCode = kData + 0x1000, kOpd = kData + 0x1100, kCounter = kData + 0x1200;
        const u32 code[] = {
            0x3CA00000u | (kCounter >> 16),    // lis r5, counter@h
            0x60A50000u | (kCounter & 0xFFFF), // ori r5, r5, counter@l
            0x80850000u,                       // lwz r4, 0(r5)
            0x38840001u,                       // addi r4, r4, 1
            0x90850000u,                       // stw r4, 0(r5)
            0x90650004u,                       // stw r3, 4(r5)
            0x4E800020u,                       // blr
        };
        for (u32 i = 0; i < std::size(code); i++) ps_write32(b, kCode + 4 * i, code[i]);
        ps_write32(b, kOpd, kCode);
        ps_write32(b, kOpd + 4, 0);

        g.call("cellGcmSetVBlankHandler", kOpd); // before init: ignored, as RPCS3
        REQUIRE(g.call("_cellGcmInitBody", kCtxPtr, kCmdSize, 0x100000, kIo) == 0);
        g.call("cellGcmSetVBlankHandler", kOpd);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const u32 calls = ps_read32(b, kCounter);
        CHECK(calls >= 3);  // ~12 expected at 59.94 Hz; loose for a loaded test machine
        CHECK(calls <= 20);
        CHECK(ps_read32(b, kCounter + 4) == 1); // head 1

        // RsxDriverInfo head 1 counts the vblanks (driver info = DMA control + 1 MB; head[1] at 0x10B8 + 0x40).
        const u32 control = static_cast<u32>(g.call("cellGcmGetControlRegister"));
        const u32 info = control - 0x40 + 0x100000;
        CHECK(load64_acquire(b, info + 0x10B8 + 0x40 + 0x30) >= calls); // the vblank thread still counts
    }
    ps_ppu_set_miss_policy(policy);
}

TEST_CASE("sys_rsx: freeing the context and the local memory, then allocating again", "[hle][gcm]")
{
    Fixture f({}, rsx_test_config());
    GuestThread g(f, 0, "rsx");
    u8* b = f.base();
    lv2::PpuThread& t = *g.t;
    REQUIRE(lv2::sys_rsx_memory_allocate(t, kOut, kOut + 8, 0x0F900000, 0, 0, 0, 0) == lv2::CELL_OK);
    REQUIRE(lv2::sys_rsx_device_map(t, kOut + 0x10, 0, 8) == lv2::CELL_OK);
    REQUIRE(lv2::sys_rsx_context_allocate(t, kOut + 0x18, kOut + 0x20, kOut + 0x28, kOut + 0x30, 0, 0) == lv2::CELL_OK);
    REQUIRE(f.kernel->rsx() != nullptr);
    CHECK(lv2::sys_rsx_memory_free(t, ps_read32(b, kOut)) == lv2::err(lv2::CELL_EBUSY)); // the context still uses it
    CHECK(lv2::sys_rsx_context_free(t, 0x1234) == lv2::err(lv2::CELL_EINVAL));
    REQUIRE(lv2::sys_rsx_context_free(t, 0x55555555) == lv2::CELL_OK);
    CHECK(f.kernel->rsx() == nullptr);
    CHECK(lv2::sys_rsx_context_free(t, 0x55555555) == lv2::err(lv2::CELL_EINVAL));
    CHECK(lv2::sys_rsx_context_iomap(t, 0x55555555, 0x00300000, kIo, 0x100000, 0) == lv2::err(lv2::CELL_EINVAL));
    REQUIRE(lv2::sys_rsx_memory_free(t, ps_read32(b, kOut)) == lv2::CELL_OK);
    CHECK(!f.mem.is_committed(PS_MEM_RSX_LOCAL_BASE));
    // Both again: a new context with its RSX thread.
    REQUIRE(lv2::sys_rsx_memory_allocate(t, kOut, kOut + 8, 0x0F900000, 0, 0, 0, 0) == lv2::CELL_OK);
    REQUIRE(lv2::sys_rsx_context_allocate(t, kOut + 0x18, kOut + 0x20, kOut + 0x28, kOut + 0x30, 0, 0) == lv2::CELL_OK);
    CHECK(f.kernel->rsx() != nullptr);
    CHECK(ps_read32(b, kOut + 0x18) == 0x55555555);
}
