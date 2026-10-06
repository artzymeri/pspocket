#pragma once

// Per-process HLE state: the main-area allocator, the import linkage (host functions reachable from guest code) and
// the TLS template. One HleProcess exists while a guest process runs; HLE functions reach it with current().

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
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
    const StubFunction* stub = nullptr; // with no impl: answered as RPCS3's stub (D-182)
    std::atomic<u64> calls{0};
};

// The running game, for cellGame: how it was started and its PARAM.SFO. A disc game has it in
// /dev_bdvd/PS3_GAME, an HDD game in /dev_hdd0/game/<title_id>.
struct GameInfo {
    enum class Boot : u8 { Disc, Hdd };
    Boot boot = Boot::Disc;
    std::string title_id;      // "BLES01807"; empty if unknown
    std::vector<u8> param_sfo; // PARAM.SFO as on the disc; empty if unknown
};

// Modules the game loads while it runs (sys_prx_load_module and the rest, D-159): the loader's (runtime/loader
// RuntimeModules), installed in the process before guest threads run. Each call returns CELL_OK or a CELL_PRX_ERROR /
// CELL_E* code; the HLE functions in sys_prx.cpp run the start and stop functions on the calling thread.
// What sys_prx_get_module_info reports of a loaded module (RPCS3 lv2_prx).
struct ModuleInfo {
    std::string name, path;
    std::array<u8, 2> version{};
    u16 attributes = 0;
    u32 start = 0, stop = 0; // module_start, module_stop descriptors
    struct Segment {
        u32 base = 0;
        u64 filesz = 0, memsz = 0;
    };
    std::vector<Segment> segments;
    std::pair<u32, u32> exports{}, imports{};
};

class ModuleLoader {
public:
    virtual ~ModuleLoader() = default;
    // Places and links the module at guest path `path`; its ID in *id.
    virtual u32 load(std::string_view path, u32* id) = 0;
    // The same for a module that starts `offset` bytes into the file (sys_prx_load_module_by_fd: one inside an
    // archive). Loaders without it take offset 0 only.
    virtual u32 load_at(std::string_view path, u64 offset, u32* id) { return offset ? 0x80011881 : load(path, id); }
    // The loaded module whose segments hold `addr`; 0 when none.
    virtual u32 id_by_address(u32) { return 0; }
    // The loaded modules' IDs, oldest first.
    virtual std::vector<u32> list() { return {}; }
    // A loaded module's information; false for an unknown ID.
    virtual bool info(u32, ModuleInfo&) { return false; }
    // Its exports linked into the imports waiting for them; *entry its module_start descriptor (0: none).
    virtual u32 start(u32 id, u32* entry) = 0;
    // module_start returned `result` (SYS_PRX_RESIDENT 0: it stays; else it goes).
    virtual void started(u32 id, s32 result) = 0;
    virtual u32 stop(u32 id, u32* entry) = 0;
    virtual void stopped(u32 id, s32 result) = 0;
    virtual u32 unload(u32 id) = 0;
    // By the name in its module info; 0 when none.
    virtual u32 id_by_name(std::string_view name) = 0;
    // The module_start descriptors of firmware modules loaded for the modules loaded so far that have not run
    // (dependencies first), each returned once: sys_prx_start_module runs them before the module's own (D-175).
    virtual std::vector<u32> take_dependency_starts() { return {}; }
};

struct ProcessConfig {
    // Abort on the first call to an unimplemented function instead of logging and returning CELL_ENOSYS.
    bool abort_on_unimplemented = false;
    GameInfo game;
    // Start the native SPURS scheduler's workers with each SPURS instance (they run the SPU tasks). Tests of the PPU
    // API alone turn them off.
    bool spurs_workers = true;
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
    // descriptor. `name` is only used for logs. 0 when the main area is full. While guest threads run (a module the
    // game loads, D-159) it is safe for the first kPreparedChunks chunks of stubs: the tables linked_at reads without
    // the lock never move before then.
    u32 link_function(std::string_view module, u32 nid, std::string_view name);

    // Links every internal function (Function::internal) of the modules in `modules` (the game's imports). Same rule
    // as link_function: before guest threads run. Returns false when the main area is full.
    bool link_internal_functions(const std::vector<std::string>& modules);
    // The descriptor of an already linked function, or 0. Safe while guest threads run.
    u32 descriptor_of(std::string_view module, u32 nid) const;

    // The linked function whose stub is at code address `cia` (descriptor + 4), or nullptr.
    LinkedFunction* linked_at(u32 cia);
    // Modules loaded at run time (D-159); null when nothing installed one.
    void set_module_loader(ModuleLoader* loader) { module_loader_ = loader; }
    ModuleLoader* module_loader() const { return module_loader_; }
    std::vector<const LinkedFunction*> linked() const;
    // Calls that reached a function without a host implementation (and no RPCS3 stub, D-182).
    u64 unimplemented_calls() const { return unimplemented_calls_.load(std::memory_order_relaxed); }
    // Calls answered as RPCS3's stub answers them (D-182).
    u64 stub_calls() const { return stub_calls_.load(std::memory_order_relaxed); }

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
    void on_stub() { stub_calls_.fetch_add(1, std::memory_order_relaxed); }

    // Per-process state of an HLE module (RPCS3 g_fxo): created on first use (from this process when T has a
    // constructor taking HleProcess&), destroyed with the process. T does its own locking.
    template <class T>
    T& state()
    {
        std::lock_guard lock(state_mutex_);
        std::shared_ptr<void>& slot = states_[std::type_index(typeid(T))];
        if (!slot) {
            if constexpr (std::is_constructible_v<T, HleProcess&>) slot = std::make_shared<T>(*this);
            else slot = std::make_shared<T>();
        }
        return *static_cast<T*>(slot.get());
    }

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
    ModuleLoader* module_loader_ = nullptr;
    std::atomic<u64> unimplemented_calls_{0};
    std::atomic<u64> stub_calls_{0};

    Tls tls_{};
    std::mutex tls_mutex_;
    bool tls_initialized_ = false;
    u32 tls_area_ = 0;  // start of the first slot's system area
    u32 tls_slot_ = 0;  // bytes per thread: mem_size + 0x30
    std::vector<bool> tls_used_;

    std::mutex state_mutex_;
    std::unordered_map<std::type_index, std::shared_ptr<void>> states_;
};

// The stub layout (RPCS3 PPUModule.cpp ppu_initialize_modules): 8 bytes per function, word 0 = descriptor + 4,
// word 1 = 0, which is both the descriptor's TOC and the stub's only instruction (illegal if it is ever executed
// without going through the dispatch table).
inline constexpr u32 kStubEntrySize = 8;
inline constexpr u32 kStubChunkSize = 0x10000;
inline constexpr u32 kEntriesPerChunk = kStubChunkSize / kStubEntrySize;
inline constexpr u32 kPreparedChunks = 8; // 65,536 linked functions before the stub tables may move

// TLS layout (RPCS3 sys_ppu_thread_.cpp): each thread's block starts with a 0x30-byte system area, then the image;
// r13 points 0x7000 bytes past the image start.
inline constexpr u32 kTlsSystemArea = 0x30;
inline constexpr u32 kTlsR13Offset = 0x7000 + kTlsSystemArea;
inline constexpr u32 kTlsAreaSize = 0x40000;

} // namespace pspocket::hle
