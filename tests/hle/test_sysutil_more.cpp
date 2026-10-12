// libsysutil's sub-libraries (D-925): pspocket's own against Sony's modules (sysutil_sub_compare.hpp: the same scripts in
// two processes, the system side answering from a thread of the test's), and what pspocket's console answers without
// the firmware. No game data.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "hle_test_util.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil_sub_compare.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::sub;

namespace {

// The script's memory.
constexpr u32 kStr = kData + 0x0100; // strings, 0x40 apart
constexpr u32 kOut = kData + 0x0800; // outputs
constexpr u32 kBuf = kData + 0x1000; // larger outputs
constexpr u32 S(u32 n) { return kStr + 0x40 * n; }

constexpr u32 CLOSED = 0x8002b007;

// Records of `n` bytes each, counting up from `seed`.
std::vector<u8> records(u32 n, u32 size, u8 seed)
{
    std::vector<u8> v(n * size);
    for (usize i = 0; i < v.size(); i++) v[i] = static_cast<u8>(seed + i);
    return v;
}

// ------------------------------------------------------------------------------------------------- cellPrintUtility

void script_print(Script& s)
{
    // The dispatcher pspocket registers, linked in both runs alike.
    s.fixture().hle->link_function("cellPrintUtility", recomp::nid_of("cellPrintUtility_PrintEvent"),
        "cellPrintUtility_PrintEvent");
    const u32 w = kOut, h = kOut + 4, st = kOut + 0x40, sent = kOut + 8, band = kBuf;
    for (u32 i = 0; i < 0x400; i++) s.w8(band + i, static_cast<u8>(i * 3));
    // The dispatcher (the module's start registered one).
    s.call("cellSysutilPrintInit", {});
    s.call("cellSysutilPrintShutdown", {});
    s.call("cellSysutilPrintShutdown", {});
    s.call("cellSysutilPrintShutdown", {}); // none left
    s.call("cellSysutilPrintInit", {});
    // pspocket's console: the channels are closed.
    s.call("cellPrintGetStatus", {st});
    s.call("cellPrintGetPrintableArea", {w, h});
    s.call("cellPrintStartJob", {1, 0});
    s.call("cellPrintEndJob", {});
    s.call("cellPrintCancelJob", {});
    s.call("cellPrintStartPage", {});
    s.call("cellPrintEndPage", {});
    s.call("cellPrintSendBand", {band, 0x100, sent});
    // The checks.
    s.call("cellPrintGetStatus", {0});
    s.call("cellPrintGetPrintableArea", {0, h});
    s.call("cellPrintGetPrintableArea", {w, 0});
    s.call("cellPrintGetPrintableArea", {0x100000000ull, h}); // the width checked as 64 bits
    s.call("cellPrintStartJob", {0, 0});
    s.call("cellPrintStartJob", {0xFFFFFFFF, 0});
    s.call("cellPrintStartJob", {1, 1});
    s.call("cellPrintSendBand", {0, 0x100, sent});
    s.call("cellPrintSendBand", {band, 0, sent});
    s.call("cellPrintSendBand", {band, 0xFFFFFFFF, sent});
    s.call("cellPrintSendBand", {band, 1, 0});
    // A system that answers.
    SystemSide& sys = s.system();
    sys.open(0);
    sys.reply(0, reply("p_j_start", {{"result", 0}}));
    s.call("cellPrintStartJob", {3, 0});
    sys.reply(0, reply("p_j_start", {{"result", -5}}));
    s.call("cellPrintStartJob", {3, 0});
    sys.reply(0, reply("p_j_start", {{"result", 4}}));
    s.call("cellPrintStartJob", {0x7FFFFFFF, 0});
    sys.reply(0, reply("p_j_start", {}));
    s.call("cellPrintStartJob", {3, 0}); // no result
    for (const char* f : {"cellPrintEndJob", "cellPrintCancelJob", "cellPrintStartPage", "cellPrintEndPage"}) {
        sys.reply(0, reply("x", {{"result", 0}}));
        s.call(f, {});
        sys.reply(0, reply("x", {{"result", -1}}));
        s.call(f, {});
    }
    const auto status = [&](s32 result, u32 size) {
        Cxml c;
        const int root = c.element("p_get_status");
        c.i32(root, "result", result);
        if (size != 0xFFFFFFFF) c.file(root, "p_status", records(1, size, 0x60));
        sys.reply(0, c);
        std::memset(s.base() + st, 0xEE, 0x40);
        s.call("cellPrintGetStatus", {st});
    };
    status(0, 44);
    status(1, 44);
    status(0, 40); // not a CellPrintStatus: nothing copied
    status(0, 48);
    status(0, 0xFFFFFFFF);
    status(-3, 44);
    const auto area = [&](std::initializer_list<std::pair<std::string, s32>> ints) {
        sys.reply(0, reply("p_area", ints));
        s.w32(w, 0xEEEEEEEE);
        s.w32(h, 0xEEEEEEEE);
        s.call("cellPrintGetPrintableArea", {w, h});
    };
    area({{"result", 0}, {"p_pixel_width", 640}, {"p_pixel_height", 480}});
    area({{"result", 0}, {"p_pixel_width", 640}});
    area({{"result", 0}, {"p_pixel_height", 480}});
    area({{"result", -2}, {"p_pixel_width", 640}, {"p_pixel_height", 480}});
    s.call("cellPrintEndPage", {}); // no reply: the system goes away
    sys.open(0);
    sys.reply(0, records(0x6100, 1, 0));
    s.call("cellPrintGetStatus", {st}); // truncated
    sys.open(0);
    // The band, on channel 1: the document, then the band in the same packet, then the reply.
    sys.open(1);
    sys.reply(1, reply("p_send_band", {{"result", 0}, {"p_send_size", 0x123}}));
    s.call("cellPrintSendBand", {band, 0x123, sent});
    sys.reply(1, reply("p_send_band", {{"result", -2}, {"p_send_size", 0x10}}));
    s.call("cellPrintSendBand", {band + 0x10, 0x10, sent});
    sys.reply(1, reply("p_send_band", {{"result", 0}}));
    s.call("cellPrintSendBand", {band, 0x20, sent}); // no size sent
    sys.reply(1, reply("p_send_band", {{"p_send_size", 0x20}}));
    s.call("cellPrintSendBand", {band, 0x20, sent}); // no result
    s.call("cellPrintSendBand", {band, 0x30, sent}); // no system: the band waits for room, the channel closes
    sys.open(1);
    sys.reply(1, records(0x6100, 1, 0));
    s.call("cellPrintSendBand", {band, 0x40, sent}); // truncated
    sys.open(1);
    s.call("cellSysutilPrintShutdown", {});
}

// ------------------------------------------------------------------------------------------------- cellPesmUtility

constexpr u64 kPesmMemory = 0x8086000000000200ull; // the recorder's shared memory, the system software's

void script_pesm(Script& s)
{
    const u32 dev = kOut, dst = kOut + 8, size = kOut + 0xC, sinf = kOut + 0x10, sinf_size = kOut + 0x14,
              ver = kOut + 0x18, one = kOut + 0x1C, info = kOut + 0x40, ret = kOut + 0x100, sample = kBuf,
              seen = kBuf + 0x2000;
    s.str(S(0), "/dev_usb000");
    s.str(S(1), "PSP-2000");
    s.str(S(2), "profile");
    for (u32 i = 0; i < 0x2000; i++) s.w8(sample + i, static_cast<u8>(i * 7 + 3));
    const auto set_info = [&](u32 device, u32 movie, u32 seed, u32 bytes) {
        for (u32 i = 0; i < 0x30; i++) s.w8(info + i, static_cast<u8>(0x40 + i));
        s.w32(info, device);
        s.w32(info + 4, movie);
        s.w32(info + 8, seed);
        s.w32(info + 12, bytes);
    };
    // The shared memory's first bytes (its address the module's answers give) where the script's memory is compared.
    const auto look = [&](u32 at) {
        std::memcpy(s.base() + seen, s.base() + at, 0x400);
        s.step(std::format("the shared memory at {:#x}", at));
    };
    set_info(0x1234, 7, 0xABCDEF01, 0x400);
    // The version and the loads; nothing made.
    s.call("cellPesmUtility_6312A6CC", {ver, one});
    s.call("cellPesmUtility_6312A6CC", {0, 0});
    s.call("cellPesmLoadAsync", {});
    s.call("cellPesmUnloadAsync", {});
    s.call("cellPesmUtility_8AB3156B", {});
    // Not initialized.
    s.call("cellPesmOpenDevice", {S(1), dev});
    s.call("cellPesmCloseDevice", {3});
    s.call("cellPesmInitEntry", {0, S(0), ret});
    s.call("cellPesmInitEntry2", {0, S(0), S(2), ret});
    s.call("cellPesmPrepareRec", {0, info});
    s.call("cellPesmStartMovieRec", {0, info});
    s.call("cellPesmEncryptSample", {0, sample, dst, info, size});
    s.call("cellPesmEncryptSample2", {0, sample, 0x100, dst});
    s.call("cellPesmGetSinf", {0, 1, sinf, sinf_size});
    // pspocket's console (closed channels): each request reads itself back.
    s.call("cellPesmEndMovieRec", {0, info});
    s.call("cellPesmFinalize", {});
    s.call("cellPesmFinalize", {});
    s.call("cellPesmFinalize2", {1});
    s.call("cellPesmEndMovieRec", {0, info});
    // The system software's shared memory for the recorder (Sony's module would wait for it for ever without it).
    REQUIRE(s.fixture().kernel->create_system_shared_memory(kPesmMemory, 0x200000, 0x200));
    s.call("cellPesmInitialize", {0x11111111, 0x22222222});
    s.call("cellPesmInitialize", {1, 2});
    s.call("cellPesmOpenDevice", {S(1), dev});
    s.call("cellPesmCloseDevice", {3});
    s.call("cellPesmInitEntry", {0, S(0), ret});
    s.call("cellPesmInitEntry2", {0, S(0), S(2), ret});
    s.call("cellPesmPrepareRec", {0, info});
    s.call("cellPesmStartMovieRec", {0, info});
    s.call("cellPesmEncryptSample", {0, sample, dst, info, size});
    look(s.r32(dst));
    s.call("cellPesmGetSinf", {0, 5, sinf, sinf_size});
    s.call("cellPesmGetSinf", {0, 0, sinf, sinf_size});
    s.call("cellPesmEndMovieRec", {0, info});
    s.call("cellPesmFinalize2", {0});
    // Left in a call: the checks past the state.
    s.call("cellPesmInitialize", {0x11111111, 0});
    s.call("cellPesmPrepareRec", {0, 0});
    s.call("cellPesmOpenDevice", {S(1), dev});
    s.call("cellPesmInitialize", {0x11111111, 0}); // again: the memory is open (one handle)
    set_info(0x1234, 7, 0xABCDEF01, 0x100001);
    s.call("cellPesmEncryptSample", {0, sample, dst, info, size});
    s.call("cellPesmInitialize", {0x11111111, 0});
    s.call("cellPesmEncryptSample2", {0, sample, 0xFFF81, dst});
    s.call("cellPesmInitialize", {0x11111111, 0});
    s.call("cellPesmFinalize", {});
    // A system that answers.
    SystemSide& sys = s.system();
    sys.open(0);
    set_info(0x55, 9, 0x13579BDF, 0x300);
    sys.reply(0, reply("PesmCmdInitialize", {{"PesmResult", 0}}));
    s.call("cellPesmInitialize", {0x11111111, 0x22222222});
    sys.reply(0, reply("PesmCmdOpenDevice", {{"PesmResult", 0}, {"PesmDevice", 7}}));
    s.call("cellPesmOpenDevice", {S(1), dev});
    sys.reply(0, reply("PesmCmdOpenDevice", {{"PesmResult", -3}}));
    s.call("cellPesmOpenDevice", {S(1), dev});
    sys.reply(0, reply("PesmCmdCloseDevice", {{"PesmResult", 0}}));
    s.call("cellPesmCloseDevice", {7});
    const auto entry = [&](const char* root, s32 result, const char* name) {
        Cxml c;
        const int e = c.element(root);
        c.i32(e, "PesmResult", result);
        if (name) c.str(e, "PesmInitentryRetname", name);
        sys.reply(0, c);
    };
    entry("PesmCmdInitEntry", 0, "ms0:/VIDEO/MOVIE.MP4");
    s.call("cellPesmInitEntry", {0, S(0), ret});
    entry("PesmCmdInitEntry", 4, nullptr);
    s.call("cellPesmInitEntry", {0, S(0), ret});
    entry("PesmCmdInitEntry2", 0, "ms0:/MP_ROOT/100MNV01/M4V00001.MP4");
    s.call("cellPesmInitEntry2", {0, S(0), 2, ret});
    entry("PesmCmdInitEntry2", -1, "");
    s.call("cellPesmInitEntry2", {0, S(0), 2, ret});
    sys.reply(0, reply("PesmCmdPrepareRec", {{"PesmResult", 0}}));
    s.call("cellPesmPrepareRec", {0, info});
    sys.reply(0, reply("PesmCmdPrepareRec", {}));
    s.call("cellPesmPrepareRec", {0, info});
    sys.reply(0, reply("PesmCmdStartMovieRec", {{"PesmResult", 0}}));
    s.call("cellPesmStartMovieRec", {0, info}); // 20 bytes of the shared memory over the info
    sys.reply(0, reply("PesmCmdStartMovieRec", {{"PesmResult", 2}}));
    s.call("cellPesmStartMovieRec", {0, info});
    set_info(0x55, 9, 0x13579BDF, 0x300);
    sys.reply(0, reply("PesmCmdEncryptSample", {{"PesmResult", 0}, {"PesmEncryptDstoffset", 0x100},
        {"PesmEncryptSize", 0x2F0}}));
    s.call("cellPesmEncryptSample", {0, sample + 0x10, dst, info, size});
    look(s.r32(dst) - 0x100);
    sys.reply(0, reply("PesmCmdEncryptSample", {{"PesmResult", -7}}));
    s.call("cellPesmEncryptSample", {0, sample, dst, info, size});
    sys.reply(0, reply("PesmCmdEncryptSample2", {{"PesmResult", 0}, {"PesmEncryptDstoffset", 0x80}}));
    s.call("cellPesmEncryptSample2", {0, sample + 0x20, 0x200, dst});
    look(s.r32(dst) - 0x80);
    sys.reply(0, reply("PesmCmdEncryptSample2", {{"PesmResult", -2}, {"PesmEncryptDstoffset", 0x40}}));
    s.call("cellPesmEncryptSample2", {0, sample, 0x10, dst});
    // The size and its header past 4 GB: 0x70 bytes copied. (Its reply always has both values here: the module starts
    // them from a word its stack happens to hold.)
    sys.reply(0, reply("PesmCmdEncryptSample2", {{"PesmResult", 0}, {"PesmEncryptDstoffset", 0x10}}));
    s.call("cellPesmEncryptSample2", {0, sample + 0x30, 0xFFFFFFF0, dst});
    look(s.r32(dst) - 0x10);
    sys.reply(0, reply("PesmCmdGetSinf", {{"PesmResult", 0}, {"PesmSinfSize", 0x40}}));
    s.call("cellPesmGetSinf", {0, 1, sinf, sinf_size});
    s.w32(sinf, 0);
    sys.reply(0, reply("PesmCmdGetSinf", {{"PesmResult", -1}, {"PesmSinfSize", 0x40}}));
    s.call("cellPesmGetSinf", {0, 1, sinf, sinf_size});
    sys.reply(0, reply("PesmCmdGetSinf", {{"PesmResult", 0}, {"PesmSinfSize", 0}}));
    s.call("cellPesmGetSinf", {0, 1, sinf, sinf_size});
    sys.reply(0, reply("PesmCmdGetSinf", {{"PesmResult", 0}}));
    s.call("cellPesmGetSinf", {0, 3, sinf, sinf_size});
    sys.reply(0, reply("PesmCmdEndMovieRec", {{"PesmResult", 0}}));
    s.call("cellPesmEndMovieRec", {0, info});
    // A reply of another root: 0x80861001 and the request kept, whose reply the next calls read for it.
    sys.reply(0, reply("PesmCmdOther", {{"PesmResult", 0}}));
    s.call("cellPesmCloseDevice", {7});
    sys.reply(0, reply("PesmCmdCloseDevice", {{"PesmResult", 5}}));
    s.call("cellPesmOpenDevice", {S(1), dev}); // reads the close's reply: not its root
    sys.reply(0, reply("PesmCmdCloseDevice", {{"PesmResult", 6}}));
    s.call("cellPesmCloseDevice", {8}); // the kept one's: answered
    // A reply that is no document, a truncated one, and the system going away.
    sys.reply(0, records(0x40, 1, 0x30));
    s.call("cellPesmCloseDevice", {8});
    sys.reply(0, records(0x6100, 1, 0));
    s.call("cellPesmCloseDevice", {8});
    sys.open(0);
    s.call("cellPesmCloseDevice", {9}); // no reply: the channel closes; the request reads itself
    sys.open(0);
    sys.reply(0, reply("PesmCmdFinalize2", {{"PesmResult", 0}}));
    s.call("cellPesmFinalize2", {1});
    sys.reply(0, reply("PesmCmdInitialize", {{"PesmResult", -4}}));
    s.call("cellPesmInitialize", {0x11111111, 0x22222222});
    sys.reply(0, reply("PesmCmdFinalize", {}));
    s.call("cellPesmFinalize", {});
    sys.reply(0, reply("PesmCmdOther", {}));
    s.call("cellPesmInitialize", {0x11111111, 0x22222222}); // another root: left initializing
    s.call("cellPesmInitialize", {0x11111111, 0x22222222});
    s.call("cellPesmEndMovieRec", {0, info}); // reads the kept request's reply: none, the channel closes
    sys.open(0);
}

// ------------------------------------------------------------------------------------------------- cellGamePs1Emu

void script_game_ps1emu(Script& s)
{
    const char* const kFn = "cellGamePs1Emu_61CE2BCD";
    // pspocket's console: the closed channel; the mode's check first.
    s.call(kFn, {0});
    s.call(kFn, {1});
    s.call(kFn, {2});
    s.call(kFn, {0xFFFFFFFF});
    s.call(kFn, {0x100000000ull}); // the mode as 32 bits
    // A system that answers (channel 1).
    SystemSide& sys = s.system();
    sys.open(1);
    sys.reply(1, reply("setPadRumble", {{"elmResult", 0}}));
    s.call(kFn, {1});
    sys.reply(1, reply("setPadRumble", {{"elmResult", -7}}));
    s.call(kFn, {0});
    sys.reply(1, reply("other", {{"elmResult", 5}})); // the root's name is not looked at
    s.call(kFn, {1});
    sys.reply(1, reply("setPadRumble", {}));
    s.call(kFn, {1}); // no result
    {
        Cxml c;
        const int e = c.element("setPadRumble");
        c.str(e, "elmResult", "0");
        sys.reply(1, c);
    }
    s.call(kFn, {1}); // a result that is no int
    s.call(kFn, {0}); // no reply: the system goes away
    sys.open(1);
    sys.reply(1, records(0x6100, 1, 0));
    s.call(kFn, {1}); // truncated
    sys.open(1);
}

} // namespace

TEST_CASE("cellPrintUtility against libsysutil_print", "[hle][lle][sysutil]")
{
    compare_script("cellPrintUtility", {"cellPrintUtility"}, script_print);
}

TEST_CASE("cellPrintUtility on pspocket's console: each request meets the closed channel", "[hle][sysutil]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    std::memcpy(b + S(0), "PRINT", 6);
    CHECK(t.call("cellPrintStartJob", 1, 0) == r3_of_code(CLOSED));
    CHECK(t.call("cellPrintStartJob", 0, 0) == r3_of_code(0x8002c404)); // no page: INVALID_PARAM before any request
    CHECK(t.call("cellPrintEndJob") == r3_of_code(CLOSED));
    CHECK(t.call("cellPrintCancelJob") == r3_of_code(CLOSED));
    CHECK(t.call("cellPrintStartPage") == r3_of_code(CLOSED));
    CHECK(t.call("cellPrintEndPage") == r3_of_code(CLOSED));
    CHECK(t.call("cellPrintGetStatus", kOut) == r3_of_code(CLOSED));
    CHECK(t.call("cellPrintSendBand", kBuf, 16, kOut + 0x40) == r3_of_code(CLOSED));
    CHECK(t.call("cellPrintGetStatus", kOut) == r3_of_code(CLOSED)); // the channel given back each time
}

TEST_CASE("cellPesmUtility against libsysutil_pesm", "[hle][lle][sysutil]")
{
    compare_script("cellPesmUtility", {"cellPesmUtility"}, script_pesm);
}

TEST_CASE("cellPesmUtility on pspocket's console: its requests read themselves back, no shared memory waited for",
    "[hle][sysutil]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    u8* b = f.base();
    std::memcpy(b + S(0), "/dev_usb000", 12);
    std::memcpy(b + S(1), "PSP", 4);
    CHECK(t.call("cellPesmUtility_6312A6CC", kOut, kOut + 4) == 0);
    CHECK(ps_read32(b, kOut) == 0x30001);
    CHECK(ps_read32(b, kOut + 4) == 1);
    CHECK(t.call("cellPesmLoadAsync") == r3_of_code(0xFFFFFFFF));
    CHECK(t.call("cellPesmUnloadAsync") == r3_of_code(0xFFFFFFFF));
    CHECK(t.call("cellPesmOpenDevice", S(1), kOut) == r3_of_code(0x80862001)); // not initialized
    CHECK(t.call("cellPesmInitEntry", 0, S(0), kBuf) == 1);
    // Its request read back as the reply: no result in it, so the value it started from (the second container's).
    CHECK(t.call("cellPesmInitialize", 0x11111111, 0) == 0);
    CHECK(t.call("cellPesmInitialize", 0x11111111, 0) == r3_of_code(0x80862001));
    CHECK(t.call("cellPesmOpenDevice", S(1), kOut) == r3_of_code(S(1))); // the name's address, also the device
    CHECK(ps_read32(b, kOut) == S(1));
    CHECK(t.call("cellPesmCloseDevice", 3) == 3);
    // No system software made the recorder's shared memory: not waited for, and the calls that need it say so.
    CHECK_FALSE(f.kernel->shared_memory_key_exists(0x8086000000000200ull));
    CHECK(t.call("cellPesmGetSinf", 0, 1, kOut, kOut + 4) == r3_of_code(0x80862002));
    CHECK(t.call("cellPesmCloseDevice", 3) == r3_of_code(0x80862001)); // left in the call, as Sony's module leaves it
    CHECK(t.call("cellPesmInitialize", 0x11111111, 0) == 0);
    ps_write32(b, kOut + 0x40, 0);
    ps_write32(b, kOut + 0x44, 0x77);
    CHECK(t.call("cellPesmEndMovieRec", 0, kOut + 0x40) == 0x77);
    CHECK(t.call("cellPesmFinalize") == 100); // its request ID
    CHECK(t.call("cellPesmFinalize") == r3_of_code(0x80862001));
    CHECK(t.call("cellPesmEndMovieRec", 0, kOut + 0x40) == r3_of_code(0x80862001));
}

TEST_CASE("cellGamePs1Emu against libsysutil_game_ps1emu", "[hle][lle][sysutil]")
{
    compare_script("cellGamePs1Emu", {"cellGamePs1Emu"}, script_game_ps1emu);
}

TEST_CASE("cellGamePs1Emu on pspocket's console: the pad rumble request meets the closed channel", "[hle][sysutil]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    CHECK(t.call("cellGamePs1Emu_61CE2BCD", 1) == r3_of_code(CLOSED));
    CHECK(t.call("cellGamePs1Emu_61CE2BCD", 0) == r3_of_code(CLOSED));
    CHECK(t.call("cellGamePs1Emu_61CE2BCD", 2) == r3_of_code(0x8002CB07)); // before any request
}
