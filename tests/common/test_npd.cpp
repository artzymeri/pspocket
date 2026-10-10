// NPD files (D-165, D-581): synthetic SDATA and EDATA files, encrypted in the test with made-up keys the way Sony's
// are, read back by recomp::NpdFile for every layout the reader knows (flags 0x02, 0x08, 0x10, 0x20, compressed, debug
// data; NPD versions 1 to 4), and an EDATA file's header hashes. Where the build has RPCS3's source, every block and
// every header check is also set against RPCS3's own code (rpcs3_ref), with the build's keys, in memory only. No real
// keys and no game data in the tests (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <cstring>
#include <numeric>
#include <optional>
#include <random>
#include <string>

#include "pspocket/core/be.h"
#include "pspocket/recomp/npd.hpp"
#include "synthetic.hpp"

#if defined(PSPOCKET_HAVE_RPCS3_REF)
#include "rpcs3_ref/rpcs3_ref.hpp"
#endif

using namespace pspocket;
using namespace pspocket::recomp;
using pspocket::test::build_npd;
using pspocket::test::NpdOptions;

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

Result<std::unique_ptr<NpdFile>> open_bytes(const std::vector<u8>& file, const NpdKeys& keys,
    const std::optional<Key128>& edata_key = std::nullopt, const NpdrmKeys& npdrm = builtin_npdrm_keys())
{
    return NpdFile::open(
        [&file](u64 offset, void* dst, usize size) {
            if (offset > file.size() || size > file.size() - offset) return false;
            std::memcpy(dst, file.data() + offset, size);
            return true;
        },
        file.size(), keys, edata_key, npdrm);
}

NpdrmKeys made_up_npdrm_keys()
{
    NpdrmKeys k;
    for (int i = 0; i < 16; i++) {
        k.omac_key2[i] = static_cast<u8>(0x71 + 3 * i);
        k.omac_key3[i] = static_cast<u8>(0x82 + 5 * i);
        k.klic_free[i] = static_cast<u8>(0x93 + 9 * i);
    }
    k.present = true;
    return k;
}

Key128 key_of(u8 seed)
{
    Key128 k{};
    for (int i = 0; i < 16; i++) k[i] = static_cast<u8>(seed + 29 * i);
    return k;
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

TEST_CASE("NpdFile: a compressed file's block stored whole but longer than a block is refused (D-605)", "[npd]")
{
    // A compressed file keeps each block's length in its metadata (plain in NPD version 1), and a block whose
    // compression end is 0 is copied out as it is. A length past the block (up to twice it, which the check before
    // allows) was copied whole into the block's buffer: RPCS3 stops there (its ensure), pspocket refuses the block.
    const NpdKeys keys = made_up_keys();
    const std::vector<u8> plain = pattern(5000);
    const std::vector<u8> file = test::build_sdata(plain, keys,
        {.version = 1, .extra_flags = kNpdCompressedFlag | kNpdDebugDataFlag, .block_size = 0x400});
    REQUIRE(open_bytes(file, keys));
    std::vector<u8> bad = file;
    const usize metadata = 0x100; // block 0's: hash, offset, length, compression end
    ps_st32(bad.data() + metadata + 0x18, 2 * 0x400 + 0x40);
    ps_st32(bad.data() + metadata + 0x1C, 0);
    auto f = open_bytes(bad, keys);
    REQUIRE_FALSE(f);
    CHECK(f.error().message.find("more than a block") != std::string::npos);
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

TEST_CASE("NpdFile: EDATA files open with their content key, in every layout (D-581)", "[npd]")
{
    const NpdKeys keys = made_up_keys();
    const NpdrmKeys npdrm = made_up_npdrm_keys();
    const struct {
        const char* what;
        s32 license;
        u32 flags;
        s32 version;
    } cases[] = {
        {"free (klicensee), AES-CMAC", 3, 0, 3},
        {"free, HMAC-SHA1 before each block", 3, kNpdFlag10 | kNpdFlag20, 3},
        {"free, encrypted key, version 4", 3, kNpdEncryptedKeyFlag | kNpdFlag10, 4},
        {"free, compressed", 3, kNpdCompressedFlag, 2},
        {"local (RIF key), AES-CMAC", 2, 0, 3},
        {"network (RIF key), not encrypted (0x02)", 1, kNpdFlag02, 3},
        {"local, version 1", 2, 0, 1},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        const Key128 key = key_of(static_cast<u8>(c.license * 40 + c.version));
        NpdOptions o{.version = c.version, .extra_flags = c.flags, .block_size = 0x400};
        o.edata = true;
        o.license = c.license;
        o.key = key;
        o.file_name = "DATA.EDAT";
        o.npdrm = &npdrm;
        const std::vector<u8> plain = pattern(4321);
        const std::vector<u8> file = build_npd(plain, keys, o);
        auto f = open_bytes(file, keys, key, npdrm);
        if (!f) FAIL(f.error().message);
        CHECK((*f)->header().flags == c.flags);
        CHECK((*f)->header().license == c.license);
        check_reads(**f, plain, 0x400);

        // Another key: a free file says so from its dev hash, any other from its first block.
        auto wrong = open_bytes(file, keys, key_of(0xEE), npdrm);
        REQUIRE_FALSE(wrong);
        if (c.license == 3) CHECK(wrong.error().message.find("klicensee is not the one") != std::string::npos);
        else CHECK(wrong.error().message.find("the first block") != std::string::npos);
        // No key at all; no keys in the build.
        auto none = open_bytes(file, keys, std::nullopt, npdrm);
        REQUIRE_FALSE(none);
        CHECK(none.error().message.find("licence key") != std::string::npos);
        CHECK_FALSE(open_bytes(file, NpdKeys{}, key, npdrm));
        if (c.license == 3) CHECK_FALSE(open_bytes(file, keys, key, NpdrmKeys{}));
    }

    // Debug data: no dev hash check, blocks stored plain.
    NpdOptions o{.version = 3, .extra_flags = kNpdDebugDataFlag, .block_size = 0x200};
    o.edata = true;
    o.key = key_of(5);
    const std::vector<u8> plain = pattern(1000);
    const std::vector<u8> debug = build_npd(plain, keys, o);
    auto f = open_bytes(debug, keys, key_of(6), npdrm);
    REQUIRE(f);
    check_reads(**f, plain, 0x200);
}

TEST_CASE("NPD headers: the dev hash and the title hash RPCS3 checks before a game uses an EDAT", "[npd]")
{
    const NpdKeys keys = made_up_keys();
    const NpdrmKeys npdrm = made_up_npdrm_keys();
    NpdOptions o{.version = 3, .extra_flags = 0, .block_size = 0x400};
    o.edata = true;
    o.license = 3;
    o.key = key_of(1);
    o.file_name = "Level1.EDAT";
    o.npdrm = &npdrm;
    const std::vector<u8> file = build_npd(pattern(100), keys, o);
    const std::span<const u8> head(file.data(), kNpdHeaderBytes);

    const auto header = parse_npd_header(head);
    REQUIRE(header);
    CHECK(header->content_id == test::kSelfContentId);
    CHECK(header->license == 3);
    CHECK_FALSE(parse_npd_header(head.first(0x8F)));

    CHECK(npd_klicensee_matches(head, key_of(1), npdrm));
    CHECK_FALSE(npd_klicensee_matches(head, key_of(2), npdrm));
    CHECK(npd_hashes_valid(head, "Level1.EDAT", key_of(1), npdrm));
    CHECK(npd_hashes_valid(head, "", key_of(1), npdrm)); // no name: the dev hash only
    // The name's extension is tried as it is, in lower and in upper case: a file made as .EDAT passes as .edat or
    // .Edat; the rest of the name must be the one it was made with.
    CHECK(npd_hashes_valid(head, "Level1.edat", key_of(1), npdrm));
    CHECK(npd_hashes_valid(head, "Level1.Edat", key_of(1), npdrm));
    CHECK_FALSE(npd_hashes_valid(head, "LEVEL1.EDAT", key_of(1), npdrm));
    CHECK_FALSE(npd_hashes_valid(head, "Level2.EDAT", key_of(1), npdrm));
    CHECK_FALSE(npd_hashes_valid(head, "Level1.EDAT", key_of(2), npdrm));

    // A file with no extension: the whole name in one case or the other.
    o.file_name = "readme";
    const std::vector<u8> plain_name = build_npd(pattern(100), keys, o);
    CHECK(npd_hashes_valid(std::span<const u8>(plain_name).first(kNpdHeaderBytes), "README", key_of(1), npdrm));

    // A local licence: no dev hash to check, the title hash still is.
    o.license = 2;
    o.file_name = "dlc.edat";
    const std::vector<u8> local = build_npd(pattern(100), keys, o);
    const std::span<const u8> local_head(local.data(), kNpdHeaderBytes);
    CHECK(npd_klicensee_matches(local_head, key_of(9), npdrm));
    CHECK(npd_hashes_valid(local_head, "dlc.edat", key_of(9), npdrm));
    CHECK_FALSE(npd_hashes_valid(local_head, "other.edat", key_of(9), npdrm));

    // Debug data passes whatever its hashes.
    std::vector<u8> debug = local;
    ps_st32(debug.data() + 0x80, kNpdDebugDataFlag);
    CHECK(npd_hashes_valid(std::span<const u8>(debug).first(kNpdHeaderBytes), "other.edat", key_of(9), npdrm));
}

#if defined(PSPOCKET_HAVE_RPCS3_REF)

TEST_CASE("NPD files against RPCS3: every block of SDATA and EDATA files as RPCS3's decrypt_block gives it",
    "[npd][rpcs3]")
{
    const NpdKeys& keys = builtin_npd_keys();
    const NpdrmKeys& npdrm = builtin_npdrm_keys();
    if (!keys.present || !npdrm.present) SKIP("this build has no NPD keys");
    const struct {
        const char* what;
        bool edata;
        s32 license;
        u32 flags;
        s32 version;
        u32 block_size;
    } cases[] = {
        {"SDATA, AES-CMAC", false, 3, 0, 3, 0x400},
        {"SDATA, HMAC-SHA1 (0x10)", false, 3, kNpdFlag10, 2, 0x200},
        {"SDATA, hashes before the blocks (0x10 | 0x20)", false, 3, kNpdFlag10 | kNpdFlag20, 3, 0x400},
        {"SDATA, encrypted key, version 4", false, 3, kNpdEncryptedKeyFlag, 4, 0x400},
        {"SDATA, version 1", false, 3, 0, 1, 0x300},
        {"SDATA, compressed", false, 3, kNpdCompressedFlag, 3, 0x400},
        {"EDATA, free", true, 3, 0, 3, 0x400},
        {"EDATA, free, encrypted key and HMAC (0x08 | 0x10)", true, 3, kNpdEncryptedKeyFlag | kNpdFlag10, 4, 0x400},
        {"EDATA, local, CMAC before each block (0x20)", true, 2, kNpdFlag20, 3, 0x400},
        {"EDATA, network, not encrypted (0x02)", true, 1, kNpdFlag02, 2, 0x200},
        {"EDATA, debug data", true, 2, kNpdDebugDataFlag, 3, 0x400},
        {"EDATA, compressed, metadata plain (version 1)", true, 3, kNpdCompressedFlag, 1, 0x400},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        NpdOptions o{.version = c.version, .extra_flags = c.flags, .block_size = c.block_size};
        o.edata = c.edata;
        o.license = c.license;
        o.key = key_of(static_cast<u8>(c.flags + static_cast<u32>(c.version)));
        o.file_name = "x.edat";
        o.npdrm = &npdrm;
        const std::vector<u8> plain = pattern(3 * c.block_size + 77);
        const std::vector<u8> file = build_npd(plain, keys, o);
        const std::optional<Key128> edata_key = c.edata ? std::optional<Key128>(o.key) : std::nullopt;
        auto ours = open_bytes(file, keys, edata_key, npdrm);
        if (!ours) FAIL(ours.error().message);
        // RPCS3's file key: an SDAT file's dev hash XOR SDAT_KEY, an EDATA file's content key.
        Key128 file_key = o.key;
        if (!c.edata)
            for (int i = 0; i < 16; i++) file_key[i] = static_cast<u8>(file[0x60 + i] ^ keys.sdat[i]);
        const u32 blocks = static_cast<u32>((plain.size() + c.block_size - 1) / c.block_size);
        for (u32 block = 0; block < blocks; block++) {
            INFO("block " << block);
            const auto theirs = test::rpcs3_ref::decrypt_block(file, block, file_key);
            REQUIRE(theirs);
            const usize at = usize{block} * c.block_size;
            std::vector<u8> mine(std::min<usize>(c.block_size, plain.size() - at));
            REQUIRE((*ours)->read(at, mine.data(), mine.size()));
            if ((c.flags & kNpdCompressedFlag) && block % 2 == 0) {
                // A compressed block that holds its bytes plain: RPCS3's decompress answers 0 for it (D-165).
                CHECK(theirs->empty());
            } else {
                CHECK((*theirs == mine)); // unexpanded: Catch2 throws printing byte vectors this long (-s)
            }
        }
        // A damaged last block: neither decrypts it.
        if (!(c.flags & kNpdDebugDataFlag)) {
            std::vector<u8> damaged = file;
            damaged[file.size() - 3] ^= 0x40;
            CHECK_FALSE(test::rpcs3_ref::decrypt_block(damaged, blocks - 1, file_key));
            auto f = open_bytes(damaged, keys, edata_key, npdrm);
            REQUIRE(f);
            std::vector<u8> last(plain.size() - usize{blocks - 1} * c.block_size);
            CHECK_FALSE((*f)->read(usize{blocks - 1} * c.block_size, last.data(), last.size()));
        }
    }
}

TEST_CASE("NPD header hashes against RPCS3: validate_dev_klic and validate_npd_hashes on random headers",
    "[npd][rpcs3]")
{
    const NpdKeys& keys = builtin_npd_keys();
    const NpdrmKeys& npdrm = builtin_npdrm_keys();
    if (!keys.present || !npdrm.present) SKIP("this build has no NPD keys");
    std::mt19937 rng(0xED47);
    const char* names[] = {"data.edat", "DATA.EDAT", "Data.Edat", "noext", "NOEXT", "a.b.c", "a.B.C", ".hidden",
        "trailing.", "x.EDAT"};
    const s32 licenses[] = {0, 1, 2, 3, 7};
    for (int round = 0; round < 200; round++) {
        NpdOptions o{.version = 1 + static_cast<s32>(rng() % 4), .extra_flags = 0, .block_size = 0x400};
        o.edata = true;
        o.license = licenses[rng() % std::size(licenses)];
        for (u8& b : o.key) b = static_cast<u8>(rng());
        o.file_name = names[rng() % std::size(names)];
        o.content_id = "UP" + std::to_string(rng() % 10000) + "-NPXX" + std::to_string(rng() % 100000) + "_00-" +
            std::to_string(rng());
        o.npdrm = &npdrm;
        if (round % 10 == 9) o.extra_flags = kNpdDebugDataFlag;
        std::vector<u8> file = build_npd(pattern(64), keys, o);
        if (round % 7 == 6) file[0x55] ^= 1;   // a damaged title hash
        if (round % 11 == 10) file[0x65] ^= 1; // a damaged dev hash
        const std::span<const u8> head(file.data(), kNpdHeaderBytes);
        Key128 other = o.key;
        other[rng() % 16] ^= 0x10;
        for (const Key128& klic : {o.key, other}) {
            CHECK(npd_klicensee_matches(head, klic, npdrm) == test::rpcs3_ref::validate_dev_klic(file, klic));
            for (const char* name : names) {
                INFO("made as " << o.file_name << ", checked as " << name << ", licence " << o.license);
                CHECK(npd_hashes_valid(head, name, klic, npdrm) ==
                    test::rpcs3_ref::validate_npd_hashes(file, name, klic));
            }
            CHECK(npd_hashes_valid(head, "", klic, npdrm) == test::rpcs3_ref::validate_npd_hashes(file, "", klic));
        }
    }
}

#endif
