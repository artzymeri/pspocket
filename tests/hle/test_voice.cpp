// cellVoice (synthetic: no game data): RPCS3's libvoice without voice devices. Init and end, ports kept as data with
// RPCS3's IDs and checks, the input/output topology checks, the service state, and reads and writes that move nothing.

#include "hle_test_util.hpp"

#include <cstring>
#include <string_view>
#include <utility>

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u64 kNotInit = 0xFFFFFFFF80310801ull, kInitialized = 0xFFFFFFFF80310802ull,
              kArgInvalid = 0xFFFFFFFF80310805ull, kTopology = 0xFFFFFFFF80310807ull,
              kResource = 0xFFFFFFFF80310808ull, kAddrInvalid = 0xFFFFFFFF8031080Aull,
              kDetached = 0xFFFFFFFF8031080Bull, kNoDevice = 0xFFFFFFFF80310812ull;

// CellVoicePortType.
constexpr u32 kInMic = 0, kInPcm = 1, kInVoice = 2, kOutPcm = 3, kOutVoice = 4, kOutSecondary = 5;
// What cellVoiceGetBitRate leaves when it refuses a port (RPCS3).
constexpr u32 kBitRateUnknown = 0x4f323285;

constexpr u32 kInitParam = kData + 0x100, kParam = kData + 0x200, kId = kData + 0x300, kOut = kData + 0x400,
              kBuf = kData + 0x800;
constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

// A CellVoicePortParam (28 bytes) at kParam: the type, threshold 100, not muted, volume 1.0, `word` at 12 (a voice
// port's bit rate, a PCM port's buffer size), then a PCM format of one 16 kHz channel with sample type `pcm_type`.
void put_port_param(u8* b, u32 type, u32 word, u32 pcm_type = 0)
{
    std::memset(b + kParam, 0, 28);
    ps_write32(b, kParam + 0, type);
    ps_write16(b, kParam + 4, 100);
    ps_write32(b, kParam + 8, 0x3F800000);
    ps_write32(b, kParam + 12, word);
    ps_write8(b, kParam + 16, 1);
    ps_write8(b, kParam + 17, 4);
    ps_write32(b, kParam + 20, pcm_type);
    ps_write32(b, kParam + 24, 16000);
}

void voice_up(GuestThread& g, u8* b)
{
    std::memset(b + kInitParam, 0, 32); // CellVoiceInitParam {eventMask 0, version 100, appType 0}
    ps_write32(b, kInitParam + 4, 100);
    REQUIRE(g.call("cellVoiceInit", kInitParam) == 0);
}

// Creates a port and returns its ID.
u32 create(GuestThread& g, u8* b, u32 type, u32 word, u32 pcm_type = 0)
{
    put_port_param(b, type, word, pcm_type);
    ps_write32(b, kId, 0xEEEEEEEE);
    REQUIRE(g.call("cellVoiceCreatePort", kId, kParam) == 0);
    return ps_read32(b, kId);
}

} // namespace

TEST_CASE("cellVoice: the 14 functions GTA V imports", "[hle][voice]")
{
    const std::pair<std::string_view, u32> expected[] = {
        {"cellVoiceInit", 0xC7CF1182},
        {"cellVoiceEnd", 0xE0E1AE12},
        {"cellVoiceStart", 0x0A563878},
        {"cellVoiceStop", 0xD3A84BE1},
        {"cellVoiceCreatePort", 0x2DE54871},
        {"cellVoiceDeletePort", 0x9F70C475},
        {"cellVoiceConnectIPortToOPort", 0xAE6A21D5},
        {"cellVoiceDisconnectIPortFromOPort", 0x18D3DF30},
        {"cellVoiceGetPortInfo", 0x54AC3519},
        {"cellVoiceSetPortAttr", 0x9D0F4AF1},
        {"cellVoiceGetPortAttr", 0xF629ED67},
        {"cellVoiceGetBitRate", 0xBEF53A2B},
        {"cellVoiceReadFromOPort", 0x36472C57},
        {"cellVoiceWriteToIPort", 0x3DAD26E7},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const auto& [name, nid] : expected) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("cellVoice", nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
}

TEST_CASE("cellVoice: nothing works before init or after end; end deletes the ports", "[hle][voice]")
{
    Fixture f;
    GuestThread g(f, 0, "voice");
    u8* b = f.base();

    CHECK(g.call("cellVoiceEnd") == kNotInit);
    CHECK(g.call("cellVoiceStart") == kNotInit);
    CHECK(g.call("cellVoiceStop") == kNotInit);
    put_port_param(b, kInMic, 0);
    ps_write32(b, kId, 0xEEEEEEEE);
    CHECK(g.call("cellVoiceCreatePort", kId, kParam) == kNotInit);
    CHECK(g.call("cellVoiceCreatePort", kId, 0) == kNotInit); // init is checked first
    CHECK(ps_read32(b, kId) == 0xEEEEEEEE);
    CHECK(g.call("cellVoiceDeletePort", 0x101) == kNotInit);
    CHECK(g.call("cellVoiceConnectIPortToOPort", 0x101, 0x102) == kNotInit);
    CHECK(g.call("cellVoiceDisconnectIPortFromOPort", 0x101, 0x102) == kNotInit);
    CHECK(g.call("cellVoiceGetPortInfo", 0x101, kOut) == kNotInit);
    CHECK(g.call("cellVoiceSetPortAttr", 0x101, 1001, kOut) == kNotInit);
    CHECK(g.call("cellVoiceGetPortAttr", 0x101, 1001, kOut) == kNotInit);
    ps_write32(b, kOut, 0xAAAAAAAA);
    CHECK(g.call("cellVoiceGetBitRate", 0x101, kOut) == kNotInit);
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAA);
    CHECK(g.call("cellVoiceReadFromOPort", 0x102, kBuf, kOut) == kNotInit);
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAA);
    CHECK(g.call("cellVoiceWriteToIPort", 0x101, kBuf, kOut) == kNotInit);

    // Init needs a parameter block (not read), once.
    CHECK(g.call("cellVoiceInit", 0) == kArgInvalid);
    voice_up(g, b);
    CHECK(g.call("cellVoiceInit", kInitParam) == kInitialized);
    CHECK(g.call("cellVoiceInit", 0) == kInitialized); // checked before the parameter

    // End deletes the ports and stops the service; the next init starts the IDs again.
    CHECK(create(g, b, kInMic, 0) == 0x0101);
    CHECK(create(g, b, kOutPcm, 0x1000) == 0x0102);
    REQUIRE(g.call("cellVoiceStart") == 0);
    REQUIRE(g.call("cellVoiceEnd") == 0);
    CHECK(g.call("cellVoiceEnd") == kNotInit);
    CHECK(g.call("cellVoiceDeletePort", 0x101) == kNotInit);
    voice_up(g, b);
    CHECK(g.call("cellVoiceDeletePort", 0x101) == kTopology);
    CHECK(g.call("cellVoiceDeletePort", 0x102) == kTopology);
    CHECK(create(g, b, kInMic, 0) == 0x0101);
    CHECK(g.call("cellVoiceGetPortInfo", 0x101, kOut) == kDetached);
    REQUIRE(g.call("cellVoiceEnd") == 0);
}

TEST_CASE("cellVoice: port parameters, IDs from the creation counter, at most 128 ports", "[hle][voice]")
{
    Fixture f;
    GuestThread g(f, 0, "voice");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    voice_up(g, b);

    // The parameters, then the types: PCM ports need a known sample type (0..5), voice ports an exact bit rate, mic
    // and secondary ports take anything. A refused port gets no ID and uses no counter value.
    ps_write32(b, kId, 0xEEEEEEEE);
    CHECK(g.call("cellVoiceCreatePort", kId, 0) == kArgInvalid);
    CHECK(g.call("cellVoiceCreatePort", kId, kUnmapped) == kAddrInvalid); // RPCS3 faults
    for (const u32 type : {6u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        put_port_param(b, type, 0);
        CHECK(g.call("cellVoiceCreatePort", kId, kParam) == kArgInvalid);
    }
    put_port_param(b, kInPcm, 0x1000, 6);
    CHECK(g.call("cellVoiceCreatePort", kId, kParam) == kArgInvalid);
    put_port_param(b, kOutPcm, 0x1000, 0xFFFFFFFF);
    CHECK(g.call("cellVoiceCreatePort", kId, kParam) == kArgInvalid);
    for (const u32 rate : {0u, 3849u, 4000u, 22534u, 0xFFFFFFFFu}) {
        put_port_param(b, kInVoice, rate);
        CHECK(g.call("cellVoiceCreatePort", kId, kParam) == kArgInvalid);
        put_port_param(b, kOutVoice, rate);
        CHECK(g.call("cellVoiceCreatePort", kId, kParam) == kArgInvalid);
    }
    CHECK(ps_read32(b, kId) == 0xEEEEEEEE);
    put_port_param(b, kInMic, 0);
    CHECK(g.call("cellVoiceCreatePort", 0, kParam) == kAddrInvalid); // the ID's pointer: RPCS3 faults

    // IDs: 0x100 | the counter, from 1.
    u32 expected = 0x0101;
    for (const u32 rate : {3850u, 4650u, 5700u, 7300u, 14400u, 16000u, 22533u}) {
        CHECK(create(g, b, kInVoice, rate) == expected++);
        CHECK(create(g, b, kOutVoice, rate) == expected++);
    }
    for (u32 pcm_type = 0; pcm_type <= 5; pcm_type++) {
        CHECK(create(g, b, kInPcm, 0x1000, pcm_type) == expected++);
        CHECK(create(g, b, kOutPcm, 0x1000, pcm_type) == expected++);
    }
    CHECK(create(g, b, kInMic, 0xFFFFFFFF, 0xFFFFFFFF) == expected++);
    CHECK(create(g, b, kOutSecondary, 0xFFFFFFFF, 0xFFFFFFFF) == expected++);
    REQUIRE(expected == 0x0101 + 28);

    // At most 128 ports.
    while (expected < 0x0101 + 128) CHECK(create(g, b, kInMic, 0) == expected++);
    put_port_param(b, kInMic, 0);
    CHECK(g.call("cellVoiceCreatePort", kId, kParam) == kResource);
    REQUIRE(g.call("cellVoiceDeletePort", 0x0105) == 0);
    CHECK(g.call("cellVoiceDeletePort", 0x0105) == kTopology);
    CHECK(create(g, b, kInMic, 0) == 0x0181); // the counter didn't move for the refused one
    CHECK(g.call("cellVoiceCreatePort", kId, kParam) == kResource);
    for (u32 id = 0x0101; id <= 0x0181; id++)
        if (id != 0x0105) REQUIRE(g.call("cellVoiceDeletePort", id) == 0);

    // The counter runs modulo 0xA0 (0x82..0x9F, then 0); an ID still in use moves the next one to the next high byte.
    // The upper 16 bits of an ID are ignored.
    const u32 keep = create(g, b, kInMic, 0);
    CHECK(keep == 0x0182);
    for (u32 i = 0x83; i <= 0xA0; i++) {
        const u32 id = create(g, b, kInMic, 0);
        CHECK(id == (0x0100 | (i % 0xA0)));
        REQUIRE(g.call("cellVoiceDeletePort", 0xABCD0000u | id) == 0);
    }
    for (u32 i = 1; i < 0x82; i++) {
        const u32 id = create(g, b, kInMic, 0);
        CHECK(id == (0x0100 | i));
        REQUIRE(g.call("cellVoiceDeletePort", id) == 0);
    }
    CHECK(create(g, b, kInMic, 0) == 0x0282); // 0x0182 is still there
    CHECK(create(g, b, kInMic, 0) == 0x0183);
    CHECK(g.call("cellVoiceDeletePort", 0xFFFF0282u) == 0);
    CHECK(g.call("cellVoiceDeletePort", 0x0282) == kTopology);
    REQUIRE(g.call("cellVoiceEnd") == 0);
}

TEST_CASE("cellVoice: topology, service state, port info, bit rates, attributes, reads and writes", "[hle][voice]")
{
    Fixture f;
    GuestThread g(f, 0, "voice");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    voice_up(g, b);
    const u32 mic = create(g, b, kInMic, 0);
    const u32 pcm_in = create(g, b, kInPcm, 0x1000, 2);
    const u32 voice_in = create(g, b, kInVoice, 7300);
    const u32 pcm_out = create(g, b, kOutPcm, 0x1000, 0);
    const u32 voice_out = create(g, b, kOutVoice, 16000);
    const u32 secondary = create(g, b, kOutSecondary, 0);
    const u32 inputs[] = {mic, pcm_in, voice_in};
    const u32 outputs[] = {pcm_out, voice_out, secondary};
    constexpr u32 kNoPort = 0x0909;

    // Connecting and disconnecting: an input port, then an output port (nothing is recorded).
    for (const char* fn : {"cellVoiceConnectIPortToOPort", "cellVoiceDisconnectIPortFromOPort"}) {
        INFO(fn);
        for (const u32 in : inputs) {
            for (const u32 out : outputs) {
                CHECK(g.call(fn, in, out) == 0);
                CHECK(g.call(fn, out, in) == kTopology);
                CHECK(g.call(fn, in, in) == kTopology);
                CHECK(g.call(fn, out, out) == kTopology);
                CHECK(g.call(fn, 0xFFFF0000u | in, out) == 0);
            }
            CHECK(g.call(fn, in, kNoPort) == kTopology);
            CHECK(g.call(fn, kNoPort, outputs[0]) == kTopology);
        }
    }

    // Port info needs the service running; only the type is written.
    std::memset(b + kOut, 0xAA, 24);
    CHECK(g.call("cellVoiceGetPortInfo", kNoPort, kOut) == kTopology); // the port is checked first
    CHECK(g.call("cellVoiceGetPortInfo", voice_in, kOut) == kDetached);
    CHECK(g.call("cellVoiceStop") == 0); // not started: nothing to do
    REQUIRE(g.call("cellVoiceStart") == 0);
    CHECK(g.call("cellVoiceStart") == 0);
    CHECK(g.call("cellVoiceGetPortInfo", kNoPort, kOut) == kTopology);
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAA);
    for (const u32 port : {mic, pcm_in, voice_in, pcm_out, voice_out, secondary}) {
        REQUIRE(g.call("cellVoiceGetPortInfo", port, kOut) == 0);
        CHECK(ps_read32(b, kOut) == (port & 0xFF) - 1); // created in type order
        CHECK(ps_read32(b, kOut + 4) == 0xAAAAAAAA);
        CHECK(ps_read32(b, kOut + 20) == 0xAAAAAAAA);
    }
    CHECK(g.call("cellVoiceGetPortInfo", mic, 0) == kAddrInvalid); // RPCS3 doesn't check: it faults
    REQUIRE(g.call("cellVoiceStop") == 0);
    CHECK(g.call("cellVoiceGetPortInfo", mic, kOut) == kDetached);

    // Bit rates: voice ports only; the unknown value is written first.
    ps_write32(b, kOut, 0);
    REQUIRE(g.call("cellVoiceGetBitRate", voice_in, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 7300);
    REQUIRE(g.call("cellVoiceGetBitRate", 0x12340000u | voice_out, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 16000);
    for (const u32 port : {mic, pcm_in, pcm_out, secondary, kNoPort}) {
        ps_write32(b, kOut, 0);
        CHECK(g.call("cellVoiceGetBitRate", port, kOut) == kTopology);
        CHECK(ps_read32(b, kOut) == kBitRateUnknown);
    }
    CHECK(g.call("cellVoiceGetBitRate", voice_in, 0) == kAddrInvalid); // RPCS3 doesn't check: it faults

    // Attributes: every port reports its device missing; the value isn't touched.
    ps_write32(b, kOut, 0xAAAAAAAA);
    for (const u32 port : {mic, pcm_in, voice_in, pcm_out, voice_out, secondary}) {
        CHECK(g.call("cellVoiceSetPortAttr", port, 1001, kOut) == kNoDevice);
        CHECK(g.call("cellVoiceGetPortAttr", port, 1000, kOut) == kNoDevice);
        CHECK(g.call("cellVoiceGetPortAttr", port, 9999, 0) == kNoDevice);
    }
    CHECK(ps_read32(b, kOut) == 0xAAAAAAAA);
    CHECK(g.call("cellVoiceSetPortAttr", kNoPort, 1001, kOut) == kTopology);
    CHECK(g.call("cellVoiceGetPortAttr", kNoPort, 1001, kOut) == kTopology);

    // Reads: output ports only; nothing to read (size 0, the buffer untouched). Writes: input ports only; the size is
    // left as it is.
    std::memset(b + kBuf, 0x55, 64);
    for (const u32 out : outputs) {
        ps_write32(b, kOut, 64);
        REQUIRE(g.call("cellVoiceReadFromOPort", out, kBuf, kOut) == 0);
        CHECK(ps_read32(b, kOut) == 0);
        CHECK(g.call("cellVoiceReadFromOPort", out, kBuf, 0) == 0);
        CHECK(g.call("cellVoiceReadFromOPort", out, kBuf, kUnmapped) == kAddrInvalid); // RPCS3 faults
        ps_write32(b, kOut, 64);
        CHECK(g.call("cellVoiceWriteToIPort", out, kBuf, kOut) == kTopology);
        CHECK(ps_read32(b, kOut) == 64);
    }
    CHECK(b[kBuf] == 0x55);
    CHECK(b[kBuf + 63] == 0x55);
    for (const u32 in : inputs) {
        ps_write32(b, kOut, 64);
        CHECK(g.call("cellVoiceReadFromOPort", in, kBuf, kOut) == kTopology);
        CHECK(ps_read32(b, kOut) == 64);
        REQUIRE(g.call("cellVoiceWriteToIPort", in, kBuf, kOut) == 0);
        CHECK(ps_read32(b, kOut) == 64);
    }
    CHECK(g.call("cellVoiceReadFromOPort", kNoPort, kBuf, kOut) == kTopology);
    CHECK(g.call("cellVoiceWriteToIPort", kNoPort, kBuf, kOut) == kTopology);

    // A deleted port is gone for every function.
    REQUIRE(g.call("cellVoiceDeletePort", voice_out) == 0);
    CHECK(g.call("cellVoiceGetBitRate", voice_out, kOut) == kTopology);
    CHECK(g.call("cellVoiceReadFromOPort", voice_out, kBuf, kOut) == kTopology);
    CHECK(g.call("cellVoiceConnectIPortToOPort", voice_in, voice_out) == kTopology);
    CHECK(g.call("cellVoiceGetPortAttr", voice_out, 1001, kOut) == kTopology);
    REQUIRE(g.call("cellVoiceEnd") == 0);
}
