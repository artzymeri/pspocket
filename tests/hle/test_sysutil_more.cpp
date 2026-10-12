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
