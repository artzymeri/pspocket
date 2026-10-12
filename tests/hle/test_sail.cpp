// cellSail (D-562) against libsail, Sony's own module, from the user's installed system software (firmware_compare.hpp;
// skipped without it): the same script in two fresh processes (libsail loaded in both), one calling libsail's exports
// and one pspocket's. The answers, the memory the script uses (the objects, a stand-in player's message queue, what
// the callbacks were called with) and the lv2 calls must be the same. No game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <array>
#include <format>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kArea = kData + 0x1000, kAreaSize = 0x3000;
constexpr u32 kAlloc = kArea, kCallbacks = kArea + 0x20, kFuture = kArea + 0x40, kFuture2 = kArea + 0x60,
              kResult = kArea + 0x80, kSource = kArea + 0x100, kFuncs = kArea + 0x200, kRa = kArea + 0x300,
              kRv = kArea + 0x400, kAttr = kArea + 0x500, kFormat = kArea + 0x520, kPlayer = kArea + 0x800,
              kRing = kArea + 0xC00, kCode = kArea + 0x1000, kRecords = kArea + 0x2000;

// A callback that records (r3..r7) at kRecords (a count, then 48 bytes each from +16) and returns 0.
void callback_code(u8* b, u32 at)
{
    const u32 words[] = {0x3D600000u | (kRecords >> 16), 0x616B0000u | (kRecords & 0xFFFF), 0x818B0000u, 0x1C0C0030u,
        0x7D2B0214u, 0xF8690010u, 0xF8890018u, 0xF8A90020u, 0xF8C90028u, 0xF8E90030u, 0x7C2004ACu, 0x398C0001u,
        0x918B0000u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, at + 4 * i, words[i]);
    ps_write32(b, at + 0xF0, at);
    ps_write32(b, at + 0xF4, 0);
}

// The lv2 calls of the script's thread in order; each other thread's (the player's control thread, SPURS's) apart.
struct Trace {
    std::mutex m;
    std::thread::id main;
    std::vector<std::string> calls;
    std::map<u32, std::vector<std::string>> others; // by lv2 thread ID (host threads are reused)
};

// libsail's mutex and condition calls with their arguments (the attributes by their words, not their stack address).
u8* g_base = nullptr;
void observe(void* user, u32 n, const u64* a)
{
    std::string s;
    const auto attr = [&](u64 at, u32 words) {
        std::string w;
        for (u32 i = 0; i < words; i++) w += std::format(" {:x}", ps_read32(g_base, static_cast<u32>(at) + 4 * i));
        return w;
    };
    switch (n) {
    case 100: s = std::format("mutex_create({:#x},{})", a[0], attr(a[1], 10)); break;
    case 105: s = std::format("cond_create({:#x}, {:#x},{})", a[0], a[1], attr(a[2], 6)); break;
    case 107:
        // The control thread's 3 s waits for a message: how many there are depends on when the script posts.
        if (a[1] == 3000000) return;
        s = std::format("{}({:#x}, {})", n, a[0], a[1]);
        break;
    case 102: s = std::format("{}({:#x}, {})", n, a[0], a[1]); break;
    case 101: case 104: case 106: case 108: s = std::format("{}({:#x})", n, a[0]); break;
    default: return;
    }
    Trace& tr = *static_cast<Trace*>(user);
    const std::lock_guard lock(tr.m);
    if (std::this_thread::get_id() == tr.main) tr.calls.push_back(s);
    else if (const lv2::PpuThread* th = lv2::current_thread()) tr.others[th->id].push_back(s);
    else tr.others[0].push_back(s);
}

struct Run {
    std::vector<std::string> lines;
    std::vector<u8> mem, heap; // the script's area; what the player's allocator gave out (parts 6 on)
    std::vector<u8> records;   // parts 14 on: the callbacks' records
    std::vector<u8> dump;      // part 12's image
    std::vector<std::string> calls;
};

using Call = std::function<u64(const char*, std::initializer_list<u64>, bool)>;
void part1(u8* b, const Call& call, u32 cb, GuestThread& t);
void part2(u8* b, const Call& call, u32 cb, GuestThread& t);
void part3(u8* b, const Call& call, u32 cb, GuestThread& t);
void part4(u8* b, const Call& call, u32 cb, GuestThread& t);
void part5(u8* b, const Call& call, u32 cb, GuestThread& t);
void part6(u8* b, const Call& call, u32 cb, GuestThread& t);
void part7(u8* b, const Call& call, u32 cb, GuestThread& t);
void part8(u8* b, const Call& call, u32 cb, GuestThread& t);
void part9(u8* b, const Call& call, u32 cb, GuestThread& t);
void part10(u8* b, const Call& call, u32 cb, GuestThread& t);
void part11(u8* b, const Call& call, u32 cb, GuestThread& t);
void part12(u8* b, const Call& call, u32 cb, GuestThread& t);
void part13(u8* b, const Call& call, u32 cb, GuestThread& t);
void part14(u8* b, const Call& call, u32 cb, GuestThread& t);
void part15(u8* b, const Call& call, u32 cb, GuestThread& t);
constexpr u32 kHeap = kData + 0x40000, kHeapPtr = kArea + 0xF00, kDump = kData + 0x100000, kDumpSize = 0x8000;
// Parts 14 on (the playback, D-919): the decoders' memory needs a larger heap of its own, the many callbacks and events
// a records area of their own, and the guest code its own place.
constexpr u32 kBigHeap = 0x02000000, kBigHeapSize = 0x04000000, kRecords2 = kData + 0x80000, kRecords2Size = 0x20000,
              kCode3 = kData + 0x20000;
u32 g_heap = kHeap; // where the part's allocator starts
std::function<u32(const char*)> g_export; // a cellSail export's descriptor on the side running (Sony's or pspocket's)
std::vector<std::pair<u32, u32>> g_mask; // ranges of memory each side fills with its own code addresses
std::vector<u32> g_rings;                // parts 14 on: the players' ring pointers (player + 0x178), sorted at the end

// SAIL_BT: a fault in either side prints the host frames (no debugger needed).
void sail_bt_handler(int sig)
{
    void* frames[64];
    const int n = backtrace(frames, 64);
    std::fprintf(stderr, "SIGNAL %d\n", sig);
    backtrace_symbols_fd(frames, n, 2);
    _exit(99);
}

Run run(const std::string& flash, bool sony, int part)
{
    if (std::getenv("SAIL_BT")) {
        signal(SIGBUS, sail_bt_handler);
        signal(SIGSEGV, sail_bt_handler);
    }
    Fixture f;
    g_heap = kHeap;
    if (part >= 14) {
        REQUIRE(f.hle->main_memory().reserve(kBigHeap, kBigHeapSize));
        REQUIRE(f.mem.commit(kBigHeap, kBigHeapSize));
        g_heap = kBigHeap;
    }
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    // The decoders' threads and callbacks (pspocket's on both sides), as the loader links them for a game.
    if (part >= 14) REQUIRE(f.hle->link_internal_functions({"cellDmux", "cellDmuxPamf", "cellAdec", "libvdec",
        "cellVpost", "cellSpurs", "cellPamf"}));
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sail(f, t, flash, "cellSail");
    u8* b = f.base();
    std::memset(b + kArea, 0, kAreaSize);
    std::memset(b + kRecords2, 0, kRecords2Size);
    callback_code(b, kCode);
    const u32 cb = kCode + 0xF0;
    Run r;
    Trace tr;
    g_base = b;
    tr.main = std::this_thread::get_id();
    f.kernel->set_syscall_observer(&observe, &tr);
    const auto call = [&](const char* name, std::initializer_list<u64> args, bool is_void = false) {
        u64 v;
        const std::string_view n(name);
        const u32 nid = n.starts_with("cellSail_") ? static_cast<u32>(std::stoul(std::string(n.substr(9)), nullptr, 16))
                                                   : recomp::nid_of(name);
        if (std::getenv("SAIL_TRACE")) std::fprintf(stderr, "CALL %s %s\n", sony ? "sony" : "mine", name);
        if (sony) {
            v = call_guest(t.ctx, sail.module->functions.at({"cellSail", nid}), args);
        } else {
            const Function* fn = registry().find("cellSail", nid);
            REQUIRE(fn);
            u32 i = 0;
            for (const u64 a : args) {
                if (i < 8) t.ctx.gpr[3 + i] = a;
                else ps_write64(b, static_cast<u32>(t.ctx.gpr[1]) + 0x70 + 8 * (i - 8), a);
                i++;
            }
            fn->fn(t.ctx, b);
            v = t.ctx.gpr[3];
        }
        r.lines.push_back(is_void ? std::format("{} -> (void)", name) : std::format("{} -> {:#x}", name, v));
        return v;
    };
    g_mask.clear();
    g_rings.clear();
    g_export = [&](const char* name) -> u32 {
        const u32 nid = recomp::nid_of(name);
        return sony ? sail.module->functions.at({"cellSail", nid}) : f.hle->link_function("cellSail", nid, name);
    };
    if (part == 1) part1(b, call, cb, t);
    else if (part == 2) part2(b, call, cb, t);
    else if (part == 3) part3(b, call, cb, t);
    else if (part == 4) part4(b, call, cb, t);
    else if (part == 5) part5(b, call, cb, t);
    else if (part == 6) part6(b, call, cb, t);
    else if (part == 7) part7(b, call, cb, t);
    else if (part == 8) part8(b, call, cb, t);
    else if (part == 9) part9(b, call, cb, t);
    else if (part == 10) part10(b, call, cb, t);
    else if (part == 11) part11(b, call, cb, t);
    else if (part == 12) part12(b, call, cb, t);
    else if (part == 13) part13(b, call, cb, t);
    else if (part == 14) part14(b, call, cb, t);
    else part15(b, call, cb, t);
    f.kernel->set_syscall_observer(nullptr, nullptr);
    if (std::getenv("SAIL_RECORDS") && part >= 14)
        for (u32 i = 0; i < ps_read32(b, kRecords2); i++) {
            const u32 rec = kRecords2 + 16 + 48 * i;
            std::fprintf(stderr, "RECORD %s %u: tag %llu %llx %llx %llx %llx %llx\n", sony ? "sony" : "mine", i,
                (unsigned long long)ps_read64(b, rec + 40), (unsigned long long)ps_read64(b, rec),
                (unsigned long long)ps_read64(b, rec + 8), (unsigned long long)ps_read64(b, rec + 16),
                (unsigned long long)ps_read64(b, rec + 24), (unsigned long long)ps_read64(b, rec + 32));
        }
    if (std::getenv("SAIL_RECORDS"))
        for (u32 i = 0; i < ps_read32(b, kRecords); i++) {
            const u32 rec = kRecords + 16 + 48 * i;
            std::fprintf(stderr, "RECORD %s %u: %llx %llx %llx %llx\n", sony ? "sony" : "mine", i,
                (unsigned long long)ps_read64(b, rec), (unsigned long long)ps_read64(b, rec + 8),
                (unsigned long long)ps_read64(b, rec + 16), (unsigned long long)ps_read64(b, rec + 24));
        }
    for (const auto& [at, n] : g_mask) std::memset(b + at, 0x5A, n);
    if (part >= 14) {
        // Each side's own code where the player, the decoders or the adapters keep it (cellSail's internal functions
        // and tables: pspocket's descriptors, or anything in libsail's segments) masked wherever it is.
        std::vector<u32> own;
        if (!sony) {
            for (const Function& fn : registry().functions())
                if (fn.module == "cellSail")
                    if (const u32 d = f.hle->descriptor_of("cellSail", fn.nid)) own.push_back(d);
            for (const char* v : {"_pspocket_cellSailSoundAdapterRendererFuncs",
                     "_pspocket_cellSailGraphicsAdapterRendererFuncs", "_pspocket_cellSailAuReceiverFuncs"})
                own.push_back(f.hle->link_variable("cellSail", recomp::nid_of(v)));
            std::sort(own.begin(), own.end());
        }
        const auto mask_code = [&](u32 from, u32 to) {
            for (u32 at = from; at + 4 <= to; at += 4) {
                const u32 v = ps_read32(b, at);
                bool code = v != 0 && std::binary_search(own.begin(), own.end(), v);
                if (sony)
                    for (const auto& [seg, size] : sail.module->segments) code = code || (v >= seg && v < seg + size);
                if (code) ps_write32(b, at, 0x5A5A5A5A);
            }
        };
        mask_code(kArea, kArea + kAreaSize);
        mask_code(g_heap, ps_read32(b, kHeapPtr));
        mask_code(kRecords2 + 16, kRecords2 + 16 + 48 * std::min<u32>(ps_read32(b, kRecords2), (kRecords2Size - 16) / 48));
        // The players' message rings in no order (the messages of different threads come as they ran).
        for (const u32 ring_at : g_rings) {
            const u32 ring = ps_read32(b, ring_at), cap = ps_read32(b, ring_at - 8);
            std::vector<std::array<u8, 16>> msgs(cap);
            for (u32 i = 0; i < cap; i++) std::memcpy(msgs[i].data(), b + ring + 16 * i, 16);
            std::sort(msgs.begin(), msgs.end());
            for (u32 i = 0; i < cap; i++) std::memcpy(b + ring + 16 * i, msgs[i].data(), 16);
        }
    }
    r.mem.assign(b + kArea, b + kArea + kAreaSize);
    if (part >= 6) r.heap.assign(b + g_heap, b + ps_read32(b, kHeapPtr));
    if (part >= 14) {
        const u32 n = std::min<u32>(ps_read32(b, kRecords2), (kRecords2Size - 16) / 48);
        r.records.assign(b + kRecords2, b + kRecords2 + 16 + 48 * n);
    }
    if (part == 12) r.dump.assign(b + kDump, b + kDump + kDumpSize);
    r.calls = tr.calls;
    // The other threads' calls, each thread's in order, the threads in no order.
    std::vector<std::string> others;
    for (const auto& [id, calls] : tr.others) {
        // A wait for work and the signal after it (other threads' only) depend on when the work came: left out.
        std::string all = "thread:";
        for (usize i = 0; i < calls.size(); i++) {
            if (calls[i].starts_with("107(")) {
                if (i + 1 < calls.size() && calls[i + 1].starts_with("108(")) i++;
                continue;
            }
            all += " " + calls[i];
        }
        others.push_back(all);
    }
    std::sort(others.begin(), others.end());
    r.calls.insert(r.calls.end(), others.begin(), others.end());
    return r;
}

void part1(u8* b, const Call& call_, u32 cb, GuestThread& t)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };

    // The allocator.
    ps_write32(b, kCallbacks, 0x11110000);
    ps_write32(b, kCallbacks + 4, 0x22220000);
    call("cellSailMemAllocatorInitialize", {0, kCallbacks, 1});
    call("cellSailMemAllocatorInitialize", {kAlloc, 0, 1});
    ps_write32(b, kCallbacks + 4, 0);
    call("cellSailMemAllocatorInitialize", {kAlloc, kCallbacks, 1});
    ps_write32(b, kCallbacks + 4, 0x22220000);
    ps_write32(b, kCallbacks, 0);
    call("cellSailMemAllocatorInitialize", {kAlloc, kCallbacks, 1});
    ps_write32(b, kCallbacks, 0x11110000);
    call("cellSailMemAllocatorInitialize", {kAlloc, kCallbacks, 0x1234});

    // The future.
    call("cellSailFutureInitialize", {kFuture});
    call("cellSailFutureIsDone", {kFuture, kResult});
    call("cellSailFutureReset", {kFuture, 0});
    call("cellSailFutureSet", {kFuture, 7});
    call("cellSailFutureSet", {kFuture, 8});
    call("cellSailFutureIsDone", {kFuture, kResult});
    call("cellSailFutureIsDone", {kFuture, 0});
    call("cellSailFutureGet", {kFuture, 0, kResult + 4});
    call("cellSailFutureGet", {kFuture, 0, 0});
    call("cellSailFutureReset", {kFuture, 0});
    call("cellSailFutureIsDone", {kFuture, kResult + 8});
    call("cellSailFutureInitialize", {kFuture2});
    call("cellSailFutureGet", {kFuture2, 1000, kResult + 12}); // times out
    call("cellSailFutureReset", {kFuture2, 1});
    call("cellSailFutureFinalize", {kFuture2});
    call("cellSailFutureFinalize", {kFuture});
    call("cellSailFutureFinalize", {kFuture});

    // A stand-in player: its message queue (three messages; drained by the script between notifications).
    {
        // sys_mutex_attribute_t, sys_cond_attribute_t (priority, not recursive, not shared, not adaptive)
        const u32 m = kAttr + 0x100, c = kAttr + 0x140;
        ps_write32(b, m, 2);
        ps_write32(b, m + 4, 0x20);
        ps_write32(b, m + 8, 0x200);
        ps_write32(b, m + 12, 0x2000);
        ps_write32(b, c, 0x200);
        REQUIRE(lv2::sys_mutex_create(*t.t, kPlayer + 0x170 + 0x14, m) == lv2::CELL_OK);
        REQUIRE(lv2::sys_cond_create(*t.t, kPlayer + 0x170 + 0x18, ps_read32(b, kPlayer + 0x170 + 0x14), c) ==
            lv2::CELL_OK);
    }
    ps_write32(b, kPlayer + 0x170, 3);
    ps_write32(b, kPlayer + 0x170 + 8, kRing);
    const auto drain = [&] { ps_write32(b, kPlayer + 0x170 + 4, 0); };

    // The source.
    for (u32 i = 0; i < 14; i++) ps_write32(b, kFuncs + 4 * i, 0x30000000 + i);
    call("cellSailSourceInitialize", {kSource, 0, 1});
    std::memset(b + kSource, 0xEE, 0x80);
    call("cellSailSourceInitialize", {kSource, kFuncs, 0x55});
    ps_write32(b, kSource, kPlayer);
    call("cellSailSourceNotifyInputEos", {kSource}, true);
    call("cellSailSourceNotifyMediaStateChanged", {kSource, 3}, true);
    call("cellSailSourceNotifySessionError", {kSource, 0x80610703, 1}, true);
    drain();
    call("cellSailSourceNotifySessionError", {kSource, 0xFFFFFFFB, 0}, true);
    call("cellSailSourceNotifyStreamOut", {kSource, 1}, true);
    call("cellSailSourceNotifyCallCompleted", {kSource, 9}, true);
    drain();
    ps_write64(b, kSource + 0x60, 0x123456789ABCull);
    call("cellSailSourceNotifyCallCompleted", {kSource, 0xFFFFFFFD}, true);
    call("cellSailSourceSetDiagHandler", {kSource, cb, 0x77});
    call("cellSailSourceNotifyInputEos", {kSource}, true);
    call("cellSailSourceNotifyMediaStateChanged", {kSource, 4}, true);
    call("cellSailSourceNotifySessionError", {kSource, 0x80610709, 1}, true);
    call("cellSailSourceNotifyStreamOut", {kSource, 1}, true);
    call("cellSailSourceNotifyCallCompleted", {kSource, 0xFFFFFFFE}, true);
    call("cellSailSourceSetDiagHandler", {kSource, 0, 0});
    call("cellSailSourceFinalize", {kSource});
    drain();

    // The renderers.
    for (const char* kind : {"Audio", "Video"}) {
        const u32 obj = kind[0] == 'A' ? kRa : kRv;
        const std::string p = std::string("cellSailRenderer") + kind;
        std::memset(b + obj, 0xEE, 0x100);
        call((p + "Initialize").c_str(), {obj, kFuncs, 0x66, 0});
        ps_write32(b, kAttr, 7);
        call((p + "Initialize").c_str(), {obj, kFuncs, 0x67, kAttr});
        ps_write32(b, kAttr, 8);
        ps_write32(b, kAttr + 4, 0);
        call((p + "Initialize").c_str(), {obj, kFuncs, 0x68, kAttr});
        for (u32 i = 0; i < 0x20; i++) b[kFormat + i] = static_cast<u8>(0x40 + i);
        ps_write32(b, kAttr + 4, kFormat);
        call((p + "Initialize").c_str(), {obj, kFuncs, 0x69, kAttr});
        ps_write32(b, obj + 0x14, kPlayer);
        call((p + "NotifyCallCompleted").c_str(), {obj, 0xFFFFFFFE}, true);
        call((p + "NotifyFrameDone").c_str(), {obj}, true);
        call((p + "NotifyOutputEos").c_str(), {obj}, true);
        drain();
        call(kind[0] == 'A' ? "cellSail_C00A450C" : "cellSail_3E9B9F80", {obj, 0x123456789ull}, true);
        call((p + "Finalize").c_str(), {obj});
        drain();
    }

    // The exports that answer -1.
    for (const char* n : {"cellSail_D5E100E7", "cellSail_7224C0F1", "cellSail_4B5A94A5", "cellSail_197EE602",
             "cellSail_86DE3AA7", "cellSail_5367B57E", "cellSail_9279F3D9", "cellSail_EC695E87", "cellSail_3BE9899B",
             "cellSail_A56F7CC5", "cellSail_5FAC53DD", "cellSail_2692816B", "cellSail_CF03D3BA", "cellSail_D7D96074",
             "cellSail_320887CC", "cellSail_0CCD506A"})
        call(n, {1, 2, 3});

}

// A bump allocator for the player (alloc(arg, boundary, size) from kHeap upward; free does nothing) and a second event
// handler, both guest code.
constexpr u32 kAllocCode = kArea + 0x1100, kFreeCode = kArea + 0x1180;
void allocator_code(u8* b)
{
    // lis/ori r11 = &heap pointer; lwz r3; addi r0,r4,-1; add r3,r3,r0; andc r3,r3,r0; add r12,r3,r5; stw r12; blr
    const u32 alloc[] = {0x3D600000u | (kHeapPtr >> 16), 0x616B0000u | (kHeapPtr & 0xFFFF), 0x806B0000u, 0x3804FFFFu,
        0x7C630214u, 0x7C630078u, 0x7D832A14u, 0x918B0000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(alloc); i++) ps_write32(b, kAllocCode + 4 * i, alloc[i]);
    ps_write32(b, kAllocCode + 0x70, kAllocCode);
    ps_write32(b, kFreeCode, 0x38600000u);
    ps_write32(b, kFreeCode + 4, 0x4E800020u);
    ps_write32(b, kFreeCode + 0x70, kFreeCode);
    ps_write32(b, kHeapPtr, kHeap);
}

void part2(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kAttr2 = kArea + 0x1340, kRes = kArea + 0x1380,
                  kNames = kArea + 0x13A0, kStr = kArea + 0x1800, kOld = kArea + 0x13C0, kInfo = kArea + 0x1900;
    const u32 kRa2 = kRa + 0x80, kRv2 = kRv + 0x80;
    ps_write32(b, kMem, kAllocCode + 0x70); // CellSailMemAllocator: alloc, free, arg
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    u32 cur[5] = {};
    const auto attr = [&](u32 mode, u32 a, u32 v, u32 u, u32 m) {
        cur[0] = mode;
        cur[1] = a;
        cur[2] = v;
        cur[3] = u;
        cur[4] = m;
        for (u32 i = 0; i < 8; i++) ps_write32(b, kAttr2 + 4 * i, 0);
        ps_write32(b, kAttr2, mode);
        ps_write32(b, kAttr2 + 4, a);
        ps_write32(b, kAttr2 + 8, v);
        ps_write32(b, kAttr2 + 12, u);
        ps_write32(b, kAttr2 + 16, m);
    };
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); return at; };
    // The built-in source's function words hold each side's own code addresses.
    const auto mask_source = [&] {
        const u32 src = ps_read32(b, kP + 0x3B0);
        if (src < kArea || src >= kArea + 0x3000) return;
        g_mask.push_back({src + 0x48, 0x38});
        g_mask.push_back({src + 0x100, 0x38});
    };

    // Refusals.
    attr(0, 1, 1, 1, 0);
    call("cellSailPlayerInitialize", {0, cur[0], kMem, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, 0, 1, cur[1], cur[2], cur[3], cur[4]});
    call("cellSailPlayerInitialize", {kP, cur[0], 0, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    attr(8, 1, 1, 1, 0);
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    attr(0, 16, 1, 1, 0);
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    attr(0, 1, 4, 1, 0);
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    call("cellSailPlayerFinalize", {kP});

    // A player of one stream of each kind.
    attr(0, 1, 1, 1, 0);
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, 0x11, cur[1], cur[2], cur[3], cur[4]});
    mask_source();
    std::vector<u8> after_init(b + kP, b + kP + 0x400);
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerGetCurrentDescriptor", {kP});
    call("cellSailPlayerIsPaused", {kP});
    call("cellSailPlayerGetRepeatMode", {kP, kInfo});
    call("cellSailPlayerSubscribeEvent", {kP, 3, 0});
    call("cellSailPlayerSubscribeEvent", {kP, 12, 0});
    call("cellSailPlayerSubscribeEvent", {kP, 2, 5});
    call("cellSailPlayerUnsubscribeEvent", {kP, 0});
    call("cellSailPlayerUnsubscribeEvent", {kP, 13});
    call("cellSailPlayerReplaceEventHandler", {kP, 0, 0, kOld, kOld + 4});
    call("cellSailPlayerReplaceEventHandler", {kP, cb + 0x100, 0x99, kOld, kOld + 4});
    call("cellSailPlayerReplaceEventHandler", {kP, cb, 0x11, 0, 0});
    // Sources.
    call("cellSailPlayerRegisterSource", {kP, str(kStr, "x-test"), kSource});
    call("cellSailPlayerRegisterSource", {kP, kStr, kSource});
    call("cellSailPlayerRegisterSource", {kP, 0, kSource});
    call("cellSailPlayerRegisterSource", {kP, kStr, 0});
    call("cellSailPlayerRegisterSource", {kP, str(kStr + 0x40, "x-two-a-very-long-protocol-name-indeed"), kSource});
    call("cellSailPlayerRegisterSource", {kP, str(kStr + 0x80, "x-three"), kSource});
    call("cellSailPlayerRegisterSource", {kP, str(kStr + 0xC0, "x-four"), kSource});
    for (u32 i = 0; i < 4; i++) ps_write32(b, kNames + 4 * i, kStr + 0x100 + 0x40 * i);
    ps_write32(b, kNames + 0x10, 3);
    call("cellSailPlayerGetRegisteredProtocols", {kP, kNames, kNames + 0x10});
    ps_write32(b, kNames + 0x10, 9);
    call("cellSailPlayerGetRegisteredProtocols", {kP, kNames, kNames + 0x10});
    call("cellSailPlayerUnregisterSource", {kP, kStr, kOld + 8});
    call("cellSailPlayerUnregisterSource", {kP, str(kStr + 0x200, "x-none"), kOld + 8});
    call("cellSailPlayerUnregisterSource", {kP, 0, kOld + 8});
    // The per-stream objects.
    std::memset(b + kRa, 0, 0x200);
    std::memset(b + kRv, 0, 0x200);
    call("cellSailPlayerSetRendererAudio", {kP, 1, kRa});
    call("cellSailPlayerSetRendererAudio", {kP, 0, 0});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRa});
    call("cellSailPlayerSetSoundAdapter", {kP, 0, kRa2});
    call("cellSailPlayerSetRendererVideo", {kP, 1, kRv});
    call("cellSailPlayerSetRendererVideo", {kP, 0, 0});
    call("cellSailPlayerSetGraphicsAdapter", {kP, 0, kRv});
    call("cellSailPlayerSetRendererVideo", {kP, 0, kRv2});
    call("cellSailPlayerSetAuReceiver", {kP, 1, kRa + 0x100});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kRa + 0x100});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kRa + 0x140});
    ps_write32(b, kRa + 0x34, 0x101);
    ps_write32(b, kRv + 0x34, 0x100);
    call("cellSailPlayerIsEsAudioMuted", {kP, 0});
    call("cellSailPlayerIsEsAudioMuted", {kP, 1});
    call("cellSailPlayerIsEsVideoMuted", {kP, 0});
    call("cellSailPlayerIsEsVideoMuted", {kP, 3});
    call("cellSailPlayerFinalize", {kP});
    call("cellSailPlayerFinalize", {kP});
    call("cellSailPlayerRegisterSource", {kP, kStr, kSource});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRa});
    std::memcpy(b + kInfo + 0x100, after_init.data(), 0x400); // the player as Initialize left it

    // Initialize2's checks, then every mode (its clock's latencies), with the messages given.
    ps_write32(b, kRes, 0x00ABCDEF);
    for (u32 i = 1; i < 4; i++) ps_write32(b, kRes + 4 * i, 0);
    attr(1, 2, 0, 0, 10);
    ps_write32(b, kAttr2 + 0x14, 1);
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 1, kAttr2, kRes});
    ps_write32(b, kAttr2 + 0x14, 0);
    ps_write32(b, kRes + 8, 1);
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 1, kAttr2, kRes});
    ps_write32(b, kRes + 8, 0);
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 1, kAttr2, 0});
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 1, kAttr2, kRes});
    mask_source();
    std::memcpy(b + kInfo + 0x500, b + kP, 0x400);
    call("cellSailPlayerFinalize", {kP});
    for (u32 mode = 0; mode < 8; mode++) {
        attr(mode, 1, 2, 0, 0);
        call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, mode, cur[1], cur[2], cur[3], cur[4]});
        mask_source();
        std::memcpy(b + kInfo + 0x900, b + kP + 8, 200); // the clock
        call("cellSailPlayerFinalize", {kP});
    }
}

// Descriptors the script lays out itself (the player makes them; their fields are what the calls read): a stand-in
// player for the stream counts, a source whose GetCapabilities callback answers the u64 at kCaps (and records the
// stream type it was asked for after it).
void part3(u8* b, const Call& call_, u32, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    constexpr u32 kD = kArea + 0x1400, kSrc = kArea + 0x1600, kCaps = kArea + 0x1700, kGetCaps = kArea + 0x1800,
                  kOut = kArea + 0x1900, kQ = kArea + 0x1A00, kEs = kArea + 0x1B00;
    // lis/ori r11 = kCaps; ld r0,0(r11); std r0,0(r7); std r4,8(r11); li r3,0; blr
    const u32 code[] = {0x3D600000u | (kCaps >> 16), 0x616B0000u | (kCaps & 0xFFFF), 0xE80B0000u, 0xF8070000u,
        0xF88B0008u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(code); i++) ps_write32(b, kGetCaps + 4 * i, code[i]);
    ps_write32(b, kGetCaps + 0x70, kGetCaps);
    ps_write32(b, kSrc + 4, 0x5151);
    ps_write32(b, kSrc + 8 + 12 * 4, kGetCaps + 0x70);
    ps_write32(b, kPlayer + 0x378, 2);
    ps_write32(b, kPlayer + 0x37C, 1);
    ps_write32(b, kPlayer + 0x380, 3);
    const auto desc = [&](u32 type, u32 flags, u8 state, u64 size) {
        std::memset(b + kD, 0, 0x100);
        ps_write32(b, kD, type);
        ps_write32(b, kD + 0x10, kArea + 0x1F00); // the URI
        ps_write32(b, kD + 0x14, 0x7777);
        ps_write32(b, kD + 0x18, flags);
        ps_write32(b, kD + 0x1C, kPlayer);
        ps_write32(b, kD + 0x24, kSrc);
        ps_write32(b, kD + 0x28, kEs);
        ps_write32(b, kD + 0x2C, kEs + 0x200);
        ps_write32(b, kD + 0x30, kEs + 0x300);
        b[kD + 0x38] = state;
        ps_write64(b, kD + 0xD0, size);
    };
    desc(0, 0, 0, 0x100000);
    call("cellSailDescriptorGetStreamType", {kD});
    call("cellSailDescriptorGetUri", {kD});
    call("cellSailDescriptorGetMediaInfo", {kD});
    for (const auto& a : std::vector<std::array<u64, 3>>{{1, 0, 0}, {1, 8, 0}, {1, 8, 1}, {2, 0, 0}, {1, 4, 0}, {1, 0, 0}})
        call("cellSailDescriptorSetParameter", {kD, a[0], a[1], a[2]});
    call("cellSailDescriptorSetAutoSelection", {kD, 0});
    call("cellSailDescriptorIsAutoSelection", {kD});
    call("cellSailDescriptorSetAutoSelection", {kD, 1});
    call("cellSailDescriptorIsAutoSelection", {kD});
    b[kD + 0x38] = 2;
    call("cellSailDescriptorSetParameter", {kD, 1, 8, 0});
    call("cellSailDescriptorSetAutoSelection", {kD, 0});
    // Elementary streams cleared.
    for (u8 state : {0, 1, 2}) {
        b[kD + 0x38] = state;
        for (u32 kind = 0; kind < 4; kind++) {
            for (u32 index = 0; index < 4; index++) {
                b[kEs + 0x90 * index] = 1;
                b[kEs + 0x200 + 0x78 * index] = static_cast<u8>(index & 1);
                b[kEs + 0x300 + 0x4C * index] = 1;
                call("cellSailDescriptorClearEs", {kD, kind, index});
            }
        }
    }
    for (u32 type = 0; type < 4; type++) {
        ps_write32(b, kD, type);
        call("cellSailDescriptorDestroyDatabase", {kD});
    }
    // Capabilities.
    for (u32 type : {0u, 1u, 2u, 5u}) {
        for (u32 flags : {0u, 1u}) {
            for (u64 caps : {0x0ull, 0x1ull, 0x11ull, 0x41ull, 0x80ull, 0xD1ull, 0xFFull}) {
                desc(type, flags, 1, 0x100000);
                ps_write64(b, kCaps, caps);
                std::memset(b + kOut, 0xEE, 0x10);
                call("cellSailDescriptorGetCapabilities", {kD, kOut, kOut + 4, kOut + 8, kOut + 12});
            }
        }
    }
    ps_write32(b, kD + 0x10, 0);
    call("cellSailDescriptorGetCapabilities", {kD, kOut, kOut + 4, kOut + 8, kOut + 12});
    // InquireCapability over a spread of inquiries (a fixed pseudo-random walk).
    u32 seed = 12345;
    const auto rnd = [&](u32 n) {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) % n;
    };
    static const u32 kStarts[] = {0, 1, 2, 4, 0x10, 0x20, 0x40, 7};
    static const u32 kSeeks[] = {0, 1, 0x10, 0x40, 5};
    static const u32 kTerms[] = {0, 1, 2, 0x20, 9};
    static const u32 kHows[] = {0, 1, 2, 8, 0x10, 0x20, 0x40};
    static const s32 kCounts[] = {0, 1, -1, 2};
    static const u64 kPos[] = {0, 0x100, 0x80000, 0x100000, 0x200000};
    static const u64 kCapsSet[] = {0x00, 0x11, 0x51, 0xD1, 0xFF, 0x10, 0x01};
    for (u32 i = 0; i < 700; i++) {
        const u32 type = rnd(4) == 3 ? 3 : rnd(3);
        desc(type, rnd(2), static_cast<u8>(rnd(6)), 0x100000);
        ps_write64(b, kCaps, kCapsSet[rnd(7)]);
        std::memset(b + kQ, 0, 0x30);
        ps_write32(b, kQ, kStarts[rnd(8)]);
        ps_write32(b, kQ + 4, kSeeks[rnd(5)]);
        ps_write32(b, kQ + 8, kTerms[rnd(5)]);
        ps_write32(b, kQ + 12, kHows[rnd(7)]);
        ps_write32(b, kQ + 16, static_cast<u32>(kCounts[rnd(4)]));
        if (rnd(10) == 0) ps_write32(b, kQ + 20, 1);
        ps_write64(b, kQ + 0x18, kPos[rnd(5)]);
        ps_write64(b, kQ + 0x20, kPos[rnd(5)]);
        call("cellSailDescriptorInquireCapability", {kD, rnd(20) == 0 ? 0u : kQ});
    }
}

// The adapters on a player of each clock kind (mode 0: latencies set; mode 7: none, so the clock measures them): their
// set-up, formats, PTS to time and the AV-sync reports a game sends.
void part4(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kSa = kArea + 0x1800, kGa = kArea + 0x1900,
                  kFuncs2 = kArea + 0x1A00, kFmt = kArea + 0x1A40, kOut = kArea + 0x1A80;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    for (u32 i = 0; i < 5; i++) ps_write32(b, kFuncs2 + 4 * i, 0x40000000 + i);
    for (u32 i = 0; i < 0x20; i++) b[kFmt + i] = static_cast<u8>(0x60 + i);
    // Each side's renderer callback table lives in its own data: its address is not compared.
    g_mask.push_back({kSa, 4});
    g_mask.push_back({kGa, 4});
    for (u32 mode : {0u, 7u}) {
        call("cellSailPlayerInitialize", {kP, mode, kMem, cb, 1, 1, 1, 0, 0});
        const u32 src = ps_read32(b, kP + 0x3B0);
        if (src >= kArea && src < kArea + 0x3000) {
            g_mask.push_back({src + 0x48, 0x38});
            g_mask.push_back({src + 0x100, 0x38});
        }
        std::memset(b + kSa, 0xEE, 0x100);
        std::memset(b + kGa, 0xEE, 0x100);
        call("cellSailSoundAdapterInitialize", {kSa, kFuncs2, 0x55});
        call("cellSailGraphicsAdapterInitialize", {kGa, kFuncs2, 0x66});
        call("cellSailPlayerSetSoundAdapter", {kP, 0, kSa});
        call("cellSailPlayerSetGraphicsAdapter", {kP, 0, kGa});
        for (u32 i = 0; i < 0x20; i++) {
            b[kSa + 0x90 + (i % 0x18)] = static_cast<u8>(0x10 + i);
            b[kGa + 0xA8 + i] = static_cast<u8>(0x30 + i);
        }
        call("cellSailSoundAdapterGetFormat", {kSa, kOut});
        call("cellSailGraphicsAdapterGetFormat", {kGa, kOut + 0x20});
        call("cellSailSoundAdapterSetPreferredFormat", {kSa, 0});
        call("cellSailSoundAdapterSetPreferredFormat", {kSa, kFmt});
        call("cellSailGraphicsAdapterSetPreferredFormat", {kGa, 0});
        call("cellSailGraphicsAdapterSetPreferredFormat", {kGa, kFmt});
        call("cellSailSoundAdapterPtsToTimePosition", {kSa, 91000, kOut + 0x40});
        call("cellSailGraphicsAdapterPtsToTimePosition", {kGa, 91000, kOut + 0x40});
        ps_write64(b, kSa + 0xD0, 1000);
        ps_write64(b, kGa + 0x98, 5000);
        for (const s64 pts : {91000ll, 1000ll, 0ll, -9000ll, 0x7FFFFFFFFFFFll}) {
            call("cellSailSoundAdapterPtsToTimePosition", {kSa, static_cast<u64>(pts), kOut + 0x48});
            call("cellSailGraphicsAdapterPtsToTimePosition", {kGa, static_cast<u64>(pts), kOut + 0x50});
        }
        // AV sync: (frame, time) reports, steady then jumping, the records' frame marks set between.
        struct Report {
            u32 frame;
            u64 time;
            u32 mark;
        };
        const Report audio[] = {{0, 5, 0}, {1, 1000, 0}, {5, 50000, 0}, {10, 213000, 3}, {12, 255000, 0}, {20, 426000, 30},
            {21, 900000, 0}, {30, 1000000, 5}, {31, 1021333, 7}, {40, 1213330, 0}};
        for (const Report& r : audio) {
            ps_write32(b, kSa + 0xC0, r.mark);
            call("cellSailSoundAdapterUpdateAvSync", {kSa, r.frame, r.time}, true);
        }
        const Report video[] = {{0, 5, 0}, {1, 1000, 0}, {5, 66000, 0}, {9, 134000, 3}, {10, 150000, 0}, {20, 317000, 30},
            {21, 1317000, 0}, {30, 1467000, 5}, {31, 1467100, 7}, {60, 1967000, 0}};
        for (const Report& r : video) {
            ps_write32(b, kGa + 0xDC, r.mark);
            call("cellSailGraphicsAdapterUpdateAvSync", {kGa, r.frame, r.time}, true);
        }
        std::memcpy(b + kOut + 0x100, b + kP + 8, 200); // the clock after
        // An AU receiver: refused with any callbacks, made without; its callback copy is each side's own code.
        constexpr u32 kAu = kArea + 0x2400;
        std::memset(b + kAu, 0xEE, 0x200);
        call("cellSailAuReceiverInitialize", {kAu, 1, 0, 0x77});
        call("cellSailAuReceiverInitialize", {kAu, 0, 1, 0x77});
        call("cellSailAuReceiverInitialize", {kAu, 0, 0, 0x77});
        std::memcpy(b + kOut + 0x200, b + kAu, 0x20);
        std::memcpy(b + kOut + 0x220, b + kAu + 0x44, 0x1BC);
        call("cellSailAuReceiverFinalize", {kAu});
        call("cellSailSoundAdapterFinalize", {kSa});
        call("cellSailGraphicsAdapterFinalize", {kGa});
        call("cellSailPlayerFinalize", {kP});
    }
}

// Every parameter set and read on a player of two audio and two video streams: each value, the per-stream ones by
// index (in range and not), video parameter 0's structure, the file calls (parameter 33).
void part5(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kOut = kArea + 0x1900, kS = kArea + 0x1A00,
                  kCalls = kArea + 0x1A80;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 1, 2, 2, 0, 0});
    const u32 src = ps_read32(b, kP + 0x3B0);
    if (src >= kArea && src < kArea + 0x3000) {
        g_mask.push_back({src + 0x48, 0x38});
        g_mask.push_back({src + 0x100, 0x38});
    }
    const auto get_all = [&] {
        for (u32 param = 0; param < 38; param++) {
            for (u64 index : {0ull, 1ull, 2ull}) {
                ps_write64(b, kOut, index);
                ps_write64(b, kOut + 8, 0xEEEEEEEEEEEEEEEEull);
                call("cellSailPlayerGetParameter", {kP, param, kOut, kOut + 8});
                call("cellSailPlayerGetParameter", {kP, param, kOut, 0});
            }
            call("cellSailPlayerGetParameter", {kP, param, 0, kOut + 8});
        }
        ps_write64(b, kOut + 8, 0x28);
        call("cellSailPlayerGetParameter", {kP, 33, kOut + 0x40, kOut + 8});
        ps_write64(b, kOut + 8, 0x20);
        call("cellSailPlayerGetParameter", {kP, 33, kOut + 0x40, kOut + 8});
    };
    get_all();
    // Values for every parameter, player-wide in `a`, per stream (index 1, then 5) in `b`.
    static const u64 kValues[] = {0, 1, 2, 3, 0xFFFFFFFF, 0x123456789ull};
    for (u32 param = 0; param < 37; param++) {
        if (param == 0 || param == 33) continue;
        for (const u64 v : kValues) {
            call("cellSailPlayerSetParameter", {kP, param, v, v});
            call("cellSailPlayerSetParameter", {kP, param, 1, v});
            call("cellSailPlayerSetParameter", {kP, param, 5, v});
        }
    }
    // Video parameter 0: off, then a structure (its two fields in and out of range).
    call("cellSailPlayerSetParameter", {kP, 0, 1, 0});
    for (const auto& f : std::vector<std::array<u32, 2>>{{0, 0}, {3, 2}, {4, 0}, {0, 3}, {1, 1}}) {
        ps_write32(b, kS + 4, f[0]);
        ps_write32(b, kS + 8, f[1]);
        call("cellSailPlayerSetParameter", {kP, 0, 1, kS});
    }
    // The file calls: refused shapes, the game's, back to libsail's.
    for (u32 i = 0; i < 10; i++) ps_write32(b, kCalls + 4 * i, i == 0 ? 0 : 0x50000000 + i);
    call("cellSailPlayerSetParameter", {kP, 33, kCalls, 0x20});
    call("cellSailPlayerSetParameter", {kP, 33, 0, 0x28});
    ps_write32(b, kCalls + 12, 0);
    call("cellSailPlayerSetParameter", {kP, 33, kCalls, 0x28});
    ps_write32(b, kCalls + 12, 0x5000000C);
    call("cellSailPlayerSetParameter", {kP, 33, kCalls, 0x28});
    get_all();
    call("cellSailPlayerSetParameter", {kP, 33, 0, 0});
    get_all();
    call("cellSailPlayerFinalize", {kP});
    call("cellSailPlayerSetParameter", {kP, 2, 5, 0});
    g_mask.push_back({kOut + 0x40, 0x28}); // parameter 33's copy of libsail's own file calls (each side's code)
}

void compare_lines(const char* what, const std::vector<std::string>& mine, const std::vector<std::string>& sony)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(mine.size(), sony.size()); i++) {
        const std::string o = i < mine.size() ? mine[i] : "(none)", s = i < sony.size() ? sony[i] : "(none)";
        if (o == s) continue;
        if (differ++ < 20) UNSCOPED_INFO(std::format("{} {}: pspocket {} | libsail {}", what, i, o, s));
    }
    CHECK(differ == 0);
}

// The built-in source's function words (each side's own code) and the event handler's r7 (past its four arguments).
void mask_player_source(u8* b, u32 p)
{
    const u32 src = ps_read32(b, p + 0x3B0);
    if (!src) return;
    g_mask.push_back({src + 0x48, 0x38});
    g_mask.push_back({src + 0x100, 0x38});
}
void mask_handler_r7(u8* b)
{
    for (u32 i = 0; i < ps_read32(b, kRecords); i++) g_mask.push_back({kRecords + 16 + 48 * i + 32, 8});
}

// Boot and Finalize of a player without streams: the control thread's set-up (SPURS of its own, or the game's), its
// events, its message loop and its end.
void part6(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kAttr2 = kArea + 0x1340, kRes = kArea + 0x1380,
                  kKeep = kArea + 0x1900;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    const auto mask_player = [&](u32 p) {
        g_mask.push_back({p + 0x3C8, 16}); // the control thread's stack: its frame, and the deepest its handler went
    };
    // Waits until the handler has recorded `n` calls (the control thread makes them).
    const auto wait_records = [&](u32 n) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (ps_read32(b, kRecords) < n && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(ps_read32(b, kRecords) == n);
    };

    // A stream without its renderer: Boot refuses.
    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x11, 1, 0, 0, 0});
    mask_player_source(b, kP);
    call("cellSailPlayerBoot", {kP, 0x1234});
    call("cellSailPlayerFinalize", {kP});
    call("cellSailPlayerBoot", {kP, 0x1234});

    // No streams: the player's own SPURS. The events of a state change (3) and a call completed (2) subscribed to.
    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x22, 0, 0, 0, 0});
    mask_player_source(b, kP);
    call("cellSailPlayerSubscribeEvent", {kP, 3, 0});
    call("cellSailPlayerSubscribeEvent", {kP, 2, 0});
    call("cellSailPlayerBoot", {kP, 0x123456789ull});
    wait_records(2);
    call("cellSailPlayerBoot", {kP, 1});
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerFinalize", {kP});
    mask_player(kP);
    std::memcpy(b + kKeep, b + kP, 0x400);
    g_mask.push_back({kKeep + 0x3C8, 16});

    // The game's SPURS (never used without streams), and no events subscribed to.
    ps_write32(b, kRes, 0x00ABCDEF);
    for (u32 i = 1; i < 4; i++) ps_write32(b, kRes + 4 * i, 0);
    for (u32 i = 0; i < 8; i++) ps_write32(b, kAttr2 + 4 * i, 0);
    ps_write32(b, kAttr2, 3);
    ps_write32(b, kAttr2 + 16, 4);
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 0x33, kAttr2, kRes});
    mask_player_source(b, kP);
    call("cellSailPlayerBoot", {kP, 0x55});
    // Nothing to wait for: the state is 2 once the thread has set up.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (b[kP + 0x163] != 2 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(b[kP + 0x163] == 2);
    call("cellSailPlayerFinalize", {kP});
    mask_player(kP);
    mask_handler_r7(b);
}

// Descriptors on a booted player: created, added, removed and destroyed by the control thread; the ones left freed at
// its end.
void part7(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kOut = kArea + 0x1900, kUri = kArea + 0x1A00;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    std::memcpy(b + kUri, "x-cell-fs:/app_home/movie.pam", 30);
    const auto until = [&](const std::function<bool()>& done) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(done());
    };

    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x22, 0, 0, 0, 0});
    mask_player_source(b, kP);
    call("cellSailPlayerDestroyDescriptor", {kP, kOut});
    call("cellSailPlayerDestroyDescriptor", {kP, 0});
    call("cellSailPlayerSubscribeEvent", {kP, 1, 0});
    call("cellSailPlayerBoot", {kP, 0});
    until([&] { return b[kP + 0x163] == 2; });

    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x40, kUri, 0});
    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x40, kUri, kOut});
    call("cellSailPlayerCreateDescriptor", {kP, 1, kUri + 0x44, kUri, kOut + 4});
    call("cellSailPlayerCreateDescriptor", {kP, 2, 0, 0, kOut + 8});
    call("cellSailPlayerCreateDescriptor", {kP, 3, 0, 0, kOut + 12});
    const u32 d0 = ps_read32(b, kOut), d1 = ps_read32(b, kOut + 4), d2 = ps_read32(b, kOut + 8);
    call("cellSailPlayerAddDescriptor", {kP, 0});
    call("cellSailPlayerAddDescriptor", {kP, d0});
    call("cellSailPlayerAddDescriptor", {kP, d0});
    call("cellSailPlayerAddDescriptor", {kP, d1});
    call("cellSailPlayerAddDescriptor", {kP, d2});
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerGetCurrentDescriptor", {kP});
    call("cellSailPlayerRemoveDescriptor", {kP, kOut + 16});
    call("cellSailPlayerRemoveDescriptor", {kP, 0});
    call("cellSailPlayerRemoveDescriptor", {kP, kOut + 20});
    call("cellSailPlayerAddDescriptor", {kP, d2});
    call("cellSailPlayerAddDescriptor", {kP, d0});
    call("cellSailPlayerDestroyDescriptor", {kP, d1});
    until([&] { return b[d1 + 0x38] == 6; });
    call("cellSailPlayerDestroyDescriptor", {kP, kOut + 0x40}); // not the player's: nothing
    call("cellSailPlayerCreateDescriptor", {kP, 0, 0, kUri, kOut + 24});
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerFinalize", {kP});
    g_mask.push_back({kP + 0x3C8, 16});
    mask_handler_r7(b);
    // The commands left in the queue's ring: their reply records and Create's arguments are on the caller's stack.
    const u32 ring = ps_read32(b, kP + 0x178);
    for (u32 i = 0; i < ps_read32(b, kP + 0x170); i++) {
        const u32 e = ring + 16 * i;
        if (ps_read32(b, e) == 0x40) g_mask.push_back({e + 8, 4});
        g_mask.push_back({e + 12, 4});
    }
}

// A PAMF header of two 2 KB units (so libsail reads it twice) with one AVC and one ATRAC3plus stream, after
// test_pamf.cpp's.
constexpr u32 kPamf = kData + 0x30000, kPamfLen = 0x1000, kPamfFile = 0x123000;
void write_sail_pamf(u8* b)
{
    const u32 h = kPamf;
    std::memset(b + h, 0, kPamfLen);
    std::memcpy(b + h, "PAMF0041", 8);
    ps_write32(b, h + 0x08, 2);
    ps_write32(b, h + 0x0C, 0x10);
    const u32 seq = h + 0x50;
    ps_write32(b, seq, 0x94);
    ps_write32(b, seq + 0x08, 0x1000);
    ps_write32(b, seq + 0x0E, 0x100000);
    ps_write32(b, seq + 0x12, 0xFF123456);
    ps_write32(b, seq + 0x1A, 2);
    ps_write8(b, seq + 0x1F, 1);
    const u32 gp = h + 0x70;
    ps_write32(b, gp, 0x74);
    ps_write32(b, gp + 0x06, 0x1000);
    ps_write32(b, gp + 0x0C, 0x100000);
    ps_write8(b, gp + 0x11, 1);
    const u32 group = h + 0x82;
    ps_write32(b, group, 0x62);
    ps_write8(b, group + 5, 2);
    const u32 avc = h + 0x88;
    ps_write8(b, avc + 0, 0x1b);
    ps_write8(b, avc + 4, 0xe0);
    ps_write32(b, avc + 8, 0xE8);
    ps_write32(b, avc + 12, 3);
    const u8 avc_info[26] = {100, 41, 0xC4, 0xff, 0, 16, 0, 9, 0, 80, 0, 45, 0, 0, 0, 0, 0, 0, 0, 8, 0xB0, 1, 1, 1,
        0xE2, 0x30};
    std::memcpy(b + avc + 16, avc_info, sizeof avc_info);
    const u32 at3 = h + 0xB8;
    ps_write8(b, at3 + 0, 0xdc);
    ps_write8(b, at3 + 4, 0xbd);
    ps_write32(b, at3 + 8, 0x10C);
    ps_write32(b, at3 + 12, 1);
    ps_write8(b, at3 + 16 + 2, 2);
    ps_write8(b, at3 + 16 + 3, 1);
    const u32 eps[3][3] = {{(1u << 14) | 5, 0x1000, 0}, {(1u << 14) | 6, 0x5000, 4}, {(2u << 14) | 7, 0x9000, 8}};
    for (u32 i = 0; i < 3; i++) {
        ps_write16(b, h + 0xE8 + 12 * i, static_cast<u16>(eps[i][0]));
        ps_write32(b, h + 0xE8 + 12 * i + 4, eps[i][1]);
        ps_write32(b, h + 0xE8 + 12 * i + 8, eps[i][2]);
    }
    ps_write32(b, h + 0x10C + 4, 0x1000);
}

// A game's source in guest code: ReadSync copies the header (up to the size asked) and logs its last call {count, size,
// buffer, u64 offset, type, URI, arg} at `log`; Cleanup counts at log+0x40 {count, arg}; a second ReadSync fails (-1).
void source_code(u8* b, u32 at, u32 log)
{
    const auto hi = [](u32 v) { return v >> 16; };
    const auto lo = [](u32 v) { return v & 0xFFFF; };
    const u32 read[] = {0x3D600000u | hi(log), 0x616B0000u | lo(log), 0x818B0000u, 0x398C0001u, 0x918B0000u,
        0x912B0004u, 0x910B0008u, 0xF8EB0010u, 0x908B0018u, 0x90CB001Cu, 0x906B0020u, 0x3D600000u | hi(kPamf),
        0x616B0000u | lo(kPamf), 0x39800000u | kPamfLen, 0x7F896040u, 0x409C0008u, 0x7D2C4B78u, 0x7D8903A6u,
        0x396BFFFFu, 0x3908FFFFu, 0x8C0B0001u, 0x9C080001u, 0x4200FFF8u, 0x3C000000u | hi(kPamfFile),
        0x60000000u | lo(kPamfFile), 0xF80A0000u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(read); i++) ps_write32(b, at + 4 * i, read[i]);
    ps_write32(b, at + 0x78, at);
    ps_write32(b, at + 0x7C, 0);
    const u32 cleanup[] = {0x3D600000u | hi(log), 0x616B0000u | lo(log), 0x818B0040u, 0x398C0001u, 0x918B0040u,
        0x906B0044u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(cleanup); i++) ps_write32(b, at + 0x80 + 4 * i, cleanup[i]);
    ps_write32(b, at + 0xB0, at + 0x80);
    ps_write32(b, at + 0xB4, 0);
    ps_write32(b, at + 0xC0, 0x3860FFFFu);
    ps_write32(b, at + 0xC4, 0x4E800020u);
    ps_write32(b, at + 0xC8, at + 0xC0);
    ps_write32(b, at + 0xCC, 0);
}

// Descriptors opened and closed: PAMF through a game's source (its makeup on first use, cleanup on last), the header
// read twice (2 KB, then its size), a source whose ReadSync fails, unknown protocols, closes from the game and from the
// control thread (destroy, the player's end).
void part8(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    write_sail_pamf(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kOut = kArea + 0x1900, kUri = kArea + 0x1A00,
                  kLog = kArea + 0x1B00, kSrcCode = kArea + 0x1C00, kSource2 = kSource + 0x80, kFuncs2 = kFuncs + 0x80;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    source_code(b, kSrcCode, kLog);
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); return at; };
    for (u32 i = 0; i < 14; i++) {
        ps_write32(b, kFuncs + 4 * i, 0);
        ps_write32(b, kFuncs2 + 4 * i, 0);
    }
    ps_write32(b, kFuncs, cb);                       // makeup: recorded
    ps_write32(b, kFuncs + 4, kSrcCode + 0xB0);      // cleanup: counted
    ps_write32(b, kFuncs + 44, kSrcCode + 0x78);     // readSync: the header
    ps_write32(b, kFuncs2, cb);
    ps_write32(b, kFuncs2 + 4, kSrcCode + 0xB0);
    ps_write32(b, kFuncs2 + 44, kSrcCode + 0xC8);    // readSync: fails
    const auto until = [&](const std::function<bool()>& done) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(done());
    };

    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x22, 0, 0, 0, 0});
    mask_player_source(b, kP);
    call("cellSailSourceInitialize", {kSource, kFuncs, 0x77});
    call("cellSailSourceInitialize", {kSource2, kFuncs2, 0x88});
    call("cellSailPlayerRegisterSource", {kP, str(kUri + 0x100, "x-test"), kSource});
    call("cellSailPlayerRegisterSource", {kP, str(kUri + 0x120, "x-bad"), kSource2});
    call("cellSailPlayerBoot", {kP, 0});
    until([&] { return b[kP + 0x163] == 2; });

    const u32 good = str(kUri, "x-test://movie.pam");
    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x80, good, kOut});
    const u32 d0 = ps_read32(b, kOut);
    call("cellSailDescriptorOpen", {d0});
    call("cellSailDescriptorOpen", {d0});
    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x80, good, kOut + 4});
    const u32 d1 = ps_read32(b, kOut + 4);
    call("cellSailDescriptorOpen", {d1});
    call("cellSailDescriptorClose", {d1});
    call("cellSailDescriptorClose", {d1});
    call("cellSailPlayerCreateDescriptor", {kP, 0, 0, str(kUri + 0x40, "x-none://a"), kOut + 8});
    call("cellSailDescriptorOpen", {ps_read32(b, kOut + 8)});
    call("cellSailPlayerCreateDescriptor", {kP, 0, 0, str(kUri + 0x60, "plain"), kOut + 12});
    call("cellSailDescriptorOpen", {ps_read32(b, kOut + 12)});
    call("cellSailPlayerCreateDescriptor", {kP, 0, 0, str(kUri + 0xC0, "x-bad://b"), kOut + 16});
    call("cellSailDescriptorOpen", {ps_read32(b, kOut + 16)});
    call("cellSailPlayerAddDescriptor", {kP, d0});
    call("cellSailDescriptorClose", {d0});
    call("cellSailDescriptorOpen", {d1});
    call("cellSailPlayerDestroyDescriptor", {kP, d1});
    until([&] { return b[d1 + 0x38] == 6; });
    call("cellSailPlayerRemoveDescriptor", {kP, kOut + 20});
    call("cellSailDescriptorClose", {d0});
    call("cellSailDescriptorOpen", {d0});
    call("cellSailPlayerFinalize", {kP});
    g_mask.push_back({kP + 0x3C8, 16});
    mask_handler_r7(b);
    // The sources' makeup records: past its two arguments.
    for (u32 i = 0; i < ps_read32(b, kRecords); i++) {
        const u32 r = kRecords + 16 + 48 * i;
        if (ps_read64(b, r) == 0x77 || ps_read64(b, r) == 0x88) g_mask.push_back({r + 16, 24});
    }
    const u32 ring = ps_read32(b, kP + 0x178);
    for (u32 i = 0; i < ps_read32(b, kP + 0x170); i++) {
        const u32 e = ring + 16 * i;
        if (ps_read32(b, e) == 0x40 || ps_read32(b, e) == 0x3C) g_mask.push_back({e + 8, 4});
        g_mask.push_back({e + 12, 4});
    }
}

// Boot and Finalize of players with streams: the audio and video pipelines (their pairs, lwmutexes and copy threads),
// the renderers' makeup and cleanup (a game's renderer, the adapters, an AU receiver), and a Boot whose video renderer's
// makeup fails.
void part9(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kLog = kArea + 0x1B00, kSrcCode = kArea + 0x1C00,
                  kRend = kArea + 0x2400, kGad = kArea + 0x2500, kAuR = kArea + 0x2600, kSad = kArea + 0x2800,
                  kRend2 = kArea + 0x2900, kAuR2 = kArea + 0x2A00, kRF = kArea + 0x2E00, kGF = kArea + 0x2E40,
                  kRFfail = kArea + 0x2E80;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    source_code(b, kSrcCode, kLog);
    const u32 fail = kSrcCode + 0xC8; // returns -1
    // A renderer's callbacks: makeup, cleanup and close recorded; the others never called here.
    for (u32 i = 0; i < 9; i++) {
        ps_write32(b, kRF + 4 * i, 0);
        ps_write32(b, kRFfail + 4 * i, 0);
    }
    ps_write32(b, kRF, cb);
    ps_write32(b, kRF + 4, cb);
    ps_write32(b, kRF + 12, cb);
    ps_write32(b, kRFfail, fail);
    ps_write32(b, kRFfail + 4, cb);
    for (u32 i = 0; i < 5; i++) ps_write32(b, kGF + 4 * i, i < 2 ? cb : 0); // the adapters' makeup and cleanup
    const auto until = [&](const std::function<bool()>& done) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(done());
    };
    const auto mask_objects = [&] {
        g_mask.push_back({kGad, 4}); // each side's adapter callback table
        g_mask.push_back({kSad, 4});
        g_mask.push_back({kAuR + 0x20, 0x24}); // each side's receiver callbacks
        g_mask.push_back({kAuR2 + 0x20, 0x24});
    };

    // Mode 0: the game's audio renderer, a graphics adapter, an AU receiver.
    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x22, 1, 1, 1, 0});
    mask_player_source(b, kP);
    call("cellSailRendererAudioInitialize", {kRend, kRF, 0x31, 0});
    call("cellSailGraphicsAdapterInitialize", {kGad, kGF, 0x66});
    call("cellSailAuReceiverInitialize", {kAuR, 0, 0, 0x77});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRend});
    call("cellSailPlayerSetGraphicsAdapter", {kP, 0, kGad});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kAuR});
    call("cellSailPlayerBoot", {kP, 0x99});
    until([&] { return b[kP + 0x163] == 2; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the copy threads waiting for work
    call("cellSailPlayerFinalize", {kP});
    g_mask.push_back({kP + 0x3C8, 16});
    std::vector<u8> first(b + kP, b + kP + 0x400);

    // Mode 1: a sound adapter, the game's video renderer whose makeup fails, an AU receiver: Boot fails, all undone.
    call("cellSailPlayerInitialize", {kP, 1, kMem, cb, 0x22, 1, 1, 1, 0});
    mask_player_source(b, kP);
    call("cellSailSoundAdapterInitialize", {kSad, kGF, 0x55});
    call("cellSailRendererVideoInitialize", {kRend2, kRFfail, 0x32, 0});
    call("cellSailAuReceiverInitialize", {kAuR2, 0, 0, 0x78});
    call("cellSailPlayerSetSoundAdapter", {kP, 0, kSad});
    call("cellSailPlayerSetRendererVideo", {kP, 0, kRend2});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kAuR2});
    call("cellSailPlayerBoot", {kP, 0x9A});
    until([&] { return b[kP + 0x163] == 10; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    call("cellSailPlayerFinalize", {kP});
    g_mask.push_back({kP + 0x3C8, 16});
    std::memcpy(b + kArea + 0x600, first.data(), 0x200); // the first player's first half kept for the comparison
    mask_objects();
    // The records: the event handler's r7 (past its four arguments), the renderers' r4..r7 (past their one).
    for (u32 i = 0; i < ps_read32(b, kRecords); i++) {
        const u32 r = kRecords + 16 + 48 * i;
        if (ps_read64(b, r) == 0x22) g_mask.push_back({r + 32, 8});
        else g_mask.push_back({r + 8, 32});
    }
}

// The header's two streams replaced: stream 0 (video: coding type 0x1B AVC or 0x02 MPEG-2, its 26 info bytes) and
// stream 1 (audio: 0xDC ATRAC3plus, 0x80 LPCM or 0x81 AC3, its private stream ID and info bytes).
void set_pamf_streams(u8* b, u8 video_coding, const std::array<u8, 26>& video_info, u8 audio_coding, u8 audio_private,
    const std::array<u8, 6>& audio_info)
{
    const u32 v = kPamf + 0x88, a = kPamf + 0xB8;
    ps_write8(b, v, video_coding);
    std::memset(b + v + 16, 0, 32);
    std::memcpy(b + v + 16, video_info.data(), video_info.size());
    ps_write8(b, a, audio_coding);
    ps_write8(b, a + 5, audio_private);
    std::memset(b + a + 16, 0, 32);
    std::memcpy(b + a + 16, audio_info.data(), audio_info.size());
}

// The elementary streams chosen as descriptors open on a player with an audio, a video and a user stream, over AVC and
// MPEG-2 pictures (progressive and interlaced, 720-wide aspect cases, frame rates libsail refuses) and ATRAC3plus, LPCM
// and AC3 sound (channel counts and sample sizes, refused ones); then SetEs and CreateDatabase on an open descriptor.
void part10(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    write_sail_pamf(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kOut = kArea + 0x1900, kUri = kArea + 0x1A00,
                  kLog = kArea + 0x1B00, kSrcCode = kArea + 0x1C00, kRend = kArea + 0x2400, kRend2 = kArea + 0x2500,
                  kAuR = kArea + 0x2600, kRF = kArea + 0x2E00, kDb = kArea + 0x600;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    source_code(b, kSrcCode, kLog);
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); return at; };
    for (u32 i = 0; i < 14; i++) ps_write32(b, kFuncs + 4 * i, 0);
    ps_write32(b, kFuncs, cb);
    ps_write32(b, kFuncs + 4, kSrcCode + 0xB0);
    ps_write32(b, kFuncs + 44, kSrcCode + 0x78);
    for (u32 i = 0; i < 9; i++) ps_write32(b, kRF + 4 * i, 0);
    ps_write32(b, kRF, cb);
    ps_write32(b, kRF + 4, cb);
    ps_write32(b, kRF + 12, cb);

    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x22, 1, 1, 1, 0});
    mask_player_source(b, kP);
    call("cellSailSourceInitialize", {kSource, kFuncs, 0x77});
    call("cellSailPlayerRegisterSource", {kP, str(kUri + 0x100, "x-test"), kSource});
    call("cellSailRendererAudioInitialize", {kRend, kRF, 0x31, 0});
    call("cellSailRendererVideoInitialize", {kRend2, kRF, 0x32, 0});
    call("cellSailAuReceiverInitialize", {kAuR, 0, 0, 0x78});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRend});
    call("cellSailPlayerSetRendererVideo", {kP, 0, kRend2});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kAuR});
    call("cellSailPlayerBoot", {kP, 0});
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (b[kP + 0x163] != 2 && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(b[kP + 0x163] == 2);

    const u32 uri = str(kUri, "x-test://movie.pam");
    // AVC: profile, level, flags (frame MBs only 0x80, signal 0x40, frame rate + 1), aspect, SAR, 0, w/16, 0, h/16,
    // crops (left, right, top, bottom: u16), then flags with deblocking 0x40 at +24.
    const std::array<u8, 26> avc720 = {100, 41, 0xC4, 0xff, 0, 16, 0, 9, 0, 80, 0, 45, 0, 0, 0, 0, 0, 0, 0, 8, 0xB0, 1,
        1, 1, 0xE2, 0x30};
    const std::array<u8, 26> avc576i = {77, 30, 0x03, 1, 0, 0, 0, 0, 0, 45, 0, 18, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0,
        0x80, 0};
    const std::array<u8, 26> avc480 = {77, 31, 0x87, 1, 0, 0, 0, 0, 0, 45, 0, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x40, 0};
    const std::array<u8, 26> avc_bad = {100, 41, 0x82, 2, 0, 0, 0, 0, 0, 40, 0, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0};
    // MPEG-2: profile and level (0x48 main, 0x44 high), flags (progressive 0x80, frame rate), aspect, SAR, 0, w/16, 0,
    // h/16, display width, display height (u16).
    const std::array<u8, 26> m2v480 = {0x48, 0, 0x84, 1, 0, 0, 0, 0, 0, 45, 0, 30, 0x02, 0xD0, 0x01, 0xE0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0};
    const std::array<u8, 26> m2v720 = {0x44, 0, 0x87, 3, 0, 0, 0, 0, 0, 80, 0, 45, 0x05, 0x00, 0x02, 0xD0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0};
    const std::array<u8, 26> m2v_bad = {0x42, 0, 0x05, 2, 0, 0, 0, 0, 0, 45, 0, 36, 0x02, 0xD0, 0x02, 0x40, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0};
    struct Variant {
        u8 video;
        std::array<u8, 26> vinfo;
        u8 audio, priv;
        std::array<u8, 6> ainfo; // +2 channels, +3 frequency, +4 LPCM sample size << 6
    };
    const Variant variants[] = {
        {0x1b, avc720, 0xdc, 0x00, {0, 0, 2, 1, 0, 0}},     // ATRAC3plus stereo
        {0x1b, avc576i, 0xdc, 0x01, {0, 0, 6, 1, 0, 0}},    // 6 channels
        {0x1b, avc480, 0xdc, 0x00, {0, 0, 8, 1, 0, 0}},     // 8 channels
        {0x02, m2v480, 0x80, 0x40, {0, 0, 2, 1, 0x40, 0}},  // LPCM stereo, 16 bits
        {0x02, m2v720, 0x80, 0x41, {0, 0, 8, 1, 0xC0, 0}},  // LPCM 8 channels, 24 bits
        {0x02, m2v480, 0x80, 0x40, {0, 0, 1, 1, 0x80, 0}},  // LPCM, a sample size refused
        {0x1b, avc720, 0x81, 0x30, {0, 0, 6, 1, 0, 0}},     // AC3 5.1
        {0x1b, avc480, 0x81, 0x30, {0, 0, 1, 1, 0, 0}},     // AC3 mono
        {0x1b, avc_bad, 0x81, 0x30, {0, 0, 3, 1, 0, 0}},    // a frame rate and a channel count refused
        {0x02, m2v_bad, 0xdc, 0x00, {0, 0, 2, 2, 0, 0}},    // the same, and 44.1 kHz
    };
    u32 opened = 0;
    for (u32 i = 0; i < std::size(variants); i++) {
        const Variant& v = variants[i];
        set_pamf_streams(b, v.video, v.vinfo, v.audio, v.priv, v.ainfo);
        call("cellSailPlayerCreateDescriptor", {kP, 0, 0, uri, kOut + 4 * i});
        const u32 d = ps_read32(b, kOut + 4 * i);
        call("cellSailDescriptorOpen", {d});
        if (i == 0) opened = d;
        // Each descriptor's choices kept for the comparison (the descriptors themselves are compared too).
        std::memcpy(b + kArea + 0x700 + 0x20 * i, b + d + 0x100, 4);
        if (std::getenv("SAIL_DIFF")) {
            const u32 a = ps_read32(b, d + 0x28), vv = ps_read32(b, d + 0x2C);
            std::fprintf(stderr, "ES %u audio sel %u type %u codec %u ch %u layout %u frame %u | video sel %u type %u "
                "kind %u w %u h %u il %u aspect %u rate %u\n", i, b[a], b[a + 2], ps_read32(b, a + 0x28), b[a + 0x61],
                ps_read32(b, a + 0x68), ps_read16(b, a + 0x62), b[vv], b[vv + 2], ps_read32(b, vv + 0x24),
                ps_read16(b, vv + 0x3C), ps_read16(b, vv + 0x3E), b[vv + 0x39], b[vv + 0x49], b[vv + 0x3B]);
        }
    }
    // SetEs and CreateDatabase on the first.
    const u32 d = opened;
    call("cellSailDescriptorSetEs", {d, 0, 0, 0, d + 0x40});
    call("cellSailDescriptorSetEs", {d, 0, 0, 1, d + 0x40});
    call("cellSailDescriptorSetEs", {d, 3, 0, 1, d + 0x40});
    call("cellSailDescriptorSetEs", {d, 1, 1, 1, d + 0x40});
    call("cellSailDescriptorCreateDatabase", {d, kDb, 0x7F, 0});
    call("cellSailDescriptorCreateDatabase", {d, kDb, 0x80, 0});
    call("cellSailDescriptorClearEs", {d, 1, 0});
    call("cellSailDescriptorSetEs", {d, 1, 0, 1, kDb});
    call("cellSailDescriptorClearEs", {d, 0, 0});
    call("cellSailDescriptorSetEs", {d, 0, 0, 1, kDb});
    call("cellSailDescriptorClearEs", {d, 2, 0});
    call("cellSailDescriptorSetEs", {d, 2, 0, 1, kDb});
    call("cellSailDescriptorSetEs", {d, 2, 0, 1, kDb});
    call("cellSailDescriptorClose", {d});
    call("cellSailDescriptorSetEs", {d, 0, 0, 1, kDb});
    call("cellSailDescriptorCreateDatabase", {d, kDb, 0x80, 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    call("cellSailPlayerFinalize", {kP});
    g_mask.push_back({kP + 0x3C8, 16});
    g_mask.push_back({kAuR + 0x20, 0x24});
    for (u32 i = 0; i < ps_read32(b, kRecords); i++) {
        const u32 r = kRecords + 16 + 48 * i;
        const u64 arg = ps_read64(b, r);
        if (arg == 0x22) g_mask.push_back({r + 32, 8});
        else if (arg == 0x77) g_mask.push_back({r + 16, 24});
        else g_mask.push_back({r + 8, 32});
    }
    const u32 ring = ps_read32(b, kP + 0x178);
    for (u32 i = 0; i < ps_read32(b, kP + 0x170); i++) {
        const u32 e = ring + 16 * i;
        if (ps_read32(b, e) == 0x40 || ps_read32(b, e) == 0x3C) g_mask.push_back({e + 8, 4});
        g_mask.push_back({e + 12, 4});
    }
}

// Pause (its event), the repeat mode and the streams' mute switches on a booted player with an audio, a video and a
// user stream.
void part11(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kRend = kArea + 0x2400, kGad = kArea + 0x2500,
                  kAuR = kArea + 0x2600, kRF = kArea + 0x2E00, kGF = kArea + 0x2E40, kInfo = kArea + 0x600,
                  kOut = kArea + 0x680;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    for (u32 i = 0; i < 9; i++) ps_write32(b, kRF + 4 * i, 0);
    ps_write32(b, kRF, cb);
    ps_write32(b, kRF + 4, cb);
    ps_write32(b, kRF + 12, cb);
    for (u32 i = 0; i < 5; i++) ps_write32(b, kGF + 4 * i, i < 2 ? cb : 0);
    for (u32 i = 0; i < 0x28; i++) b[kInfo + i] = static_cast<u8>(0x90 + i);

    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x22, 1, 1, 1, 0});
    mask_player_source(b, kP);
    call("cellSailRendererAudioInitialize", {kRend, kRF, 0x31, 0});
    call("cellSailGraphicsAdapterInitialize", {kGad, kGF, 0x66});
    call("cellSailAuReceiverInitialize", {kAuR, 0, 0, 0x78});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRend});
    call("cellSailPlayerSetGraphicsAdapter", {kP, 0, kGad});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kAuR});
    call("cellSailPlayerBoot", {kP, 0});
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (b[kP + 0x163] != 2 && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(b[kP + 0x163] == 2);

    call("cellSailPlayerIsPaused", {kP});
    call("cellSailPlayerSetPaused", {kP, 1});
    call("cellSailPlayerSetPaused", {kP, 0x80000000});
    call("cellSailPlayerIsPaused", {kP});
    call("cellSailPlayerSetPaused", {kP, 0});
    call("cellSailPlayerSetPaused", {kP, 0});
    call("cellSailPlayerSetPaused", {kP, 7});
    call("cellSailPlayerUnsubscribeEvent", {kP, 7});
    call("cellSailPlayerSetPaused", {kP, 0});
    for (u32 mode = 0; mode < 4; mode++) {
        call("cellSailPlayerSetRepeatMode", {kP, mode, mode == 1 ? kInfo : 0});
        call("cellSailPlayerGetRepeatMode", {kP, kOut + 0x30 * mode});
    }
    call("cellSailPlayerSetEsAudioMuted", {kP, 0, 1});
    call("cellSailPlayerIsEsAudioMuted", {kP, 0});
    call("cellSailPlayerSetEsAudioMuted", {kP, 0, 0x100});
    call("cellSailPlayerSetEsAudioMuted", {kP, 1, 1});
    call("cellSailPlayerSetEsVideoMuted", {kP, 0, 1});
    call("cellSailPlayerIsEsVideoMuted", {kP, 0});
    call("cellSailPlayerSetEsVideoMuted", {kP, 0, 2});
    call("cellSailPlayerSetEsVideoMuted", {kP, 5, 1});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    call("cellSailPlayerFinalize", {kP});
    g_mask.push_back({kP + 0x3C8, 16});
    g_mask.push_back({kGad, 4});
    g_mask.push_back({kAuR + 0x20, 0x24});
    for (u32 i = 0; i < ps_read32(b, kRecords); i++) {
        const u32 r = kRecords + 16 + 48 * i;
        if (ps_read64(b, r) == 0x22) g_mask.push_back({r + 32, 8});
        else g_mask.push_back({r + 8, 32});
    }
    const u32 ring = ps_read32(b, kP + 0x178);
    for (u32 i = 0; i < ps_read32(b, kP + 0x170); i++) {
        const u32 e = ring + 16 * i;
        const u32 code = ps_read32(b, e);
        if (code == 0x45 || code == 0x46 || code == 0x47) g_mask.push_back({e + 8, 4});
        g_mask.push_back({e + 12, 4});
    }
}

// DumpImage of a booted player with an audio, a video and a user stream and two descriptors held (one open, with a
// URI), at many sizes and an unaligned buffer: every place the image can stop, and the whole image. Each side's own code
// addresses in the player (the built-in source's callbacks and file calls, the adapter's and the AU receiver's tables)
// and the control thread's stack words are set to the same values on both sides while dumping.
void part12(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    write_sail_pamf(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kOut = kArea + 0x1900, kUri = kArea + 0x1A00,
                  kLog = kArea + 0x1B00, kSrcCode = kArea + 0x1C00, kRend = kArea + 0x2400, kGad = kArea + 0x2500,
                  kAuR = kArea + 0x2600, kRF = kArea + 0x2E00, kGF = kArea + 0x2E40, kArg = kArea + 0x2900;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    source_code(b, kSrcCode, kLog);
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); return at; };
    for (u32 i = 0; i < 14; i++) ps_write32(b, kFuncs + 4 * i, 0);
    ps_write32(b, kFuncs, cb);
    ps_write32(b, kFuncs + 4, kSrcCode + 0xB0);
    ps_write32(b, kFuncs + 44, kSrcCode + 0x78);
    for (u32 i = 0; i < 9; i++) ps_write32(b, kRF + 4 * i, 0);
    ps_write32(b, kRF, cb);
    ps_write32(b, kRF + 4, cb);
    ps_write32(b, kRF + 12, cb);
    for (u32 i = 0; i < 5; i++) ps_write32(b, kGF + 4 * i, i < 2 ? cb : 0);
    for (u32 i = 0; i < 0x100; i++) b[kArg + i] = static_cast<u8>(i * 7);

    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x22, 1, 1, 1, 0});
    call("cellSailSourceInitialize", {kSource, kFuncs, 0x77});
    call("cellSailPlayerRegisterSource", {kP, str(kUri + 0x100, "x-test"), kSource});
    call("cellSailRendererAudioInitialize", {kRend, kRF, kArg, 0});
    call("cellSailGraphicsAdapterInitialize", {kGad, kGF, 0x66});
    call("cellSailAuReceiverInitialize", {kAuR, 0, 0, 0x78});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRend});
    call("cellSailPlayerSetGraphicsAdapter", {kP, 0, kGad});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kAuR});
    call("cellSailPlayerBoot", {kP, 0});
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (b[kP + 0x163] != 2 && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(b[kP + 0x163] == 2);
    call("cellSailPlayerCreateDescriptor", {kP, 0, 0, str(kUri, "x-test://a-movie-with-a-long-name.pam"), kOut});
    call("cellSailDescriptorOpen", {ps_read32(b, kOut)});
    call("cellSailPlayerAddDescriptor", {kP, ps_read32(b, kOut)});
    call("cellSailPlayerCreateDescriptor", {kP, 0, 0, 0, kOut + 4});
    call("cellSailPlayerAddDescriptor", {kP, ps_read32(b, kOut + 4)});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The same values on both sides while dumping, put back after.
    const u32 src = ps_read32(b, kP + 0x2E8), fsb = ps_read32(b, src + 4);
    const std::pair<u32, u32> own[] = {{kP + 0x3C8, 16}, {src + 8, 0x38}, {fsb + 0x100, 0x38}, {kGad, 4},
        {kAuR + 0x20, 0x24}};
    std::vector<std::vector<u8>> saved;
    for (const auto& [at, n] : own) {
        saved.emplace_back(b + at, b + at + n);
        std::memset(b + at, 0x5A, n);
    }
    std::memset(b + kDump, 0xEE, kDumpSize);
    const u64 total = call("cellSailPlayerDumpImage", {kP, kDump + 5, kDumpSize - 5});
    std::vector<u8> image(b + kDump, b + kDump + kDumpSize);
    for (u32 size = 0; size < static_cast<u32>(total) + 0x80; size += size < 0x100 ? 1 : 13)
        call("cellSailPlayerDumpImage", {kP, kDump + 5, size});
    call("cellSailPlayerDumpImage", {kP, kDump, 0x7FFF});
    std::memcpy(b + kDump, image.data(), image.size());
    for (usize i = 0; i < std::size(own); i++) std::memcpy(b + own[i].first, saved[i].data(), own[i].second);

    call("cellSailPlayerFinalize", {kP});
    mask_player_source(b, kP);
    g_mask.push_back({kP + 0x3C8, 16});
    g_mask.push_back({kGad, 4});
    g_mask.push_back({kAuR + 0x20, 0x24});
    for (u32 i = 0; i < ps_read32(b, kRecords); i++) {
        const u32 r = kRecords + 16 + 48 * i;
        const u64 arg = ps_read64(b, r);
        if (arg == 0x22) g_mask.push_back({r + 32, 8});
        else if (arg == 0x77) g_mask.push_back({r + 16, 24});
        else g_mask.push_back({r + 8, 32});
    }
    const u32 ring = ps_read32(b, kP + 0x178);
    for (u32 i = 0; i < ps_read32(b, kP + 0x170); i++) {
        const u32 e = ring + 16 * i;
        if (ps_read32(b, e) == 0x40 || ps_read32(b, e) == 0x3C) g_mask.push_back({e + 8, 4});
        g_mask.push_back({e + 12, 4});
    }
}

// A source callback in guest code that tells the player its call completed: NotifyCallCompleted(its argument (the
// source), the result at table+4) through the export's descriptor at table+0, counting its calls at table+8.
void notify_code(u8* b, u32 at, u32 table)
{
    const u32 code[] = {0x7C0802A6u, 0xF8010010u, 0xF821FF81u, 0xF8410028u, 0x3D600000u | (table >> 16),
        0x616B0000u | (table & 0xFFFF), 0x818B0000u, 0x808B0004u, 0x814B0008u, 0x394A0001u, 0x914B0008u, 0x800C0000u,
        0x804C0004u, 0x7C0903A6u, 0x4E800421u, 0xE8410028u, 0x38210080u, 0xE8010010u, 0x7C0803A6u, 0x38600000u,
        0x4E800020u};
    for (u32 i = 0; i < std::size(code); i++) ps_write32(b, at + 4 * i, code[i]);
    ps_write32(b, at + 0x58, at);
    ps_write32(b, at + 0x5C, 0);
}

// OpenStream and CloseStream of a player without streams through a game's source: refusals, an open and a close (the
// source told to open and close, the dmux pipeline opened and closed, the events), and an open whose source fails
// (everything closed again, the call completed with CANCEL).
void part13(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    write_sail_pamf(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kOut = kArea + 0x1900, kUri = kArea + 0x1A00,
                  kLog = kArea + 0x1B00, kSrcCode = kArea + 0x1C00, kOpenCode = kArea + 0x2400,
                  kCloseCode = kArea + 0x2480, kTables = kArea + 0x2500;
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    source_code(b, kSrcCode, kLog);
    notify_code(b, kOpenCode, kTables);
    notify_code(b, kCloseCode, kTables + 0x10);
    ps_write32(b, kTables, g_export("cellSailSourceNotifyCallCompleted"));
    ps_write32(b, kTables + 0x10, g_export("cellSailSourceNotifyCallCompleted"));
    g_mask.push_back({kTables, 4});
    g_mask.push_back({kTables + 0x10, 4});
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); return at; };
    for (u32 i = 0; i < 14; i++) ps_write32(b, kFuncs + 4 * i, 0);
    ps_write32(b, kFuncs, cb);
    ps_write32(b, kFuncs + 4, kSrcCode + 0xB0);
    ps_write32(b, kFuncs + 8, kOpenCode + 0x58);
    ps_write32(b, kFuncs + 12, kCloseCode + 0x58);
    ps_write32(b, kFuncs + 44, kSrcCode + 0x78);
    const auto until_state = [&](u8 st) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (b[kP + 0x163] != st && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(b[kP + 0x163] == st);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the call's completion event after it
    };

    call("cellSailPlayerInitialize", {kP, 0, kMem, cb, 0x22, 0, 0, 0, 0});
    mask_player_source(b, kP);
    call("cellSailSourceInitialize", {kSource, kFuncs, kSource});
    call("cellSailPlayerRegisterSource", {kP, str(kUri + 0x100, "x-test"), kSource});
    call("cellSailPlayerBoot", {kP, 0});
    until_state(2);
    call("cellSailPlayerOpenStream", {kP, 0x111});
    call("cellSailPlayerCloseStream", {kP, 0x222});

    const u32 uri = str(kUri, "x-test://movie.pam");
    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x80, uri, kOut});
    const u32 d0 = ps_read32(b, kOut);
    call("cellSailDescriptorOpen", {d0});
    call("cellSailPlayerAddDescriptor", {kP, d0});
    // cellDmux keeps the pipeline's callback (each side's own code) at +0x1C of its memory, the first the open takes.
    const auto mask_dmux_callback = [&] { g_mask.push_back({((ps_read32(b, kHeapPtr) + 0x7F) & ~0x7Fu) + 0x1C, 4}); };
    mask_dmux_callback();
    call("cellSailPlayerOpenStream", {kP, 0x123456789ull});
    until_state(4);
    call("cellSailPlayerOpenStream", {kP, 1});
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerCloseStream", {kP, 0x987654321ull});
    until_state(2);
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerGetCurrentDescriptor", {kP});

    // The source fails to open.
    ps_write32(b, kTables + 4, 0x80610A01);
    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x80, uri, kOut + 4});
    const u32 d1 = ps_read32(b, kOut + 4);
    call("cellSailDescriptorOpen", {d1});
    call("cellSailPlayerAddDescriptor", {kP, d1});
    mask_dmux_callback();
    call("cellSailPlayerOpenStream", {kP, 0xBAD});
    until_state(2);
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerFinalize", {kP});

    g_mask.push_back({kP + 0x3C8, 16});
    for (u32 i = 0; i < ps_read32(b, kRecords); i++) {
        const u32 r = kRecords + 16 + 48 * i;
        const u64 arg = ps_read64(b, r);
        if (arg == 0x22) g_mask.push_back({r + 32, 8});
        else g_mask.push_back({r + 16, 24});
    }
    const u32 ring = ps_read32(b, kP + 0x178);
    for (u32 i = 0; i < ps_read32(b, kP + 0x170); i++) {
        const u32 e = ring + 16 * i;
        const u32 c = ps_read32(b, e);
        if (c == 0x40 || c == 0x3C || c == 0x4C || c == 0x5D) g_mask.push_back({e + 8, 4});
        if (c != 0) g_mask.push_back({e + 12, 4});
    }
}


// ------------------------------------------------------------------------------------------------- the playback (D-919)

// A recorder like callback_code (into the parts' own records area) that also keeps `tag` at +40 of its record.
void tagged_code(u8* b, u32 at, u32 tag)
{
    const u32 words[] = {0x3D600000u | (kRecords2 >> 16), 0x616B0000u | (kRecords2 & 0xFFFF), 0x818B0000u, 0x1C0C0030u,
        0x7D2B0214u, 0xF8690010u, 0xF8890018u, 0xF8A90020u, 0xF8C90028u, 0xF8E90030u, 0x38000000u | tag, 0xF8090038u,
        0x7C2004ACu, 0x398C0001u, 0x918B0000u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, at + 4 * i, words[i]);
    ps_write32(b, at + 0x70, at);
    ps_write32(b, at + 0x74, 0);
}
u32 tagged(u32 at) { return at + 0x70; }

// A PAMF header in kPamf for the streams given (coding type, stream ID, private stream ID, info bytes, entry points),
// their entry point tables right after them, as libpamf lays them out; `units` of data after the header.
struct PamfStream {
    u8 coding, id, priv;
    std::vector<u8> info;
    u32 eps;
};
void write_pamf(u8* b, const std::vector<PamfStream>& streams, u32 units = 0x40)
{
    const u32 h = kPamf, n = static_cast<u32>(streams.size());
    std::memset(b + h, 0, kPamfLen);
    std::memcpy(b + h, "PAMF0041", 8);
    ps_write32(b, h + 0x08, 1);
    ps_write32(b, h + 0x0C, units);
    const u32 tables = 0x88 + 0x30 * n;
    const u32 seq = h + 0x50;
    ps_write32(b, seq, tables - 0x54);
    ps_write32(b, seq + 0x08, 0x1000);
    ps_write32(b, seq + 0x0E, 0x100000);
    ps_write32(b, seq + 0x12, 0xFF123456);
    ps_write32(b, seq + 0x1A, n);
    ps_write8(b, seq + 0x1F, 1);
    const u32 gp = h + 0x70;
    ps_write32(b, gp, tables - 0x74);
    ps_write32(b, gp + 0x06, 0x1000);
    ps_write32(b, gp + 0x0C, 0x100000);
    ps_write8(b, gp + 0x11, 1);
    const u32 group = h + 0x82;
    ps_write32(b, group, tables - 0x86);
    ps_write8(b, group + 5, static_cast<u8>(n));
    u32 ep_at = tables;
    for (u32 i = 0; i < n; i++) {
        const PamfStream& st = streams[i];
        const u32 at = h + 0x88 + 0x30 * i;
        ps_write8(b, at + 0, st.coding);
        ps_write8(b, at + 4, st.id);
        ps_write8(b, at + 5, st.priv);
        ps_write32(b, at + 8, st.eps ? ep_at : 0);
        ps_write32(b, at + 12, st.eps);
        std::memcpy(b + at + 16, st.info.data(), std::min<usize>(st.info.size(), 32));
        for (u32 e = 0; e < st.eps; e++) {
            ps_write16(b, h + ep_at, static_cast<u16>((1u << 14) | (5 + e)));
            ps_write32(b, h + ep_at + 4, 0x1000 + 0x4000 * e);
            ps_write32(b, h + ep_at + 8, 4 * e);
            ep_at += 12;
        }
    }
}

// AVC 96x64 (main profile, level 3.0, 29.97 Hz, frame MBs only, deblocking), ATRAC3plus stereo at 48 kHz, LPCM stereo
// 16-bit at 48 kHz, user data.
PamfStream avc96x64() { return {0x1b, 0xe0, 0, {77, 30, 0x84, 1, 0, 0, 0, 0, 0, 6, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0}, 3}; }
PamfStream avc128x64() { return {0x1b, 0xe1, 0, {77, 30, 0x84, 1, 0, 0, 0, 0, 0, 8, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0}, 1}; }
PamfStream at3_stereo() { return {0xdc, 0xbd, 0x00, {0, 0, 2, 1, 0, 0}, 1}; }
PamfStream lpcm_stereo() { return {0x80, 0xbd, 0x40, {0, 0, 2, 1, 0x40, 0}, 1}; }
PamfStream user_data() { return {0xdd, 0xbd, 0x20, {}, 0}; }

void write_sail_pamf3(u8* b) { write_pamf(b, {avc96x64(), at3_stereo(), user_data()}); }

// The graphics adapter's picture allocator: alloc(arg, size, count, array) fills the array with pictures 64 KB apart
// from `pics` and answers 0 (all given).
void pictures_code(u8* b, u32 at, u32 pics)
{
    // li r9,0; lis r10,hi; ori r10,r10,lo; cmpw r9,r5; bge +20; slwi r11,r9,2; stwx r10,r6,r11; addis r10,r10,1;
    // addi r9,r9,1; b -20; li r3,0; blr
    const u32 words[] = {0x39200000u, 0x3D400000u | (pics >> 16), 0x614A0000u | (pics & 0xFFFF), 0x7C092800u,
        0x40800018u, 0x552B103Au, 0x7D46592Eu, 0x3D4A0001u, 0x39290001u, 0x4BFFFFE8u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, at + 4 * i, words[i]);
    ps_write32(b, at + 0x70, at);
    ps_write32(b, at + 0x74, 0);
}

// Masks for a player's message ring (its commands' stack addresses), the ring then compared in no order.
void mask_ring(u8* b, u32 p)
{
    const u32 ring = ps_read32(b, p + 0x178);
    for (u32 i = 0; i < ps_read32(b, p + 0x170); i++) {
        const u32 e = ring + 16 * i, c = ps_read32(b, e);
        const bool command = (c >= 0x3C && c <= 0x47) || c == 0x4C || c == 0x5D || c == 0x6C || c == 0x7D ||
            c == 0x8C || c == 0x92 || c == 0x98 || c == 0x9E || c == 0xA1 || c == 0xA4 || c == 0xA7;
        if (command) g_mask.push_back({e + 8, 8});
    }
    g_rings.push_back(p + 0x178);
}

// The records' registers past each callback's arguments (whatever its caller left there) and the stack addresses
// some are handed.
void mask_records(u8* b)
{
    for (u32 i = 0; i < ps_read32(b, kRecords2); i++) {
        const u32 r = kRecords2 + 16 + 48 * i;
        switch (ps_read64(b, r + 40)) {
        case 1: g_mask.push_back({r + 32, 8}); break;      // the event handler: r7
        case 2: g_mask.push_back({r + 16, 24}); break;     // the source's makeup: r5..r7
        case 3: case 4: g_mask.push_back({r + 8, 32}); break; // makeup and cleanup: r4..r7
        case 5: g_mask.push_back({r + 8, 8}); g_mask.push_back({r + 24, 16}); break; // the format: a stack copy
        case 6: g_mask.push_back({r + 16, 24}); break;     // the pictures given back: a stack array
        default: break;
        }
    }
}

// The elementary streams opened and closed (D-919 part 2) on a player with an audio stream (the game's renderer), a
// video stream (a graphics adapter) and a user stream (an AU receiver): OpenStream opening the chosen streams, each
// call's refusals, CloseEs, OpenEs, ReopenEs of unchanged streams, CloseStream closing what is open, the player's end.
void part14(u8* b, const Call& call_, u32, GuestThread& t)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    ps_write32(b, kHeapPtr, g_heap);
    write_sail_pamf3(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kOut = kArea + 0x1900, kUri = kArea + 0x1A00,
                  kLog = kArea + 0x1B00, kRend = kArea + 0x2600, kGad = kArea + 0x2680, kAuR = kArea + 0x2800,
                  kRdA = kArea + 0x700, kRdV = kArea + 0x780, kRdU = kArea + 0x800, kSrcCode = kCode3,
                  kOpenCode = kCode3 + 0x800, kCloseCode = kCode3 + 0x880, kTables = kCode3 + 0x900,
                  kRF = kCode3 + 0xA00, kGF = kCode3 + 0xA40, kCode2 = kCode3 + 0xB00;
    const u32 kRecHandler = kCode2, kRecSource = kCode2 + 0x80, kRecRend = kCode2 + 0x100, kRecGad = kCode2 + 0x180,
              kRecFormat = kCode2 + 0x200, kRecFree = kCode2 + 0x280, kPicsCode = kCode2 + 0x300,
              kROpen = kCode2 + 0x380, kRClose = kCode2 + 0x400;
    tagged_code(b, kRecHandler, 1);
    tagged_code(b, kRecSource, 2);
    tagged_code(b, kRecRend, 3);
    tagged_code(b, kRecGad, 4);
    tagged_code(b, kRecFormat, 5);
    tagged_code(b, kRecFree, 6);
    pictures_code(b, kPicsCode, g_heap + kBigHeapSize - 0x200000);
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    source_code(b, kSrcCode, kLog);
    // The source's open and close and the renderer's open (four frames) and close answer at once.
    notify_code(b, kOpenCode, kTables);
    notify_code(b, kCloseCode, kTables + 0x10);
    notify_code(b, kROpen, kTables + 0x20);
    notify_code(b, kRClose, kTables + 0x30);
    ps_write32(b, kTables, g_export("cellSailSourceNotifyCallCompleted"));
    ps_write32(b, kTables + 0x10, g_export("cellSailSourceNotifyCallCompleted"));
    ps_write32(b, kTables + 0x20, g_export("cellSailRendererAudioNotifyCallCompleted"));
    ps_write32(b, kTables + 0x24, 4);
    ps_write32(b, kTables + 0x30, g_export("cellSailRendererAudioNotifyCallCompleted"));
    for (const u32 tb : {kTables, kTables + 0x10, kTables + 0x20, kTables + 0x30}) g_mask.push_back({tb, 4});
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); return at; };
    for (u32 i = 0; i < 14; i++) ps_write32(b, kFuncs + 4 * i, 0);
    ps_write32(b, kFuncs, tagged(kRecSource));
    ps_write32(b, kFuncs + 4, kSrcCode + 0xB0);
    ps_write32(b, kFuncs + 8, kOpenCode + 0x58);
    ps_write32(b, kFuncs + 12, kCloseCode + 0x58);
    ps_write32(b, kFuncs + 44, kSrcCode + 0x78);
    for (u32 i = 0; i < 9; i++) ps_write32(b, kRF + 4 * i, 0);
    ps_write32(b, kRF, tagged(kRecRend));
    ps_write32(b, kRF + 4, tagged(kRecRend));
    ps_write32(b, kRF + 8, kROpen + 0x58);
    ps_write32(b, kRF + 12, kRClose + 0x58);
    const u32 gf[5] = {tagged(kRecGad), tagged(kRecGad), tagged(kRecFormat), kPicsCode + 0x70, tagged(kRecFree)};
    for (u32 i = 0; i < 5; i++) ps_write32(b, kGF + 4 * i, gf[i]);
    const auto until = [&](const std::function<bool()>& done) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(done());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };
    const auto idle = [&] {
        until([&] { return __atomic_load_n(reinterpret_cast<u32*>(b + kP + 0x250), __ATOMIC_ACQUIRE) == 0; });
    };
    const auto state = [&](u8 st) { until([&] { return __atomic_load_n(b + kP + 0x163, __ATOMIC_ACQUIRE) == st; }); };
    // Readers set to each stream, for OpenEs (pspocket's cellPamf on both sides).
    const auto pamf = [&](const char* name, std::initializer_list<u64> a) {
        const Function* fn = registry().find("cellPamf", recomp::nid_of(name));
        REQUIRE(fn);
        u32 i = 3;
        for (const u64 v : a) t.ctx.gpr[i++] = v;
        fn->fn(t.ctx, b);
        return static_cast<s32>(t.ctx.gpr[3]);
    };

    call("cellSailPlayerInitialize", {kP, 0, kMem, tagged(kRecHandler), 0x22, 1, 1, 1, 0});
    call("cellSailSourceInitialize", {kSource, kFuncs, kSource});
    call("cellSailPlayerRegisterSource", {kP, str(kUri + 0x100, "x-test"), kSource});
    call("cellSailRendererAudioInitialize", {kRend, kRF, kRend, 0});
    call("cellSailGraphicsAdapterInitialize", {kGad, kGF, 0x66});
    call("cellSailAuReceiverInitialize", {kAuR, 0, 0, 0x78});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRend});
    call("cellSailPlayerSetGraphicsAdapter", {kP, 0, kGad});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kAuR});
    for (u32 e = 1; e < 12; e++) call("cellSailPlayerSubscribeEvent", {kP, e, 0});
    call("cellSailPlayerBoot", {kP, 0x99});
    state(2);
    const u32 uri = str(kUri, "x-test://movie.pam");
    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x80, uri, kOut});
    const u32 d = ps_read32(b, kOut);
    call("cellSailDescriptorOpen", {d});
    call("cellSailPlayerAddDescriptor", {kP, d});
    call("cellSailPlayerOpenStream", {kP, 0x1234});
    state(4);
    idle();
    for (const auto& [rd, type] : {std::pair{kRdA, 21u}, std::pair{kRdV, 20u}, std::pair{kRdU, 5u}}) {
        CHECK(pamf("cellPamfReaderInitialize", {rd, kPamf, 0, 1}) == 0);
        CHECK(pamf("cellPamfReaderSetStreamWithTypeAndIndex", {rd, type, 0}) >= 0);
    }
    // Refusals: a chosen stream opened again, an index out of range, the pointer that must be given, a stream not open.
    call("cellSailPlayerOpenEsAudio", {kP, 0, kRdA, 1, 0x40});
    call("cellSailPlayerOpenEsVideo", {kP, 3, kRdV, 1, 0x41});
    call("cellSailPlayerOpenEsUser", {kP, 0, kRdU, 0, 0x42});
    call("cellSailPlayerCloseEsUser", {kP, 2, 0x43});
    // Each kind closed and opened again; the audio and video streams reopened unchanged.
    call("cellSailPlayerCloseEsAudio", {kP, 0, 0x50});
    idle();
    call("cellSailPlayerCloseEsAudio", {kP, 0, 0x51});
    call("cellSailPlayerCloseEsVideo", {kP, 0, 0x52});
    idle();
    call("cellSailPlayerCloseEsUser", {kP, 0, 0x53});
    idle();
    call("cellSailPlayerOpenEsAudio", {kP, 0, kRdA, 1, 0x54});
    idle();
    call("cellSailPlayerOpenEsVideo", {kP, 0, kRdV, 1, 0x55});
    idle();
    call("cellSailPlayerOpenEsUser", {kP, 0, kRdU, 1, 0x56});
    idle();
    call("cellSailPlayerReopenEsAudio", {kP, 0, kRdA, 1, 0x57});
    idle();
    call("cellSailPlayerReopenEsVideo", {kP, 0, kRdV, 1, 0x58});
    idle();
    call("cellSailPlayerReopenEsUser", {kP, 0, kRdU, 1, 0x59});
    idle();
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerCloseStream", {kP, 0x60});
    state(2);
    idle();
    call("cellSailPlayerFinalize", {kP});
    g_mask.push_back({kP + 0x3C8, 16});
    mask_records(b);
    mask_ring(b, kP);
}

// Elementary streams of a mode-1 player (D-919 part 2) with a sound adapter, the game's video renderer and an AU
// receiver over five streams (AVC 96x64 and 128x64, ATRAC3plus, LPCM, user data): reopened with a changed format
// (closed and opened again, LPCM and the larger picture), an open whose renderer refuses (closed again, the call still
// completing), opened again; CloseStream; a second stream opened and the player finalized with it open.
void part15(u8* b, const Call& call_, u32, GuestThread& t)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    ps_write32(b, kHeapPtr, g_heap);
    write_pamf(b, {avc96x64(), avc128x64(), at3_stereo(), lpcm_stereo(), user_data()});
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kOut = kArea + 0x1900, kUri = kArea + 0x1A00,
                  kLog = kArea + 0x1B00, kRend2 = kArea + 0x2600, kSad = kArea + 0x2700, kAuR = kArea + 0x2800,
                  kRdA = kArea + 0x700, kRdV = kArea + 0x780, kSrcCode = kCode3, kOpenCode = kCode3 + 0x800,
                  kCloseCode = kCode3 + 0x880, kTables = kCode3 + 0x900, kRF = kCode3 + 0xA00, kSF = kCode3 + 0xA40,
                  kCode2 = kCode3 + 0xB00;
    const u32 kRecHandler = kCode2, kRecSource = kCode2 + 0x80, kRecRend = kCode2 + 0x100, kRecSad = kCode2 + 0x180,
              kRecFormat = kCode2 + 0x200, kROpen = kCode2 + 0x380, kRClose = kCode2 + 0x400;
    tagged_code(b, kRecHandler, 1);
    tagged_code(b, kRecSource, 2);
    tagged_code(b, kRecRend, 3);
    tagged_code(b, kRecSad, 4);
    tagged_code(b, kRecFormat, 5);
    ps_write32(b, kMem, kAllocCode + 0x70);
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    source_code(b, kSrcCode, kLog);
    notify_code(b, kOpenCode, kTables);
    notify_code(b, kCloseCode, kTables + 0x10);
    notify_code(b, kROpen, kTables + 0x20);
    notify_code(b, kRClose, kTables + 0x30);
    ps_write32(b, kTables, g_export("cellSailSourceNotifyCallCompleted"));
    ps_write32(b, kTables + 0x10, g_export("cellSailSourceNotifyCallCompleted"));
    ps_write32(b, kTables + 0x20, g_export("cellSailRendererVideoNotifyCallCompleted"));
    ps_write32(b, kTables + 0x24, 3);
    ps_write32(b, kTables + 0x30, g_export("cellSailRendererVideoNotifyCallCompleted"));
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); return at; };
    for (u32 i = 0; i < 14; i++) ps_write32(b, kFuncs + 4 * i, 0);
    ps_write32(b, kFuncs, tagged(kRecSource));
    ps_write32(b, kFuncs + 4, kSrcCode + 0xB0);
    ps_write32(b, kFuncs + 8, kOpenCode + 0x58);
    ps_write32(b, kFuncs + 12, kCloseCode + 0x58);
    ps_write32(b, kFuncs + 44, kSrcCode + 0x78);
    for (u32 i = 0; i < 9; i++) ps_write32(b, kRF + 4 * i, 0);
    ps_write32(b, kRF, tagged(kRecRend));
    ps_write32(b, kRF + 4, tagged(kRecRend));
    ps_write32(b, kRF + 8, kROpen + 0x58);
    ps_write32(b, kRF + 12, kRClose + 0x58);
    const u32 sf[3] = {tagged(kRecSad), tagged(kRecSad), tagged(kRecFormat)};
    for (u32 i = 0; i < 3; i++) ps_write32(b, kSF + 4 * i, sf[i]);
    const auto until = [&](const std::function<bool()>& done) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(done());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };
    const auto idle = [&] {
        until([&] { return __atomic_load_n(reinterpret_cast<u32*>(b + kP + 0x250), __ATOMIC_ACQUIRE) == 0; });
    };
    const auto state = [&](u8 st) { until([&] { return __atomic_load_n(b + kP + 0x163, __ATOMIC_ACQUIRE) == st; }); };
    const auto pamf = [&](const char* name, std::initializer_list<u64> a) {
        const Function* fn = registry().find("cellPamf", recomp::nid_of(name));
        REQUIRE(fn);
        u32 i = 3;
        for (const u64 v : a) t.ctx.gpr[i++] = v;
        fn->fn(t.ctx, b);
        return static_cast<s32>(t.ctx.gpr[3]);
    };

    call("cellSailPlayerInitialize", {kP, 1, kMem, tagged(kRecHandler), 0x22, 1, 1, 1, 0});
    call("cellSailSourceInitialize", {kSource, kFuncs, kSource});
    call("cellSailPlayerRegisterSource", {kP, str(kUri + 0x100, "x-test"), kSource});
    call("cellSailSoundAdapterInitialize", {kSad, kSF, 0x55});
    call("cellSailRendererVideoInitialize", {kRend2, kRF, kRend2, 0});
    call("cellSailAuReceiverInitialize", {kAuR, 0, 0, 0x78});
    call("cellSailPlayerSetSoundAdapter", {kP, 0, kSad});
    call("cellSailPlayerSetRendererVideo", {kP, 0, kRend2});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kAuR});
    for (u32 e = 1; e < 12; e++) call("cellSailPlayerSubscribeEvent", {kP, e, 0});
    call("cellSailPlayerBoot", {kP, 0x98});
    state(2);
    const u32 uri = str(kUri, "x-test://movie.pam");
    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x80, uri, kOut});
    const u32 d = ps_read32(b, kOut);
    call("cellSailDescriptorOpen", {d});
    call("cellSailPlayerAddDescriptor", {kP, d});
    call("cellSailPlayerOpenStream", {kP, 0x1234});
    state(4);
    idle();
    CHECK(pamf("cellPamfReaderInitialize", {kRdA, kPamf, 0, 1}) == 0);
    CHECK(pamf("cellPamfReaderSetStreamWithTypeAndIndex", {kRdA, 21, 1}) >= 0); // LPCM
    CHECK(pamf("cellPamfReaderInitialize", {kRdV, kPamf, 0, 1}) == 0);
    CHECK(pamf("cellPamfReaderSetStreamWithTypeAndIndex", {kRdV, 20, 1}) >= 0); // AVC 128x64
    call("cellSailPlayerReopenEsAudio", {kP, 0, kRdA, 1, 0x70});
    idle();
    call("cellSailPlayerReopenEsVideo", {kP, 0, kRdV, 1, 0x71});
    idle();
    // The video renderer refuses to open.
    ps_write32(b, kTables + 0x24, 0x80610A0B);
    call("cellSailPlayerCloseEsVideo", {kP, 0, 0x72});
    idle();
    call("cellSailPlayerOpenEsVideo", {kP, 0, kRdV, 1, 0x73});
    idle();
    ps_write32(b, kTables + 0x24, 3);
    call("cellSailPlayerOpenEsVideo", {kP, 0, kRdV, 1, 0x74});
    idle();
    call("cellSailPlayerCloseStream", {kP, 0x75});
    state(2);
    idle();
    // A second stream opened, the player finalized with it open.
    call("cellSailPlayerCreateDescriptor", {kP, 0, kUri + 0x80, uri, kOut + 4});
    const u32 d2 = ps_read32(b, kOut + 4);
    call("cellSailDescriptorOpen", {d2});
    call("cellSailPlayerAddDescriptor", {kP, d2});
    call("cellSailPlayerOpenStream", {kP, 0x76});
    state(4);
    idle();
    call("cellSailPlayerFinalize", {kP});
    g_mask.push_back({kP + 0x3C8, 16});
    mask_records(b);
    mask_ring(b, kP);
}
} // namespace

void compare_runs(int part)
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    const Run mine = run(*flash, false, part);
    const Run sony = run(*flash, true, part);
    compare_lines("answer", mine.lines, sony.lines);
    compare_lines("lv2 call", mine.calls, sony.calls);
    usize first = 0;
    while (first < mine.mem.size() && mine.mem[first] == sony.mem[first]) first++;
    INFO(std::format("memory differs first at {:#x} (pspocket {:#04x}, libsail {:#04x})", kArea + first,
        first < mine.mem.size() ? mine.mem[first] : 0, first < sony.mem.size() ? sony.mem[first] : 0));
    CHECK(first == mine.mem.size());
    if (std::getenv("SAIL_DIFF")) {
        const auto dump = [](const char* what, u32 base, const std::vector<u8>& a, const std::vector<u8>& c) {
            for (usize i = 0; i < std::min(a.size(), c.size()); i++)
                if (a[i] != c[i]) std::fprintf(stderr, "%s %#zx: pspocket %02x libsail %02x\n", what, base + i, a[i], c[i]);
        };
        dump("area", kArea, mine.mem, sony.mem);
        for (const std::string& l : mine.lines) std::fprintf(stderr, "ANSWER %s\n", l.c_str());
        for (const auto* r : {&mine, &sony})
            for (const std::string& c : r->calls)
                std::fprintf(stderr, "%s %s\n", r == &mine ? "CALLS-MINE" : "CALLS-SONY", c.c_str());
        dump("heap", kHeap, mine.heap, sony.heap);
    }
    CHECK(mine.records.size() == sony.records.size());
    usize rr = 0;
    while (rr < mine.records.size() && rr < sony.records.size() && mine.records[rr] == sony.records[rr]) rr++;
    INFO(std::format("the records differ first at {:#x}", kRecords2 + rr));
    CHECK(rr == std::min(mine.records.size(), sony.records.size()));
    if (std::getenv("SAIL_DIFF"))
        for (usize i = 0; i < std::min(mine.records.size(), sony.records.size()); i++)
            if (mine.records[i] != sony.records[i])
                std::fprintf(stderr, "records %#zx: pspocket %02x libsail %02x\n", kRecords2 + i, mine.records[i],
                    sony.records[i]);
    usize dd = 0;
    while (dd < mine.dump.size() && mine.dump[dd] == sony.dump[dd]) dd++;
    INFO(std::format("the image differs first at {:#x}", kDump + dd));
    CHECK(dd == mine.dump.size());
    CHECK(mine.heap.size() == sony.heap.size());
    usize h = 0;
    while (h < mine.heap.size() && h < sony.heap.size() && mine.heap[h] == sony.heap[h]) h++;
    INFO(std::format("the allocator's memory differs first at {:#x} (pspocket {:#04x}, libsail {:#04x})", kHeap + h,
        h < mine.heap.size() ? mine.heap[h] : 0, h < sony.heap.size() ? sony.heap[h] : 0));
    CHECK(h == std::min(mine.heap.size(), sony.heap.size()));
}

TEST_CASE("cellSail against libsail: allocator, future, source, renderers", "[hle][lle][sail]") { compare_runs(1); }

TEST_CASE("cellSail against libsail: the player's life before it plays", "[hle][lle][sail]") { compare_runs(2); }

TEST_CASE("cellSail against libsail: descriptors' fields and capabilities", "[hle][lle][sail]") { compare_runs(3); }

TEST_CASE("cellSail against libsail: the adapters' set-up, formats and AV sync", "[hle][lle][sail]") { compare_runs(4); }

TEST_CASE("cellSail against libsail: every player parameter set and read", "[hle][lle][sail]") { compare_runs(5); }

TEST_CASE("cellSail against libsail: Boot and Finalize of a player without streams", "[hle][lle][sail]")
{
    compare_runs(6);
}

TEST_CASE("cellSail against libsail: descriptors created, added, removed and destroyed", "[hle][lle][sail]")
{
    compare_runs(7);
}

TEST_CASE("cellSail against libsail: descriptors opened and closed through a game's source", "[hle][lle][sail]")
{
    compare_runs(8);
}


TEST_CASE("cellSail against libsail: Boot and Finalize of players with streams", "[hle][lle][sail]")
{
    compare_runs(9);
}


TEST_CASE("cellSail against libsail: the elementary streams chosen, SetEs and CreateDatabase", "[hle][lle][sail]")
{
    compare_runs(10);
}


TEST_CASE("cellSail against libsail: pause, repeat mode and the streams' mute switches", "[hle][lle][sail]")
{
    compare_runs(11);
}


TEST_CASE("cellSail against libsail: DumpImage at every size", "[hle][lle][sail]")
{
    compare_runs(12);
}


TEST_CASE("cellSail against libsail: OpenStream and CloseStream of a player without streams", "[hle][lle][sail]")
{
    compare_runs(13);
}


TEST_CASE("cellSail against libsail: elementary streams opened and closed", "[hle][lle][sail]")
{
    compare_runs(14);
}


TEST_CASE("cellSail against libsail: elementary streams reopened with new formats and left open", "[hle][lle][sail]")
{
    compare_runs(15);
}
