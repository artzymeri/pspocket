#pragma once

// Per-process HLE state: the main-area allocator, the import linkage (host functions reachable from guest code) and
// the TLS template. One HleProcess exists while a guest process runs; HLE functions reach it with current().

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::lv2 {
class Kernel;
}

namespace pspocket::hle {

// Allocations inside the main executable's area (RPCS3 vm::main: vm::falloc for ELF segments, vm::alloc for HLE
// data such as descriptors and TLS). Committed and zeroed. Thread-safe.
class MainMemory {
public:
    MainMemory(GuestMemory& memory, u32 begin, u32 end);

    // Marks [addr, addr + size) used without committing it (the loader commits ELF segments itself).
    bool reserve(u32 addr, u32 size);
    // First fit at `align` (a power of two, RPCS3's vm::alloc default 64 KB). Returns 0 when nothing fits.
    u32 alloc(u32 size, u32 align = 0x10000);
    void free(u32 addr);

private:
    GuestMemory& memory_;
    const u32 begin_, end_;
    std::mutex mutex_;
    std::vector<std::pair<u32, u32>> used_; // sorted [start, end)
};

// One imported function bound to a stub. Calls are counted (relaxed) for reports.
struct LinkedFunction {
    std::string module;
    std::string name; // from the NID database when known, else empty
    u32 nid = 0;
    u32 descriptor = 0; // guest address of the 8-byte entry: {func = descriptor + 4, toc = 0}
    const Function* impl = nullptr;
    std::atomic<u64> calls{0};
};

struct ProcessConfig {
    // Abort on the first call to an unimplemented function instead of logging and returning CELL_ENOSYS.
    bool abort_on_unimplemented = false;
};

class HleProcess {
public:
    HleProcess(lv2::Kernel& kernel, u32 main_begin, u32 main_end, ProcessConfig config = {});
    ~HleProcess();
    HleProcess(const HleProcess&) = delete;
    HleProcess& operator=(const HleProcess&) = delete;

    // Makes this the process current() returns and the target of HLE stub calls.
    void install();
    static HleProcess* current();

    lv2::Kernel& kernel() { return kernel_; }
    MainMemory& main_memory() { return main_; }
    const ProcessConfig& config() const { return config_; }

    // Returns the descriptor address to store in an import slot for (module, nid). The same pair always gets the same
    // descriptor. `name` is only used for logs. Must be called before guest threads run (it registers dispatch
    // entries); 0 when the main area is full.
    u32 link_function(std::string_view module, u32 nid, std::string_view name);

    // The linked function whose stub is at code address `cia` (descriptor + 4), or nullptr.
    LinkedFunction* linked_at(u32 cia);
    std::vector<const LinkedFunction*> linked() const;
    // Calls that reached a function without a host implementation.
    u64 unimplemented_calls() const { return unimplemented_calls_.load(std::memory_order_relaxed); }

    // ---- TLS (RPCS3 Emu/Cell/Modules/sys_ppu_thread_.cpp) ----
    struct Tls {
        u32 image = 0;      // TLS image in the executable
        u32 image_size = 0; // p_filesz
        u32 mem_size = 0;   // p_memsz
    };
    void set_tls_template(const Tls& tls) { tls_ = tls; }
    const Tls& tls_template() const { return tls_; }
    // sys_initialize_tls: sets up the per-thread TLS area once. Returns false if it was already set up.
    bool initialize_tls(const Tls& tls);
    // A thread's TLS block (system area + image + zeroed rest); r13 = result + 0x7030. 0 when out of memory.
    u32 alloc_tls();
    void free_tls(u32 addr);

    // Used by the stub trampoline.
    void on_unimplemented() { unimplemented_calls_.fetch_add(1, std::memory_order_relaxed); }

private:
    struct Chunk {
        u32 base = 0;
        u32 used = 0; // entries
    };

    lv2::Kernel& kernel_;
    ProcessConfig config_;
    MainMemory main_;

    mutable std::mutex link_mutex_;
    std::vector<Chunk> chunks_;
    std::vector<std::unique_ptr<LinkedFunction>> linked_; // index = chunk * kEntriesPerChunk + entry
    std::atomic<u64> unimplemented_calls_{0};

    Tls tls_{};
    std::mutex tls_mutex_;
    bool tls_initialized_ = false;
    u32 tls_area_ = 0;  // start of the first slot's system area
    u32 tls_slot_ = 0;  // bytes per thread: mem_size + 0x30
    std::vector<bool> tls_used_;
};

// The stub layout (RPCS3 PPUModule.cpp ppu_initialize_modules): 8 bytes per function, word 0 = descriptor + 4,
// word 1 = 0, which is both the descriptor's TOC and the stub's only instruction (illegal if it is ever executed
// without going through the dispatch table).
inline constexpr u32 kStubEntrySize = 8;
inline constexpr u32 kStubChunkSize = 0x10000;
inline constexpr u32 kEntriesPerChunk = kStubChunkSize / kStubEntrySize;

// TLS layout (RPCS3 sys_ppu_thread_.cpp): each thread's block starts with a 0x30-byte system area, then the image;
// r13 points 0x7000 bytes past the image start.
inline constexpr u32 kTlsSystemArea = 0x30;
inline constexpr u32 kTlsR13Offset = 0x7000 + kTlsSystemArea;
inline constexpr u32 kTlsAreaSize = 0x40000;

} // namespace pspocket::hle
