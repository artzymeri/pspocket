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
#include "sysutil_sub.hpp"
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

// ------------------------------------------------------------------------------------------------- cellDtcpIpUtility

constexpr u64 kDtcpMemory = 0x8006040000000A10ull; // the client's shared memory, the system software's
constexpr u32 kDtcpCode = kData + 0x3800;          // the game's callback (its descriptor at +0xF0)
constexpr u32 kDtcpRecords = kData + 0x3A00;       // what it was called with: a count, then 0x20 bytes a call

// The DTCP-IP client's set-up in both runs: the game's callback (it records r3..r7: stw r3..r7 at
// records + 0x10 + 0x20 * count, the count incremented), the module's data named alike in the lv2 calls (Sony's in its
// data segment, pspocket's block laid out the same). Answers the data's address.
u32 dtcp_setup(Script& s)
{
    const u32 words[] = {0x3D600000u | (kDtcpRecords >> 16), 0x616B0000u | (kDtcpRecords & 0xFFFF), 0x818B0000u,
        0x558A2834u, 0x7D4A5A14u, 0x906A0010u, 0x908A0014u, 0x90AA0018u, 0x90CA001Cu, 0x90EA0020u, 0x398C0001u,
        0x918B0000u, 0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) s.w32(kDtcpCode + 4 * i, words[i]);
    s.w32(kDtcpCode + 0xF0, kDtcpCode);
    s.w32(kDtcpCode + 0xF4, 0);
    u32 data = 0;
    if (s.sony()) {
        // Its data segment (vaddr 0x8740 in the module's image) as the loader placed it; 0x87E0 is 0xA0 into it.
        REQUIRE(s.module().segments.size() >= 2);
        data = s.module().segments[1].first + 0xA0;
    } else {
        data = pspocket::hle::sysutil::sub::dtcp_data();
    }
    REQUIRE(data);
    s.name_region(data, 0x200, "dtcp");
    return data;
}

// An lv2 call of the script's (a memory container's making), its own step.
u32 script_sc(Script& s, u32 n, std::initializer_list<u64> args, const char* what)
{
    const u32 r = static_cast<u32>(lv2_syscall(s.thread().ctx, n, args));
    s.step(std::format("{} -> {:#x}", what, r));
    return r;
}

// The open options: `n` IDs and values.
void dtcp_options(Script& s, u32 ids, u32 values, std::initializer_list<std::pair<u32, u32>> o)
{
    u32 i = 0;
    for (const auto& [id, v] : o) {
        s.w32(ids + 4 * i, id);
        s.w32(values + 4 * i, v);
        i++;
    }
}

void script_dtcp(Script& s)
{
    const u32 data = dtcp_setup(s), cb = kDtcpCode + 0xF0;
    const u32 url = S(0), ip = S(1), host = kOut + 0x80, nohost = kOut + 0x90, ids = kOut + 0xA0, vals = kOut + 0xC0,
              got = kOut, emi = kOut + 4, copied = kOut + 8, freep = kOut + 12, status = kOut + 16, buf = kBuf,
              seen = kBuf + 0x1000, big = kData + 0x20000, cid = kOut + 0x20, mem = kOut + 0x24;
    s.str(url, "http://192.168.0.2:8200/MediaItems/22.mp4");
    s.str(ip, "192.168.0.2");
    s.w32(host, ip);
    s.w8(host + 4, 0x20);
    s.w8(host + 5, 0x08);
    for (u32 i = 0; i < 0x20000; i++) s.w8(big + i, static_cast<u8>(i * 13 + (i >> 9)));
    const auto look = [&](u32 at, const char* what) {
        std::memcpy(s.base() + seen, s.base() + at, 0x400);
        s.step(std::format("the shared memory: {}", what));
    };
    const auto shm = [&] { return s.r32(data + 0x190); };

    // Not initialized: no shared memory, no callback.
    s.call("cellDtcpIpSuspendActivationForDebug", {});
    for (const char* f : {"cellDtcpIpActivate", "cellDtcpIpCheckActivation", "cellDtcpIpClose",
             "cellDtcpIpStartSequence", "cellDtcpIpStopSequence", "cellDtcpIpFinalize",
             "cellDtcpIpUtilityClearTextByte_7F4A8B0E", "cellDtcpIpUtilityClearTextByte_1EC068FB",
             "cellDtcpIpUtilityClearTextByte_550D3527", "cellDtcpIpUtilityClearTextByte_97619DB5",
             "cellDtcpIpUtilityMove_A98DEADE", "cellDtcpIpUtilityMove_9A219C98"})
        s.call(f, {});
    s.call("cellDtcpIpOpen", {url, host});
    s.call("cellDtcpIpRead", {buf, 0x100});
    s.call("cellDtcpIpRead", {0, 0x100});
    s.call("cellDtcpIpRead", {buf, 0});
    s.call("cellDtcpIpRead", {buf, 0x20001});
    s.call("cellDtcpIpSeek", {1000});
    s.call("cellDtcpIpSetEncryptedData", {big, 0x100, 0, copied, freep});
    s.call("cellDtcpIpGetDecryptedData", {0, 0x100, got, emi});
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x100, 0, emi});
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x100, got, 0});
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x100, got, emi});
    s.call("cellDtcpIpUtilityClearTextByte_0AA9D63F", {host});
    s.call("cellDtcpIpUtilityClearTextByte_65A86A64", {status});
    s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {url, 0, 0, 0});
    s.call("cellDtcpIpUtilityClearTextByte_2070C99E", {0, 0});
    s.call("cellDtcpIpUtilityMove_269A634B", {url, host, 0, 0, 0});
    s.call("cellDtcpIpUtilityMove_F4EF1B28", {url, host});
    s.call("cellDtcpIpUtilityMove_CA87F652", {buf, 0x100});
    s.call("cellDtcpIpUtilityEx_B17436B5", {big, 0x100, 0, copied, freep, 1});
    s.call("cellDtcpIpUtilityEx_4654188A", {url, host, 0, 0, 0});
    s.dispatch(0x14A, 1ull << 32, 0); // no dispatcher registered

    // Initialize's checks: the container (none, not there, too small, not all available), the callback.
    s.call("cellDtcpIpInitialize", {0xFFFFFFFF, cb, 0x1234, 1000, 0});
    s.call("cellDtcpIpInitialize", {0x12345, cb, 0x1234, 1000, 0});
    script_sc(s, 324, {cid, 0x500000}, "sys_memory_container_create(5 MB)");
    const u32 c5 = s.r32(cid);
    s.call("cellDtcpIpInitialize", {c5, cb, 0x1234, 1000, 0});
    script_sc(s, 324, {cid, 0x600000}, "sys_memory_container_create(6 MB)");
    const u32 c6 = s.r32(cid);
    s.call("cellDtcpIpInitialize", {c6, 0, 0x1234, 1000, 0});
    script_sc(s, 324, {cid, 0x700000}, "sys_memory_container_create(7 MB)");
    const u32 c7 = s.r32(cid);
    script_sc(s, 350, {0x100000, c7, 0x400, mem}, "sys_memory_allocate_from_container(1 MB)");
    s.call("cellDtcpIpInitialize", {c7, cb, 0x1234, 1000, 0});
    // pspocket's console: Initialize meets the closed channel and undoes itself.
    s.call("cellDtcpIpInitialize", {c6, cb, 0x1234, 1000, 0});
    s.call("cellDtcpIpInitialize", {c6, cb, 0x1234, 1000, 0x4000000000000001ull}); // the move bit cleared
    s.call("cellDtcpIpActivate", {});

    // A system that answers; the system software's shared memory for the client.
    SystemSide& sys = s.system();
    sys.open(0);
    REQUIRE(s.fixture().kernel->create_system_shared_memory(kDtcpMemory, 0x180000, 0x200));
    sys.reply(0, reply("x", {{"result", -9}}));
    s.call("cellDtcpIpInitialize", {c6, cb, 0x1234, 1000, 0}); // refused: undone
    sys.reply(0, reply("x", {{"result", 3}}));
    s.call("cellDtcpIpInitialize", {c6, cb, 0x1234, 1000, 0});
    s.call("cellDtcpIpInitialize", {c6, cb, 0x1234, 1000, 0}); // already
    s.call("cellDtcpIpActivate", {});                          // no shared memory yet
    s.dispatch(0x14A, 1ull << 32 | 5, 0x77ull << 32 | 0x55);  // a status not 0: nothing made
    s.dispatch(0x14A, 1ull << 32, 0x77ull << 32 | 0x55);       // the shared memory made
    for (u32 i = 0; i < 0x400; i++) s.w8(shm() + i, static_cast<u8>(0x80 + i * 3));
    look(shm(), "after its making");
    for (const u32 kind : {3u, 4u, 5u, 7u, 0xCu, 0xDu, 0x1000001u, 0x1000002u, 0x1000003u, 0x1000004u, 0x1000005u,
             0x2000001u, 0x3000001u, 0xEu, 0u})
        s.dispatch(0x14A, u64{kind} << 32 | 2, 0x11ull << 32 | 0x22);
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpActivate", {});
    sys.reply(0, reply("x", {{"result", -3}}));
    s.call("cellDtcpIpCheckActivation", {});
    sys.reply(0, reply("x", {}));
    s.call("cellDtcpIpCheckActivation", {}); // no result
    {
        Cxml c;
        const int e = c.element("x");
        c.str(e, "result", "0");
        sys.reply(0, c);
    }
    s.call("cellDtcpIpActivate", {}); // a result that is no int
    sys.reply(0, records(0x40, 1, 0x30));
    s.call("cellDtcpIpActivate", {}); // no document
    sys.reply(0, records(0x6100, 1, 0));
    s.call("cellDtcpIpActivate", {}); // truncated
    sys.open(0);
    s.call("cellDtcpIpActivate", {}); // no reply: the system goes away
    sys.open(0);
    // Opening.
    s.call("cellDtcpIpOpen", {0, host});
    s.call("cellDtcpIpOpen", {url, 0});
    s.call("cellDtcpIpOpen", {url, nohost});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpOpen", {url, host});
    s.call("cellDtcpIpUtilityEx_4654188A", {url, host, 0, 0, 0}); // OpenEx: the buffering mode only
    // OpenContentEx's options.
    dtcp_options(s, ids, vals, {{2, 4}, {20000, 1}, {1, 2}, {3, 0x80}, {4, 1}, {5, 0xFFFF}});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {url, 6, ids, vals});
    s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {0, 0, 0, 0});
    s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {url, 1, 0, vals});
    s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {url, 1, ids, 0});
    for (const auto& [id, v] : std::initializer_list<std::pair<u32, u32>>{{1, 3}, {2, 0x401}, {3, 0x81}, {4, 0x81},
             {20000, 2}, {6, 0}, {0, 0}, {0xFFFFFFFF, 0}}) {
        dtcp_options(s, ids, vals, {{5, 1}, {id, v}});
        s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {url, 2, ids, vals});
    }
    // Authentication, the content's end.
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_7F4A8B0E", {});
    s.call("cellDtcpIpUtilityClearTextByte_0AA9D63F", {0});
    s.call("cellDtcpIpUtilityClearTextByte_0AA9D63F", {nohost});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_0AA9D63F", {host});
    sys.reply(0, reply("x", {{"result", -1}}));
    s.call("cellDtcpIpUtilityClearTextByte_1EC068FB", {});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_550D3527", {});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_97619DB5", {});
    // The HTTP status: the reply's "param".
    s.call("cellDtcpIpUtilityClearTextByte_65A86A64", {0});
    sys.reply(0, reply("x", {{"result", 0}, {"param", 404}}));
    s.call("cellDtcpIpUtilityClearTextByte_65A86A64", {status});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_65A86A64", {status}); // no param
    sys.reply(0, reply("x", {{"result", -2}, {"param", 200}}));
    s.call("cellDtcpIpUtilityClearTextByte_65A86A64", {status});
    // Reading: one at a time; the system's event 9 copies it.
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpRead", {buf, 0x100});
    s.call("cellDtcpIpRead", {buf, 0x100}); // busy
    s.dispatch(0x14A, 9ull << 32, 0x80ull << 32 | 0x33);
    s.dispatch(0x14A, 9ull << 32, 0x80ull << 32 | 0x33); // no buffer
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpRead", {buf + 0x200, 0x40});
    s.dispatch(0x14A, 9ull << 32, 0x41ull << 32); // larger than the buffer
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpRead", {buf + 0x200, 0x40});
    s.dispatch(0x14A, 9ull << 32 | 7, 0x10ull << 32 | 1); // a status: passed on
    sys.reply(0, reply("x", {{"result", -4}}));
    s.call("cellDtcpIpRead", {buf + 0x200, 0x40}); // refused: the buffer forgotten
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityMove_CA87F652", {buf + 0x300, 0x20});
    s.dispatch(0x14A, 9ull << 32, 0x20ull << 32);
    // Seeking: one at a time, ended by the system's events 8 and 0x2000002.
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpSeek", {0x123456789ull});
    s.call("cellDtcpIpSeek", {5}); // busy
    s.call("cellDtcpIpUtilityClearTextByte_2070C99E", {0x10, 0x20}); // busy
    s.dispatch(0x14A, 8ull << 32, 0);
    s.call("cellDtcpIpUtilityClearTextByte_2070C99E", {0x7FFFFFFFFFFFull, 0});
    s.call("cellDtcpIpUtilityClearTextByte_2070C99E", {0, 0x7FFFFFFFFFFFull});
    s.call("cellDtcpIpUtilityClearTextByte_2070C99E", {0x200, 0x100});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_2070C99E", {0x7FFFFFFFFFFEull, 0});
    s.dispatch(0x14A, 0x2000002ull << 32 | 1, 0);
    sys.reply(0, reply("x", {{"result", -6}}));
    s.call("cellDtcpIpUtilityClearTextByte_2070C99E", {0x100, 0x200}); // refused: not seeking
    // Moving.
    s.call("cellDtcpIpUtilityMove_269A634B", {url, host, 1, 0, vals});
    s.call("cellDtcpIpUtilityMove_269A634B", {0, host, 0, 0, 0});
    s.call("cellDtcpIpUtilityMove_269A634B", {url, nohost, 0, 0, 0});
    for (const auto& [id, v] : std::initializer_list<std::pair<u32, u32>>{{2, 0x401}, {3, 0x81}, {4, 0x81}, {1, 1},
             {20000, 1}}) {
        dtcp_options(s, ids, vals, {{5, 3}, {id, v}});
        s.call("cellDtcpIpUtilityMove_269A634B", {url, host, 2, ids, vals});
    }
    dtcp_options(s, ids, vals, {{2, 0x400}, {3, 8}, {4, 0x80}, {5, 2}});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityMove_269A634B", {url, host, 4, ids, vals});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityMove_F4EF1B28", {url, host});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityMove_9A219C98", {});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityMove_A98DEADE", {}); // the cancel set
    s.call("cellDtcpIpRead", {buf, 0x10});
    s.call("cellDtcpIpUtilityClearTextByte_65A86A64", {status});
    s.call("cellDtcpIpSeek", {1});
    s.dispatch(0x14A, 8ull << 32, 0);        // a seek's end while cancelled
    s.dispatch(0x14A, 9ull << 32, 0x10ull << 32); // a read's end while cancelled
    s.dispatch(0x14A, 10ull << 32, 0x10ull << 32 | 0x10); // ring space while cancelled: not passed on
    s.dispatch(0x14A, 6ull << 32 | 3, 0);    // the buffers reset: the cancel cleared
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpStartSequence", {});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpStopSequence", {});
    s.dispatch(0x14A, 6ull << 32, 0);
    sys.reply(0, reply("x", {{"result", -1}}));
    s.call("cellDtcpIpStopSequence", {}); // refused: no cancel
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpClose", {});
    // Finalizing: the system's event 2 finalizes.
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpFinalize", {});
    s.dispatch(0x14A, 2ull << 32 | 4, 0);
    s.call("cellDtcpIpFinalize", {});
    s.call("cellDtcpIpActivate", {});
    s.dispatch(0x14A, 2ull << 32, 0); // no dispatcher any more

    // The buffering mode.
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpInitialize", {c6, cb, 0x99, 500, 1});
    s.dispatch(0x14A, 1ull << 32, 0);
    for (const char* f : {"cellDtcpIpUtilityClearTextByte_7F4A8B0E", "cellDtcpIpUtilityClearTextByte_97619DB5"})
        s.call(f, {});
    s.call("cellDtcpIpRead", {buf, 0x10});
    s.call("cellDtcpIpSeek", {1});
    s.call("cellDtcpIpUtilityClearTextByte_2070C99E", {0, 0});
    s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {url, 0, 0, 0});
    s.call("cellDtcpIpUtilityClearTextByte_65A86A64", {status});
    s.call("cellDtcpIpUtilityMove_CA87F652", {buf, 0x10});
    dtcp_options(s, ids, vals, {{3, 64}, {5, 9}, {4, 1}});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityEx_4654188A", {url, host, 3, ids, vals});
    for (const auto& [id, v] : std::initializer_list<std::pair<u32, u32>>{{2, 1}, {4, 0x81}, {3, 0x81}}) {
        dtcp_options(s, ids, vals, {{id, v}});
        s.call("cellDtcpIpUtilityEx_4654188A", {url, host, 1, ids, vals});
    }
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpOpen", {0, host}); // no URL needed
    // The ring: full until the system resets it; then filled, consumed, wrapped.
    s.call("cellDtcpIpSetEncryptedData", {big, 0x8000, 0, copied, freep});
    s.call("cellDtcpIpSetEncryptedData", {0, 0x8000, 0, copied, freep});
    s.dispatch(0x14A, 6ull << 32, 0);
    s.call("cellDtcpIpSetEncryptedData", {0, 0, 0, copied, freep}); // nothing: no request
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpSetEncryptedData", {0, 0, 1, copied, freep}); // the file's end alone
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpSetEncryptedData", {big, 0x18000, 0, copied, freep});
    look(shm() + 0x40000, "the ring's start");
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityEx_B17436B5", {big + 0x100, 0x10000, 1, copied, 0, 1});
    sys.reply(0, reply("x", {{"result", 2}}));
    s.call("cellDtcpIpUtilityEx_B17436B5", {big, 0x100, 0, 0, freep, 0});
    s.dispatch(0x14A, 10ull << 32 | 1, 0x20000ull << 32 | 0x20000);
    for (u32 i = 0; i < 8; i++) {
        sys.reply(0, reply("x", {{"result", 0}}));
        s.call("cellDtcpIpSetEncryptedData", {big + 0x10 * i, 0x20000, i & 1, copied, freep});
    }
    s.call("cellDtcpIpSetEncryptedData", {big, 0x100, 0, copied, freep}); // full: nothing
    s.dispatch(0x14A, 10ull << 32, 0x60000ull << 32 | 0x40000);
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpSetEncryptedData", {big + 0x777, 0x1FFFF, 1, copied, freep}); // around the ring's end
    look(shm() + 0x40000, "the ring's start, wrapped");
    look(shm() + 0x40000 + 0xFFC00, "the ring's end");
    // Decrypted data: the system's event 11 fills the slots.
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x200, got, emi}); // empty
    for (u32 i = 0; i < 0x200; i++) s.w8(shm() + i, static_cast<u8>(i ^ 0x5A));
    for (u32 i = 0; i < 0x200; i++) s.w8(shm() + 0x20000 + i, static_cast<u8>(i ^ 0xA5));
    s.dispatch(0x14A, 0xBull << 32, 0x100ull << 32 | 3);
    s.dispatch(0x14A, 0xBull << 32 | 1, 0x80ull << 32 | 1);
    s.dispatch(0x14A, 0xBull << 32, 0ull << 32 | 2);
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x200, got, emi});
    s.call("cellDtcpIpGetDecryptedData", {buf + 0x200, 0x40, got, emi}); // too small
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpGetDecryptedData", {buf + 0x200, 0x80, got, emi});
    sys.reply(0, reply("x", {{"result", -8}}));
    s.call("cellDtcpIpGetDecryptedData", {buf + 0x300, 0x80, got, emi}); // an empty one
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x200, got, emi});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpStopSequence", {}); // the slots stopped, the cancel set
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x200, got, emi});
    s.call("cellDtcpIpSetEncryptedData", {big, 0x100, 0, copied, freep});
    s.dispatch(0x14A, 0xBull << 32, 0x10ull << 32 | 1); // a stopped slot kept
    s.dispatch(0x14A, 6ull << 32, 0);
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x200, got, emi});
    // Finalizing refused: undone here.
    sys.reply(0, reply("x", {{"result", -1}}));
    s.call("cellDtcpIpFinalize", {});
    s.call("cellDtcpIpGetDecryptedData", {buf, 0x200, got, emi});
    s.dispatch(0x14A, 0xBull << 32, 0x10ull << 32 | 1); // no dispatcher
    // The move mode here: the system's memory is the other mode's size, so its making fails.
    script_sc(s, 324, {cid, 0x500000}, "sys_memory_container_create(5 MB)");
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_545E08CB", {s.r32(cid), cb, 8, 1000, 0x4000000000000000ull});
    s.dispatch(0x14A, 1ull << 32, 0);
    s.call("cellDtcpIpActivate", {});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpFinalize", {});
    s.dispatch(0x14A, 2ull << 32, 0);
}

// The move mode: the shared memory's smaller size (5 MB of container suffice), Open unavailable, the data
// connection's buffer required.
void script_dtcp_move(Script& s)
{
    const u32 data = dtcp_setup(s), cb = kDtcpCode + 0xF0;
    (void)data;
    const u32 url = S(0), ip = S(1), host = kOut + 0x80, ids = kOut + 0xA0, vals = kOut + 0xC0, cid = kOut + 0x20;
    s.str(url, "http://10.0.0.1/c.mp4");
    s.str(ip, "10.0.0.1");
    s.w32(host, ip);
    s.w8(host + 5, 80);
    script_sc(s, 324, {cid, 0x500000}, "sys_memory_container_create(5 MB)");
    const u32 c5 = s.r32(cid);
    s.call("cellDtcpIpInitialize", {c5, cb, 7, 1000, 0x4000000000000000ull}); // the move bit cleared: 6 MB needed
    SystemSide& sys = s.system();
    sys.open(0);
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_545E08CB", {c5, cb, 7, 1000, 0x4000000000000000ull});
    REQUIRE(s.fixture().kernel->create_system_shared_memory(kDtcpMemory, 0xD0000, 0x200));
    s.dispatch(0x14A, 1ull << 32, 0);
    s.call("cellDtcpIpOpen", {url, host});
    s.call("cellDtcpIpUtilityMove_F4EF1B28", {url, host});
    s.call("cellDtcpIpUtilityMove_269A634B", {url, host, 0, 0, 0}); // no buffer size
    dtcp_options(s, ids, vals, {{2, 0x101}});
    s.call("cellDtcpIpUtilityMove_269A634B", {url, host, 1, ids, vals});
    dtcp_options(s, ids, vals, {{2, 0x100}});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityMove_269A634B", {url, host, 1, ids, vals});
    s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {url, 0, 0, 0});
    dtcp_options(s, ids, vals, {{2, 1}, {20000, 1}});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityClearTextByte_2F87926F", {url, 2, ids, vals});
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpUtilityMove_CA87F652", {kBuf, 0x100});
    s.dispatch(0x14A, 9ull << 32, 0x100ull << 32);
    sys.reply(0, reply("x", {{"result", 0}}));
    s.call("cellDtcpIpFinalize", {});
    s.dispatch(0x14A, 2ull << 32, 0);
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

TEST_CASE("cellDtcpIpUtility against libsysutil_dtcp_ip", "[hle][lle][sysutil]")
{
    compare_script("cellDtcpIpUtility",
        {"cellDtcpIpUtility", "cellDtcpIpUtilityClearTextByte", "cellDtcpIpUtilityMove", "cellDtcpIpUtilityEx"},
        script_dtcp);
}

TEST_CASE("cellDtcpIpUtility against libsysutil_dtcp_ip: the move mode", "[hle][lle][sysutil]")
{
    compare_script("cellDtcpIpUtility (move)",
        {"cellDtcpIpUtility", "cellDtcpIpUtilityClearTextByte", "cellDtcpIpUtilityMove", "cellDtcpIpUtilityEx"},
        script_dtcp_move);
}

TEST_CASE("cellDtcpIpUtility on pspocket's console: Initialize meets the closed channel and undoes itself",
    "[hle][sysutil]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    u8* b = f.base();
    const u32 cb = kData + 0x3000; // a callback's descriptor (never called: no system sends events)
    ps_write32(b, cb, kData + 0x3100);
    CHECK(t.call("cellDtcpIpActivate") == r3_of_code(0x80024E02));
    CHECK(t.call("cellDtcpIpFinalize") == r3_of_code(0x80024E02));
    CHECK(t.call("cellDtcpIpInitialize", 0xFFFFFFFF, cb, 0, 1000, 0) == r3_of_code(0x8002B107));
    REQUIRE(lv2_syscall(t.ctx, 324, {kOut, 0x600000}) == 0); // sys_memory_container_create
    const u32 container = ps_read32(b, kOut);
    for (int i = 0; i < 2; i++) CHECK(t.call("cellDtcpIpInitialize", container, cb, 0, 1000, 0) == r3_of_code(CLOSED));
    CHECK(hle::sysutil_dispatcher(0x14A) == 0); // given back
    CHECK(t.call("cellDtcpIpFinalize") == r3_of_code(0x80024E02));
    CHECK(t.call("cellDtcpIpRead", kBuf, 0x10) == r3_of_code(0x80024E02));
    CHECK(t.call("cellDtcpIpSuspendActivationForDebug") == 0);
}
