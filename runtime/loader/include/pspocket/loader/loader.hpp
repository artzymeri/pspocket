#pragma once

// Process loader: a PPU executable (decrypted ELF) -> a running lv2 process with its imports linked to HLE stubs.
//
//     auto exe = loader::Executable::parse(bytes);         // bytes stay owned by the Executable
//     lv2::Config cfg = exe->kernel_config();
//     loader::BootState boot;                               // must outlive the process
//     loader::prepare_kernel_config(cfg, boot);             // installs the main-thread executor
//     lv2::Kernel kernel(memory, cfg);  kernel.install();
//     hle::HleProcess hle(kernel, PS_MEM_MAIN_BASE, cfg.main_area_end);  hle.install();
//     u32 main_id;  loader::boot(*exe, kernel, hle, boot, {}, main_id);
//     kernel.wait_for_exit();
//
// Semantics follow RPCS3 rpcs3/Emu/Cell/PPUModule.cpp ppu_load_exec with liblv2 in HLE mode: LOAD segments at their
// p_vaddr, imports linked (ppu_load_imports, ppu_patch_refs), a main thread with the primary priority and stack size
// of sys_process_param, sys_initialize_tls (sysPrxForUser HLE) before the entry, and the entry called with
// r3 = argc, r4 = argv, r5 = envp, r6 = envc, r7 = thread ID, r8..r10 = TLS image / size / memory size,
// r11 = e_entry, r12 = malloc page size. argv and envp (arrays of u64 pointers) and their strings sit at the top of the
// main thread's stack, r1 moved below them.
//
// Deviations: PRX modules (LLE firmware libraries, the game's own) are not loaded yet; segments are writable (RPCS3
// write-protects read-only LOAD segments); the main thread's stack is taken from the default memory container but
// the executable's segments are not (RPCS3 takes both).

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/ppu_module.hpp"
#include "pspocket/recomp/result.hpp"

namespace pspocket::hle {
class HleProcess;
}

namespace pspocket::loader {

class Executable {
public:
    // Parses an ELF executable (not a SELF: decrypt first) and its module tables.
    static recomp::Result<std::shared_ptr<Executable>> parse(std::vector<u8> bytes);

    const recomp::ElfFile& elf() const { return *elf_; }
    const recomp::PpuModule& module() const { return module_; }
    u32 entry_descriptor() const { return static_cast<u32>(elf_->header().entry); }

    u32 sdk_version() const;
    s32 primary_prio() const;
    u32 primary_stack_size() const; // decoded from sys_process_param (RPCS3 ppu_load_exec "Fix primary stack size")
    u32 malloc_pagesize() const;

    // lv2 configuration derived from sys_process_param (SDK version, main area end).
    lv2::Config kernel_config(lv2::Config base = {}) const;

private:
    Executable() = default;
    std::vector<u8> bytes_;
    std::unique_ptr<recomp::ElfFile> elf_;
    recomp::PpuModule module_;
};

struct BootOptions {
    // argv[0] is the executable's guest path, as lv2 passes it.
    std::vector<std::string> argv = {"/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"};
    std::vector<std::string> envp;
};

// Shared between boot() and the main thread's executor. Keep it alive until the process has exited.
struct BootState {
    std::shared_ptr<const Executable> exe;
    BootOptions options;
    u32 main_thread_id = 0;
    std::atomic<bool> main_prepared{false};
    // The executor every thread finally runs (default: lv2::default_executor).
    lv2::Executor inner = lv2::default_executor;
    void* inner_user = nullptr;
};

// Wraps cfg.executor so the main thread runs sys_initialize_tls and receives its arguments before the entry.
void prepare_kernel_config(lv2::Config& cfg, BootState& state);

struct LoadReport {
    u32 segments = 0;
    u32 functions_linked = 0;
    u32 functions_implemented = 0; // linked to an HLE implementation
    u32 variables_unresolved = 0;
};

// Maps the segments and links the imports. No guest code runs.
lv2::Result load(const Executable& exe, lv2::Kernel& kernel, hle::HleProcess& hle, LoadReport* report = nullptr);

// Creates and starts the main thread of a loaded executable.
lv2::Result start_main_thread(std::shared_ptr<const Executable> exe, lv2::Kernel& kernel, BootState& state,
    const BootOptions& options, u32& main_thread_id);

// load() then start_main_thread().
lv2::Result boot(std::shared_ptr<const Executable> exe, lv2::Kernel& kernel, hle::HleProcess& hle, BootState& state,
    const BootOptions& options, u32& main_thread_id, LoadReport* report = nullptr);

} // namespace pspocket::loader
