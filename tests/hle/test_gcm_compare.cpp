// cellGcmSys against libgcm_sys.sprx (D-193's method, D-519): the same scripts go to pspocket's cellGcmSys and to
// Sony's module, each in a fresh process initialised by its own _cellGcmInitBody over pspocket's lv2 (sys_rsx). Every
// answer, every byte written to the test's memory, the driver information's head and handler words, and every RSX
// call into lv2 (sys_rsx_context_attribute and sys_rsx_attribute, with their arguments) must be the same. Skipped
// without the firmware (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp); no game data.

#include <catch_amalgamated.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/rsx/rsx.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kIo = 0x00200000;   // the game's IO buffer: 1 MB of main memory mapped at IO offset 0
constexpr u32 kCtxPtr = kData;    // CellGcmContextData* written by _cellGcmInitBody
constexpr u32 kOut = kData + 0x1000;
constexpr u32 kOutSize = 0x2000;
constexpr u32 kCmdSize = 0x40000;
constexpr u8 kFill = 0xA5;
constexpr u64 kVoid = 1; // Op flag: the function returns nothing (its r3 is not compared)
constexpr u64 kCtxArg = 0xC7C7C7C7; // an argument replaced by the default context's address at the call
constexpr u32 kMaps = 0x01000000, kMapsSize = 0x00C00000; // 12 MB of main memory, MB-aligned

// A call; with fn "=" a 32-bit store into guest memory first (args: address, value), "=di" into the driver
// information (args: offset, value), "=di64" a 64-bit one.
struct Op {
    const char* fn;
    std::vector<u64> args;
    u64 flags = 0;
};

// How the run initialises GCM: _cellGcmInitBody, or libgcm_sys's unnamed 0xB70B4AD3 with its fifth argument
// (D-1040: 0 is the command buffer in local memory); the init's answer, its lv2 RSX calls and the game's context
// (begin, end, current) are compared too.
struct Init {
    const char* fn = "_cellGcmInitBody";
    std::vector<u64> args{kCtxPtr, kCmdSize, 0x100000, kIo};
};

struct Run {
    u32 init_answer = 0;
    std::vector<u32> context; // begin, end, current
    std::vector<std::string> init_calls;
    std::vector<u32> answers;
    std::vector<u8> out;
    std::vector<u8> driver; // the heads and the handler mask
    std::vector<std::string> rsx_calls;
};

struct Calls {
    std::mutex mutex;
    std::vector<std::string> list;
    bool on = false;
};

void observe(void* user, u32 number, const u64* a)
{
    auto* c = static_cast<Calls*>(user);
    if (number != 674 && number != 677 && number != 672 && number != 673) return;
    std::lock_guard lock(c->mutex);
    if (!c->on) return;
    // Package 0xFED (the HLE's vblank) is pspocket's interrupt thread, not a call the function makes.
    if (number == 674 && static_cast<u32>(a[1]) == 0xFED) return;
    const u32 count = number == 673 ? 3 : number == 674 ? 6 : 5; // the call's own arguments
    std::string line = std::to_string(number);
    for (u32 i = 0; i < count; i++) line += std::format(" {:#x}", a[i]);
    c->list.push_back(line);
}

lv2::Config rsx_config()
{
    lv2::Config c;
    c.rsx_log_failures = true;
    return c;
}

Run run(const std::string& flash, const std::vector<Op>& ops, bool sony, const Init& init = {})
{
    Fixture f({}, rsx_config());
    REQUIRE(f.hle->link_internal_functions({"cellGcmSys"}));
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary lib(f, t, flash, "cellGcmSys");
    REQUIRE(f.hle->main_memory().reserve(kMaps, kMapsSize)); // main memory the scripts map for the RSX
    REQUIRE(f.mem.commit(kMaps, kMapsSize));
    Calls calls;
    f.kernel->set_syscall_observer(&observe, &calls);
    u8* base = f.base();
    std::memset(base + kOut, kFill, kOutSize);
    const auto call = [&](const char* name, const std::vector<u64>& args) -> u32 {
        if (sony) {
            std::array<u64, 8> a{};
            std::copy(args.begin(), args.end(), a.begin());
            const std::string_view n = name;
            const u32 nid = n.starts_with("cellGcmSys_") ? static_cast<u32>(std::strtoul(name + 11, nullptr, 16))
                                                         : recomp::nid_of(name);
            return lib.call(t, nid, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
        }
        const Function& fn = GuestThread::find(name);
        for (usize i = 0; i < args.size(); i++) t.ctx.gpr[3 + i] = args[i];
        const u64 sp = t.ctx.gpr[1];
        fn.fn(t.ctx, base);
        CHECK(t.ctx.gpr[1] == sp);
        return static_cast<u32>(t.ctx.gpr[3]);
    };
    Run r;
    calls.on = true; // the init's calls
    r.init_answer = call(init.fn, init.args);
    {
        std::lock_guard lock(calls.mutex);
        r.init_calls = std::move(calls.list);
        calls.list.clear();
    }
    if (r.init_answer != 0) return r;
    const u32 context = ps_read32(base, kCtxPtr);
    for (const u32 off : {0u, 4u, 8u}) r.context.push_back(ps_read32(base, context + off));
    rsx::Rsx* rsx = f.kernel->rsx();
    REQUIRE(rsx);
    const u32 di = 0x10200000; // lv2's driver information (sys_rsx's layout)
    for (const Op& op : ops) {
        const std::string_view name = op.fn;
        if (name == "=") {
            ps_write32(base, static_cast<u32>(op.args[0]), static_cast<u32>(op.args[1]));
        } else if (name == "=di") {
            ps_write32(base, di + static_cast<u32>(op.args[0]), static_cast<u32>(op.args[1]));
        } else if (name == "=di64") {
            ps_write64(base, di + static_cast<u32>(op.args[0]), op.args[1]);
        }
        if (name.starts_with("=")) {
            r.answers.push_back(0);
            continue;
        }
        std::vector<u64> args = op.args;
        for (u64& v : args)
            if (v == kCtxArg) v = ps_read32(base, kCtxPtr);
        const u32 answer = call(op.fn, args);
        r.answers.push_back(op.flags & kVoid ? 0 : answer);
    }
    // Flips lv2 asked for run on the RSX thread: let them finish before looking.
    for (u64 seen = ~0ull; seen != rsx->flip_count();) {
        seen = rsx->flip_count();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(rsx->wait_idle(2'000'000'000ull));
    {
        std::lock_guard lock(calls.mutex);
        calls.on = false;
        r.rsx_calls = calls.list;
    }
    f.kernel->set_syscall_observer(nullptr, nullptr);
    r.out.assign(base + kOut, base + kOut + kOutSize);
    r.driver.assign(base + di + 0x10B8, base + di + 0x12D0);
    // The vblank fields the HLE's interrupt thread changes on its own: not compared.
    for (u32 h = 0; h < 2; h++) {
        for (const u32 off : {0x0u, 0x4u, 0x1Cu, 0x20u, 0x24u, 0x30u, 0x34u, 0x3Cu}) { // flip and vblank times
            std::memset(r.driver.data() + 0x40 * h + off, 0, 4);
        }
    }
    return r;
}

void compare(const std::string& flash, const std::vector<Op>& ops, const std::string& what, const Init& init = {},
    bool driver_info = true)
{
    const Run mine = run(flash, ops, false, init);
    const Run sonys = run(flash, ops, true, init);
    {
        INFO(std::format("{}: the init's answer", what));
        CHECK(mine.init_answer == sonys.init_answer);
        std::string a, b;
        for (const auto& c : mine.init_calls) a += c + "\n";
        for (const auto& c : sonys.init_calls) b += c + "\n";
        INFO(std::format("{}: the init's lv2 RSX calls", what));
        CHECK(a == b);
    }
    if (mine.init_answer != 0 || sonys.init_answer != 0) return;
    // Begin and end. Not current: Sony's init writes 0x3a0 bytes of the RSX's first state into the buffer (current is
    // begin + 0x3a0 after it), pspocket's none, whichever the entry (found with D-1040, not changed by it).
    for (usize i = 0; i < 2; i++) {
        INFO(std::format("{}: the context's word {} (begin, end): ours {:#x}, Sony's {:#x}", what, i,
            mine.context[i], sonys.context[i]));
        CHECK(mine.context[i] == sonys.context[i]);
    }
    for (usize i = 0; i < ops.size(); i++) {
        INFO(std::format("{}: step {} {} -> ours {:#x}, Sony's {:#x}", what, i, ops[i].fn, mine.answers[i],
            sonys.answers[i]));
        CHECK(mine.answers[i] == sonys.answers[i]);
    }
    usize at = 0;
    while (at < mine.out.size() && mine.out[at] == sonys.out[at]) at++;
    for (usize i = at & ~usize{3}; i < std::min<usize>(mine.out.size(), (at & ~usize{3}) + 0x80); i += 4) {
        const u32 x = ps_ld32(mine.out.data() + i), y = ps_ld32(sonys.out.data() + i);
        if (x != y) UNSCOPED_INFO(std::format("{}: output +{:#x}: ours {:#010x}, Sony's {:#010x}", what, i, x, y));
    }
    INFO(std::format("{}: output differs first at +{:#x}", what, at));
    CHECK(at == mine.out.size());
    for (usize i = 0; i + 4 <= mine.driver.size(); i += 4) {
        const u32 x = ps_ld32(mine.driver.data() + i), y = ps_ld32(sonys.driver.data() + i);
        if (x != y) UNSCOPED_INFO(std::format("{}: driver +{:#x}: ours {:#010x}, Sony's {:#010x}", what, 0x10B8 + i, x, y));
    }
    at = 0;
    while (driver_info && at < mine.driver.size() && mine.driver[at] == sonys.driver[at]) at++;
    if (!driver_info) at = mine.driver.size();
    const auto word = [](const std::vector<u8>& v, usize i) {
        i &= ~usize{3};
        return i + 4 <= v.size() ? ps_ld32(v.data() + i) : 0u;
    };
    INFO(std::format("{}: driver information differs first at +{:#x} (ours {:#010x}, Sony's {:#010x})", what,
        0x10B8 + at, word(mine.driver, at), word(sonys.driver, at)));
    CHECK(at == mine.driver.size());
    std::string a, b;
    for (const auto& c : mine.rsx_calls) a += c + "\n";
    for (const auto& c : sonys.rsx_calls) b += c + "\n";
    INFO(std::format("{}: lv2 RSX calls", what));
    CHECK(a == b);
}

} // namespace

TEST_CASE("cellGcmSys against libgcm_sys", "[hle][lle][gcm]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    const std::string& fl = *flash;

    SECTION("init") { compare(fl, {}, "init"); }

    // D-1040: GTA IV's (BLUS30127) init, 0xB70B4AD3 with r7 = 0: the command buffer in local memory, nothing mapped
    // for the RSX; with r7 = 1 it is _cellGcmInitBody. The state each leaves, through the getters.
    SECTION("init: 0xB70B4AD3, the command buffer in local memory or in IO")
    {
        // Not the default command and segment word sizes: Sony's are the init's cmdSize / 4 and 0x2000, pspocket's
        // RPCS3's 0x400 and 0x100, whichever the entry (found with D-1040, not changed by it).
        const std::vector<Op> state{{"cellGcmGetConfiguration", {kOut}, kVoid}, {"cellGcmAddressToOffset", {0xC0000000, kOut + 0x40}}, {"cellGcmAddressToOffset", {0xC0301000, kOut + 0x44}},
            {"cellGcmAddressToOffset", {kIo, kOut + 0x48}}, {"cellGcmAddressToOffset", {kIo + 0x1000, kOut + 0x4C}},
            {"cellGcmIoOffsetToAddress", {0, kOut + 0x50}}, {"cellGcmIoOffsetToAddress", {0x1000, kOut + 0x54}},
            {"cellGcmGetMaxIoMapSize", {}}, {"cellGcmMapMainMemory", {kMaps, 0x100000, kOut + 0x58}},
            {"cellGcmAddressToOffset", {kMaps, kOut + 0x5C}}, {"cellGcmIoOffsetToAddress", {0, kOut + 0x60}}};
        compare(fl, state, "local, as GTA IV", {"cellGcmSys_B70B4AD3", {kCtxPtr, 0x300000, 0, 0, 0}});
        compare(fl, state, "local, an IO buffer given", {"cellGcmSys_B70B4AD3", {kCtxPtr, kCmdSize, 0x100000, kIo, 0}});
        compare(fl, state, "r7 1", {"cellGcmSys_B70B4AD3", {kCtxPtr, kCmdSize, 0x100000, kIo, 1}});
        compare(fl, state, "_cellGcmInitBody");
        // Sony's checks of the size in local memory (0xa50-0xa64): over 0xffff, a multiple of 4.
        compare(fl, {}, "local, too small", {"cellGcmSys_B70B4AD3", {kCtxPtr, 0xFFFF, 0, 0, 0}});
        compare(fl, {}, "local, unaligned", {"cellGcmSys_B70B4AD3", {kCtxPtr, 0x10002, 0, 0, 0}});
    }

    SECTION("the magic number table and the driver information's getters")
    {
        std::vector<Op> ops;
        for (const u32 base : {0xBBBB0000u, 0xBBBB1000u, 0xBBBB2000u, 0xBEEF2000u, 0xBEEF4096u, 0xCCCC0000u})
            for (u32 i = 0; i < 12; i++) ops.push_back({"_cellGcmFunc3", {base + i}});
        for (const u32 v : {0u, 1u, 0xFFFFFFFFu, 0xBEEF1FFFu, 0xBBBAFFFFu}) ops.push_back({"_cellGcmFunc3", {v}});
        ops.push_back({"_cellGcmFunc1", {}});
        ops.push_back({"_cellGcmFunc2", {}});
        ops.push_back({"_cellGcmFunc13", {}});
        ops.push_back({"_cellGcmFunc38", {kOut}});
        // The heads' words: set, then read through each getter.
        ops.push_back({"=di", {0x10B8 + 0x38, 1}});                  // head 0 field
        ops.push_back({"=di", {0x10B8 + 0x40 + 0x38, 7}});           // head 1 field
        ops.push_back({"cellGcmGetCurrentField", {}});
        ops.push_back({"=di64", {0x10B8 + 0x00, 0x123456789ull}});    // head 0 last flip time (ticks)
        ops.push_back({"=di64", {0x10B8 + 0x40, 0x23456789Aull}});    // head 1
        ops.push_back({"cellGcmGetLastFlipTime2", {}, 0});
        ops.push_back({"cellGcmGetLastFlipTime", {}, 0});
        ops.push_back({"=di", {0x10B8 + 0x08, 0x80000000}});
        ops.push_back({"cellGcmGetFlipStatus2", {}});
        ops.push_back({"=di", {0x10B8 + 0x08, 0x40000000}});
        ops.push_back({"cellGcmGetFlipStatus2", {}});
        compare(fl, ops, "getters");
    }

    SECTION("flip status, flip mode, vblank frequencies, handlers")
    {
        compare(fl,
            {{"cellGcmSetFlipStatus2", {}, kVoid}, {"cellGcmGetFlipStatus2", {}}, {"cellGcmResetFlipStatus2", {}, kVoid},
                {"cellGcmGetFlipStatus2", {}}, {"cellGcmSetFlipStatus", {}, kVoid}, {"cellGcmGetFlipStatus", {}},
                {"cellGcmResetFlipStatus", {}, kVoid}, {"cellGcmGetFlipStatus", {}},
                {"cellGcmSetFlipMode2", {1}, kVoid}, {"cellGcmSetFlipMode2", {2}, kVoid},
                {"cellGcmSetFlipMode2", {3}, kVoid}, {"cellGcmSetFlipMode2", {7}, kVoid},
                {"cellGcmSetFlipMode", {1}, kVoid}, {"cellGcmSetFlipMode", {2}, kVoid},
                {"cellGcmSetFlipMode", {3}, kVoid}, {"cellGcmSetVBlankFrequency", {1}, kVoid},
                {"cellGcmSetVBlankFrequency", {2}, kVoid}, {"cellGcmSetVBlankFrequency", {3}, kVoid},
                {"cellGcmSetVBlankFrequency", {9}, kVoid}, {"cellGcmSetSecondVFrequency", {1}, kVoid},
                {"cellGcmSetSecondVFrequency", {2}, kVoid}, {"cellGcmSetSecondVFrequency", {3}, kVoid},
                {"cellGcmSetFlipHandler2", {0x1000}, kVoid}, {"cellGcmSetSecondVHandler", {0x2000}, kVoid},
                {"cellGcmSetFlipHandler", {0x3000}, kVoid}, {"cellGcmSetVBlankHandler", {0x4000}, kVoid},
                {"cellGcmSetUserHandler", {0x5000}, kVoid}, {"cellGcmSetQueueHandler", {0x6000}, kVoid},
                {"cellGcmSetFlipHandler2", {0}, kVoid}, {"cellGcmSetSecondVHandler", {0}, kVoid},
                {"cellGcmSetFlipHandler", {0}, kVoid}, {"cellGcmSetVBlankHandler", {0}, kVoid},
                {"cellGcmSetUserHandler", {0}, kVoid}, {"cellGcmSetQueueHandler", {0}, kVoid},
                {"_cellGcmFunc4", {}}, {"cellGcmDumpGraphicsError", {}}},
            "flip settings");
    }

    SECTION("the hardware cursor")
    {
        compare(fl,
            {{"cellGcmSetCursorEnable", {}}, {"cellGcmUpdateCursor", {}}, {"cellGcmSetCursorPosition", {1, 2}},
                {"cellGcmInitCursor", {}}, {"cellGcmInitCursor", {}}, {"cellGcmUpdateCursor", {}},
                {"cellGcmSetCursorPosition", {100, 0}}, {"cellGcmSetCursorPosition", {4096, 5}},
                {"cellGcmSetCursorPosition", {5, 0xFFFFFFFF}}, {"cellGcmSetCursorPosition", {4095, 4095}},
                {"cellGcmSetCursorImageOffset", {0x100000}}, {"cellGcmSetCursorEnable", {}},
                {"cellGcmUpdateCursor", {}}, {"cellGcmUpdateCursor", {}}, {"cellGcmSetCursorDisable", {}},
                {"cellGcmUpdateCursor", {}}, {"cellGcmSetCursorEnable", {}}, {"cellGcmSetCursorImageOffset", {0x200}},
                {"cellGcmUpdateCursor", {}}},
            "cursor");
    }

    SECTION("sorting the IO mappings")
    {
        std::vector<Op> ops;
        const u32 mb = 0x100000;
        // Mappings of 1, 2 and 3 MB at scattered IO offsets, some unmapped again: holes to pack.
        const u32 maps[][3] = {{kMaps, 5 * mb, mb}, {kMaps + mb, 9 * mb, 2 * mb}, {kMaps + 3 * mb, 2 * mb, mb},
            {kMaps + 4 * mb, 20 * mb, 3 * mb}, {kMaps + 7 * mb, 14 * mb, mb}, {kMaps + 8 * mb, 30 * mb, 2 * mb}};
        for (const auto& m : maps) ops.push_back({"cellGcmMapEaIoAddress", {m[0], m[1], m[2]}});
        ops.push_back({"cellGcmUnmapIoAddress", {2 * mb}});
        ops.push_back({"cellGcmUnmapEaIoAddress", {kMaps + 7 * mb}});
        const auto where = [&](u32 at) {
            for (u32 i = 0; i < 12; i++) ops.push_back({"cellGcmAddressToOffset", {kMaps + i * mb + 0x123, kOut + at + 4 * i}});
            for (u32 io = 0; io < 36; io++)
                ops.push_back({"cellGcmIoOffsetToAddress", {io * mb + 0x40, kOut + at + 0x40 + 4 * io}});
        };
        where(0);
        ops.push_back({"cellGcmSortRemapEaIoAddress", {}});
        where(0x200);
        ops.push_back({"cellGcmSortRemapEaIoAddress", {}});
        ops.push_back({"cellGcmMapEaIoAddress", {kMaps + 10 * mb, 40 * mb, mb}});
        ops.push_back({"cellGcmSortRemapEaIoAddress", {}});
        where(0x400);
        ops.push_back({"cellGcmGetMaxIoMapSize", {}});
        compare(fl, ops, "sort");
    }

    SECTION("flips: immediate, command 2, the flip ring")
    {
        compare(fl,
            {{"cellGcmSetDisplayBuffer", {0, 0x100000, 0x1000, 1280, 720}},
                {"cellGcmSetDisplayBuffer", {1, 0x500000, 0x1000, 1280, 720}},
                {"cellGcmGetDisplayBufferByFlipIndex", {0}}, {"cellGcmGetDisplayBufferByFlipIndex", {1}},
                {"cellGcmGetDisplayBufferByFlipIndex", {9}}, {"cellGcmSetFlipImmediate2", {8}},
                {"cellGcmSetFlipImmediate2", {1}}, {"cellGcmSetFlipImmediate2", {0x101}},
                {"cellGcmGetVBlankCount2", {}}, {"cellGcmGetLastSecondVTime", {}},
                {"=", {kOut + 0x800, kOut + 0x900}}, {"=", {kOut + 0x804, kOut + 0xA00}},
                {"=", {kOut + 0x808, kOut + 0x900}}, {"=", {kOut + 0x80C, 0}},
                {"_cellGcmSetFlipCommand2", {kOut + 0x800, 1}, kVoid}, {"cellGcmSetFlipImmediate", {0}},
                {"cellGcmSetFlipImmediate", {9}}},
            "flips");
    }

    // D-1046: what cellGcmSetPrepareFlip answers on the default context: the flip ring's slot, not the id.
    SECTION("flips: cellGcmSetPrepareFlip's answer")
    {
        compare(fl,
            {{"cellGcmSetDisplayBuffer", {0, 0x100000, 0x1000, 1280, 720}}, {"cellGcmSetPrepareFlip", {kCtxArg, 0}},
                {"cellGcmSetPrepareFlip", {kCtxArg, 0}}, {"cellGcmSetPrepareFlip", {kCtxArg, 0}},
                {"cellGcmSetPrepareFlip", {kCtxArg, 8}}, {"cellGcmSetFlip", {kCtxArg, 0}}},
            "prepare flip", {}, false);
        // Not the driver information: the RSX runs what each wrote, pspocket's GCM_FLIP_COMMAND (RPCS3's, which
        // queues the buffer on head 1 before it flips: flip flags 0xc0000001) and Sony's own words (0x80000000).
    }

    SECTION("tiles: set, invalidate")
    {
        compare(fl,
            {{"cellGcmSetTile", {3, 0, 0x100000, 0x200000, 0x1000, 0, 0, 0}}, {"cellGcmSetInvalidateTile", {3}, kVoid},
                {"cellGcmSetInvalidateTile", {4}, kVoid}, {"cellGcmSetTile", {5, 1, 0x10000, 0x10000, 0x200, 0, 0, 1}},
                {"cellGcmSetInvalidateTile", {5}, kVoid}},
            "tiles");
    }
}
