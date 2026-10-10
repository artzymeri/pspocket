// cellSpursJq (D-537) against libspurs_jq, Sony's own module of the library, from the user's installed system software
// (firmware_compare.hpp; skipped without it). The same scripts run in two fresh processes, one calling pspocket's
// functions (runtime/hle/src/cellSpursJq.cpp) and one Sony's module, both through guest descriptors (so libspurs_jq's
// frame-relative slips land alike): every answer, every byte of the queues, command lists, descriptor pools,
// semaphores, ports and the SPURS instance (but the policy module's address, the class name's and the exception
// handler's, which are each side's own), and the lv2 calls the script's thread makes with their arguments, the same.
// One script runs with SPURS' workers off (the PPU side alone, deterministic); another runs jobs through libspurs_jq's
// own policy module (taken from /dev_flash on both sides) and compares what they did. No game data: the jobs are SPU
// code built here.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

// The scripts' memory (offsets in kData's 2 MB).
constexpr u32 kSpursAttr = kData + 0x0000, kSpurs = kData + 0x1000; // SPURS1: 4 KB
constexpr u32 kAttr = kData + 0x4000;                               // job queue attributes, 0x80 apart
constexpr u32 kPrio = kData + 0x4400, kPoolDesc = kData + 0x4480;
constexpr u32 kOut = kData + 0x4500;                                // output words
constexpr u32 kJq = kData + 0x5000;                                 // 5 queues, 4 KB apart
constexpr u32 kList = kData + 0xA000;                               // 5 command lists, 4 KB apart
constexpr u32 kPool = kData + 0x10000;                              // 16 KB
constexpr u32 kSem = kData + 0x14000;                               // semaphores, 0x80 apart
constexpr u32 kPort = kData + 0x15000;                              // ports, 0x80 apart
constexpr u32 kPort2 = kData + 0x16000;                             // port2s, 0x80 apart
constexpr u32 kPortBuf = kData + 0x17000;                           // ports' copy buffers
constexpr u32 kDescs = kData + 0x18000;                             // job descriptors, 0x400 apart
constexpr u32 kBins = kData + 0x20000;                              // job binaries, 0x1000 apart
constexpr u32 kInput = kData + 0x30000;
constexpr u32 kBigList = kData + 0x100000;                          // a command list of depth 0x8000
constexpr u32 kAreaEnd = kData + 0x34000;

u32 jq(u32 i) { return kJq + 0x1000 * i; }
u32 list(u32 i) { return kList + 0x1000 * i; }
u32 attr(u32 i) { return kAttr + 0x80 * i; }
u32 sem(u32 i) { return kSem + 0x80 * i; }
u32 port(u32 i) { return kPort + 0x80 * i; }
u32 port2(u32 i) { return kPort2 + 0x80 * i; }
u32 desc(u32 i) { return kDescs + 0x400 * i; }
u32 bin(u32 i) { return kBins + 0x1000 * i; }
u32 out(u32 i) { return kOut + 4 * i; }

// ------------------------------------------------------------------------------------------- the two sides

struct Calls {
    std::mutex mutex;
    std::thread::id thread;
    bool on = false;
    std::vector<std::string> list;
};

bool stack_address(u64 v) { return (v >= kStacks && v < kStacks + 0x100000) || (v >= 0xD0000000 && v < 0xF0000000); }

u32 arg_count(u32 number)
{
    switch (number) {
    case 30: case 83: case 91: case 101: return 1;
    case 351: return 2;
    case 82: case 90: return 3;
    case 85: return 5;
    default: return 4;
    }
}

void observe(void* user, u32 number, const u64* a)
{
    auto* c = static_cast<Calls*>(user);
    if (std::this_thread::get_id() != c->thread || !c->on) return;
    std::string line = std::to_string(number);
    for (u32 i = 0; i < arg_count(number); i++)
        line += stack_address(a[i]) ? std::string(" stack") : std::format(" {:#x}", a[i]);
    std::lock_guard lock(c->mutex);
    c->list.push_back(line);
}

ProcessConfig config(bool workers)
{
    ProcessConfig c;
    c.spurs_workers = workers;
    return c;
}

// One process: Sony's libspurs_jq or pspocket's, the firmware's /dev_flash mounted (pspocket's reads its policy
// module there), a SPURS instance of `spus` SPUs.
struct Side {
    Fixture f;
    GuestThread t;
    bool sony;
    std::optional<FirmwareLibrary> module;
    Calls calls;

    Side(bool is_sony, bool workers, const std::string& flash, u32 spus = 1)
        : f(config(workers)), t(f, 0, "ppu"), sony(is_sony)
    {
        t.call("sys_initialize_tls", 0, 0, 0, 0);
        REQUIRE(f.kernel->vfs_mount("/dev_flash", flash, true));
        if (sony) module.emplace(f, t, flash, "cellSpursJq");
        std::memset(f.base() + kData, 0, kAreaEnd - kData);
        REQUIRE(t.call("_cellSpursAttributeInitialize", kSpursAttr, 2, 0x330000, spus, 200, 2016, 0) == 0);
        REQUIRE(t.call("cellSpursInitializeWithAttribute", kSpurs, kSpursAttr) == 0);
        calls.thread = std::this_thread::get_id();
        f.kernel->set_syscall_observer(&observe, &calls);
    }
    ~Side()
    {
        f.kernel->set_syscall_observer(nullptr, nullptr);
        t.call("cellSpursFinalize", kSpurs);
    }
    u8* b() { return f.base(); }

    // A cellSpursJq function through its guest descriptor: Sony's export, or pspocket's linked stub.
    u32 call(const char* name, std::initializer_list<u64> args)
    {
        const u32 nid = recomp::nid_of(name);
        u32 opd = 0;
        if (sony) {
            const auto it = module->module->functions.find({"cellSpursJq", nid});
            INFO(name);
            REQUIRE(it != module->module->functions.end());
            opd = it->second;
        } else {
            opd = f.hle->link_function("cellSpursJq", nid, name);
            REQUIRE(opd != 0);
        }
        if (std::getenv("PSPOCKET_JQ_PROBE")) std::fprintf(stderr, "PROBE %s %s\n", sony ? "sony" : "ours", name);
        calls.on = true;
        const u32 r = static_cast<u32>(call_guest(t.ctx, opd, args));
        calls.on = false;
        if (std::getenv("PSPOCKET_JQ_PROBE")) std::fprintf(stderr, "PROBE -> %#x\n", r);
        return r;
    }
};

// What a script saw: its answers in order, memory at its checkpoints, the lv2 calls.
struct Log {
    std::vector<std::string> answers;
    std::vector<std::pair<std::string, std::vector<u8>>> snaps;
    std::vector<std::string> calls;
};

// The fields each side fills with its own addresses: workload `wid`'s policy module, class name and exception handler.
bool own_field(u32 at, const std::vector<u32>& wids)
{
    for (const u32 w : wids) {
        const u32 info = kSpurs + 0xB00 + 0x20 * w, name = kSpurs + 0xE00 + 0x10 * w, f = kSpurs + 0x100 + 0x80 * w;
        if ((at >= info && at < info + 8) || (at >= name && at < name + 8) || (at >= f && at < f + 8)) return true;
    }
    return false;
}

// Compares the two logs: answers one by one, then each checkpoint (the first differing byte named), then the lv2
// calls. Returns the number of checks.
usize compare(const Log& mine, const Log& sonys, const std::vector<u32>& wids)
{
    usize checks = 0;
    REQUIRE(mine.answers.size() == sonys.answers.size());
    for (usize i = 0; i < mine.answers.size(); i++, checks++) {
        INFO("step " << i);
        CHECK(mine.answers[i] == sonys.answers[i]);
    }
    REQUIRE(mine.snaps.size() == sonys.snaps.size());
    for (usize k = 0; k < mine.snaps.size(); k++) {
        const auto& [label, x] = mine.snaps[k];
        const auto& y = sonys.snaps[k].second;
        REQUIRE(x.size() == y.size());
        usize first = x.size();
        for (usize i = 0; i < x.size(); i++) {
            checks++;
            if (x[i] != y[i] && !own_field(kData + static_cast<u32>(i), wids)) {
                first = i;
                break;
            }
        }
        INFO(std::format("checkpoint '{}': first difference at {:#x} (ours {:02x}, Sony's {:02x})", label, kData + first,
            first < x.size() ? x[first] : 0, first < y.size() ? y[first] : 0));
        CHECK(first == x.size());
    }
    const usize n = std::min(mine.calls.size(), sonys.calls.size());
    for (usize i = 0; i < n; i++, checks++) {
        INFO("lv2 call " << i);
        CHECK(mine.calls[i] == sonys.calls[i]);
    }
    CHECK(mine.calls.size() == sonys.calls.size());
    return checks;
}

// ----------------------------------------------------------------------------------------- the PPU-side script

// Made-up job descriptors for the size checks of GetSuspendedJobSize: a descriptor and a binary from a seed.
struct Rng {
    u64 s;
    u32 next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(s >> 33);
    }
    u32 pick(std::initializer_list<u32> v) { return v.begin()[next() % v.size()]; }
};

void made_up_job(u8* b, u32 d, u32 bi, u32 seed)
{
    Rng r{seed * 0x9E3779B97F4A7C15ull + 7};
    std::memset(b + d, 0, 0x400);
    std::memset(b + bi, 0, 0x400);
    const u32 ea_flags = r.pick({0, 0, 0, 1, 2, 8});
    ps_write32(b, d + 0x00, r.pick({0, 0, 0, 1, 2}));        // the binary's high word (with jobType 4: extra size)
    ps_write32(b, d + 0x04, r.pick({bi, bi, bi, 0, 1}) | ea_flags);
    ps_write16(b, d + 0x08, static_cast<u16>(r.pick({0, 1, 0x20, 0x100, 0x1000})));
    const u32 n_dma = r.pick({0, 1, 2, 3, 5});
    const u32 n_cache = r.pick({0, 0, 1, 2, 5});
    ps_write16(b, d + 0x0A, static_cast<u16>(8 * n_dma + r.pick({0, 0, 0, 0, 4})));
    ps_write32(b, d + 0x14, r.pick({0, 0x40, 0x100, 0x4000, 0x8, 0x10000}));
    ps_write32(b, d + 0x18, r.pick({0, 0x20, 0x400, 0x4, 0x20000}));
    ps_write16(b, d + 0x1C, static_cast<u16>(r.pick({0, 0x80, 0x400, 0x2000})));
    ps_write16(b, d + 0x1E, static_cast<u16>(r.pick({0, 0x10, 0x100, 0x1000})));
    ps_write32(b, d + 0x24, 8 * n_cache + r.pick({0, 0, 0, 0, 4, 40}));
    ps_write8(b, d + 0x2C, static_cast<u8>(r.pick({0, 2, 4, 6, 0xFF})));
    for (u32 i = 0; i < n_dma; i++) {
        const u32 size = r.pick({0, 1, 2, 3, 4, 8, 12, 16, 0x20, 0x4000, 0x4001, 0x8010});
        const u32 ea = r.pick({kInput, kInput + 1, kInput + 4, kInput + 8, 0}) | (r.pick({0, 0, 0, 1}) << 31);
        ps_write32(b, d + 0x30 + 8 * i, size | (r.pick({0, 0, 0, 1}) << 31));
        ps_write32(b, d + 0x34 + 8 * i, ea & 0x7FFFFFFF);
    }
    for (u32 i = 0; i < n_cache; i++) {
        ps_write32(b, d + 0x30 + 8 * n_dma + 8 * i, r.pick({0, 8, 0x10, 0x18, 0x40, 0x1000}));
        ps_write32(b, d + 0x34 + 8 * n_dma + 8 * i, r.pick({kInput, kInput + 8, 0}));
    }
    // The binary: a job runtime header, its version, its tag, the sizes after it.
    if (r.next() % 8) std::memcpy(b + bi + 0x30, "JOBCRT Ver", 10);
    const u32 version = r.pick({0x342, 0x342, 0x330, 0x335, 0x343, 0x320, 0x350, 0x341});
    ps_write8(b, bi + 0x3A, static_cast<u8>(version >> 4));
    ps_write8(b, bi + 0x3B, static_cast<u8>(version & 0xF));
    ps_write32(b, bi + 0x20, r.pick({0x42494E32, 0x62696E32, 0, 0x12345678}));
    ps_write16(b, bi + 0x2E, static_cast<u16>(r.pick({0, 0x10, 0x200})));
    const u32 info = r.pick({0x40, 0x80, 0x100});
    ps_write32(b, bi + 0x3C, info);
    ps_write32(b, bi + 0x40, r.pick({0, 0x20, 0x40}));
    if (r.next() % 2) std::memcpy(b + bi + info + 0x10, "%SPURS JOB INFO%", 16);
    for (u32 at : {bi + 0x50, bi + info + 0x20}) {
        const u32 n = r.pick({0, 1, 2, 3});
        for (u32 i = 0; i < n; i++) {
            ps_write32(b, at + 8 * i, i);
            ps_write32(b, at + 8 * i + 4, r.pick({0x10, 0x80, 0x400}));
        }
        ps_write32(b, at + 8 * n, 0xFFFFFFFF);
    }
}

Log ppu_script(Side& s)
{
    Log log;
    u8* b = s.b();
    const auto call = [&](const char* name, std::initializer_list<u64> args) {
        const u32 r = s.call(name, args);
        std::string line = std::format("{}(", name);
        for (const u64 a : args) line += std::format("{:#x},", a);
        log.answers.push_back(line + std::format(") -> {:#x}", r));
        return r;
    };
    const auto snap = [&](std::string label) {
        log.snaps.emplace_back(std::move(label), std::vector<u8>(b + kData, b + kAreaEnd));
    };
    const auto write = [&](u32 at, u32 v) { ps_write32(b, at, v); };
    std::memset(b + kPrio, 1, 8);

    // Attributes: every setter's range edges.
    call("cellSpursJobQueueAttributeInitialize", {0});
    for (u32 i = 0; i < 6; i++) call("cellSpursJobQueueAttributeInitialize", {attr(i)});
    for (const u32 v : {0u, 1u, 16u, 17u, 0xFFFFFFFFu}) call("cellSpursJobQueueAttributeSetMaxGrab", {attr(0), v});
    call("cellSpursJobQueueAttributeSetMaxGrab", {0, 1});
    for (const u32 v : {0u, 1u, 0x100u, 0x2FFu}) {
        call("cellSpursJobQueueAttributeSetSubmitWithEntryLock", {attr(5), v});
        call("cellSpursJobQueueAttributeSetDoBusyWaiting", {attr(5), v});
        call("cellSpursJobQueueAttributeSetIsHaltOnError", {attr(5), v});
        call("cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck", {attr(5), v});
    }
    call("cellSpursJobQueueAttributeSetSubmitWithEntryLock", {0, 1});
    call("cellSpursJobQueueAttributeSetDoBusyWaiting", {0, 1});
    call("cellSpursJobQueueAttributeSetIsHaltOnError", {0, 1});
    call("cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck", {0, 1});
    for (const u32 v : {0u, 0x80u, 0x100u, 0x180u, 0x1C0u, 0x380u, 0x3FFu, 0x400u})
        call("cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", {attr(1), v});
    call("cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", {0, 0x100});
    for (const auto& [n, g] : std::initializer_list<std::pair<u32, u32>>{{0, 1}, {255, 16}, {256, 1}, {1, 0}, {1, 17}, {3, 2}})
        call("cellSpursJobQueueAttributeSetGrabParameters", {attr(2), n, g});
    call("cellSpursJobQueueAttributeSetGrabParameters", {0, 1, 1});
    snap("attributes");

    // The attributes the queues are made with: 0 default, 1 entry lock + busy waiting + halt + memcheck, 2 pool, 3 an old
    // SDK, 4 errors.
    call("cellSpursJobQueueAttributeInitialize", {attr(0)});
    call("cellSpursJobQueueAttributeInitialize", {attr(1)});
    call("cellSpursJobQueueAttributeSetSubmitWithEntryLock", {attr(1), 1});
    call("cellSpursJobQueueAttributeSetIsHaltOnError", {attr(1), 1});
    call("cellSpursJobQueueAttributeSetIsJobTypeMemoryCheck", {attr(1), 1});
    call("cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", {attr(1), 0x380});
    call("cellSpursJobQueueAttributeSetGrabParameters", {attr(1), 3, 7});
    call("cellSpursJobQueueAttributeInitialize", {attr(2)});
    call("cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", {attr(2), 0x380});
    call("cellSpursJobQueueAttributeInitialize", {attr(3)});
    call("cellSpursJobQueueAttributeSetDoBusyWaiting", {attr(3), 1});
    call("cellSpursJobQueueAttributeSetGrabParameters", {attr(3), 9, 3});

    // Creation's refusals.
    const auto create = [&](std::initializer_list<u64> a) { return call("_cellSpursCreateJobQueue", a); };
    const auto create_pool = [&](std::initializer_list<u64> a) {
        return call("_cellSpursCreateJobQueueWithJobDescriptorPool", a);
    };
    create({1, 0x330000, 0, jq(4), attr(0), 0, list(4), 16, 1, kPrio});
    create({1, 0x330000, kSpurs, 0, attr(0), 0, list(4), 16, 1, kPrio});
    create({1, 0x330000, kSpurs, jq(4), 0, 0, list(4), 16, 1, kPrio});
    create({1, 0x330000, kSpurs, jq(4), attr(0), 0, 0, 16, 1, kPrio});
    create({1, 0x330000, kSpurs + 8, jq(4), attr(0), 0, list(4), 16, 1, kPrio});
    create({1, 0x330000, kSpurs, jq(4) + 0x40, attr(0), 0, list(4), 16, 1, kPrio});
    create({1, 0x330000, kSpurs, jq(4), attr(0), 0, list(4) + 0x10, 16, 1, kPrio});
    create({1, 0x330000, kSpurs, jq(4), attr(0), 0, list(4), 0x8001, 1, kPrio});
    create({1, 0x330000, kSpurs, jq(4), attr(0), 0, list(4), 16, 9, kPrio});
    create_pool({1, 0x330000, kSpurs, jq(4), attr(0), kPool, 0, 0, list(4), 16, 1, kPrio});
    create_pool({1, 0x330000, kSpurs, jq(4), attr(0), 0, kPoolDesc, 0, list(4), 16, 1, kPrio});
    create_pool({1, 0x330000, kSpurs, jq(4), attr(0), kPool + 0x40, kPoolDesc, 0, list(4), 16, 1, kPrio});
    write(kPoolDesc, 1);
    create_pool({1, 0x330000, kSpurs, jq(4), attr(0), kPool, kPoolDesc, 0, list(4), 16, 1, kPrio});
    for (const auto& [at, v] : std::initializer_list<std::pair<u32, u32>>{{4, 0}, {4, 17}, {8, 0x80}, {8, 0x400}, {8, 0x101}}) {
        call("cellSpursJobQueueAttributeInitialize", {attr(4)});
        write(attr(4) + at, v);
        create({1, 0x330000, kSpurs, jq(4), attr(4), 0, list(4), 16, 1, kPrio});
    }
    snap("refusals");

    // Four queues: default (depth 16), entry lock (depth 5), a pool (depth 8), an old SDK (depth 1).
    CHECK(create({1, 0x330000, kSpurs, jq(0), attr(0), kData + 0x4F00, list(0), 16, 1, kPrio}) == 0);
    CHECK(create({2, 0x330000, kSpurs, jq(1), attr(1), 0, list(1), 5, 2, kPrio}) == 0);
    const u32 counts[8] = {2, 2, 1, 0, 1, 0, 0, 1};
    for (u32 i = 0; i < 8; i++) write(kPoolDesc + 4 * i, counts[i]);
    CHECK(create_pool({3, 0x340000, kSpurs, jq(2), attr(2), kPool, kPoolDesc, 0, list(2), 8, 8, kPrio}) == 0);
    CHECK(create({1, 0x300000, kSpurs, jq(3), attr(3), 0, list(3), 1, 1, kPrio}) == 0);
    std::memcpy(b + kData + 0x4F00, "my queue", 9);
    snap("created");

    // Getters.
    for (u32 q = 0; q < 5; q++) {
        call("cellSpursJobQueueGetSpurs", {jq(q)});
        call("cellSpursJobQueueGetHandleCount", {jq(q)});
        call("cellSpursJobQueueGetMaxSizeJobDescriptor", {jq(q)});
        call("cellSpursGetJobQueueId", {jq(q), out(q)});
        call("cellSpursJobQueueGetError", {jq(q), out(8), out(9)});
    }
    for (const u32 q : {0u, jq(0) + 0x10}) {
        call("cellSpursJobQueueGetSpurs", {q});
        call("cellSpursJobQueueGetHandleCount", {q});
        call("cellSpursJobQueueGetMaxSizeJobDescriptor", {q});
        call("cellSpursGetJobQueueId", {q, out(0)});
        call("cellSpursJobQueueGetError", {q, out(8), out(9)});
    }
    call("cellSpursGetJobQueueId", {jq(0), 0});
    call("cellSpursJobQueueGetError", {jq(0), 0, out(9)});
    call("cellSpursJobQueueGetError", {jq(0), out(8), 0});
    write(jq(0) + 0xF04, 0x80410A1F);
    ps_write64(b, jq(0) + 0xF08, 0x123456789ABCDEF0ull);
    call("cellSpursJobQueueGetError", {jq(0), out(8), out(9)});
    write(jq(0) + 0xF04, 0);
    snap("getters");

    // Handles: open until the bitmap has some, close some, reopen; refusals.
    for (u32 i = 0; i < 6; i++) call("cellSpursJobQueueOpen", {jq(0), out(16 + i)});
    call("cellSpursJobQueueClose", {jq(0), 2});
    call("cellSpursJobQueueClose", {jq(0), 2});
    call("cellSpursJobQueueClose", {jq(0), 1024});
    call("cellSpursJobQueueClose", {jq(0), 0xFFFFFFFF});
    call("cellSpursJobQueueOpen", {jq(0), out(22)});
    call("cellSpursJobQueueOpen", {0, out(22)});
    call("cellSpursJobQueueOpen", {jq(0), 0});
    call("cellSpursJobQueueOpen", {jq(0) + 4, out(22)});
    call("cellSpursJobQueueClose", {0, 1});
    call("cellSpursJobQueueClose", {jq(0) + 4, 1});
    for (u32 q = 1; q < 4; q++) call("cellSpursJobQueueOpen", {jq(q), out(24 + q)});
    call("cellSpursJobQueueOpen", {jq(4), out(28)}); // never created: not running
    call("cellSpursJobQueueGetHandleCount", {jq(0)});
    snap("handles");

    // Pushes: every function's refusals, then the rings filled.
    const u32 h0 = ps_read32(b, out(16)), h1 = ps_read32(b, out(25)), h2 = ps_read32(b, out(26)), h3 = ps_read32(b, out(27));
    for (u32 i = 0; i < 8; i++) {
        std::memset(b + desc(i), 0, 0x400);
        ps_write32(b, desc(i) + 4, bin(0));
    }
    ps_write32(b, desc(8), 3);      // a job list of 3 jobs of 256 bytes
    ps_write32(b, desc(8) + 4, 0x100);
    ps_write64(b, desc(8) + 8, desc(0));
    call("cellSpursJobQueueSemaphoreInitialize", {sem(0), jq(0)});
    const u32 s0 = sem(0);
    for (const u32 size : {0x40u, 0x80u, 0x100u, 0x3C0u, 0x380u, 0x400u, 0x20u, 0x200u}) {
        call("_cellSpursJobQueuePushJobBody", {jq(0), h0, desc(0), size, 0, s0, 0});
        call("_cellSpursJobQueuePushBody", {jq(0), h0, desc(0), size, s0, 0});
        call("_cellSpursJobQueuePushJobBody2", {jq(0), h0, desc(0), size, 1, s0, 1, 0});
        call("_cellSpursJobQueuePushJob2Body", {jq(0), h0, desc(0), size, 2, 3, s0});
        call("_cellSpursJobQueuePushAndReleaseJobBody", {jq(0), h0, desc(0), size, 2, 4, s0});
    }
    for (const auto& a : std::vector<std::vector<u64>>{
             {0, h0, desc(0), 0x100, 0, s0, 0}, {jq(0), h0, 0, 0x100, 0, s0, 0}, {jq(0) + 8, h0, desc(0), 0x100, 0, s0, 0},
             {jq(0), h0, desc(0) + 8, 0x100, 0, s0, 0}, {jq(0), h0, desc(0), 0x100, 0, s0 + 0x10, 0},
             {jq(0), h0, desc(0), 0x100, 16, s0, 0}, {jq(0), 1024, desc(0), 0x100, 0, s0, 0},
             {jq(0), 2, desc(0), 0x100, 0, s0, 0}, {jq(0), 0xFFFFFFFF, desc(0), 0x100, 0, s0, 0}}) {
        call("_cellSpursJobQueuePushJobBody", {a[0], a[1], a[2], a[3], a[4], a[5], a[6]});
    }
    call("_cellSpursJobQueuePushJob2Body", {jq(0), h0, desc(0), 0x100, 0, 8, s0});
    call("_cellSpursJobQueuePushAndReleaseJobBody", {jq(0), h0, desc(0), 0x100, 0, 9, s0});
    call("_cellSpursJobQueuePushJobListBody", {jq(0), h0, 0, 0, s0, 0});
    call("_cellSpursJobQueuePushJobListBody", {jq(0), h0, desc(8) + 4, 0, s0, 0});
    call("_cellSpursJobQueuePushJobListBody", {jq(0), h0, desc(8), 16, s0, 0});
    call("_cellSpursJobQueuePushSync", {jq(0), h0, 0, 0});
    call("_cellSpursJobQueuePushSync", {jq(0), h0, 0x10000, 0});
    call("_cellSpursJobQueuePushSync", {0, h0, 1, 0});
    call("_cellSpursJobQueuePushFlush", {jq(0) + 0x20, h0, 0});
    snap("push refusals");
    // The default queue (depth 16) filled with every kind of command, then AGAIN.
    u32 n = 0;
    for (;;) {
        u32 r = 0;
        switch (n % 7) {
        case 0: r = call("_cellSpursJobQueuePushJobBody", {jq(0), h0, desc(n % 8), 0x100, n % 16, s0, 0}); break;
        case 1: r = call("_cellSpursJobQueuePushBody", {jq(0), h0, desc(n % 8), 0x40, 0, 0}); break;
        case 2: r = call("_cellSpursJobQueuePushJobBody2", {jq(0), h0, desc(n % 8), 0x80, 3, s0, 1, 0}); break;
        case 3: r = call("_cellSpursJobQueuePushJob2Body", {jq(0), h0, desc(n % 8), 0x100, 5, 6, 0}); break;
        case 4: r = call("_cellSpursJobQueuePushJobListBody", {jq(0), h0, desc(8), 7, s0, 0}); break;
        case 5: r = call("_cellSpursJobQueuePushSync", {jq(0), h0, 0x8421, 0}); break;
        default: r = call("_cellSpursJobQueuePushFlush", {jq(0), h0, 0}); break;
        }
        if (r || ++n > 40) break;
    }
    snap("default ring full");
    // The entry lock queue (depth 5).
    for (u32 i = 0; i < 7; i++) call("_cellSpursJobQueuePushJobBody", {jq(1), h1, desc(i), 0x380, i, 0, 0});
    snap("entry lock ring full");
    // The old SDK queue (depth 1).
    for (u32 i = 0; i < 3; i++) call("_cellSpursJobQueuePushFlush", {jq(3), h3, 0});
    snap("depth 1 ring full");

    // The pool: allocations until AGAIN (classes 0, 2 and 7; one of class 1's two; none of class 4's: the port2 copies
    // below take those, waiting for one when there is none), sizes without a class, pushes of pool descriptors and others.
    for (const u32 size : {0x40u, 0x100u, 0x180u, 0x280u, 0x300u, 0x380u, 0x30u, 0x400u, 0x3FFu}) {
        for (u32 k = 0; k < 4; k++)
            call("_cellSpursJobQueueAllocateJobDescriptorBody", {jq(2), h2, size, 4, out(32 + k)});
    }
    call("_cellSpursJobQueueAllocateJobDescriptorBody", {jq(2), h2, 0x80, 4, out(36)});
    call("_cellSpursJobQueueAllocateJobDescriptorBody", {jq(2), h2, 0x40, 2, out(40)});
    call("_cellSpursJobQueueAllocateJobDescriptorBody", {jq(2), h2, 0x40, 1, out(40)});
    call("_cellSpursJobQueueAllocateJobDescriptorBody", {jq(2), h2, 0x40, 4, 0});
    call("_cellSpursJobQueueAllocateJobDescriptorBody", {jq(2), 5, 0x40, 4, out(40)});
    call("_cellSpursJobQueueAllocateJobDescriptorBody", {jq(0), h0, 0x40, 4, out(40)});
    call("_cellSpursJobQueuePushAndReleaseJobBody", {jq(2), h2, kPool, 0x380, 1, 4, 0});
    call("_cellSpursJobQueuePushAndReleaseJobBody", {jq(2), h2, kPool + 0x380, 0x100, 1, 2, 0});
    call("_cellSpursJobQueuePushJob2Body", {jq(2), h2, kPool, 0x100, 1, 4, 0});
    call("_cellSpursJobQueuePushJob2Body", {jq(2), h2, desc(1), 0x100, 1, 4, 0});
    call("_cellSpursJobQueuePushAndReleaseJobBody", {jq(2), h2, desc(1), 0x100, 1, 4, 0});
    snap("pool");

    // Semaphores.
    call("cellSpursJobQueueSemaphoreInitialize", {0, jq(0)});
    call("cellSpursJobQueueSemaphoreInitialize", {sem(1), 0});
    call("cellSpursJobQueueSemaphoreInitialize", {sem(1) + 8, jq(0)});
    call("cellSpursJobQueueSemaphoreInitialize", {sem(1), jq(0) + 8});
    call("cellSpursJobQueueSemaphoreInitialize", {sem(1), jq(0)});
    for (const u32 c : {0u, 1u, 0x0FFFFFFFu, 0x10000000u}) call("cellSpursJobQueueSemaphoreTryAcquire", {sem(1), c});
    ps_write64(b, sem(1), 5ull << 32);
    for (const u32 c : {2u, 2u, 2u, 1u, 0u}) call("cellSpursJobQueueSemaphoreTryAcquire", {sem(1), c});
    ps_write64(b, sem(1), 0x8000000300000007ull);
    call("cellSpursJobQueueSemaphoreTryAcquire", {sem(1), 1});
    ps_write64(b, sem(1), 0x0FFFFFFF00000000ull | 0x1234);
    call("cellSpursJobQueueSemaphoreTryAcquire", {sem(1), 0x0FFFFFFF});
    ps_write64(b, sem(1), 0x6000000500000099ull);
    call("cellSpursJobQueueSemaphoreTryAcquire", {sem(1), 3});
    call("cellSpursJobQueueSemaphoreTryAcquire", {0, 1});
    call("cellSpursJobQueueSemaphoreTryAcquire", {sem(1) + 4, 1});
    call("cellSpursJobQueueSemaphoreAcquire", {sem(1), 1});
    call("cellSpursJobQueueSemaphoreAcquire", {sem(1), 0x10000000});
    snap("semaphores");

    // Signals: a job not waiting keeps the signal; one waiting goes on its queue's list.
    std::memset(b + desc(9), 0, 0x80);
    call("cellSpursJobQueueSendSignal", {desc(9)});
    call("cellSpursJobQueueSendSignal", {desc(9)});
    std::memset(b + desc(10), 0, 0x80);
    ps_write32(b, desc(10) + 0x28, jq(0));
    write(desc(10) + 0x2C, 0x00403456);
    call("cellSpursJobQueueSendSignal", {desc(10)});
    std::memset(b + desc(11), 0, 0x80);
    ps_write32(b, desc(11) + 0x28, jq(0));
    write(desc(11) + 0x2C, 0x00C07800);
    call("cellSpursJobQueueSendSignal", {desc(11)});
    call("cellSpursJobQueueSendSignal", {0});
    call("cellSpursJobQueueSendSignal", {desc(11) + 0x10});
    snap("signals");

    // Ports.
    call("cellSpursJobQueuePortInitialize", {0, jq(0), 0});
    call("cellSpursJobQueuePortInitialize", {port(0), 0, 0});
    call("cellSpursJobQueuePortInitialize", {port(0) + 8, jq(0), 0});
    call("cellSpursJobQueuePortInitialize", {port(0), jq(0) + 8, 0});
    call("cellSpursJobQueuePortInitialize", {port(0), jq(0), 0});
    call("cellSpursJobQueuePortInitialize", {port(1), jq(0), 1});
    call("cellSpursJobQueuePortInitialize", {port(2), jq(4), 0}); // a queue not running: Open fails
    call("cellSpursJobQueuePortInitializeWithDescriptorBuffer", {port(3), jq(1), 0, 0x380, 4, 0});
    call("cellSpursJobQueuePortInitializeWithDescriptorBuffer", {port(3), jq(1), kPortBuf + 8, 0x380, 4, 0});
    call("cellSpursJobQueuePortInitializeWithDescriptorBuffer", {port(3), jq(1), kPortBuf, 0x3C0, 4, 0});
    call("cellSpursJobQueuePortInitializeWithDescriptorBuffer", {port(3), jq(1), kPortBuf, 0x380, 3, 1});
    call("cellSpursJobQueuePortInitializeWithDescriptorBuffer", {port(4), jq(3), kPortBuf + 0xC00, 0x40, 2, 0});
    for (u32 p = 0; p < 5; p++) call("cellSpursJobQueuePortGetJobQueue", {port(p)});
    snap("ports");
    // Pushes through the ports (the entry lock queue has room after its policy module... none here: AGAIN).
    call("_cellSpursJobQueuePortPushJobBody", {port(0), desc(0), 0x100, 1, 1, 0});
    call("_cellSpursJobQueuePortPushJobBody", {port(1), desc(0), 0x100, 1, 1, 0});
    call("_cellSpursJobQueuePortPushJobBody2", {port(3), desc(0), 0x380, 2, 1, 1, 0});
    call("_cellSpursJobQueuePortPushBody", {port(3), desc(0), 0x40, 1, 0});
    call("_cellSpursJobQueuePortPushJobListBody", {port(3), desc(8), 3, 1, 0});
    call("_cellSpursJobQueuePortCopyPushJobBody", {port(3), desc(1), 0x380, 4, 1, 0});
    call("_cellSpursJobQueuePortCopyPushJobBody2", {port(3), desc(2), 0x100, 5, 0, 1, 0});
    call("_cellSpursJobQueuePortCopyPushBody", {port(3), desc(3), 0x80, 1, 0});
    call("_cellSpursJobQueuePortCopyPushBody", {port(0), desc(3), 0x80, 1, 0}); // no buffer: PERM
    call("_cellSpursJobQueuePortCopyPushBody", {port(4), desc(3), 0x80, 1, 0}); // larger than the slots
    call("_cellSpursJobQueuePortCopyPushBody", {port(4), desc(3), 0x40, 1, 0});
    call("_cellSpursJobQueuePortPushSync", {port(3), 0x3, 0});
    call("_cellSpursJobQueuePortPushFlush", {port(3), 0});
    call("_cellSpursJobQueuePortPushJobBody", {0, desc(0), 0x100, 1, 1, 0});
    call("_cellSpursJobQueuePortPushJobBody", {port(2), desc(0), 0x100, 1, 1, 0});
    snap("port pushes");
    // Syncs: the semaphore lacks what the port counted (AGAIN, the count kept), then has it.
    call("cellSpursJobQueuePortTrySync", {port(3)});
    call("cellSpursJobQueuePortTrySync", {port(0)});
    ps_write64(b, port(3), 9ull << 32);
    call("cellSpursJobQueuePortTrySync", {port(3)});
    call("cellSpursJobQueuePortTrySync", {port(1)});
    ps_write64(b, port(1) + 0x10, ps_read64(b, port(1) + 0x10) | 0x100);
    call("cellSpursJobQueuePortTrySync", {port(1)});
    call("_cellSpursJobQueuePortPushJobBody", {port(1), desc(0), 0x100, 1, 1, 0});
    call("cellSpursJobQueuePortTrySync", {0});
    call("cellSpursJobQueuePortSync", {port(3)});
    snap("port syncs");
    call("cellSpursJobQueuePortFinalize", {port(0)});
    call("cellSpursJobQueuePortFinalize", {port(0)});
    call("cellSpursJobQueuePortFinalize", {0});
    snap("port finalized");

    // Port2s.
    call("cellSpursJobQueuePort2Create", {port2(0), jq(2)});
    call("cellSpursJobQueuePort2Create", {port2(1), jq(0)});
    call("cellSpursJobQueuePort2Create", {port2(2), jq(4)});
    call("cellSpursJobQueuePort2Create", {0, jq(2)});
    call("cellSpursJobQueuePort2Create", {port2(3) + 4, jq(2)});
    for (u32 p = 0; p < 3; p++) call("cellSpursJobQueuePort2GetJobQueue", {port2(p)});
    call("cellSpursJobQueuePort2PushSync", {port2(1), 1, 4});
    call("cellSpursJobQueuePort2PushSync", {port2(1), 1, 1});
    call("cellSpursJobQueuePort2PushFlush", {port2(1), 4});
    call("cellSpursJobQueuePort2PushFlush", {port2(1), 3});
    call("_cellSpursJobQueuePort2PushJobListBody", {port2(0), desc(8), 2, 5});
    call("_cellSpursJobQueuePort2PushJobListBody", {port2(0), desc(8), 2, 2});
    call("_cellSpursJobQueuePort2PushJobBody", {port2(0), desc(4), 0x100, 1, 5});
    call("_cellSpursJobQueuePort2PushJobBody", {port2(0), kPool, 0x100, 1, 5});
    call("_cellSpursJobQueuePort2PushJobBody", {port2(0), desc(4), 0x100, 1, 8});
    call("_cellSpursJobQueuePort2PushAndReleaseJobBody", {port2(0), kPool + 0x380, 0x380, 1, 7});
    call("_cellSpursJobQueuePort2PushAndReleaseJobBody", {port2(0), desc(4), 0x100, 1, 5});
    call("_cellSpursJobQueuePort2CopyPushJobBody", {port2(0), desc(5), 0x80, 0x80, 2, 1});
    call("_cellSpursJobQueuePort2CopyPushJobBody", {port2(0), desc(6), 0x100, 0x200, 3, 2});
    call("_cellSpursJobQueuePort2CopyPushJobBody", {port2(0), desc(5), 0x100, 0x80, 2, 1});
    call("_cellSpursJobQueuePort2CopyPushJobBody", {port2(0), desc(5), 0x100, 0x380, 2, 4});
    call("cellSpursJobQueuePort2AllocateJobDescriptor", {port2(0), 0x380, 4, out(41)});
    call("cellSpursJobQueuePort2AllocateJobDescriptor", {port2(0), 0x380, 5, out(41)});
    call("cellSpursJobQueuePort2AllocateJobDescriptor", {port2(1), 0x100, 4, out(42)});
    snap("port2 pushes");
    call("cellSpursJobQueuePort2Sync", {port2(0), 4});
    call("cellSpursJobQueuePort2Sync", {port2(0), 5});
    ps_write64(b, port2(0), 3ull << 32);
    call("cellSpursJobQueuePort2Sync", {port2(0), 4});
    call("cellSpursJobQueuePort2Sync", {port2(1), 0});
    call("cellSpursJobQueuePort2Destroy", {port2(0)});
    call("cellSpursJobQueuePort2Destroy", {port2(0)});
    call("cellSpursJobQueuePort2Destroy", {port2(1)});
    call("cellSpursJobQueuePort2Destroy", {0});
    snap("port2 syncs");

    // Exception handlers.
    call("cellSpursJobQueueSetExceptionEventHandler", {jq(0), kData + 0x4F80, 0x1234});
    call("cellSpursJobQueueSetExceptionEventHandler", {jq(0), kData + 0x4F80, 0x1234});
    call("cellSpursJobQueueSetExceptionEventHandler2", {jq(1), kData + 0x4F88, 0xFFFFFFFF});
    call("cellSpursJobQueueSetExceptionEventHandler", {jq(0), 0, 1});
    call("cellSpursJobQueueSetExceptionEventHandler", {0, 4, 1});
    call("cellSpursJobQueueSetExceptionEventHandler2", {jq(0) + 4, 4, 1});
    call("cellSpursJobQueueSetExceptionEventHandler", {jq(4), 4, 1});
    call("cellSpursJobQueueUnsetExceptionEventHandler", {jq(0)});
    call("cellSpursJobQueueUnsetExceptionEventHandler", {jq(4)});
    call("cellSpursJobQueueUnsetExceptionEventHandler", {0});
    snap("handlers");

    // Waiting modes.
    for (const u32 m : {1u, 1u, 0u, 2u, 0u}) call("cellSpursJobQueueSetWaitingMode", {jq(0), m});
    ps_write8(b, jq(0) + 0x60, 1);
    call("cellSpursJobQueueSetWaitingMode", {jq(0), 1});
    ps_write8(b, jq(0) + 0x60, 0);
    call("cellSpursJobQueueSetWaitingMode", {0, 1});
    call("cellSpursJobQueueSetWaitingMode", {jq(0) + 4, 1});
    snap("waiting modes");

    // Suspended job sizes over made-up descriptors.
    for (u32 seed = 0; seed < 400; seed++) {
        made_up_job(b, desc(12), bin(1), seed);
        const u32 size = Rng{seed}.pick({0x100, 0x180, 0x380, 0x80, 0x400, 0x101});
        const u32 mode = Rng{seed + 99}.pick({0, 1, 0xFFFFFFFF, 2});
        write(out(50), 0xDEADBEEF);
        call("cellSpursJobQueueGetSuspendedJobSize", {desc(12), size, mode, out(50)});
        log.answers.push_back(std::format("  size {:#x}", ps_read32(b, out(50))));
    }
    // The firmware-free test's job (tests/hle/test_spursjq.cpp), its answers checked here.
    std::memset(b + desc(13), 0, 0x100);
    std::memset(b + bin(2), 0, 0x200);
    ps_write32(b, desc(13) + 0x04, bin(2));
    ps_write16(b, desc(13) + 0x08, 0x20);
    ps_write16(b, desc(13) + 0x0A, 8);
    ps_write32(b, desc(13) + 0x14, 0x40);
    ps_write32(b, desc(13) + 0x18, 0x20);
    ps_write16(b, desc(13) + 0x1E, 1);
    ps_write32(b, desc(13) + 0x30, 0x40);
    ps_write32(b, desc(13) + 0x34, kInput);
    std::memcpy(b + bin(2) + 0x30, "JOBCRT Ver", 10);
    ps_write8(b, bin(2) + 0x3A, 0x34);
    ps_write8(b, bin(2) + 0x3B, 0x02);
    ps_write32(b, bin(2) + 0x3C, 0x80);
    ps_write32(b, bin(2) + 0x40, 0x40);
    std::memcpy(b + bin(2) + 0x90, "%SPURS JOB INFO%", 16);
    ps_write32(b, bin(2) + 0xA4, 0x10);
    ps_write32(b, bin(2) + 0xA8, 0xFFFFFFFF);
    for (const u32 mode : {1u, 0u}) {
        call("cellSpursJobQueueGetSuspendedJobSize", {desc(13), 0x100, mode, out(51)});
        log.answers.push_back(std::format("  fixed {:#x}", ps_read32(b, out(51))));
    }
    ps_write8(b, bin(2) + 0x3A, 0x33);
    call("cellSpursJobQueueGetSuspendedJobSize", {desc(13), 0x100, 1, out(51)});
    call("cellSpursJobQueueGetSuspendedJobSize", {desc(12), 0x100, 0, 0});
    call("cellSpursJobQueueGetSuspendedJobSize", {desc(12), 0x100, 0, out(50) + 1});
    call("cellSpursJobQueueGetSuspendedJobSize", {desc(12) + 8, 0x100, 0, out(50)});
    snap("suspended sizes");

    // Shutdown and join: refused with handles open, then done.
    call("cellSpursShutdownJobQueue", {jq(0)});
    for (const u32 h : {0u, 1u, 3u, 4u, 5u, 2u}) call("cellSpursJobQueueClose", {jq(0), h});
    call("cellSpursJobQueuePortFinalize", {port(1)});
    call("cellSpursJobQueueGetHandleCount", {jq(0)});
    for (u32 h = 0; h < 64; h++) // what the ports left open
        if (ps_read32(b, jq(0) + 0x580 + 4 * (h / 32)) & (0x80000000u >> (h % 32))) call("cellSpursJobQueueClose", {jq(0), h});
    // (A join waits for the shutdown: only after one.)
    if (call("cellSpursShutdownJobQueue", {jq(0)}) == 0) {
        call("cellSpursShutdownJobQueue", {jq(0)});
        call("cellSpursJoinJobQueue", {jq(0), out(60)});
        call("cellSpursJoinJobQueue", {jq(0), out(60)});
    }
    call("cellSpursJoinJobQueue", {jq(0), 0});
    call("cellSpursShutdownJobQueue", {0});
    call("cellSpursShutdownJobQueue", {jq(0) + 8});
    for (u32 h = 0; h < 64; h++)
        if (ps_read32(b, jq(3) + 0x580 + 4 * (h / 32)) & (0x80000000u >> (h % 32))) call("cellSpursJobQueueClose", {jq(3), h});
    if (call("cellSpursShutdownJobQueue", {jq(3)}) == 0) call("cellSpursJoinJobQueue", {jq(3), out(61)});
    snap("joined");
    // A depth of 0x8000 (the largest) and a queue on a joined one's place.
    CHECK(create({1, 0x330000, kSpurs, jq(0), attr(0), 0, kBigList, 0x8000, 1, kPrio}) == 0);
    snap("again");
    log.calls = s.calls.list;
    return log;
}

// ---------------------------------------------------------------------------------------- jobs on SPURS

// The recorder job (as tests/hle/test_spurs_jobchain.cpp's, with the job runtime's entry): it copies its first input
// quadword and its descriptor's work area to the output EA in its work area, then a done word 0x80 bytes after.
std::vector<u32> recorder()
{
    std::vector<u32> c = {as::ila(2, 0x10000), as::ila(2, 0x20000), as::ila(2, 0x30000), as::ila(2, 0x3F000),
        as::xori(80, 80, 4), as::br(4), as::xori(80, 80, 4), as::br(0x30 - 0x1C), 0, 0, 0, 0};
    const auto e = [&](u32 w) { c.push_back(w); };
    const auto put = [&](u32 ls_reg, u32 ea_reg, s32 size) {
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
    e(as::rotqbyi(11, 10, 12)); // $11: the output buffer
    e(as::lqd(13, 0x00, 3));    // the input buffer
    e(as::lqd(14, 0x00, 13));
    e(as::stqd(14, 0x00, 11));
    e(as::lqd(14, 0x60, 4));    // the descriptor's work area
    e(as::stqd(14, 0x10, 11));
    e(as::lqd(19, 0x20, 3));
    e(as::rotqbyi(19, 19, 4)); // $19: the DMA tag
    e(as::lqd(20, 0x60, 4));   // the output EA
    put(11, 20, 0x20);
    e(as::il(21, 1));
    e(as::stqd(21, 0x40, 11));
    e(as::ai(24, 11, 0x40));
    e(as::ai(25, 20, 0x80));
    put(24, 25, 0x10);
    e(as::bi(0));
    return c;
}

constexpr u32 kJobBin = kData + 0x22000, kJobBinSize = 0x200;
constexpr u32 kJobOut = kData + 0x24000; // 0x100 per job

u32 job_out(u32 i) { return kJobOut + 0x100 * i; }
u32 jdesc(u32 i) { return kData + 0x40000 + 0x100 * i; } // the job script's descriptors

// A descriptor for the recorder: one 16-byte input, a 0x40-byte output, its work area the output EA and the job's
// number.
void job_desc(u8* b, u32 at, u32 i, u32 size = 0x100)
{
    std::memset(b + at, 0, size);
    ps_write32(b, at + 0x04, kJobBin);
    ps_write16(b, at + 0x08, static_cast<u16>(kJobBinSize / 16));
    ps_write16(b, at + 0x0A, 8);
    ps_write32(b, at + 0x14, 0x10);
    ps_write32(b, at + 0x18, 0x40);
    ps_write16(b, at + 0x1E, 0x10);
    ps_write32(b, at + 0x30, 0x10);
    ps_write32(b, at + 0x34, kInput + 0x10 * (i % 64));
    ps_write32(b, at + 0x60, job_out(i));
    ps_write32(b, at + 0x64, i);
}

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(20000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return true;
}

// Jobs through a queue, a port, a port2 and the pool: blocking pushes past the ring's depth (they wait for the policy
// module to make room), semaphores the jobs release, syncs and flushes, then shutdown and join. The log: answers, and
// for each job what it recorded.
Log job_script(Side& s, bool entry_lock, bool busy)
{
    Log log;
    u8* b = s.b();
    const auto call = [&](const char* name, std::initializer_list<u64> args) {
        const u32 r = s.call(name, args);
        log.answers.push_back(std::format("{} -> {:#x}", name, r));
        return r;
    };
    const std::vector<u32> code = recorder();
    REQUIRE(code.size() * 4 <= kJobBinSize);
    std::memset(b + kJobBin, 0, kJobBinSize);
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kJobBin + 4 * static_cast<u32>(i), code[i]);
    for (u32 i = 0; i < 0x400; i++) ps_write8(b, kInput + i, static_cast<u8>(i * 7 + 3));
    std::memset(b + kPrio, 1, 8);
    call("cellSpursJobQueueAttributeInitialize", {attr(0)});
    call("cellSpursJobQueueAttributeSetSubmitWithEntryLock", {attr(0), entry_lock});
    call("cellSpursJobQueueAttributeSetDoBusyWaiting", {attr(0), busy});
    call("cellSpursJobQueueAttributeSetMaxSizeJobDescriptor", {attr(0), 0x380});
    const u32 counts[8] = {0, 0, 4, 0, 0, 0, 0, 2};
    for (u32 i = 0; i < 8; i++) ps_write32(b, kPoolDesc + 4 * i, counts[i]);
    REQUIRE(call("_cellSpursCreateJobQueueWithJobDescriptorPool",
                {1, 0x330000, kSpurs, jq(0), attr(0), kPool, kPoolDesc, 0, list(0), 16, 1, kPrio}) == 0);
    REQUIRE(call("cellSpursJobQueueOpen", {jq(0), out(0)}) == 0);
    const u32 h = ps_read32(b, out(0));
    call("cellSpursJobQueueSemaphoreInitialize", {sem(0), jq(0)});
    // 24 jobs on a ring of 16 (the policy module reads the waiting ring after it in whole lines: a depth of 16s): the
    // pushes wait for room.
    u32 job = 0;
    for (; job < 24; job++) {
        job_desc(b, jdesc(job), job);
        call("_cellSpursJobQueuePushJobBody", {jq(0), h, jdesc(job), 0x100, job % 4, sem(0), 1});
    }
    call("cellSpursJobQueueSemaphoreAcquire", {sem(0), 24});
    // A job list of 3, a sync on its tags and a flush.
    for (u32 k = 0; k < 3; k++) job_desc(b, kPortBuf + 0x100 * k, job + k);
    ps_write32(b, jdesc(60), 3);
    ps_write32(b, jdesc(60) + 4, 0x100);
    ps_write64(b, jdesc(60) + 8, kPortBuf); // CellSpursJobList: count, job size, u64 EA of the jobs
    call("_cellSpursJobQueuePushJobListBody", {jq(0), h, jdesc(60), 5, sem(0), 1});
    job += 3;
    call("_cellSpursJobQueuePushSync", {jq(0), h, 0x0020, 1});
    call("_cellSpursJobQueuePushFlush", {jq(0), h, 1});
    call("cellSpursJobQueueSemaphoreAcquire", {sem(0), 3});
    // A port: jobs and copies counted by its semaphore, then its sync.
    call("cellSpursJobQueuePortInitializeWithDescriptorBuffer", {port(0), jq(0), kPortBuf + 0x800, 0x100, 4, 1});
    for (u32 k = 0; k < 4; k++, job++) {
        job_desc(b, jdesc(job), job);
        if (k % 2) call("_cellSpursJobQueuePortCopyPushJobBody", {port(0), jdesc(job), 0x100, k, 1, 1});
        else call("_cellSpursJobQueuePortPushJobBody", {port(0), jdesc(job), 0x100, k, 1, 1});
    }
    call("cellSpursJobQueuePortSync", {port(0)});
    call("cellSpursJobQueuePortFinalize", {port(0)});
    // A port2: its own jobs, copies into the pool (given back after they ran: more than the pool holds), its sync.
    call("cellSpursJobQueuePort2Create", {port2(0), jq(0)});
    for (u32 k = 0; k < 6; k++, job++) {
        job_desc(b, jdesc(job), job);
        if (k % 2) call("_cellSpursJobQueuePort2CopyPushJobBody", {port2(0), jdesc(job), 0x100, 0x100, k, 1});
        else call("_cellSpursJobQueuePort2PushJobBody", {port2(0), jdesc(job), 0x100, k, 1});
    }
    call("cellSpursJobQueuePort2Sync", {port2(0), 0});
    call("cellSpursJobQueuePort2Destroy", {port2(0)});
    // The pool: allocations (waiting for the descriptors the policy module gives back) pushed to be given back.
    for (u32 k = 0; k < 6; k++, job++) {
        ps_write32(b, out(1), 0);
        call("_cellSpursJobQueueAllocateJobDescriptorBody", {jq(0), h, 0x380, 0, out(1)});
        const u32 d = ps_read32(b, out(1));
        job_desc(b, d, job, 0x380);
        call("_cellSpursJobQueuePushAndReleaseJobBody", {jq(0), h, d, 0x380, k, 0, sem(0)});
    }
    call("cellSpursJobQueueSemaphoreAcquire", {sem(0), 6});
    const u32 jobs = job;
    for (u32 i = 0; i < jobs; i++) {
        const bool done = wait_for([&] { return ps_read32(b, job_out(i) + 0x80) != 0; });
        log.answers.push_back(std::format("job {}: done {} input {:08x}{:08x} work {:08x} {:08x}", i, done,
            ps_read32(b, job_out(i)), ps_read32(b, job_out(i) + 4), ps_read32(b, job_out(i) + 0x10),
            ps_read32(b, job_out(i) + 0x14)));
    }
    call("cellSpursJobQueueClose", {jq(0), h});
    call("cellSpursShutdownJobQueue", {jq(0)});
    call("cellSpursJoinJobQueue", {jq(0), out(2)});
    call("cellSpursJobQueueGetError", {jq(0), out(3), out(4)});
    log.answers.push_back(std::format("exit {:#x} error {:#x} cause {:#x} semaphore {:#x}", ps_read32(b, out(2)),
        ps_read32(b, out(3)), ps_read32(b, out(4)), ps_read64(b, sem(0))));
    // The queue at rest: the rings' headers, the waiters' event flag bits (libspurs_jq keeps bit 0 once a semaphore
    // took it), the pool's free lists, the signalled jobs, the handles. How often a pusher or an allocation had to wait
    // depends on how fast the SPU drains, so a waiting ring shows only whether it is drained (read index = write index)
    // and its bytes other than the lap.
    const auto waiting = [&](u32 at) {
        const u64 h = ps_read64(b, at);
        const bool drained = (h >> 48) == ((h >> 32) & 0xFFFF);
        return std::format("{} {:#x}", drained ? "drained" : "busy", h & 0xFFFF00FFu);
    };
    log.answers.push_back(std::format("headers {:#x} {} bits {:08x}{:08x}{:08x}{:08x} signals {:#x} handles {:#x}",
        ps_read64(b, jq(0)), waiting(jq(0) + 8), ps_read32(b, jq(0) + 0x88), ps_read32(b, jq(0) + 0x8C),
        ps_read32(b, jq(0) + 0x90), ps_read32(b, jq(0) + 0x94), ps_read32(b, jq(0) + 0x68), ps_read32(b, jq(0) + 0x70)));
    for (u32 c = 0; c < 8; c++)
        log.answers.push_back(std::format("class {}: {:#x} {}", c, ps_read64(b, jq(0) + 0x680 + 16 * c),
            waiting(jq(0) + 0x688 + 16 * c)));
    return log;
}

} // namespace

TEST_CASE("cellSpursJq: the PPU side, call for call against libspurs_jq (workers off)", "[hle][spursjq][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH not set");
    Log sonys, mine;
    {
        Side s(true, false, *flash);
        sonys = ppu_script(s);
    }
    {
        Side s(false, false, *flash);
        mine = ppu_script(s);
    }
    const usize checks = compare(mine, sonys, {0, 1, 2, 3, 4});
    std::fprintf(stderr, "cellSpursJq PPU script: %zu calls, %zu checkpoints, %zu lv2 calls, %zu checks\n",
        mine.answers.size(), mine.snaps.size(), mine.calls.size(), checks);
    if (const char* dump = std::getenv("PSPOCKET_JQ_DUMP")) {
        if (FILE* fp = std::fopen(dump, "w")) {
            for (usize i = 0; i < sonys.answers.size(); i++)
                std::fprintf(fp, "%s | %s\n", sonys.answers[i].c_str(), mine.answers[i].c_str());
            for (const std::string& c : sonys.calls) std::fprintf(fp, "lv2 %s\n", c.c_str());
            std::fclose(fp);
        }
    }
}

TEST_CASE("cellSpursJq: jobs through libspurs_jq's policy module, the PPU side ours or Sony's", "[hle][spursjq][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH not set");
    for (const auto& [entry_lock, busy] : {std::pair{false, false}, std::pair{true, false}, std::pair{false, true}}) {
        INFO("entry lock " << entry_lock << " busy waiting " << busy);
        Log sonys, mine;
        {
            Side s(true, true, *flash);
            sonys = job_script(s, entry_lock, busy);
        }
        {
            Side s(false, true, *flash);
            mine = job_script(s, entry_lock, busy);
        }
        REQUIRE(mine.answers.size() == sonys.answers.size());
        for (usize i = 0; i < mine.answers.size(); i++) {
            INFO("step " << i);
            CHECK(mine.answers[i] == sonys.answers[i]);
        }
        std::fprintf(stderr, "cellSpursJq jobs (entry lock %d, busy %d): %zu steps\n", entry_lock, busy,
            mine.answers.size());
    }
}


// Speed (rule 8): pushes and non-blocking acquires on a queue no SPU drains, pspocket's against libspurs_jq's (the
// firmware module runs interpreted in tests: an upper bound for its translated code).
TEST_CASE("cellSpursJq: push timing", "[.bench]")
{
    const auto flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH not set");
    for (const bool sony : {false, true}) {
        Side s(sony, false, *flash);
        u8* b = s.b();
        std::memset(b + kPrio, 1, 8);
        s.call("cellSpursJobQueueAttributeInitialize", {attr(0)});
        REQUIRE(s.call("_cellSpursCreateJobQueue", {1, 0x330000, kSpurs, jq(0), attr(0), 0, kBigList, 0x8000, 1, kPrio}) == 0);
        REQUIRE(s.call("cellSpursJobQueueOpen", {jq(0), out(0)}) == 0);
        const u32 h = ps_read32(b, out(0));
        job_desc(b, jdesc(0), 0);
        const u32 nid = recomp::nid_of("_cellSpursJobQueuePushJobBody");
        const u32 opd = sony ? s.module->module->functions.at({"cellSpursJq", nid})
                             : s.f.hle->link_function("cellSpursJq", nid, "_cellSpursJobQueuePushJobBody");
        constexpr u32 kN = 20000;
        const auto t0 = std::chrono::steady_clock::now();
        for (u32 i = 0; i < kN; i++) {
            if (i % 0x8000 == 0x7FFF) ps_write64(b, jq(0), ps_read64(b, jq(0)) ^ 0x40); // drained (lap)
            call_guest(s.t.ctx, opd, {jq(0), h, jdesc(0), 0x100, i % 16, 0, 0});
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kN;
        std::fprintf(stderr, "push: %s %.0f ns each\n", sony ? "libspurs_jq (interpreted)" : "pspocket", ns);
    }
}
