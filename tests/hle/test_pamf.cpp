// cellPamf (D-193): RPCS3's libpamf over a synthetic PAMF header built here (no game data): two streams, AVC on
// channel 0 with three entry points and ATRAC3plus on channel 0 with one. The header's checks, the reader, the codec
// information, the entry points and their iterator, and the LLE bugs RPCS3 keeps. Then the same calls to Sony's
// libpamf from the user's installed system software, when PSPOCKET_TEST_DEV_FLASH names it (firmware_compare.hpp):
// every answer and every byte written must be the same.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kHdr = kData + 0x10000, kReader = kData + 0x20000, kOut = kData + 0x21000, kIt = kData + 0x22000,
              kEp = kData + 0x23000, kTs = kData + 0x24000;
constexpr u64 kHeaderSize = 0x800, kDataSize = 0x8000;

constexpr u32 STREAM_NOT_FOUND = 0x80610501, INVALID_PAMF = 0x80610502, INVALID_ARG = 0x80610503,
              UNKNOWN_TYPE = 0x80610504, UNSUPPORTED_VERSION = 0x80610505, UNKNOWN_STREAM = 0x80610506,
              EP_NOT_FOUND = 0x80610507, NOT_AVAILABLE = 0x80610508;
constexpr u32 TYPE_AVC = 0, TYPE_ATRAC3PLUS = 2, TYPE_LPCM = 3, TYPE_VIDEO = 20, TYPE_AUDIO = 21;

// The header: sequence info at 0x50, its grouping period at 0x70, the group at 0x82, two 48-byte stream headers at
// 0x88, the entry point tables right after them (0xE8, then 0x10C), as libpamf lays them out.
void write_pamf(u8* b)
{
    std::memset(b + kHdr, 0, kHeaderSize);
    std::memcpy(b + kHdr, "PAMF0041", 8);
    ps_write32(b, kHdr + 0x08, 1);    // header size, in 2048-byte units
    ps_write32(b, kHdr + 0x0C, 0x10); // data size
    const u32 seq = kHdr + 0x50;
    ps_write32(b, seq, 0x94);                  // size, up to the end of the streams
    ps_write32(b, seq + 0x08, 0x1000);         // start pts
    ps_write32(b, seq + 0x0E, 0x100000);       // end pts
    ps_write32(b, seq + 0x12, 0xFF123456);     // mux rate bound (22 bits used)
    ps_write32(b, seq + 0x1A, 2);              // total streams
    ps_write8(b, seq + 0x1F, 1);               // grouping periods
    const u32 gp = kHdr + 0x70;
    ps_write32(b, gp, 0x74);
    ps_write32(b, gp + 0x06, 0x1000);
    ps_write32(b, gp + 0x0C, 0x100000);
    ps_write8(b, gp + 0x11, 1); // groups
    const u32 group = kHdr + 0x82;
    ps_write32(b, group, 0x62);
    ps_write8(b, group + 5, 2); // streams
    // Stream 0: AVC, channel 0, 3 entry points at 0xE8. High profile 4.1, 1280x720, frame-MBs-only, 29.97 fps
    // (frameRateInfo 3 + 1), SAR given, signal information (format 5, full range, BT.709), CABAC, deblocking.
    const u32 avc = kHdr + 0x88;
    ps_write8(b, avc + 0, 0x1b);
    ps_write8(b, avc + 4, 0xe0);
    ps_write32(b, avc + 8, 0xE8);
    ps_write32(b, avc + 12, 3);
    const u8 avc_info[26] = {100, 41, 0xC4, 0xff, 0, 16, 0, 9, 0, 80, 0, 45, 0, 0, 0, 0, 0, 0, 0, 8, 0xB0, 1, 1, 1,
        0xE2, 0x30};
    std::memcpy(b + avc + 16, avc_info, sizeof avc_info);
    // Stream 1: ATRAC3plus, channel 0, 1 entry point at 0x10C; stereo, 48 kHz.
    const u32 at3 = kHdr + 0xB8;
    ps_write8(b, at3 + 0, 0xdc);
    ps_write8(b, at3 + 4, 0xbd);
    ps_write8(b, at3 + 5, 0x00);
    ps_write32(b, at3 + 8, 0x10C);
    ps_write32(b, at3 + 12, 1);
    ps_write8(b, at3 + 16 + 2, 2);
    ps_write8(b, at3 + 16 + 3, 1);
    // Entry points: value0 (indexN - 1 in the top two bits, the reference picture offset), pts, rpn offset.
    const u32 eps[3][3] = {{(1u << 14) | 5, 0x1000, 0}, {(1u << 14) | 6, 0x5000, 4}, {(2u << 14) | 7, 0x9000, 8}};
    for (u32 i = 0; i < 3; i++) {
        ps_write16(b, kHdr + 0xE8 + 12 * i, static_cast<u16>(eps[i][0]));
        ps_write32(b, kHdr + 0xE8 + 12 * i + 4, eps[i][1]);
        ps_write32(b, kHdr + 0xE8 + 12 * i + 8, eps[i][2]);
    }
    ps_write32(b, kHdr + 0x10C + 4, 0x1000);
}

u32 code(u64 r) { return static_cast<u32>(r); }

} // namespace

TEST_CASE("cellPamf: the header's sizes and checks, as libpamf", "[hle][pamf]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    write_pamf(b);
    CHECK(a.call("cellPamfVerify", kHdr, kHeaderSize + kDataSize) == 0);
    CHECK(a.call("cellPamfVerify", kHdr, 0) == 0); // a file size of 0 is not checked
    CHECK(code(a.call("cellPamfVerify", kHdr, kHeaderSize)) == INVALID_PAMF);
    CHECK(a.call("cellPamfGetHeaderSize", kHdr, 0, kOut) == 0);
    CHECK(ps_read64(b, kOut) == kHeaderSize);
    CHECK(a.call("cellPamfGetStreamOffsetAndSize", kHdr, 0, kOut, kOut + 8) == 0);
    CHECK(ps_read64(b, kOut + 8) == kDataSize);
    CHECK(a.call("cellPamfGetHeaderSize2", kHdr, 0, 2, kOut) == 0); // the minimum header: no entry point tables
    CHECK(ps_read64(b, kOut) == 0xE8);

    SECTION("magic and version")
    {
        ps_write32(b, kHdr, 0x50534D46); // "PSMF"
        CHECK(code(a.call("cellPamfVerify", kHdr, 0)) == UNKNOWN_TYPE);
        ps_write32(b, kHdr, 0x50414D46);
        std::memcpy(b + kHdr + 4, "0039", 4);
        CHECK(code(a.call("cellPamfGetHeaderSize", kHdr, 0, kOut)) == UNSUPPORTED_VERSION);
    }
    SECTION("a channel used twice for one type")
    {
        ps_write8(b, kHdr + 0xB8, 0x1b);
        ps_write8(b, kHdr + 0xB8 + 4, 0xe0);
        CHECK(code(a.call("cellPamfVerify", kHdr, 0)) == INVALID_PAMF);
    }
    SECTION("an unknown stream")
    {
        ps_write8(b, kHdr + 0xB8, 0x42);
        CHECK(code(a.call("cellPamfVerify", kHdr, 0)) == UNKNOWN_STREAM);
    }
    SECTION("entry points going back, or a gap between the tables")
    {
        ps_write32(b, kHdr + 0xE8 + 12 + 8, 9);
        CHECK(code(a.call("cellPamfVerify", kHdr, 0)) == INVALID_PAMF);
        ps_write32(b, kHdr + 0xE8 + 12 + 8, 4);
        ps_write32(b, kHdr + 0xB8 + 8, 0x110);
        CHECK(code(a.call("cellPamfVerify", kHdr, 0)) == INVALID_PAMF);
        CHECK(code(a.call("cellPamfReaderInitialize", kReader, kHdr, 0, 3)) == 0); // the minimum header skips them
    }
    SECTION("the end time not after the start")
    {
        ps_write32(b, kHdr + 0x50 + 0x0E, 0x1000);
        CHECK(code(a.call("cellPamfVerify", kHdr, 0)) == INVALID_PAMF);
    }
    SECTION("the stream counts disagree")
    {
        ps_write32(b, kHdr + 0x50 + 0x1A, 3);
        CHECK(code(a.call("cellPamfVerify", kHdr, 0)) == INVALID_PAMF);
    }
}

TEST_CASE("cellPamf: the reader, its streams and their codec information", "[hle][pamf]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    write_pamf(b);
    REQUIRE(a.call("cellPamfReaderInitialize", kReader, kHdr, kHeaderSize + kDataSize, 1) == 0);
    CHECK(ps_read64(b, kReader) == kHeaderSize);
    CHECK(ps_read16(b, kReader + 0x16) == 41); // the version
    CHECK(a.call("cellPamfReaderGetStreamIndex", kReader) == 0);
    CHECK(a.call("cellPamfReaderGetNumberOfStreams", kReader) == 2);
    CHECK(a.call("cellPamfReaderGetNumberOfSpecificStreams", kReader, TYPE_VIDEO) == 1);
    CHECK(a.call("cellPamfReaderGetNumberOfSpecificStreams", kReader, TYPE_AUDIO) == 1);
    CHECK(a.call("cellPamfReaderGetNumberOfSpecificStreams", kReader, TYPE_LPCM) == 0);
    CHECK(a.call("cellPamfReaderGetMuxRateBound", kReader) == 0x123456);
    CHECK(a.call("cellPamfReaderGetPresentationStartTime", kReader, kTs) == 0);
    CHECK(ps_read32(b, kTs) == 0);
    CHECK(ps_read32(b, kTs + 4) == 0x1000);
    CHECK(a.call("cellPamfReaderGetPresentationEndTime", kReader, kTs) == 0);
    CHECK(ps_read32(b, kTs + 4) == 0x100000);

    // The AVC stream's information.
    CHECK(code(a.call("cellPamfReaderGetStreamInfo", kReader, kOut, 0x1F)) == INVALID_ARG); // too small for it
    REQUIRE(a.call("cellPamfReaderGetStreamInfo", kReader, kOut, 0x20) == 0);
    CHECK(ps_read8(b, kOut + 0x00) == 100);
    CHECK(ps_read8(b, kOut + 0x01) == 41);
    CHECK(ps_read8(b, kOut + 0x02) == 1);       // frameMbsOnlyFlag
    CHECK(ps_read8(b, kOut + 0x03) == 1);       // videoSignalInfoFlag
    CHECK(ps_read8(b, kOut + 0x04) == 3);       // frameRateInfo: CELL_PAMF_AVC_FRC_30000DIV1001
    CHECK(ps_read8(b, kOut + 0x05) == 0xff);
    CHECK(ps_read16(b, kOut + 0x06) == 16);     // the SAR, given with aspect ratio 0xff
    CHECK(ps_read16(b, kOut + 0x08) == 9);
    CHECK(ps_read16(b, kOut + 0x0A) == 1280);
    CHECK(ps_read16(b, kOut + 0x0C) == 720);
    CHECK(ps_read16(b, kOut + 0x14) == 8);      // frameCropBottomOffset
    CHECK(ps_read8(b, kOut + 0x16) == 5);       // videoFormat
    CHECK(ps_read8(b, kOut + 0x17) == 1);       // videoFullRangeFlag
    CHECK(ps_read8(b, kOut + 0x18) == 1);       // BT.709
    CHECK(ps_read8(b, kOut + 0x1B) == 1);       // CABAC
    CHECK(ps_read8(b, kOut + 0x1C) == 1);       // deblocking
    CHECK(ps_read8(b, kOut + 0x1D) == 2);       // minNumSlicePerPictureIdc
    CHECK(ps_read8(b, kOut + 0x1E) == 2);       // nfwIdc
    CHECK(ps_read8(b, kOut + 0x1F) == 0x30);
    CHECK(a.call("cellPamfReaderGetEsFilterId", kReader, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0xe0);
    CHECK(ps_read32(b, kOut + 4) == 0);
    CHECK(ps_read32(b, kOut + 8) == 1); // AVC

    // The audio stream, by type and index.
    CHECK(a.call("cellPamfReaderSetStreamWithTypeAndIndex", kReader, TYPE_AUDIO, 0) == 1);
    CHECK(a.call("cellPamfReaderGetStreamIndex", kReader) == 1);
    CHECK(code(a.call("cellPamfReaderSetStreamWithTypeAndIndex", kReader, TYPE_AUDIO, 1)) == STREAM_NOT_FOUND);
    CHECK(code(a.call("cellPamfReaderSetStreamWithTypeAndIndex", kReader, TYPE_AUDIO, 2)) == INVALID_ARG);
    REQUIRE(a.call("cellPamfReaderGetStreamTypeAndChannel", kReader, kOut, kOut + 1) == 0);
    CHECK(ps_read8(b, kOut) == TYPE_ATRAC3PLUS);
    CHECK(ps_read8(b, kOut + 1) == 0);
    REQUIRE(a.call("cellPamfReaderGetStreamInfo", kReader, kOut, 8) == 0);
    CHECK(ps_read32(b, kOut) == 1); // CELL_PAMF_FS_48kHz
    CHECK(ps_read8(b, kOut + 4) == 2);
    CHECK(a.call("cellPamfReaderGetEsFilterId", kReader, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0xbd);
    CHECK(ps_read32(b, kOut + 8) == 0);

    // By index; past the streams.
    CHECK(a.call("cellPamfReaderSetStreamWithIndex", kReader, 0) == 0);
    CHECK(code(a.call("cellPamfReaderSetStreamWithIndex", kReader, 2)) == INVALID_ARG);
    CHECK(a.call("cellPamfReaderGetStreamIndex", kReader) == 0);

    // By type and channel, broken on LLE: it steps 16 stream headers at a time, so only the first is found, and a
    // stream it misses leaves the error code as the index.
    CHECK(a.call("cellPamfReaderSetStreamWithTypeAndChannel", kReader, TYPE_AVC, 0) == 0);
    CHECK(code(a.call("cellPamfReaderSetStreamWithTypeAndChannel", kReader, TYPE_ATRAC3PLUS, 0)) == STREAM_NOT_FOUND);
    CHECK(code(a.call("cellPamfReaderGetStreamIndex", kReader)) == STREAM_NOT_FOUND);
    CHECK(ps_read32(b, kReader + 0x34) == kHdr + 0x88); // the first stream
    CHECK(code(a.call("cellPamfReaderSetStreamWithTypeAndChannel", kReader, 99, 0)) == INVALID_ARG);

    // A stream type and channel as an elementary stream filter.
    CHECK(a.call("cellPamfStreamTypeToEsFilterId", TYPE_LPCM, 1, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0xbd);
    CHECK(ps_read32(b, kOut + 4) == 0x41);
    CHECK(ps_read32(b, kOut + 8) == 0);
    CHECK(a.call("cellPamfStreamTypeToEsFilterId", TYPE_AVC, 2, kOut) == 0);
    CHECK(ps_read32(b, kOut) == 0xe2);
    CHECK(ps_read32(b, kOut + 8) == 1);
    CHECK(code(a.call("cellPamfStreamTypeToEsFilterId", 99, 0, kOut)) == INVALID_ARG);
    CHECK(code(a.call("cellPamfStreamTypeToEsFilterId", TYPE_AVC, 0, 0)) == INVALID_ARG);
}

TEST_CASE("cellPamf: entry points by index and by time, and their iterator", "[hle][pamf]")
{
    Fixture f;
    GuestThread a(f, 0, "ppu");
    u8* b = f.base();
    write_pamf(b);
    REQUIRE(a.call("cellPamfReaderInitialize", kReader, kHdr, 0, 1) == 0);
    CHECK(a.call("cellPamfReaderGetNumberOfEp", kReader) == 3);

    REQUIRE(a.call("cellPamfReaderGetEpIteratorWithIndex", kReader, 1, kIt) == 0);
    CHECK(ps_read8(b, kIt) == 1); // PAMF
    CHECK(ps_read32(b, kIt + 4) == 1);
    CHECK(ps_read32(b, kIt + 8) == 3);
    REQUIRE(a.call("cellPamfEpIteratorGetEp", kIt, kEp) == 0);
    CHECK(ps_read32(b, kEp) == 2);                  // indexN
    CHECK(ps_read32(b, kEp + 4) == 6 * 0x800 + 0x800); // nThRefPictureOffset
    CHECK(ps_read32(b, kEp + 12) == 0x5000);        // pts
    CHECK(ps_read64(b, kEp + 16) == 4 * 0x800);     // rpnOffset
    CHECK(code(a.call("cellPamfReaderGetEpIteratorWithIndex", kReader, 3, kIt)) == INVALID_ARG);

    // Moving clamps at both ends and says how far it went.
    CHECK(static_cast<s32>(a.call("cellPamfEpIteratorMove", kIt, 5, kEp)) == 1);
    CHECK(ps_read32(b, kIt + 4) == 2);
    CHECK(ps_read32(b, kEp) == 3);
    CHECK(static_cast<s32>(a.call("cellPamfEpIteratorMove", kIt, -10, kEp)) == -2);
    CHECK(ps_read32(b, kIt + 4) == 0);
    CHECK(ps_read32(b, kEp + 12) == 0x1000);

    // By time: the last entry point at or before it, else the first.
    ps_write32(b, kTs, 0);
    ps_write32(b, kTs + 4, 0x6000);
    REQUIRE(a.call("cellPamfReaderGetEpIteratorWithTimeStamp", kReader, kTs, kIt) == 0);
    CHECK(ps_read32(b, kIt + 4) == 1);
    ps_write32(b, kTs + 4, 0x500);
    REQUIRE(a.call("cellPamfReaderGetEpIteratorWithTimeStamp", kReader, kTs, kIt) == 0);
    CHECK(ps_read32(b, kIt + 4) == 0);

    // With the next entry point's offset, in bytes.
    REQUIRE(a.call("cellPamfReaderGetEpWithIndex", kReader, 1, kEp, 0x20) == 0);
    CHECK(ps_read64(b, kEp + 0x18) == 8 * 0x800);
    REQUIRE(a.call("cellPamfReaderGetEpWithIndex", kReader, 2, kEp, 0x20) == 0);
    CHECK(ps_read64(b, kEp + 0x18) == kDataSize); // the last: the data's size
    ps_write32(b, kTs + 4, 0x5000);
    ps_write64(b, kEp + 0x18, 0);
    REQUIRE(a.call("cellPamfReaderGetEpWithTimeStamp", kReader, kTs, kEp, 0x20) == 0);
    CHECK(ps_read64(b, kEp + 0x18) == 8 * 0x800); // in bytes, as Sony's (RPCS3: 8)

    // The audio stream's one entry point; a stream without any.
    REQUIRE(a.call("cellPamfReaderSetStreamWithIndex", kReader, 1) == 0);
    CHECK(a.call("cellPamfReaderGetNumberOfEp", kReader) == 1);
    ps_write32(b, kHdr + 0xB8 + 8, 0);
    ps_write32(b, kHdr + 0xB8 + 12, 0);
    CHECK(code(a.call("cellPamfReaderGetEpIteratorWithTimeStamp", kReader, kTs, kIt)) == EP_NOT_FOUND);

    // A reader over the minimum header has no entry points to give.
    write_pamf(b);
    REQUIRE(a.call("cellPamfReaderInitialize", kReader, kHdr, 0, 3) == 0);
    CHECK(code(a.call("cellPamfReaderGetEpIteratorWithIndex", kReader, 0, kIt)) == NOT_AVAILABLE);
    CHECK(code(a.call("cellPamfReaderGetEpWithIndex", kReader, 0, kEp, 0x20)) == NOT_AVAILABLE);
}

namespace {

// One call's answer and what the scratch areas held after it.
struct Answer {
    std::string call;
    u32 r3 = 0;
    std::vector<u8> memory;
};

// Calls a cellPamf function by name and NID: pspocket's (the registry) or Sony's (the firmware module).
using PamfCall = std::function<u32(const char* name, u32 nid, std::initializer_list<u64> args)>;

// Every scenario of the tests above and more, in one sequence; the header written afresh before each group.
std::vector<Answer> pamf_sequence(u8* b, const PamfCall& call)
{
    std::vector<Answer> out;
    const auto run = [&](const char* name, u32 nid, std::initializer_list<u64> args) {
        Answer a;
        a.r3 = call(name, nid, args);
        a.call = name;
        for (const u64 v : args) a.call += std::format(" {:#x}", v);
        for (const auto& [at, size] : {std::pair{kReader, 0x80u}, {kOut, 0x40u}, {kIt, 0x10u}, {kEp, 0x20u}, {kTs, 8u}})
            a.memory.insert(a.memory.end(), b + at, b + at + size);
        out.push_back(std::move(a));
    };
    const auto clear = [&] {
        write_pamf(b);
        for (const u32 at : {kReader, kOut, kIt, kEp, kTs}) std::memset(b + at, 0, 0x100);
    };
    // The header's checks: valid, then each defect.
    clear();
    run("cellPamfVerify", 0xD1A40EF4, {kHdr, kHeaderSize + kDataSize});
    run("cellPamfVerify", 0xD1A40EF4, {kHdr, 0});
    run("cellPamfVerify", 0xD1A40EF4, {kHdr, kHeaderSize});
    run("cellPamfGetHeaderSize", 0xCA8181C1, {kHdr, 0, kOut});
    run("cellPamfGetHeaderSize2", 0x90FC9A59, {kHdr, 0, 2, kOut + 8});
    run("cellPamfGetHeaderSize2", 0x90FC9A59, {kHdr, 0, 0, kOut + 16});
    run("cellPamfGetStreamOffsetAndSize", 0x44F5C9E3, {kHdr, kHeaderSize + kDataSize, kOut + 24, kOut + 32});
    const std::vector<std::function<void()>> defects = {
        [&] { ps_write32(b, kHdr, 0x50534D46); },
        [&] { std::memcpy(b + kHdr + 4, "0039", 4); },
        [&] { std::memcpy(b + kHdr + 4, "0040", 4); },
        [&] { ps_write32(b, kHdr + 0x08, 0); },
        [&] { ps_write8(b, kHdr + 0xB8, 0x1b), ps_write8(b, kHdr + 0xB8 + 4, 0xe0); },
        [&] { ps_write8(b, kHdr + 0xB8, 0x42); },
        [&] { ps_write32(b, kHdr + 0xE8 + 12 + 8, 9); },
        [&] { ps_write32(b, kHdr + 0xB8 + 8, 0x110); },
        [&] { ps_write32(b, kHdr + 0x50 + 0x0E, 0x1000); },
        [&] { ps_write32(b, kHdr + 0x50 + 0x1A, 3); },
        [&] { ps_write8(b, kHdr + 0x50 + 0x1F, 2); },
        [&] { ps_write8(b, kHdr + 0x82 + 5, 0); },
        [&] { ps_write32(b, kHdr + 0x10, 0x200); },
        [&] { ps_write32(b, kHdr + 0x10, 0x200), ps_write32(b, kHdr + 0x14, 0x10); },
        [&] { ps_write32(b, kHdr + 0x70, 0x80); },
        [&] { ps_write32(b, kHdr + 0xE8 + 4, 0xFFFFFFFF), ps_write16(b, kHdr + 0xE8 + 2, 1); },
    };
    for (const auto& defect : defects) {
        clear();
        defect();
        run("cellPamfVerify", 0xD1A40EF4, {kHdr, 0});
        run("cellPamfGetHeaderSize2", 0x90FC9A59, {kHdr, 0, 2, kOut});
        run("cellPamfReaderInitialize", 0xB8436EE5, {kReader, kHdr, 0, 3});
    }
    // The reader and its streams.
    clear();
    run("cellPamfReaderInitialize", 0xB8436EE5, {kReader, kHdr, kHeaderSize + kDataSize, 1});
    run("cellPamfReaderGetStreamIndex", 0x041CC708, {kReader});
    run("cellPamfReaderGetNumberOfStreams", 0x37F723F7, {kReader});
    for (const u64 type : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 20, 21, 22, 23})
        run("cellPamfReaderGetNumberOfSpecificStreams", 0xD0230671, {kReader, type});
    run("cellPamfReaderGetMuxRateBound", 0xDB70296C, {kReader});
    run("cellPamfReaderGetPresentationStartTime", 0x4DE501B1, {kReader, kTs});
    run("cellPamfReaderGetPresentationEndTime", 0xF61609D6, {kReader, kTs});
    for (const u64 index : {0, 1, 2}) {
        run("cellPamfReaderSetStreamWithIndex", 0x461534B4, {kReader, index});
        for (const u64 size : {0x1F, 0x20, 8, 7})
            run("cellPamfReaderGetStreamInfo", 0x67FD273B, {kReader, kOut, size});
        run("cellPamfReaderGetEsFilterId", 0x71DF326A, {kReader, kOut + 0x20});
        run("cellPamfReaderGetStreamTypeAndChannel", 0x9AB20793, {kReader, kOut + 0x30, kOut + 0x31});
        run("cellPamfReaderGetNumberOfEp", 0xD9EA3457, {kReader});
    }
    for (const u64 type : {0, 1, 2, 3, 20, 21, 22})
        for (const u64 index : {0, 1, 2})
            run("cellPamfReaderSetStreamWithTypeAndIndex", 0x28B4E2C1, {kReader, type, index});
    for (const u64 type : {0, 2, 3})
        for (const u64 ch : {0, 1})
            run("cellPamfReaderSetStreamWithTypeAndChannel", 0x03FD2CAA, {kReader, type, ch});
    // Entry points.
    clear();
    run("cellPamfReaderInitialize", 0xB8436EE5, {kReader, kHdr, 0, 1});
    for (const u64 index : {0, 1, 2, 3}) {
        run("cellPamfReaderGetEpIteratorWithIndex", 0xE8586EC6, {kReader, index, kIt});
        run("cellPamfEpIteratorGetEp", 0x1ABEB9D6, {kIt, kEp});
        for (const u64 unk : {0x18, 0x20})
            run("cellPamfReaderGetEpWithIndex", 0xFBE551D3, {kReader, index, kEp, unk});
    }
    run("cellPamfReaderGetEpIteratorWithIndex", 0xE8586EC6, {kReader, 1, kIt});
    for (const s64 steps : {1, 5, -1, -10, 0, 2})
        run("cellPamfEpIteratorMove", 0x50B83205, {kIt, static_cast<u64>(steps), kEp});
    for (const u32 ts : {0x0u, 0x500u, 0x1000u, 0x4FFFu, 0x5000u, 0x6000u, 0x9000u, 0xFFFFFFFFu}) {
        ps_write32(b, kTs, 0);
        ps_write32(b, kTs + 4, ts);
        run("cellPamfReaderGetEpIteratorWithTimeStamp", 0x439FBA17, {kReader, kTs, kIt});
        run("cellPamfReaderGetEpWithTimeStamp", 0x7AF9E578, {kReader, kTs, kEp, 0x20});
    }
    run("cellPamfReaderSetStreamWithIndex", 0x461534B4, {kReader, 1});
    run("cellPamfReaderGetEpIteratorWithTimeStamp", 0x439FBA17, {kReader, kTs, kIt});
    clear();
    run("cellPamfReaderInitialize", 0xB8436EE5, {kReader, kHdr, 0, 3});
    run("cellPamfReaderGetEpIteratorWithIndex", 0xE8586EC6, {kReader, 0, kIt});
    run("cellPamfReaderGetEpWithIndex", 0xFBE551D3, {kReader, 0, kEp, 0x20});
    // Stream types and channels as elementary stream filters.
    for (const u64 type : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20})
        for (const u64 ch : {0, 1, 15})
            run("cellPamfStreamTypeToEsFilterId", 0x01067E22, {type, ch, kOut});
    return out;
}

} // namespace

TEST_CASE("cellPamf: the same answers as Sony's libpamf (the user's firmware)", "[hle][pamf][firmware]")
{
    const auto flash = test_dev_flash();
    if (!flash) {
        SUCCEED("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
        return;
    }
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    FirmwareLibrary sony(f, t, *flash, "cellPamf");
    const std::vector<Answer> theirs = pamf_sequence(b, [&](const char*, u32 nid, std::initializer_list<u64> args) {
        return sony.call(t, nid, args);
    });
    const std::vector<Answer> ours = pamf_sequence(b, [&](const char* name, u32, std::initializer_list<u64> args) {
        const hle::Function& fn = GuestThread::find(name);
        u32 r = 3;
        for (const u64 v : args) t.ctx.gpr[r++] = v;
        fn.fn(t.ctx, b);
        return static_cast<u32>(t.ctx.gpr[3]);
    });
    REQUIRE(ours.size() == theirs.size());
    usize differ = 0;
    for (usize i = 0; i < ours.size(); i++) {
        if (ours[i].r3 == theirs[i].r3 && ours[i].memory == theirs[i].memory) continue;
        usize at = 0;
        while (at < ours[i].memory.size() && ours[i].memory[at] == theirs[i].memory[at]) at++;
        if (++differ <= 20)
            UNSCOPED_INFO(std::format("{}: {:#x} (pspocket) vs {:#x} (Sony){}", ours[i].call, ours[i].r3, theirs[i].r3,
                at < ours[i].memory.size() ? std::format("; memory differs first at byte {} ({:#04x} vs {:#04x})", at,
                                                 ours[i].memory[at], theirs[i].memory[at])
                                           : std::string()));
    }
    INFO(std::format("{} of {} calls differ", differ, ours.size()));
    CHECK(differ == 0);
}
