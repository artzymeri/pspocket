// cellSpursJq (D-537) without the firmware: the attributes, creation's refusals (and its answer when the installed
// system software, which holds the job queue's policy module, is not there), the command ring's two push protocols on a
// queue laid out as _cellSpursCreateJobQueue lays it out, semaphores, a port, and GetSuspendedJobSize's answers for
// made-up jobs (the values libspurs_jq gives, from tests/hle/test_spurs_jq_compare.cpp's runs). No game data.

#include <catch_amalgamated.hpp>

#include <cstring>

#include "hle_test_util.hpp"
#include "pspocket/core/be.h"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kSpursAttr = kData + 0x0000, kSpurs = kData + 0x1000, kAttr = kData + 0x4000, kPrio = kData + 0x4400;
constexpr u32 kOut = kData + 0x4500, kJq = kData + 0x5000, kList = kData + 0x7000, kSem = kData + 0x9000;
constexpr u32 kPort = kData + 0x9100, kPortBuf = kData + 0x9800, kDesc = kData + 0xA000, kBin = kData + 0xB000;
constexpr u32 kInvalPm = 0x80410A0F;

s32 r32(u64 v) { return static_cast<s32>(static_cast<u32>(v)); }
constexpr s32 err(u32 v) { return static_cast<s32>(v); }

struct Jq {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    explicit Jq(bool spurs = true)
    {
        if (!spurs) return;
        REQUIRE(t.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, 1, 200, 2016, 0) == 0);
        REQUIRE(t.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
    }

    // A queue as creation lays it out (depth `depth`, entry lock or not), running, with handle 0 open and workload 0 of
    // the SPURS instance named (none is registered: the pushes' kicks are refused, which pushes ignore).
    void lay_out(u32 depth, bool entry_lock)
    {
        std::memset(b + kJq, 0, 0x1000);
        std::memset(b + kList, 0, 0x2000);
        ps_write64(b, kJq + 0x00, 0x60);
        ps_write8(b, kJq + 0x0D, 1);
        ps_write8(b, kJq + 0x0E, 1);
        ps_write32(b, kJq + 0x10, kList);
        ps_write16(b, kJq + 0x14, static_cast<u16>(depth));
        ps_write16(b, kJq + 0x16, 0x100);
        ps_write64(b, kJq + 0x60, 0x800);
        ps_write32(b, kJq + 0x6C, kSpurs | 0);
        ps_write8(b, kJq + 0x7A, entry_lock ? 1 : 0);
        ps_write32(b, kJq + 0xF10, kSpurs);
        ps_write32(b, kJq + 0xF14, 0x380);
        ps_write32(b, kJq + 0xFF8, 0x330000);
        ps_write32(b, kJq + 0x70, 1u << 16);       // one handle open
        ps_write32(b, kJq + 0x580, 0x80000000u);  // handle 0
    }
    u64 header() { return ps_read64(b, kJq); }
    u64 entry(u32 i) { return ps_read64(b, kList + 8 * i); }
};

} // namespace

TEST_CASE("cellSpursJq: attributes, as libspurs_jq sets them", "[hle][spursjq]")
{
    Jq q(false);
    auto& t = q.t;
    u8* b = q.b;
    std::memset(b + kAttr, 0xAA, 0x20);
    CHECK(r32(t.call("cellSpursJobQueueAttributeInitialize", 0)) == err(0x80410A11));
    CHECK(t.call("cellSpursJobQueueAttributeInitialize", kAttr) == 0);
    CHECK(ps_read32(b, kAttr + 0x00) == 0xAAAAAAAA); // left as it was
    CHECK(ps_read32(b, kAttr + 0x04) == 4);
    CHECK(ps_read32(b, kAttr + 0x08) == 0x100);
    CHECK(ps_read32(b, kAttr + 0x0C) == 0);
    CHECK(ps_read32(b, kAttr + 0x10) == 0xFF);
    CHECK(r32(t.call("cellSpursJobQueueAttributeSetMaxGrab", kAttr, 0)) == err(0x80410A02));
    CHECK(r32(t.call("cellSpursJobQueueAttributeSetMaxGrab", kAttr, 17)) == err(0x80410A02));
    CHECK(t.call("cellSpursJobQueueAttributeSetMaxGrab", kAttr, 16) == 0);
    CHECK(ps_read32(b, kAttr + 4) == 16);
    CHECK(t.call("cellSpursJobQueueAttributeSetSubmitWithEntryLock", kAttr, 0x101) == 0);
    CHECK(t.call("cellSpursJobQueueAttributeSetDoBusyWaiting", kAttr, 1) == 0);
    CHECK(t.call("cellSpursJobQueueAttributeSetIsHaltOnError", kAttr, 2) == 0);
    CHECK(t.call("cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck", kAttr, 3) == 0);
    CHECK(ps_read32(b, kAttr + 0x0C) == 0x01010203); // each the argument's low byte
    for (const u32 v : {0x80u, 0x1C0u, 0x3FFu, 0x400u})
        CHECK(r32(t.call("cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", kAttr, v)) == err(0x80410A02));
    CHECK(t.call("cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", kAttr, 0x380) == 0);
    CHECK(r32(t.call("cellSpursJobQueueAttributeSetGrabParameters", kAttr, 256, 1)) == err(0x80410A02));
    CHECK(r32(t.call("cellSpursJobQueueAttributeSetGrabParameters", kAttr, 1, 0)) == err(0x80410A02));
    CHECK(t.call("cellSpursJobQueueAttributeSetGrabParameters", kAttr, 255, 3) == 0);
    CHECK(ps_read32(b, kAttr + 0x10) == 255);
    CHECK(ps_read32(b, kAttr + 0x04) == 3);
}

TEST_CASE("cellSpursJq: creation's refusals, and no policy module without the system software", "[hle][spursjq]")
{
    Jq q;
    auto& t = q.t;
    u8* b = q.b;
    REQUIRE(t.call("cellSpursJobQueueAttributeInitialize", kAttr) == 0);
    std::memset(b + kPrio, 1, 8);
    const auto create = [&](u32 spurs, u32 jq, u32 attr, u32 list, u32 depth, u32 spus) {
        return r32(t.call("_cellSpursCreateJobQueue", 1, 0x330000, spurs, jq, attr, 0, list, depth, spus, kPrio));
    };
    CHECK(create(0, kJq, kAttr, kList, 16, 1) == err(0x80410A11));
    CHECK(create(kSpurs, kJq, kAttr, 0, 16, 1) == err(0x80410A11));
    CHECK(create(kSpurs + 4, kJq, kAttr, kList, 16, 1) == err(0x80410A10));
    CHECK(create(kSpurs, kJq + 0x40, kAttr, kList, 16, 1) == err(0x80410A10));
    CHECK(create(kSpurs, kJq, kAttr, kList, 0x8001, 1) == err(0x80410A02));
    CHECK(create(kSpurs, kJq, kAttr, kList, 16, 9) == err(0x80410A02));
    CHECK(r32(t.call("_cellSpursCreateJobQueueWithJobDescriptorPool", 1, 0x330000, kSpurs, kJq, kAttr, kPortBuf, 0, 0,
              kList, 16, 1, kPrio)) == err(0x80410A11));
    // Valid arguments, but no /dev_flash here: the queue's objects are made and destroyed again, STAT.
    CHECK(create(kSpurs, kJq, kAttr, kList, 16, 1) == err(kInvalPm));
    CHECK(ps_read32(b, kJq + 0xF10) == kSpurs);
    CHECK(ps_read32(b, kJq + 0x6C) == 0); // no workload
    CHECK(r32(t.call("cellSpursJobQueueOpen", kJq, kOut)) == err(0x80410A0F)); // not running
}

TEST_CASE("cellSpursJq: the command ring, as libspurs_jq's two pushers fill it", "[hle][spursjq]")
{
    for (const bool entry_lock : {false, true}) {
        INFO("entry lock " << entry_lock);
        Jq q;
        auto& t = q.t;
        q.lay_out(16, entry_lock);
        // Every kind of command: the descriptor (size / 128 in bits 3..1), its tag and semaphore, the type, the lap bit.
        REQUIRE(t.call("_cellSpursJobQueuePushJobBody", kJq, 0, kDesc, 0x100, 5, kSem, 0) == 0);
        CHECK(q.entry(0) == ((u64{kSem} << 32) | (u64{5} << 35) | kDesc | 4 | 1));
        REQUIRE(t.call("_cellSpursJobQueuePushBody", kJq, 0, kDesc + 0x10, 0x40, 0, 0) == 0);
        CHECK(q.entry(1) == ((kDesc + 0x10) | 1u));
        REQUIRE(t.call("_cellSpursJobQueuePushJobBody2", kJq, 0, kDesc, 0x380, 15, 0, 1, 0) == 0);
        CHECK(q.entry(2) == ((u64{15} << 35) | (1ull << 34) | kDesc | 0xE | 1));
        ps_write32(q.b, kDesc + 0x200, 2);
        ps_write32(q.b, kDesc + 0x204, 0x80);
        REQUIRE(t.call("_cellSpursJobQueuePushJobListBody", kJq, 0, kDesc + 0x200, 3, 0, 0) == 0);
        CHECK(q.entry(3) == ((1ull << 32) | (u64{3} << 35) | (kDesc + 0x200) | 1));
        REQUIRE(t.call("_cellSpursJobQueuePushSync", kJq, 0, 0x8001, 0) == 0);
        CHECK(q.entry(4) == ((0x8001ull << 48) | (7ull << 32) | 1));
        REQUIRE(t.call("_cellSpursJobQueuePushFlush", kJq, 0, 0) == 0);
        CHECK(q.entry(5) == ((3ull << 32) | 1));
        // The handle must be open; refusals change nothing.
        CHECK(r32(t.call("_cellSpursJobQueuePushFlush", kJq, 1, 0)) == err(0x80410A02));
        CHECK(r32(t.call("_cellSpursJobQueuePushJobBody", kJq, 0, kDesc, 0x3C0, 0, 0, 0)) == err(0x80410A02));
        CHECK(r32(t.call("_cellSpursJobQueuePushJobBody", kJq, 0, kDesc, 0x100, 16, 0, 0)) == err(0x80410A02));
        CHECK(r32(t.call("_cellSpursJobQueuePushJobBody", kJq, 0, kDesc + 8, 0x100, 0, 0, 0)) == err(0x80410A10));
        // Fill the ring: the write index wraps and the write lap (bit 5) turns; then AGAIN.
        for (u32 i = 6; i < 16; i++) REQUIRE(t.call("_cellSpursJobQueuePushFlush", kJq, 0, 0) == 0);
        CHECK(q.header() == 0x40); // read 0, write 0, laps: read 1, write 0
        CHECK(r32(t.call("_cellSpursJobQueuePushFlush", kJq, 0, 0)) == err(0x80410A01));
        // The policy module takes two: the next pushes go in the new lap (bit 0 clear).
        ps_write64(q.b, kJq, (2ull << 48) | 0x40);
        REQUIRE(t.call("_cellSpursJobQueuePushFlush", kJq, 0, 0) == 0);
        CHECK(q.entry(0) == (3ull << 32));
        CHECK(((q.header() >> 32) & 0xFFFF) == 1);
    }
}

TEST_CASE("cellSpursJq: semaphores, signals and a port without SPURS running jobs", "[hle][spursjq]")
{
    Jq q;
    auto& t = q.t;
    u8* b = q.b;
    q.lay_out(16, false);
    CHECK(t.call("cellSpursJobQueueSemaphoreInitialize", kSem, kJq) == 0);
    CHECK(ps_read64(b, kSem) == 0);
    CHECK(ps_read32(b, kSem + 8) == kJq);
    CHECK(r32(t.call("cellSpursJobQueueSemaphoreTryAcquire", kSem, 1)) == err(0x80410A01));
    ps_write64(b, kSem, 5ull << 32);
    CHECK(t.call("cellSpursJobQueueSemaphoreTryAcquire", kSem, 2) == 0);
    CHECK(ps_read64(b, kSem) == 3ull << 32);
    CHECK(r32(t.call("cellSpursJobQueueSemaphoreTryAcquire", kSem, 0x10000000)) == err(0x80410A02));
    ps_write64(b, kSem, (1ull << 63) | (3ull << 32));
    CHECK(r32(t.call("cellSpursJobQueueSemaphoreTryAcquire", kSem, 1)) == err(0x80410A0A)); // a waiter: BUSY
    // A job waiting for its signal goes on the queue's list (its index byte added), the others keep the signal.
    std::memset(b + kDesc, 0, 0x80);
    CHECK(t.call("cellSpursJobQueueSendSignal", kDesc) == 0);
    CHECK(ps_read32(b, kDesc + 0x2C) == 0x00800000);
    std::memset(b + kDesc + 0x80, 0, 0x80);
    ps_write32(b, kDesc + 0x80 + 0x28, kJq);
    ps_write32(b, kDesc + 0x80 + 0x2C, 0x00C00500);
    ps_write32(b, kJq + 0x68, 0x1234);
    CHECK(t.call("cellSpursJobQueueSendSignal", kDesc + 0x80) == 0);
    CHECK(ps_read32(b, kDesc + 0x80 + 0x2C) == 0);
    CHECK(ps_read32(b, kDesc + 0x80) == 0x1234);
    CHECK(ps_read32(b, kJq + 0x68) == kDesc + 0x80 + 5);
    // A port: its own semaphore counts the jobs pushed with it; its sync takes them.
    CHECK(t.call("cellSpursJobQueuePortInitializeWithDescriptorBuffer", kPort, kJq, kPortBuf, 0x100, 2, 0) == 0);
    CHECK(ps_read32(b, kPort + 0x20) == kJq);
    CHECK(ps_read32(b, kPort + 0x24) == 1); // the next handle
    CHECK(t.call("_cellSpursJobQueuePortPushJobBody", kPort, kDesc, 0x100, 1, 1, 0) == 0);
    for (u32 i = 0; i < 0x100; i++) ps_write8(b, kDesc + 0x400 + i, static_cast<u8>(i));
    CHECK(t.call("_cellSpursJobQueuePortCopyPushJobBody", kPort, kDesc + 0x400, 0x100, 2, 1, 0) == 0);
    CHECK(std::memcmp(b + kPortBuf, b + kDesc + 0x400, 0x100) == 0);
    CHECK(t.call("_cellSpursJobQueuePortCopyPushJobBody", kPort, kDesc + 0x400, 0x100, 2, 1, 0) == 0);
    CHECK(r32(t.call("_cellSpursJobQueuePortCopyPushJobBody", kPort, kDesc + 0x400, 0x100, 2, 1, 0)) == err(0x80410A01));
    CHECK(ps_read32(b, kPort + 0x10) == 3);
    CHECK(ps_read16(b, kPort + 0x14) == 2);
    CHECK(r32(t.call("cellSpursJobQueuePortTrySync", kPort)) == err(0x80410A01));
    ps_write64(b, kPort, 3ull << 32); // the policy module released the three jobs
    CHECK(t.call("cellSpursJobQueuePortTrySync", kPort) == 0);
    CHECK(ps_read32(b, kPort + 0x10) == 0);
    CHECK(ps_read16(b, kPort + 0x14) == 0);
    CHECK(t.call("cellSpursJobQueuePortFinalize", kPort) == 0);
    CHECK(ps_read32(b, kJq + 0x580) == 0x80000000u);
}

TEST_CASE("cellSpursJq: GetSuspendedJobSize, libspurs_jq's answers", "[hle][spursjq]")
{
    Jq q(false);
    auto& t = q.t;
    u8* b = q.b;
    // A job of one 0x40-byte input, 0x20 bytes out, 16 bytes of scratch, the default stack; its binary a 4.2 job
    // runtime's (version 0x342 with the job information marker), its image 0x40 bytes plus one 0x10-byte entry.
    std::memset(b + kDesc, 0, 0x100);
    std::memset(b + kBin, 0, 0x200);
    ps_write32(b, kDesc + 0x04, kBin);
    ps_write16(b, kDesc + 0x08, 0x20);
    ps_write16(b, kDesc + 0x0A, 8);
    ps_write32(b, kDesc + 0x14, 0x40);
    ps_write32(b, kDesc + 0x18, 0x20);
    ps_write16(b, kDesc + 0x1E, 1);
    ps_write32(b, kDesc + 0x30, 0x40);
    ps_write32(b, kDesc + 0x34, kData + 0xC000);
    std::memcpy(b + kBin + 0x30, "JOBCRT Ver", 10);
    ps_write8(b, kBin + 0x3A, 0x34);
    ps_write8(b, kBin + 0x3B, 0x02);
    ps_write32(b, kBin + 0x3C, 0x80);
    ps_write32(b, kBin + 0x40, 0x40);
    std::memcpy(b + kBin + 0x90, "%SPURS JOB INFO%", 16);
    ps_write32(b, kBin + 0xA0, 0);
    ps_write32(b, kBin + 0xA4, 0x10);
    ps_write32(b, kBin + 0xA8, 0xFFFFFFFF);
    ps_write32(b, kOut, 0);
    CHECK(t.call("cellSpursJobQueueGetSuspendedJobSize", kDesc, 0x100, 1, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x2000 + 0x50 + 0x100 + 0x80 + 0x80 + 0x80 + 0x500);
    CHECK(t.call("cellSpursJobQueueGetSuspendedJobSize", kDesc, 0x100, 0, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0x2000 + 0x200 + 0x100 + 0x80 + 0x80 + 0x80 + 0x500);
    // Refusals: an old job runtime (PERM), sizes and alignments.
    ps_write8(b, kBin + 0x3A, 0x33);
    CHECK(r32(t.call("cellSpursJobQueueGetSuspendedJobSize", kDesc, 0x100, 1, kOut)) == err(0x80410A09));
    CHECK(r32(t.call("cellSpursJobQueueGetSuspendedJobSize", kDesc, 0x40, 1, kOut)) == err(0x80410A02));
    CHECK(r32(t.call("cellSpursJobQueueGetSuspendedJobSize", kDesc, 0x100, 2, kOut)) == err(0x80410A02));
    CHECK(r32(t.call("cellSpursJobQueueGetSuspendedJobSize", kDesc + 4, 0x100, 1, kOut)) == err(0x80410A10));
    CHECK(r32(t.call("cellSpursJobQueueGetSuspendedJobSize", kDesc, 0x100, 1, 0)) == err(0x80410A11));
}
