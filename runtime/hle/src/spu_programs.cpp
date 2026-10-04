// SPU programs as SPURS loads them (spu_programs.hpp, D-076): the lifted program for each image, and the dump of the
// images that have none.

#include "spu_programs.hpp"

#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "pspocket/core/log.hpp"
#include "pspocket/spu/runtime.h"

namespace pspocket::hle::spurs {
namespace {

constexpr std::string_view kChan = "spurs";

const char* kind_name(ImageKind k)
{
    switch (k) {
    case ImageKind::Task: return "task";
    case ImageKind::Job: return "job";
    case ImageKind::PolicyModule: return "policy-module";
    }
    return "?";
}

struct Registry {
    std::mutex mutex;
    std::string dump_dir;
    std::unordered_set<u64> seen; // images without a lifted program, by key (hash ^ LS address)
};

Registry& registry()
{
    static Registry r;
    return r;
}

std::atomic<u64> g_attached{0}, g_unknown{0}, g_images{0};

// The file name of an image: its program key, as spu-lift names the program (see scripts/lift-spu.sh).
std::string image_name(u64 hash, u32 ls_start) { return std::format("spu_{:016x}_{:05x}", hash, ls_start); }

void dump(const std::string& dir, u8* base, const SpuImage& image, u64 hash)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string name = image_name(hash, image.ls_start);
    const std::filesystem::path bin = std::filesystem::path(dir) / (name + ".bin");
    if (std::filesystem::exists(bin, ec)) return; // an earlier run saved it
    {
        std::ofstream out(bin, std::ios::binary);
        out.write(reinterpret_cast<const char*>(base + image.guest), image.size);
        if (!out) {
            PS_LOG_ERROR(kChan, "--spu-dump: cannot write {}", bin.string());
            return;
        }
    }
    std::ofstream meta(std::filesystem::path(dir) / (name + ".txt"));
    meta << std::format("kind {}\nguest {:#x}\nbase {:#x}\nsize {:#x}\nentry {:#x}\n", kind_name(image.kind), image.guest,
        image.ls_start, image.size, image.entry);
}

} // namespace

void attach_program(SPUContext& ctx, u8* base, const SpuImage& image)
{
    const u64 hash = ps_spu_hash64(base + image.guest, image.size);
    const ps_spu_program* program = ps_spu_find_program(hash, image.ls_start, image.size);
    ps_spu_attach_program(&ctx, program);
    if (program) {
        g_attached.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_unknown.fetch_add(1, std::memory_order_relaxed);
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    if (!r.seen.insert(hash ^ image.ls_start).second) return;
    g_images.fetch_add(1, std::memory_order_relaxed);
    // CLAUDE.md rule 5 asks dev builds to abort on an unknown SPU hash. Until the game's programs are collected
    // (D-076), every build logs it once and interprets.
    PS_LOG_INFO(kChan, "SPU {} {} (guest {:#x}, LS 0x{:05x}+{:#x}, entry 0x{:05x}) has no lifted program: interpreted",
        kind_name(image.kind), image_name(hash, image.ls_start), image.guest, image.ls_start, image.size, image.entry);
    if (!r.dump_dir.empty()) dump(r.dump_dir, base, image, hash);
}

} // namespace pspocket::hle::spurs

namespace pspocket::hle {

void set_spu_dump_dir(std::string dir)
{
    spurs::Registry& r = spurs::registry();
    std::lock_guard lock(r.mutex);
    r.dump_dir = std::move(dir);
}

SpuProgramStats spu_program_stats()
{
    return {spurs::g_attached.load(std::memory_order_relaxed), spurs::g_unknown.load(std::memory_order_relaxed),
        spurs::g_images.load(std::memory_order_relaxed)};
}

} // namespace pspocket::hle
