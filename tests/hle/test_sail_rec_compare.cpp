// cellSailRec against Sony's libsail_rec (D-918): the same calls to pspocket's library and to Sony's module (the user's
// installed system software; skipped without it, compare_util.hpp), each half in a process of its own, on the same
// objects at the same addresses: the recorder, its block from the game's allocator (guest code here, logging every
// call), feeders, the composer, profiles and video converters; a SPURS instance (pspocket's, in both halves).
// Compared after every call: the answer, the objects' bytes (each half's own code addresses, the lv2 objects' IDs and the
// thread's stack addresses masked), the allocator's log, the event handler's records and the lv2 calls the call made.
// No game data.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "compare_util.hpp"
#include "jpeg_writer.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
namespace as = pspocket::recomp::ppu::as;

namespace {

// The guest memory the scripts use (kData: 2 MB).
constexpr u32 kCode = kData;                 // guest code: the allocator, the event handler, callbacks
constexpr u32 kOpd = kData + 0x1000;         // their descriptors
constexpr u32 kAllocDesc = kOpd, kFreeDesc = kOpd + 8, kHandlerDesc = kOpd + 0x10, kCallbackDesc = kOpd + 0x18;
constexpr u32 kAllocObj = kData + 0x1100;    // CellSailMemAllocator {alloc, free, arg}
constexpr u32 kHeapState = kData + 0x1110;   // the allocator's argument: {next free, fail after n (0: never), count}
constexpr u32 kAllocLog = kData + 0x1200;    // {kind 1 alloc / 2 free, boundary, size or pointer, answer} from +16
constexpr u32 kAllocRecs = 0xE0;
constexpr u32 kEvents = kData + 0x2000;      // the handler's records: count, then {arg, event, a, b} (32 bytes) from +16
constexpr u32 kEventRecs = 0x7F;
constexpr u32 kArgs = kData + 0x3000;        // attributes, parameters, small outputs (4 KB)
constexpr u32 kSpurs = kData + 0x4000;       // CellSpurs (4 KB, 128-aligned)
constexpr u32 kRec = kData + 0x6000;         // the recorder (0x2000)
constexpr u32 kFeeders = kData + 0x8000;     // feeders, 0x200 each: audio 0-3, video 4-7
constexpr u32 kComposer = kData + 0x9000;    // the composer (0x140)
constexpr u32 kCallbacks = kData + 0x9200;   // callback tables the scripts hand objects (0x100)
constexpr u32 kProfile = kData + 0xA000;     // a profile and its parameter tables
constexpr u32 kConv = kData + 0xC000;        // a video converter (0x200)
constexpr u32 kStreams = kData + 0xD000;     // composer streams and their units (12 KB)
constexpr u32 kDump = kData + 0x10000;       // DumpImage's buffer (0x8000)
constexpr u32 kBoot = kData + 0x20000;       // the booted recorder's feeders' callbacks: 4 tables of 11 (0x40 apart),
constexpr u32 kBootDescs = kBoot + 0x100;    // their 44 descriptors (the recording callback, TOC: its answer word)
constexpr u32 kBootAnswers = kBoot + 0x300;  // and their 44 answers
constexpr u32 kHeap = kData + 0x40000;       // the allocator's heap
constexpr u32 kHeapEnd = kData + 0x100000;

// The allocator: alloc(arg, boundary, size) hands out the heap aligned to the boundary (failing once its countdown,
// when set, runs out); free(arg, boundary, pointer) only records. Both log their calls. The event handler records
// (arg, event, a, b); the generic callback records nothing and answers 0.
void write_code(u8* b)
{
    as::Assembler c(kCode);
    // alloc
    const auto alloc_fail = c.new_label(), alloc_log = c.new_label();
    c.emit(as::lwz(9, 4, 3));        // countdown
    c.emit(as::cmpwi(0, 9, 0));
    const auto no_countdown = c.new_label();
    c.beq(0, no_countdown);
    c.emit(as::addi(9, 9, -1));
    c.emit(as::stw(9, 4, 3));
    c.emit(as::cmpwi(0, 9, 0));
    c.beq(0, alloc_fail);
    c.bind(no_countdown);
    c.emit(as::lwz(9, 0, 3));        // next
    c.emit(as::addi(10, 4, -1));
    c.emit(as::add(9, 9, 10));
    c.emit(as::andc(9, 9, 10));      // aligned
    c.emit(as::add(11, 9, 5));
    c.emit(as::stw(11, 0, 3));
    c.emit(as::mr(12, 9));
    c.b(alloc_log);
    c.bind(alloc_fail);
    c.emit(as::li(12, 0));
    c.bind(alloc_log);
    c.emit(as::lis(7, static_cast<s32>(kAllocLog >> 16)));
    c.emit(as::ori(7, 7, kAllocLog & 0xFFFF));
    c.emit(as::lwz(8, 0, 7));
    c.emit(as::andi_(10, 8, kAllocRecs - 1));
    c.emit(as::slwi(10, 10, 4));
    c.emit(as::add(10, 10, 7));
    c.emit(as::li(11, 1));
    c.emit(as::stw(11, 16, 10));
    c.emit(as::stw(4, 20, 10));
    c.emit(as::stw(5, 24, 10));
    c.emit(as::stw(12, 28, 10));
    c.emit(as::addi(8, 8, 1));
    c.emit(as::stw(8, 0, 7));
    c.emit(as::mr(3, 12));
    c.emit(as::blr());
    while (c.pc() < kCode + 0x200) c.emit(as::nop());
    // free
    c.emit(as::lis(7, static_cast<s32>(kAllocLog >> 16)));
    c.emit(as::ori(7, 7, kAllocLog & 0xFFFF));
    c.emit(as::lwz(8, 0, 7));
    c.emit(as::andi_(10, 8, kAllocRecs - 1));
    c.emit(as::slwi(10, 10, 4));
    c.emit(as::add(10, 10, 7));
    c.emit(as::li(11, 2));
    c.emit(as::stw(11, 16, 10));
    c.emit(as::stw(4, 20, 10));
    c.emit(as::stw(5, 24, 10));
    c.emit(as::li(11, 0));
    c.emit(as::stw(11, 28, 10));
    c.emit(as::addi(8, 8, 1));
    c.emit(as::stw(8, 0, 7));
    c.emit(as::blr());
    while (c.pc() < kCode + 0x300) c.emit(as::nop());
    // the event handler
    c.emit(as::lis(7, static_cast<s32>(kEvents >> 16)));
    c.emit(as::ori(7, 7, kEvents & 0xFFFF));
    c.emit(as::lwz(8, 0, 7));
    c.emit(as::andi_(10, 8, kEventRecs));
    c.emit(as::slwi(10, 10, 5));
    c.emit(as::add(10, 10, 7));
    c.emit(as::std_(3, 16, 10));
    c.emit(as::std_(4, 24, 10));
    c.emit(as::std_(5, 32, 10));
    c.emit(as::std_(6, 40, 10));
    c.emit(as::lwsync());
    c.emit(as::addi(8, 8, 1));
    c.emit(as::stw(8, 0, 7));
    c.emit(as::li(3, 0));
    c.emit(as::blr());
    while (c.pc() < kCode + 0x400) c.emit(as::nop());
    // the generic callback
    c.emit(as::li(3, 0));
    c.emit(as::blr());
    while (c.pc() < kCode + 0x500) c.emit(as::nop());
    // the recording callback: a record {its TOC (r2, which names it), r3, 0, 0} with the handler's, the word at r2 its
    // answer
    c.emit(as::lis(7, static_cast<s32>(kEvents >> 16)));
    c.emit(as::ori(7, 7, kEvents & 0xFFFF));
    c.emit(as::lwz(8, 0, 7));
    c.emit(as::andi_(10, 8, kEventRecs));
    c.emit(as::slwi(10, 10, 5));
    c.emit(as::add(10, 10, 7));
    c.emit(as::std_(2, 16, 10));
    c.emit(as::std_(3, 24, 10));
    c.emit(as::li(9, 0));
    c.emit(as::std_(9, 32, 10));
    c.emit(as::std_(9, 40, 10));
    c.emit(as::lwsync());
    c.emit(as::addi(8, 8, 1));
    c.emit(as::stw(8, 0, 7));
    c.emit(as::lwz(3, 0, 2));
    c.emit(as::blr());
    const std::vector<u8> code = c.finish();
    std::memcpy(b + kCode, code.data(), code.size());
    ps_write32(b, kAllocDesc, kCode);
    ps_write32(b, kFreeDesc, kCode + 0x200);
    ps_write32(b, kHandlerDesc, kCode + 0x300);
    ps_write32(b, kCallbackDesc, kCode + 0x400);
    for (u32 d = kAllocDesc; d <= kCallbackDesc; d += 8) ps_write32(b, d + 4, 0);
    ps_write32(b, kAllocObj, kAllocDesc);
    ps_write32(b, kAllocObj + 4, kFreeDesc);
    ps_write32(b, kAllocObj + 8, kHeapState);
    ps_write32(b, kHeapState, kHeap);
    ps_write32(b, kHeapState + 4, 0);
}

// The lv2 calls of the script's thread (numbers; the objects' IDs and stack addresses are each process's own). The
// access control question pspocket's DumpImage asks (sc 1 and 871: Sony's module asks it once at its start) is left out.
struct Trace {
    std::mutex m;
    std::thread::id caller;
    std::vector<u32> calls;
    bool lightweight = true; // false: the lwmutex and lwcond calls (95 to 99, 111 to 115) left out
};
void observe(void* user, u32 n, const u64*)
{
    if (n == 1 || n == 871) return;
    Trace& tr = *static_cast<Trace*>(user);
    if (!tr.lightweight && ((n >= 95 && n <= 99) || (n >= 111 && n <= 115))) return;
    std::lock_guard lock(tr.m);
    if (std::this_thread::get_id() == tr.caller) tr.calls.push_back(n);
}

struct H : Half<> {
    Trace tr;
    std::unique_ptr<loader::LleModules> imports; // pspocket's half: the firmware libraries cellSailRec imports

    H(const Setup& s, bool sony, bool with_imports = false) : Half<>(s, sony, "cellSailRec")
    {
        // libsail_rec's SPU programs and tables (pspocket's own reads them from the installed module).
        if (const auto flash = test_dev_flash()) REQUIRE(f.kernel->vfs_mount("/dev_flash", *flash, true));
        if (with_imports && !sony && s.mode == Mode::Firmware) {
            // As the loader links a host module's imports (LleModules::link_host_imports, D-197), which passes over a
            // firmware-first library's (its firmware module imports for itself): each module loaded, each export linked.
            imports = std::make_unique<loader::LleModules>(s.flash);
            REQUIRE(imports->index());
            for (const hle::HostImport& i : hle::registry().imports()) {
                if (i.module != "cellSailRec" || f.hle->imported(i.library, i.nid)) continue;
                std::string why;
                const loader::PrxModule* m = imports->load(i.library, *f.hle, &why);
                INFO(why);
                REQUIRE(m);
                const auto& exports = i.variable ? m->variables : m->functions;
                const auto it = exports.find({std::string(i.library), i.nid});
                REQUIRE(it != exports.end());
                f.hle->link_import(i.library, i.nid, it->second);
            }
            for (const hle::ModuleEntry& start : imports->take_starts()) hle::run_module_entry(t.ctx, start, 0, 0);
        }
        std::memset(b + kData, 0, 0x100000);
        write_code(b);
        tr.caller = std::this_thread::get_id();
        f.kernel->set_syscall_observer(&observe, &tr);
    }
    ~H() { f.kernel->set_syscall_observer(nullptr, nullptr); }

    u32 r32(u32 a) const { return ps_read32(b, a); }
    void w32(u32 a, u32 v) { ps_write32(b, a, v); }

    // Calls a cellSailRec export; its answer and lv2 calls as lines.
    u64 call(const std::string& key, std::string_view fn, std::initializer_list<u64> args)
    {
        {
            std::lock_guard lock(tr.m);
            tr.calls.clear();
        }
        trace("calling: " + key);
        u64 v = ~0ull;
        if (args.size() <= 8) {
            const std::optional<u64> r = side.call(fn, args);
            v = r ? *r : ~0ull;
            o.line(key, r ? answer(*r) : std::string("(not exported)"));
        } else {
            const u32 d = side.descriptor(fn);
            REQUIRE(d);
            v = hle::call_guest(t.ctx, d, args);
            o.line(key, answer(v));
        }
        std::string calls;
        {
            std::lock_guard lock(tr.m);
            for (const u32 n : tr.calls) calls += std::format(" {}", n);
        }
        o.line(key + ": lv2", calls);
        return v;
    }

    // Guest bytes as a line, with ranges masked (offsets from `at`).
    void mem(const std::string& key, u32 at, u32 size, std::initializer_list<std::pair<u32, u32>> mask = {})
    {
        std::vector<u8> v(b + at, b + at + size);
        for (const auto& [off, n] : mask)
            for (u32 i = 0; i < n && off + i < size; i++) v[off + i] = 0x5A;
        o.bytes(key, std::move(v));
    }

    // The allocator's log and the handler's records since the last look, as lines.
    u32 allocs_seen = 0, events_seen = 0;
    void logs(const std::string& key)
    {
        const u32 na = r32(kAllocLog);
        for (; allocs_seen < na; allocs_seen++) {
            const u32 at = kAllocLog + 16 + 16 * (allocs_seen & (kAllocRecs - 1));
            o.line(std::format("{}: allocator {}", key, allocs_seen),
                std::format("{} {:#x} {:#x} {:#x}", r32(at), r32(at + 4), r32(at + 8), r32(at + 12)));
        }
        const u32 ne = r32(kEvents);
        for (; events_seen < ne; events_seen++) {
            const u32 at = kEvents + 16 + 32 * (events_seen & kEventRecs);
            o.line(std::format("{}: event {}", key, events_seen),
                std::format("{:#x} {:#x} {:#x} {:#x}", ps_read64(b, at), ps_read64(b, at + 8), ps_read64(b, at + 16),
                    ps_read64(b, at + 24)));
        }
    }

    // The recorder's bytes (0x2000), with what is each half's own masked: the queue's lv2 mutex and condition IDs, the
    // lwmutex's and lwcond's sleep queues, the MP4 composer's file calls (each half's code), the stack addresses.
    void recorder(const std::string& key)
    {
        mem(key + ": the recorder", kRec, 0x2000,
            {{0x4BC, 8}, {0x4F0, 4}, {0x4FC, 4}, {0x7AC, 4}, {0x7B4, 4 * 5}, {0x7D0, 4 * 3}, {0x870, 16}});
    }
    // The recorder's block (the slots and the queue's ring) from the allocator.
    void block(const std::string& key)
    {
        const u32 blk = r32(kRec + 0x7F0);
        if (!blk) {
            o.line(key + ": the block", "none");
            return;
        }
        const u32 msgs = r32(kRec + 0x4A8);
        const u32 size = msgs * 16 + (r32(kRec + 0x7E4) + r32(kRec + 0x7E8) + r32(kRec + 0x7EC)) * 0x8000;
        mem(key + ": the block", blk, std::min<u32>(size, 0x20000));
    }
};

// Initialize's attribute {0x1C, mode, audio, video, user, messages, timescale} at kArgs and resource {0xC, SPURS, 0}.
void attribute(H& h, u32 mode, u32 a, u32 v, u32 u, u32 msgs = 0, u32 timescale = 0)
{
    const u32 at = kArgs;
    h.w32(at, 0x1C);
    h.w32(at + 4, mode);
    h.w32(at + 8, a);
    h.w32(at + 12, v);
    h.w32(at + 16, u);
    h.w32(at + 20, msgs);
    h.w32(at + 24, timescale);
    h.w32(at + 0x20, 0xC);
    h.w32(at + 0x24, kSpurs);
    h.w32(at + 0x28, 0);
}
constexpr u32 kAttr = kArgs, kRes = kArgs + 0x20;

// The SPURS instance both halves' recorders use (pspocket's cellSpurs).
void make_spurs(H& h)
{
    REQUIRE(h.t.call("cellSpursInitialize", kSpurs, 2, 200, 2016, 0) == 0);
}

void finish_spurs(H& h) { REQUIRE(h.t.call("cellSpursFinalize", kSpurs) == 0); }

// ------------------------------------------------------------------------------------------------- the scripts

// Initialize's checks and what it lays out; Finalize; allocation failures.
void script_initialize(H& h)
{
    make_spurs(h);
    attribute(h, 0, 1, 1, 0);
    h.call("Initialize(NULL recorder)", "cellSailRecorderInitialize", {0, kAllocObj, kHandlerDesc, 7, kAttr, kRes});
    h.call("Initialize(NULL attribute)", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 7, 0, kRes});
    h.w32(kAttr, 0x1D);
    std::memset(h.b + kRec, 0xEE, 0x2000);
    h.call("Initialize(attribute size 0x1D)", "cellSailRecorderInitialize",
        {kRec, kAllocObj, kHandlerDesc, 7, kAttr, kRes});
    h.mem("the recorder after a wrong attribute size", kRec, 0x40);
    h.w32(kAttr, 0x1C);
    h.w32(kRes, 0xD);
    h.call("Initialize(resource size 0xD)", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 7, kAttr, kRes});
    h.call("Initialize(NULL resource)", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 7, kAttr, 0});
    h.w32(kRes, 0xC);
    h.call("Initialize(NULL allocator)", "cellSailRecorderInitialize", {kRec, 0, kHandlerDesc, 7, kAttr, kRes});
    h.mem("the recorder after a NULL allocator (cleared)", kRec, 0x40);
    h.w32(kRes + 4, 0);
    h.call("Initialize(no SPURS)", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 7, kAttr, kRes});
    h.w32(kRes + 4, kSpurs);
    h.w32(kRes + 8, 1);
    h.call("Initialize(resource word 2 set)", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 7, kAttr, kRes});
    h.w32(kRes + 8, 0);
    h.w32(kAttr + 4, 2);
    h.call("Initialize(mode 2)", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 7, kAttr, kRes});
    h.recorder("mode 2");
    h.logs("mode 2");
    struct Case {
        u32 mode, a, v, u, msgs, ts;
    };
    for (const Case& c : std::vector<Case>{{0, 0, 0, 0, 0, 0}, {1, 1, 1, 0, 0, 0}, {0, 2, 3, 1, 0, 0},
             {1, 1, 0, 2, 7, 90000}, {0, 3, 2, 0, 1, 1}}) {
        const std::string k = std::format("Initialize(mode {}, {} audio, {} video, {} user, {} messages, timescale {})",
            c.mode, c.a, c.v, c.u, c.msgs, c.ts);
        attribute(h, c.mode, c.a, c.v, c.u, c.msgs, c.ts);
        h.call(k, "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 0x1234, kAttr, kRes});
        h.recorder(k);
        h.block(k);
        h.logs(k);
        h.call(k + ": Finalize", "cellSailRecorderFinalize", {kRec});
        h.recorder(k + ": Finalize");
        h.logs(k + ": Finalize");
        h.call(k + ": Finalize again", "cellSailRecorderFinalize", {kRec});
    }
    // The allocator failing.
    attribute(h, 0, 1, 1, 1);
    h.w32(kHeapState + 4, 1);
    h.call("Initialize with the allocator failing", "cellSailRecorderInitialize",
        {kRec, kAllocObj, kHandlerDesc, 0, kAttr, kRes});
    h.recorder("the allocator failing");
    h.logs("the allocator failing");
    h.call("Finalize after the allocator failed", "cellSailRecorderFinalize", {kRec});
    finish_spurs(h);
}

// An initialized recorder (2 audio, 3 video, 1 user stream) for the scripts that need one.
void init_recorder(H& h, u32 a = 2, u32 v = 3, u32 u = 1)
{
    make_spurs(h);
    attribute(h, 0, a, v, u);
    REQUIRE(h.call("Initialize", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 0x77, kAttr, kRes}) == 0);
}

// Set and get every parameter type with indices in and out of range.
void script_parameters(H& h)
{
    init_recorder(h);
    const std::vector<s64> types = {-1, 0, 1, 2, 3, 0x1C, 0x1D, 0x1E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
        0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x60, 0x100,
        0x100000002ll};
    const u64 big = 0x1122334455667788ull;
    for (const s64 t : types) {
        const u64 type = static_cast<u64>(t);
        h.call(std::format("SetParameter({:#x}, big, 0)", t), "cellSailRecorderSetParameter", {kRec, type, big, 0});
        h.call(std::format("SetParameter({:#x}, big, 1)", t), "cellSailRecorderSetParameter", {kRec, type, big, 1});
        for (const u64 index : {0ull, 1ull, 2ull, 3ull, 0xFFFFFFFFull, 0x100000001ull})
            h.call(std::format("SetParameter({:#x}, {:#x}, big)", t, index), "cellSailRecorderSetParameter",
                {kRec, type, index, big ^ (index << 8)});
        h.call(std::format("SetParameter({:#x}, 1, 0)", t), "cellSailRecorderSetParameter", {kRec, type, 1, 0});
    }
    h.recorder("after the parameters");
    h.block("after the parameters");
    const u32 p0 = kArgs + 0x100, p1 = kArgs + 0x108;
    for (const s64 t : types) {
        const u64 type = static_cast<u64>(t);
        h.call(std::format("GetParameter({:#x}, NULL, NULL)", t), "cellSailRecorderGetParameter", {kRec, type, 0, 0});
        for (const u32 index : {0u, 1u, 2u, 3u}) {
            ps_write64(h.b, p0, 0xEEEEEEEE00000000ull | index);
            ps_write64(h.b, p1, 0xEEEEEEEEEEEEEEEEull);
            h.call(std::format("GetParameter({:#x}, [{}], p1)", t, index), "cellSailRecorderGetParameter",
                {kRec, type, p0, p1});
            h.mem(std::format("GetParameter({:#x}, [{}], p1): the words", t, index), p0, 16);
            ps_write64(h.b, p0, 0xEEEEEEEE00000000ull | index);
            h.call(std::format("GetParameter({:#x}, [{}], NULL)", t, index), "cellSailRecorderGetParameter",
                {kRec, type, p0, 0});
            h.mem(std::format("GetParameter({:#x}, [{}], NULL): the word", t, index), p0, 8);
        }
    }
    finish_spurs(h);
}

// Feeders and the composer given to the recorder, events, Boot's refusals, the commands before Boot, DumpImage.
void script_recorder_api(H& h)
{
    init_recorder(h);
    const u32 fa0 = kFeeders, fa1 = kFeeders + 0x200, fv0 = kFeeders + 0x800, fv1 = kFeeders + 0xA00;
    h.call("SetFeederAudio(2)", "cellSailRecorderSetFeederAudio", {kRec, 2, fa0});
    h.call("SetFeederAudio(0, NULL)", "cellSailRecorderSetFeederAudio", {kRec, 0, 0});
    h.call("SetFeederAudio(0)", "cellSailRecorderSetFeederAudio", {kRec, 0, fa0});
    h.call("SetFeederAudio(0) again", "cellSailRecorderSetFeederAudio", {kRec, 0, fa1});
    h.call("SetFeederAudio(0x100000001)", "cellSailRecorderSetFeederAudio", {kRec, 0x100000001ull, 0x100000000ull | fa1});
    h.call("SetFeederVideo(3)", "cellSailRecorderSetFeederVideo", {kRec, 3, fv0});
    h.call("SetFeederVideo(0, NULL)", "cellSailRecorderSetFeederVideo", {kRec, 0, 0});
    h.call("SetFeederVideo(1)", "cellSailRecorderSetFeederVideo", {kRec, 1, fv0});
    h.call("Boot without every feeder", "cellSailRecorderBoot", {kRec, 0x99});
    h.call("SetFeederVideo(2)", "cellSailRecorderSetFeederVideo", {kRec, 2, fv1});
    h.call("RegisterComposer(1)", "cellSailRecorderRegisterComposer", {kRec, 1, kComposer});
    h.call("RegisterComposer(3, NULL)", "cellSailRecorderRegisterComposer", {kRec, 3, 0});
    h.call("RegisterComposer(3)", "cellSailRecorderRegisterComposer", {kRec, 3, kComposer});
    h.call("RegisterComposer(3) again", "cellSailRecorderRegisterComposer", {kRec, 3, kComposer + 0x10});
    h.mem("the composer's type", kComposer + 0x54, 4);
    h.w32(kArgs + 0x200, 0xEEEEEEEE);
    h.call("UnregisterComposer(1)", "cellSailRecorderUnregisterComposer", {kRec, 1, kArgs + 0x200});
    h.call("UnregisterComposer(3)", "cellSailRecorderUnregisterComposer", {kRec, 3, kArgs + 0x200});
    h.mem("UnregisterComposer: the composer", kArgs + 0x200, 4);
    h.call("UnregisterComposer(3, NULL)", "cellSailRecorderUnregisterComposer", {kRec, 3, 0});
    h.call("RegisterComposer(3) once more", "cellSailRecorderRegisterComposer", {kRec, 3, kComposer});
    for (const u64 e : {0ull, 1ull, 5ull, 11ull, 12ull, 63ull, 0x100000003ull}) {
        h.call(std::format("UnsubscribeEvent({:#x})", e), "cellSailRecorderUnsubscribeEvent", {kRec, e});
        h.mem(std::format("UnsubscribeEvent({:#x}): the mask", e), kRec + 0x4C8, 8);
    }
    for (const auto& [e, p] : std::vector<std::pair<u64, u64>>{{1, 0}, {11, 0}, {12, 0}, {2, 1}, {0x100000004ull, 0},
             {3, 0x100000000ull}}) {
        h.call(std::format("SubscribeEvent({:#x}, {:#x})", e, p), "cellSailRecorderSubscribeEvent", {kRec, e, p});
        h.mem(std::format("SubscribeEvent({:#x}, {:#x}): the mask", e, p), kRec + 0x4C8, 8);
    }
    ps_write64(h.b, kArgs + 0x210, ~0ull);
    h.call("ReplaceEventHandler(NULL)", "cellSailRecorderReplaceEventHandler", {kRec, 0, 1, kArgs + 0x210, kArgs + 0x214});
    h.call("ReplaceEventHandler", "cellSailRecorderReplaceEventHandler",
        {kRec, kCallbackDesc, 0x55, kArgs + 0x210, kArgs + 0x214});
    h.mem("ReplaceEventHandler: the old ones", kArgs + 0x210, 8);
    h.call("ReplaceEventHandler back, no old ones", "cellSailRecorderReplaceEventHandler", {kRec, kHandlerDesc, 0x66, 0, 0});
    // The commands before Boot.
    h.call("CreateProfile(1, NULL)", "cellSailRecorderCreateProfile", {kRec, 1, 0});
    h.call("CreateProfile(2)", "cellSailRecorderCreateProfile", {kRec, 2, kArgs + 0x220});
    h.call("CreateProfile(1)", "cellSailRecorderCreateProfile", {kRec, 1, kArgs + 0x220});
    h.call("CreateProfile(3)", "cellSailRecorderCreateProfile", {kRec, 3, kArgs + 0x220});
    h.call("DestroyProfile(NULL)", "cellSailRecorderDestroyProfile", {kRec, 0});
    h.call("DestroyProfile", "cellSailRecorderDestroyProfile", {kRec, kProfile});
    h.call("DestroyVideoConverter(NULL)", "cellSailRecorderDestroyVideoConverter", {kRec, 0});
    h.call("DestroyVideoConverter", "cellSailRecorderDestroyVideoConverter", {kRec, kConv});
    h.call("OpenStream(NULL profile)", "cellSailRecorderOpenStream", {kRec, 0, 1, kArgs, 2, 3, 4});
    h.call("OpenStream", "cellSailRecorderOpenStream", {kRec, kProfile, 1, kArgs, 2, 3, 4});
    h.call("CloseStream", "cellSailRecorderCloseStream", {kRec, 5});
    h.call("Start", "cellSailRecorderStart", {kRec, 1, 2, 3});
    h.call("Stop", "cellSailRecorderStop", {kRec, 1, 2});
    h.call("Cancel", "cellSailRecorderCancel", {kRec});
    // CreateVideoConverter's checks: formats with each field changed.
    const u32 in = kArgs + 0x300, out = kArgs + 0x340;
    const auto format = [&](u32 at, s8 coding, s16 w, s16 hgt) {
        std::memset(h.b + at, 0xFF, 0x20);
        h.b[at] = static_cast<u8>(coding);
        ps_write16(h.b, at + 4, static_cast<u16>(w));
        ps_write16(h.b, at + 6, static_cast<u16>(hgt));
        h.b[at + 0x10] = 1;
        h.b[at + 0x12] = 2;
    };
    const auto convert = [&](const std::string& k, u64 mode, u64 zero = 0, u64 outp = kArgs + 0x380) {
        h.call(k, "cellSailRecorderCreateVideoConverter", {kRec, mode, in, out, zero, 6, 7, outp});
    };
    format(in, 11, 320, 240);
    format(out, 1, -1, -1);
    convert("CreateVideoConverter(11 to 1)", 0);
    convert("CreateVideoConverter(mode 3)", 3);
    convert("CreateVideoConverter(the zero word set)", 0, 1);
    convert("CreateVideoConverter(no converter pointer)", 0, 0, 0);
    h.call("CreateVideoConverter(no in)", "cellSailRecorderCreateVideoConverter", {kRec, 0, 0, out, 0, 6, 7, kArgs + 0x380});
    h.call("CreateVideoConverter(no out)", "cellSailRecorderCreateVideoConverter", {kRec, 0, in, 0, 0, 6, 7, kArgs + 0x380});
    for (const u32 off : {0u, 1u, 2u, 3u, 4u, 6u, 8u, 0x11u, 0x13u, 0x14u, 0x18u}) {
        format(in, 11, 320, 240);
        format(out, 1, -1, -1);
        h.b[in + off] = static_cast<u8>(h.b[in + off] ^ (off == 0 || off == 4 || off == 6 ? 0xFF : 0x01));
        if (off == 4 || off == 6) h.b[in + off + 1] = 0xFF;
        convert(std::format("CreateVideoConverter(in byte {:#x} changed)", off), 1);
        format(in, 11, 320, 240);
        h.b[out + off] = static_cast<u8>(h.b[out + off] ^ 0x01);
        convert(std::format("CreateVideoConverter(out byte {:#x} changed)", off), 1);
    }
    for (const auto& [ci, co] : std::vector<std::pair<s8, s8>>{{4, 1}, {9, 1}, {4, 9}, {11, 1}, {11, 9}, {11, 4},
             {12, 1}, {12, 9}, {12, 4}, {12, 12}, {1, 4}, {2, 1}, {10, 1}, {13, 1}}) {
        format(in, ci, 640, 480);
        format(out, co, 640, 480);
        convert(std::format("CreateVideoConverter({} to {})", ci, co), 2);
        format(out, co, 320, 480);
        convert(std::format("CreateVideoConverter({} to {}, another width)", ci, co), 2);
    }
    // DumpImage at many sizes, its buffer poisoned first; the objects' fields each half has its own normalized.
    h.call("SetFeederAudio(1)", "cellSailRecorderSetFeederAudio", {kRec, 1, fa1});
    for (u32 i = 0; i < 11; i++) h.w32(kCallbacks + 4 * i, kCallbackDesc);
    h.call("FeederAudioInitialize", "cellSailFeederAudioInitialize", {fa0, kCallbacks, 0x10, 0});
    std::vector<std::pair<u32, std::vector<u8>>> saved;
    const auto normalize = [&](u32 at, u32 n) {
        saved.emplace_back(at, std::vector<u8>(h.b + at, h.b + at + n));
        std::memset(h.b + at, 0x5A, n);
    };
    for (const auto& [off, n] : std::vector<std::pair<u32, u32>>{{0x4BC, 8}, {0x4F0, 4}, {0x4FC, 4}, {0x7AC, 4},
             {0x7B4, 20}, {0x7D0, 12}, {0x870, 16}})
        normalize(kRec + off, n);
    normalize(fa0, 0x2C);
    for (const u32 size : {0u, 1u, 7u, 8u, 9u, 63u, 64u, 67u, 68u, 0x80u, 0x8FFu, 0x900u, 0x901u, 0xE67u, 0xE68u,
             0xE80u, 0x1000u, 0x1137u, 0x1138u, 0x1140u, 0x13FFu, 0x1400u, 0x2000u, 0x3000u, 0x4000u, 0x8000u}) {
        for (const u32 base : {kDump, kDump + 0x13}) {
            std::memset(h.b + kDump, 0xEE, 0x8040);
            const std::string k = std::format("DumpImage(+{:#x}, {:#x})", base - kDump, size);
            h.call(k, "cellSailRecorderDumpImage", {kRec, base, size});
            h.mem(k + ": the image", kDump, std::min<u32>(size + 0x40, 0x8040));
        }
    }
    for (const auto& [at, bytes] : saved) std::memcpy(h.b + at, bytes.data(), bytes.size());
    h.call("Finalize", "cellSailRecorderFinalize", {kRec});
    h.logs("the end");
    finish_spurs(h);
}

// The recorder's queue: its words and ring, as lines.
void queue(H& h, const std::string& key)
{
    h.mem(key + ": the queue", kRec + 0x4A8, 0x14);
    const u32 ring = h.r32(kRec + 0x4B0), cap = h.r32(kRec + 0x4A8);
    if (ring && cap) h.mem(key + ": the ring", ring, std::min<u32>(cap, 64) * 16);
}

// Feeders: Initialize's checks and what it lays out, Finalize, every notification (to the recorder's queue, or to a
// diagnostic handler the feeder holds).
void script_feeders(H& h)
{
    init_recorder(h);
    const u32 fa = kFeeders, fv = kFeeders + 0x800;
    for (u32 i = 0; i < 11; i++) h.w32(kCallbacks + 4 * i, kCallbackDesc + 0x10000 * i);
    for (const bool video : {false, true}) {
        const std::string kind = video ? "Video" : "Audio";
        const std::string init = "cellSailFeeder" + kind + "Initialize";
        const u32 f = video ? fv : fa;
        std::memset(h.b + f, 0xEE, 0x200);
        h.call(kind + "Initialize(NULL funcs)", init, {f, 0, 0x10, 0});
        for (const u32 off : {0x1Cu, 0x20u, 0x24u, 0x28u, 0x0u, 0x18u}) {
            const u32 keep = h.r32(kCallbacks + off);
            h.w32(kCallbacks + off, 0);
            h.call(std::format("{}Initialize(callback +{:#x} NULL)", kind, off), init, {f, kCallbacks, 0x10, 0});
            h.w32(kCallbacks + off, keep);
            h.mem(std::format("{}Initialize(callback +{:#x} NULL): the feeder", kind, off), f, 0x200, {{0, 0x2C}});
        }
        h.call(kind + "Initialize(an attribute)", init, {f, kCallbacks, 0x10, kArgs});
        h.call(kind + "Initialize", init, {f, kCallbacks, 0x10, 0});
        h.mem(kind + "Initialize: the feeder", f, 0x200, {{0, 0x2C}});
        h.call(kind + "Finalize", "cellSailFeeder" + kind + "Finalize", {f});
        h.mem(kind + "Finalize: the feeder", f, 0x200);
        h.call(kind + "Initialize again", init, {f, kCallbacks, 0x20, 0});
        // Notifications to the recorder's queue (the feeder given the recorder as Boot would).
        h.w32(f + 0x34, kRec);
        const std::string n = "cellSailFeeder" + kind + "Notify";
        h.call(kind + "NotifyCallCompleted(-5)", n + "CallCompleted", {f, static_cast<u64>(-5)});
        h.call(kind + "NotifyFrameOut(3)", n + "FrameOut", {f, 3});
        h.call(kind + "NotifySessionEnd", n + "SessionEnd", {f});
        h.call(kind + "NotifySessionError(0x80610703, 0)", n + "SessionError", {f, 0x80610703, 0});
        h.call(kind + "NotifySessionError(0x80610703, 1)", n + "SessionError", {f, 0x80610703, 1});
        h.call(kind + "NotifySessionError(7, 0x102)", n + "SessionError", {f, 7, 0x102});
        queue(h, kind + " notifications");
        // A diagnostic handler at +0x98 (audio) or +0xA0 (video).
        const u32 diag = video ? 0xA0 : 0x98;
        h.w32(f + diag, kHandlerDesc);
        h.w32(f + diag + 4, 0xAB);
        h.call(kind + "NotifyCallCompleted(9) to the handler", n + "CallCompleted", {f, 9});
        h.call(kind + "NotifyFrameOut(4) to the handler", n + "FrameOut", {f, 4});
        h.call(kind + "NotifySessionEnd to the handler", n + "SessionEnd", {f});
        h.call(kind + "NotifySessionError(0x8061070a, 0x301) to the handler", n + "SessionError", {f, 0x8061070a, 0x301});
        h.logs(kind + " diagnostics");
        h.w32(f + diag, 0);
    }
    queue(h, "the end");
    h.call("Finalize", "cellSailRecorderFinalize", {kRec});
    finish_spurs(h);
}

// The composer: Initialize, Finalize, the parameters of its streams, its notifications, access units taken and given
// back on streams laid out as libsail_rec lays them out.
void script_composer(H& h)
{
    init_recorder(h);
    const u32 c = kComposer;
    const auto composer_mask = std::initializer_list<std::pair<u32, u32>>{{0x4C, 4}, {0xC0, 4}, {0xCC, 4}, {0xE0, 4},
        {0xEC, 4}};
    for (u32 i = 0; i < 17; i++) h.w32(kCallbacks + 4 * i, kCallbackDesc + 0x10000 * i);
    h.call("ComposerInitialize(NULL funcs)", "cellSailComposerInitialize", {c, 0, 0x30});
    for (const u32 off : {0x34u, 0x38u, 0x3Cu}) {
        h.w32(kCallbacks + off, 1);
        h.call(std::format("ComposerInitialize(+{:#x} set)", off), "cellSailComposerInitialize", {c, kCallbacks, 0x30});
        h.w32(kCallbacks + off, 0);
    }
    h.call("ComposerInitialize", "cellSailComposerInitialize", {c, kCallbacks, 0x30});
    h.mem("ComposerInitialize: the composer", c, 0x140, composer_mask);
    h.w32(c + 0x50, 2);
    h.call("ComposerFinalize in state 2", "cellSailComposerFinalize", {c});
    h.w32(c + 0x50, 1);
    h.call("ComposerFinalize", "cellSailComposerFinalize", {c});
    h.mem("ComposerFinalize: the composer", c, 0x140);
    h.call("ComposerInitialize again", "cellSailComposerInitialize", {c, kCallbacks, 0x31});
    h.call("GetStreamParameter(NULL)", "cellSailComposerGetStreamParameter", {0, kArgs});
    h.call("GetStreamParameter without stream parameters", "cellSailComposerGetStreamParameter", {c, kArgs});
    h.w32(c + 0xA8, kArgs + 0x400);
    std::memset(h.b + kArgs, 0xEE, 0x40);
    h.call("GetStreamParameter", "cellSailComposerGetStreamParameter", {c, kArgs});
    h.call("GetStreamParameter (the composer's high word set)", "cellSailComposerGetStreamParameter",
        {0x100000000ull | c, kArgs});
    h.mem("GetStreamParameter: nothing written", kArgs, 0x40);
    h.call("GetEsUserParameter", "cellSailComposerGetEsUserParameter", {c, 0, kArgs, 0x10});
    // Streams: stream objects {handle, ..., state record at +8}; state records: +4 index, +8 state, +0x18 parameters,
    // +0x68 timescale, +0x6C ring, +0x70 ring size, +0x74 units there, +0x7C next to read.
    const u32 sa = kStreams, sv = kStreams + 0x200, su = kStreams + 0x400; // the composer's stream entries
    const u32 sta = kStreams + 0x800, stv = kStreams + 0xA00, stu = kStreams + 0xC00;
    const u32 units = kStreams + 0x1000; // access units, 0x60 bytes each
    for (u32 i = 0; i < 0x60 * 8; i++) h.b[units + i] = static_cast<u8>(i * 7 + 3);
    for (const auto& [entry, st, table] : std::vector<std::tuple<u32, u32, u32>>{{sa, sta, 0x68}, {sv, stv, 0x78},
             {su, stu, 0x88}}) {
        h.w32(c + table, entry);
        h.w32(entry + 8, st);
        h.w32(st + 4, 0);
        h.w32(st + 8, 10);
        for (u32 i = 0; i < 0x50; i++) h.b[st + 0x18 + i] = static_cast<u8>(0x40 + i);
        h.w32(st + 0x68, 44100);
        h.w32(st + 0x6C, st + 0x100);
        h.w32(st + 0x70, 6);
        for (u32 i = 0; i < 6; i++) h.w32(st + 0x100 + 4 * i, units + 0x60 * i);
        h.w32(st + 0x74, 4);
        h.w32(st + 0x7C, 3);
    }
    h.w32(c + 0x5C, 1);
    h.w32(c + 0x60, 1);
    h.w32(c + 0x64, 1);
    h.w32(stv + 0x68, 90000);
    for (const bool video : {true, false}) {
        const std::string fn = video ? "cellSailComposerGetEsVideoParameter" : "cellSailComposerGetEsAudioParameter";
        const u32 size = video ? 0x50 : 0x48, st = video ? stv : sta;
        // Index -1 reads the word before the table: the video table's is +0x74 (0), the audio one's the user stream
        // count, 0 here (libsail_rec would read a stream at that count).
        h.w32(c + 0x64, 0);
        for (const s64 index : {-1ll, 0ll, 1ll, 3ll, 4ll}) {
            std::memset(h.b + kArgs, 0xEE, 0x60);
            h.call(std::format("{}({})", fn, index), fn, {c, static_cast<u64>(index), kArgs, size});
            h.mem(std::format("{}({}): out", fn, index), kArgs, 0x60);
        }
        h.call(fn + "(NULL)", fn, {0, 0, kArgs, size});
        h.call(fn + "(no out)", fn, {c, 0, 0, size});
        h.call(fn + "(another size)", fn, {c, 0, kArgs, size + 8});
        for (const u32 state : {9u, 11u, 13u, 14u}) {
            h.w32(st + 8, state);
            h.call(std::format("{} in state {}", fn, state), fn, {c, 0, kArgs, size});
        }
        h.w32(st + 8, 10);
        h.w32(c + 0x64, 1);
    }
    // Notifications: the pending call's completion (once), the diagnostic handler, the session error.
    h.w32(c, kRec);
    ps_write64(h.b, c + 0xA0, 0x1234567800000042ull);
    h.call("NotifyCallCompleted(-2)", "cellSailComposerNotifyCallCompleted", {c, static_cast<u64>(-2)});
    h.call("NotifyCallCompleted(3) with none pending", "cellSailComposerNotifyCallCompleted", {c, 3});
    h.call("NotifySessionError(0x80610702, 1)", "cellSailComposerNotifySessionError", {c, 0x80610702, 1});
    h.call("NotifySessionError(5, 0)", "cellSailComposerNotifySessionError", {c, 5, 0});
    h.w32(c + 0xF4, kHandlerDesc);
    h.w32(c + 0xF8, 0x99);
    ps_write64(h.b, c + 0xA0, 0x77);
    h.call("NotifyCallCompleted(4) to the handler", "cellSailComposerNotifyCallCompleted", {c, 4});
    h.w32(c + 0xF4, 0);
    h.logs("composer notifications");
    queue(h, "composer notifications");
    // Access units.
    for (const char* kind : {"User", "Video", "Audio"}) {
        const std::string k(kind);
        const u32 st = k == "User" ? stu : k == "Video" ? stv : sta;
        const auto look = [&](const std::string& key) {
            h.mem(key + ": out", kArgs, 0x200);
            h.mem(key + ": the stream", st, 0x80);
            h.mem(key + ": remaining", kArgs + 0x300, 4);
        };
        for (const auto& [idx, n] : std::vector<std::pair<u64, s64>>{{0, 2}, {1, 1}, {0, 0}, {0, -1}, {0, 5},
                 {0x100000000ull, 1}}) {
            std::memset(h.b + kArgs, 0xEE, 0x400);
            h.w32(kArgs, 0x11111111);
            h.w32(kArgs + 4, 0x22222222);
            const std::string key = std::format("TryGetEs{}Au({:#x}, {})", k, idx, n);
            h.call(key, "cellSailComposerTryGetEs" + k + "Au", {c, idx, kArgs, static_cast<u64>(n), kArgs + 0x300});
            look(key);
            const std::string key2 = std::format("GetEs{}Au({:#x}, {})", k, idx, n);
            h.call(key2, "cellSailComposerGetEs" + k + "Au", {c, idx, kArgs, static_cast<u64>(n), kArgs + 0x300, 1000});
            look(key2);
        }
        h.call(std::format("TryGetEs{}Au(no out)", k), "cellSailComposerTryGetEs" + k + "Au", {c, 0, 0, 1, 0});
        h.call(std::format("TryGetEs{}Au(no out, none)", k), "cellSailComposerTryGetEs" + k + "Au", {c, 0, 0, 0, 0});
        for (const s64 n : {1ll, 2ll, 0ll, -1ll, 9ll}) {
            h.call(std::format("ReleaseEs{}Au({})", k, n), "cellSailComposerReleaseEs" + k + "Au",
                {c, 0, static_cast<u64>(n)});
            h.mem(std::format("ReleaseEs{}Au({}): the stream", k, n), st, 0x80);
        }
        h.call(std::format("ReleaseEs{}Au(index 1)", k), "cellSailComposerReleaseEs" + k + "Au", {c, 1, 1});
        // None there: TryGet takes none; Get waits for one (1 ms) and gives up.
        std::memset(h.b + kArgs, 0xEE, 0x400);
        h.call(std::format("TryGetEs{}Au with none there", k), "cellSailComposerTryGetEs" + k + "Au",
            {c, 0, kArgs, 2, kArgs + 0x300});
        look(std::format("TryGetEs{}Au with none there", k));
        h.call(std::format("GetEs{}Au with none there", k), "cellSailComposerGetEs" + k + "Au",
            {c, 0, kArgs, 2, kArgs + 0x300, 1000});
        look(std::format("GetEs{}Au with none there", k));
        h.call(std::format("GetEs{}Au with none there and none asked", k), "cellSailComposerGetEs" + k + "Au",
            {c, 0, kArgs, 0, kArgs + 0x300, 1000});
        h.w32(st + 0x74, 3);
        h.w32(st + 0x7C, 5);
    }
    queue(h, "access units");
    h.w32(c, 0);
    h.w32(c + 0x50, 1);
    h.call("ComposerFinalize at the end", "cellSailComposerFinalize", {c});
    h.call("Finalize", "cellSailRecorderFinalize", {kRec});
    finish_spurs(h);
}

// Profiles: the stream, audio and video parameters on a profile laid out as libsail_rec's control thread makes one.
void script_profiles(H& h)
{
    init_recorder(h);
    const u32 p = kProfile, pa = kProfile + 0x600, pv = kProfile + 0x900, pu = kProfile + 0x1000;
    std::memset(h.b + p, 0, 0x2000);
    h.w32(p, 1);
    h.w32(p + 0x438, 1);
    h.w32(p + 0x550, pa);
    h.w32(p + 0x554, pv);
    h.w32(p + 0x558, pu);
    h.w32(p + 0x55C, kRec);
    const auto look = [&](const std::string& key) {
        h.mem(key + ": the profile", p, 0x568);
        h.mem(key + ": audio", pa, 0x108 * 2);
        h.mem(key + ": video", pv, 0x220 * 3);
    };
    // Stream parameters.
    const u32 sp1 = kArgs, sp2 = kArgs + 0x40;
    for (const u32 v : {0xFFFFFFFFu, 0xFFFFFu, 0x100000u, 0x200001u}) {
        h.w32(sp1, 8);
        h.w32(sp1 + 4, v);
        h.call(std::format("SetStreamParameter({{8, {:#x}}}, NULL)", v), "cellSailProfileSetStreamParameter",
            {p, sp1, 0});
        look(std::format("SetStreamParameter({:#x})", v));
    }
    h.w32(sp1, 9);
    h.call("SetStreamParameter({9})", "cellSailProfileSetStreamParameter", {p, sp1, 0});
    for (u32 i = 0; i < 0x38; i++) h.b[sp2 + i] = static_cast<u8>(i + 1);
    for (const auto& [type, size, flag] : std::vector<std::tuple<u32, u32, u32>>{{1, 0x38, 0}, {1, 0x38, 1},
             {1, 0x38, 0xFFFFFFFF}, {1, 0x38, 2}, {1, 0xC, 0}, {3, 0xC, 5}, {3, 0x38, 0}, {2, 0xC, 0}}) {
        h.w32(p + 0x438, type);
        h.w32(sp2, size);
        h.w32(sp2 + 4, flag);
        h.call(std::format("SetStreamParameter(NULL, {{{:#x}, {:#x}}}) type {}", size, flag, type),
            "cellSailProfileSetStreamParameter", {p, 0, sp2});
        look(std::format("SetStreamParameter type {} {:#x} {:#x}", type, size, flag));
    }
    h.w32(p + 0x438, 1);
    h.w32(p, 2);
    h.call("SetStreamParameter in use", "cellSailProfileSetStreamParameter", {p, 0, 0});
    h.call("SetEsAudioParameter in use", "cellSailProfileSetEsAudioParameter", {p, 0, 0, 0});
    h.call("SetEsVideoParameter in use", "cellSailProfileSetEsVideoParameter", {p, 0, 0, 0});
    h.w32(p, 1);
    // Audio parameters (0x48 bytes, the fields the game may not set all ones) and the extension (0x84 bytes).
    const u32 ap = kArgs + 0x100, ax = kArgs + 0x200;
    const auto audio_param = [&](s32 codec, s32 rate_a, s32 rate_b, s8 ch, s32 rate, s32 layout) {
        std::memset(h.b + ap, 0xFF, 0x48);
        h.w32(ap, 0x48);
        h.w32(ap + 4, static_cast<u32>(codec));
        h.w32(ap + 0x14, static_cast<u32>(rate_a));
        h.w32(ap + 0x18, static_cast<u32>(rate_b));
        h.w32(ap + 0x1C, 0x1C1C1C1C);
        h.b[ap + 0x31] = static_cast<u8>(ch);
        h.w32(ap + 0x34, static_cast<u32>(rate));
        h.w32(ap + 0x38, static_cast<u32>(layout));
    };
    const auto audio_ext = [&] {
        std::memset(h.b + ax, 0, 0x84);
        h.w32(ax, 0x84);
    };
    h.call("SetEsAudioParameter(index 2)", "cellSailProfileSetEsAudioParameter", {p, 2, 0, 0});
    h.call("SetEsAudioParameter(NULL)", "cellSailProfileSetEsAudioParameter", {p, 1, 0, 0});
    look("SetEsAudioParameter(NULL)");
    audio_param(-1, -1, -1, -1, -1, -1);
    h.call("SetEsAudioParameter(defaults)", "cellSailProfileSetEsAudioParameter", {p, 0, ap, 0});
    look("SetEsAudioParameter(defaults)");
    h.w32(ap, 0x47);
    h.call("SetEsAudioParameter(size 0x47)", "cellSailProfileSetEsAudioParameter", {p, 0, ap, 0});
    audio_param(1, 0, 0, 2, 48000, -1);
    audio_ext();
    h.w32(ax, 0x83);
    h.call("SetEsAudioParameter(extension size 0x83)", "cellSailProfileSetEsAudioParameter", {p, 0, ap, ax});
    for (const u32 off : {8u, 0xCu, 0x10u, 0x20u, 0x24u, 0x28u, 0x30u, 0x32u, 0x3Cu, 0x40u, 0x44u}) {
        audio_param(1, 0, 0, 2, 48000, -1);
        h.b[ap + off] = 0x12;
        h.call(std::format("SetEsAudioParameter(byte {:#x} set)", off), "cellSailProfileSetEsAudioParameter",
            {p, 0, ap, 0});
    }
    struct Audio {
        s32 codec, a, b;
        s8 ch;
        s32 rate, layout;
    };
    for (const Audio& c : std::vector<Audio>{{1, 0, 0, 2, 48000, -1}, {1, 0, 0, 1, 44100, 1}, {1, 0, 0, 2, 44100, 1},
             {1, 0, 0, 3, 48000, -1}, {1, 0, 0, -1, -1, -1}, {1, 0, 0, 0, 0, -1}, {1, 0, 0, 2, 22050, -1},
             {1, 0, 0, 2, 24000, -1}, {1, 0, 0, 2, 32000, -1}, {1, 0, 0, 2, 16000, -1}, {1, 0, 0, 2, 96000, -1},
             {1, 96000, 96000, 2, 48000, -1}, {1, 96000, 128000, 2, 48000, -1}, {1, 20000, 20000, 2, 48000, -1},
             {1, 288000, 288000, 2, 48000, -1}, {1, 288001, 288001, 2, 48000, -1}, {1, 64000, 64000, 2, -1, -1},
             {1, -5, 0, 2, 48000, -1}, {1, 0, 7, 1, 32000, -1}, {2, 0, 0, 2, 48000, -1}, {0, 0, 0, 2, 48000, -1}}) {
        audio_param(c.codec, c.a, c.b, c.ch, c.rate, c.layout);
        const std::string k = std::format("SetEsAudioParameter(codec {}, rates {} {}, {} ch, {} Hz, layout {})",
            c.codec, c.a, c.b, static_cast<int>(c.ch), c.rate, c.layout);
        h.call(k, "cellSailProfileSetEsAudioParameter", {p, 1, ap, 0});
        look(k);
    }
    // The extension's fields, each set in turn.
    audio_param(1, 0, 0, 2, 48000, -1);
    audio_ext();
    h.call("SetEsAudioParameter(the default extension)", "cellSailProfileSetEsAudioParameter", {p, 0, ap, ax});
    look("SetEsAudioParameter(the default extension)");
    for (u32 off = 4; off < 0x84; off += 4) {
        audio_ext();
        h.w32(ax + off, off == 0x10 ? 0x3F800000 : off == 0x18 || off == 0x20 ? 2 : 1);
        h.call(std::format("SetEsAudioParameter(extension word {:#x} set)", off), "cellSailProfileSetEsAudioParameter",
            {p, 0, ap, ax});
        h.mem(std::format("SetEsAudioParameter(extension word {:#x} set): audio", off), pa, 0x108);
    }
    audio_ext();
    h.w32(ax + 0x10, 0x80000000); // -0.0
    h.call("SetEsAudioParameter(extension float -0)", "cellSailProfileSetEsAudioParameter", {p, 0, ap, ax});
    h.w32(ax + 0x10, 0x7FC00000); // NaN
    h.call("SetEsAudioParameter(extension float NaN)", "cellSailProfileSetEsAudioParameter", {p, 0, ap, ax});
    // Video parameters (0x50 bytes) and the extension {16, 0, options, count}.
    const u32 vp = kArgs + 0x300, vx = kArgs + 0x380, opts = kArgs + 0x400;
    const auto video_param = [&](s32 codec, u8 rate_code) {
        std::memset(h.b + vp, 0xFF, 0x50);
        h.w32(vp, 0x50);
        h.w32(vp + 4, static_cast<u32>(codec));
        h.w32(vp + 0xC, 1280);
        h.w32(vp + 0x10, 720);
        h.w32(vp + 0x14, 0x14141414);
        h.w32(vp + 0x18, 0x18181818);
        h.w32(vp + 0x1C, 90000);
        h.w32(vp + 0x20, 0x20202020);
        h.b[vp + 0x33] = rate_code;
        h.b[vp + 0x34] = 0x34;
        h.b[vp + 0x41] = 0x41;
    };
    h.call("SetEsVideoParameter(index 3)", "cellSailProfileSetEsVideoParameter", {p, 3, 0, 0});
    h.call("SetEsVideoParameter(NULL)", "cellSailProfileSetEsVideoParameter", {p, 2, 0, 0});
    look("SetEsVideoParameter(NULL)");
    video_param(0x22, 0xFF);
    h.w32(vp, 0x51);
    h.call("SetEsVideoParameter(size 0x51)", "cellSailProfileSetEsVideoParameter", {p, 0, vp, 0});
    for (const u32 off : {8u, 0x24u, 0x28u, 0x30u, 0x31u, 0x32u, 0x38u, 0x3Cu, 0x40u, 0x42u, 0x43u, 0x48u}) {
        video_param(0x22, 0xFF);
        h.b[vp + off] = 0x12;
        h.call(std::format("SetEsVideoParameter(byte {:#x} set)", off), "cellSailProfileSetEsVideoParameter",
            {p, 0, vp, 0});
    }
    for (const s32 codec : {-1, 0, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25}) {
        for (const u8 code : {u8{0xFF}, u8{0}, u8{3}, u8{6}, u8{7}, u8{8}}) {
            video_param(codec, code);
            const std::string k = std::format("SetEsVideoParameter(codec {:#x}, rate code {})", codec, code);
            h.call(k, "cellSailProfileSetEsVideoParameter", {p, 1, vp, 0});
            h.mem(k + ": video", pv + 0x220, 0x220);
        }
    }
    // Options.
    const auto ext = [&](u32 size, u32 zero, u32 options, u32 n) {
        h.w32(vx, size);
        h.w32(vx + 4, zero);
        h.w32(vx + 8, options);
        h.w32(vx + 12, n);
    };
    const u32 avc_ids[] = {0x3E83, 0xA781, 0x1E02, 0x3D94}, m4v_ids[] = {0x7ED4, 0xFA03};
    for (u32 i = 0; i < 40; i++) {
        h.w32(opts + 8 * i, avc_ids[i % 4]);
        h.w32(opts + 8 * i + 4, 0x100 + i);
    }
    for (const s32 codec : {-1, 0x21, 0x22, 0x23, 0x24}) {
        struct X {
            u32 size, zero, options, n;
        };
        for (const X& x : std::vector<X>{{16, 0, 0, 0}, {16, 0, opts, 0}, {16, 0, opts, 4}, {16, 0, opts, 32},
                 {16, 0, opts, 33}, {15, 0, opts, 1}, {16, 1, opts, 1}}) {
            video_param(codec, 2);
            ext(x.size, x.zero, x.options, x.n);
            const std::string k = std::format("SetEsVideoParameter(codec {:#x}, extension {} {} {} {})", codec,
                x.size, x.zero, x.options ? "options" : "none", x.n);
            h.call(k, "cellSailProfileSetEsVideoParameter", {p, 0, vp, vx});
            h.mem(k + ": video", pv, 0x220);
        }
    }
    for (u32 i = 0; i < 3; i++) h.w32(opts + 8 * i, m4v_ids[i % 2]);
    video_param(0x21, 4);
    ext(16, 0, opts, 3);
    h.call("SetEsVideoParameter(MPEG-4, its options)", "cellSailProfileSetEsVideoParameter", {p, 2, vp, vx});
    h.mem("SetEsVideoParameter(MPEG-4, its options): video", pv + 0x440, 0x220);
    h.w32(opts + 8, 0x3E83);
    h.call("SetEsVideoParameter(MPEG-4, an AVC option)", "cellSailProfileSetEsVideoParameter", {p, 2, vp, vx});
    video_param(0x22, 4);
    h.call("SetEsVideoParameter(AVC, an MPEG-4 option)", "cellSailProfileSetEsVideoParameter", {p, 2, vp, vx});
    look("the end");
    h.call("Finalize", "cellSailRecorderFinalize", {kRec});
    finish_spurs(h);
}

// Video converters: the queue of requests and results on a converter laid out as libsail_rec's control thread makes
// one (its lock and conditions made here), busy with a request, so nothing more starts converting.
void script_converter(H& h)
{
    const u32 v = kConv;
    std::memset(h.b + v, 0, 0x200);
    const u32 attr = kArgs;
    ps_write32(h.b, attr, 2);
    ps_write32(h.b, attr + 4, 0x20);
    std::memcpy(h.b + attr + 8, "_saicv0", 8);
    REQUIRE(h.t.call("sys_lwmutex_create", v + 0x30, attr) == 0);
    std::memcpy(h.b + attr + 0x10, "_saicv1", 8);
    REQUIRE(h.t.call("sys_lwcond_create", v + 0x48, v + 0x30, attr + 0x10) == 0);
    REQUIRE(h.t.call("sys_lwcond_create", v + 0x50, v + 0x30, attr + 0x10) == 0);
    h.w32(v + 0x58, 1);
    h.w32(v + 0x5C, 3);
    const auto look = [&](const std::string& key) {
        h.mem(key + ": the converter", v, 0x100, {{0x40, 4}, {0x4C, 4}, {0x54, 4}});
        h.mem(key + ": out", kArgs + 0x100, 8);
    };
    h.call("CanGetResult", "cellSailVideoConverterCanGetResult", {v});
    h.call("CanProcess", "cellSailVideoConverterCanProcess", {v});
    for (u32 i = 0; i < 3; i++) {
        h.call(std::format("Process {}", i), "cellSailVideoConverterProcess",
            {v, 0x1000 + i, 0x2000 + i, 0x3000 + i, 0x4000 + i, 0, 0x5000 + i});
        look(std::format("Process {}", i));
    }
    h.call("CanProcess when full", "cellSailVideoConverterCanProcess", {v});
    h.call("Process when full", "cellSailVideoConverterProcess", {v, 1, 2, 3, 4, 0, 5});
    h.call("Process when full, a wait", "cellSailVideoConverterProcess", {v, 1, 2, 3, 4, 1000, 5});
    look("Process when full");
    ps_write64(h.b, kArgs + 0x100, 0xEEEEEEEEEEEEEEEEull);
    h.call("GetResult with none", "cellSailVideoConverterGetResult", {v, 0, kArgs + 0x100, kArgs + 0x104});
    h.call("GetResult with none, a wait", "cellSailVideoConverterGetResult", {v, 1000, kArgs + 0x100, kArgs + 0x104});
    look("GetResult with none");
    // Results as the conversion leaves them.
    h.w32(v + 0x64, 2);
    h.w32(v + 0x74, 2);
    for (u32 i = 0; i < 3; i++) {
        h.w32(v + 0xA8 + 8 * i, 0x6000 + i);
        h.w32(v + 0xAC + 8 * i, 0x7000 + i);
    }
    h.call("CanGetResult with two", "cellSailVideoConverterCanGetResult", {v});
    for (u32 i = 0; i < 3; i++) {
        h.call(std::format("GetResult {}", i), "cellSailVideoConverterGetResult", {v, 0, kArgs + 0x100, kArgs + 0x104});
        look(std::format("GetResult {}", i));
    }
}

// ------------------------------------------------------------------------------------------------- booted

// Feeder k's (0 to 3) callbacks: the recording callback, named by its TOC (its answer word), each answering 0.
void tagged_feeders(H& h)
{
    for (u32 k = 0; k < 4; k++)
        for (u32 i = 0; i < 11; i++) {
            const u32 n = k * 11 + i, d = kBootDescs + 8 * n;
            h.w32(d, kCode + 0x500);
            h.w32(d + 4, kBootAnswers + 4 * n);
            h.w32(kBootAnswers + 4 * n, 0);
            h.w32(kBoot + 0x40 * k + 4 * i, d);
        }
}
u32 answer_of(u32 feeder, u32 callback) { return kBootAnswers + 4 * (feeder * 11 + callback); }
u32 table_of(u32 feeder) { return kBoot + 0x40 * feeder; }
// The feeders: audio i is feeder i, video j feeder 2 + j.
u32 feeder_at(u32 k) { return kFeeders + 0x200 * k; }

// Waits (10 s at most) for the handler's records to reach `n`, or for a record of `event` from the next one on.
void wait_records(H& h, u32 n, const std::string& what)
{
    for (int i = 0; i < 10000 && h.r32(kEvents) < n; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (h.r32(kEvents) < n) FAIL(what << ": " << h.r32(kEvents) << " of the handler's records, not " << n);
}
void wait_event(H& h, u64 event, const std::string& what)
{
    for (int i = 0; i < 10000; i++) {
        for (u32 k = h.events_seen; k < h.r32(kEvents); k++)
            if (ps_read64(h.b, kEvents + 16 + 32 * (k & kEventRecs) + 8) == event) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    FAIL(what << ": no event " << std::format("{:#x}", event));
}
// Waits for the control thread to wait for its next message (+0x510 200).
void wait_idle(H& h, const std::string& what)
{
    for (int i = 0; i < 10000 && h.r32(kRec + 0x510) != 200; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (h.r32(kRec + 0x510) != 200) FAIL(what << ": the control thread does not wait for a message");
}

// A booted recorder's bytes: also masked the threads' IDs, the message the control thread is at (its timing), the SPURS
// queue's pop index (marked while the task handler waits) and its lv2 event queue, the clock's lwmutex's sleep queue.
void booted(H& h, const std::string& key)
{
    h.mem(key + ": the recorder", kRec, 0x2000,
        {{0x4A0, 8}, {0x4BC, 8}, {0x4F0, 4}, {0x4FC, 4}, {0x510, 4}, {0x518, 8}, {0x580, 4}, {0x5F0, 8}, {0x730, 4},
            {0x7AC, 4}, {0x7B4, 4 * 5}, {0x7D0, 4 * 3}, {0x870, 16}});
}
// The same once SPU tasks have sent messages: each message left in the SPURS queue's ring keeps its third word, which
// for a 4:2:2 conversion is its SPU program's run time.
void booted_after_tasks(H& h, const std::string& key)
{
    std::vector<u8> v(h.b + kRec, h.b + kRec + 0x2000);
    for (const auto& [off, n] : std::vector<std::pair<u32, u32>>{{0x4A0, 8}, {0x4BC, 8}, {0x4F0, 4}, {0x4FC, 4},
             {0x510, 4}, {0x518, 8}, {0x580, 4}, {0x5F0, 8}, {0x730, 4}, {0x7AC, 4}, {0x7B4, 4 * 5}, {0x7D0, 4 * 3},
             {0x870, 16}})
        for (u32 i = 0; i < n; i++) v[off + i] = 0x5A;
    for (u32 e = 0; e < 16; e++)
        for (u32 i = 0; i < 4; i++) v[0x608 + 16 * e + i] = 0x5A;
    h.o.bytes(key + ": the recorder", std::move(v));
}
void feeders(H& h, const std::string& key, u32 n)
{
    for (u32 k = 0; k < n; k++) h.mem(std::format("{}: feeder {}", key, k), feeder_at(k), 0x200, {{0, 0x2C}});
}

// One recorder booted: mode, 2 audio and 2 video streams (feeders 0 to 3), 1 user stream; `fail` the feeder whose makeup
// answers `answer` (none: 4); `cleanup` false: feeder 1 and 3 without a cleanup callback.
void boot_round(H& h, const std::string& k, u32 mode, u32 fail, u32 answer, bool cleanup, bool composer = false)
{
    tagged_feeders(h);
    if (fail < 4) h.w32(answer_of(fail, 0), answer);
    if (!cleanup) {
        h.w32(table_of(1) + 4, 0);
        h.w32(table_of(3) + 4, 0);
    }
    attribute(h, mode, 2, 2, 1);
    REQUIRE(h.call(k + ": Initialize", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 0x77, kAttr, kRes}) ==
            0);
    for (u32 i = 0; i < 4; i++) {
        const bool video = i >= 2;
        const std::string kind = video ? "Video" : "Audio";
        h.call(std::format("{}: Feeder{}Initialize({})", k, kind, i), "cellSailFeeder" + kind + "Initialize",
            {feeder_at(i), table_of(i), 0x100 + i, 0});
        h.call(std::format("{}: SetFeeder{}({})", k, kind, i), "cellSailRecorderSetFeeder" + kind,
            {kRec, video ? i - 2 : i, feeder_at(i)});
    }
    if (composer) h.call(k + ": RegisterComposer(3)", "cellSailRecorderRegisterComposer", {kRec, 3, kComposer});
    h.call(k + ": Boot", "cellSailRecorderBoot", {kRec, 0x99});
    wait_event(h, u64{2} << 32 | 0x20, k + ": Boot");
    if (fail < 4) {
        for (int i = 0; i < 10000 && h.r32(kRec + 0x704) != 10; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
        wait_idle(h, k + ": Boot");
    }
    h.o.line(k + ": Boot: the state", std::format("{}", h.r32(kRec + 0x704)));
    booted(h, k + ": Boot");
    h.block(k + ": Boot");
    feeders(h, k + ": Boot", 4);
    h.logs(k + ": Boot");
}

// Boot, the profiles, Cancel and Finalize on a booted recorder; Boot failing at a feeder's makeup; a recorder without a
// clock (mode 1).
void script_boot(H& h)
{
    h.tr.lightweight = false;
    make_spurs(h);
    boot_round(h, "mode 0", 0, 4, 0, true, true);
    const u32 out = kArgs + 0x400;
    h.w32(out, 0xEEEEEEEE);
    h.call("CreateProfile(1)", "cellSailRecorderCreateProfile", {kRec, 1, out});
    const u32 p1 = h.r32(out);
    h.o.line("CreateProfile(1): the profile's place", std::format("{:#x}", p1 - kHeap));
    if (p1 >= kHeap && p1 < kHeapEnd) h.mem("CreateProfile(1): the profile", p1, 0x1000 + (2 * 2 + 2 + 1) * 0x200);
    h.logs("CreateProfile(1)");
    booted(h, "CreateProfile(1)");
    h.call("CreateProfile(3)", "cellSailRecorderCreateProfile", {kRec, 3, out});
    const u32 p2 = h.r32(out);
    if (p2 >= kHeap && p2 < kHeapEnd) h.mem("CreateProfile(3): the profile", p2, 0x1000 + (2 * 2 + 2 + 1) * 0x200);
    h.logs("CreateProfile(3)");
    h.w32(kHeapState + 4, 1);
    h.w32(out, 0xEEEEEEEE);
    h.call("CreateProfile with the allocator failing", "cellSailRecorderCreateProfile", {kRec, 1, out});
    h.mem("CreateProfile with the allocator failing: the out word", out, 4);
    h.logs("CreateProfile with the allocator failing");
    h.call("CreateProfile(1) a third time", "cellSailRecorderCreateProfile", {kRec, 1, out});
    const u32 p3 = h.r32(out);
    h.logs("CreateProfile(1) a third time");
    h.call("DestroyProfile(the first)", "cellSailRecorderDestroyProfile", {kRec, p1});
    h.logs("DestroyProfile(the first)");
    if (p1 >= kHeap && p1 < kHeapEnd) h.mem("DestroyProfile(the first): its first 4 KB", p1, 0x1000);
    h.call("DestroyProfile(the first) again", "cellSailRecorderDestroyProfile", {kRec, p1});
    h.call("DestroyProfile(not a profile)", "cellSailRecorderDestroyProfile", {kRec, kArgs + 0x800});
    h.logs("DestroyProfile(not a profile)");
    booted(h, "the profiles");
    // Cancel: each feeder's callback 6 (asynchronous: the handler's records awaited).
    const u32 before = h.r32(kEvents);
    h.call("Cancel", "cellSailRecorderCancel", {kRec});
    wait_records(h, before + 4, "Cancel");
    h.logs("Cancel");
    h.w32(table_of(0) + 4 * 6, 0);
    h.w32(table_of(3) + 4 * 6, 0);
    h.call("FeederAudioInitialize(0) with no callback 6", "cellSailFeederAudioInitialize", {feeder_at(0), table_of(0), 0x200, 0});
    h.call("FeederVideoInitialize(1) with no callback 6", "cellSailFeederVideoInitialize", {feeder_at(3), table_of(3), 0x203, 0});
    h.call("Cancel with two feeders without callback 6", "cellSailRecorderCancel", {kRec});
    wait_records(h, before + 6, "Cancel with two feeders without callback 6");
    h.logs("Cancel with two feeders without callback 6");
    (void)p3;
    // Finalize: the control thread ends (the feeders cleaned up, the profiles freed), the recorder taken down.
    h.call("Finalize", "cellSailRecorderFinalize", {kRec});
    h.mem("Finalize: the recorder", kRec, 0x2000, {{0x4A0, 8}, {0x4BC, 8}, {0x4F0, 4}, {0x4FC, 4}, {0x518, 8}, {0x580, 4},
        {0x5F0, 8}, {0x7AC, 4}, {0x7B4, 4 * 5}, {0x7D0, 4 * 3}, {0x870, 16}});
    feeders(h, "Finalize", 4);
    h.logs("Finalize");
    h.call("Finalize again", "cellSailRecorderFinalize", {kRec});

    // Boot failing: audio feeder 1's makeup, then video feeder 0's (feeder 2), each answering an error.
    boot_round(h, "audio 1's makeup failing", 0, 1, 0x80610777, true);
    h.call("audio 1's makeup failing: Finalize", "cellSailRecorderFinalize", {kRec});
    h.logs("audio 1's makeup failing: Finalize");
    boot_round(h, "video 0's makeup failing", 0, 2, 0xFFFFFFFE, false);
    h.call("video 0's makeup failing: Finalize", "cellSailRecorderFinalize", {kRec});
    h.logs("video 0's makeup failing: Finalize");
    boot_round(h, "audio 0's makeup failing, mode 1", 1, 0, 0x80000000, true);
    h.call("audio 0's makeup failing, mode 1: Finalize", "cellSailRecorderFinalize", {kRec});
    // A recorder without a clock (mode 1), feeders 1 and 3 without a cleanup.
    boot_round(h, "mode 1", 1, 4, 0, false);
    h.call("mode 1: CreateProfile(1)", "cellSailRecorderCreateProfile", {kRec, 1, out});
    h.logs("mode 1: CreateProfile(1)");
    h.call("mode 1: Finalize", "cellSailRecorderFinalize", {kRec});
    feeders(h, "mode 1: Finalize", 4);
    h.logs("mode 1: Finalize");
    finish_spurs(h);
}


// ------------------------------------------------------------------------------------------------- video converters

constexpr u32 kPictures = kData + 0x100000;  // the converters' source pictures and results (960 KB)
constexpr u32 kConvDesc = kBoot + 0x400;     // a converter's callback: the recording callback, named kBoot + 0x480

// A booted recorder (1 audio, 1 video stream) for the converters.
void boot_for_converters(H& h)
{
    tagged_feeders(h);
    attribute(h, 0, 1, 1, 0);
    REQUIRE(h.call("Initialize", "cellSailRecorderInitialize", {kRec, kAllocObj, kHandlerDesc, 0x77, kAttr, kRes}) == 0);
    h.call("FeederAudioInitialize", "cellSailFeederAudioInitialize", {feeder_at(0), table_of(0), 0x100, 0});
    h.call("SetFeederAudio", "cellSailRecorderSetFeederAudio", {kRec, 0, feeder_at(0)});
    h.call("FeederVideoInitialize", "cellSailFeederVideoInitialize", {feeder_at(2), table_of(2), 0x102, 0});
    h.call("SetFeederVideo", "cellSailRecorderSetFeederVideo", {kRec, 0, feeder_at(2)});
    h.call("Boot", "cellSailRecorderBoot", {kRec, 0x99});
    wait_event(h, u64{2} << 32 | 0x20, "Boot");
    wait_idle(h, "Boot");
    h.logs("Boot");
}

// The conversion's kind by the codings (libsail_rec's): a 4:2:2 coding (2, 4, 9, 10) to ARGB or RGBA (0, 1) is kind 0,
// raw Bayer (12) to ARGB or RGBA kind 1 and to a 4:2:2 coding kind 2 (each an SPU task of libsail_rec's), JPEG (11)
// kind 3 (the firmware's cellAvchatJpgDec).
int conversion_kind(s8 cin, s8 cout)
{
    if (cin == 11) return 3;
    if (cin == 12) return cout == 0 || cout == 1 ? 1 : 2;
    return 0;
}

// A converter's bytes, its work area's and its requests' (the sync objects' sleep queues and libsail_rec's own
// callback masked). What is each run's own: kind 0's SPU program answers its own run time in decrementer ticks (its
// results' words), and the SPU task a request gets has the ID of the previous one or the next by whether that task has
// ended when the next request starts (each SPU kind's task ID in its work area). The JPEG decoder's work area is the
// firmware decoder's own state (its pointers and its SPURS objects), compared by its first 0x40 bytes only.
void converter(H& h, const std::string& key, u32 conv, u32 work_size, bool own_callback, int kind)
{
    std::initializer_list<std::pair<u32, u32>> mask = {{0x40, 4}, {0x4C, 4}, {0x54, 4}};
    std::vector<std::pair<u32, u32>> m(mask);
    if (own_callback) m.push_back({0x24, 4});
    if (kind == 0) m.insert(m.end(), {{0xAC, 4}, {0xB4, 4}});
    std::vector<u8> v(h.b + conv, h.b + conv + 0x100);
    for (const auto& [off, n] : m)
        for (u32 i = 0; i < n; i++) v[off + i] = 0x5A;
    h.o.bytes(key + ": the converter", std::move(v));
    if (!work_size) return;
    if (kind == 3) {
        h.mem(key + ": the work area's start", conv - work_size, std::min<u32>(work_size, 0x40));
        return;
    }
    const u32 id = kind == 0 ? 0x140 : 0x50;
    h.mem(key + ": the work area", conv - work_size, work_size, {{id, 8}});
}

void picture(H& h, u32 at, u32 size, u32 seed)
{
    for (u32 i = 0; i < size; i++) h.b[at + i] = static_cast<u8>((i * 7 + (i >> 8) * 13 + seed) & 0xFF);
}

// GetResult's two words: the result (kind 0's a time, a note) and the request's tag.
void result_words(H& h, const std::string& key, int kind)
{
    if (kind == 0) {
        h.o.note(std::format("{}: the result (the SPU program's run time) {:#x}", key, h.r32(kArgs + 0x3A0)));
        h.mem(key + ": the tag", kArgs + 0x3A4, 4);
        return;
    }
    h.mem(key, kArgs + 0x3A0, 8);
}

// One conversion pair: CreateVideoConverter, two pictures processed (queued together when the capacity allows),
// their results, the converter's callback, DestroyVideoConverter. A JPEG's pictures are two JPEG files made here.
void convert_round(H& h, const std::string& k, u32 capacity, s8 cin, s8 cout, s16 w, s16 hgt, s8 scan, s8 order,
    bool callback)
{
    const int kind = conversion_kind(cin, cout);
    const u32 in = kArgs + 0x300, out = kArgs + 0x340, convp = kArgs + 0x380;
    std::memset(h.b + in, 0xFF, 0x20);
    std::memset(h.b + out, 0xFF, 0x20);
    h.b[in] = static_cast<u8>(cin);
    ps_write16(h.b, in + 4, static_cast<u16>(w));
    ps_write16(h.b, in + 6, static_cast<u16>(hgt));
    h.b[in + 0x10] = static_cast<u8>(scan);
    h.b[in + 0x12] = static_cast<u8>(order);
    h.b[out] = static_cast<u8>(cout);
    h.b[out + 0x10] = 1;
    h.b[out + 0x12] = 2;
    h.w32(convp, 0xEEEEEEEE);
    h.w32(kConvDesc, kCode + 0x500);
    h.w32(kConvDesc + 4, kBoot + 0x480);
    h.w32(kBoot + 0x480, 0);
    const u64 r = h.call(k + ": CreateVideoConverter", "cellSailRecorderCreateVideoConverter",
        {kRec, capacity, in, out, 0, callback ? u64{kConvDesc} : 0, callback ? u64{0x4321} : 0, convp});
    h.logs(k + ": CreateVideoConverter");
    const u32 conv = h.r32(convp);
    if (static_cast<u32>(r) != 0) return;
    const u32 bytes = static_cast<u32>(w) * static_cast<u32>(hgt) * (cout == 0 || cout == 1 ? 4 : 2);
    const u32 src0 = kPictures, src1 = kPictures + 0x40000, dst0 = kPictures + 0x80000, dst1 = kPictures + 0xC0000;
    u32 in0 = static_cast<u32>(w) * static_cast<u32>(hgt) * 4, in1 = in0;
    if (kind == 3) {
        // Two JPEG files of the picture's size: 4:2:0 and 4:2:2 chroma.
        namespace jpeg = pspocket::test::jpeg;
        const std::vector<u8> j0 =
            jpeg::encode(jpeg::pattern(static_cast<u32>(w), static_cast<u32>(hgt), false, 2, 2, 1));
        const std::vector<u8> j1 =
            jpeg::encode(jpeg::pattern(static_cast<u32>(w), static_cast<u32>(hgt), false, 2, 1, 7));
        REQUIRE(j0.size() < 0x40000);
        REQUIRE(j1.size() < 0x40000);
        std::memcpy(h.b + src0, j0.data(), j0.size());
        std::memcpy(h.b + src1, j1.data(), j1.size());
        in0 = static_cast<u32>(j0.size());
        in1 = static_cast<u32>(j1.size());
    } else {
        picture(h, src0, in0, 1);
        picture(h, src1, in1, 77);
    }
    std::memset(h.b + dst0, 0xEE, bytes + 0x100);
    std::memset(h.b + dst1, 0xEE, bytes + 0x100);
    const u32 work = h.r32(conv + 0x1C);
    converter(h, k + ": made", conv, conv - work, !callback, kind);
    h.call(k + ": CanProcess", "cellSailVideoConverterCanProcess", {conv});
    const u32 before = h.r32(kEvents);
    h.call(k + ": Process 1", "cellSailVideoConverterProcess", {conv, src0, in0, dst0, bytes, 0, 0x71});
    const bool both = capacity >= 2;
    if (both) h.call(k + ": Process 2", "cellSailVideoConverterProcess", {conv, src1, in1, dst1, bytes, 0, 0x72});
    for (u32 n = 1; n <= (both ? 2u : 1u); n++) {
        for (int i = 0; i < 20000 && h.r32(conv + 0x64) < (both ? 2u : n); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!both) break;
    }
    const u32 results = h.r32(conv + 0x64);
    h.o.line(k + ": results", std::format("{}", results));
    for (u32 n = 0; n < results; n++) {
        h.w32(kArgs + 0x3A0, 0xEEEEEEEE);
        h.w32(kArgs + 0x3A4, 0xEEEEEEEE);
        h.call(std::format("{}: GetResult {}", k, n), "cellSailVideoConverterGetResult",
            {conv, 0, kArgs + 0x3A0, kArgs + 0x3A4});
        result_words(h, std::format("{}: GetResult {}: the words", k, n), kind);
    }
    if (!both) {
        h.call(k + ": Process 2", "cellSailVideoConverterProcess", {conv, src1, in1, dst1, bytes, 0, 0x72});
        for (int i = 0; i < 20000 && h.r32(conv + 0x64) < 1; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        h.call(k + ": GetResult 2", "cellSailVideoConverterGetResult", {conv, 0, kArgs + 0x3A0, kArgs + 0x3A4});
        result_words(h, k + ": GetResult 2: the words", kind);
    }
    if (callback) wait_records(h, before + 2, k + ": the callbacks");
    else wait_idle(h, k + ": the callbacks");
    h.call(k + ": GetResult with none", "cellSailVideoConverterGetResult", {conv, 0, kArgs + 0x3A0, kArgs + 0x3A4});
    h.mem(k + ": picture 1", dst0, bytes + 0x100);
    h.mem(k + ": picture 2", dst1, bytes + 0x100);
    converter(h, k + ": done", conv, conv - work, !callback, kind);
    h.logs(k + ": converted");
    h.call(k + ": DestroyVideoConverter", "cellSailRecorderDestroyVideoConverter", {kRec, conv});
    h.logs(k + ": DestroyVideoConverter");
}

// Video converters on a booted recorder: each SPU kind (the 4:2:2 codings to ARGB and RGBA with the scans, raw Bayer to
// ARGB, RGBA and the 4:2:2 codings), the JPEG decoder's (JPEG to RGBA, YUYV and VYUY), one or two requests queued, with
// and without a callback; converters refused (the allocator failing, nothing queued with a capacity of 0), one
// destroyed that is not there; the end of a recorder holding converters.
void script_converters(H& h)
{
    h.tr.lightweight = false;
    std::memset(h.b + kPictures, 0, 0x100000);
    make_spurs(h);
    boot_for_converters(h);
    convert_round(h, "4 to 1", 2, 4, 1, 64, 32, 1, 2, true);
    convert_round(h, "9 to 1, interlaced", 1, 9, 1, 64, 32, 0, 1, false);
    convert_round(h, "4 to 1, interlaced other field", 2, 4, 1, 64, 32, 0, 0, true);
    convert_round(h, "4 to 1, 720 lines", 1, 4, 1, 32, 720, 2, 2, true);
    convert_round(h, "2 to 0", 2, 2, 0, 64, 32, 1, 2, false);
    convert_round(h, "10 to 0, interlaced", 1, 10, 0, 64, 32, 0, 1, true);
    convert_round(h, "12 to 1", 2, 12, 1, 64, 32, 1, 2, true);
    convert_round(h, "12 to 0", 1, 12, 0, 64, 32, 1, 2, false);
    convert_round(h, "12 to 4", 2, 12, 4, 64, 32, 1, 2, false);
    convert_round(h, "12 to 9", 1, 12, 9, 64, 32, 1, 2, true);
    convert_round(h, "12 to 2", 1, 12, 2, 64, 32, 1, 2, true);
    convert_round(h, "12 to 10", 2, 12, 10, 64, 32, 1, 2, false);
    convert_round(h, "11 to 1", 2, 11, 1, 64, 32, 1, 2, true);
    convert_round(h, "11 to 4", 1, 11, 4, 64, 32, 1, 2, false);
    convert_round(h, "11 to 9", 2, 11, 9, 48, 16, 1, 2, true);
    // Refusals: the allocator failing for an SPU kind (the common taskset refused; the taskset made and the block
    // refused, the taskset then kept with its user counted, as libsail_rec leaves it) and for a JPEG one (the decoder's
    // module loaded for its size and unloaded again); a converter with no room (capacity 0).
    for (const auto& [name, cin, cout, fail] : std::vector<std::tuple<std::string, s8, s8, u32>>{
             {"12 to 1, the taskset refused", 12, 1, 1}, {"11 to 1, the block refused", 11, 1, 1},
             {"12 to 1, the block refused", 12, 1, 2}}) {
        const u32 in = kArgs + 0x300, out = kArgs + 0x340;
        std::memset(h.b + in, 0xFF, 0x20);
        std::memset(h.b + out, 0xFF, 0x20);
        h.b[in] = static_cast<u8>(cin);
        ps_write16(h.b, in + 4, 64);
        ps_write16(h.b, in + 6, 32);
        h.b[out] = static_cast<u8>(cout);
        h.w32(kArgs + 0x380, 0xEEEEEEEE);
        h.w32(kHeapState + 4, fail);
        h.call(name, "cellSailRecorderCreateVideoConverter", {kRec, 1, in, out, 0, 0, 0, kArgs + 0x380});
        h.w32(kHeapState + 4, 0);
        h.mem(name + ": the out word", kArgs + 0x380, 4);
        h.logs(name);
        booted_after_tasks(h, name);
    }
    {
        const u32 in = kArgs + 0x300, out = kArgs + 0x340;
        std::memset(h.b + in, 0xFF, 0x20);
        std::memset(h.b + out, 0xFF, 0x20);
        h.b[in] = 4;
        ps_write16(h.b, in + 4, 64);
        ps_write16(h.b, in + 6, 32);
        h.b[out] = 1;
        h.call("capacity 0", "cellSailRecorderCreateVideoConverter", {kRec, 0, in, out, 0, 0, 0, kArgs + 0x380});
        const u32 conv = h.r32(kArgs + 0x380);
        h.call("capacity 0: CanProcess", "cellSailVideoConverterCanProcess", {conv});
        h.call("capacity 0: Process", "cellSailVideoConverterProcess", {conv, kPictures, 0x100, kPictures + 0x80000,
            0x100, 0, 1});
        h.call("capacity 0: CanGetResult", "cellSailVideoConverterCanGetResult", {conv});
        h.logs("capacity 0");
        h.call("capacity 0: DestroyVideoConverter", "cellSailRecorderDestroyVideoConverter", {kRec, conv});
        h.call("capacity 0: DestroyVideoConverter again", "cellSailRecorderDestroyVideoConverter", {kRec, conv});
        h.logs("capacity 0: DestroyVideoConverter");
    }
    // Two converters left for the recorder's end.
    const u32 in = kArgs + 0x300, out = kArgs + 0x340;
    for (const u32 p : {kArgs + 0x3C0u, kArgs + 0x3C4u}) {
        std::memset(h.b + in, 0xFF, 0x20);
        std::memset(h.b + out, 0xFF, 0x20);
        h.b[in] = 12;
        ps_write16(h.b, in + 4, 64);
        ps_write16(h.b, in + 6, 32);
        h.b[out] = 1;
        h.call("CreateVideoConverter left for the end", "cellSailRecorderCreateVideoConverter",
            {kRec, 2, in, out, 0, 0, 0, p});
    }
    h.logs("two converters left");
    h.call("Finalize", "cellSailRecorderFinalize", {kRec});
    h.logs("Finalize");
    finish_spurs(h);
}

Observed half_initialize(const Setup& s, bool sony)
{
    H h(s, sony);
    script_initialize(h);
    return std::move(h.o);
}

Observed half_parameters(const Setup& s, bool sony)
{
    H h(s, sony);
    script_parameters(h);
    return std::move(h.o);
}

Observed half_feeders(const Setup& s, bool sony)
{
    H h(s, sony);
    script_feeders(h);
    return std::move(h.o);
}

Observed half_composer(const Setup& s, bool sony)
{
    H h(s, sony);
    script_composer(h);
    return std::move(h.o);
}

Observed half_profiles(const Setup& s, bool sony)
{
    H h(s, sony);
    script_profiles(h);
    return std::move(h.o);
}

Observed half_converter(const Setup& s, bool sony)
{
    H h(s, sony);
    script_converter(h);
    return std::move(h.o);
}

Observed half_converters(const Setup& s, bool sony)
{
    H h(s, sony, true);
    script_converters(h);
    return std::move(h.o);
}

Observed half_boot(const Setup& s, bool sony)
{
    H h(s, sony);
    script_boot(h);
    return std::move(h.o);
}

Observed half_recorder_api(const Setup& s, bool sony)
{
    H h(s, sony);
    script_recorder_api(h);
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellSailRec's 58 functions are pspocket's own", "[hle][sailrec]")
{
    const hle::Registry& r = hle::registry();
    usize n = 0;
    for (const hle::Function& f : r.functions())
        if (f.module == "cellSailRec" && !f.internal) n++;
    CHECK(n == 58);
    check_names("cellSailRec", {"cellSailRecorderInitialize", "cellSailRecorderFinalize", "cellSailRecorderSetParameter",
        "cellSailRecorderGetParameter", "cellSailRecorderSetFeederAudio", "cellSailRecorderSetFeederVideo",
        "cellSailRecorderRegisterComposer", "cellSailRecorderUnregisterComposer", "cellSailRecorderSubscribeEvent",
        "cellSailRecorderUnsubscribeEvent", "cellSailRecorderReplaceEventHandler", "cellSailRecorderBoot",
        "cellSailRecorderCreateProfile", "cellSailRecorderDestroyProfile", "cellSailRecorderCreateVideoConverter",
        "cellSailRecorderDestroyVideoConverter", "cellSailRecorderOpenStream", "cellSailRecorderCloseStream",
        "cellSailRecorderStart", "cellSailRecorderStop", "cellSailRecorderCancel", "cellSailRecorderDumpImage",
        "cellSailFeederAudioInitialize", "cellSailFeederAudioFinalize", "cellSailFeederAudioNotifyCallCompleted",
        "cellSailFeederAudioNotifyFrameOut", "cellSailFeederAudioNotifySessionEnd",
        "cellSailFeederAudioNotifySessionError", "cellSailFeederVideoInitialize", "cellSailFeederVideoFinalize",
        "cellSailFeederVideoNotifyCallCompleted", "cellSailFeederVideoNotifyFrameOut",
        "cellSailFeederVideoNotifySessionEnd", "cellSailFeederVideoNotifySessionError", "cellSailComposerInitialize",
        "cellSailComposerFinalize", "cellSailComposerGetStreamParameter", "cellSailComposerGetEsUserParameter",
        "cellSailComposerGetEsVideoParameter", "cellSailComposerGetEsAudioParameter",
        "cellSailComposerNotifyCallCompleted", "cellSailComposerNotifySessionError", "cellSailComposerGetEsUserAu",
        "cellSailComposerGetEsVideoAu", "cellSailComposerGetEsAudioAu", "cellSailComposerTryGetEsUserAu",
        "cellSailComposerTryGetEsVideoAu", "cellSailComposerTryGetEsAudioAu", "cellSailComposerReleaseEsUserAu",
        "cellSailComposerReleaseEsVideoAu", "cellSailComposerReleaseEsAudioAu", "cellSailProfileSetStreamParameter",
        "cellSailProfileSetEsAudioParameter", "cellSailProfileSetEsVideoParameter",
        "cellSailVideoConverterCanGetResult", "cellSailVideoConverterCanProcess", "cellSailVideoConverterGetResult",
        "cellSailVideoConverterProcess"},
        /*marked=*/true);
}

TEST_CASE("cellSailRec against Sony's libsail_rec: the recorder before Boot", "[hle][lle][compare][sailrec]")
{
    const Setup s = setup();
    run(s, "cellSailRec: Initialize and Finalize", [&](bool sony) { return half_initialize(s, sony); },
        [](const Observed&) {});
    run(s, "cellSailRec: the parameters", [&](bool sony) { return half_parameters(s, sony); }, [](const Observed&) {});
    run(s, "cellSailRec: feeders, composer, events, commands before Boot, DumpImage",
        [&](bool sony) { return half_recorder_api(s, sony); }, [](const Observed&) {});
}

TEST_CASE("cellSailRec against Sony's libsail_rec: feeders, the composer, profiles, video converters",
    "[hle][lle][compare][sailrec]")
{
    const Setup s = setup();
    run(s, "cellSailRec: feeders", [&](bool sony) { return half_feeders(s, sony); }, [](const Observed&) {});
    run(s, "cellSailRec: the composer", [&](bool sony) { return half_composer(s, sony); }, [](const Observed&) {});
    run(s, "cellSailRec: profiles", [&](bool sony) { return half_profiles(s, sony); }, [](const Observed&) {});
    run(s, "cellSailRec: video converters", [&](bool sony) { return half_converter(s, sony); }, [](const Observed&) {});
}

TEST_CASE("cellSailRec against Sony's libsail_rec: the booted recorder", "[hle][lle][compare][sailrec]")
{
    const Setup s = setup();
    run(s, "cellSailRec: Boot, profiles, Cancel, Finalize", [&](bool sony) { return half_boot(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("mode 0: Boot: the state") == "2");
            CHECK(o.value("CreateProfile(1)") == "0x0");
            CHECK(o.value("CreateProfile with the allocator failing") == "0x806107f0");
            CHECK(o.value("DestroyProfile(not a profile)") == "0x0");
            CHECK(o.value("Finalize") == "0x0");
            CHECK(o.value("audio 1's makeup failing: Boot: the state") == "10");
            CHECK(o.value("mode 1: Boot: the state") == "2");
        });
}

TEST_CASE("cellSailRec against Sony's libsail_rec: video converters' conversions", "[hle][lle][compare][sailrec]")
{
    const Setup s = setup();
    run(s, "cellSailRec: conversions", [&](bool sony) { return half_converters(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("4 to 1: CreateVideoConverter") == "0x0");
            CHECK(o.value("4 to 1: results") == "2");
        });
}
