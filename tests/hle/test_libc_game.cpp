// sys_libc compared with a game's statically linked copy of the SDK's C library (D-904): see game_executable.hpp. The
// tests run with test_hle "[gamelibc]" and skip without the game's executable and its map.

#include <catch_amalgamated.hpp>

#include <bit>
#include <format>
#include <string>
#include <vector>

#include "game_executable.hpp"

using namespace pspocket;
using pspocket::test::GameExecutable;

TEST_CASE("sys_libc's _Printf writes what the game's _Printf writes", "[gamelibc][libc]")
{
    PS_REQUIRE_GAME_LIBC();
    GameExecutable g;
    const u32 prout = g.guest_prout();
    struct Case {
        const char* fmt;
        std::vector<u64> args;
    };
    const u32 hi = g.cstring("hi");
    const Case cases[] = {
        {"%d %s|%x|%5.2f|%-6d|%+.3e|%g", {42, hi, 255, std::bit_cast<u64>(3.14159), 7, std::bit_cast<u64>(-1234.5678),
                                              std::bit_cast<u64>(0.0001)}},
        {"%%|%c|%05d|%#o|%#X|%lld|%hhd", {'A', 33, 8, 0xBEEF, static_cast<u64>(-5), 0x1FF}},
        {"%.0f %.1f %.20f", {std::bit_cast<u64>(0.5), std::bit_cast<u64>(0.25), std::bit_cast<u64>(0.1)}},
        {"%a %A %e", {std::bit_cast<u64>(1.0), std::bit_cast<u64>(-0.75), std::bit_cast<u64>(1e300)}},
    };
    for (const Case& c : cases) {
        const u32 fmt = g.cstring(c.fmt);
        const u32 ap_game = g.va_list_of(c.args), ap_own = g.va_list_of(c.args);
        const u32 out_game = g.alloc(512), out_own = g.alloc(512);
        const s32 n_game = static_cast<s32>(g.call_game("_Printf", {prout, out_game, fmt, ap_game}));
        const s32 n_own = static_cast<s32>(g.call_own("sys_libc", "_Printf", {prout, out_own, fmt, ap_own}));
        INFO(c.fmt);
        CHECK(n_own == n_game);
        CHECK(g.cstr(out_own) == g.cstr(out_game));
    }
}
