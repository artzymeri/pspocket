// Method handlers with side effects in guest memory or the backend: NV0039 copies, NV308A pixel uploads, NV3089
// unscaled blits, reports and notifies, flips (HLE flip command and the lv1 driver methods), user commands.

#include <catch_amalgamated.hpp>

#include "rsx_test_util.hpp"

using namespace pspocket;
using namespace pspocket::rsx;
using namespace pspocket::rsx::test;
using Kind = RecordingBackend::Kind;

TEST_CASE("NV0039 copies a 2D block from main to local memory")
{
    Fixture f;
    // Source: 4 lines of 8 bytes at main IO offset kMainDataIo + 0x100 with pitch 16.
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 16; x++) ps_write8(f.base(), Fixture::kMainDataEa + 0x100 + y * 16 + x, static_cast<u8>(y * 16 + x));
    }
    auto b = f.at(0);
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaHostBuffer, loc::kDmaFrameBuffer});
    b.method(m::NV0039_OFFSET_IN, {Fixture::kMainDataIo + 0x100, 0x5000, 16, 8, 8, 4, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 8; x++) CHECK(ps_read8(f.base(), PS_MEM_RSX_LOCAL_BASE + 0x5000 + y * 8 + x) == y * 16 + x);
    }
    // The source was flushed (a surface could live there), then the destination (a target there is written back
    // before the copy lands over it, D-110), and the destination invalidated.
    REQUIRE(f.backend.of(Kind::Flush).size() == 2);
    CHECK(f.backend.of(Kind::Flush)[0]->a == Fixture::kMainDataEa + 0x100);
    CHECK(f.backend.of(Kind::Flush)[0]->b == 16 * 3 + 8);
    CHECK(f.backend.of(Kind::Flush)[1]->a == PS_MEM_RSX_LOCAL_BASE + 0x5000);
    CHECK(f.backend.of(Kind::Flush)[1]->b == 32);
    REQUIRE(f.backend.of(Kind::Invalidate).size() == 1);
    CHECK(f.backend.of(Kind::Invalidate)[0]->a == PS_MEM_RSX_LOCAL_BASE + 0x5000);
    CHECK(f.backend.of(Kind::Invalidate)[0]->b == 32);
}

TEST_CASE("NV0039 handles overlapping and strided copies")
{
    Fixture f;
    const u32 base = PS_MEM_RSX_LOCAL_BASE + 0x6000;
    for (u32 i = 0; i < 64; i++) ps_write8(f.base(), base + i, static_cast<u8>(i));
    auto b = f.at(0);
    // Overlapping 1D move by 4 bytes within local memory: 32 bytes from offset 0x6000 to 0x6004.
    b.method(m::NV0039_SET_CONTEXT_DMA_BUFFER_IN, {loc::kDmaFrameBuffer, loc::kDmaFrameBuffer});
    b.method(m::NV0039_OFFSET_IN, {0x6000, 0x6004, 32, 32, 32, 1, 0x101});
    b.method(m::NV0039_BUFFER_NOTIFY, 0);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    for (u32 i = 0; i < 32; i++) CHECK(ps_read8(f.base(), base + 4 + i) == i);

    // Gather every 4th byte (input column stride 4) into packed bytes.
    for (u32 i = 0; i < 16; i++) ps_write8(f.base(), base + 0x100 + i, static_cast<u8>(0xa0 + i));
    auto c = f.at(b.offset());
    c.method(m::NV0039_OFFSET_IN, {0x6100, 0x6200, 16, 4, 4, 1, (1u << 8) | 4});
    c.method(m::NV0039_BUFFER_NOTIFY, 0);
    REQUIRE(f.submit(c) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());
    for (u32 i = 0; i < 4; i++) CHECK(ps_read8(f.base(), base + 0x200 + i) == 0xa0 + i * 4);
}

TEST_CASE("NV308A writes pixels into the NV3062 destination surface")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN, loc::kDmaFrameBuffer);
    b.method(m::NV3062_SET_COLOR_FORMAT, {10, (256u << 16) | 64, 0, 0x7000}); // A8R8G8B8, pitch 256, dst 0x7000
    b.method(m::NV308A_POINT, {(3u << 16) | 2, 4, 4});                          // x 2, y 3, size out 4 wide
    b.method(m::NV308A_COLOR, {0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00, 0xdeadbeef}); // 5th is clipped
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());
    const u32 row = PS_MEM_RSX_LOCAL_BASE + 0x7000 + 3 * 256;
    CHECK(f.read32(row + 2 * 4) == 0x11223344);
    CHECK(f.read32(row + 3 * 4) == 0x55667788);
    CHECK(f.read32(row + 5 * 4) == 0xddeeff00);
    CHECK(f.read32(row + 6 * 4) == 0);
    CHECK(ps_read8(f.base(), row + 2 * 4) == 0x11); // big-endian ARGB in memory

    // R5G6B5 destination converts from ARGB8.
    auto c = f.at(b.offset());
    c.method(m::NV3062_SET_COLOR_FORMAT, 4);
    c.method(m::NV308A_POINT, 0);
    c.method(m::NV308A_COLOR, 0xffff0000); // red
    REQUIRE(f.submit(c) == Rsx::RunStatus::Idle);
    CHECK(ps_read16(f.base(), PS_MEM_RSX_LOCAL_BASE + 0x7000) == 0xf800);
}

TEST_CASE("NV3089 performs unscaled copies and reports unsupported configurations")
{
    Fixture f;
    // Source image 8x4 A8R8G8B8 at local 0x9000, pitch 32
    for (u32 y = 0; y < 4; y++) {
        for (u32 x = 0; x < 8; x++) f.write32(PS_MEM_RSX_LOCAL_BASE + 0x9000 + y * 32 + x * 4, (y << 8) | x);
    }
    auto b = f.at(0);
    b.method(m::NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN, loc::kDmaFrameBuffer);
    b.method(m::NV3062_SET_COLOR_FORMAT, {10, (64u << 16) | 64, 0, 0xa000}); // dst pitch 64
    b.method(m::NV3089_SET_CONTEXT_DMA_IMAGE, loc::kDmaFrameBuffer);
    b.method(m::NV3089_SET_CONTEXT_SURFACE, 0x313371C3);
    b.method(m::NV3089_SET_COLOR_FORMAT, {3, 3}); // A8R8G8B8, SRCCOPY
    b.method(m::NV3089_CLIP_POINT, {0, (2u << 16) | 3, (1u << 16) | 1, (2u << 16) | 3, 0x100000, 0x100000});
    b.method(m::NV3089_IMAGE_IN_SIZE, {(4u << 16) | 8, (2u << 16) | 32, 0x9000});
    b.method(m::NV3089_IMAGE_IN, (1u * 16 << 16) | (2u * 16)); // source (2, 1), corner origin
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());
    // 3x2 block from source (2,1) lands at destination (1,1)
    for (u32 y = 0; y < 2; y++) {
        for (u32 x = 0; x < 3; x++) {
            CHECK(f.read32(PS_MEM_RSX_LOCAL_BASE + 0xa000 + (1 + y) * 64 + (1 + x) * 4) == (((1 + y) << 8) | (2 + x)));
        }
    }
    CHECK(f.read32(PS_MEM_RSX_LOCAL_BASE + 0xa000 + 1 * 64 + 4 * 4) == 0);

    // Scaled blits are not implemented yet: reported, not silently dropped.
    auto c = f.at(b.offset());
    c.method(m::NV3089_DS_DX, {0x80000, 0x100000});
    c.method(m::NV3089_IMAGE_IN, 0);
    REQUIRE(f.submit(c) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].kind == FailKind::Unsupported);
    CHECK(f.failures[0].message.find("NV3089") != std::string::npos);
}

TEST_CASE("reports and notifies write timestamps and values")
{
    Fixture f;
    // Notify area 0 lives in main memory at IO 0x0f100000.
    REQUIRE(f.rsx->io_map().map(0x0f100000, Fixture::kMainDataEa, 0x100000));
    auto b = f.at(0);
    b.method(m::NV4097_SET_CONTEXT_DMA_REPORT, loc::kDmaReportLocal);
    b.method(m::NV4097_GET_REPORT, (reports::kZcullStats << 24) | 0x20);
    b.method(m::NV4097_SET_CONTEXT_DMA_NOTIFIES, loc::kDmaNotifyMain0);
    b.method(m::NV4097_NOTIFY, 0);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());
    const u32 report = f.layout().report_addr(2);
    CHECK(ps_read64(f.base(), report) != ~0ull); // initialised to ~0, now a timestamp
    CHECK(f.read32(report + 8) == 0);
    CHECK(f.read32(report + 12) == 0);
    CHECK(ps_read64(f.base(), Fixture::kMainDataEa) != 0);
    CHECK(ps_read64(f.base(), Fixture::kMainDataEa + 8) == 0);

    // Occlusion queries are not implemented: reported, with the configured stand-in value written.
    auto c = f.at(b.offset());
    c.method(m::NV4097_GET_REPORT, (reports::kZpassPixelCount << 24) | 0x30);
    REQUIRE(f.submit(c) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].kind == FailKind::Unsupported);
    CHECK(f.read32(f.layout().report_addr(3) + 8) == f.config.fake_zpass_count);
}

TEST_CASE("the HLE flip command queues on head 1, flips the display buffer and clears the flip label")
{
    Fixture f;
    f.rsx->set_display_buffer(1, {0x100000, 1280 * 4, 1280, 720});
    std::vector<u32> flipped;
    std::vector<std::pair<u32, u32>> queued;
    std::string order;
    f.rsx->on_flip = [&](u32 id) {
        flipped.push_back(id);
        order += 'f';
    };
    f.rsx->on_queue = [&](u32 head, u32 id) {
        queued.emplace_back(head, id);
        order += 'q';
    };
    f.write32(f.layout().label_addr(1), 0x12345678);
    auto b = f.at(0);
    b.method(m::GCM_FLIP_COMMAND, 1);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    const auto flips = f.backend.of(Kind::Flip);
    REQUIRE(flips.size() == 1);
    CHECK(flips[0]->a == 1);
    REQUIRE(flips[0]->display.has_value());
    CHECK(flips[0]->display->width == 1280);
    CHECK(flips[0]->display->offset == 0x100000);
    CHECK(flipped == std::vector<u32>{1});
    // RPCS3 flip_command: the queue (its queue handler) before the flip (D-170).
    CHECK(queued == std::vector<std::pair<u32, u32>>{{1, 1}});
    CHECK(order == "qf");
    CHECK(f.read32(f.layout().label_addr(1)) == 0);
    CHECK(f.rsx->flip_count() == 1);
}

TEST_CASE("lv1 driver flip and queue methods")
{
    Fixture f;
    f.rsx->set_display_buffer(0, {0x000000, 1280 * 4, 1280, 720});
    f.rsx->set_display_buffer(2, {0x400000, 1280 * 4, 1280, 720});
    std::vector<std::pair<u32, u32>> queued;
    f.rsx->on_queue = [&](u32 head, u32 id) { queued.emplace_back(head, id); };
    auto b = f.at(0);
    b.method(m::GCM_DRIVER_QUEUE + 0, 2);              // queue buffer 2 on head 0
    b.method(m::GCM_FLIP_HEAD + 0, 0x80000000u | 0xf); // flip the last queued buffer
    b.method(m::GCM_FLIP_HEAD + 0, 0x400000);           // flip by display buffer offset
    b.method(m::GCM_FLIP_HEAD + 1, 0x80000000u | 0);    // flip buffer 0 explicitly
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.empty());
    CHECK(queued == std::vector<std::pair<u32, u32>>{{0, 2}});
    const auto flips = f.backend.of(Kind::Flip);
    REQUIRE(flips.size() == 3);
    CHECK(flips[0]->a == 2);
    CHECK(flips[1]->a == 2);
    CHECK(flips[2]->a == 0);
}

TEST_CASE("user commands drain the backend and call the HLE handler")
{
    Fixture f;
    std::vector<u32> args;
    f.rsx->on_user_command = [&](u32 a) { args.push_back(a); };
    auto b = f.at(0);
    b.method(m::GCM_SET_USER_COMMAND, 0xabc);
    b.method(m::NV4097_WAIT_FOR_IDLE, 0);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    CHECK(args == std::vector<u32>{0xabc});
    CHECK(f.backend.of(Kind::Sync).size() == 2);
}

TEST_CASE("sync_on_label waits for the GPU before label writes")
{
    Config cfg = test_config();
    cfg.sync_on_label = true;
    Fixture f(cfg);
    auto b = f.at(0);
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 64 * 0x10);
    b.method(m::NV406E_SEMAPHORE_RELEASE, 1);
    b.method(m::NV406E_SET_REFERENCE, 2);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    CHECK(f.backend.of(Kind::Sync).size() == 2);
}

TEST_CASE("translate covers every DMA context")
{
    Fixture f;
    const auto& l = f.layout();
    CHECK(f.rsx->translate(0x1000, loc::kLocal) == PS_MEM_RSX_LOCAL_BASE + 0x1000);
    CHECK(f.rsx->translate(0x1000, loc::kDmaFrameBuffer) == PS_MEM_RSX_LOCAL_BASE + 0x1000);
    CHECK_FALSE(f.rsx->translate(0x0F900000, loc::kLocal).has_value()); // past usable local memory
    CHECK(f.rsx->translate(0x10, loc::kMain) == Fixture::kCmdEa + 0x10);
    CHECK(f.rsx->translate(Fixture::kMainDataIo + 4, loc::kDmaHostBuffer) == Fixture::kMainDataEa + 4);
    CHECK_FALSE(f.rsx->translate(0x00800000, loc::kMain).has_value());
    CHECK(f.rsx->translate(0x20, loc::kDmaSemaphoreR) == l.reports_addr + 0x20);
    CHECK(f.rsx->translate(0x20, loc::kDmaReportLocal) == l.reports_addr + reports::kReportOffset + 0x20);
    CHECK(f.rsx->translate(0x30, loc::kDmaDeviceRW) == l.device_addr + 0x30);
    CHECK_FALSE(f.rsx->translate(0x1000, loc::kDmaSemaphoreRW).has_value());
    CHECK_FALSE(f.rsx->translate(0, 0x12345678).has_value());
    // A range that runs off the mapped IO pages is rejected
    CHECK_FALSE(f.rsx->translate(Fixture::kMainDataIo + 0x100000 - 4, loc::kMain, 16).has_value());
}

TEST_CASE("IO map semantics")
{
    IoMap io;
    CHECK_FALSE(io.io_to_ea(0).has_value());
    REQUIRE(io.map(0x100000, 0x20000000, 0x200000));
    CHECK(io.io_to_ea(0x100004) == 0x20000004u);
    CHECK(io.io_to_ea(0x2fffff) == 0x201fffffu);
    CHECK(io.ea_to_io(0x20100010) == 0x200010u);
    CHECK(io.is_mapped(0x100000, 0x200000));
    CHECK_FALSE(io.is_mapped(0x100000, 0x200001));
    CHECK_FALSE(io.map(0x100, 0x20000000, 0x100000));         // unaligned
    CHECK_FALSE(io.map(0x1ff00000, 0x20000000, 0x200000));    // past 512 MB
    // Remapping an IO page drops the old EA's reverse entry
    REQUIRE(io.map(0x100000, 0x30000000, 0x100000));
    CHECK(io.io_to_ea(0x100000) == 0x30000000u);
    CHECK_FALSE(io.ea_to_io(0x20000000).has_value());
    REQUIRE(io.unmap(0x100000, 0x200000));
    CHECK_FALSE(io.io_to_ea(0x100000).has_value());
    CHECK_FALSE(io.ea_to_io(0x30000000).has_value());
}
