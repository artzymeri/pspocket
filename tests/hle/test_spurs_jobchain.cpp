// The native job chain policy (D-055) running synthetic SPU jobs (built in code with the SPU assembler: no game data).
// One job binary, laid out like a real one (GUID, the marker pair, code from 0x30), records what it was started with:
// its registers, its context, its input, read-only cache element and kernel context, and a counter kept in its own
// data. Chains of these jobs check libsre's entry contract and LS placement, the LS cache, END and a restart, the
// commands (NOP, CALL/RET, NEXT, JOBLIST, SYNC across two SPUs, GUARD, JTS), the error path, and that a running chain
// signalled again is left when it ends.

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "pspocket/hle/spu_programs.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/interpreter.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kSpursAttr = kData + 0x1000, kSpurs = kData + 0x2000, kJcAttr = kData + 0x4000, kPrio = kData + 0x4400;
constexpr u32 kJc = kData + 0x5000, kGuard = kData + 0x5400;
constexpr u32 kCmds = kData + 0x6000;     // command lists
constexpr u32 kDescs = kData + 0x8000;    // descriptors, 128 bytes each
constexpr u32 kJobList = kData + 0x9800;  // CellSpursJobList
constexpr u32 kListDescs = kData + 0xA000;
constexpr u32 kInput = kData + 0xC000;    // 0x40 bytes of input per job
constexpr u32 kCache = kData + 0xD000;    // the cache element (0x20 bytes)
constexpr u32 kOut = kData + 0x10000;     // 0x100 per job: the record (0xC0) and its done word (0xC0)
constexpr u32 kStarted = kData + 0x14000; // 16 bytes per job
constexpr u32 kBin = kData + 0x20000, kBin2 = kData + 0x30000; // up to 64 KB each
constexpr u32 kBinSize = 0x220; // code, and the counter at +0x200

constexpr u64 kSync = 0x02, kRet = 0x77, kEnd = 0x7F, kJts = 0x0000000800000012ull;
constexpr u64 call(u32 ea) { return ea | 4u; }
constexpr u64 next(u32 ea) { return ea | 3u; }
constexpr u64 joblist(u32 ea) { return ea | 6u; }
constexpr u64 guard(u32 ea) { return ea | 0x0Fu; }

u32 desc(u32 i) { return kDescs + 0x80 * i; }
u32 out(u32 i) { return kOut + 0x100 * i; }

// The recorder job. Its record at the work area's output EA: 0x00 $3, 0x10 $4, 0x20 $126, 0x30 $1, 0x40..0x6F its
// context, 0x70 and 0xB0 its input's first two quadwords, 0x80 the counter (+1 per run, kept in the binary's data),
// 0x90 the cache element's first quadword, 0xA0 LS 0x1C0 (the kernel context's CellSpurs EA and SPU number); then a
// done word at +0xC0. The work area (descriptor 0x60): the output EA, a delay (loop iterations before the record), the
// EA of a "started" record (16 bytes) written before the delay.
std::vector<u32> recorder()
{
    std::vector<u32> c = {as::ila(2, 0x10000), as::ila(2, 0x20000), as::ila(2, 0x30000), as::ila(2, 0x3F000),
        as::xori(80, 80, 4), as::br(4), as::xori(80, 80, 4), as::br(0x30 - 0x1C), 0, 0, 0, 0};
    const auto e = [&](u32 w) { c.push_back(w); };
    const auto put = [&](u32 ls_reg, u32 ea_reg, s32 size) { // on the context's tag ($19), then wait for it
        e(as::wrch(PS_MFC_LSA, ls_reg));
        e(as::il(23, 0));
        e(as::wrch(PS_MFC_EAH, 23));
        e(as::wrch(PS_MFC_EAL, ea_reg));
        e(as::il(23, size));
        e(as::wrch(PS_MFC_Size, 23));
        e(as::wrch(PS_MFC_TagID, 19));
        e(as::il(23, PS_MFC_PUTF));
        e(as::wrch(PS_MFC_Cmd, 23));
        e(as::il(23, 1));
        e(as::shl(23, 23, 19));
        e(as::wrch(PS_MFC_WrTagMask, 23));
        e(as::il(23, PS_MFC_TAG_UPDATE_ALL));
        e(as::wrch(PS_MFC_WrTagUpdate, 23));
        e(as::rdch(23, PS_MFC_RdTagStat));
    };
    e(as::lqd(10, 0x10, 3));
    e(as::rotqbyi(11, 10, 12)); // $11: oBuffer (context 0x1C)
    e(as::stqd(3, 0x00, 11));
    e(as::stqd(4, 0x10, 11));
    e(as::stqd(126, 0x20, 11));
    e(as::stqd(1, 0x30, 11));
    for (s32 q = 0; q < 3; q++) {
        e(as::lqd(12, 0x10 * q, 3));
        e(as::stqd(12, 0x40 + 0x10 * q, 11));
    }
    e(as::lqd(13, 0x00, 3)); // ioBuffer
    e(as::lqd(14, 0x00, 13));
    e(as::stqd(14, 0x70, 11));
    e(as::lqd(14, 0x10, 13));
    e(as::stqd(14, 0xB0, 11));
    e(as::lqd(15, 0x200, 126)); // the counter, in the binary's own data
    e(as::ai(15, 15, 1));
    e(as::stqd(15, 0x200, 126));
    e(as::stqd(15, 0x80, 11));
    e(as::lqd(16, 0x00, 3));
    e(as::rotqbyi(16, 16, 4)); // cacheBuffer[0]
    e(as::lqd(17, 0x00, 16));
    e(as::stqd(17, 0x90, 11));
    e(as::lqa(18, 0x1C0));
    e(as::stqd(18, 0xA0, 11));
    e(as::lqd(19, 0x20, 3));
    e(as::rotqbyi(19, 19, 4)); // $19: dmaTag
    e(as::lqd(20, 0x60, 4));   // the work area
    e(as::rotqbyi(21, 20, 8)); // started EA
    const usize skip = c.size();
    e(0);
    put(11, 21, 0x10);
    c[skip] = as::brz(21, static_cast<s32>(c.size() - skip) * 4);
    e(as::rotqbyi(22, 20, 4)); // delay
    const usize loop = c.size();
    e(0);
    e(as::ai(22, 22, -1));
    e(as::br(static_cast<s32>(loop) * 4 - static_cast<s32>(c.size()) * 4));
    c[loop] = as::brz(22, static_cast<s32>(c.size() - loop) * 4);
    put(11, 20, 0xC0); // the record
    e(as::ai(24, 11, 0x80));
    e(as::ai(25, 20, 0xC0));
    put(24, 25, 0x10); // then the done word
    e(as::bi(0));
    return c;
}

// A 128-byte descriptor for the recorder: input elements {size, EA}, one cache element, buffer sizes, and the work
// area (the output's EA, a delay, the "started" EA).
struct Desc {
    u32 out = 0;
    std::vector<std::pair<u32, u32>> inputs = {{0x40, kInput}};
    u32 delay = 0, started = 0;
    u16 scratch = 0x10; // 16-byte units
    u16 stack = 0;      // 16-byte units; 0: 8 KB
    u32 in_size = 0x100;
    u32 binary = kBin, binary_size = kBinSize;
};

void write_desc(u8* base, u32 at, const Desc& d)
{
    std::memset(base + at, 0, 0x80);
    ps_write32(base, at + 0x04, d.binary);
    ps_write16(base, at + 0x08, static_cast<u16>(d.binary_size / 16));
    ps_write16(base, at + 0x0A, static_cast<u16>(8 * d.inputs.size()));
    ps_write32(base, at + 0x14, d.in_size);
    ps_write32(base, at + 0x18, 0xC0); // the output buffer: the record
    ps_write16(base, at + 0x1C, d.stack);
    ps_write16(base, at + 0x1E, d.scratch);
    ps_write32(base, at + 0x24, 8); // one cache element
    u32 p = at + 0x30;
    for (const auto& [size, ea] : d.inputs) {
        ps_write32(base, p, size);
        ps_write32(base, p + 4, ea);
        p += 8;
    }
    ps_write32(base, p, 0x20);
    ps_write32(base, p + 4, kCache);
    ps_write32(base, at + 0x60, d.out);
    ps_write32(base, at + 0x64, d.delay);
    ps_write32(base, at + 0x68, d.started);
}

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return true;
}

// One SPURS instance and one job chain at kCmds (descriptors of 128 bytes, autoReadyCount, tags 4 and 5) over the
// recorder binary.
struct ChainFixture {
    Fixture f;
    GuestThread a{f, 0, "ppu"};
    u8* base = f.base();

    explicit ChainFixture(u32 nspus = 1, u32 max_grab = 4, u32 max_contention = 1)
    {
        REQUIRE(a.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, nspus, 200, 2016, 0) == 0);
        REQUIRE(a.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
        const std::vector<u32> code = recorder();
        REQUIRE(code.size() * 4 <= 0x200);
        std::memset(base + kBin, 0, kBinSize);
        for (usize i = 0; i < code.size(); i++) ps_write32(base, kBin + 4 * static_cast<u32>(i), code[i]);
        for (u32 i = 0; i < 0x200; i++) ps_write8(base, kInput + i, static_cast<u8>(0x40 + i));
        for (u32 i = 0; i < 0x20; i++) ps_write8(base, kCache + i, static_cast<u8>(0xC0 + i));
        std::memset(base + kPrio, 1, 8);
        REQUIRE(a.call("_cellSpursJobChainAttributeInitialize", 3, 0x330000, kJcAttr, kCmds, 0x80, max_grab, kPrio,
                    max_contention, 1, 4, 5, 0, 0x100, 0) == 0);
        REQUIRE(a.call("cellSpursCreateJobChainWithAttribute", kSpurs, kJc, kJcAttr) == 0);
    }
    ~ChainFixture() { CHECK(a.call("cellSpursFinalize", kSpurs) == 0); }

    void run() { REQUIRE(a.call("cellSpursRunJobChain", kJc) == 0); }
    // A command the chain's SPU may be reading: the PPU's store after its lwsync (D-585).
    void cmd(u32 at, u64 v) { store64_release(base, at, v); }
    u32 wid() { return ps_read32(base, kJc + 0x74); }
    // The done word comes in the job's last PUT, after its record: polled as the PPU polls it, with acquire order.
    bool done(u32 i) { return load32_acquire(base, out(i) + 0xC0) != 0; }
    bool wait_done(u32 i) { return wait_for([&] { return done(i); }); }
    // The SPU left the chain: no contention on its workload, and no signal pending (CellSpurs line 0, which the
    // workers update with reservation operations: read as one copy of the line).
    bool wait_left()
    {
        return wait_for([&] {
            const LineCopy line0 = line_copy(base, kSpurs);
            return line0.byte(0x20 + wid()) == 0 && !(line0.half(0x70) & (0x8000u >> wid()));
        });
    }
    u32 rec(u32 i, u32 offset) { return ps_read32(base, out(i) + offset); }
};

} // namespace

TEST_CASE("job chains: jobs start with libsre's registers, context and LS pages; END; the LS cache; a restart",
    "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0)});
    write_desc(b, desc(1), {.out = out(1), .inputs = {{0x40, kInput + 0x40}}});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, desc(1));
    s.cmd(kCmds + 0x10, kEnd);
    s.run();
    REQUIRE(s.wait_done(0));
    REQUIRE(s.wait_done(1));

    // The first job: its binary at the heap's first page, then its input, cache element, output and scratch + stack.
    CHECK(s.rec(0, 0x00) == 0x4940);  // $3: the context
    CHECK(s.rec(0, 0x10) == 0x3F400); // $4: the first descriptor buffer (0x3F800 - 4 * 0x100)
    CHECK(s.rec(0, 0x20) == 0x4C00);  // $126: the binary
    CHECK(s.rec(0, 0x30) == 0x7FD0);  // $sp: 0x30 below the top of scratch + stack (0x5C00 + 9 pages)
    CHECK(s.rec(0, 0x34) == 0x1FD0);  // the 8 KB stack, less 0x30 (SDK 3.3)
    CHECK(s.rec(0, 0x38) == 0x2400);  // the allocation
    CHECK(s.rec(0, 0x40) == 0x5000);  // ioBuffer
    CHECK(s.rec(0, 0x44) == 0x5400);  // cacheBuffer[0]
    CHECK(s.rec(0, 0x48) == 0);
    CHECK(ps_read8(b, out(0) + 0x54) == 0x10); // descriptor size / 128 in the high nibble
    CHECK(ps_read16(b, out(0) + 0x58) == 1);   // numIoBuffer
    CHECK(ps_read16(b, out(0) + 0x5A) == 1);   // numCacheBuffer
    CHECK(s.rec(0, 0x5C) == 0x5800);           // oBuffer
    CHECK(s.rec(0, 0x60) == 0x5C00);           // sBuffer
    CHECK(s.rec(0, 0x64) == 4);                // dmaTag: tag1
    CHECK(ps_read64(b, out(0) + 0x68) == desc(0));
    CHECK(std::memcmp(b + out(0) + 0x70, b + kInput, 16) == 0);
    CHECK(std::memcmp(b + out(0) + 0x90, b + kCache, 16) == 0);
    CHECK(ps_read64(b, out(0) + 0xA0) == kSpurs); // the kernel context stays valid while a job runs
    CHECK(s.rec(0, 0xA8) == 0);                   // SPU 0
    CHECK(s.rec(0, 0x80) == 1);

    // The second: the other tag and descriptor buffer, the binary and cache element still in LS (its counter kept).
    CHECK(s.rec(1, 0x10) == 0x3F500);
    CHECK(s.rec(1, 0x20) == 0x4C00);
    CHECK(s.rec(1, 0x40) == 0x5000);
    CHECK(s.rec(1, 0x64) == 5);
    CHECK(s.rec(1, 0x80) == 2);
    CHECK(std::memcmp(b + out(1) + 0x70, b + kInput + 0x40, 16) == 0);

    // END: the ready count goes to 0, pc stays on it, and the SPU leaves with the chain unlocked and idle.
    REQUIRE(s.wait_left());
    CHECK(ps_read8(b, kSpurs + s.wid()) == 0);
    CHECK(ps_read64(b, kJc + 0x00) == kCmds + 0x10);
    CHECK(ps_read8(b, kJc + 0x29) == 0); // lock
    CHECK(ps_read8(b, kJc + 0x25) == 0); // busy
    CHECK(ps_read32(b, kJc + 0x80) == 0);

    // The PPU rewrites the END and runs the chain again: the module starts afresh (an empty LS cache).
    write_desc(b, desc(2), {.out = out(2), .inputs = {{0x40, kInput + 0x80}}});
    s.cmd(kCmds + 0x18, kEnd);
    s.cmd(kCmds + 0x10, desc(2));
    s.run();
    REQUIRE(s.wait_done(2));
    CHECK(s.rec(2, 0x80) == 1);
    CHECK(s.rec(2, 0x20) == 0x4C00);
    CHECK(s.rec(2, 0x10) == 0x3F400);
    REQUIRE(s.wait_left());
    CHECK(ps_read64(b, kJc + 0x00) == kCmds + 0x18);
}

namespace {

std::atomic<u64> g_stepped{0};

// A "translated" recorder: an entry at every instruction that runs it in the interpreter, counted.
void step_one(SPUContext* ctx, uint8_t*)
{
    g_stepped.fetch_add(1, std::memory_order_relaxed);
    spu::step(*ctx);
}

} // namespace

TEST_CASE("job chains: a cached binary loaded with no program takes the one registered since at its next job (D-234)",
    "[hle][spurs][spurs-jobchain]")
{
    // Its program, lifted from the recorder (every word before its counter at +0x200 code).
    const ps_spu_miss_policy policy = ps_spu_get_miss_policy();
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET); // the job's return leaves the program (CLAUDE.md rule 5)
    ChainFixture s;
    u8* b = s.base;
    std::vector<ps_spu_entry> entries;
    for (u32 a = 0; a < kBinSize; a += 4) entries.push_back({0x4C00 + a, &step_one});
    const u32 code_words[(kBinSize / 4 + 31) / 32] = {~0u, ~0u, ~0u, ~0u, 0};
    ps_spu_program program{"spu_recorder", ps_spu_hash64(b + kBin, kBinSize), 0x4C00, kBinSize, PS_SPU_XFLOAT_APPROXIMATE,
        1, static_cast<u32>(entries.size()), entries.data(), nullptr, code_words,
        ps_spu_code_hash(b + kBin, kBinSize, code_words)};

    // The first job loads the binary into the LS cache (none registered: interpreted) and waits after it started;
    // the program is registered meanwhile; the second job, from the cache, runs it.
    write_desc(b, desc(0), {.out = out(0), .delay = 3000000, .started = kStarted});
    write_desc(b, desc(1), {.out = out(1)});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, desc(1));
    s.cmd(kCmds + 0x10, kEnd);
    g_stepped = 0;
    const u64 before = hle::spu_program_stats().reidentified;
    s.run();
    REQUIRE(wait_for([&] { return load32_acquire(b, kStarted) != 0; }));
    ps_spu_register_program(&program);
    REQUIRE(s.wait_done(0));
    REQUIRE(s.wait_done(1));
    REQUIRE(s.wait_left());
    CHECK(g_stepped.load() > 0);
    CHECK(hle::spu_program_stats().reidentified - before == 1);
    CHECK(s.rec(1, 0x80) == 2); // the binary's counter, kept in the cache
    CHECK(s.rec(1, 0x20) == 0x4C00);
    ps_spu_unregister_program(&program);
    ps_spu_set_miss_policy(policy);
}

// Regression: the previous job's scratch + stack was freed only when the next job's was allocated, after that job's
// other buffers, so a job whose buffers need those pages got MEMORY_SIZE (GTA V's Secondary chain).
TEST_CASE("job chains: a job's buffers get the pages of the previous job's scratch + stack", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    // The heap: pages 19..252. Job 0's frame takes 200 pages (0x30000 scratch + 8 KB stack); job 1's input needs 40,
    // which fit only where that frame was.
    write_desc(b, desc(0), {.out = out(0), .scratch = 0x3000});
    write_desc(b, desc(1), {.out = out(1), .in_size = 0xA000});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, desc(1));
    s.cmd(kCmds + 0x10, kEnd);
    s.run();
    REQUIRE(s.wait_done(1));
    CHECK(s.rec(0, 0x60) == 0x5C00); // job 0's frame: pages 23..222
    CHECK(s.rec(1, 0x40) == 0x5800); // job 1's input: from page 22, over job 0's frame
    CHECK(s.rec(1, 0x5C) == 0x5000); // its output: the 1-page gap job 0's input left
    CHECK(s.rec(1, 0x60) == 0xF800); // its frame: after the input
    CHECK(ps_read32(b, kJc + 0x80) == 0);
    REQUIRE(s.wait_left());
}

// Regression: a job that fits the heap only when packed (GTA V's Secondary chain: 232 of 234 pages) after a job with
// another binary got MEMORY_SIZE: its binary went in after the cached one, in the middle of the pages, and no room was
// left in one piece for its scratch + stack.
TEST_CASE("job chains: a job that fits only when packed is laid out again in an emptied heap", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    std::memcpy(b + kBin2, b + kBin, kBinSize); // the recorder again, as another binary
    // GTA V's two jobs: 48 + 6 + 1 + 177 pages, then 40 + 6 + 1 + 185, and a 1-page cache element each (the heap:
    // pages 19..252).
    write_desc(b, desc(0), {.out = out(0), .scratch = 0x2929, .stack = 0x300, .in_size = 0x1590, .binary_size = 0xC000});
    write_desc(b, desc(1), {.out = out(1), .scratch = 0x2B40, .stack = 0x300, .in_size = 0x1590, .binary = kBin2,
                               .binary_size = 0xA000});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, desc(1));
    s.cmd(kCmds + 0x10, kEnd);
    s.run();
    REQUIRE(s.wait_done(1));
    CHECK(s.rec(1, 0x20) == 0x4C00);  // the binary at the first page again (19..58)
    CHECK(s.rec(1, 0x40) == 0xEC00);  // then the input (59..64), cache element (65), output (66), scratch + stack
    CHECK(s.rec(1, 0x44) == 0x10400); // (67..251)
    CHECK(s.rec(1, 0x5C) == 0x10800);
    CHECK(s.rec(1, 0x60) == 0x10C00);
    CHECK(s.rec(1, 0x30) == 0x3EFD0);
    CHECK(ps_read32(b, kJc + 0x80) == 0);
    REQUIRE(s.wait_left());
}

TEST_CASE("job chains: NOP, CALL and RET, NEXT, JOBLIST and SYNC run in the chain's order", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    for (u32 i = 0; i < 6; i++) write_desc(b, i == 2 || i == 3 ? kListDescs + 0x80 * (i - 2) : desc(i), {.out = out(i)});
    // 0: NOP, CALL sub, JOB 1, NEXT tail; sub: JOB 0, RET; tail: JOBLIST {2, 3}, JOB 4, SYNC, JOB 5, END.
    s.cmd(kCmds + 0x00, 0);
    s.cmd(kCmds + 0x08, call(kCmds + 0x80));
    s.cmd(kCmds + 0x10, desc(1));
    s.cmd(kCmds + 0x18, next(kCmds + 0x100));
    s.cmd(kCmds + 0x80, desc(0));
    s.cmd(kCmds + 0x88, kRet);
    s.cmd(kCmds + 0x100, joblist(kJobList));
    s.cmd(kCmds + 0x108, desc(4));
    s.cmd(kCmds + 0x110, kSync);
    s.cmd(kCmds + 0x118, desc(5));
    s.cmd(kCmds + 0x120, kEnd);
    ps_write32(b, kJobList + 0x0, 2);
    ps_write32(b, kJobList + 0x4, 0x80);
    ps_write64(b, kJobList + 0x8, kListDescs);
    s.run();
    for (u32 i = 0; i < 6; i++) REQUIRE(s.wait_done(i));
    // One SPU and one binary: the counter numbers the jobs in the order they ran.
    for (u32 i = 0; i < 6; i++) CHECK(s.rec(i, 0x80) == i + 1);
    REQUIRE(s.wait_left());
    CHECK(ps_read64(b, kJc + 0x00) == kCmds + 0x120);
    CHECK(ps_read64(b, kJc + 0x18) == 0); // the call stack is empty again
    CHECK(ps_read32(b, kJc + 0x50) == 0); // the job list is used up
    CHECK(ps_read64(b, kJc + 0x58) == kListDescs + 0x100);
}

TEST_CASE("job chains: a SYNC waits for the jobs before it on every SPU", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s(2, 1, 2);
    u8* b = s.base;
    constexpr u32 kSlow = 3'000'000;
    write_desc(b, desc(0), {.out = out(0), .delay = kSlow});
    write_desc(b, desc(1), {.out = out(1), .delay = kSlow});
    write_desc(b, desc(2), {.out = out(2), .inputs = {{0x10, out(0)}, {0x10, out(1)}}}); // reads both records
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, desc(1));
    s.cmd(kCmds + 0x10, kSync);
    s.cmd(kCmds + 0x18, desc(2));
    s.cmd(kCmds + 0x20, kEnd);
    s.run();
    REQUIRE(s.wait_done(2));
    CHECK(s.rec(2, 0x70) == 0x4940); // job 0's record was there when job 2 started
    CHECK(s.rec(2, 0xB0) == 0x4940); // and job 1's
    INFO("the two slow jobs ran on SPUs " << s.rec(0, 0xA8) << " and " << s.rec(1, 0xA8));
    REQUIRE(s.wait_left());
    CHECK(ps_read8(b, kJc + 0x25) == 0); // no SPU is left busy
}

TEST_CASE("job chains: a GUARD holds the chain until its count is down and the chain is run again",
    "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    std::memset(b + kGuard, 0, 128);
    ps_write32(b, kGuard + 0x00, 1); // ncount0
    ps_write32(b, kGuard + 0x04, 1); // ncount1
    ps_write64(b, kGuard + 0x08, kJc);
    ps_write32(b, kGuard + 0x10, 1); // requestSpuCount
    write_desc(b, desc(0), {.out = out(0)});
    write_desc(b, desc(1), {.out = out(1)});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, guard(kGuard));
    s.cmd(kCmds + 0x10, desc(1));
    s.cmd(kCmds + 0x18, kEnd);
    s.run();
    REQUIRE(s.wait_done(0));
    REQUIRE(s.wait_left());
    CHECK_FALSE(s.done(1));
    CHECK(ps_read32(b, kGuard + 0x30) == 1);          // the chain waits on it
    CHECK((ps_read8(b, kJc + 0x2C) & 0x08) != 0);     // and says so
    CHECK(ps_read8(b, kSpurs + s.wid()) == 0);        // ready count 0
    CHECK(ps_read64(b, kJc + 0x00) == kCmds + 0x08);  // pc on the guard

    // What cellSpursJobGuardNotify does when the count reaches 0 and the chain waits: the count, then a run.
    ps_write32(b, kGuard + 0x00, 0);
    s.run();
    REQUIRE(s.wait_done(1));
    REQUIRE(s.wait_left());
    CHECK(ps_read32(b, kGuard + 0x30) == 0);
    CHECK((ps_read8(b, kJc + 0x2C) & 0x08) == 0);
}

TEST_CASE("job chains: a JTS keeps the chain on its SPU until the PPU rewrites it", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0)});
    write_desc(b, desc(1), {.out = out(1)});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, kJts);
    s.cmd(kCmds + 0x10, kEnd);
    s.run();
    REQUIRE(s.wait_done(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(ps_read8(b, kSpurs + 0x20 + s.wid()) == 1); // still on the SPU, waiting at the JTS
    CHECK(ps_read64(b, kJc + 0x00) == kCmds + 0x08);
    s.cmd(kCmds + 0x08, desc(1)); // no RunJobChain: the waiting SPU sees it
    REQUIRE(s.wait_done(1));
    REQUIRE(s.wait_left());
    CHECK(ps_read64(b, kJc + 0x00) == kCmds + 0x10);
}

TEST_CASE("job chains: a JTS half rewritten into a JOB still waits until its high word is written (D-125)",
    "[hle][spurs][spurs-jobchain]")
{
    // The PPU writes the JOB's low word over the JTS first: in between the command is 0x00000008_<job EA>, which
    // GTA V's escape caught, and which ran as a job at that 36-bit address (an MFC fault, the game stopped).
    ChainFixture s;
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0)});
    write_desc(b, desc(1), {.out = out(1)});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, (kJts & ~0xFFFFFFFFull) | desc(1)); // the low word written, the high word not yet
    s.cmd(kCmds + 0x10, kEnd);
    s.run();
    REQUIRE(s.wait_done(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(ps_read8(b, kSpurs + 0x20 + s.wid()) == 1); // still on the SPU, waiting
    CHECK(ps_read64(b, kJc + 0x00) == kCmds + 0x08);
    s.cmd(kCmds + 0x08, desc(1)); // the high word lands
    REQUIRE(s.wait_done(1));
    REQUIRE(s.wait_left());
    CHECK(ps_read64(b, kJc + 0x00) == kCmds + 0x10);
}

TEST_CASE("job chains: an unknown command halts the chain, records the error and shuts the workload down",
    "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s(1, 1);
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0)});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, 0x1F); // opcode 7, 0x1F: no such command
    s.cmd(kCmds + 0x10, kEnd);
    s.run();
    REQUIRE(s.wait_done(0));
    REQUIRE(wait_for([&] { return line_byte(b, kJc + 0x23) == 1; })); // isHalted
    CHECK(ps_read32(b, kJc + 0x80) == 0x80410A18);                    // CELL_SPURS_JOB_ERROR_UNKNOWN_COMMAND
    CHECK(ps_read64(b, kJc + 0x88) == kCmds + 0x08);                  // the cause: the command's address
    CHECK(ps_read8(b, kSpurs + 0x80 + s.wid()) == 3);                 // SPURS_WKL_STATE_SHUTTING_DOWN
    CHECK(ps_read8(b, kJc + 0x29) == 1); // libsre leaves the lock taken when the error comes up in a visit
}

// Regression: a poll that kept the current workload did not consume its workload signal, so a chain signalled again
// while it ran kept its SPU after its END forever (the poll always found the signal and kept the chain).
TEST_CASE("job chains: a chain signalled again while it runs leaves its SPU after its END", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0), .delay = 3'000'000, .started = kStarted});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, kEnd);
    s.run();
    REQUIRE(wait_for([&] { return load32_acquire(b, kStarted) == 0x4940; }));
    s.run(); // while the job runs
    REQUIRE(s.wait_done(0));
    CHECK(s.wait_left());
}

// cellSpursShutdownJobChain and cellSpursJoinJobChain (D-136, RPCS3's checks and order): a chain shut down and joined
// gives its workload slot back, its workload ID becomes 32, and the join returns the chain's error word.
TEST_CASE("job chains: shut down and joined, a chain gives its workload back; the join returns the chain's error",
    "[hle][spurs][spurs-jobchain]")
{
    constexpr u64 kNull = 0xFFFFFFFF80410A11ull, kAlign = 0xFFFFFFFF80410A10ull, kInval = 0xFFFFFFFF80410A02ull,
                  kStat = 0xFFFFFFFF80410A0Full;
    ChainFixture s;
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0)});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, kEnd);
    s.run();
    REQUIRE(s.wait_done(0));
    const u32 wid = s.wid();
    REQUIRE(wid < 16);

    CHECK(s.a.call("cellSpursShutdownJobChain", 0) == kNull);
    CHECK(s.a.call("cellSpursShutdownJobChain", kJc + 0x40) == kAlign);
    CHECK(s.a.call("cellSpursJoinJobChain", 0) == kNull);
    CHECK(s.a.call("cellSpursJoinJobChain", kJc + 0x40) == kAlign);
    ps_write32(b, kSpurs + 0xD6C, 1); // CellSpurs::exception: the workload's STAT becomes the chain's
    CHECK(s.a.call("cellSpursShutdownJobChain", kJc) == kStat);
    CHECK(s.a.call("cellSpursJoinJobChain", kJc) == kStat);
    ps_write32(b, kSpurs + 0xD6C, 0);

    REQUIRE(s.a.call("cellSpursShutdownJobChain", kJc) == 0);
    CHECK(ps_read8(b, kSpurs + 0x80 + wid) == 4);           // SPURS_WKL_STATE_REMOVABLE
    CHECK(s.a.call("cellSpursShutdownJobChain", kJc) == 0); // again: already removable
    CHECK(s.a.call("cellSpursJoinJobChain", kJc) == 0);     // no error in the chain
    CHECK(ps_read32(b, kJc + 0x74) == 32);
    CHECK(ps_read8(b, kSpurs + 0x80 + wid) == 0); // NON_EXISTENT
    CHECK((ps_read32(b, kSpurs + 0xB0) & (0x80000000u >> wid)) == 0);
    CHECK((ps_read32(b, kSpurs + 0xB4) & (0x80000000u >> wid)) == 0);
    CHECK(s.a.call("cellSpursJoinJobChain", kJc) == kInval); // workload 32: past the last one
    CHECK(s.a.call("cellSpursShutdownJobChain", kJc) == kInval);

    // The slot is free: the chain created again gets it and runs.
    REQUIRE(s.a.call("cellSpursCreateJobChainWithAttribute", kSpurs, kJc, kJcAttr) == 0);
    CHECK(s.wid() == wid);
    ps_write32(b, out(0) + 0xC0, 0);
    s.run();
    CHECK(s.wait_done(0));
}

// A join waits for the SPU still running the chain's job when it was shut down: the game frees the chain after it.
TEST_CASE("job chains: a join waits for the job running when its chain was shut down", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0), .delay = 20'000'000, .started = kStarted});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, kEnd);
    s.run();
    REQUIRE(wait_for([&] { return load32_acquire(b, kStarted) == 0x4940; }));
    const u32 wid = s.wid();
    REQUIRE(s.a.call("cellSpursShutdownJobChain", kJc) == 0);
    REQUIRE(!s.done(0)); // the job is still in its delay
    CHECK(s.a.call("cellSpursJoinJobChain", kJc) == 0);
    CHECK(s.done(0)); // it finished before the join returned
    CHECK(ps_read8(b, kSpurs + 0x20 + wid) == 0); // and no SPU is counted on the slot
}

// A chain that stops on an error shuts its workload down from the SPU (SHUTTING_DOWN); the join finishes the shutdown
// once the SPU left, as the system service would, and returns the error.
TEST_CASE("job chains: a chain halted by an error is joined, and the join returns the error", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s(1, 1);
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0)});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, 0x1F); // no such command
    s.cmd(kCmds + 0x10, kEnd);
    s.run();
    REQUIRE(s.wait_done(0));
    REQUIRE(wait_for([&] { return line_byte(b, kJc + 0x23) == 1; })); // isHalted
    const u32 wid = s.wid();
    CHECK(s.a.call("cellSpursJoinJobChain", kJc) == 0xFFFFFFFF80410A18ull); // CELL_SPURS_JOB_ERROR_UNKNOWN_COMMAND
    CHECK(ps_read8(b, kSpurs + 0x80 + wid) == 0);
    CHECK((ps_read8(b, kSpurs + 0xA0 + wid) & 1) == 1); // the shutdown's completion bit
    CHECK(ps_read32(b, kJc + 0x74) == 32);
}

// ------------------------------------------------------------------------------------------------- D-171

namespace {

constexpr u64 kJobNull = 0xFFFFFFFF80410A11ull, kJobAlign = 0xFFFFFFFF80410A10ull, kJobInval = 0xFFFFFFFF80410A02ull,
              kJobPerm = 0xFFFFFFFF80410A09ull, kJobStat = 0xFFFFFFFF80410A0Full, kJobBusy = 0xFFFFFFFF80410A0Aull;
constexpr u32 kOld = kData + 0x5800; // a chain of the old API (0x110 bytes)
constexpr u32 kOldCmds = kCmds + 0x400;

// The old API's chain over the recorder: descriptors of 128 bytes, grab limit 4, contention 1, tags 4/5.
u64 create_old_chain(ChainFixture& s, u32 chain, u32 entry, u32 auto_ready_count = 1)
{
    return s.a.call("cellSpursCreateJobChain", kSpurs, chain, entry, 0x80, 4, kPrio, 1, auto_ready_count, 4, 5);
}

} // namespace

// cellSpursCreateJobChain follows libsre: RPCS3's checks the attribute fields the old API has not, and always fails.
TEST_CASE("job chains (D-171): cellSpursCreateJobChain, the old API, runs with cellSpursKickJobChain",
    "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    write_desc(b, desc(0), {.out = out(0)});
    write_desc(b, desc(1), {.out = out(1)});
    s.cmd(kOldCmds + 0x00, desc(0));
    s.cmd(kOldCmds + 0x08, desc(1));
    s.cmd(kOldCmds + 0x10, kEnd);

    // libsre's checks, in its order.
    CHECK(create_old_chain(s, kOld, 0) == kJobNull);
    CHECK(create_old_chain(s, kOld, kOldCmds + 4) == kJobAlign);
    CHECK(s.a.call("cellSpursCreateJobChain", kSpurs, kOld, kOldCmds, 0x80, 0, kPrio, 1, 1, 4, 5) == kJobInval);
    CHECK(s.a.call("cellSpursCreateJobChain", kSpurs, kOld, kOldCmds, 0x80, 17, kPrio, 1, 1, 4, 5) == kJobInval);
    CHECK(s.a.call("cellSpursCreateJobChain", kSpurs, kOld, kOldCmds, 0x80, 4, kPrio, 1, 1, 32, 5) == kJobInval);
    ps_write8(b, kPrio + 3, 16);
    CHECK(create_old_chain(s, kOld, kOldCmds) == kJobInval);
    ps_write8(b, kPrio + 3, 1);
    CHECK(s.a.call("cellSpursCreateJobChain", kSpurs, kOld, kOldCmds, 0x180, 4, kPrio, 1, 1, 4, 5) == kJobInval);
    CHECK(s.a.call("cellSpursCreateJobChain", 0, kOld, kOldCmds, 0x80, 4, kPrio, 1, 1, 4, 5) == kJobNull);
    CHECK(s.a.call("cellSpursCreateJobChain", kSpurs, kOld + 0x40, kOldCmds, 0x80, 4, kPrio, 1, 1, 4, 5) == kJobAlign);
    // Tag 31 is accepted (the attribute's check refuses it from SDK 1.9).
    std::memset(b + kOld, 0xAB, 0x110);
    REQUIRE(s.a.call("cellSpursCreateJobChain", kSpurs, kOld, kOldCmds, 0x80, 4, kPrio, 1, 1, 31, 5) == 0);
    const u32 wid = ps_read32(b, kOld + 0x74);
    CHECK(wid == s.wid() + 1);
    CHECK(ps_read64(b, kOld + 0x00) == kOldCmds);
    CHECK(ps_read8(b, kOld + 0x2A) == 31);
    CHECK(ps_read8(b, kOld + 0x2B) == 5);
    CHECK(ps_read8(b, kOld + 0x2C) == 0); // no fixed memory, 0x100-byte descriptors at most, no haltOnError
    CHECK(ps_read8(b, kOld + 0x2D) == 1); // revision 1 (SDK 3.6)
    CHECK(ps_read8(b, kOld + 0x24) == 1); // autoReadyCount
    CHECK(ps_read16(b, kOld + 0x70) == 0x80);
    CHECK(ps_read16(b, kOld + 0x72) == 4);
    CHECK(ps_read64(b, kOld + 0x78) == kSpurs);
    CHECK(ps_read32(b, kOld + 0x80) == 0);
    CHECK(ps_read32(b, kOld + 0x10C) == 0); // 0x110 bytes cleared
    REQUIRE(s.a.call("cellSpursShutdownJobChain", kOld) == 0); // created again below with the recorder's tags
    REQUIRE(s.a.call("cellSpursJoinJobChain", kOld) == 0);
    REQUIRE(create_old_chain(s, kOld, kOldCmds) == 0);

    // cellSpursRunJobChain is for revisions 2 and 3, cellSpursKickJobChain for 0 and 1.
    CHECK(s.a.call("cellSpursRunJobChain", kOld) == kJobPerm);
    CHECK(s.a.call("cellSpursKickJobChain", kJc, 1) == kJobPerm);
    CHECK(s.a.call("cellSpursKickJobChain", 0, 1) == kJobNull);
    CHECK(s.a.call("cellSpursKickJobChain", kOld + 0x40, 1) == kJobAlign);
    REQUIRE(s.a.call("cellSpursKickJobChain", kOld, 0) == 0); // autoReadyCount: count 0 becomes 1 (libsre)
    REQUIRE(s.wait_done(0));
    REQUIRE(s.wait_done(1));
    CHECK(s.rec(0, 0x80) == 1);
    CHECK(s.rec(1, 0x80) == 2);
    const u32 old_wid = ps_read32(b, kOld + 0x74);
    REQUIRE(wait_for([&] {
        const LineCopy line0 = line_copy(b, kSpurs);
        return line0.byte(0x20 + old_wid) == 0 && line0.byte(old_wid) == 0;
    }));
    CHECK(ps_read64(b, kOld + 0x00) == kOldCmds + 0x10); // on its END

    // Without autoReadyCount the kick stores the count given (libsre; RPCS3 has the two the other way round).
    constexpr u32 kOld2 = kOld + 0x200;
    s.cmd(kOldCmds + 0x80, kEnd);
    REQUIRE(create_old_chain(s, kOld2, kOldCmds + 0x80, 0) == 0);
    const u32 wid2 = ps_read32(b, kOld2 + 0x74);
    store8_release(b, kSpurs + 0xB00 + 0x20 * wid2 + 0x18, 0); // off SPU 0 (as cellSpursSetPriority): the count stays
    REQUIRE(s.a.call("cellSpursKickJobChain", kOld2, 3) == 0);
    CHECK(ps_read8(b, kSpurs + wid2) == 3);
}

TEST_CASE("job chains (D-171): ids, errors, the SPURS address, information, exception handlers, grab limits, urgent "
          "calls", "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    const u32 wid = s.wid();
    constexpr u32 kOutWord = kData + 0x5700, kInfo = kData + 0x5A00;

    CHECK(s.a.call("cellSpursGetJobChainId", 0, kOutWord) == kJobNull);
    CHECK(s.a.call("cellSpursGetJobChainId", kJc, 0) == kJobNull);
    CHECK(s.a.call("cellSpursGetJobChainId", kJc + 0x40, kOutWord) == kJobAlign);
    REQUIRE(s.a.call("cellSpursGetJobChainId", kJc, kOutWord) == 0);
    CHECK(ps_read32(b, kOutWord) == wid);

    // cellSpursJobChainGetError: the error word is the result; its cause only with an error (libsre checks cause).
    CHECK(s.a.call("cellSpursJobChainGetError", kJc, 0) == kJobNull);
    ps_write32(b, kOutWord, 0x1234);
    REQUIRE(s.a.call("cellSpursJobChainGetError", kJc, kOutWord) == 0);
    CHECK(ps_read32(b, kOutWord) == 0);
    ps_write32(b, kJc + 0x80, 0x80410A18);
    ps_write64(b, kJc + 0x88, kCmds + 0x18);
    CHECK(s.a.call("cellSpursJobChainGetError", kJc, kOutWord) == 0xFFFFFFFF80410A18ull);
    CHECK(ps_read32(b, kOutWord) == kCmds + 0x18);

    // cellSpursGetJobChainInfo (libsre's fields, the grab limit included).
    ps_write64(b, kJc + 0x30, 0x1111);
    ps_write64(b, kJc + 0x38, 0x2222);
    ps_write64(b, kJc + 0x10, 0x3333);
    ps_write8(b, kJc + 0x23, 1);
    std::memset(b + kInfo, 0xAB, 0x58);
    CHECK(s.a.call("cellSpursGetJobChainInfo", kJc, 0) == kJobNull);
    CHECK(s.a.call("cellSpursGetJobChainInfo", kJc + 0x40, kInfo) == kJobAlign);
    REQUIRE(s.a.call("cellSpursGetJobChainInfo", kJc, kInfo) == 0);
    CHECK(ps_read64(b, kInfo + 0x00) == 0x1111);
    CHECK(ps_read64(b, kInfo + 0x08) == 0x2222);
    CHECK(ps_read64(b, kInfo + 0x10) == 0);
    CHECK(ps_read32(b, kInfo + 0x20) == kCmds);        // pc
    CHECK(ps_read32(b, kInfo + 0x24) == 0);            // link registers
    CHECK(ps_read32(b, kInfo + 0x28) == 0x3333);
    CHECK(ps_read32(b, kInfo + 0x30) == kCmds + 0x18); // cause
    CHECK(ps_read32(b, kInfo + 0x34) == 0x80410A18);   // status
    CHECK(ps_read32(b, kInfo + 0x38) == 0x100);        // maximum descriptor size
    CHECK(ps_read32(b, kInfo + 0x3C) == wid);
    CHECK(ps_read8(b, kInfo + 0x40) == 1);  // autoReadyCount
    CHECK(ps_read8(b, kInfo + 0x41) == 1);  // isHalted
    CHECK(ps_read8(b, kInfo + 0x42) == 0);  // isFixedMemAlloc
    CHECK(ps_read8(b, kInfo + 0x43) == 0xAB);
    CHECK(ps_read16(b, kInfo + 0x44) == 4); // grab limit
    CHECK(ps_read32(b, kInfo + 0x48) == 0); // no name
    CHECK(ps_read32(b, kInfo + 0x4C) == 0);
    ps_write64(b, kJc + 0x30, 0);
    ps_write64(b, kJc + 0x38, 0);
    ps_write64(b, kJc + 0x10, 0);
    ps_write8(b, kJc + 0x23, 0);
    ps_write32(b, kJc + 0x80, 0);
    ps_write64(b, kJc + 0x88, 0);

    // cellSpursJobChainGetSpursAddress (libsre checks the workload ID).
    REQUIRE(s.a.call("cellSpursJobChainGetSpursAddress", kJc, kOutWord) == 0);
    CHECK(ps_read32(b, kOutWord) == kSpurs);
    CHECK(s.a.call("cellSpursJobChainGetSpursAddress", kJc, 0) == kJobNull);

    // The exception handler.
    CHECK(s.a.call("cellSpursJobChainSetExceptionEventHandler", kJc, 0, 0) == kJobNull);
    CHECK(s.a.call("cellSpursJobChainSetExceptionEventHandler", kJc + 0x40, 0x5000, 0) == kJobAlign);
    REQUIRE(s.a.call("cellSpursJobChainSetExceptionEventHandler", kJc, 0x5000, 0x77) == 0);
    CHECK(ps_read64(b, kJc + 0x98) == 0x5000);
    CHECK(ps_read64(b, kJc + 0xA0) == 0x77);
    CHECK(s.a.call("cellSpursJobChainSetExceptionEventHandler", kJc, 0x5000, 0x77) == kJobBusy);
    REQUIRE(s.a.call("cellSpursGetJobChainInfo", kJc, kInfo) == 0);
    CHECK(ps_read32(b, kInfo + 0x4C) == 0x5000);
    CHECK(ps_read32(b, kInfo + 0x50) == 0x77);
    REQUIRE(s.a.call("cellSpursJobChainUnsetExceptionEventHandler", kJc) == 0);
    CHECK(ps_read64(b, kJc + 0x98) == 0);
    CHECK(ps_read64(b, kJc + 0xA0) == 0);

    // A workload ID past 31.
    ps_write32(b, kJc + 0x74, 40);
    CHECK(s.a.call("cellSpursJobChainGetSpursAddress", kJc, kOutWord) == kJobInval);
    CHECK(s.a.call("cellSpursJobChainSetExceptionEventHandler", kJc, 0x5000, 0) == kJobInval);
    CHECK(s.a.call("cellSpursJobChainUnsetExceptionEventHandler", kJc) == kJobInval);
    CHECK(s.a.call("cellSpursGetJobChainInfo", kJc, kInfo) == kJobInval);
    CHECK(s.a.call("cellSpursKickJobChain", kJc, 1) == kJobInval);
    CHECK(s.a.call("cellSpursJobSetMaxGrab", kJc, 2) == kJobStat);
    REQUIRE(s.a.call("cellSpursGetJobChainId", kJc, kOutWord) == 0); // not checked
    CHECK(ps_read32(b, kOutWord) == 40);
    ps_write32(b, kJc + 0x74, wid);

    // cellSpursJobSetMaxGrab: the limit at 0x72 (libsre); STAT for a chain whose SPURS or workload is not valid.
    CHECK(s.a.call("cellSpursJobSetMaxGrab", 0, 2) == kJobNull);
    CHECK(s.a.call("cellSpursJobSetMaxGrab", kJc + 0x40, 2) == kJobAlign);
    CHECK(s.a.call("cellSpursJobSetMaxGrab", kJc, 0) == kJobInval);
    CHECK(s.a.call("cellSpursJobSetMaxGrab", kJc, 17) == kJobInval);
    ps_write64(b, kJc + 0x78, 0);
    CHECK(s.a.call("cellSpursJobSetMaxGrab", kJc, 2) == kJobStat);
    ps_write64(b, kJc + 0x78, kSpurs + 0x40);
    CHECK(s.a.call("cellSpursJobSetMaxGrab", kJc, 2) == kJobStat);
    ps_write64(b, kJc + 0x78, kSpurs);
    ps_write32(b, kJc + 0x74, 9); // not registered
    CHECK(s.a.call("cellSpursJobSetMaxGrab", kJc, 2) == kJobStat);
    ps_write32(b, kJc + 0x74, wid);
    REQUIRE(s.a.call("cellSpursJobSetMaxGrab", kJc, 2) == 0);
    CHECK(ps_read16(b, kJc + 0x72) == 2);
    CHECK(ps_read16(b, kJc + 0x70) == 0x80); // the descriptor size is left

    // cellSpursAddUrgentCall: a CALL to the list in the first free urgent slot.
    CHECK(s.a.call("cellSpursAddUrgentCall", kJc, 0) == kJobNull);
    CHECK(s.a.call("cellSpursAddUrgentCall", kJc, kCmds + 0x204) == kJobAlign);
    REQUIRE(s.a.call("cellSpursAddUrgentCall", kJc, kCmds + 0x200) == 0);
    CHECK(ps_read64(b, kJc + 0x30) == kCmds + 0x204); // | CALL
    ps_write64(b, kJc + 0x30, 0);

    // cellSpursJobChainAttributeSetJobTypeMemoryCheck: the attribute's flag, the chain's 2; the jobs run (the check
    // itself is not done natively).
    CHECK(s.a.call("cellSpursJobChainAttributeSetJobTypeMemoryCheck", 0) == kJobNull);
    CHECK(s.a.call("cellSpursJobChainAttributeSetJobTypeMemoryCheck", kJcAttr + 4) == kJobAlign);
    REQUIRE(s.a.call("cellSpursJobChainAttributeSetJobTypeMemoryCheck", kJcAttr) == 0);
    CHECK(ps_read8(b, kJcAttr + 0x3C) == 1);
    constexpr u32 kChecked = kData + 0x5C00;
    s.cmd(kCmds + 0x300, desc(0));
    s.cmd(kCmds + 0x308, kEnd);
    write_desc(b, desc(0), {.out = out(0)});
    ps_write32(b, kJcAttr + 0x08, kCmds + 0x300);
    REQUIRE(s.a.call("cellSpursCreateJobChainWithAttribute", kSpurs, kChecked, kJcAttr) == 0);
    CHECK(ps_read8(b, kChecked + 0x94) == 2);
    REQUIRE(s.a.call("cellSpursRunJobChain", kChecked) == 0);
    REQUIRE(s.wait_done(0));
}

TEST_CASE("job chains (D-171): guards: initialize, notify runs or kicks the chain waiting on it, reset",
    "[hle][spurs][spurs-jobchain]")
{
    ChainFixture s;
    u8* b = s.base;
    constexpr u32 kGuard2 = kGuard + 0x80;

    CHECK(s.a.call("cellSpursJobGuardInitialize", 0, kGuard, 1, 1, 0) == kJobNull);
    CHECK(s.a.call("cellSpursJobGuardInitialize", kJc, 0, 1, 1, 0) == kJobNull);
    CHECK(s.a.call("cellSpursJobGuardInitialize", kJc, kGuard + 8, 1, 1, 0) == kJobAlign);
    const u32 wid = s.wid();
    ps_write32(b, kJc + 0x74, 40);
    CHECK(s.a.call("cellSpursJobGuardInitialize", kJc, kGuard, 1, 1, 0) == kJobInval);
    ps_write32(b, kJc + 0x74, wid);
    std::memset(b + kGuard, 0xAB, 128);
    REQUIRE(s.a.call("cellSpursJobGuardInitialize", kJc, kGuard, 1, 0x102, 0x101) == 0);
    CHECK(ps_read32(b, kGuard + 0x00) == 1);
    CHECK(ps_read32(b, kGuard + 0x04) == 1);
    CHECK(ps_read64(b, kGuard + 0x08) == kJc); // a u64, as libsre
    CHECK(ps_read32(b, kGuard + 0x10) == 2);   // the low bytes
    CHECK(ps_read32(b, kGuard + 0x20) == 1);
    CHECK(ps_read32(b, kGuard + 0x30) == 0);
    CHECK(ps_read32(b, kGuard + 0x7C) == 0);

    // The chain waits on the guard; the notify that brings its count to 0 runs it again (a chain of revision 2 or 3:
    // cellSpursRunJobChain), and the guard, passed with autoReset, counts from notifyCount again.
    write_desc(b, desc(0), {.out = out(0)});
    write_desc(b, desc(1), {.out = out(1)});
    s.cmd(kCmds + 0x00, desc(0));
    s.cmd(kCmds + 0x08, guard(kGuard));
    s.cmd(kCmds + 0x10, desc(1));
    s.cmd(kCmds + 0x18, kEnd);
    s.run();
    REQUIRE(s.wait_done(0));
    REQUIRE(s.wait_left());
    CHECK(ps_read32(b, kGuard + 0x30) == 1); // the chain waits on it
    CHECK_FALSE(s.done(1));
    CHECK(s.a.call("cellSpursJobGuardNotify", 0) == kJobNull);
    CHECK(s.a.call("cellSpursJobGuardNotify", kGuard + 8) == kJobAlign);
    REQUIRE(s.a.call("cellSpursJobGuardNotify", kGuard) == 0);
    REQUIRE(s.wait_done(1));
    REQUIRE(s.wait_left());
    CHECK(ps_read32(b, kGuard + 0x00) == 1); // autoReset
    CHECK(ps_read32(b, kGuard + 0x30) == 0);
    // No chain waits now: the count goes down and nothing is run; a notify at 0 is STAT.
    REQUIRE(s.a.call("cellSpursJobGuardNotify", kGuard) == 0);
    CHECK(ps_read32(b, kGuard + 0x00) == 0);
    CHECK(s.a.call("cellSpursJobGuardNotify", kGuard) == kJobStat);
    CHECK(s.a.call("cellSpursJobGuardReset", 0) == kJobNull);
    CHECK(s.a.call("cellSpursJobGuardReset", kGuard + 8) == kJobAlign);
    REQUIRE(s.a.call("cellSpursJobGuardReset", kGuard) == 0);
    CHECK(ps_read32(b, kGuard + 0x00) == 1);

    // A guard of two notifications on an old-API chain: the last one kicks the chain (cellSpursKickJobChain).
    write_desc(b, desc(2), {.out = out(2)});
    write_desc(b, desc(3), {.out = out(3)});
    s.cmd(kOldCmds + 0x00, desc(2));
    s.cmd(kOldCmds + 0x08, guard(kGuard2));
    s.cmd(kOldCmds + 0x10, desc(3));
    s.cmd(kOldCmds + 0x18, kEnd);
    REQUIRE(create_old_chain(s, kOld, kOldCmds) == 0);
    const u32 old_wid = ps_read32(b, kOld + 0x74);
    REQUIRE(s.a.call("cellSpursJobGuardInitialize", kOld, kGuard2, 2, 1, 0) == 0);
    REQUIRE(s.a.call("cellSpursKickJobChain", kOld, 0) == 0);
    REQUIRE(s.wait_done(2));
    REQUIRE(wait_for([&] { return line_word(b, kGuard2 + 0x30) == 1; }));
    // Blocked on the guard: its ready count 0, and no SPU on it.
    REQUIRE(wait_for([&] {
        const LineCopy line0 = line_copy(b, kSpurs);
        return line0.byte(0x20 + old_wid) == 0 && line0.byte(old_wid) == 0;
    }));
    REQUIRE(s.a.call("cellSpursJobGuardNotify", kGuard2) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(s.done(3)); // one notification left
    REQUIRE(s.a.call("cellSpursJobGuardNotify", kGuard2) == 0);
    REQUIRE(s.wait_done(3));
    CHECK(ps_read32(b, kGuard2 + 0x00) == 0); // no autoReset
}
