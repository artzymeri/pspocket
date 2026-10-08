// sysPrxForUser against liblv2, Sony's own module of the library, from the user's installed system software (D-193's
// method, D-501): the same calls with made-up arguments go to both, and the answers and every byte written must be the
// same. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp); no game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kText = kData + 0x1000;  // the format or path
constexpr u32 kSlots = kData + 0x2000; // a va_list's 64-bit slots
constexpr u32 kStrs = kData + 0x3000;  // %s arguments
constexpr u32 kOut = kData + 0x8000;   // the output, then a guard
constexpr u32 kOutSize = 0x200, kGuard = 0x40;

u64 dbl(double v) { return std::bit_cast<u64>(v); }

struct Case {
    std::string fmt;
    std::vector<u64> args; // 64-bit slots as a caller passes them; doubles as their bits
};

// Made-up formats over every conversion, flag, width, precision and length the formatters may differ on.
std::vector<Case> cases()
{
    const u64 s1 = kStrs, s2 = kStrs + 0x40, s3 = kStrs + 0x80;
    return {
        {"plain text, no conversions", {}},
        {"%d %i %u", {42, static_cast<u64>(-7), 0xFFFFFFFFu}},
        {"%d|%i", {0xFFFFFFFF80000000ull, 0x123456789ull}},
        {"%x %X %o %#x %#X %#o %#o", {0xBEEF, 0xBEEF, 8, 0x1F, 0x1F, 8, 0}},
        {"%5d|%-5d|%05d|%+d|% d|%+5d|%-+5d|%.3d|%8.3d|%-8.3d|%08.3d", {42, 42, 42, 42, 42, 42, 42, 7, 7, 7, 7}},
        {"%.0d|%.0x|%#.0x|%#.0o|%5.0d|", {0, 0, 0, 0, 0}},
        {"%hd %hu %hhd %hhu %hx", {0x12345, 0x18000, 0x1FF, 0x1FF, 0xABCDEF}},
        {"%ld %lu %lx %lX", {0x123456789ull, 0xFFFFFFFFFFFFFFFFull, 0x123456789ABCull, 0xFFFFFFFF00000001ull}},
        {"%lld %llu %llx %llX %lli", {0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x123456789ABCDEFull, 1, 2}},
        {"%zd %zu %zx %td %jd %ju", {static_cast<u64>(-1), 0x100000000ull + 5, 0xABC, 3, 0x8000000000000000ull, 9}},
        {"%c|%3c|%-3c|", {'A', 'B', 'C'}},
        {"%s|%10s|%-10s|%.2s|%10.2s|%s", {s1, s2, s1, s3, s3, 0}},
        {"%p %p", {0x1234, 0}},
        {"100%% %%d %5%|", {}},
        {"%f %F %e %E %g %G", {dbl(3.14159265), dbl(-2.5), dbl(12345.678), dbl(0.000123), dbl(1e-10), dbl(123456789.0)}},
        {"%.0f %.1f %.10f %10.3f %-10.3f| %+f % f %010.2f", {dbl(2.5), dbl(0.05), dbl(1.0 / 3), dbl(3.14159),
                                                               dbl(3.14159), dbl(1), dbl(1), dbl(-1.5)}},
        {"%#.0f %#g %g %g %g", {dbl(3), dbl(1.5), dbl(100000), dbl(1000000), dbl(0.0001)}},
        {"%a %A %.3a", {dbl(1.0), dbl(-0.5), dbl(3.14)}},
        {"%f %f %f %e", {dbl(1.0 / 0.0), dbl(-1.0 / 0.0), std::bit_cast<u64>(0x7FF8000000000000ull), dbl(0.0)}},
        {"%lf %Lf|", {dbl(1.5), dbl(2.5)}},
        {"%*d|%-*d|%.*d|%*.*d|", {6, 42, 6, 42, 4, 7, 8, 3, 9}},
        {"%*d|", {static_cast<u64>(-6), 42}},
        {"%k %y %w|", {1, 2, 3}},
        {"%d %", {5}},
        {"%5", {}},
        {"%q%d", {1, 2}},
        {"%n|", {kData + 0x7000}},
        {"%b %B", {5, 6}},
        {"%#5x|%-#8x|%#010x|", {0xAB, 0xAB, 0xAB}},
        {"%1$d %2$d", {1, 2}},
        {"%'d %I64d", {1234567, 5}},
        {"%d %d %d %d %d %d %d %d %d %d %d %d", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
        {"%s %s %s %s %s %s %s %s %s %s", {s1, s2, s3, s1, s2, s3, s1, s2, s3, s1}},
        {"%.3s|%.0s|%.10s|", {s1, s1, s2}},
        {"%-0d|%0-d|%+ d|% +d|", {1, 2, 3, 4}},
        {"%hhn %hn %ln|", {kData + 0x7000, kData + 0x7010, kData + 0x7020}},
        {"ab%nc%lln|", {kData + 0x7000, kData + 0x7010}},
        {"%10p|%-10p|%010p|%#p|%.4p|%p|", {0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0x123456789ABCull}},
        {"%5%|%-5%|%05%|%5.2%|", {}},
        {"%5q|%-5k|%05y|", {}},
        {"%Ld %Lu %Lx %LX|", {0x123456789ull, 0xFFFFFFFFFFFFFFFFull, 0x123456789ull, 0xABull}},
        {"%+u % u %+x % x %+o|", {1, 2, 3, 4, 5}},
        {"%-05d|%-010x|%-#010x|", {7, 0xAB, 0xAB}},
        {"%.d|%.x|%.u|%.o|%.s|", {0, 0, 0, 0, kStrs}},
        {"%c%c%c|", {0x141, 0x7A, 0xFFFFFF30}},
        {"%lc %ls|", {'Q', kStrs}},
        {"%0*d|%-*.*s|%.*s|", {5, 42, 8, 3, kStrs + 0x40, static_cast<u64>(-1), kStrs}},
        {"%5s|%-5s|%05s|%5c|%05c|", {kStrs + 0x80, kStrs + 0x80, kStrs + 0x80, 'z', 'z'}},
        {"%llc|%hhs|%lp|", {'R', kStrs, 0x55}},
        {"%ll", {1}},
        {"%.5", {1}},
        {"%-", {1}},
        {"%#", {1}},
        {"%x%", {0x10}},
        {"%%%d%%", {9}},
        {"%3$s %1$s", {kStrs, kStrs, kStrs}},
        {"%#5o|%#05o|%#-5o|%#.3o|%#.0x|%#5.0x|", {8, 8, 8, 8, 0, 0}},
        {"%+.0d|% .0d|%+5.0d|", {0, 0, 0}},
        {"%d %u %x", {0xFFFFFFFFull, 0x1FFFFFFFFull, 0xFFFFFFFF00000000ull}},
        {"%lld %llu %llx %lli", {0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0, static_cast<u64>(-1)}},
        {"%20lld|%-20llx|%020llu|%.20lld|", {static_cast<u64>(-5), 0xABC, 7, 8}},
        {"%*%|%.*%|", {5, 3}},
        {"%v %r %m %e %%|", {1}},
        {"%05.2s|%-05s|%0-5c|%07.3s|", {kStrs, kStrs + 0x80, 'w', kStrs + 0x40}},
        // The cases tests/hle/test_sysprx_libc.cpp checks without the firmware, as it writes them.
        {"%lu %zu %ju %llu", {static_cast<u64>(-1), static_cast<u64>(-1), static_cast<u64>(-1), static_cast<u64>(-1)}},
        {"%hd %hhd %hu %hhu", {0x18000, 0x1FF, 0x18000, 0x1FF}},
        {"[%o|%#o|%#o|%#.3o|%#.0o]", {8, 8, 0, 8, 0}},
        {"%llx %hx %hhx", {0x123456789ABCull, 0x12345, 0x1FF}},
        {"%b|%5b|%B", {5, 2, 0}},
        {"%p|%20p|%-18p|", {0x10000, 0xD0001234, 1}},
        {"%d %f %d", {1, dbl(0.25), 2}},
        {"%*.*f|%+.1e|%08.2f|%a", {6, 2, dbl(1.0), dbl(-0.5), dbl(-3.25), dbl(1.0)}},
        {"a%yb%dc", {5}},
        {"%d %Lf %d", {1, dbl(1.0), 3}},
        {"%d%n", {1, kData + 0x7000}},
        {"%d %hld", {1, 2}},
        {"%5*d", {3, 4}},
        {"%lc", {'x'}},
        {"100%", {}},
        {"50%-", {}},
        // Flags, widths, precisions and lengths in any order (liblv2 looks each character up in one table).
        {"%5-d|%.3-5d|%-5.3d|%05-d|%5.3-0d|", {1, 2, 3, 4, 5}},
        {"%*5d|%5*d|%5*.*d|%.*5d|", {7, 1, 3, 2, 6, 4, 2, 9, 3}},
        {"%D %O %U %lD %LD|", {static_cast<u64>(-3), 8, static_cast<u64>(-1), static_cast<u64>(-4), 0x123456789ull}},
        {"%Lld %lLd %llld %hld %lhd %hhld %lh d|", {0x123456789ull, 0x123456789ull, 0x123456789ull, 0x123456789ull,
                                                    0x123456789ull, 0x123456789ull, 0x123456789ull}},
        {"%.5.3d|%.3.d|%.d|%.*.3d|%3.5.1d|", {7, 7, 7, 4, 7, 7}},
        {"%0-5d|% +d|%+ d|%+-5d|%- +5d|%#-#5x|", {1, 2, 3, 4, 5, 6}},
        {"%4294967301d|%.4294967298d|%.2147483648d|%2147483648d|", {1, 2, 3, 4}},
        {"%#lx %#llx %#Lo %lu%lu|", {0xABCDEF, 0x123456789ABull, 9, 1, 2}},
        {"%hhhhd %hlhd %llllx|", {0x12345, 0x12345, 0x123456789ull}},
    };
}

// The TTY's text while a comparison runs: sys_tty_write logs it on the "tty" channel, a line at a time.
struct TtyCapture {
    std::string text;
    static void sink(void* user, log::Level, std::string_view channel, std::string_view message)
    {
        if (channel == "tty") static_cast<TtyCapture*>(user)->text += message;
    }
    TtyCapture() { log::set_sink(&sink, this); }
    ~TtyCapture() { log::set_sink(nullptr, nullptr); }
};
TtyCapture* g_tty = nullptr;

void put_cstr(u8* b, u32 addr, std::string_view s)
{
    std::memcpy(b + addr, s.data(), s.size());
    b[addr + s.size()] = 0;
}

void setup(u8* b, const Case& c)
{
    if (g_tty) g_tty->text.clear();
    put_cstr(b, kText, c.fmt);
    put_cstr(b, kStrs, "hello");
    put_cstr(b, kStrs + 0x40, "a longer string here");
    put_cstr(b, kStrs + 0x80, "xyz");
    for (usize i = 0; i < c.args.size(); i++) ps_write64(b, kSlots + 8 * static_cast<u32>(i), c.args[i]);
    std::memset(b + kOut, 0xCC, kOutSize + kGuard);
    std::memset(b + kData + 0x7000, 0xEE, 0x40);
}

struct Answer {
    std::string call;
    u32 r3 = 0;
    std::vector<u8> memory; // the output and its guard, then the %n targets
    std::string tty;        // what went to the TTY, its lines joined (the log splits them where each side wrote)
};


Answer answer(u8* b, std::string call, u32 r3)
{
    Answer a{std::move(call), r3, {}, g_tty ? g_tty->text : std::string()};
    if (g_tty) g_tty->text.clear();
    a.memory.assign(b + kOut, b + kOut + kOutSize + kGuard);
    a.memory.insert(a.memory.end(), b + kData + 0x7000, b + kData + 0x7040);
    return a;
}

// pspocket's function `name` with these register (and stack) arguments, as guest code calls it.
u32 ours(GuestThread& t, const char* name, const std::vector<u64>& args)
{
    const Function& fn = GuestThread::find(name);
    for (usize i = 0; i < args.size(); i++) {
        if (i < 8) t.ctx.gpr[3 + i] = args[i];
        else ps_write64(t.f.base(), static_cast<u32>(t.ctx.gpr[1]) + 0x70 + 8 * static_cast<u32>(i - 8), args[i]);
    }
    fn.fn(t.ctx, t.f.base());
    return static_cast<u32>(t.ctx.gpr[3]);
}

u32 theirs(GuestThread& t, const FirmwareLibrary& sony, const char* name, const std::vector<u64>& args)
{
    const auto it = sony.module->functions.find({"sysPrxForUser", recomp::nid_of(name)});
    REQUIRE(it != sony.module->functions.end());
    REQUIRE(args.size() <= 16);
    std::array<u64, 16> a{};
    std::copy(args.begin(), args.end(), a.begin());
    // Arguments past the call's own are zeros in registers and the save area, which the callee does not read.
    return static_cast<u32>(call_guest(t.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8],
                                                               a[9], a[10], a[11], a[12], a[13], a[14], a[15]}));
}

// Every call of the printf family for one case: the v functions with the va_list, the others with the arguments in
// registers and on the stack, each snprintf at several sizes.
std::vector<Answer> run(u8* b, const Case& c, const std::function<u32(const char*, const std::vector<u64>&)>& call)
{
    std::vector<Answer> out;
    std::vector<u64> regs;
    const auto with = [&](std::initializer_list<u64> fixed) {
        regs.assign(fixed);
        regs.insert(regs.end(), c.args.begin(), c.args.end());
        return regs;
    };
    for (const u32 size : {0u, 1u, 4u, 17u, kOutSize}) {
        setup(b, c);
        out.push_back(answer(b, std::format("_sys_vsnprintf(\"{}\", {})", c.fmt, size),
            call("_sys_vsnprintf", {kOut, size, kText, kSlots})));
        setup(b, c);
        out.push_back(answer(b, std::format("_sys_snprintf(\"{}\", {})", c.fmt, size),
            call("_sys_snprintf", with({kOut, size, kText}))));
    }
    setup(b, c);
    out.push_back(answer(b, std::format("_sys_vsprintf(\"{}\")", c.fmt), call("_sys_vsprintf", {kOut, kText, kSlots})));
    setup(b, c);
    out.push_back(answer(b, std::format("_sys_sprintf(\"{}\")", c.fmt), call("_sys_sprintf", with({kOut, kText}))));
    setup(b, c);
    out.push_back(answer(b, std::format("_sys_printf(\"{}\")", c.fmt), call("_sys_printf", with({kText}))));
    setup(b, c);
    out.push_back(answer(b, std::format("_sys_vprintf(\"{}\")", c.fmt), call("_sys_vprintf", {kText, kSlots})));
    return out;
}

void compare(const std::vector<Answer>& ours_, const std::vector<Answer>& theirs_)
{
    REQUIRE(ours_.size() == theirs_.size());
    usize differ = 0;
    for (usize i = 0; i < ours_.size(); i++) {
        const Answer& o = ours_[i];
        const Answer& s = theirs_[i];
        if (o.r3 == s.r3 && o.memory == s.memory && o.tty == s.tty) continue;
        usize at = 0;
        while (at < o.memory.size() && o.memory[at] == s.memory[at]) at++;
        const auto text = [](const std::vector<u8>& m) {
            std::string r;
            for (usize k = 0; k < kOutSize && m[k] != 0 && m[k] != 0xCC; k++) r += static_cast<char>(m[k]);
            return r;
        };
        if (++differ <= 1000)
            UNSCOPED_INFO(std::format("{}: {:#x} \"{}\" (pspocket) vs {:#x} \"{}\" (Sony){}", o.call, o.r3,
                text(o.memory), s.r3, text(s.memory),
                at < o.memory.size() ? std::format("; memory differs first at byte {} ({:#04x} vs {:#04x})", at,
                                           o.memory[at], s.memory[at])
                                     : std::string(),
                o.tty == s.tty ? std::string() : std::format("; TTY \"{}\" vs \"{}\"", o.tty, s.tty)));
    }
    INFO(std::format("{} of {} calls differ", differ, ours_.size()));
    CHECK(differ == 0);
}

} // namespace

TEST_CASE("sysPrxForUser's printf family against liblv2", "[hle][lle][lv2]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    FirmwareLibrary sony(f, t, *flash, "sysPrxForUser", false);
    t.call("sys_initialize_tls", 0, 0, 0, 0); // liblv2's printf lock is its lwmutex, which reads the thread's TLS
    TtyCapture tty;
    g_tty = &tty;
    std::vector<Answer> all_ours, all_theirs;
    for (const Case& c : cases()) {
        const auto mine = run(b, c, [&](const char* name, const std::vector<u64>& a) { return ours(t, name, a); });
        const auto sonys = run(b, c, [&](const char* name, const std::vector<u64>& a) {
            return theirs(t, sony, name, a);
        });
        all_ours.insert(all_ours.end(), mine.begin(), mine.end());
        all_theirs.insert(all_theirs.end(), sonys.begin(), sonys.end());
    }
    g_tty = nullptr;
    compare(all_ours, all_theirs);
}

TEST_CASE("sysPrxForUser's small functions against liblv2", "[hle][lle][lv2]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    FirmwareLibrary sony(f, t, *flash, "sysPrxForUser", false);
    std::vector<Answer> mine, sonys;
    const auto both = [&](const char* name, std::vector<u64> args, const std::function<void()>& prepare) {
        prepare();
        mine.push_back(answer(b, name, ours(t, name, args)));
        prepare();
        sonys.push_back(answer(b, name, theirs(t, sony, name, args)));
    };
    const auto path = [&](std::string_view p) { return [&b, p] { put_cstr(b, kText, p); }; };
    for (const char* p : {"", "/", "a", "/dev_hdd0/game/x.self", "dir/", "a//b", "//", "a/b/", "/a/b/c", "x/y//"}) {
        both("__sys_printf_basename", {kText}, path(p));
    }
    const auto nothing = [] {};
    both("sys_process_spawn_with_memory_budget", {}, nothing);
    both("sys_process_wait_for_game_process", {}, nothing);
    both("_sys_process_wait_for_game_process", {}, nothing);
    both("sys_process_spawn", {0, kText, 0, 0, 1000, 0x1000}, path("/app_home/a.self"));
    both("sys_process_spawn", {0, kText, 0, 0, 1000, 0x80000}, path("/app_home/a.self"));
    both("sys_game_watchdog_clear", {}, nothing);
    both("sys_game_get_rtc_status", {kOut}, [&] { std::memset(b + kOut, 0xCC, kOutSize + kGuard); });
    both("sys_game_board_storage_read", {kOut}, [&] { std::memset(b + kOut, 0xCC, kOutSize + kGuard); });
    compare(mine, sonys);

    // The timebase's low word: never 0, and moving.
    const u32 a = static_cast<u32>(ours(t, "__sys_time", {}));
    const u32 c = static_cast<u32>(theirs(t, sony, "__sys_time", {}));
    CHECK(a != 0);
    CHECK(c != 0);
}

namespace {

// One side's heap calls, by name, with r3 back.
using HeapCall = std::function<u32(const char*, const std::vector<u64>&)>;

// A made-up sequence over one heap: allocations of every size class, aligned ones, frees in an order that merges
// neighbours, the top and the designated victim, and the reports after each step. Pointers are kept as their distance
// from the heap's ID (the two heaps lie at different addresses); everything else as it is.
std::vector<std::string> heap_script(u8* b, const HeapCall& call, u32 name, u32 size, u32 flags, u32 attr)
{
    std::vector<std::string> log;
    const u32 h = call("_sys_heap_create_heap", {name, size, flags, attr});
    log.push_back(std::format("create {}", h ? "ok" : "0"));
    if (!h) return log;
    const auto rel = [h](u32 p) { return p ? std::format("+{:#x}", p - h) : std::string("0"); };
    const auto report = [&](std::string_view what) {
        const u32 info = kData + 0x7100;
        std::memset(b + info, 0xEE, 40);
        const u32 r = call("_sys_heap_get_mallinfo", {h, info});
        std::string line = std::format("{}: mallinfo {} [", what, r);
        for (u32 i = 0; i < 10; i++) line += std::format(" {:#x}", ps_read32(b, info + 4 * i));
        line += std::format(" ] free {:#x} stats {}", call("_sys_heap_get_total_free_size", {h}),
            call("_sys_heap_stats", {h}));
        log.push_back(line);
    };
    report("new");
    std::vector<u32> blocks;
    const u32 sizes[] = {0, 1, 6, 7, 8, 15, 16, 17, 24, 100, 231, 232, 233, 255, 256, 257, 500, 1000, 1024, 4000,
                         4096, 10000, 65536, 0x20000, 0x7FFFFFFF, 0xFFFFFFC0};
    for (const u32 sz : sizes) {
        const u32 p = call("_sys_heap_malloc", {h, sz});
        log.push_back(std::format("malloc {:#x} -> {}", sz, rel(p)));
        blocks.push_back(p);
    }
    report("after mallocs");
    for (usize i = 0; i < blocks.size(); i += 3) call("_sys_heap_free", {h, blocks[i]}); // holes to reuse
    report("after frees");
    for (const u32 sz : {8u, 24u, 100u, 300u, 600u, 2000u, 9000u}) log.push_back(std::format("malloc {:#x} -> {}", sz, rel(call("_sys_heap_malloc", {h, sz}))));
    report("after reuse");
    for (const u32 al : {1u, 8u, 16u, 32u, 64u, 100u, 128u, 4096u, 0x10000u}) {
        for (const u32 sz : {1u, 40u, 333u}) {
            log.push_back(std::format("memalign {:#x} {:#x} -> {}", al, sz, rel(call("_sys_heap_memalign", {h, al, sz}))));
        }
    }
    report("after memalign");
    const u32 attrs = kData + 0x7200;
    for (const auto& [asz, aal] : std::vector<std::pair<u32, u32>>{{8, 64}, {8, 0}, {8, 6}, {8, 48}, {4, 64}, {16, 256}}) {
        ps_write32(b, attrs, asz);
        ps_write32(b, attrs + 4, aal);
        log.push_back(std::format("alloc_heap_memory attr {} {} -> {}", asz, aal,
            rel(call("_sys_heap_alloc_heap_memory", {h, 200, attrs}))));
    }
    log.push_back(std::format("alloc_heap_memory -> {}", rel(call("_sys_heap_alloc_heap_memory", {h, 200, 0}))));
    for (usize i = 1; i < blocks.size(); i += 3) call("_sys_heap_free", {h, blocks[i]});
    for (usize i = 2; i < blocks.size(); i += 3) call("_sys_heap_free", {h, blocks[i]});
    report("after all frees");
    // Many small blocks, freed in an interleaved order, then reused: the small bins and the trees.
    std::vector<u32> small;
    for (u32 i = 0; i < 200; i++) small.push_back(call("_sys_heap_malloc", {h, 16 + (i * 37) % 700}));
    for (usize i = 0; i < small.size(); i += 2) call("_sys_heap_free", {h, small[i]});
    report("after small frees");
    std::string got;
    for (u32 i = 0; i < 120; i++) got += " " + rel(call("_sys_heap_malloc", {h, 8 + (i * 53) % 900}));
    log.push_back("reuse:" + got);
    for (usize i = 1; i < small.size(); i += 2) call("_sys_heap_free", {h, small[i]});
    report("end");
    log.push_back(std::format("delete {}", call("_sys_heap_delete_heap", {h})));
    return log;
}

void compare_logs(const std::vector<std::string>& ours_, const std::vector<std::string>& theirs_)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(ours_.size(), theirs_.size()); i++) {
        const std::string o = i < ours_.size() ? ours_[i] : "(none)", s = i < theirs_.size() ? theirs_[i] : "(none)";
        if (o == s) continue;
        if (++differ <= 60) UNSCOPED_INFO(std::format("{}: {} (pspocket) vs {} (Sony)", i, o, s));
    }
    INFO(std::format("{} of {} lines differ", differ, theirs_.size()));
    CHECK(differ == 0);
}

} // namespace

TEST_CASE("liblv2's heaps against liblv2", "[hle][lle][lv2]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    FirmwareLibrary sony(f, t, *flash, "sysPrxForUser", false);
    t.call("sys_initialize_tls", 0, 0, 0, 0);
    TtyCapture tty;
    const HeapCall mine = [&](const char* name, const std::vector<u64>& a) { return ours(t, name, a); };
    const HeapCall sonys = [&](const char* name, const std::vector<u64>& a) { return theirs(t, sony, name, a); };
    put_cstr(b, kText, "a heap of the test's");
    const u32 attr = kData + 0x7300, user_memory = kData + 0x100000;
    ps_write32(b, attr, 8);
    ps_write32(b, attr + 4, user_memory);
    struct Heap {
        u32 size, flags, attr;
    };
    for (const Heap& hp : {Heap{0x30000, 0, 0}, Heap{0x80000, 0x110000, 0}, Heap{0x12345, 0x7, 0},
                           Heap{0x40000, 0x80000000, attr}, Heap{0x40000, 0x80000001, attr}, Heap{0x10000, 0, attr}}) {
        INFO(std::format("heap size {:#x} flags {:#x} attr {:#x}", hp.size, hp.flags, hp.attr));
        const auto ours_log = heap_script(b, mine, kText, hp.size, hp.flags, hp.attr);
        CHECK(ours_log.size() > 50); // the heap was made and the whole script ran
        compare_logs(ours_log, heap_script(b, sonys, kText, hp.size, hp.flags, hp.attr));
    }
    // A heap of 0, and an attribute that is not 8 bytes.
    std::vector<std::string> mine_log, sony_log;
    for (const auto* side : {&mine, &sonys}) {
        auto& log = side == &mine ? mine_log : sony_log;
        tty.text.clear();
        // (_sys_heap_delete_heap of 0 faults in liblv2: it reads the heap before it checks it.)
        for (const char* fn : {"_sys_heap_malloc", "_sys_heap_free", "_sys_heap_get_total_free_size", "_sys_heap_stats"})
            log.push_back(std::format("{}(0) -> {:#x}", fn, (*side)(fn, {0, 16, 16})));
        log.push_back("tty: " + tty.text);
        ps_write32(b, attr, 12);
        log.push_back(std::format("create with a 12-byte attribute -> {:#x}",
            (*side)("_sys_heap_create_heap", {kText, 0x10000, 0x80000000, attr})));
        ps_write32(b, attr, 8);
    }
    compare_logs(mine_log, sony_log);
}
