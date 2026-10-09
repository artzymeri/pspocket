// cellDaisy (D-538) against libsre, Sony's own module of the library, from the user's installed system software
// (firmware_compare.hpp; skipped without it): the same script of calls goes to libsre's exports and to pspocket's, each
// in a process of its own with libsre loaded (so both have the same memory and lv2 IDs), and the answers, every byte
// the calls leave, the signal callbacks they make (with their arguments) and the lv2 calls (with theirs) must be the
// same: Lock over many random operations at several depths, _snprintf, the LFQueue2 calls on cellSync LFQueues (each
// side's own cellSync), the ScatterGatherInterlock with a release callback, with an event port, and with SPU threads
// (a group made here, never started), and one thread probing while another releases. No game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kLock = kData + 0x1000;
constexpr u32 kQueue = kData + 0x2000, kQueue2 = kData + 0x2100, kQueueBuf = kData + 0x3000;
constexpr u32 kAtomic = kData + 0x4000, kAtomic2 = kData + 0x4080;
constexpr u32 kSgi = kData + 0x4200, kSgi2 = kData + 0x4280, kIds = kData + 0x4300, kOut = kData + 0x4400;
constexpr u32 kStr = kData + 0x4800, kBuf = kData + 0x4C00;
constexpr u32 kCode = kData + 0x5000;    // the signal callbacks' code (0x100 apart), their descriptors at +0xF0
constexpr u32 kRecords = kData + 0x6000; // a count, the callbacks' result at +4, then (kind, r3, r4) 0x40 apart from +0x40
constexpr u32 kArea = kData + 0x1000, kAreaEnd = kData + 0x7000;
constexpr u32 kSpu = kData + 0x10000;    // the SPU group's ELF, image and attributes (not compared)
constexpr u32 kSysOut = kData + 0x8000;  // the test's own lv2 calls' outputs

// A signal callback: records (kind, r3, r4) at kRecords and returns the word at kRecords + 4.
void callback_code(u8* b, u32 at, u32 kind)
{
    const u32 words[] = {0x3D600000u | (kRecords >> 16), 0x616B0000u | (kRecords & 0xFFFF), 0x818B0000u, 0x1C0C0040u,
        0x7D2B0214u, 0x38000000u | kind, 0xF8090040u, 0xF8690048u, 0xF8890050u, 0x7C2004ACu, 0x398C0001u, 0x918B0000u,
        0x806B0004u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, at + 4 * i, words[i]);
    ps_write32(b, at + 0xF0, at);
    ps_write32(b, at + 0xF4, 0);
}
constexpr u32 desc(u32 kind) { return kCode + 0x100 * kind + 0xF0; }

// The lv2 calls the functions make, as text, per thread.
struct Trace {
    std::mutex m;
    std::thread::id main;
    u8* base = nullptr;
    std::vector<std::string> mine, other;
};

std::string cstr(u8* b, u32 at, u32 max)
{
    std::string s;
    for (u32 i = 0; i < max && b[at + i]; i++) s.push_back(static_cast<char>(b[at + i]));
    return s;
}

void observe(void* user, u32 n, const u64* a)
{
    auto& tr = *static_cast<Trace*>(user);
    u8* b = tr.base;
    std::string s;
    switch (n) {
    case 128:
        s = std::format("queue_create(-> {:#x}, protocol {}, type {}, \"{}\", key {:#x}, size {})", a[0],
            ps_read32(b, static_cast<u32>(a[1])), ps_read32(b, static_cast<u32>(a[1]) + 4),
            cstr(b, static_cast<u32>(a[1]) + 8, 8), a[2], a[3]);
        break;
    case 129: s = std::format("queue_destroy({:#x}, mode {})", a[0], a[1]); break;
    case 130: s = std::format("queue_receive({:#x}, {:#x}, timeout {})", a[0], a[1], a[2]); break;
    case 134: s = std::format("port_create(-> {:#x}, type {}, name {:#x})", a[0], a[1], a[2]); break;
    case 135: s = std::format("port_destroy({:#x})", a[0]); break;
    case 136: s = std::format("port_connect_local({:#x}, {:#x})", a[0], a[1]); break;
    case 137: s = std::format("port_disconnect({:#x})", a[0]); break;
    case 138: s = std::format("port_send({:#x}, {:#x}, {:#x}, {:#x})", a[0], a[1], a[2], a[3]); break;
    case 191: s = std::format("spu_connect_event({:#x}, {:#x}, {}, {})", a[0], a[1], a[2], a[3]); break;
    case 192: s = std::format("spu_disconnect_event({:#x}, {}, {})", a[0], a[1], a[2]); break;
    case 403: s = std::format("tty \"{}\"", std::string(reinterpret_cast<const char*>(b + a[1]), a[2])); break;
    // libsre's cellSyncLFQueueInitialize asks for the process's SDK version (getpid, get_sdk_version); pspocket's
    // cellSync reads it natively. Not cellDaisy's.
    case 1: case 25: return;
    default: s = std::format("syscall {}", n); break;
    }
    std::lock_guard lock(tr.m);
    (std::this_thread::get_id() == tr.main ? tr.mine : tr.other).push_back(std::move(s));
}

// One side's call of a function by module and name: r3's low word. pspocket's are called on a frame as call_guest
// makes for libsre's (so stack addresses in lv2 calls are the same), arguments past eight in its parameter save area.
using Call = std::function<u32(GuestThread&, const char*, const char*, std::vector<u64>)>;

Call mine()
{
    return [](GuestThread& t, const char*, const char* name, std::vector<u64> args) {
        const Function& fn = GuestThread::find(name);
        u8* b = t.f.base();
        const u64 saved = t.ctx.gpr[1];
        const u32 extra = args.size() > 8 ? static_cast<u32>(args.size()) - 8 : 0;
        t.ctx.gpr[1] = (saved - 0x80 - 8 * extra) & ~u64{15};
        ps_write64(b, static_cast<u32>(t.ctx.gpr[1]), saved);
        for (usize i = 0; i < args.size(); i++) {
            if (i < 8) t.ctx.gpr[3 + i] = args[i];
            else ps_write64(b, static_cast<u32>(t.ctx.gpr[1]) + 0x70 + 8 * static_cast<u32>(i - 8), args[i]);
        }
        fn.fn(t.ctx, b);
        t.ctx.gpr[1] = saved;
        return static_cast<u32>(t.ctx.gpr[3]);
    };
}

Call sonys(const FirmwareLibrary& sre)
{
    return [&sre](GuestThread& t, const char* module, const char* name, std::vector<u64> args) {
        const u32 opd = sre.module->functions.at({module, recomp::nid_of(name)});
        std::array<u64, 12> a{};
        std::copy(args.begin(), args.end(), a.begin());
        switch (args.size()) {
        case 9: return static_cast<u32>(call_guest(t.ctx, opd, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]}));
        case 10:
            return static_cast<u32>(call_guest(t.ctx, opd, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]}));
        default: REQUIRE(args.size() <= 8); return static_cast<u32>(call_guest(t.ctx, opd, {a[0], a[1], a[2], a[3], a[4],
            a[5], a[6], a[7]}));
        }
    };
}

// The mangled names (libsre exports each under both _QN and _ZN; the script uses one or the other).
namespace n {
constexpr const char* kInit = "_ZN4cell5Daisy4Lock10initializeEj";
constexpr const char* kPushOpen = "_QN4cell5Daisy4Lock8pushOpenEv";
constexpr const char* kPushClose = "_ZN4cell5Daisy4Lock9pushCloseEv";
constexpr const char* kPopOpen = "_QN4cell5Daisy4Lock7popOpenEv";
constexpr const char* kPopClose = "_ZN4cell5Daisy4Lock8popCloseEv";
constexpr const char* kHead = "_QN4cell5Daisy4Lock18getNextHeadPointerEv";
constexpr const char* kTail = "_ZN4cell5Daisy4Lock18getNextTailPointerEv";
constexpr const char* kConsume = "_QN4cell5Daisy4Lock15completeConsumeEj";
constexpr const char* kProduce = "_ZN4cell5Daisy4Lock15completeProduceEj";
constexpr const char* kSnprintf = "_QN4cell5Daisy9_snprintfEPcjPKcz";
constexpr const char* kSnprintfZ = "_ZN4cell5Daisy9_snprintfEPcjPKcz";
constexpr const char* kGetPop = "_QN4cell5Daisy21LFQueue2GetPopPointerEPNS0_8LFQueue2EPij";
constexpr const char* kCompletePop = "_ZN4cell5Daisy26LFQueue2CompletePopPointerEPNS0_8LFQueue2EiPFiPvjEj";
constexpr const char* kQPushOpen = "_ZN4cell5Daisy16LFQueue2PushOpenEPNS0_8LFQueue2E";
constexpr const char* kQPushClose = "_QN4cell5Daisy17LFQueue2PushCloseEPNS0_8LFQueue2EPFiPvjE";
constexpr const char* kQPopOpen = "_QN4cell5Daisy15LFQueue2PopOpenEPNS0_8LFQueue2E";
constexpr const char* kQPopClose = "_ZN4cell5Daisy16LFQueue2PopCloseEPNS0_8LFQueue2EPFiPvjE";
constexpr const char* kUnfinished = "_QN4cell5Daisy29LFQueue2HasUnfinishedConsumerEPNS0_8LFQueue2Ej";
constexpr const char* kSgiCallback = "_ZN4cell5Daisy22ScatterGatherInterlockC1EPVNS0_16_AtomicInterlockEjPvPFiS5_jE";
constexpr const char* kSgiCallback2 = "_QN4cell5Daisy22ScatterGatherInterlockC2EPVNS0_16_AtomicInterlockEjPvPFiS5_jE";
constexpr const char* kSgiSpus = "_QN4cell5Daisy22ScatterGatherInterlockC1EPVNS0_16_AtomicInterlockEjPjjh";
constexpr const char* kSgiSpus2 = "_ZN4cell5Daisy22ScatterGatherInterlockC2EPVNS0_16_AtomicInterlockEjPjjh";
constexpr const char* kSgiDtor = "_ZN4cell5Daisy22ScatterGatherInterlockD1Ev";
constexpr const char* kSgiDtor2 = "_QN4cell5Daisy22ScatterGatherInterlockD2Ev";
constexpr const char* kProceed = "_QN4cell5Daisy22ScatterGatherInterlock21proceedSequenceNumberEv";
constexpr const char* kProbe = "_ZN4cell5Daisy22ScatterGatherInterlock5probeEj";
constexpr const char* kRelease = "_QN4cell5Daisy22ScatterGatherInterlock7releaseEv";
} // namespace n

// The functions with no result: their r3 is whatever the code left there, not compared.
bool returns_nothing(std::string_view name)
{
    for (const char* v : {n::kPushOpen, n::kPushClose, n::kPopOpen, n::kPopClose, n::kConsume, n::kProduce,
             n::kQPushOpen, n::kQPopOpen, n::kSgiCallback, n::kSgiCallback2, n::kSgiSpus, n::kSgiSpus2, n::kSgiDtor,
             n::kSgiDtor2, n::kProceed})
        if (name == v) return true;
    return false;
}

// The lv2 objects the test makes in each process (the same IDs in both).
struct Objects {
    u32 queue = 0, port = 0, queue2 = 0, spu_queue = 0;
    u32 threads[2] = {};
};

Objects make_objects(GuestThread& g)
{
    lv2::PpuThread& t = *g.t;
    u8* b = g.f.base();
    Objects o;
    const u32 attr = kSysOut + 0x100;
    ps_write32(b, attr, lv2::SYS_SYNC_PRIORITY);
    ps_write32(b, attr + 4, 1);
    ps_write64(b, attr + 8, 0);
    REQUIRE(lv2::sys_event_queue_create(t, kSysOut, attr, 0, 16) == lv2::CELL_OK);
    o.queue = ps_read32(b, kSysOut);
    REQUIRE(lv2::sys_event_port_create(t, kSysOut, 1, 0) == lv2::CELL_OK);
    o.port = ps_read32(b, kSysOut);
    REQUIRE(lv2::sys_event_port_connect_local(t, o.port, o.queue) == lv2::CELL_OK);
    REQUIRE(lv2::sys_event_queue_create(t, kSysOut, attr, 0, 16) == lv2::CELL_OK);
    o.queue2 = ps_read32(b, kSysOut);
    REQUIRE(lv2::sys_event_queue_create(t, kSysOut, attr, 0, 16) == lv2::CELL_OK);
    o.spu_queue = ps_read32(b, kSysOut);
    // Two SPU threads of a group never started: an ELF of one `stop`.
    const u32 elf = kSpu;
    std::memset(b + elf, 0, 0x200);
    static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(b + elf, kIdent, 16);
    ps_write16(b, elf + 0x10, 2);
    ps_write16(b, elf + 0x12, 0x17);
    ps_write32(b, elf + 0x14, 1);
    ps_write32(b, elf + 0x1C, 0x34);
    ps_write16(b, elf + 0x28, 0x34);
    ps_write16(b, elf + 0x2A, 0x20);
    ps_write16(b, elf + 0x2C, 1);
    ps_write32(b, elf + 0x34, 1);
    ps_write32(b, elf + 0x38, 0x100);
    ps_write32(b, elf + 0x44, 4);
    ps_write32(b, elf + 0x48, 0x104);
    ps_write32(b, elf + 0x4C, 7);
    ps_write32(b, elf + 0x50, 4);
    const u32 img = kSpu + 0x400, name = kSpu + 0x420, gattr = kSpu + 0x440, tattr = kSpu + 0x460, args = kSpu + 0x480;
    REQUIRE(lv2::_sys_spu_image_import(t, img, elf, 0x200, 0) == lv2::CELL_OK);
    std::memcpy(b + name, "daisy", 6);
    ps_write32(b, gattr, 6);
    ps_write32(b, gattr + 4, name);
    ps_write32(b, gattr + 8, 0);
    REQUIRE(lv2::sys_spu_thread_group_create(t, kSysOut, 2, 100, gattr) == lv2::CELL_OK);
    const u32 group = ps_read32(b, kSysOut);
    ps_write32(b, tattr, name);
    ps_write32(b, tattr + 4, 6);
    ps_write32(b, tattr + 8, 0);
    std::memset(b + args, 0, 0x20);
    for (u32 i = 0; i < 2; i++) {
        REQUIRE(lv2::sys_spu_thread_initialize(t, kSysOut, group, i, img, tattr, args) == lv2::CELL_OK);
        o.threads[i] = ps_read32(b, kSysOut);
    }
    return o;
}

struct Lcg {
    u32 s;
    u32 next() { return s = s * 1103515245u + 12345u, s >> 8; }
};

std::vector<std::string> script(GuestThread& t, const Call& call, const Objects& o)
{
    u8* b = t.f.base();
    std::vector<std::string> log;
    std::memset(b + kArea, 0, kAreaEnd - kArea);
    for (u32 k = 1; k <= 3; k++) callback_code(b, kCode + 0x100 * k, k);
    const auto step = [&](const char* module, const char* name, std::vector<u64> args) {
        const u32 r = call(t, module, name, args);
        if (returns_nothing(name)) log.push_back(name);
        else log.push_back(std::format("{} -> {:#x}", name, r));
        return r;
    };
    const auto daisy = [&](const char* name, std::vector<u64> args) { return step("cellDaisy", name, std::move(args)); };
    const auto snapshot = [&](std::string_view what) {
        std::string s = std::format("{}:", what);
        for (u32 a = kArea; a < kAreaEnd; a += 4) {
            if (a >= kCode && a < kRecords) continue;
            if (const u32 w = ps_read32(b, a)) s += std::format(" {:x}={:x}", a - kArea, w);
        }
        log.push_back(s);
        ps_write32(b, kRecords, 0); // the callbacks' records start again
        std::memset(b + kRecords + 0x40, 0, 0x800);
    };

    // ---- Lock: the checks, then random operations at several depths.
    daisy(n::kInit, {kLock, 0x8000});
    daisy(n::kInit, {kLock, 4});
    daisy(n::kInit, {kLock, 4});
    daisy(n::kInit, {kLock, 5});
    snapshot("lock set up");
    Lcg rng{0x5EED};
    for (const u32 depth : {1u, 2u, 3u, 4u, 8u, 16u, 17u, 0x7FFFu}) {
        std::memset(b + kLock, 0, 0x20);
        daisy(n::kInit, {kLock, depth});
        std::vector<u32> produced, consumed;
        for (u32 i = 0; i < 400; i++) {
            const u32 op = rng.next() % 12;
            const auto pick = [&](std::vector<u32>& from) -> u32 {
                if (from.empty() || rng.next() % 8 == 0) return rng.next() % (2 * depth + 3);
                const usize k = rng.next() % from.size();
                const u32 p = from[k];
                from.erase(from.begin() + static_cast<std::ptrdiff_t>(k));
                return p;
            };
            switch (op) {
            case 0: daisy(n::kPushOpen, {kLock}); break;
            case 1: daisy(n::kPushClose, {kLock}); break;
            case 2: daisy(n::kPopOpen, {kLock}); break;
            case 3: daisy(n::kPopClose, {kLock}); break;
            case 4: case 5: {
                const u32 r = daisy(n::kTail, {kLock});
                if (r < 0x40000000) produced.push_back((r + 2 * depth - 1) % (2 * depth));
                break;
            }
            case 6: case 7: {
                const u32 r = daisy(n::kHead, {kLock});
                if (r < 0x40000000) consumed.push_back((r + 2 * depth - 1) % (2 * depth));
                break;
            }
            case 8: case 9: daisy(n::kProduce, {kLock, pick(produced)}); break;
            default: daisy(n::kConsume, {kLock, pick(consumed)}); break;
            }
            std::string s = "lock";
            for (u32 a = 0; a < 0x20; a += 4) s += std::format(" {:x}", ps_read32(b, kLock + a));
            log.push_back(s);
        }
    }

    // ---- _snprintf: liblv2's formatter, arguments in registers and on the stack.
    std::memcpy(b + kStr, "n=%d s=%s x=%#x c=%c [%5.2s] [%-4d] %u %lld|", 46);
    std::memcpy(b + kStr + 0x80, "word", 5);
    for (const u32 count : {0x80u, 12u, 1u, 0u}) {
        std::memset(b + kBuf, 0xEE, 0x100);
        daisy(n::kSnprintf, {kBuf, count, kStr, u64(-42), kStr + 0x80, 0xBEEF, 'q', kStr + 0x80, 7, 0xFFFFFFFF});
        snapshot(std::format("snprintf {}", count));
    }
    std::memcpy(b + kStr + 0x40, "%lld %d", 8);
    daisy(n::kSnprintfZ, {kBuf, 0x40, kStr + 0x40, 0x123456789ABCDEFull, 5});
    snapshot("snprintf ll");

    // ---- LFQueue2 on cellSync LFQueues (SPU2PPU, 8 deep, 16-byte entries; and a PPU2SPU one).
    step("cellSync", "cellSyncLFQueueInitialize", {kQueue, kQueueBuf, 16, 8, 1, 0x12345600});
    step("cellSync", "cellSyncLFQueueInitialize", {kQueue2, kQueueBuf + 0x800, 16, 8, 2, 0});
    snapshot("queues set up");
    daisy(n::kGetPop, {kQueue2, kOut, 0});
    daisy(n::kGetPop, {kQueue, kOut, 1}); // blocking, no event queue attached: STAT
    daisy(n::kGetPop, {kQueue, kOut, 0}); // empty
    ps_write16(b, kQueue + 8, 3);         // three pushes completed
    for (u32 i = 0; i < 4; i++) {
        daisy(n::kGetPop, {kQueue, kOut, 0});
        log.push_back(std::format("pointer {}", ps_read32(b, kOut)));
    }
    snapshot("three popped");
    // Completions out of order, with Daisy's pop waiters and cellSync's SPU waiters.
    ps_write32(b, kRecords + 4, 0);
    ps_write16(b, kQueue + 0x84, 0xA001); // waiters 0, 2 and 15
    ps_write16(b, kQueue + 0xD0, 9);      // waits for pointer 9: too far
    ps_write16(b, kQueue + 0xD4, 2);
    ps_write16(b, kQueue + 0xEE, 1);
    ps_write16(b, kQueue + 0xB0, 0x70);
    ps_write16(b, kQueue + 0xB4, 0x72);
    ps_write16(b, kQueue + 0xCE, 0x7F);
    daisy(n::kCompletePop, {kQueue, 1, desc(1), 0});
    snapshot("completed 1");
    daisy(n::kCompletePop, {kQueue, 0, desc(1), 0});
    snapshot("completed 0");
    ps_write16(b, kQueue + 0x50, (0u << 10) | (0u << 5) | 2u); // cellSync's pop side: two SPU waiters
    ps_write16(b, kQueue + 0x52, 0x31);
    ps_write16(b, kQueue + 0x54, 0x32);
    daisy(n::kCompletePop, {kQueue, 2, desc(2), 0});
    snapshot("completed 2 (cellSync signals)");
    daisy(n::kCompletePop, {kQueue2, 0, desc(2), 0}); // the other way: cellSync's PERM
    ps_write16(b, kQueue + 0x50, 0); // no SPU waiter left (with one, a quiet completion waits for it to go)
    daisy(n::kCompletePop, {kQueue, 5, desc(2), 1});
    snapshot("completed 5, noQueueFull");
    // Closed: nothing left and no pusher -> 3; a pusher still open -> AGAIN.
    b[kQueue + 0x81] = 4;
    daisy(n::kGetPop, {kQueue, kOut, 0});
    b[kQueue + 0x80] = 1;
    daisy(n::kGetPop, {kQueue, kOut, 0});
    b[kQueue + 0x80] = 0;
    // An event queue attached; a wait already marked (m_h3): the pointer taken, a sleep (an event is there), the mark
    // cleared, and the next pointer taken.
    ps_write32(b, kQueue + 0x20, 0);
    ps_write32(b, kQueue + 0x7C, o.queue);
    ps_write16(b, kQueue + 8, 7);
    ps_write16(b, kQueue + 4, 1);
    REQUIRE(lv2::sys_event_port_send(*t.t, o.port, 1, 2, 3) == lv2::CELL_OK);
    daisy(n::kGetPop, {kQueue, kOut, 0});
    log.push_back(std::format("pointer {}", ps_read32(b, kOut)));
    daisy(n::kGetPop, {kQueue, kOut, 1});
    log.push_back(std::format("pointer {}", ps_read32(b, kOut)));
    daisy(n::kGetPop, {kQueue, kOut, 3});
    log.push_back(std::format("pointer {}", ps_read32(b, kOut)));
    snapshot("after waits");
    ps_write16(b, kQueue + 8, 0); // nothing pushed past; closed, no pusher: 3 even blocking
    ps_write16(b, kQueue + 6, 0);
    daisy(n::kGetPop, {kQueue, kOut, 1});
    snapshot("closed, blocking");

    // Pushers: two open, the first close leaves one; the last close signals the push side's SPU waiters in its ring.
    b[kQueue + 0x81] = 0;
    daisy(n::kQPushOpen, {kQueue});
    daisy(n::kQPushOpen, {kQueue});
    snapshot("two pushers");
    daisy(n::kQPushClose, {kQueue, desc(3)});
    ps_write16(b, kQueue + 0x30, (28u << 10) | (5u << 5) | 1u); // head 28, tail 1: slots 13, 14, then 0
    for (u32 i = 0; i < 15; i++) ps_write16(b, kQueue + 0x32 + 2 * i, static_cast<u16>(0x100 + i));
    daisy(n::kQPushClose, {kQueue, desc(3)});
    snapshot("last pusher closed");
    daisy(n::kQPushClose, {kQueue, desc(3)});
    // Poppers: the last close closes the queue and signals the close waiters until one fails.
    daisy(n::kQPopOpen, {kQueue});
    daisy(n::kQPopOpen, {kQueue});
    daisy(n::kQPopClose, {kQueue, desc(1)});
    snapshot("one popper left");
    ps_write16(b, kQueue + 0x82, 0xA021);
    for (u32 i = 0; i < 16; i++) ps_write16(b, kQueue + 0x90 + 2 * i, static_cast<u16>(0x200 + i));
    daisy(n::kQPopClose, {kQueue, desc(1)});
    snapshot("last popper closed");
    daisy(n::kQPopOpen, {kQueue});
    ps_write16(b, kQueue + 0x82, 0xA021);
    ps_write32(b, kRecords + 4, 0x55);   // the second signal fails
    daisy(n::kQPopClose, {kQueue, desc(2)});
    ps_write32(b, kRecords + 4, 0);
    snapshot("a signal failed");
    daisy(n::kQPopClose, {kQueue, desc(1)});
    // HasUnfinishedConsumer.
    b[kQueue + 0x81] = 2;
    daisy(n::kUnfinished, {kQueue, 0});
    b[kQueue + 0x81] = 4;
    ps_write32(b, kQueue + 0x20, 0xFFFFFFFF); // no event queue
    for (const u32 popped : {5u, 4u, 3u}) {
        ps_write16(b, kQueue + 0, static_cast<u16>(popped));
        ps_write16(b, kQueue + 8, 5);
        for (const u32 flags : {0u, 1u, 2u, 3u, 4u}) daisy(n::kUnfinished, {kQueue, flags});
        snapshot(std::format("unfinished, {} popped of 5", popped));
    }
    ps_write16(b, kQueue + 0, 15); // past the wrap: 15 -> 1 is two
    ps_write16(b, kQueue + 8, 1);
    daisy(n::kUnfinished, {kQueue, 1});
    ps_write32(b, kQueue + 0x20, 0); // an event queue, and a wait marked: one sleep, then done
    ps_write16(b, kQueue + 0, 5);
    ps_write16(b, kQueue + 8, 5);
    ps_write16(b, kQueue + 0xC, 1);
    REQUIRE(lv2::sys_event_port_send(*t.t, o.port, 4, 5, 6) == lv2::CELL_OK);
    daisy(n::kUnfinished, {kQueue, 2});
    snapshot("unfinished after a wait");

    // ---- ScatterGatherInterlock with a release callback.
    daisy(n::kSgiCallback, {kSgi, kAtomic, 3, 0xABC, desc(2)});
    snapshot("callback interlock");
    daisy(n::kProbe, {kSgi, 0});
    for (u32 i = 0; i < 4; i++) daisy(n::kProceed, {kSgi});
    daisy(n::kProbe, {kSgi, 0});
    snapshot("counted");
    ps_write32(b, kAtomic + 4, 0x80000000); // a probe waits (as one that marked its wait), on this event queue
    ps_write64(b, kAtomic + 8, o.queue2);
    daisy(n::kRelease, {kSgi}); // an event port made, connected to the queue, an event sent
    snapshot("released to a waiter");
    ps_write32(b, kAtomic + 4, 7); // a callback argument + 1
    ps_write32(b, kRecords + 4, 0x66);
    daisy(n::kRelease, {kSgi});
    ps_write32(b, kRecords + 4, 0);
    daisy(n::kRelease, {kSgi});
    snapshot("released to the callback");
    ps_write32(b, kAtomic + 4, 0x80000000);
    daisy(n::kRelease, {kSgi}); // the port is there: only the event
    daisy(n::kSgiDtor, {kSgi});
    snapshot("callback interlock gone");
    daisy(n::kSgiCallback2, {kSgi2, kAtomic2, 1, 0, desc(1)});
    daisy(n::kSgiDtor2, {kSgi2}); // no port made: nothing to undo

    // ---- ScatterGatherInterlock with SPU threads.
    std::memset(b + kAtomic, 0, 0x100);
    daisy(n::kSgiSpus, {kSgi, kAtomic, 2, kIds, 0, 3});
    snapshot("no SPU threads");
    daisy(n::kSgiDtor, {kSgi});
    ps_write32(b, kIds, o.threads[0]);
    ps_write32(b, kIds + 4, o.threads[1]);
    daisy(n::kSgiSpus2, {kSgi, kAtomic, 2, kIds, 2, 3});
    snapshot("two SPU threads on port 3");
    daisy(n::kSgiDtor2, {kSgi});
    REQUIRE(lv2::sys_spu_thread_connect_event(*t.t, o.threads[0], o.spu_queue, 1, 4) == lv2::CELL_OK);
    REQUIRE(lv2::sys_spu_thread_connect_event(*t.t, o.threads[0], o.spu_queue, 1, 5) == lv2::CELL_OK);
    daisy(n::kSgiSpus, {kSgi, kAtomic, 1, kIds, 2, 4}); // ports 4 and 5 taken on the first thread: 6
    snapshot("two SPU threads, ports 4 and 5 taken");
    daisy(n::kSgiDtor, {kSgi});
    snapshot("SPU interlock gone");
    return log;
}

// A thread probing and waiting while another releases the interlock.
std::vector<std::string> probe_while_released(Fixture& f, GuestThread& t, const Call& call)
{
    u8* b = f.base();
    std::vector<std::string> log;
    std::memset(b + kAtomic, 0, 0x100);
    call(t, "cellDaisy", n::kSgiSpus, {kSgi, kAtomic, 1, kIds, 0, 0});
    call(t, "cellDaisy", n::kProceed, {kSgi});
    std::atomic<u32> released{0};
    std::thread other([&] {
        GuestThread g(f, 1, "releaser");
        const auto start = std::chrono::steady_clock::now();
        while (!(ps_read32(b, kAtomic + 4) & 0x80000000)) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(20)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        released = call(g, "cellDaisy", n::kRelease, {kSgi}) | 0x100;
    });
    const u32 r = call(t, "cellDaisy", n::kProbe, {kSgi, 1});
    other.join();
    log.push_back(std::format("probe -> {:#x}, release -> {:#x}", r, released.load()));
    std::string s = "atomic";
    for (u32 a = 0; a < 0x18; a += 4) s += std::format(" {:x}", ps_read32(b, kAtomic + a));
    s += " interlock";
    for (u32 a = 0; a < 0x3C; a += 4) s += std::format(" {:x}", ps_read32(b, kSgi + a));
    log.push_back(s);
    call(t, "cellDaisy", n::kSgiDtor, {kSgi});
    return log;
}

struct Run {
    std::vector<std::string> log, threads, calls, other_calls;
};

Run run(const std::string& flash, bool sony)
{
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sre(f, t, flash, "cellDaisy", false); // libsre in both processes: the same memory and IDs
    const Objects o = make_objects(t);
    Trace tr;
    tr.main = std::this_thread::get_id();
    tr.base = f.base();
    f.kernel->set_syscall_observer(&observe, &tr);
    const Call c = sony ? sonys(sre) : mine();
    Run r;
    r.log = script(t, c, o);
    r.threads = probe_while_released(f, t, c);
    f.kernel->set_syscall_observer(nullptr, nullptr);
    r.calls = tr.mine;
    r.other_calls = tr.other;
    return r;
}

void compare_logs(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(a.size(), b.size()); i++) {
        const std::string x = i < a.size() ? a[i] : "(none)", y = i < b.size() ? b[i] : "(none)";
        if (x == y) continue;
        if (++differ <= 40) UNSCOPED_INFO(std::format("{}: {}\n  (pspocket)\n  {}\n  (Sony)", i, x, y));
    }
    INFO(std::format("{} of {} lines differ", differ, b.size()));
    CHECK(differ == 0);
}

} // namespace

TEST_CASE("cellDaisy: every libsre export under both names; a lock's operations (pspocket)", "[hle][daisy]")
{
    usize names = 0;
    for (const Function& fn : registry().functions())
        if (fn.module == "cellDaisy") names++;
    CHECK(names == 52);
    Fixture f;
    GuestThread t(f, 0, "ppu");
    const Call c = mine();
    u8* b = f.base();
    std::memset(b + kLock, 0, 0x20);
    CHECK(c(t, "cellDaisy", n::kInit, {kLock, 2}) == 0);
    CHECK(c(t, "cellDaisy", n::kTail, {kLock}) == 1); // the next pointer to produce, moved on
    CHECK(c(t, "cellDaisy", n::kTail, {kLock}) == 2);
    CHECK(c(t, "cellDaisy", n::kHead, {kLock}) == 0x40000001); // nothing produced yet, producers may come
    c(t, "cellDaisy", n::kProduce, {kLock, 0});
    CHECK(ps_read16(b, kLock + 8) == 1);
    CHECK(c(t, "cellDaisy", n::kHead, {kLock}) == 1);
    c(t, "cellDaisy", n::kPushOpen, {kLock});
    c(t, "cellDaisy", n::kPushClose, {kLock});
    c(t, "cellDaisy", n::kConsume, {kLock, 0});
    CHECK(c(t, "cellDaisy", n::kHead, {kLock}) == 0x40000002); // drained and the producers gone
    std::memcpy(b + kStr, "%d-%s", 6);
    std::memcpy(b + kStr + 0x10, "ab", 3);
    CHECK(c(t, "cellDaisy", n::kSnprintf, {kBuf, 0x20, kStr, 12, kStr + 0x10}) == 5);
    CHECK(std::string(reinterpret_cast<const char*>(b + kBuf)) == "12-ab");
}

TEST_CASE("cellDaisy against libsre: answers, bytes, signals and lv2 calls", "[hle][lle][daisy]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    const Run theirs = run(*flash, true);
    const Run ours = run(*flash, false);
    CHECK(theirs.log.size() > 3000);
    compare_logs(ours.log, theirs.log);
    compare_logs(ours.threads, theirs.threads);
    compare_logs(ours.calls, theirs.calls);
    compare_logs(ours.other_calls, theirs.other_calls);
    CHECK(std::count_if(theirs.calls.begin(), theirs.calls.end(),
              [](const std::string& s) { return s.starts_with("spu_connect_event"); }) >= 4);
}
