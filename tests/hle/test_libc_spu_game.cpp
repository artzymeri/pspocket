// sys_libc part 6 (D-904) compared with a game's statically linked copy of the SDK's C library (game_executable.hpp):
// the SPU threads' local store accessors on a running SPU thread made here, and Sony's descriptor slots. test_hle
// "[gamelibc]"; skipped without the game's executable and its map.

#include <catch_amalgamated.hpp>

#include <bit>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "game_executable.hpp"
#include "spu_program.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/spu/asm.hpp"

using namespace pspocket;
using pspocket::test::GameExecutable;
namespace as = pspocket::recomp::spu::as;

namespace {

// The LS bytes [ls, ls + n) of an SPU thread, through lv2.
std::vector<u8> ls_bytes(GameExecutable& g, u32 spu, u32 ls, u32 n)
{
    std::vector<u8> out;
    const u32 v = g.alloc(8);
    for (u32 i = 0; i < n; i++) {
        REQUIRE(lv2::sys_spu_thread_read_ls(*g.thread, spu, ls + i, v, 1) == lv2::CELL_OK);
        out.push_back(static_cast<u8>(ps_read64(g.b, v)));
    }
    return out;
}

} // namespace

TEST_CASE("sys_libc's SPU thread accessors do what the game's do", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    pspocket::test::SpuProgram p;
    p.emit(as::br(0));
    std::vector<u8> fill(0x800);
    for (u32 i = 0; i < fill.size(); i++) fill[i] = static_cast<u8>(i * 37 + 11);
    p.data(0x1000, fill);
    p.data(0x2000, fill); // the game's writes go to 0x2000.., pspocket's to 0x3000..: the same bytes before
    p.data(0x3000, fill);
    const u32 scratch = g.alloc(0x4000);
    const auto group = pspocket::test::make_spu_group(*g.thread, g.b, scratch, p);
    REQUIRE(group.thread);
    REQUIRE(lv2::sys_spu_thread_group_start(*g.thread, group.group) == lv2::CELL_OK);
    const u32 spu = group.thread;
    u64 compared = 0;

    const char* reads[] = {"read_char", "read_short", "read_int", "read_long", "read_llong", "read_uchar",
        "read_ushort", "read_uint", "read_ulong", "read_ullong", "read_float", "read_double", "read_ldouble",
        "read_ptr"};
    for (const char* r : reads) {
        const std::string name = std::string("spu_thread_") + r;
        for (u32 ls = 0x1000; ls < 0x1010; ls++) {
            g.ctx.fpr[1] = 0;
            const u64 a = g.call_game(name, {spu, ls});
            const double fa = g.ctx.fpr[1];
            g.ctx.fpr[1] = 0;
            const u64 b = g.call_own("sys_libc", name, {spu, ls});
            const double fb = g.ctx.fpr[1];
            INFO(name << " at " << std::format("{:#x}", ls));
            // A word the SDK's library could not read leaves its stack's bytes: compare where lv2 reads it.
            const bool misaligned_word = ls % 4 && std::string(r).find("char") == std::string::npos &&
                                         std::string(r).find("short") == std::string::npos;
            if (misaligned_word) continue;
            // A float or double comes back in f1; r3 is whatever the function left there (the ABI leaves it).
            const bool fp = std::string(r).find("float") != std::string::npos ||
                            std::string(r).find("double") != std::string::npos;
            if (!fp) CHECK(a == b);
            CHECK(std::bit_cast<u64>(fa) == std::bit_cast<u64>(fb));
            compared++;
        }
    }
    // read_mem: byte by byte into the caller's memory.
    for (u32 n : {0u, 1u, 5u, 33u}) {
        const u32 da = g.alloc(64), db = g.alloc(64);
        const u64 ra = g.call_game("spu_thread_read_mem", {spu, 0x1003, da, n});
        const u64 rb = g.call_own("sys_libc", "spu_thread_read_mem", {spu, 0x1003, db, n});
        INFO("read_mem of " << n);
        CHECK(ra == rb);
        CHECK(g.bytes(da, 64) == g.bytes(db, 64));
        compared++;
    }

    // Writes: the same call on the game's area and on pspocket's, then the areas compared.
    struct W {
        const char* name;
        u32 off;
        u64 value;
        double f;
    };
    const W writes[] = {{"write_char", 0x01, 0x1AB, 0}, {"write_char", 0x07, 0x80, 0}, {"write_uchar", 0x0A, 0xFE, 0},
        {"write_short", 0x10, 0x1BEEF, 0}, {"write_short", 0x13, 0x8001, 0}, {"write_ushort", 0x16, 0x7FFF, 0},
        {"write_int", 0x20, 0xFFFFFFFFCAFEBABEull, 0}, {"write_long", 0x24, 0x80000001, 0},
        {"write_uint", 0x28, 0x1122334455667788ull, 0}, {"write_ulong", 0x2C, 0x99AABBCC, 0},
        {"write_ptr", 0x30, 0x10203040, 0}, {"write_llong", 0x38, 0x0102030405060708ull, 0},
        {"write_ullong", 0x40, 0xF1F2F3F4F5F6F7F8ull, 0}, {"write_float", 0x48, 0, 3.25},
        {"write_double", 0x50, 0, -1e-300}, {"write_ldouble", 0x58, 0, 12345.678},
        {"write_int", 0x61, 7, 0}, {"write_llong", 0x66, 9, 0}};
    for (const W& w : writes) {
        const std::string name = std::string("spu_thread_") + w.name;
        g.ctx.fpr[1] = w.f;
        const u64 a = g.call_game(name, {spu, 0x2000 + w.off, w.value});
        g.ctx.fpr[1] = w.f;
        const u64 b = g.call_own("sys_libc", name, {spu, 0x3000 + w.off, w.value});
        INFO(name << " at +" << std::format("{:#x}", w.off));
        CHECK(static_cast<s32>(a) == static_cast<s32>(b));
        compared++;
    }
    // write_mem from the caller's memory, of every length to 9 and two longer ones.
    u32 off = 0x100;
    for (u32 n : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 17u, 64u}) {
        const u32 src = g.alloc(64);
        for (u32 i = 0; i < 64; i++) g.b[src + i] = static_cast<u8>(0xA0 + i + n);
        const u64 a = g.call_game("spu_thread_write_mem", {spu, 0x2000 + off, src, n});
        const u64 b = g.call_own("sys_libc", "spu_thread_write_mem", {spu, 0x3000 + off, src, n});
        INFO("write_mem of " << n);
        CHECK(static_cast<s32>(a) == static_cast<s32>(b));
        off += 0x50;
        compared++;
    }
    CHECK(ls_bytes(g, spu, 0x2000, 0x800) == ls_bytes(g, spu, 0x3000, 0x800));
    REQUIRE(lv2::sys_spu_thread_group_terminate(*g.thread, group.group, 0) == lv2::CELL_OK);
    // Once the group has ended lv2 refuses the store: the writes answer the same error.
    CHECK(static_cast<s32>(g.call_game("spu_thread_write_int", {spu, 0x2000, 1})) ==
          static_cast<s32>(g.call_own("sys_libc", "spu_thread_write_int", {spu, 0x3000, 1})));
    WARN(compared << " calls compared");
}

TEST_CASE("sys_libc's descriptor slots are the game's", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    g.call_game("_fd_table_init", {}); // the game's start-up fills its table
    const std::vector<std::vector<s64>> script = {{1, 10, 7}, {1, 11, 0}, {0, 3}, {1, 12, 0}, {0, 0}, {1, 13, 0},
        {0, 256}, {0, -1}, {0, 255}, {1, 14, 2}};
    for (const auto& s : script) {
        const char* name = s[0] ? "_get_fd" : "_put_fd";
        const u64 a = s[0] ? g.call_game(name, {static_cast<u64>(s[1]), static_cast<u64>(s[2])})
                           : g.call_game(name, {static_cast<u64>(s[1])});
        const u64 b = s[0] ? g.call_own("sys_libc", name, {static_cast<u64>(s[1]), static_cast<u64>(s[2])})
                           : g.call_own("sys_libc", name, {static_cast<u64>(s[1])});
        INFO(name << " " << s[1]);
        if (s[0]) CHECK(static_cast<s32>(a) == static_cast<s32>(b));
    }
    s32 ga = 0, gb = 0;
    while (static_cast<s32>(g.call_game("_get_fd", {5, 5})) != -1) ga++;
    while (static_cast<s32>(g.call_own("sys_libc", "_get_fd", {5, 5})) != -1) gb++;
    CHECK(ga == gb);
}
