#pragma once

// What the HLE libraries' parser fuzzing shares (D-603, tests/common/mutation_fuzz.hpp): guest memory with a guard
// after it, so a read or write past a game's buffer faults (the process ends with the guest address) instead of
// finding stale bytes; the image decoders' guest malloc and free; a temporary host folder.
//
// An input goes flush against the end of its committed area (place_input): the next pages are reserved in the main
// memory allocator and never committed. An output buffer the same way (place_output), so a write past its size
// faults too.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

#include "hle_test_util.hpp"
#include "mutation_fuzz.hpp"
#include "pspocket/core/be.h"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "pspocket/spu/runtime.h"

namespace pspocket::hle::test::fuzzing {

namespace fuzz = pspocket::test::fuzz;

// Guest code without lifted code (the tests' callbacks) runs in the interpreters while this lives (the default policy
// aborts).
struct InterpretMisses {
    const ps_ppu_miss_policy ppu = ps_ppu_get_miss_policy();
    const ps_spu_miss_policy spu = ps_spu_get_miss_policy();
    InterpretMisses()
    {
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    }
    ~InterpretMisses()
    {
        ps_ppu_set_miss_policy(ppu);
        ps_spu_set_miss_policy(spu);
    }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// Inputs up to 1 MB end at kInputEnd, outputs up to 1 MB at kOutputEnd; 64 KB guards after each.
inline constexpr u32 kInputEnd = 0x00600000, kOutputEnd = 0x00800000, kAreaSize = 0x00100000, kGuardSize = 0x10000;

inline void add_guarded_areas(Fixture& f)
{
    for (const u32 end : {kInputEnd, kOutputEnd}) {
        REQUIRE(f.hle->main_memory().reserve(end - kAreaSize, kAreaSize + kGuardSize));
        REQUIRE(f.mem.commit(end - kAreaSize, kAreaSize));
    }
}

// `bytes` against the end of the input area: their guest address.
inline u32 place_input(u8* base, std::span<const u8> bytes)
{
    REQUIRE(bytes.size() <= kAreaSize);
    const u32 at = kInputEnd - static_cast<u32>(bytes.size());
    if (!bytes.empty()) std::memcpy(base + at, bytes.data(), bytes.size());
    return at;
}

// `size` bytes against the end of the output area, filled with 0xCC: their guest address.
inline u32 place_output(u8* base, u32 size)
{
    REQUIRE(size <= kAreaSize);
    const u32 at = kOutputEnd - size;
    std::memset(base + at, 0xCC, size);
    return at;
}

// Guest code: a malloc callback (size r3, arg r4) handing out 16-byte aligned blocks from a bump pointer at arg + 0,
// counting them at arg + 4, and a free callback (ptr r3, arg r4) counting at arg + 8, returning 0 (the image decoders'
// CellXxxDecThreadInParam callbacks, as test_pngdec.cpp's without the logs). The bump pointer goes back to `heap` with
// reset_heap() between inputs.
struct GuestHeap {
    u32 code = 0, malloc_opd = 0, free_opd = 0, ctl = 0, heap = 0, size = 0;

    void write(u8* b) const
    {
        namespace as = pspocket::recomp::ppu::as;
        const std::vector<u32> words = {
            as::lwz(5, 0, 4), as::add(6, 5, 3), as::addi(6, 6, 15), as::rlwinm(6, 6, 0, 0, 27), as::stw(6, 0, 4),
            as::lwz(7, 4, 4), as::addi(7, 7, 1), as::stw(7, 4, 4), as::mr(3, 5), as::blr(),
            as::lwz(7, 8, 4), as::addi(7, 7, 1), as::stw(7, 8, 4), as::li(3, 0), as::blr(),
        };
        for (usize i = 0; i < words.size(); i++) ps_write32(b, code + 4 * static_cast<u32>(i), words[i]);
        ps_write32(b, malloc_opd, code);
        ps_write32(b, malloc_opd + 4, 0);
        ps_write32(b, free_opd, code + 10 * 4);
        ps_write32(b, free_opd + 4, 0);
        reset(b, heap);
    }
    void reset(u8* b, u32 next) const
    {
        ps_write32(b, ctl, next);
        ps_write32(b, ctl + 4, 0);
        ps_write32(b, ctl + 8, 0);
    }
    u32 next(u8* b) const { return ps_read32(b, ctl); }
    // Whether the decoder asked for more than the heap holds (the bump pointer went past it).
    bool overrun(u8* b) const { return next(b) > heap + size; }
};

// A temporary host folder, removed at the end.
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(std::string_view name)
        : path(std::filesystem::temp_directory_path() / std::format("pspocket-fuzz-{}-{}", name, ::getpid()))
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

inline void write_host_file(const std::filesystem::path& p, std::span<const u8> bytes)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline std::vector<u8> bytes_of(std::string_view s) { return std::vector<u8>(s.begin(), s.end()); }

} // namespace pspocket::hle::test::fuzzing
