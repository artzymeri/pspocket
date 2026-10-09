// cellSail (D-562) against libsail, Sony's own module, from the user's installed system software (firmware_compare.hpp;
// skipped without it): the same script in two fresh processes (libsail loaded in both), one calling libsail's exports
// and one pspocket's. The answers, the memory the script uses (the objects, a stand-in player's message queue, what
// the callbacks were called with) and the lv2 calls must be the same. No game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <functional>
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

using Call = std::function<u64(const char*, std::initializer_list<u64>, bool)>;
void part1(u8* b, const Call& call, u32 cb, GuestThread& t);
void part2(u8* b, const Call& call, u32 cb, GuestThread& t);
std::vector<std::pair<u32, u32>> g_mask; // ranges of memory each side fills with its own code addresses

Run run(const std::string& flash, bool sony, int part)
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
        if (std::getenv("SAIL_TRACE")) std::fprintf(stderr, "CALL %s %s\n", sony ? "sony" : "mine", name);
        if (sony) {
            v = call_guest(t.ctx, sail.module->functions.at({"cellSail", nid}), args);
        } else {
            const Function* fn = registry().find("cellSail", nid);
            REQUIRE(fn);
            u32 i = 0;
            for (const u64 a : args) {
                if (i < 8) t.ctx.gpr[3 + i] = a;
                else ps_write64(b, static_cast<u32>(t.ctx.gpr[1]) + 0x70 + 8 * (i - 8), a);
                i++;
            }
            fn->fn(t.ctx, b);
            v = t.ctx.gpr[3];
        }
        r.lines.push_back(is_void ? std::format("{} -> (void)", name) : std::format("{} -> {:#x}", name, v));
        return v;
    };
    g_mask.clear();
    if (part == 1) part1(b, call, cb, t);
    else part2(b, call, cb, t);
    f.kernel->set_syscall_observer(nullptr, nullptr);
    for (const auto& [at, n] : g_mask) std::memset(b + at, 0x5A, n);
    r.mem.assign(b + kArea, b + kArea + kAreaSize);
    r.calls = tr.calls;
    return r;
}

void part1(u8* b, const Call& call_, u32 cb, GuestThread& t)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };

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

}

// A bump allocator for the player (alloc(arg, boundary, size) from kHeap upward; free does nothing) and a second event
// handler, both guest code.
constexpr u32 kHeap = kData + 0x40000, kHeapPtr = kArea + 0xF00, kAllocCode = kArea + 0x1100, kFreeCode = kArea + 0x1180;
void allocator_code(u8* b)
{
    // lis/ori r11 = &heap pointer; lwz r3; addi r0,r4,-1; add r3,r3,r0; andc r3,r3,r0; add r12,r3,r5; stw r12; blr
    const u32 alloc[] = {0x3D600000u | (kHeapPtr >> 16), 0x616B0000u | (kHeapPtr & 0xFFFF), 0x806B0000u, 0x3804FFFFu,
        0x7C630214u, 0x7C630078u, 0x7D832A14u, 0x918B0000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(alloc); i++) ps_write32(b, kAllocCode + 4 * i, alloc[i]);
    ps_write32(b, kAllocCode + 0x70, kAllocCode);
    ps_write32(b, kFreeCode, 0x38600000u);
    ps_write32(b, kFreeCode + 4, 0x4E800020u);
    ps_write32(b, kFreeCode + 0x70, kFreeCode);
    ps_write32(b, kHeapPtr, kHeap);
}

void part2(u8* b, const Call& call_, u32 cb, GuestThread&)
{
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return call_(n, a, v); };
    allocator_code(b);
    constexpr u32 kP = kArea + 0x1400, kMem = kArea + 0x1300, kAttr2 = kArea + 0x1340, kRes = kArea + 0x1380,
                  kNames = kArea + 0x13A0, kStr = kArea + 0x1800, kOld = kArea + 0x13C0, kInfo = kArea + 0x1900;
    const u32 kRa2 = kRa + 0x80, kRv2 = kRv + 0x80;
    ps_write32(b, kMem, kAllocCode + 0x70); // CellSailMemAllocator: alloc, free, arg
    ps_write32(b, kMem + 4, kFreeCode + 0x70);
    ps_write32(b, kMem + 8, 0x4321);
    u32 cur[5] = {};
    const auto attr = [&](u32 mode, u32 a, u32 v, u32 u, u32 m) {
        cur[0] = mode;
        cur[1] = a;
        cur[2] = v;
        cur[3] = u;
        cur[4] = m;
        for (u32 i = 0; i < 8; i++) ps_write32(b, kAttr2 + 4 * i, 0);
        ps_write32(b, kAttr2, mode);
        ps_write32(b, kAttr2 + 4, a);
        ps_write32(b, kAttr2 + 8, v);
        ps_write32(b, kAttr2 + 12, u);
        ps_write32(b, kAttr2 + 16, m);
    };
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); return at; };
    // The built-in source's function words hold each side's own code addresses.
    const auto mask_source = [&] {
        const u32 src = ps_read32(b, kP + 0x3B0);
        if (src < kArea || src >= kArea + 0x3000) return;
        g_mask.push_back({src + 0x48, 0x38});
        g_mask.push_back({src + 0x100, 0x38});
    };

    // Refusals.
    attr(0, 1, 1, 1, 0);
    call("cellSailPlayerInitialize", {0, cur[0], kMem, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, 0, 1, cur[1], cur[2], cur[3], cur[4]});
    call("cellSailPlayerInitialize", {kP, cur[0], 0, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    attr(8, 1, 1, 1, 0);
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    attr(0, 16, 1, 1, 0);
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    attr(0, 1, 4, 1, 0);
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, 1, cur[1], cur[2], cur[3], cur[4]});
    call("cellSailPlayerFinalize", {kP});

    // A player of one stream of each kind.
    attr(0, 1, 1, 1, 0);
    call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, 0x11, cur[1], cur[2], cur[3], cur[4]});
    mask_source();
    std::vector<u8> after_init(b + kP, b + kP + 0x400);
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerGetCurrentDescriptor", {kP});
    call("cellSailPlayerIsPaused", {kP});
    call("cellSailPlayerGetRepeatMode", {kP, kInfo});
    call("cellSailPlayerSubscribeEvent", {kP, 3, 0});
    call("cellSailPlayerSubscribeEvent", {kP, 12, 0});
    call("cellSailPlayerSubscribeEvent", {kP, 2, 5});
    call("cellSailPlayerUnsubscribeEvent", {kP, 0});
    call("cellSailPlayerUnsubscribeEvent", {kP, 13});
    call("cellSailPlayerReplaceEventHandler", {kP, 0, 0, kOld, kOld + 4});
    call("cellSailPlayerReplaceEventHandler", {kP, cb + 0x100, 0x99, kOld, kOld + 4});
    call("cellSailPlayerReplaceEventHandler", {kP, cb, 0x11, 0, 0});
    // Sources.
    call("cellSailPlayerRegisterSource", {kP, str(kStr, "x-test"), kSource});
    call("cellSailPlayerRegisterSource", {kP, kStr, kSource});
    call("cellSailPlayerRegisterSource", {kP, 0, kSource});
    call("cellSailPlayerRegisterSource", {kP, kStr, 0});
    call("cellSailPlayerRegisterSource", {kP, str(kStr + 0x40, "x-two-a-very-long-protocol-name-indeed"), kSource});
    call("cellSailPlayerRegisterSource", {kP, str(kStr + 0x80, "x-three"), kSource});
    call("cellSailPlayerRegisterSource", {kP, str(kStr + 0xC0, "x-four"), kSource});
    for (u32 i = 0; i < 4; i++) ps_write32(b, kNames + 4 * i, kStr + 0x100 + 0x40 * i);
    ps_write32(b, kNames + 0x10, 3);
    call("cellSailPlayerGetRegisteredProtocols", {kP, kNames, kNames + 0x10});
    ps_write32(b, kNames + 0x10, 9);
    call("cellSailPlayerGetRegisteredProtocols", {kP, kNames, kNames + 0x10});
    call("cellSailPlayerUnregisterSource", {kP, kStr, kOld + 8});
    call("cellSailPlayerUnregisterSource", {kP, str(kStr + 0x200, "x-none"), kOld + 8});
    call("cellSailPlayerUnregisterSource", {kP, 0, kOld + 8});
    // The per-stream objects.
    std::memset(b + kRa, 0, 0x200);
    std::memset(b + kRv, 0, 0x200);
    call("cellSailPlayerSetRendererAudio", {kP, 1, kRa});
    call("cellSailPlayerSetRendererAudio", {kP, 0, 0});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRa});
    call("cellSailPlayerSetSoundAdapter", {kP, 0, kRa2});
    call("cellSailPlayerSetRendererVideo", {kP, 1, kRv});
    call("cellSailPlayerSetRendererVideo", {kP, 0, 0});
    call("cellSailPlayerSetGraphicsAdapter", {kP, 0, kRv});
    call("cellSailPlayerSetRendererVideo", {kP, 0, kRv2});
    call("cellSailPlayerSetAuReceiver", {kP, 1, kRa + 0x100});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kRa + 0x100});
    call("cellSailPlayerSetAuReceiver", {kP, 0, kRa + 0x140});
    ps_write32(b, kRa + 0x34, 0x101);
    ps_write32(b, kRv + 0x34, 0x100);
    call("cellSailPlayerIsEsAudioMuted", {kP, 0});
    call("cellSailPlayerIsEsAudioMuted", {kP, 1});
    call("cellSailPlayerIsEsVideoMuted", {kP, 0});
    call("cellSailPlayerIsEsVideoMuted", {kP, 3});
    call("cellSailPlayerFinalize", {kP});
    call("cellSailPlayerFinalize", {kP});
    call("cellSailPlayerRegisterSource", {kP, kStr, kSource});
    call("cellSailPlayerSetRendererAudio", {kP, 0, kRa});
    std::memcpy(b + kInfo + 0x100, after_init.data(), 0x400); // the player as Initialize left it

    // Initialize2's checks, then every mode (its clock's latencies), with the messages given.
    ps_write32(b, kRes, 0x00ABCDEF);
    for (u32 i = 1; i < 4; i++) ps_write32(b, kRes + 4 * i, 0);
    attr(1, 2, 0, 0, 10);
    ps_write32(b, kAttr2 + 0x14, 1);
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 1, kAttr2, kRes});
    ps_write32(b, kAttr2 + 0x14, 0);
    ps_write32(b, kRes + 8, 1);
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 1, kAttr2, kRes});
    ps_write32(b, kRes + 8, 0);
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 1, kAttr2, 0});
    call("cellSailPlayerInitialize2", {kP, kMem, cb, 1, kAttr2, kRes});
    mask_source();
    std::memcpy(b + kInfo + 0x500, b + kP, 0x400);
    call("cellSailPlayerFinalize", {kP});
    for (u32 mode = 0; mode < 8; mode++) {
        attr(mode, 1, 2, 0, 0);
        call("cellSailPlayerInitialize", {kP, cur[0], kMem, cb, mode, cur[1], cur[2], cur[3], cur[4]});
        mask_source();
        std::memcpy(b + kInfo + 0x900, b + kP + 8, 200); // the clock
        call("cellSailPlayerFinalize", {kP});
    }
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

void compare_runs(int part)
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    const Run mine = run(*flash, false, part);
    const Run sony = run(*flash, true, part);
    compare_lines("answer", mine.lines, sony.lines);
    compare_lines("lv2 call", mine.calls, sony.calls);
    usize first = 0;
    while (first < mine.mem.size() && mine.mem[first] == sony.mem[first]) first++;
    INFO(std::format("memory differs first at {:#x} (pspocket {:#04x}, libsail {:#04x})", kArea + first,
        first < mine.mem.size() ? mine.mem[first] : 0, first < sony.mem.size() ? sony.mem[first] : 0));
    CHECK(first == mine.mem.size());
}

TEST_CASE("cellSail against libsail: allocator, future, source, renderers", "[hle][lle][sail]") { compare_runs(1); }

TEST_CASE("cellSail against libsail: the player's life before it plays", "[hle][lle][sail]") { compare_runs(2); }
