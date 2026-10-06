// NPD files (D-165): synthetic SDATA files, encrypted in the test with made-up keys the way Sony's are, read back by
// recomp::NpdFile for every layout the reader knows (flags 0x02, 0x08, 0x10, 0x20, compressed, debug data; NPD
// versions 1 to 4). No real keys and no game data (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <numeric>

#include "pspocket/recomp/npd.hpp"
#include "synthetic.hpp"

#if defined(__APPLE__)

using namespace pspocket;
using namespace pspocket::recomp;

namespace {

NpdKeys made_up_keys()
{
    NpdKeys k;
    for (int i = 0; i < 16; i++) {
        k.sdat[i] = static_cast<u8>(0x10 + 7 * i);
        k.edat_key0[i] = static_cast<u8>(0x20 + 11 * i);
        k.edat_key1[i] = static_cast<u8>(0x30 + 13 * i);
        k.edat_hash0[i] = static_cast<u8>(0x40 + 17 * i);
        k.edat_hash1[i] = static_cast<u8>(0x50 + 19 * i);
        k.edat_iv[i] = static_cast<u8>(0x60 + 23 * i);
    }
    k.present = true;
    return k;
}

std::vector<u8> pattern(usize size)
{
    std::vector<u8> v(size);
    u32 x = 0x12345678;
    for (u8& b : v) {
        x = x * 1103515245u + 12345u;
        b = static_cast<u8>(x >> 16);
    }
    return v;
}

Result<std::unique_ptr<NpdFile>> open_bytes(const std::vector<u8>& file, const NpdKeys& keys)
{
    return NpdFile::open(
        [&file](u64 offset, void* dst, usize size) {
            if (offset > file.size() || size > file.size() - offset) return false;
            std::memcpy(dst, file.data() + offset, size);
            return true;
        },
        file.size(), keys);
}

// Every byte through one read, then pieces that start and end inside blocks and cross their edges.
void check_reads(const NpdFile& f, const std::vector<u8>& plain, u32 block_size)
{
    REQUIRE(f.size() == plain.size());
    std::vector<u8> all(plain.size());
    std::string error;
    REQUIRE(f.read(0, all.data(), all.size(), &error));
    CHECK(error.empty());
    CHECK(all == plain);
    const usize pieces[][2] = {{0, 1}, {1, block_size - 2}, {block_size - 3, 7}, {block_size, block_size},
        {2 * block_size - 1, 2}, {plain.size() - 1, 1}, {plain.size() - 5, 5}, {block_size / 2, 2 * block_size + 9}};
    for (const auto& [at, size] : pieces) {
        if (at + size > plain.size()) continue;
        std::vector<u8> got(size);
        REQUIRE(f.read(at, got.data(), size));
        CHECK(std::equal(got.begin(), got.end(), plain.begin() + static_cast<std::ptrdiff_t>(at)));
    }
    u8 past = 0;
    CHECK_FALSE(f.read(plain.size(), &past, 1));
}

} // namespace

TEST_CASE("AES-CMAC: RFC 4493's examples", "[npd]")
{
    const u8 key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    const u8 m[64] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
        0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8,
        0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
        0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
    const struct {
        usize size;
        u8 tag[16];
    } examples[] = {
        {0, {0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28, 0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46}},
        {16, {0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44, 0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c}},
        {40, {0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30, 0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27}},
        {64, {0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92, 0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe}},
    };
    for (const auto& e : examples) {
        u8 tag[16]{};
        REQUIRE(npd_aes_cmac(key, m, e.size, tag));
        CHECK(std::memcmp(tag, e.tag, 16) == 0);
    }
}

TEST_CASE("NpdFile: SDATA files in every layout come back as their plain bytes", "[npd]")
{
    const NpdKeys keys = made_up_keys();
    const struct {
        const char* what;
        test::SdataOptions options;
        usize size;
    } cases[] = {
        {"AES-CMAC, version 3", {.version = 3, .extra_flags = 0, .block_size = 0x400}, 5000},
        {"a whole number of blocks", {.version = 2, .extra_flags = 0, .block_size = 0x200}, 0x1000},
        {"HMAC-SHA1 (flag 0x10)", {.version = 3, .extra_flags = kNpdFlag10, .block_size = 0x400}, 3333},
        {"hash before each block (0x10 | 0x20)", {.version = 3, .extra_flags = kNpdFlag10 | kNpdFlag20,
             .block_size = 0x400}, 4097},
        {"CMAC before each block (0x20)", {.version = 2, .extra_flags = kNpdFlag20, .block_size = 0x300}, 2000},
        {"encrypted key, version 2 (EDAT_KEY_0)", {.version = 2, .extra_flags = kNpdEncryptedKeyFlag,
             .block_size = 0x400}, 2500},
        {"encrypted key, version 4 (EDAT_KEY_1)", {.version = 4, .extra_flags = kNpdEncryptedKeyFlag | kNpdFlag10,
             .block_size = 0x400}, 2500},
        {"not encrypted, still hashed (0x02)", {.version = 3, .extra_flags = kNpdFlag02, .block_size = 0x400}, 1800},
        {"debug data", {.version = 3, .extra_flags = kNpdDebugDataFlag, .block_size = 0x400}, 1800},
        {"version 1: no IV, no dev hash in the block key", {.version = 1, .extra_flags = 0, .block_size = 0x400}, 3000},
        {"compressed, metadata XORed (version 3)", {.version = 3, .extra_flags = kNpdCompressedFlag,
             .block_size = 0x400}, 5000},
        {"compressed, metadata plain (version 1)", {.version = 1, .extra_flags = kNpdCompressedFlag,
             .block_size = 0x400}, 5000},
        {"compressed with HMAC-SHA1", {.version = 3, .extra_flags = kNpdCompressedFlag | kNpdFlag10,
             .block_size = 0x400}, 2600},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        const std::vector<u8> plain = pattern(c.size);
        const std::vector<u8> file = test::build_sdata(plain, keys, c.options);
        REQUIRE(file.size() > 0x100);
        auto f = open_bytes(file, keys);
        if (!f) FAIL(f.error().message);
        CHECK((*f)->header().flags == (kNpdSdatFlag | c.options.extra_flags));
        CHECK((*f)->header().block_size == c.options.block_size);
        CHECK((*f)->header().content_id == test::kSelfContentId);
        check_reads(**f, plain, c.options.block_size);
    }
}

TEST_CASE("NpdFile: a damaged block, wrong keys, EDATA and other files are refused", "[npd]")
{
    const NpdKeys keys = made_up_keys();
    const std::vector<u8> plain = pattern(3000);
    const std::vector<u8> file = test::build_sdata(plain, keys, {.version = 3, .extra_flags = 0, .block_size = 0x400});

    SECTION("a byte changed in the second block: its reads fail, the first block's still work")
    {
        std::vector<u8> bad = file;
        const usize second_block = 0x100 + 3 * 0x10 + 0x400; // three blocks' hashes, then the data
        bad[second_block + 5] ^= 0x01;
        auto f = open_bytes(bad, keys);
        REQUIRE(f);
        std::vector<u8> got(0x400);
        std::string error;
        CHECK((*f)->read(0, got.data(), got.size()));
        CHECK_FALSE((*f)->read(0x400, got.data(), 16, &error));
        CHECK(error.find("hash does not match") != std::string::npos);
        CHECK_FALSE((*f)->read(0x3F0, got.data(), 0x20)); // across into the damaged block
    }
    SECTION("the first block damaged, or the wrong SDAT key: the open fails")
    {
        std::vector<u8> bad = file;
        bad[0x100 + 3 * 0x10] ^= 0x80;
        auto f = open_bytes(bad, keys);
        REQUIRE_FALSE(f);
        CHECK(f.error().message.find("the first block") != std::string::npos);
        NpdKeys other = keys;
        other.sdat[3] ^= 1;
        CHECK_FALSE(open_bytes(file, other));
    }
    SECTION("an EDATA file (no SDAT flag) needs a licence key: refused")
    {
        std::vector<u8> edata = file;
        edata[0x83] = 0; // flags 0x01000000 -> 0
        edata[0x80] = 0;
        auto f = open_bytes(edata, keys);
        REQUIRE_FALSE(f);
        CHECK(f.error().message.find("EDATA") != std::string::npos);
    }
    SECTION("no keys in the build, not an NPD file, too short")
    {
        CHECK_FALSE(open_bytes(file, NpdKeys{}));
        std::vector<u8> other = file;
        other[0] = 'X';
        auto f = open_bytes(other, keys);
        REQUIRE_FALSE(f);
        CHECK(f.error().message == "not an NPD file");
        CHECK_FALSE(open_bytes(std::vector<u8>(file.begin(), file.begin() + 0x80), keys));
    }
}

TEST_CASE("npd_decompress: the plain form, and damaged input never writes past its buffers", "[npd]")
{
    u8 in[5 + 10] = {0x81, 0, 0, 0, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    u8 out[16]{};
    CHECK(npd_decompress(out, in, sizeof(in), sizeof(out)) == 10);
    CHECK(out[0] == 1);
    CHECK(out[9] == 10);
    CHECK(npd_decompress(out, in, sizeof(in), 8) == -1);  // longer than the block
    CHECK(npd_decompress(out, in, 9, sizeof(out)) == -1); // longer than the input
    CHECK(npd_decompress(out, in, 3, sizeof(out)) == -1);

    // Noise through the range decoder: any count, or -1, but nothing beyond `out`'s 256 bytes (a guard follows).
    u32 x = 99;
    for (int round = 0; round < 200; round++) {
        std::vector<u8> noise(64 + round);
        for (u8& b : noise) {
            x = x * 1664525u + 1013904223u;
            b = static_cast<u8>(x >> 24);
        }
        noise[0] = static_cast<u8>(noise[0] & 0x07); // a compressed stream's header
        std::vector<u8> buffer(256 + 64, 0xEE);
        const int n = npd_decompress(buffer.data(), noise.data(), noise.size(), 256);
        CHECK(n <= 256);
        CHECK(std::all_of(buffer.begin() + 256, buffer.end(), [](u8 b) { return b == 0xEE; }));
    }
}

#endif
