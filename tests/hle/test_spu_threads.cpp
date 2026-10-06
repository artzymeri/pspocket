// lv2 SPU thread groups (D-167): synthetic SPU ELFs built here run on the SPU interpreter in lv2 thread groups.
// Arguments and exit status, group exit, terminate, restart, mailboxes, SPU events both ways, event flag bits,
// signal notification OR mode, local store access, the group DMA window, the run event, and a process end while a
// thread waits. No game data (CLAUDE.md rule 1).

#include "hle_test_util.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kElf = kData + 0x10000, kElf2 = kData + 0x14000, kImg = kData + 0x100, kImg2 = kData + 0x110;
constexpr u32 kGroup = kData + 0x200, kThread = kData + 0x204, kCause = kData + 0x20C,
              kStatus = kData + 0x210, kQueue = kData + 0x214, kPort = kData + 0x218, kFlag = kData + 0x21C,
              kValue = kData + 0x228;
constexpr u32 kGroupAttr = kData + 0x300, kThreadAttr = kData + 0x340, kArgs = kData + 0x380, kQueueAttr = kData + 0x3C0,
              kFlagAttr = kData + 0x400, kName = kData + 0x440;
constexpr u32 kOut = kData + 0x8000;   // what the SPU programs store
constexpr u32 kBuf = 0x3800;           // their LS buffer for DMA
constexpr u32 SYS_SPU_THREAD_EVENT_USER = 1, SYS_SPU_QUEUE = 2;
constexpr u64 kUserKey = 0xFFFFFFFF53505501ull, kRunKey = 0xFFFFFFFF53505500ull;

// An SPU program at LS 0.
struct Program {
    std::vector<u32> code;
    void emit(u32 insn) { code.push_back(insn); }
    usize here() const { return code.size(); }
    void load32(u32 rt, u32 v)
    {
        emit(as::ilhu(rt, static_cast<s32>(v >> 16)));
        emit(as::iohl(rt, static_cast<s32>(v & 0xFFFF)));
    }
    // 16 bytes between LS (address in ls_reg's preferred word) and EA (ea_reg's), then waits for the tag.
    void dma16(u32 cmd, u32 ls_reg, u32 ea_reg, u32 size = 16)
    {
        constexpr u32 t = 70, tag = 5;
        emit(as::wrch(PS_MFC_LSA, ls_reg));
        emit(as::il(t, 0));
        emit(as::wrch(PS_MFC_EAH, t));
        emit(as::wrch(PS_MFC_EAL, ea_reg));
        emit(as::il(t, static_cast<s32>(size)));
        emit(as::wrch(PS_MFC_Size, t));
        emit(as::il(t, tag));
        emit(as::wrch(PS_MFC_TagID, t));
        emit(as::il(t, static_cast<s32>(cmd)));
        emit(as::wrch(PS_MFC_Cmd, t));
        emit(as::il(t, 1 << tag));
        emit(as::wrch(PS_MFC_WrTagMask, t));
        emit(as::il(t, PS_MFC_TAG_UPDATE_ALL));
        emit(as::wrch(PS_MFC_WrTagUpdate, t));
        emit(as::rdch(t, PS_MFC_RdTagStat));
    }
    // Stores register `value` (16 bytes) at EA `ea` + `offset`.
    void put(u32 value, u32 ea, u32 offset)
    {
        emit(as::il(71, kBuf));
        emit(as::stqd(value, 0, 71));
        load32(72, ea + offset);
        dma16(PS_MFC_PUT, 71, 72);
    }
    void thread_exit(u32 status)
    {
        load32(80, status);
        emit(as::wrch(PS_SPU_WrOutMbox, 80));
        emit(as::stop(0x102));
    }
    void group_exit(u32 status)
    {
        load32(80, status);
        emit(as::wrch(PS_SPU_WrOutMbox, 80));
        emit(as::stop(0x101));
    }
    void spin() { emit(as::br(0)); }
};

// The program as a 32-bit big-endian SPU ELF: one PT_LOAD at LS 0 (0x100 bytes of bss after it) and a PT_NOTE.
void write_elf(u8* b, u32 ea, const Program& p)
{
    const u32 size = static_cast<u32>(p.code.size() * 4);
    std::memset(b + ea, 0, 0x200 + size);
    static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(b + ea, kIdent, 16);
    ps_write16(b, ea + 0x10, 2);    // ET_EXEC
    ps_write16(b, ea + 0x12, 0x17); // EM_SPU
    ps_write32(b, ea + 0x14, 1);
    ps_write32(b, ea + 0x18, 0);    // entry
    ps_write32(b, ea + 0x1C, 0x34); // phoff
    ps_write16(b, ea + 0x28, 0x34);
    ps_write16(b, ea + 0x2A, 0x20);
    ps_write16(b, ea + 0x2C, 2);
    const u32 ph = ea + 0x34;
    ps_write32(b, ph + 0x00, 1);     // PT_LOAD
    ps_write32(b, ph + 0x04, 0x100); // offset
    ps_write32(b, ph + 0x08, 0);     // vaddr
    ps_write32(b, ph + 0x10, size);
    ps_write32(b, ph + 0x14, size + 0x100);
    ps_write32(b, ph + 0x18, 7);
    ps_write32(b, ph + 0x20, 4);     // PT_NOTE
    ps_write32(b, ph + 0x24, 0x80);
    ps_write32(b, ph + 0x30, 0x20);
    for (usize i = 0; i < p.code.size(); i++) ps_write32(b, ea + 0x100 + 4 * static_cast<u32>(i), p.code[i]);
}

struct SpuFixture {
    Fixture f;
    GuestThread g{f, 0, "ppu"};
    lv2::PpuThread& t = *g.t;
    u8* b = f.base();

    u32 import_image(u32 img, u32 elf, u32 elf_size)
    {
        REQUIRE(lv2::_sys_spu_image_import(t, img, elf, elf_size, 0) == lv2::CELL_OK);
        return ps_read32(b, img + 4);
    }
    u32 create_group(u32 num, s32 prio = 100, s32 type = 0)
    {
        std::memcpy(b + kName, "test", 5);
        ps_write32(b, kGroupAttr, 5);
        ps_write32(b, kGroupAttr + 4, kName);
        ps_write32(b, kGroupAttr + 8, static_cast<u32>(type));
        REQUIRE(lv2::sys_spu_thread_group_create(t, kGroup, num, prio, kGroupAttr) == lv2::CELL_OK);
        return ps_read32(b, kGroup);
    }
    u32 init_thread(u32 group, u32 spu_num, u32 img, std::array<u64, 4> args = {})
    {
        ps_write32(b, kThreadAttr, kName);
        ps_write32(b, kThreadAttr + 4, 5);
        ps_write32(b, kThreadAttr + 8, 0);
        for (u32 i = 0; i < 4; i++) ps_write64(b, kArgs + 8 * i, args[i]);
        REQUIRE(lv2::sys_spu_thread_initialize(t, kThread, group, spu_num, img, kThreadAttr, kArgs) == lv2::CELL_OK);
        return ps_read32(b, kThread);
    }
    std::pair<u32, u32> join(u32 group)
    {
        REQUIRE(lv2::sys_spu_thread_group_join(t, group, kCause, kStatus) == lv2::CELL_OK);
        return {ps_read32(b, kCause), ps_read32(b, kStatus)};
    }
    u32 queue(u32 type)
    {
        ps_write32(b, kQueueAttr, lv2::SYS_SYNC_PRIORITY);
        ps_write32(b, kQueueAttr + 4, type);
        ps_write64(b, kQueueAttr + 8, 0);
        REQUIRE(lv2::sys_event_queue_create(t, kQueue, kQueueAttr, 0, 16) == lv2::CELL_OK);
        return ps_read32(b, kQueue);
    }
};

} // namespace

TEST_CASE("SPU thread groups: arguments, thread exit status, restart, images, destroy", "[hle][spu-threads]")
{
    SpuFixture s;
    Program p; // stores $1, $3, $4, $5, $6 to the EA in $3's preferred word... from a fixed EA, then exits 0x1234
    p.put(1, kOut, 0x00);
    p.put(3, kOut, 0x10);
    p.put(4, kOut, 0x20);
    p.put(5, kOut, 0x30);
    p.put(6, kOut, 0x40);
    p.thread_exit(0x1234);
    write_elf(s.b, kElf, p);
    const u32 elf_size = 0x100 + static_cast<u32>(p.code.size() * 4);
    const u32 image = s.import_image(kImg, kElf, elf_size);
    CHECK(ps_read32(s.b, kImg) == 1); // SYS_SPU_IMAGE_TYPE_KERNEL
    CHECK(ps_read32(s.b, kImg + 8) == 0);
    REQUIRE(lv2::_sys_spu_image_get_information(s.t, kImg, kValue, kValue + 4) == lv2::CELL_OK);
    CHECK(ps_read32(s.b, kValue) == 0);
    CHECK(ps_read32(s.b, kValue + 4) == 3); // COPY, FILL (the bss), INFO
    REQUIRE(lv2::_sys_spu_image_get_segments(s.t, kImg, kOut + 0x800, 3) == lv2::CELL_OK);
    CHECK(ps_read32(s.b, kOut + 0x800) == 1);
    CHECK(ps_read32(s.b, kOut + 0x808) == elf_size - 0x100);
    CHECK(ps_read32(s.b, kOut + 0x818) == 2);
    CHECK(ps_read32(s.b, kOut + 0x830) == 4);
    (void)image;

    const u32 group = s.create_group(1);
    CHECK(group == 0x04000100);
    const u32 thread = s.init_thread(group, 0, kImg, {0x1111, 0x2222, 0x3333, 0x4444});
    CHECK(thread == (group & 0xFFFFFF));
    CHECK(lv2::sys_spu_thread_get_exit_status(s.t, thread, kValue) == lv2::err(lv2::CELL_ESTAT));
    CHECK(lv2::sys_spu_thread_initialize(s.t, kThread, group, 1, kImg, kThreadAttr, kArgs) == lv2::err(lv2::CELL_EBUSY));
    for (int run = 0; run < 2; run++) {
        INFO("run " << run);
        std::memset(s.b + kOut, 0, 0x50);
        REQUIRE(lv2::sys_spu_thread_group_start(s.t, group) == lv2::CELL_OK);
        const auto [cause, status] = s.join(group);
        CHECK(cause == 2); // SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT
        CHECK(status == 0);
        CHECK(ps_read32(s.b, kOut + 0x00) == 0x3FFF0);    // $1's preferred word
        CHECK(ps_read64(s.b, kOut + 0x10) == 0x1111);     // arguments in the preferred doublewords
        CHECK(ps_read64(s.b, kOut + 0x20) == 0x2222);
        CHECK(ps_read64(s.b, kOut + 0x30) == 0x3333);
        CHECK(ps_read64(s.b, kOut + 0x40) == 0x4444);
        REQUIRE(lv2::sys_spu_thread_get_exit_status(s.t, thread, kValue) == lv2::CELL_OK);
        CHECK(ps_read32(s.b, kValue) == 0x1234);
        // New arguments for the second run.
        for (u32 i = 0; i < 4; i++) ps_write64(s.b, kArgs + 8 * i, 0x1111 * (i + 1));
        REQUIRE(lv2::sys_spu_thread_set_argument(s.t, thread, kArgs) == lv2::CELL_OK);
    }
    REQUIRE(lv2::sys_spu_thread_group_destroy(s.t, group) == lv2::CELL_OK);
    CHECK(lv2::sys_spu_thread_group_start(s.t, group) == lv2::err(lv2::CELL_ESRCH));
    REQUIRE(lv2::_sys_spu_image_close(s.t, kImg) == lv2::CELL_OK);
    CHECK(lv2::_sys_spu_image_close(s.t, kImg) == lv2::err(lv2::CELL_ESRCH));
}

TEST_CASE("SPU thread groups: create and initialize refuse what RPCS3 refuses", "[hle][spu-threads]")
{
    SpuFixture s;
    std::memcpy(s.b + kName, "x", 2);
    ps_write32(s.b, kGroupAttr, 2);
    ps_write32(s.b, kGroupAttr + 4, kName);
    ps_write32(s.b, kGroupAttr + 8, 0);
    CHECK(lv2::sys_spu_thread_group_create(s.t, kGroup, 0, 100, kGroupAttr) == lv2::err(lv2::CELL_EINVAL)); // no threads
    CHECK(lv2::sys_spu_thread_group_create(s.t, kGroup, 7, 100, kGroupAttr) == lv2::err(lv2::CELL_EINVAL)); // 7 threads
    CHECK(lv2::sys_spu_thread_group_create(s.t, kGroup, 1, 15, kGroupAttr) == lv2::err(lv2::CELL_EINVAL));  // priority
    CHECK(lv2::sys_spu_thread_group_create(s.t, kGroup, 1, 256, kGroupAttr) == lv2::err(lv2::CELL_EINVAL));
    ps_write32(s.b, kGroupAttr + 8, 0x2); // a system type: root only
    CHECK(lv2::sys_spu_thread_group_create(s.t, kGroup, 1, 100, kGroupAttr) == lv2::err(lv2::CELL_EINVAL));
    ps_write32(s.b, kGroupAttr + 8, 0x99);
    CHECK(lv2::sys_spu_thread_group_create(s.t, kGroup, 1, 100, kGroupAttr) == lv2::err(lv2::CELL_EINVAL));
    ps_write32(s.b, kGroupAttr + 8, 0);
    ps_write32(s.b, kGroupAttr, 0x81);
    CHECK(lv2::sys_spu_thread_group_create(s.t, kGroup, 1, 100, kGroupAttr) == lv2::err(lv2::CELL_EINVAL)); // name
    // Six SPUs for groups: a 6-thread group fits, a second one is busy.
    const u32 big = s.create_group(6);
    ps_write32(s.b, kGroupAttr, 2);
    CHECK(lv2::sys_spu_thread_group_create(s.t, kGroup, 1, 100, kGroupAttr) == lv2::CELL_OK); // scheduled: shares
    REQUIRE(lv2::sys_spu_thread_group_destroy(s.t, big) == lv2::CELL_OK);
    // Start before every thread is initialized: ESTAT; a user image with a bad entry: EINVAL.
    const u32 g2 = s.create_group(2);
    CHECK(lv2::sys_spu_thread_group_start(s.t, g2) == lv2::err(lv2::CELL_ESTAT));
    ps_write32(s.b, kImg2, 0);          // USER
    ps_write32(s.b, kImg2 + 4, 0x40000); // entry past the local store
    ps_write32(s.b, kImg2 + 8, kOut);
    ps_write32(s.b, kImg2 + 12, 1);
    ps_write32(s.b, kThreadAttr, kName);
    ps_write32(s.b, kThreadAttr + 4, 2);
    ps_write32(s.b, kThreadAttr + 8, 0);
    CHECK(lv2::sys_spu_thread_initialize(s.t, kThread, g2, 0, kImg2, kThreadAttr, kArgs) == lv2::err(lv2::CELL_EINVAL));
    // A user segment that is not 16-aligned.
    ps_write32(s.b, kImg2 + 4, 0);
    ps_write32(s.b, kOut, 1);       // COPY
    ps_write32(s.b, kOut + 4, 0x8); // ls
    ps_write32(s.b, kOut + 8, 0x10);
    ps_write32(s.b, kOut + 12, kElf);
    CHECK(lv2::sys_spu_thread_initialize(s.t, kThread, g2, 0, kImg2, kThreadAttr, kArgs) == lv2::err(lv2::CELL_EINVAL));
    CHECK(lv2::sys_spu_thread_write_ls(s.t, 0x01000100, 0, 0, 3) == lv2::err(lv2::CELL_EINVAL)); // type 3
}

TEST_CASE("SPU thread groups: a group exit stops a spinning sibling; terminate ends a spinning group", "[hle][spu-threads]")
{
    SpuFixture s;
    Program spin;
    spin.spin();
    write_elf(s.b, kElf, spin);
    s.import_image(kImg, kElf, 0x104);
    Program leaver;
    leaver.group_exit(0x77);
    write_elf(s.b, kElf2, leaver);
    s.import_image(kImg2, kElf2, 0x100 + static_cast<u32>(leaver.code.size() * 4));

    const u32 group = s.create_group(2);
    s.init_thread(group, 0, kImg);
    s.init_thread(group, 1, kImg2);
    REQUIRE(lv2::sys_spu_thread_group_start(s.t, group) == lv2::CELL_OK);
    auto [cause, status] = s.join(group);
    CHECK(cause == 1); // GROUP_EXIT
    CHECK(status == 0x77);

    const u32 g2 = s.create_group(1);
    s.init_thread(g2, 0, kImg);
    REQUIRE(lv2::sys_spu_thread_group_start(s.t, g2) == lv2::CELL_OK);
    CHECK(lv2::sys_spu_thread_group_start(s.t, g2) == lv2::err(lv2::CELL_ESTAT));
    CHECK(lv2::sys_spu_thread_group_destroy(s.t, g2) == lv2::err(lv2::CELL_EBUSY));
    // Suspend and resume while it spins, then terminate.
    REQUIRE(lv2::sys_spu_thread_group_suspend(s.t, g2) == lv2::CELL_OK);
    REQUIRE(lv2::sys_spu_thread_group_resume(s.t, g2) == lv2::CELL_OK);
    CHECK(lv2::sys_spu_thread_group_resume(s.t, g2) == lv2::err(lv2::CELL_ESTAT));
    REQUIRE(lv2::sys_spu_thread_group_terminate(s.t, g2, 0x55) == lv2::CELL_OK);
    std::tie(cause, status) = s.join(g2);
    CHECK(cause == 4); // TERMINATED
    CHECK(status == 0x55);
    CHECK(lv2::sys_spu_thread_group_terminate(s.t, g2, 0) == lv2::err(lv2::CELL_ESTAT)); // not running
}

TEST_CASE("SPU thread groups: inbound mailbox, send_event to a PPU queue, receive_event from an SPU queue",
    "[hle][spu-threads]")
{
    SpuFixture s;
    Program p;
    // v = read_in_mbox(); send_event(port 5, data0 = v, data1 = v + 1) -> its answer in the inbound mailbox
    p.emit(as::rdch(10, PS_SPU_RdInMbox));
    p.emit(as::ai(11, 10, 1));
    p.emit(as::wrch(PS_SPU_WrOutMbox, 11));
    p.emit(as::ilhu(12, 0x0500));
    p.emit(as::or_(12, 12, 10));
    p.emit(as::wrch(PS_SPU_WrOutIntrMbox, 12));
    p.emit(as::rdch(13, PS_SPU_RdInMbox)); // send_event's result
    p.put(13, kOut, 0x00);
    // receive_event(SPU queue 0x10): result, data1..3 from the inbound mailbox
    p.emit(as::il(14, 0x10));
    p.emit(as::wrch(PS_SPU_WrOutMbox, 14));
    p.emit(as::stop(0x110));
    for (u32 i = 0; i < 4; i++) {
        p.emit(as::rdch(20 + i, PS_SPU_RdInMbox));
        p.put(20 + i, kOut, 0x10 * (i + 1));
    }
    p.thread_exit(0);
    write_elf(s.b, kElf, p);
    s.import_image(kImg, kElf, 0x100 + static_cast<u32>(p.code.size() * 4));
    const u32 group = s.create_group(1);
    const u32 thread = s.init_thread(group, 0, kImg);

    const u32 ppu_queue = s.queue(1); // SYS_PPU_QUEUE
    const u32 spu_queue = s.queue(SYS_SPU_QUEUE);
    REQUIRE(lv2::sys_spu_thread_connect_event(s.t, thread, ppu_queue, SYS_SPU_THREAD_EVENT_USER, 5) == lv2::CELL_OK);
    CHECK(lv2::sys_spu_thread_connect_event(s.t, thread, ppu_queue, SYS_SPU_THREAD_EVENT_USER, 5) == lv2::err(lv2::CELL_EISCONN));
    CHECK(lv2::sys_spu_thread_connect_event(s.t, thread, ppu_queue, 2, 6) == lv2::err(lv2::CELL_EINVAL));
    CHECK(lv2::sys_spu_thread_bind_queue(s.t, thread, ppu_queue, 0x11) == lv2::err(lv2::CELL_EINVAL)); // a PPU queue
    REQUIRE(lv2::sys_spu_thread_bind_queue(s.t, thread, spu_queue, 0x10) == lv2::CELL_OK);
    CHECK(lv2::sys_spu_thread_bind_queue(s.t, thread, spu_queue, 0x12) == lv2::err(lv2::CELL_EBUSY));

    REQUIRE(lv2::sys_spu_thread_group_start(s.t, group) == lv2::CELL_OK);
    REQUIRE(lv2::sys_spu_thread_write_spu_mb(s.t, thread, 0x42) == lv2::CELL_OK);
    REQUIRE(lv2::sys_event_queue_receive(s.t, ppu_queue, 0, 2'000'000) == lv2::CELL_OK);
    CHECK(s.g.ctx.gpr[4] == kUserKey);
    CHECK(s.g.ctx.gpr[5] == thread);
    CHECK(s.g.ctx.gpr[6] == ((u64{5} << 32) | 0x42));
    CHECK(s.g.ctx.gpr[7] == 0x43);
    // The SPU goes on to wait on its queue (microseconds from here): the group is then WAITING, which terminate refuses.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(lv2::sys_spu_thread_group_terminate(s.t, group, 0) == lv2::err(lv2::CELL_ESTAT));
    REQUIRE(lv2::sys_event_port_create(s.t, kPort, lv2::SYS_EVENT_PORT_LOCAL, 0) == lv2::CELL_OK);
    const u32 port = ps_read32(s.b, kPort);
    REQUIRE(lv2::sys_event_port_connect_local(s.t, port, spu_queue) == lv2::CELL_OK);
    REQUIRE(lv2::sys_event_port_send(s.t, port, 0x101, 0x202, 0x303) == lv2::CELL_OK);
    const auto [cause, status] = s.join(group);
    CHECK(cause == 2);
    CHECK(ps_read32(s.b, kOut + 0x00) == 0);     // send_event: CELL_OK
    CHECK(ps_read32(s.b, kOut + 0x10) == 0);     // receive_event: CELL_OK
    CHECK(ps_read32(s.b, kOut + 0x20) == 0x101);
    CHECK(ps_read32(s.b, kOut + 0x30) == 0x202);
    CHECK(ps_read32(s.b, kOut + 0x40) == 0x303);
}

TEST_CASE("SPU thread groups: event flag bits, signal notification OR mode, local store access, the run event",
    "[hle][spu-threads]")
{
    SpuFixture s;
    Program p;
    // event_flag_set_bit(flag in $3's preferred doubleword, bit 5) with its answer; then SNR1 (OR mode) to kOut + 0x10
    p.emit(as::rotqbyi(3, 3, 4)); // the doubleword's low word into the preferred slot
    p.emit(as::wrch(PS_SPU_WrOutMbox, 3));
    p.load32(10, (128u << 24) | 5);
    p.emit(as::wrch(PS_SPU_WrOutIntrMbox, 10));
    p.emit(as::rdch(11, PS_SPU_RdInMbox));
    p.put(11, kOut, 0x00);
    p.emit(as::rdch(12, PS_SPU_RdSigNotify1));
    p.put(12, kOut, 0x10);
    p.emit(as::lqa(13, 0x3000)); // what the PPU wrote into the local store
    p.put(13, kOut, 0x20);
    p.thread_exit(0);
    write_elf(s.b, kElf, p);
    s.import_image(kImg, kElf, 0x100 + static_cast<u32>(p.code.size() * 4));

    ps_write32(s.b, kFlagAttr, lv2::SYS_SYNC_PRIORITY);
    ps_write32(s.b, kFlagAttr + 4, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write64(s.b, kFlagAttr + 8, 0);
    ps_write32(s.b, kFlagAttr + 16, 0);
    ps_write32(s.b, kFlagAttr + 20, 0x10000); // SYS_SYNC_WAITER_SINGLE
    ps_write64(s.b, kFlagAttr + 24, 0);
    REQUIRE(lv2::sys_event_flag_create(s.t, kFlag, kFlagAttr, 0) == lv2::CELL_OK);
    const u32 flag = ps_read32(s.b, kFlag);

    const u32 group = s.create_group(1);
    const u32 thread = s.init_thread(group, 0, kImg, {flag, 0, 0, 0});
    const u32 run_queue = s.queue(1);
    REQUIRE(lv2::sys_spu_thread_group_connect_event(s.t, group, run_queue, 1) == lv2::CELL_OK);
    CHECK(lv2::sys_spu_thread_group_connect_event(s.t, group, run_queue, 1) == lv2::err(lv2::CELL_EBUSY));
    CHECK(lv2::sys_spu_thread_group_connect_event(s.t, group, run_queue, 4) == lv2::err(lv2::CELL_EINVAL));
    REQUIRE(lv2::sys_spu_thread_set_spu_cfg(s.t, thread, 1) == lv2::CELL_OK); // SNR1 accumulates
    REQUIRE(lv2::sys_spu_thread_get_spu_cfg(s.t, thread, kValue) == lv2::CELL_OK);
    CHECK(ps_read64(s.b, kValue) == 1);
    CHECK(lv2::sys_spu_thread_set_spu_cfg(s.t, thread, 4) == lv2::err(lv2::CELL_EINVAL));
    CHECK(lv2::sys_spu_thread_write_ls(s.t, thread, 0x3000, 0x5A5A, 4) == lv2::err(lv2::CELL_ESTAT)); // not running

    REQUIRE(lv2::sys_spu_thread_group_start(s.t, group) == lv2::CELL_OK);
    REQUIRE(lv2::sys_event_queue_receive(s.t, run_queue, 0, 2'000'000) == lv2::CELL_OK);
    CHECK(s.g.ctx.gpr[4] == kRunKey);
    CHECK(s.g.ctx.gpr[5] == group);
    REQUIRE(lv2::sys_event_flag_wait(s.t, flag, 1u << 5, lv2::SYS_EVENT_FLAG_WAIT_AND, kValue, 2'000'000) == lv2::CELL_OK);
    // The thread now waits on SNR1: write the local store, then the two signals it ORs together.
    REQUIRE(lv2::sys_spu_thread_write_ls(s.t, thread, 0x3000, 0x5A5A5A5A, 4) == lv2::CELL_OK);
    REQUIRE(lv2::sys_spu_thread_read_ls(s.t, thread, 0x3000, kValue, 4) == lv2::CELL_OK);
    CHECK(ps_read64(s.b, kValue) == 0x5A5A5A5A);
    CHECK(lv2::sys_spu_thread_write_snr(s.t, thread, 2, 1) == lv2::err(lv2::CELL_EINVAL));
    REQUIRE(lv2::sys_spu_thread_write_snr(s.t, thread, 0, 0x10) == lv2::CELL_OK);
    REQUIRE(lv2::sys_spu_thread_write_snr(s.t, thread, 0, 0x01) == lv2::CELL_OK);
    s.join(group);
    CHECK(ps_read32(s.b, kOut + 0x00) == 0); // event_flag_set_bit: CELL_OK
    const u32 snr = ps_read32(s.b, kOut + 0x10);
    CHECK((snr == 0x11 || snr == 0x10)); // both signals ORed, unless the SPU read between them
    CHECK(ps_read32(s.b, kOut + 0x20) == 0x5A5A5A5A);
}

TEST_CASE("SPU thread groups: a thread reaches another's local store and signal register through the group window",
    "[hle][spu-threads]")
{
    SpuFixture s;
    // Thread on SPU 0: writes 16 bytes into SPU 1's local store at 0x3000, then signals SPU 1's SNR1.
    Program sender;
    sender.load32(10, 0xC0FFEE);
    sender.emit(as::il(11, kBuf));
    sender.emit(as::stqd(10, 0, 11));
    sender.load32(12, 0xF0100000u + 0x3000);
    sender.dma16(PS_MFC_PUT, 11, 12);
    sender.load32(13, 1);
    sender.emit(as::rotqbyi(13, 13, 4)); // into the last word (byte 12 takes byte 0): a 4-byte DMA keeps the low bits
    sender.emit(as::stqd(13, 0, 11));
    sender.emit(as::ai(11, 11, 0xC));
    sender.load32(12, 0xF0100000u + 0x5400C);
    sender.dma16(PS_MFC_SNDSIG, 11, 12, 4);
    sender.thread_exit(0);
    write_elf(s.b, kElf, sender);
    s.import_image(kImg, kElf, 0x100 + static_cast<u32>(sender.code.size() * 4));
    // Thread on SPU 1: waits for SNR1, then copies its local store's 0x3000 out.
    Program receiver;
    receiver.emit(as::rdch(10, PS_SPU_RdSigNotify1));
    receiver.emit(as::il(11, 0x3000));
    receiver.load32(12, kOut);
    receiver.dma16(PS_MFC_PUT, 11, 12);
    receiver.thread_exit(0);
    write_elf(s.b, kElf2, receiver);
    s.import_image(kImg2, kElf2, 0x100 + static_cast<u32>(receiver.code.size() * 4));

    const u32 group = s.create_group(2);
    s.init_thread(group, 1, kImg2);
    s.init_thread(group, 0, kImg);
    std::memset(s.b + kOut, 0, 16);
    REQUIRE(lv2::sys_spu_thread_group_start(s.t, group) == lv2::CELL_OK);
    const auto [cause, status] = s.join(group);
    CHECK(cause == 2);
    CHECK(ps_read32(s.b, kOut) == 0xC0FFEE);
}

TEST_CASE("SPU thread groups: the process can end while a thread waits on its queue", "[hle][spu-threads]")
{
    auto s = std::make_unique<SpuFixture>();
    Program p;
    p.emit(as::il(14, 0x10));
    p.emit(as::wrch(PS_SPU_WrOutMbox, 14));
    p.emit(as::stop(0x110));
    p.thread_exit(0);
    write_elf(s->b, kElf, p);
    s->import_image(kImg, kElf, 0x100 + static_cast<u32>(p.code.size() * 4));
    const u32 group = s->create_group(1);
    const u32 thread = s->init_thread(group, 0, kImg);
    const u32 spu_queue = s->queue(SYS_SPU_QUEUE);
    REQUIRE(lv2::sys_spu_thread_bind_queue(s->t, thread, spu_queue, 0x10) == lv2::CELL_OK);
    REQUIRE(lv2::sys_spu_thread_group_start(s->t, group) == lv2::CELL_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(s->f.kernel->shutdown(std::chrono::milliseconds(3000)));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    s.reset();
}
