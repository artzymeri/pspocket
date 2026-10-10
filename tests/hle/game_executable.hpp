// A game's executable loaded, never started, so that the SDK library code it links statically can be called next to
// pspocket's own functions of the same names (D-904). Firmware modules are the reference where Sony ships one; the C
// library has none (a game links it into itself), so a game's copy is the reference there.
//
// PSPOCKET_TEST_AC_EBOOT names the executable (a decrypted EBOOT.elf; Assassin's Creed's, BLES00158, was used) and
// PSPOCKET_TEST_AC_LIBC_MAP a text file of "name 0xADDR" lines: the code addresses of the library's functions in that
// executable, found by reading it. Both are game data and stay in inventory/ (CLAUDE.md rule 1): nothing of the game
// is written into a test. Tests skip without them.
#pragma once

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp" // runtime/hle/src (internal): call_guest
#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/guest_image.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::test {

inline const char* game_eboot() { return std::getenv("PSPOCKET_TEST_AC_EBOOT"); }
inline const char* game_libc_map() { return std::getenv("PSPOCKET_TEST_AC_LIBC_MAP"); }

#define PS_REQUIRE_GAME_LIBC()                                                                                       \
    do {                                                                                                             \
        if (!::pspocket::test::game_eboot() || !::pspocket::test::game_libc_map())                                   \
            SKIP("PSPOCKET_TEST_AC_EBOOT and PSPOCKET_TEST_AC_LIBC_MAP are not set: no game's C library to compare "  \
                 "with");                                                                                            \
    } while (0)

struct GameExecutable {
    GuestMemory memory;
    lv2::Config cfg;
    loader::BootState state;
    std::unique_ptr<lv2::Kernel> kernel;
    std::unique_ptr<hle::HleProcess> hle;
    std::shared_ptr<loader::Executable> exe;
    std::vector<std::pair<u32, u32>> writable; // [vaddr, vaddr + memsz) of the writable segments
    std::map<std::string, u32, std::less<>> code; // the map: name -> code address
    std::map<std::string, u32, std::less<>> descs; // descriptors made for them
    PPUContext ctx{};
    lv2::PpuThread* thread = nullptr;
    u8* b = nullptr;
    u32 toc = 0;
    u32 heap = 0, heap_end = 0;

    GameExecutable() : GameExecutable(game_eboot(), game_libc_map()) {}
    GameExecutable(const char* path, const char* map_path)
    {
        REQUIRE(path);
        std::ifstream f(path, std::ios::binary);
        REQUIRE(f.good());
        std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto parsed = loader::Executable::parse(std::move(bytes));
        REQUIRE(parsed);
        exe = *parsed;
        REQUIRE(memory.reserve());
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        cfg = exe->kernel_config();
        loader::prepare_kernel_config(cfg, state);
        kernel = std::make_unique<lv2::Kernel>(memory, cfg);
        kernel->install();
        hle = std::make_unique<hle::HleProcess>(*kernel, PS_MEM_MAIN_BASE, cfg.main_area_end);
        hle->install();
        REQUIRE(loader::load(*exe, *kernel, *hle) == lv2::CELL_OK);
        b = memory.base();
        auto image = recomp::GuestImage::from_elf(exe->elf());
        REQUIRE(image);
        for (const auto& s : image->segments())
            if ((s.flags & 2) && s.memsz) writable.emplace_back(s.vaddr, s.vaddr + static_cast<u32>(s.memsz));
        // The entry point is a descriptor {code, TOC}: the TOC every function of the executable shares.
        toc = ps_read32(b, exe->entry_descriptor() + 4);
        REQUIRE(toc);
        // A thread with a stack and the executable's TLS (the C library keeps errno and its states there).
        const u32 stack = hle->main_memory().alloc(0x100000);
        heap = hle->main_memory().alloc(0x1000000);
        REQUIRE(stack);
        REQUIRE(heap);
        heap_end = heap + 0x1000000;
        ps_ppu_context_init(&ctx);
        ctx.gpr[1] = stack + 0x100000 - 0x200;
        ctx.gpr[2] = toc;
        thread = kernel->attach_thread(ctx, "compare");
        const auto& tls = exe->module().tls;
        const hle::Function* init = hle::registry().find("sysPrxForUser", recomp::nid_of("sys_initialize_tls"));
        REQUIRE(init);
        ctx.gpr[3] = thread->id;
        ctx.gpr[4] = tls ? tls->vaddr : 0;
        ctx.gpr[5] = tls ? tls->image_size : 0;
        ctx.gpr[6] = tls ? tls->size : 0;
        init->fn(ctx, b);
        if (map_path) {
            std::ifstream m(map_path);
            REQUIRE(m.good());
            std::string line;
            while (std::getline(m, line)) {
                std::istringstream in(line);
                std::string name, addr;
                if (!(in >> name >> addr) || name.empty() || name[0] == '#') continue;
                code[name] = static_cast<u32>(std::stoul(addr, nullptr, 0));
            }
        }
    }
    ~GameExecutable()
    {
        if (thread) kernel->detach_thread(thread);
        hle.reset();
        kernel.reset();
    }
    GameExecutable(const GameExecutable&) = delete;
    GameExecutable& operator=(const GameExecutable&) = delete;

    // Zeroed memory of the fixture's heap, 16-byte aligned.
    u32 alloc(u32 n)
    {
        const u32 a = (heap + 15) & ~15u;
        REQUIRE(a + n <= heap_end);
        heap = a + n;
        std::memset(b + a, 0, n);
        return a;
    }
    u32 cstring(std::string_view s)
    {
        const u32 p = alloc(static_cast<u32>(s.size()) + 1);
        std::memcpy(b + p, s.data(), s.size());
        return p;
    }
    std::string bytes(u32 a, u32 n) const { return std::string(reinterpret_cast<const char*>(b + a), n); }
    std::string cstr(u32 a) const { return std::string(reinterpret_cast<const char*>(b + a)); }

    // Guest code in the fixture's heap (one word per instruction) and a descriptor for it.
    u32 guest_code(std::initializer_list<u32> words)
    {
        const u32 c = alloc(4 * static_cast<u32>(words.size()));
        u32 a = c;
        for (const u32 w : words) {
            ps_write32(b, a, w);
            a += 4;
        }
        const u32 d = alloc(8);
        ps_write32(b, d, c);
        ps_write32(b, d + 4, toc);
        return d;
    }
    // A printf output function as the library's sprintf has it, void *prout(void *dst, const char *s, size_t n):
    // the bytes copied to dst, dst + n returned.
    u32 guest_prout()
    {
        return guest_code({
            0x7C661B78u, // mr r6,r3
            0x2C050000u, // cmpwi r5,0
            0x4182001Cu, // beq +0x1C
            0x7CA903A6u, // mtctr r5
            0x88E40000u, // lbz r7,0(r4)
            0x98E60000u, // stb r7,0(r6)
            0x38840001u, // addi r4,r4,1
            0x38C60001u, // addi r6,r6,1
            0x4200FFF0u, // bdnz -0x10
            0x7CC33378u, // mr r3,r6
            0x4E800020u, // blr
        });
    }
    // A va_list as the PPC64 ABI makes one: the arguments' doublewords one after another (a double's bits as one).
    u32 va_list_of(std::initializer_list<u64> args) { return va_list_of(std::span<const u64>(args.begin(), args.size())); }
    u32 va_list_of(std::span<const u64> args)
    {
        const u32 v = alloc(8 * static_cast<u32>(args.size()) + 8);
        u32 a = v;
        for (const u64 x : args) {
            ps_write64(b, a, x);
            a += 8;
        }
        return v;
    }

    // ---- the game's library
    bool has(std::string_view name) const { return code.find(name) != code.end(); }
    // A descriptor {code, TOC} for the game's function `name` (from the map).
    u32 game(std::string_view name)
    {
        if (const auto it = descs.find(name); it != descs.end()) return it->second;
        const auto it = code.find(name);
        INFO("the map has no " << name);
        REQUIRE(it != code.end());
        const u32 d = alloc(8);
        ps_write32(b, d, it->second);
        ps_write32(b, d + 4, toc);
        descs.emplace(std::string(name), d);
        return d;
    }
    // A call of the game's function, as guest code calls it; floating-point arguments go in ctx.fpr first.
    u64 call_game(std::string_view name, std::initializer_list<u64> args) { return hle::call_guest(ctx, game(name), args); }

    // ---- pspocket's
    const hle::Function* own(std::string_view module, std::string_view name) const
    {
        return hle::registry().find(module, recomp::nid_of(name));
    }
    // A call of pspocket's function of `module` through its registered body, its arguments in r3.. (the ninth and
    // later in the parameter save area, as guest code passes them).
    u64 call_own(std::string_view module, std::string_view name, std::initializer_list<u64> args)
    {
        const hle::Function* f = own(module, name);
        INFO(module << "::" << name);
        REQUIRE(f);
        const u64 sp = ctx.gpr[1];
        ctx.gpr[1] = (sp - 0x100) & ~u64{15};
        ps_write64(b, static_cast<u32>(ctx.gpr[1]), sp);
        u32 i = 0;
        for (const u64 a : args) {
            if (i < 8) ctx.gpr[3 + i] = a;
            else ps_write64(b, static_cast<u32>(ctx.gpr[1]) + 0x70 + 8 * (i - 8), a);
            i++;
        }
        f->fn(ctx, b);
        ctx.gpr[1] = sp;
        return ctx.gpr[3];
    }
    // A host variable's guest address (linked into this process on first use).
    u32 own_variable(std::string_view module, std::string_view name)
    {
        return hle->link_variable(module, recomp::nid_of(name));
    }
};

} // namespace pspocket::test
