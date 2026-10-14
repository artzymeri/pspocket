// The video codec cores' version queries against Sony's (D-575, D-597): libavcdec, libmvcdec, libsjvtd, libsmvd2,
// libsmvd4 and libsvc1d (in /dev_flash/sys/internal), each in a process of its own with pspocket's functions and with
// Sony's module. Compared: every answer and the 16 bytes around each output (filled first, so a word written past the
// one RPCS3 knows shows too). A pointer outside guest memory is not sent to Sony's (it would write there). The exports
// Sony's module has that pspocket does not implement are listed as notes (what is left of these libraries). No game
// data; skipped without the firmware (compare_util.hpp).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "compare_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

namespace {

constexpr u32 kOut = kData + 0x100;

struct Core {
    const char* library;
    std::vector<const char*> functions; // the version query first; avc and mvc: the characteristics second
    bool void_version;                  // GetVersion answers nothing (avc, mvc)
};

const std::vector<Core>& cores()
{
    static const std::vector<Core> c = {
        {"libavcdec", {"avcDecGetVersion", "avcDecQueryCharacteristics"}, true},
        {"libmvcdec", {"mvcDecGetVersion", "mvcDecQueryCharacteristics"}, true},
        {"libsjvtd", {"sjvtdGetVersionNumber"}, false},
        {"libsmvd2", {"smvd2GetVersionNumber"}, false},
        {"libsmvd4", {"smvd4GetVersionNumber"}, false},
        {"libsvc1d", {"svc1dGetVersionNumber"}, false},
    };
    return c;
}

Observed half(const Setup& s, bool sony_half, const Core& core)
{
    Half<> h(s, sony_half, core.library);
    u8* b = h.b;
    for (usize i = 0; i < core.functions.size(); i++) {
        const char* fn = core.functions[i];
        const bool answers = i > 0 || !core.void_version;
        // A good pointer, a misaligned one (the PPU stores words anywhere) and none.
        for (const u32 out : {kOut, kOut + 2, 0u}) {
            std::memset(b + kOut - 4, 0xEE, 24);
            const std::string key = std::format("{}({:#x})", fn, out);
            trace("calling: " + key);
            const std::optional<u64> v = h.side.call(fn, {out});
            if (!v) h.o.line(key, "(not exported)");
            else if (answers) h.o.line(key, answer(*v));
            h.o.line(key + ": the bytes around the output", hex(b + kOut - 4, 24));
        }
    }
    // What else Sony's module exports (the library's decoding calls, still Sony's alone).
    if (const FirmwareLibrary* fw = h.side.firmware()) {
        std::string names;
        usize n = 0;
        for (const auto& [key, descriptor] : fw->module->functions) {
            if (key.first != core.library) continue;
            if (hle::registry().find(core.library, key.second)) continue;
            const std::optional<std::string_view> name = recomp::NidDb::builtin().find(core.library, key.second);
            names += std::format(" {}", name ? std::string(*name) : std::format("{:#010x}", key.second));
            n++;
        }
        h.o.note(std::format("Sony's {} also exports {} functions pspocket does not implement:{}", core.library, n, names));
    }
    return std::move(h.o);
}

} // namespace

TEST_CASE("The codec cores' comparison calls Sony's exports by their names; pspocket's version queries, firmware first",
    "[hle][codec-cores]")
{
    check_names("libavcdec", {"avcDecGetVersion", "avcDecQueryCharacteristics"}, false); // pspocket's own (D-933)
    check_names("libmvcdec", {"mvcDecGetVersion", "mvcDecQueryCharacteristics"}, false); // pspocket's own (D-933)
    check_names("libsjvtd", {"sjvtdGetVersionNumber"}, false); // all of libsjvtd is pspocket's own (D-920)
    check_names("libsmvd2", {"smvd2GetVersionNumber"}, false); // all of libsmvd2 is pspocket's own (D-920)
    check_names("libsmvd4", {"smvd4GetVersionNumber"}, false); // all of libsmvd4 is pspocket's own (D-920)
    check_names("libsvc1d", {"svc1dGetVersionNumber"}, false); // all of libsvc1d is pspocket's own (D-920)
}

TEST_CASE("The video codec cores' version queries against Sony's modules", "[.][hle][lle][compare][codec-cores]")
{
    const Setup s = setup();
    for (const Core& core : cores()) {
        INFO(core.library);
        run(s, std::format("{} against Sony's {}.sprx", core.library, core.library),
            [&](bool sony) { return half(s, sony, core); },
            [&](const Observed& o) {
                // pspocket's own half: RPCS3's versions (test_codec_cores.cpp).
                const std::string fn = core.functions[0];
                const std::string bytes = o.value(fn + std::format("({:#x}): the bytes around the output", kOut));
                const std::string version = std::string(core.library) == "libavcdec" ? "00011300"
                    : std::string(core.library) == "libmvcdec"                       ? "00010804"
                    : std::string(core.library) == "libsjvtd"                        ? "01050000"
                    : std::string(core.library) == "libsmvd2"                        ? "01030000"
                    : std::string(core.library) == "libsmvd4"                        ? "01080000"
                                                                                     : "01090000";
                CHECK(bytes == "hex:eeeeeeee" + version + "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
                if (!core.void_version) CHECK(o.value(fn + std::format("({:#x})", 0)) != "0x0");
                else
                    CHECK(o.value(core.functions[1] + std::format("({:#x}): the bytes around the output", kOut)) ==
                        std::format("hex:eeeeeeee00000005{:08x}eeeeeeeeeeeeeeeeeeeeeeee",
                            std::string(core.library) == "libavcdec" ? 20 : 36));
            });
    }
}
