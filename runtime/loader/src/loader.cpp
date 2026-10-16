// Process loader. Reference: RPCS3 rpcs3/Emu/Cell/PPUModule.cpp (ppu_load_exec, ppu_load_imports, ppu_patch_refs),
// rpcs3/Emu/Cell/lv2/sys_process.h (primary stack size codes). See loader.hpp for the model and deviations.

#include "pspocket/loader/loader.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

#include "pspocket/core/immutable_memory.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/process.hpp"
#include "pspocket/loader/lle.hpp"
#include "refs.hpp"
#include "pspocket/recomp/guest_image.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::loader {
namespace {

constexpr std::string_view kChan = "loader";

constexpr u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }
constexpr u64 align_down(u64 v, u64 a) { return v & ~(a - 1); }

} // namespace

namespace {
thread_local std::vector<u32>* t_watched_refs = nullptr; // WatchRefs
} // namespace

WatchRefs::WatchRefs(std::vector<u32>* out) : previous_(t_watched_refs) { t_watched_refs = out; }
WatchRefs::~WatchRefs() { t_watched_refs = previous_; }

// RPCS3 ppu_patch_refs: a reference table of {type, addr, addend} entries ending with type 0.
void patch_refs(GuestMemory& mem, u32 table, u32 value, std::string_view what)
{
    u8* base = mem.base();
    for (u32 at = table;; at += 12) {
        if (!mem.is_committed(at) || !mem.is_committed(at + 11)) {
            PS_LOG_ERROR(kChan, "{}: reference table at {:#010x} runs into unmapped memory", what, table);
            return;
        }
        const u32 type = ps_read32(base, at);
        if (type == 0) return;
        const u32 addr = ps_read32(base, at + 4);
        const u32 addend = ps_read32(base, at + 8);
        const u32 data = value + addend;
        if (addend) PS_LOG_WARN(kChan, "{}: REF({}) at {:#010x} has addend {:#x}", what, type, addr, addend);
        if (!mem.is_committed(addr) || !mem.is_committed(addr + (type == 1 ? 3 : 1))) { // its last byte too (D-605)
            PS_LOG_ERROR(kChan, "{}: REF({}) target {:#010x} is unmapped", what, type, addr);
            continue;
        }
        if (t_watched_refs) t_watched_refs->push_back(addr);
        switch (type) {
        case 1: ps_write32(base, addr, data); break;                                                    // ADDR32
        case 4: ps_write16(base, addr, static_cast<u16>(data)); break;                                  // ADDR16_LO
        case 6: ps_write16(base, addr, static_cast<u16>((data >> 16) + ((data & 0x8000) ? 1 : 0))); break; // ADDR16_HA
        case 57: // ADDR16_LO_DS: the top 14 bits of the halfword (ppu_bf_t<be_t<u16>, 0, 14>)
            ps_write16(base, addr, static_cast<u16>((ps_read16(base, addr) & 3) | (data & 0xFFFC)));
            break;
        default: PS_LOG_ERROR(kChan, "{}: unknown REF type {} at {:#010x}", what, type, addr); break;
        }
    }
}

namespace {

// Writes `text` (NUL-terminated) at addr, returns the next 16-byte aligned address (RPCS3 aligns each string to 16).
u32 put_string(u8* base, u32 addr, const std::string& text)
{
    std::memcpy(base + addr, text.c_str(), text.size() + 1);
    return static_cast<u32>(addr + align_up(text.size() + 1, 0x10));
}

// The main thread's executor: sys_initialize_tls and the entry arguments, then the real executor.
void main_executor(void* user, PPUContext& ctx, u8* base, u32 entry)
{
    auto& st = *static_cast<BootState*>(user);
    lv2::PpuThread* t = lv2::current_thread();
    if (t && t->is_main && !st.main_prepared.exchange(true)) {
        const Executable& exe = *st.exe;
        const auto& tls = exe.module().tls;
        const u32 tls_vaddr = tls ? tls->vaddr : 0, tls_fsize = tls ? tls->image_size : 0,
                  tls_vsize = tls ? tls->size : 0;

        // RPCS3 ppu_load_exec: { set_args 4: thread id, TLS image, size, memory size }, hle_call sys_initialize_tls.
        if (const hle::Function* f = hle::registry().find("sysPrxForUser", 0x744680A2)) {
            ctx.gpr[3] = t->id;
            ctx.gpr[4] = tls_vaddr;
            ctx.gpr[5] = tls_fsize;
            ctx.gpr[6] = tls_vsize;
            f->fn(ctx, base);
        } else {
            PS_FATAL(kChan, "sysPrxForUser::sys_initialize_tls is not registered");
        }

        // Arguments at the top of the stack: u64 pointer arrays (argv, NULL, envp, NULL), then the strings.
        const auto& argv = st.options.argv;
        const auto& envp = st.options.envp;
        const u32 pointers = static_cast<u32>(8 * align_up(envp.size() + argv.size() + 2, 2));
        u32 alloc = pointers;
        for (const auto& a : argv) alloc += static_cast<u32>(align_up(a.size() + 1, 0x10));
        for (const auto& e : envp) alloc += static_cast<u32>(align_up(e.size() + 1, 0x10));
        if (alloc >= t->stack_size) PS_FATAL(kChan, "process arguments ({:#x} bytes) do not fit the main stack", alloc);
        const u32 args = t->stack_addr + t->stack_size - alloc;
        u32 strings = args + pointers;
        u32 slot = args;
        const u32 argv_addr = slot;
        for (const auto& a : argv) {
            ps_write64(base, slot, strings);
            slot += 8;
            strings = put_string(base, strings, a);
        }
        ps_write64(base, slot, 0);
        slot += 8;
        const u32 envp_addr = slot;
        for (const auto& e : envp) {
            ps_write64(base, slot, strings);
            slot += 8;
            strings = put_string(base, strings, e);
        }
        ps_write64(base, slot, 0);
        ctx.gpr[1] -= alloc;

        // The firmware modules' starts (D-157), dependencies first, as RPCS3 ppu_load_exec: { set_args 2: 0, 0 },
        // lle_call; a module with a module_prologue has that called with module_start's descriptor first, as
        // liblv2's entryx does (D-916 part 1: the prologue runs the module's static constructors). Each returns to
        // kThreadReturnAddress like a callback; r1, r2 and LR come back.
        const u64 toc = ctx.gpr[2], lr = ctx.lr;
        for (const hle::ModuleEntry& start : st.module_starts) {
            const u32 opd = start.wrapper ? start.wrapper : start.entry;
            const u64 sp = ctx.gpr[1];
            ctx.gpr[1] = (sp - 0x80) & ~u64{15};
            ps_write64(base, static_cast<u32>(ctx.gpr[1]), sp);
            ctx.gpr[2] = ps_read32(base, opd + 4);
            if (start.wrapper) {
                ctx.gpr[3] = start.entry ? u64{start.entry} : ~u64{0};
                ctx.gpr[4] = 0;
                ctx.gpr[5] = 0;
            } else {
                ctx.gpr[3] = 0;
                ctx.gpr[4] = 0;
            }
            ctx.lr = lv2::kThreadReturnAddress;
            st.inner(st.inner_user, ctx, base, ps_read32(base, opd));
            PS_LOG_INFO(kChan, "module_start {:#010x} (prologue {:#010x}) returned {:#x}", start.entry, start.wrapper,
                static_cast<u32>(ctx.gpr[3]));
            ctx.gpr[1] = sp;
        }
        ctx.gpr[2] = toc;
        ctx.lr = lr;

        // { set_args 8: argc, argv, envp, envc, thread id, TLS image, size, memory size }, r11 = e_entry,
        // r12 = malloc page size, entry_call.
        ctx.gpr[3] = argv.size();
        ctx.gpr[4] = argv_addr;
        ctx.gpr[5] = envp_addr;
        ctx.gpr[6] = envp.size();
        ctx.gpr[7] = t->id;
        ctx.gpr[8] = tls_vaddr;
        ctx.gpr[9] = tls_fsize;
        ctx.gpr[10] = tls_vsize;
        ctx.gpr[11] = exe.entry_descriptor();
        ctx.gpr[12] = exe.malloc_pagesize();
        PS_LOG_INFO(kChan, "main thread {:#x}: entry {:#010x}, r13 {:#x}, sp {:#010x}", t->id, entry, ctx.gpr[13],
            ctx.gpr[1]);
    }
    st.inner(st.inner_user, ctx, base, entry);
}

} // namespace

// ------------------------------------------------------------------------------------------------- Executable

recomp::Result<std::shared_ptr<Executable>> Executable::parse(std::vector<u8> bytes)
{
    std::shared_ptr<Executable> exe(new Executable());
    exe->bytes_ = std::move(bytes);
    auto elf = recomp::ElfFile::parse(exe->bytes_);
    if (!elf) return elf.error();
    if (!elf->is_ppu()) return recomp::make_error("not a 64-bit PowerPC ELF (decrypt SELF files first)");
    if (elf->header().type != 2) return recomp::make_error("not an executable (ELF type {:#x}; PRX loading is not implemented)", elf->header().type);
    exe->elf_ = std::make_unique<recomp::ElfFile>(std::move(elf.value()));
    auto module = recomp::analyze_ppu_module(*exe->elf_);
    if (!module) return module.error();
    exe->module_ = std::move(module.value());
    recomp::resolve_names(exe->module_, recomp::NidDb::builtin());
    for (const auto& w : exe->module_.warnings) PS_LOG_WARN(kChan, "{}", w);
    return exe;
}

u32 Executable::sdk_version() const
{
    return module_.process_param ? module_.process_param->sdk_version : 0x00360001;
}

s32 Executable::primary_prio() const
{
    return module_.process_param ? module_.process_param->primary_prio : 1050;
}

u32 Executable::primary_stack_size() const
{
    const u32 sz = module_.process_param ? module_.process_param->primary_stacksize : 0x100000;
    switch (sz) {
    case 0x10: return 32 * 1024;
    case 0x20: return 64 * 1024;
    case 0x30: return 96 * 1024;
    case 0x40: return 128 * 1024;
    case 0x50: return 256 * 1024;
    case 0x60: return 512 * 1024;
    case 0x70: return 1024 * 1024;
    // RPCS3: the minimum appears to be 64 KB rather than SYS_PROCESS_PARAM_STACK_SIZE_MIN.
    default: return static_cast<u32>(align_up(std::clamp<u32>(sz, 0x10000, 0x100000), 4096));
    }
}

u32 Executable::malloc_pagesize() const
{
    return module_.process_param ? module_.process_param->malloc_pagesize : 0x100000;
}

lv2::Config Executable::kernel_config(lv2::Config base) const
{
    base.sdk_version = sdk_version();
    if (module_.process_param && module_.process_param->magic_ok()) {
        base.ppc_seg = module_.process_param->ppc_seg;
        base.crash_dump_param_addr = module_.process_param->crash_dump_param_addr.value_or(0);
    }
    base.main_area_end = sdk_version() < 0x200000 ? 0x20000000u : 0x10000000u; // RPCS3 is_vm_main_512
    return base;
}

// ------------------------------------------------------------------------------------------------- boot

void prepare_kernel_config(lv2::Config& cfg, BootState& state)
{
    if (!state.options.argv.empty()) cfg.executable_path = state.options.argv[0];
    state.inner = cfg.executor ? cfg.executor : lv2::default_executor;
    state.inner_user = cfg.executor_user;
    cfg.executor = main_executor;
    cfg.executor_user = &state;
}

bool register_translated_ppu(const GuestMemory& memory, const u32* image_words, u32 image_word_count,
    void (*register_fn)())
{
    for (u32 i = 0; i < image_word_count; i++) {
        const u32 addr = image_words[2 * i], word = image_words[2 * i + 1];
        if (!memory.is_committed(addr) || ps_read32(memory.base(), addr) != word) {
            PS_LOG_WARN(kChan, "the built-in translated PPU code is for another executable (the word at {:#010x} differs): "
                               "interpreting this one", addr);
            return false;
        }
    }
    register_fn();
    return true;
}

lv2::Result load(const Executable& exe_ref, lv2::Kernel& kernel, hle::HleProcess& hle, LoadReport* report,
    LleModules* lle)
{
    const Executable* exe = &exe_ref;
    using lv2::err;
    LoadReport rep;
    GuestMemory& mem = kernel.memory();
    u8* base = mem.base();
    const u64 page = GuestMemory::host_page_size();
    const u32 main_end = kernel.config().main_area_end;

    // LOAD segments at p_vaddr (RPCS3 vm::falloc in vm::main). The GuestImage holds each segment's file bytes.
    auto image = recomp::GuestImage::from_elf(exe->elf());
    if (!image) {
        PS_LOG_ERROR(kChan, "cannot place segments: {}", image.error().message);
        return err(lv2::CELL_ENOEXEC);
    }
    // Host-page ranges of all segments, merged (neighbouring segments may share a page), reserved in the main area
    // so HLE allocations never land on them.
    std::vector<std::pair<u64, u64>> ranges;
    for (const auto& seg : image->segments()) {
        if (seg.memsz == 0) continue;
        const u64 lo = align_down(seg.vaddr, page), hi = align_up(seg.vaddr + seg.memsz, page);
        if (seg.vaddr < PS_MEM_MAIN_BASE || hi > main_end) {
            PS_LOG_ERROR(kChan, "segment {:#010x}+{:#x} is outside the main area", seg.vaddr, seg.memsz);
            return err(lv2::CELL_ENOEXEC);
        }
        ranges.emplace_back(lo, hi);
    }
    std::sort(ranges.begin(), ranges.end());
    for (usize i = 0; i < ranges.size(); i++) {
        u64 lo = ranges[i].first, hi = ranges[i].second;
        while (i + 1 < ranges.size() && ranges[i + 1].first <= hi) hi = std::max(hi, ranges[++i].second);
        if (!hle.main_memory().reserve(static_cast<u32>(lo), static_cast<u32>(hi - lo)) ||
            !mem.commit(static_cast<u32>(lo), hi - lo)) {
            PS_LOG_ERROR(kChan, "cannot map {:#010x}..{:#010x} for the executable", lo, hi);
            return err(lv2::CELL_ENOMEM);
        }
    }
    clear_immutable_ranges(); // a new process's (an earlier executable's in tests)
    for (const auto& seg : image->segments()) {
        if (seg.memsz == 0) continue;
        std::memcpy(base + seg.vaddr, seg.data.data(), seg.data.size());
        std::memset(base + seg.vaddr + seg.data.size(), 0, seg.memsz - seg.data.size());
        PS_LOG_DEBUG(kChan, "segment {:#010x}+{:#x} (file {:#x}, flags {:#x})", seg.vaddr, seg.memsz, seg.data.size(),
            seg.flags);
        rep.segments++;
        // Written by nobody from now on (D-1013): the SPU program tracker need not check code copied from it again.
        if (!(seg.flags & (recomp::elf::kPfW | recomp::elf::kPfSpuW | recomp::elf::kPfRsxW)))
            add_immutable_range(static_cast<u32>(seg.vaddr), static_cast<u32>(seg.vaddr + seg.memsz));
    }

    // Which system libraries this game gets from the firmware's own modules and which from the host (D-175).
    if (lle) lle->decide(LleModules::needs_of(exe->module()), [&kernel](u32 n) { return kernel.syscall_bound(n); });

    // Imports (RPCS3 ppu_load_imports): each stub table slot gets the linked descriptor: the firmware module's export
    // when the library goes LLE (D-157, D-175), else the host module's stub.
    for (const auto& lib : exe->module().imports) {
        const PrxModule* module = nullptr;
        if (lle && lle->chooses(lib.name)) {
            std::string why;
            module = lle->load(lib.name, hle, &why);
            if (!module) PS_LOG_ERROR(kChan, "{} stays on the host: {}", lib.name, why);
        }
        for (const auto& fn : lib.functions) {
            u32 descriptor = 0;
            bool from_firmware = false;
            if (module) {
                if (const auto it = module->functions.find({lib.name, fn.nid}); it != module->functions.end()) {
                    descriptor = it->second;
                    from_firmware = true;
                    rep.functions_lle++;
                } else {
                    PS_LOG_ERROR(kChan, "{}::{} ({:#010x}) is not in {}", lib.name, fn.resolved.name, fn.nid, module->file);
                }
            }
            if (!descriptor) descriptor = hle.link_function(lib.name, fn.nid, fn.resolved.name);
            if (!descriptor) return err(lv2::CELL_ENOMEM);
            ps_write32(base, fn.slot, descriptor);
            if (fn.refs) patch_refs(mem, *fn.refs, descriptor, std::format("{}::{}", lib.name, fn.resolved.name));
            rep.functions_linked++;
            if (hle::registry().find(lib.name, fn.nid)) rep.functions_implemented++;
            else if (!module && hle::find_rpcs3_stub(lib.name, fn.nid)) rep.functions_stubbed++;
            else if (!from_firmware) rep.missing.push_back({lib.name, std::string(fn.resolved.name), fn.nid});
        }
        for (const auto& var : lib.variables) {
            if (module) {
                if (const auto it = module->variables.find({lib.name, var.nid}); it != module->variables.end()) {
                    patch_refs(mem, var.ref_table, it->second, std::format("{}::{}", lib.name, var.resolved.name));
                    continue;
                }
            }
            if (const u32 addr = module ? 0 : hle.link_variable(lib.name, var.nid)) { // the host's (D-503)
                patch_refs(mem, var.ref_table, addr, std::format("{}::{}", lib.name, var.resolved.name));
                continue;
            }
            PS_LOG_ERROR(kChan, "UNIMPLEMENTED variable {}::{} ({:#010x}): {} references left unpatched", lib.name,
                var.resolved.name.empty() ? "?" : var.resolved.name, var.nid, var.refs.size());
            rep.variables_unresolved++;
        }
        for (const auto& p : lib.problems) PS_LOG_WARN(kChan, "import library {}: {}", lib.name, p);
    }
    // What the host's modules for those libraries take from the firmware's (D-197): cellAdec's codec cores.
    if (lle) lle->link_host_imports(LleModules::needs_of(exe->module()), hle);
    if (lle) rep.lle_modules = static_cast<u32>(lle->loaded().size());
    // Functions the host's modules hand out by address (cellGcmCallback, the demuxer's and decoders' threads), linked
    // while no guest thread runs: every host module's, as one module the game imports calls into others it need not
    // (cellSail into cellDmux, cellAdec, cellVdec, cellVpost; D-903).
    if (!hle.link_all_internal_functions()) return err(lv2::CELL_ENOMEM);
    PS_LOG_INFO(kChan, "{} segments, {} imported functions ({} implemented, {} by {} firmware modules), {} unresolved "
                       "variables", rep.segments, rep.functions_linked, rep.functions_implemented, rep.functions_lle,
        rep.lle_modules, rep.variables_unresolved);
    for (const auto& m : rep.missing) // D-1061: named at load, before anything runs
        PS_LOG_ERROR(kChan, "nothing answers the imported function {}::{} ({:#010x}): a call to it fails", m.library,
            m.name.empty() ? "?" : m.name, m.nid);
    if (report) *report = rep;
    return lv2::CELL_OK;
}

lv2::Result start_main_thread(std::shared_ptr<const Executable> exe, lv2::Kernel& kernel, BootState& state,
    const BootOptions& options, u32& main_thread_id)
{
    using lv2::err;
    GuestMemory& mem = kernel.memory();
    u8* base = mem.base();

    // Main thread (RPCS3 ppu_load_exec "Initialize main thread").
    const u32 opd = exe->entry_descriptor();
    if (!mem.is_committed(opd)) {
        PS_LOG_ERROR(kChan, "entry descriptor {:#010x} is not in a loaded segment", opd);
        return err(lv2::CELL_ENOEXEC);
    }
    state.exe = exe;
    state.options = options;
    lv2::ThreadParams p;
    p.name = "main_thread";
    p.entry = ps_read32(base, opd);
    p.rtoc = ps_read32(base, opd + 4);
    p.prio = exe->primary_prio();
    p.stack_size = exe->primary_stack_size();
    p.main = true;
    p.joinable = false;
    u32 id = 0;
    if (const lv2::Result r = kernel.create_thread(p, id); r != lv2::CELL_OK) return r;
    state.main_thread_id = id;
    main_thread_id = id;
    return kernel.start_thread(id);
}

lv2::Result boot(std::shared_ptr<const Executable> exe, lv2::Kernel& kernel, hle::HleProcess& hle, BootState& state,
    const BootOptions& options, u32& main_thread_id, LoadReport* report)
{
    if (const lv2::Result r = load(*exe, kernel, hle, report); r != lv2::CELL_OK) return r;
    return start_main_thread(std::move(exe), kernel, state, options, main_thread_id);
}

} // namespace pspocket::loader
