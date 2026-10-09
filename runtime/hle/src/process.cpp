// HLE registry, main-area allocator, import linkage and TLS.
// References: RPCS3 rpcs3/Emu/Cell/PPUModule.cpp (ppu_initialize_modules: the 8-byte stub entries; ppu_load_imports),
// rpcs3/Emu/Memory/vm.cpp (vm::alloc first fit in vm::main), Emu/Cell/Modules/sys_ppu_thread_.cpp (TLS).

#include "pspocket/hle/process.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

#include "guest.hpp"
#include "prx.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/disasm.hpp"
#include "pspocket/ppu/runtime.h"

namespace pspocket::hle {

void register_all_modules(Registry& r); // modules.cpp
void install_npd_opener(lv2::Kernel& kernel); // sys_fs.cpp
void install_spu_thread_hooks(lv2::Kernel& kernel); // spu_programs.cpp

namespace {

constexpr std::string_view kChan = "hle";

std::atomic<HleProcess*> g_current{nullptr};

constexpr u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

// "0x1 0x2 ..." for r3..r10, the argument registers of a call.
std::string arg_registers(const PPUContext& ctx)
{
    std::string out;
    for (int i = 3; i <= 10; i++) out += std::format("{}{:#x}", i == 3 ? "" : " ", ctx.gpr[i]);
    return out;
}

// Every stub entry's code address is registered with this function. ps_ppu_call_indirect / ps_ppu_jump_indirect and
// the interpreter's call_lifted path set ctx->cia to the target before calling it, which identifies the import.
void stub_trampoline(PPUContext* ctx, uint8_t* base)
{
    HleProcess* p = HleProcess::current();
    LinkedFunction* f = p ? p->linked_at(ctx->cia) : nullptr;
    if (!f) PS_FATAL(kChan, "HLE stub call at {:#010x} with no linked function (LR {:#010x})", ctx->cia, ctx->lr);
    const u64 n = f->calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (f->impl) {
        PS_LOG_TRACE(kChan, "{}::{} (LR {:#010x}, r3 {:#x}, r4 {:#x}, r5 {:#x}) on thread {:#x}", f->module, f->name, ctx->lr,
            ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], lv2::current_thread() ? lv2::current_thread()->id : 0);
        f->impl->fn(*ctx, base);
        return;
    }
    const std::string_view name = f->name.empty() ? std::string_view("?") : std::string_view(f->name);
    if (f->stub) {
        // D-182: RPCS3's module has this function as a stub too; it is answered the same way, and logged like a gap
        // (the first calls, then every 1024th), so the log still lists what is not implemented.
        p->on_stub();
        if (n <= 8 || n % 1024 == 0) {
            PS_LOG_WARN(kChan, "STUB {}::{} ({:#010x}) called from {:#010x}, r3..r10 = {} -> {} as RPCS3 (call #{})",
                f->module, name, f->nid, static_cast<u32>(ctx->lr) - 4, arg_registers(*ctx),
                f->stub->sets_r3 ? std::format("{:#x}", f->stub->r3) : std::string("void"), n);
        }
        if (f->stub->sets_r3) ctx->gpr[3] = f->stub->r3;
        return;
    }
    p->on_unimplemented();
    if (p->config().abort_on_unimplemented) {
        PS_FATAL(kChan, "UNIMPLEMENTED {}::{} ({:#010x}) called from {:#010x}, r3..r10 = {}", f->module, name, f->nid,
            static_cast<u32>(ctx->lr) - 4, arg_registers(*ctx));
    }
    if (n <= 8 || n % 1024 == 0) { // as lv2's unimplemented syscalls: a polled function must not flood the log
        PS_LOG_ERROR(kChan, "UNIMPLEMENTED {}::{} ({:#010x}) called from {:#010x}, r3..r10 = {} (call #{})", f->module,
            name, f->nid, static_cast<u32>(ctx->lr) - 4, arg_registers(*ctx), n);
    }
    ctx->gpr[3] = lv2::to_r3(lv2::err(lv2::CELL_ENOSYS));
}

} // namespace

void log_call_site(PPUContext& ctx, u8* base, std::string_view why)
{
    constexpr u32 kInstructions = 16;
    const u32 ret = static_cast<u32>(ctx.lr);
    PS_LOG_ERROR(kChan, "{} (called from {:#010x}); r3..r10 = {}", why, ret - 4, arg_registers(ctx));
    lv2::PpuThread* t = static_cast<lv2::PpuThread*>(ctx.runtime);
    if (!t || ret < 4 * kInstructions || !guest_readable(*t, ret - 4 * kInstructions, 4 * kInstructions)) return;
    for (u32 pc = ret - 4 * kInstructions; pc < ret; pc += 4) {
        PS_LOG_ERROR(kChan, "  {:#010x}: {}", pc, ppu::disassemble(ps_read32(base, pc), pc));
    }
}

// ------------------------------------------------------------------------------------------------- registry

namespace {
std::string registry_key(std::string_view prefix, std::string_view module, u32 nid)
{
    return std::format("{}{}:{:08x}", prefix, module, nid);
}
} // namespace

void Registry::add(std::string_view module, std::initializer_list<Function> list)
{
    for (Function f : list) {
        f.module = module;
        if (!index_.emplace(registry_key("", module, f.nid), functions_.size()).second)
            PS_FATAL(kChan, "HLE function {}::{} ({:#010x}) registered twice", module, f.name, f.nid);
        functions_.push_back(f);
    }
}

const Function* Registry::find(std::string_view module, u32 nid) const
{
    const auto it = index_.find(registry_key("", module, nid));
    return it == index_.end() ? nullptr : &functions_[it->second];
}

void Registry::add_variables(std::string_view module, std::initializer_list<Variable> list)
{
    for (Variable v : list) {
        v.module = module;
        if (!index_.emplace(registry_key("v", module, v.nid), variables_.size()).second)
            PS_FATAL(kChan, "HLE variable {}::{} ({:#010x}) registered twice", module, v.name, v.nid);
        variables_.push_back(v);
    }
}

const Variable* Registry::find_variable(std::string_view module, u32 nid) const
{
    const auto it = index_.find(registry_key("v", module, nid));
    return it == index_.end() ? nullptr : &variables_[it->second];
}

void Registry::add_imports(std::string_view module, std::span<const HostImport> list)
{
    for (HostImport i : list) {
        i.module = module;
        imports_.push_back(i);
    }
}

const Registry& registry()
{
    static const Registry r = [] {
        Registry built;
        register_all_modules(built);
        return built;
    }();
    return r;
}

// ------------------------------------------------------------------------------------------------- main memory

MainMemory::MainMemory(GuestMemory& memory, u32 begin, u32 end) : memory_(memory), begin_(begin), end_(end) {}

bool MainMemory::reserve(u32 addr, u32 size)
{
    if (size == 0 || addr < begin_ || u64{addr} + size > end_) return false;
    std::lock_guard lock(mutex_);
    const u32 end = addr + size;
    for (const auto& [s, e] : used_)
        if (addr < e && s < end) return false;
    used_.emplace_back(addr, end);
    std::sort(used_.begin(), used_.end());
    return true;
}

u32 MainMemory::alloc(u32 size, u32 align)
{
    if (size == 0) return 0;
    // Whole host pages, so two allocations never share a page that one of them decommits.
    const u64 page = GuestMemory::host_page_size();
    const u64 bytes = align_up(size, page);
    align = static_cast<u32>(std::max<u64>(align, page));
    std::lock_guard lock(mutex_);
    u64 at = align_up(begin_, align);
    for (const auto& [s, e] : used_) {
        if (at + bytes <= s) break;
        at = std::max<u64>(at, align_up(e, align));
    }
    if (at + bytes > end_) return 0;
    if (!memory_.commit(static_cast<u32>(at), bytes)) return 0;
    std::memset(memory_.host(static_cast<u32>(at)), 0, bytes);
    used_.emplace_back(static_cast<u32>(at), static_cast<u32>(at + bytes));
    std::sort(used_.begin(), used_.end());
    return static_cast<u32>(at);
}

void MainMemory::free(u32 addr)
{
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(used_.begin(), used_.end(), [&](const auto& r) { return r.first == addr; });
    if (it == used_.end()) {
        PS_LOG_ERROR(kChan, "MainMemory::free({:#010x}): not an allocation", addr);
        return;
    }
    memory_.decommit(it->first, it->second - it->first);
    used_.erase(it);
}

// ------------------------------------------------------------------------------------------------- process

HleProcess::HleProcess(lv2::Kernel& kernel, u32 main_begin, u32 main_end, ProcessConfig config)
    : kernel_(kernel), config_(config), main_(kernel.memory(), main_begin, main_end)
{
    install_npd_opener(kernel);       // sys_fs.cpp: encrypted SDATA files (D-165)
    install_spu_thread_hooks(kernel); // spu_programs.cpp: SPU thread groups' programs and SELF images (D-167)
    // Sized once, so linking while guest threads call stubs (D-159) never moves what linked_at reads.
    chunks_.reserve(kPreparedChunks);
    linked_.resize(usize{kPreparedChunks} * kEntriesPerChunk);
}

HleProcess::~HleProcess()
{
    if (kernel_modules_ && kernel_.module_host() == kernel_modules_.get()) kernel_.set_module_host(nullptr);
    HleProcess* self = this;
    g_current.compare_exchange_strong(self, nullptr);
    for (const auto& f : linked_)
        if (f) ps_ppu_dispatch_unregister(f->descriptor + 4);
}

void HleProcess::install() { g_current.store(this); }

void HleProcess::set_module_loader(ModuleLoader* loader)
{
    module_loader_ = loader;
    if (kernel_modules_ && kernel_.module_host() == kernel_modules_.get()) kernel_.set_module_host(nullptr);
    kernel_modules_ = loader ? make_kernel_module_host(*this) : nullptr;
    if (kernel_modules_) kernel_.set_module_host(kernel_modules_.get());
}

HleProcess* HleProcess::current() { return g_current.load(); }

u32 HleProcess::link_function(std::string_view module, u32 nid, std::string_view name)
{
    std::lock_guard lock(link_mutex_);
    for (usize c = 0; c < chunks_.size(); c++) {
        for (u32 e = 0; e < chunks_[c].used; e++) {
            const auto& f = linked_[c * kEntriesPerChunk + e];
            if (f && f->nid == nid && f->module == module) return f->descriptor;
        }
    }

    if (chunks_.empty() || chunks_.back().used == kEntriesPerChunk) {
        const u32 base = main_.alloc(kStubChunkSize, kStubChunkSize);
        if (!base) {
            PS_LOG_ERROR(kChan, "no main-area memory for HLE stubs ({}::{})", module, name);
            return 0;
        }
        if (chunks_.size() == kPreparedChunks)
            PS_LOG_WARN(kChan, "more than {} chunks of HLE stubs: linking from now on is unsafe while guest threads run",
                kPreparedChunks);
        chunks_.push_back({base, 0});
        if (linked_.size() < chunks_.size() * kEntriesPerChunk) linked_.resize(chunks_.size() * kEntriesPerChunk);
    }
    Chunk& c = chunks_.back();
    const u32 index = static_cast<u32>(chunks_.size() - 1) * kEntriesPerChunk + c.used;
    const u32 descriptor = c.base + c.used * kStubEntrySize;
    c.used++;

    auto f = std::make_unique<LinkedFunction>();
    f->module = std::string(module);
    f->name = std::string(name);
    f->nid = nid;
    f->descriptor = descriptor;
    f->impl = registry().find(module, nid);
    if (!f->impl) f->stub = find_rpcs3_stub(module, nid);

    u8* base = kernel_.memory().base();
    ps_write32(base, descriptor, descriptor + 4);
    ps_write32(base, descriptor + 4, 0);
    ps_ppu_dispatch_register(descriptor + 4, stub_trampoline);

    PS_LOG_DEBUG(kChan, "import {}::{} ({:#010x}) -> {:#010x}{}", module, name.empty() ? "?" : name, nid, descriptor,
        f->impl ? "" : f->stub ? " (RPCS3 stub)" : " (unimplemented)");
    linked_[index] = std::move(f);
    return descriptor;
}

bool HleProcess::link_internal_functions(const std::vector<std::string>& modules)
{
    for (const Function& f : registry().functions()) {
        if (!f.internal) continue;
        if (std::find(modules.begin(), modules.end(), f.module) == modules.end()) continue;
        if (!link_function(f.module, f.nid, f.name)) return false;
    }
    return true;
}

u32 HleProcess::descriptor_of(std::string_view module, u32 nid) const
{
    std::lock_guard lock(link_mutex_);
    for (usize c = 0; c < chunks_.size(); c++) {
        for (u32 e = 0; e < chunks_[c].used; e++) {
            const auto& f = linked_[c * kEntriesPerChunk + e];
            if (f && f->nid == nid && f->module == module) return f->descriptor;
        }
    }
    return 0;
}

u32 HleProcess::link_variable(std::string_view module, u32 nid)
{
    const Variable* v = registry().find_variable(module, nid);
    if (!v) return 0;
    std::lock_guard lock(link_mutex_);
    if (const auto it = variables_.find({std::string(module), nid}); it != variables_.end()) return it->second;
    // Laid out one after another in blocks of 64 KB of the main area (a larger one gets a block of its own).
    constexpr u32 kBlock = 0x10000;
    const u32 align = std::max<u32>(v->align, 1);
    u32 at = variable_block_ ? (variable_block_ + variable_used_ + align - 1) & ~(align - 1) : 0;
    if (!variable_block_ || at + v->size > variable_block_ + kBlock) {
        const u32 size = std::max<u32>(kBlock, (v->size + 0xFFFF) & ~0xFFFFu);
        const u32 block = main_.alloc(size, kBlock);
        if (!block) {
            PS_LOG_ERROR(kChan, "no main-area memory for the host variable {}::{}", module, v->name);
            return 0;
        }
        variable_block_ = block;
        variable_used_ = 0;
        at = block;
    }
    variable_used_ = at + v->size - variable_block_;
    u8* base = kernel_.memory().base();
    std::memset(base + at, 0, v->size);
    if (v->init) v->init(base, at);
    variables_[{std::string(module), nid}] = at;
    PS_LOG_DEBUG(kChan, "variable {}::{} ({:#010x}) at {:#010x}", module, v->name, nid, at);
    return at;
}

void HleProcess::link_import(std::string_view library, u32 nid, u32 address)
{
    std::lock_guard lock(link_mutex_);
    imports_[{std::string(library), nid}] = address;
}

u32 HleProcess::imported(std::string_view library, u32 nid) const
{
    std::lock_guard lock(link_mutex_);
    const auto it = imports_.find({std::string(library), nid});
    return it == imports_.end() ? 0 : it->second;
}

LinkedFunction* HleProcess::linked_at(u32 cia)
{
    // Read without the lock: linking happens before guest threads run (the dispatch table has the same rule).
    for (usize i = 0; i < chunks_.size(); i++) {
        const u32 base = chunks_[i].base;
        if (cia < base || cia >= base + kStubChunkSize) continue;
        const u32 off = cia - base;
        if (off % kStubEntrySize != 4) return nullptr;
        return linked_[i * kEntriesPerChunk + off / kStubEntrySize].get();
    }
    return nullptr;
}

std::vector<const LinkedFunction*> HleProcess::linked() const
{
    std::lock_guard lock(link_mutex_);
    std::vector<const LinkedFunction*> out;
    for (const auto& f : linked_)
        if (f) out.push_back(f.get());
    return out;
}

// ------------------------------------------------------------------------------------------------- TLS

bool HleProcess::initialize_tls(const Tls& tls)
{
    std::lock_guard lock(tls_mutex_);
    if (tls_initialized_) return false;
    // RPCS3 sys_initialize_tls: one 256 KB area in vm::main, the first slot starting 0x30 into it, as many slots of
    // (mem_size + 0x30) bytes as fit. Threads beyond that get their own allocation (ppu_alloc_tls).
    tls_ = tls;
    tls_slot_ = tls.mem_size + kTlsSystemArea;
    const u32 area = main_.alloc(kTlsAreaSize);
    if (!area) return false;
    tls_area_ = area + kTlsSystemArea;
    tls_used_.assign((kTlsAreaSize - kTlsSystemArea) / tls_slot_, false);
    tls_initialized_ = true;
    PS_LOG_INFO(kChan, "TLS initialized: area {:#010x}, {:#x} bytes per thread, {} threads", area, tls_slot_,
        tls_used_.size());
    return true;
}

u32 HleProcess::alloc_tls()
{
    u32 addr = 0;
    {
        std::lock_guard lock(tls_mutex_);
        if (!tls_initialized_) return 0;
        for (usize i = 0; i < tls_used_.size(); i++) {
            if (!tls_used_[i]) {
                tls_used_[i] = true;
                addr = tls_area_ + static_cast<u32>(i) * tls_slot_;
                break;
            }
        }
    }
    if (!addr) addr = main_.alloc(tls_slot_);
    if (!addr) return 0;
    // RPCS3 ppu_alloc_tls: clear the system area, copy the image, zero the rest.
    GuestMemory& mem = kernel_.memory();
    std::memset(mem.host(addr), 0, kTlsSystemArea);
    if (tls_.image_size) std::memcpy(mem.host(addr + kTlsSystemArea), mem.host(tls_.image), tls_.image_size);
    std::memset(mem.host(addr + kTlsSystemArea + tls_.image_size), 0, tls_.mem_size - tls_.image_size);
    return addr;
}

void HleProcess::free_tls(u32 addr)
{
    std::lock_guard lock(tls_mutex_);
    if (addr >= tls_area_ && tls_slot_ && (addr - tls_area_) % tls_slot_ == 0 &&
        (addr - tls_area_) / tls_slot_ < tls_used_.size()) {
        tls_used_[(addr - tls_area_) / tls_slot_] = false;
        return;
    }
    main_.free(addr);
}

} // namespace pspocket::hle
