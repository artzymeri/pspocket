// HLE registry, import linkage and the process loader, end to end: a synthetic executable (tests/common/synthetic.cpp)
// whose _start is replaced by a small program is booted by the loader and run by the PPU interpreter through lv2.
// No game data (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"
#include "guest.hpp"
#include "hle_test_util.hpp"
#include "synthetic.hpp"

using namespace pspocket;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u64 kEnosys = 0xFFFFFFFF80010003ull; // CELL_ENOSYS, sign-extended

struct Process {
    GuestMemory memory;
    lv2::Config cfg;
    loader::BootState state;
    std::unique_ptr<lv2::Kernel> kernel;
    std::unique_ptr<hle::HleProcess> hle;

    explicit Process(const loader::Executable& exe)
    {
        REQUIRE(memory.reserve());
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        cfg = exe.kernel_config();
        loader::prepare_kernel_config(cfg, state);
        kernel = std::make_unique<lv2::Kernel>(memory, cfg);
        kernel->install();
        hle = std::make_unique<hle::HleProcess>(*kernel, PS_MEM_MAIN_BASE, cfg.main_area_end);
        hle->install();
    }
    ~Process()
    {
        hle.reset();
        kernel.reset();
    }
};

// The synthetic executable with _start (text 0x10200, file offset 0x200; the 0x1C0 bytes of function bodies after it
// are free) replaced by `code`.
std::vector<u8> exec_with_start(const test::SyntheticPpuExec& x, const std::vector<u32>& code)
{
    REQUIRE(x.entry_func == 0x10200);
    REQUIRE(code.size() * 4 <= 0x1C0);
    std::vector<u8> bytes = x.bytes;
    for (usize i = 0; i < code.size(); i++) ps_st32(bytes.data() + 0x200 + 4 * i, code[i]);
    return bytes;
}

} // namespace

TEST_CASE("HLE NIDs match their names", "[hle]")
{
    for (const hle::Function& f : hle::registry().functions()) {
        INFO(f.module << "::" << f.name);
        // A function known only by its NID is named after it (RPCS3's REG_FNID names, e.g. cellSysutil_E1EC7B6A, and
        // cellSysmodule_0x59521326).
        // The nameless module-management library's NIDs are fixed ones RPCS3 names (module_start ...): the NID
        // database's names for them.
        const auto known = recomp::NidDb::builtin().find(f.module, f.nid);
        CHECK((f.nid == recomp::nid_of(f.name) || f.name.ends_with(std::format("_{:08X}", f.nid)) ||
            f.name.ends_with(std::format("_0x{:08X}", f.nid)) || (f.module.empty() && known && *known == f.name)));
    }
    CHECK(hle::registry().find("sysPrxForUser", recomp::nid_of("sys_initialize_tls")) != nullptr);
    // A variable's NID is its name's, or the one RPCS3 registers it under (REG_VNID: the codec cores' operation tables,
    // D-591), which the NID database holds under that name.
    for (const hle::Variable& v : hle::registry().variables()) {
        INFO(v.module << "::" << v.name);
        const auto known = recomp::NidDb::builtin().find(v.module, v.nid);
        CHECK((v.nid == recomp::nid_of(v.name) || (known && *known == v.name)));
    }
}

TEST_CASE("Main memory allocator: first fit, alignment, reserved ranges", "[hle]")
{
    GuestMemory mem;
    REQUIRE(mem.reserve());
    hle::MainMemory m(mem, 0x10000, 0x10000000);
    REQUIRE(m.reserve(0x10000, 0x20000)); // an "ELF segment"
    CHECK_FALSE(m.reserve(0x20000, 0x1000));
    const u32 a = m.alloc(0x100);
    CHECK(a == 0x30000);
    CHECK(mem.is_committed(a));
    const u32 b = m.alloc(0x100, 0x100000);
    CHECK(b == 0x100000);
    m.free(a);
    CHECK_FALSE(mem.is_committed(a));
    CHECK(m.alloc(0x100) == 0x30000);
}

TEST_CASE("Linked stubs: descriptor layout, implemented and unimplemented calls", "[hle]")
{
    const auto x = test::build_ppu_exec();
    auto exe = loader::Executable::parse(x.bytes);
    REQUIRE(exe);
    Process p(**exe);
    u8* base = p.memory.base();

    const u32 tls_fn = p.hle->link_function("sysPrxForUser", 0x744680A2, "sys_initialize_tls");
    const u32 missing = p.hle->link_function("cellPad", 0x12345678, "");
    REQUIRE(tls_fn != 0);
    CHECK(p.hle->link_function("sysPrxForUser", 0x744680A2, "sys_initialize_tls") == tls_fn); // deduplicated
    CHECK(missing == tls_fn + hle::kStubEntrySize);
    // RPCS3 layout: {func = descriptor + 4, toc = 0}, the toc word doubling as the stub instruction.
    CHECK(ps_read32(base, tls_fn) == tls_fn + 4);
    CHECK(ps_read32(base, tls_fn + 4) == 0);
    CHECK(ps_ppu_dispatch_find(tls_fn + 4) != nullptr);

    // Implemented: sys_initialize_tls through the dispatch path, as `bctr` from an import stub reaches it.
    PPUContext ctx;
    ps_ppu_context_init(&ctx);
    ctx.gpr[3] = 0x01000001;
    ctx.gpr[4] = x.tls_vaddr;
    ctx.gpr[5] = x.tls_filesz;
    ctx.gpr[6] = x.tls_memsz;
    // The TLS image must be in guest memory: map the executable's data page by hand (reserved, so HLE allocations
    // don't land on it).
    REQUIRE(p.hle->main_memory().reserve(0x20000, 0x10000));
    REQUIRE(p.memory.commit(0x20000, 0x10000));
    std::memcpy(base + x.tls_vaddr, x.bytes.data() + 0x4000 + (x.tls_vaddr - 0x20000), x.tls_filesz);
    ps_ppu_call_indirect(&ctx, base, tls_fn + 4);
    REQUIRE(ctx.gpr[13] != 0);
    const u32 block = static_cast<u32>(ctx.gpr[13]) - hle::kTlsR13Offset;
    CHECK(ps_read32(base, block) == 0);                             // system area cleared
    CHECK(ps_read32(base, block + hle::kTlsSystemArea) == 0x11111111u); // image copied
    CHECK(ps_read32(base, block + hle::kTlsSystemArea + x.tls_filesz) == 0); // rest zeroed

    // Unimplemented: logs, returns CELL_ENOSYS, counts.
    ctx.gpr[3] = 7;
    ps_ppu_call_indirect(&ctx, base, missing + 4);
    CHECK(ctx.gpr[3] == kEnosys);
    CHECK(p.hle->unimplemented_calls() == 1);
    CHECK(p.hle->linked_at(missing + 4)->calls.load() == 1);
    CHECK(p.hle->linked_at(missing) == nullptr); // the descriptor word is not a stub
}

TEST_CASE("RPCS3 stubs: their NIDs, the answer, and an implementation taking precedence (D-182)", "[hle]")
{
    // Each NID is its name's, or a REG_FNID's own number, which then ends the generated name. (Fewer of them as each
    // library becomes pspocket's own.)
    REQUIRE(!hle::rpcs3_stubs().empty());
    for (const hle::StubFunction& s : hle::rpcs3_stubs()) {
        INFO(s.module << "::" << s.name);
        CHECK((s.nid == recomp::nid_of(s.name) || s.name.ends_with(std::format("{:08X}", s.nid))));
        CHECK(hle::find_rpcs3_stub(s.module, s.nid) == &s);
    }
    CHECK(hle::find_rpcs3_stub("cellSysutil", 0x12345678) == nullptr);

    const auto x = test::build_ppu_exec();
    auto exe = loader::Executable::parse(x.bytes);
    REQUIRE(exe);
    Process p(**exe);
    u8* base = p.memory.base();

    // A function RPCS3 leaves a stub answering CELL_OK, which pspocket has not made its own yet (the first of them; the
    // libraries are being made pspocket's own one by one, D-500): CELL_OK, counted apart from the gaps.
    PPUContext ctx;
    ps_ppu_context_init(&ctx);
    const hle::StubFunction* ok_stub = nullptr;
    for (const hle::StubFunction& s : hle::rpcs3_stubs())
        if (!ok_stub && s.sets_r3 && s.r3 == 0 && !hle::registry().find(s.module, s.nid)) ok_stub = &s;
    if (ok_stub) {
        INFO(ok_stub->module << "::" << ok_stub->name);
        REQUIRE(hle::find_rpcs3_stub(ok_stub->module, ok_stub->nid) == ok_stub);
        const u32 stub = p.hle->link_function(ok_stub->module, ok_stub->nid, ok_stub->name);
        REQUIRE(stub != 0);
        CHECK(p.hle->linked_at(stub + 4)->stub != nullptr);
        ctx.gpr[3] = 0x1234;
        ps_ppu_call_indirect(&ctx, base, stub + 4);
        CHECK(ctx.gpr[3] == 0);
        CHECK(p.hle->stub_calls() == 1);
        CHECK(p.hle->unimplemented_calls() == 0);
    }

    // cellMic::cellMicIsAttached answers 1 there.
    const u32 attached = p.hle->link_function("cellMic", recomp::nid_of("cellMicIsAttached"), "cellMicIsAttached");
    if (!hle::registry().find("cellMic", recomp::nid_of("cellMicIsAttached"))) {
        ps_ppu_call_indirect(&ctx, base, attached + 4);
        CHECK(ctx.gpr[3] == 1);
    }

    // Wherever the registry implements a stubbed function, the implementation is linked.
    for (const hle::StubFunction& s : hle::rpcs3_stubs()) {
        if (!hle::registry().find(s.module, s.nid)) continue;
        const u32 d = p.hle->link_function(s.module, s.nid, s.name);
        INFO(s.module << "::" << s.name);
        CHECK(p.hle->linked_at(d + 4)->impl != nullptr);
        CHECK(p.hle->linked_at(d + 4)->stub == nullptr);
    }
}

TEST_CASE("Loader: boot a synthetic executable to its first unimplemented import and its exit", "[hle][loader]")
{
    const auto x = test::build_ppu_exec();
    // synthLib's import 0xDEADBEEF: a NID no HLE module will ever implement.
    const u32 stub12 = x.first_stub_code + 12 * 0x20;
    const u32 kOut = 0x21000;                      // bss of the data segment

    std::vector<u32> code;
    auto at = [&] { return x.entry_func + static_cast<u32>(code.size() * 4); };
    code.push_back(as::mflr(0));
    code.push_back(as::std_(0, 16, 1));
    code.push_back(as::stdu(1, -128, 1));
    code.push_back(as::lis(31, kOut >> 16));
    code.push_back(as::ori(31, 31, kOut & 0xFFFF));
    const u32 saved[] = {3, 4, 6, 7, 13, 11, 12, 2, 8};
    for (u32 i = 0; i < std::size(saved); i++) code.push_back(as::std_(saved[i], static_cast<s32>(8 * i), 31));
    code.push_back(as::lwz(5, -0x7000, 13)); // first word of this thread's TLS image
    code.push_back(as::std_(5, 72, 31));
    code.push_back(as::ld(5, 0, 4)); // argv[0]
    code.push_back(as::lbz(5, 0, 5));
    code.push_back(as::std_(5, 80, 31));
    code.push_back(as::ld(5, 8, 4)); // argv[1] (the stack is freed when the thread exits: record it here)
    code.push_back(as::std_(5, 96, 31));
    for (int call = 0; call < 2; call++) {
        code.push_back(as::li(3, 5));
        code.push_back(as::b_rel(static_cast<s32>(stub12 - at()), true)); // bl synthLib 0xDEADBEEF stub
        code.push_back(as::ld(2, 40, 1));                                  // restore the TOC the stub saved
        code.push_back(as::std_(3, 88, 31));
    }
    code.push_back(as::li(3, 42));
    code.push_back(as::li(11, 22)); // _sys_process_exit(42)
    code.push_back(as::sc());

    auto exe = loader::Executable::parse(exec_with_start(x, code));
    REQUIRE(exe);
    CHECK((*exe)->primary_stack_size() == 0x10000);
    Process p(**exe);
    u32 main_id = 0;
    loader::LoadReport report;
    REQUIRE(loader::boot(*exe, *p.kernel, *p.hle, p.state, {}, main_id, &report) == lv2::CELL_OK);
    CHECK(report.functions_linked == 14);
    CHECK(report.functions_implemented == 12); // sysPrxForUser::sys_lwmutex_lock, _sys_printf, sys_ppu_thread_exit,
                                               // sys_process_exit, sys_fs::cellFsOpen, cellGcmSys::_cellGcmInitBody,
                                               // cellGcmSetFlipMode and cellGcmGetConfiguration,
                                               // cellSysutil::cellSysutilRegisterCallback, sys_io::cellPadInit,
                                               // cellSpurs::cellSpursInitialize (D-171), sys_libc::memcpy (D-184)
    // What nothing answers is named in the report (D-1061): synthLib's 0xDEADBEEF, and nothing pspocket has.
    bool deadbeef = false;
    for (const auto& m : report.missing) {
        INFO(m.library << " " << m.nid);
        CHECK((m.library == "synthLib" || !hle::registry().find(m.library, m.nid)));
        deadbeef |= m.library == "synthLib" && m.nid == 0xDEADBEEF;
    }
    CHECK(deadbeef);
    CHECK(report.missing.size() == report.functions_linked - report.functions_implemented - report.functions_stubbed -
                                       report.functions_lle);
    // sys_libc::_Ctype is pspocket's own since D-515: its reference is patched with the variable's address.
    CHECK(report.variables_unresolved == 0);
    CHECK(ps_read32(p.memory.base(), x.var_ref_target) == p.hle->link_variable("sys_libc", 0xD97B0687));
    // Every host module's internal functions are linked, imported or not (D-903): cellDmux's demuxer thread among them,
    // though the executable imports no cellDmux.
    u32 internals = 0, dmux_internals = 0;
    for (const hle::Function& fn : hle::registry().functions()) {
        if (!fn.internal) continue;
        internals++;
        if (fn.module == "cellDmux") dmux_internals++;
        INFO(fn.module << "::" << fn.name);
        CHECK(p.hle->descriptor_of(fn.module, fn.nid) != 0);
    }
    CHECK(internals > 0);
    CHECK(dmux_internals > 0);

    REQUIRE(p.kernel->wait_for_exit(std::chrono::seconds(10)));
    CHECK(p.kernel->exit_status() == 42);

    u8* base = p.memory.base();
    auto out = [&](u32 i) { return ps_read64(base, kOut + 8 * i); };
    CHECK(out(0) == 1);                    // argc
    const u32 argv = static_cast<u32>(out(1));
    CHECK(argv >= PS_MEM_STACK_BASE);
    CHECK(out(2) == 0);                    // envc
    CHECK(out(3) == main_id);
    CHECK(out(4) != 0);                    // r13 from sys_initialize_tls
    CHECK(out(5) == x.entry);              // r11 = e_entry
    CHECK(out(6) == x.malloc_pagesize);    // r12
    CHECK(out(7) == x.toc);                // r2 from the entry descriptor
    CHECK(out(8) == x.tls_vaddr);          // r8
    CHECK(out(9) == 0x11111111u);          // TLS image copied for the main thread
    CHECK(out(10) == '/');                 // argv[0] = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"
    CHECK(out(11) == kEnosys);             // the unimplemented import returned CELL_ENOSYS
    CHECK(out(12) == 0);                   // argv[1] = NULL

    // The import slot points at a linked stub, which counted both calls.
    const u32 descriptor = ps_read32(base, x.first_stub_slot + 12 * 4);
    const hle::LinkedFunction* f = p.hle->linked_at(descriptor + 4);
    REQUIRE(f != nullptr);
    CHECK(f->module == "synthLib");
    CHECK(f->nid == 0xDEADBEEF);
    CHECK(f->calls.load() == 2);
    CHECK(p.hle->unimplemented_calls() == 2);
    // HLE allocations stay clear of the executable's pages (text 0x10000+0x4000, data 0x20000+0x2000).
    CHECK(descriptor >= 0x22000);
    CHECK_FALSE(p.hle->main_memory().reserve(0x20000, 0x1000));
}

TEST_CASE("call_guest: arguments past eight in the caller's parameter save area (D-197)", "[hle]")
{
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
    hle::test::Fixture f;
    hle::test::GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    // A guest function adding its eleven arguments: r3..r10, then r1 + 0x70, + 0x78, + 0x80.
    constexpr u32 kCode = hle::test::kData, kOpd = hle::test::kData + 0x100;
    std::vector<u32> code;
    for (u32 r = 4; r <= 10; r++) code.push_back(as::add(3, 3, r));
    for (s32 at = 0x70; at <= 0x80; at += 8) {
        code.push_back(as::ld(11, at, 1));
        code.push_back(as::add(3, 3, 11));
    }
    code.push_back(as::blr());
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kOpd, kCode);
    ps_write32(b, kOpd + 4, 0);
    const u64 sp = t.ctx.gpr[1];
    const u64 sum = hle::call_guest(t.ctx, kOpd, {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024});
    CHECK(sum == 2047);
    CHECK(t.ctx.gpr[1] == sp);
    CHECK(hle::call_guest(t.ctx, kOpd, {1, 2, 4, 8, 16, 32, 64, 128, 0, 0, 0}) == 255);
}

namespace {
double fp_mix(PPUContext&, double a, u32 b, float c, s32 d, double e) { return a + b + static_cast<double>(c) + d + e; }
float fp_half(PPUContext&, float x) { return x / 2; }
} // namespace

// D-503: floating point arguments come from f1.. by their own count and still take their slot in the sequence, so the
// integers after them come from the registers after theirs; a float or double result goes to f1.
TEST_CASE("Typed binding: floating point arguments and results", "[hle]")
{
    PPUContext ctx;
    ps_ppu_context_init(&ctx);
    ctx.fpr[1] = 1.5;                       // a: slot 0
    ctx.gpr[4] = 2;                         // b: slot 1
    ctx.fpr[2] = 0.25;                      // c: slot 2
    ctx.gpr[6] = static_cast<u64>(-3);      // d: slot 3
    ctx.fpr[3] = 10.0;                      // e: slot 4
    hle::detail::thunk<&fp_mix>(ctx, nullptr);
    CHECK(ctx.fpr[1] == 1.5 + 2 + 0.25 - 3 + 10.0);
    ctx.fpr[1] = 3.0;
    hle::detail::thunk<&fp_half>(ctx, nullptr);
    CHECK(ctx.fpr[1] == 1.5);
}

// D-503: a host variable is laid out once per process, initialised, and every import of it gets the same address.
TEST_CASE("Host variables: laid out once and linked by address", "[hle]")
{
    const auto x = test::build_ppu_exec();
    auto exe = loader::Executable::parse(x.bytes);
    REQUIRE(exe);
    Process p(**exe);
    REQUIRE(hle::registry().find_variable("sysPrxForUser", 0x7DF066CF) != nullptr);
    const u32 a = p.hle->link_variable("sysPrxForUser", 0x7DF066CF);
    REQUIRE(a != 0);
    CHECK(a % 4 == 0);
    CHECK(ps_read32(p.memory.base(), a) == 0x00010001);
    CHECK(p.hle->link_variable("sysPrxForUser", 0x7DF066CF) == a);
    CHECK(p.hle->link_variable("sysPrxForUser", 0x12345678) == 0);
}

// D-602: a host variable that another thread is laying out (its initializer still running) is handed out once it is
// laid out. link_variable published the address first, so a second thread could read the variable unwritten: a
// libsync2 queue's first push read the default notifier table as zeros while the first pop laid it out (a spurious
// INVAL), found by ThreadSanitizer. Eight threads link that table at once, in a fresh process each round, and read its
// four notifiers the moment they have it.
TEST_CASE("Host variables: a thread linking one that another is laying out gets it laid out (D-602)", "[hle]")
{
    const u32 nid = recomp::nid_of("_pspocket_cellSync2NotifierTable");
    REQUIRE(hle::registry().find_variable("cellSync2", nid) != nullptr);
    constexpr u32 kThreads = 8;
    u32 unwritten = 0;
    for (int round = 0; round < 20; round++) {
        hle::test::Fixture f;
        std::atomic<u32> waiting{0}, zeros{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (u32 i = 0; i < kThreads; i++) {
            threads.emplace_back([&] {
                waiting++;
                while (!go.load()) std::this_thread::yield();
                const u32 table = f.hle->link_variable("cellSync2", nid);
                for (u32 k = 0; k < 4; k++)
                    if (!table || ps_read32(f.base(), table + 4 * k) == 0) zeros++;
            });
        }
        while (waiting.load() < kThreads) std::this_thread::yield();
        go = true;
        for (std::thread& t : threads) t.join();
        unwritten += zeros.load();
    }
    CHECK(unwritten == 0);
}
