// cellSail (D-562) against libsail, Sony's own module, from the user's installed system software (firmware_compare.hpp;
// skipped without it): the same script in two fresh processes (libsail loaded in both), one calling libsail's exports
// and one pspocket's. The answers, the memory the script uses (the objects, a stand-in player's message queue, what
// the callbacks were called with) and the lv2 calls must be the same. No game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kArea = kData + 0x1000, kAreaSize = 0x3000;
constexpr u32 kAlloc = kArea, kCallbacks = kArea + 0x20, kFuture = kArea + 0x40, kFuture2 = kArea + 0x60,
              kResult = kArea + 0x80, kSource = kArea + 0x100, kFuncs = kArea + 0x200, kRa = kArea + 0x300,
              kRv = kArea + 0x400, kAttr = kArea + 0x500, kFormat = kArea + 0x520, kPlayer = kArea + 0x800,
              kRing = kArea + 0xC00, kCode = kArea + 0x1000, kRecords = kArea + 0x2000;

// A callback that records (r3..r7) at kRecords (a count, then 48 bytes each from +16) and returns 0.
void callback_code(u8* b, u32 at)
{
    const u32 words[] = {0x3D600000u | (kRecords >> 16), 0x616B0000u | (kRecords & 0xFFFF), 0x818B0000u, 0x1C0C0030u,
        0x7D2B0214u, 0xF8690010u, 0xF8890018u, 0xF8A90020u, 0xF8C90028u, 0xF8E90030u, 0x7C2004ACu, 0x398C0001u,
        0x918B0000u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, at + 4 * i, words[i]);
    ps_write32(b, at + 0xF0, at);
    ps_write32(b, at + 0xF4, 0);
}

struct Trace {
    std::vector<std::string> calls;
};

// libsail's mutex and condition calls with their arguments (the attributes by their words, not their stack address).
u8* g_base = nullptr;
void observe(void* user, u32 n, const u64* a)
{
    std::string s;
    const auto attr = [&](u64 at, u32 words) {
        std::string w;
        for (u32 i = 0; i < words; i++) w += std::format(" {:x}", ps_read32(g_base, static_cast<u32>(at) + 4 * i));
        return w;
    };
    switch (n) {
    case 100: s = std::format("mutex_create({:#x},{})", a[0], attr(a[1], 10)); break;
    case 105: s = std::format("cond_create({:#x}, {:#x},{})", a[0], a[1], attr(a[2], 6)); break;
    case 102: case 107: s = std::format("{}({:#x}, {})", n, a[0], a[1]); break;
    case 101: case 104: case 106: case 108: s = std::format("{}({:#x})", n, a[0]); break;
    default: return;
    }
    static_cast<Trace*>(user)->calls.push_back(s);
}

struct Run {
    std::vector<std::string> lines;
    std::vector<u8> mem;
    std::vector<std::string> calls;
};

Run run(const std::string& flash, bool sony)
{
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sail(f, t, flash, "cellSail");
    u8* b = f.base();
    std::memset(b + kArea, 0, kAreaSize);
    callback_code(b, kCode);
    const u32 cb = kCode + 0xF0;
    Run r;
    Trace tr;
    g_base = b;
    f.kernel->set_syscall_observer(&observe, &tr);
    const auto call = [&](const char* name, std::initializer_list<u64> args, bool is_void = false) {
        u64 v;
        const std::string_view n(name);
        const u32 nid = n.starts_with("cellSail_") ? static_cast<u32>(std::stoul(std::string(n.substr(9)), nullptr, 16))
                                                   : recomp::nid_of(name);
        if (sony) {
            v = call_guest(t.ctx, sail.module->functions.at({"cellSail", nid}), args);
        } else {
            const Function* fn = registry().find("cellSail", nid);
            REQUIRE(fn);
            u32 i = 3;
            for (const u64 a : args) t.ctx.gpr[i++] = a;
            fn->fn(t.ctx, b);
            v = t.ctx.gpr[3];
        }
        r.lines.push_back(is_void ? std::format("{} -> (void)", name) : std::format("{} -> {:#x}", name, v));
        return v;
    };

    // The allocator.
    ps_write32(b, kCallbacks, 0x11110000);
    ps_write32(b, kCallbacks + 4, 0x22220000);
    call("cellSailMemAllocatorInitialize", {0, kCallbacks, 1});
    call("cellSailMemAllocatorInitialize", {kAlloc, 0, 1});
    ps_write32(b, kCallbacks + 4, 0);
    call("cellSailMemAllocatorInitialize", {kAlloc, kCallbacks, 1});
    ps_write32(b, kCallbacks + 4, 0x22220000);
    ps_write32(b, kCallbacks, 0);
    call("cellSailMemAllocatorInitialize", {kAlloc, kCallbacks, 1});
    ps_write32(b, kCallbacks, 0x11110000);
    call("cellSailMemAllocatorInitialize", {kAlloc, kCallbacks, 0x1234});

    // The future.
    call("cellSailFutureInitialize", {kFuture});
    call("cellSailFutureIsDone", {kFuture, kResult});
    call("cellSailFutureReset", {kFuture, 0});
    call("cellSailFutureSet", {kFuture, 7});
    call("cellSailFutureSet", {kFuture, 8});
    call("cellSailFutureIsDone", {kFuture, kResult});
    call("cellSailFutureIsDone", {kFuture, 0});
    call("cellSailFutureGet", {kFuture, 0, kResult + 4});
    call("cellSailFutureGet", {kFuture, 0, 0});
    call("cellSailFutureReset", {kFuture, 0});
    call("cellSailFutureIsDone", {kFuture, kResult + 8});
    call("cellSailFutureInitialize", {kFuture2});
    call("cellSailFutureGet", {kFuture2, 1000, kResult + 12}); // times out
    call("cellSailFutureReset", {kFuture2, 1});
    call("cellSailFutureFinalize", {kFuture2});
    call("cellSailFutureFinalize", {kFuture});
    call("cellSailFutureFinalize", {kFuture});

    // A stand-in player: its message queue (three messages; drained by the script between notifications).
    {
        // sys_mutex_attribute_t, sys_cond_attribute_t (priority, not recursive, not shared, not adaptive)
        const u32 m = kAttr + 0x100, c = kAttr + 0x140;
        ps_write32(b, m, 2);
        ps_write32(b, m + 4, 0x20);
        ps_write32(b, m + 8, 0x200);
        ps_write32(b, m + 12, 0x2000);
        ps_write32(b, c, 0x200);
        REQUIRE(lv2::sys_mutex_create(*t.t, kPlayer + 0x170 + 0x14, m) == lv2::CELL_OK);
        REQUIRE(lv2::sys_cond_create(*t.t, kPlayer + 0x170 + 0x18, ps_read32(b, kPlayer + 0x170 + 0x14), c) ==
            lv2::CELL_OK);
    }
    ps_write32(b, kPlayer + 0x170, 3);
    ps_write32(b, kPlayer + 0x170 + 8, kRing);
    const auto drain = [&] { ps_write32(b, kPlayer + 0x170 + 4, 0); };

    // The source.
    for (u32 i = 0; i < 14; i++) ps_write32(b, kFuncs + 4 * i, 0x30000000 + i);
    call("cellSailSourceInitialize", {kSource, 0, 1});
    std::memset(b + kSource, 0xEE, 0x80);
    call("cellSailSourceInitialize", {kSource, kFuncs, 0x55});
    ps_write32(b, kSource, kPlayer);
    call("cellSailSourceNotifyInputEos", {kSource}, true);
    call("cellSailSourceNotifyMediaStateChanged", {kSource, 3}, true);
    call("cellSailSourceNotifySessionError", {kSource, 0x80610703, 1}, true);
    drain();
    call("cellSailSourceNotifySessionError", {kSource, 0xFFFFFFFB, 0}, true);
    call("cellSailSourceNotifyStreamOut", {kSource, 1}, true);
    call("cellSailSourceNotifyCallCompleted", {kSource, 9}, true);
    drain();
    ps_write64(b, kSource + 0x60, 0x123456789ABCull);
    call("cellSailSourceNotifyCallCompleted", {kSource, 0xFFFFFFFD}, true);
    call("cellSailSourceSetDiagHandler", {kSource, cb, 0x77});
    call("cellSailSourceNotifyInputEos", {kSource}, true);
    call("cellSailSourceNotifyMediaStateChanged", {kSource, 4}, true);
    call("cellSailSourceNotifySessionError", {kSource, 0x80610709, 1}, true);
    call("cellSailSourceNotifyStreamOut", {kSource, 1}, true);
    call("cellSailSourceNotifyCallCompleted", {kSource, 0xFFFFFFFE}, true);
    call("cellSailSourceSetDiagHandler", {kSource, 0, 0});
    call("cellSailSourceFinalize", {kSource});
    drain();

    // The renderers.
    for (const char* kind : {"Audio", "Video"}) {
        const u32 obj = kind[0] == 'A' ? kRa : kRv;
        const std::string p = std::string("cellSailRenderer") + kind;
        std::memset(b + obj, 0xEE, 0x100);
        call((p + "Initialize").c_str(), {obj, kFuncs, 0x66, 0});
        ps_write32(b, kAttr, 7);
        call((p + "Initialize").c_str(), {obj, kFuncs, 0x67, kAttr});
        ps_write32(b, kAttr, 8);
        ps_write32(b, kAttr + 4, 0);
        call((p + "Initialize").c_str(), {obj, kFuncs, 0x68, kAttr});
        for (u32 i = 0; i < 0x20; i++) b[kFormat + i] = static_cast<u8>(0x40 + i);
        ps_write32(b, kAttr + 4, kFormat);
        call((p + "Initialize").c_str(), {obj, kFuncs, 0x69, kAttr});
        ps_write32(b, obj + 0x14, kPlayer);
        call((p + "NotifyCallCompleted").c_str(), {obj, 0xFFFFFFFE}, true);
        call((p + "NotifyFrameDone").c_str(), {obj}, true);
        call((p + "NotifyOutputEos").c_str(), {obj}, true);
        drain();
        call(kind[0] == 'A' ? "cellSail_C00A450C" : "cellSail_3E9B9F80", {obj, 0x123456789ull}, true);
        call((p + "Finalize").c_str(), {obj});
        drain();
    }

    // The exports that answer -1.
    for (const char* n : {"cellSail_D5E100E7", "cellSail_7224C0F1", "cellSail_4B5A94A5", "cellSail_197EE602",
             "cellSail_86DE3AA7", "cellSail_5367B57E", "cellSail_9279F3D9", "cellSail_EC695E87", "cellSail_3BE9899B",
             "cellSail_A56F7CC5", "cellSail_5FAC53DD", "cellSail_2692816B", "cellSail_CF03D3BA", "cellSail_D7D96074",
             "cellSail_320887CC", "cellSail_0CCD506A"})
        call(n, {1, 2, 3});

    f.kernel->set_syscall_observer(nullptr, nullptr);
    r.mem.assign(b + kArea, b + kArea + kAreaSize);
    r.calls = tr.calls;
    return r;
}

void compare_lines(const char* what, const std::vector<std::string>& mine, const std::vector<std::string>& sony)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(mine.size(), sony.size()); i++) {
        const std::string o = i < mine.size() ? mine[i] : "(none)", s = i < sony.size() ? sony[i] : "(none)";
        if (o == s) continue;
        if (differ++ < 20) UNSCOPED_INFO(std::format("{} {}: pspocket {} | libsail {}", what, i, o, s));
    }
    CHECK(differ == 0);
}

} // namespace

TEST_CASE("cellSail against libsail: allocator, future, source, renderers", "[hle][lle][sail]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    const Run mine = run(*flash, false);
    const Run sony = run(*flash, true);
    compare_lines("answer", mine.lines, sony.lines);
    compare_lines("lv2 call", mine.calls, sony.calls);
    usize first = 0;
    while (first < mine.mem.size() && mine.mem[first] == sony.mem[first]) first++;
    INFO(std::format("memory differs first at {:#x} (pspocket {:#04x}, libsail {:#04x})", kArea + first,
        first < mine.mem.size() ? mine.mem[first] : 0, first < sony.mem.size() ? sony.mem[first] : 0));
    CHECK(first == mine.mem.size());
}
