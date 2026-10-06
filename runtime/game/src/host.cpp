#include "pspocket/game/host.hpp"

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>

#include "pspocket/codesign/load.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/fp.h"

namespace pspocket::game {
namespace {

constexpr const char* kChan = "game";

std::string hex(u64 v) { return std::format("{:016x}", v); }

} // namespace

const ps_game_runtime& host_runtime()
{
    static const ps_game_runtime rt = {
        .layout = PS_GAME_LIBRARY_LAYOUT,
        .size = sizeof(ps_game_runtime),
        .abi_hash = ps_game_abi_hash,
        .ppu_dispatch = &ps_ppu_dispatch,
        .ppu_dispatch_miss_stub = ps_ppu_dispatch_miss_stub,
        .ppu_dispatch_register_table = ps_ppu_dispatch_register_table,
        .ppu_dispatch_unregister = ps_ppu_dispatch_unregister,
        .ppu_jump_miss = ps_ppu_jump_miss,
        .ppu_jump_table_miss = ps_ppu_jump_table_miss,
        .ppu_interp_one = ps_ppu_interp_one,
        .ppu_interp_until = ps_ppu_interp_until,
        .ppu_illegal = ps_ppu_illegal,
        .ppu_syscall = ps_ppu_syscall,
        .ppu_trap = ps_ppu_trap,
        .ppu_timebase = ps_ppu_timebase,
        .ppu_lwarx = ps_ppu_lwarx,
        .ppu_ldarx = ps_ppu_ldarx,
        .ppu_stwcx = ps_ppu_stwcx,
        .ppu_stdcx = ps_ppu_stdcx,
        .ppu_lswx = ps_ppu_lswx,
        .ppu_stswx = ps_ppu_stswx,
        .spu_register_program = ps_spu_register_program,
        .spu_rdch = ps_spu_rdch,
        .spu_wrch = ps_spu_wrch,
        .spu_rchcnt = ps_spu_rchcnt,
        .spu_stop = ps_spu_stop,
        .spu_halt = ps_spu_halt,
        .spu_illegal = ps_spu_illegal,
        .spu_events_pending = ps_spu_events_pending,
        .spu_set_interrupts = ps_spu_set_interrupts,
        .spu_frest_fraction_lut = ps_spu_frest_fraction_lut,
        .spu_frest_exponent_lut = ps_spu_frest_exponent_lut,
        .spu_frsqest_fraction_lut = ps_spu_frsqest_fraction_lut,
        .spu_frsqest_exponent_lut = ps_spu_frsqest_exponent_lut,
    };
    return rt;
}

size_t remove_stale_copies(const std::filesystem::path& dir)
{
    // The copies of processes that ended (kill(pid, 0) finds no such process; another app's or user's answers EPERM
    // and stays). A process keeps its own while it runs: the library is mapped from it. Nothing removed them before:
    // a game, the system library (232 MB with every SPU program, D-191) and each test made one per load, and the
    // Mac's disk filled with 24 GB of them.
    size_t removed = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (!name.starts_with("pspocket-game-") || !name.ends_with(".dylib")) continue;
        const long pid = std::strtol(name.c_str() + 14, nullptr, 10);
        if (pid <= 0 || pid == getpid() || kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) continue;
        if (std::filesystem::remove(e.path(), ec)) removed++;
    }
    return removed;
}

bool load_library(const std::string& path, const GuestMemory& memory, LoadedLibrary* out, std::string* error,
    const LoadOptions& options)
{
    namespace fs = std::filesystem;
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    // A fresh copy, so the kernel reads this file's signature and not one it cached for an earlier file there.
    static std::atomic<int> counter{0};
    const fs::path copy = fs::temp_directory_path() /
        std::format("pspocket-game-{}-{}.dylib", getpid(), counter.fetch_add(1));
    std::error_code ec;
    remove_stale_copies(copy.parent_path());
    fs::copy_file(path, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail(std::format("cannot copy {}: {}", path, ec.message()));
    std::string steps;
    codesign::register_signature(copy.string(), &steps);
    void* handle = dlopen(copy.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* why = dlerror();
        return fail(std::format("{}: {}; dlopen refused: {}", path, steps, why ? why : "?"));
    }
    const auto entry = reinterpret_cast<ps_game_library_entry>(dlsym(handle, PS_GAME_LIBRARY_ENTRY));
    if (!entry) return fail(std::format("{} is not a game library (no {})", path, PS_GAME_LIBRARY_ENTRY));
    const ps_game_library* lib = entry(&host_runtime());
    if (!lib || lib->layout != PS_GAME_LIBRARY_LAYOUT || lib->size != sizeof(ps_game_library))
        return fail(std::format("{} has another library layout ({}, this pspocket {}): prepare the game again", path,
            lib ? lib->layout : 0, PS_GAME_LIBRARY_LAYOUT));
    if (lib->abi_hash != ps_game_abi_hash)
        return fail(std::format("{} was made for another runtime (ABI {}, this pspocket {}): prepare the game again",
            path, hex(lib->abi_hash), hex(ps_game_abi_hash)));
    if (!lib->ppu_register && lib->ppu_function_count != 0)
        return fail(std::format("{} did not bind to this runtime", path));
    if (options.executable_sha256 && std::memcmp(options.executable_sha256, lib->executable_sha256, 32) != 0)
        return fail(std::format("{} ({}) was made from something else", path, lib->title_id ? lib->title_id : "?"));
    // The executable it was translated from only (D-098): the sampled words must be in guest memory.
    for (u32 i = 0; options.check_image_words && i < lib->ppu_image_word_count; i++) {
        const u32 addr = lib->ppu_image_words[2 * i], word = lib->ppu_image_words[2 * i + 1];
        if (!memory.is_committed(addr) || ps_read32(memory.base(), addr) != word)
            return fail(std::format("{} ({}) is for another executable: the word at {:#010x} differs", path,
                lib->title_id ? lib->title_id : "?", addr));
    }
    if (lib->ppu_register) lib->ppu_register();
    if (lib->spu_register) lib->spu_register();
    const bool current = lib->translator_hash == ps_game_translator_hash;
    PS_LOG_INFO(kChan, "game library {} ({}, made by {}): {} PPU functions, {} SPU programs; {}{}", path,
        lib->title_id ? lib->title_id : "?", lib->made_by ? lib->made_by : "?", lib->ppu_function_count,
        lib->spu_program_count, steps.empty() ? "loaded" : steps,
        current ? "" : "; translated by other lifters than this build's (preparing it again would update it)");
    if (out) *out = {lib, path, current};
    return true;
}

} // namespace pspocket::game
