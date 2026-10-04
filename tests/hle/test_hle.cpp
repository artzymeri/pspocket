// HLE registry, import linkage and the process loader, end to end: a synthetic executable (tests/common/synthetic.cpp)
// whose _start is replaced by a small program is booted by the loader and run by the PPU interpreter through lv2.
// No game data (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>

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
        CHECK(f.nid == recomp::nid_of(f.name));
    }
    CHECK(hle::registry().find("sysPrxForUser", recomp::nid_of("sys_initialize_tls")) != nullptr);
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
    CHECK(report.functions_implemented == 8); // sysPrxForUser::sys_lwmutex_lock, sys_ppu_thread_exit, sys_process_exit,
                                              // sys_fs::cellFsOpen, cellGcmSys::_cellGcmInitBody and
                                              // cellGcmGetConfiguration, cellSysutil::cellSysutilRegisterCallback,
                                              // sys_io::cellPadInit
    CHECK(report.variables_unresolved == 1);

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
