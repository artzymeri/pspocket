#include <catch_amalgamated.hpp>

#include <string>
#include <vector>

#include "pspocket/recomp/bytes.hpp"
#include "pspocket/recomp/hash.hpp"

using namespace pspocket;
using namespace pspocket::recomp;

namespace {

// FIPS 180-4 example messages (NIST CSRC "Examples with intermediate values").
const std::string kOneBlock = "abc";
const std::string kTwoBlock = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; // 448 bits
const std::string kFourBlock = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"; // 896 bits

std::string sha1_hex(std::string_view s) { return to_hex(Sha1::hash(s)); }
std::string sha256_hex(std::string_view s) { return to_hex(Sha256::hash(s)); }

std::vector<u8> pattern(usize n)
{
    std::vector<u8> v(n);
    for (usize i = 0; i < n; i++) v[i] = static_cast<u8>((i * 131 + 7) ^ (i >> 5));
    return v;
}

} // namespace

TEST_CASE("SHA-1 matches the FIPS 180 vectors")
{
    CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1_hex(kOneBlock) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1_hex(kTwoBlock) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(sha1_hex(kFourBlock) == "a49b2446a02c645bf419f995b67091253a04a259");
}

TEST_CASE("SHA-256 matches the FIPS 180 vectors")
{
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(kOneBlock) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(kTwoBlock) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256_hex(kFourBlock) == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("one million 'a' (FIPS 180 long-message vector), fed incrementally")
{
    Sha1 sha1;
    Sha256 sha256;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; i++) {
        sha1.update(chunk);
        sha256.update(chunk);
    }
    CHECK(to_hex(sha1.finish()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    CHECK(to_hex(sha256.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const std::string million(1'000'000, 'a');
    CHECK(sha1_hex(million) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("incremental hashing equals one-shot hashing for every split")
{
    // Lengths around the 55/56/64-byte padding boundaries, split at every position.
    for (const usize n : {usize{0}, usize{1}, usize{55}, usize{56}, usize{57}, usize{63}, usize{64}, usize{65},
             usize{119}, usize{120}, usize{128}, usize{200}}) {
        const auto data = pattern(n);
        const auto want1 = Sha1::hash(data);
        const auto want256 = Sha256::hash(data);
        for (usize split = 0; split <= n; split++) {
            Sha1 a;
            Sha256 b;
            a.update(std::span<const u8>(data).first(split));
            a.update(std::span<const u8>(data).subspan(split));
            b.update(std::span<const u8>(data).first(split));
            b.update(std::span<const u8>(data).subspan(split));
            REQUIRE(a.finish() == want1);
            REQUIRE(b.finish() == want256);
        }
    }
}

TEST_CASE("byte-at-a-time updates and reset() reuse")
{
    const auto data = pattern(1000);
    Sha256 h;
    for (const u8 byte : data) h.update(std::span<const u8>(&byte, 1));
    CHECK(h.finish() == Sha256::hash(data));

    h.reset();
    h.update(kOneBlock);
    CHECK(to_hex(h.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    Sha1 s;
    s.update("a");
    s.reset();
    s.update(kOneBlock);
    CHECK(to_hex(s.finish()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE("an empty update changes nothing, its data null and bytes waiting in the block (D-605)")
{
    // A span of no bytes may have a null data(), which memcpy must not be given, even for no bytes (UBSan's
    // nonnull-attribute): the sweep found it from the inventory's hashing of an empty segment.
    Sha1 s;
    s.update("ab");
    s.update(std::span<const u8>());
    s.update("c");
    CHECK(to_hex(s.finish()) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    Sha256 h;
    h.update("abc");
    h.update(std::span<const u8>());
    CHECK(to_hex(h.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("hex helpers")
{
    const std::vector<u8> bytes{0x00, 0x7F, 0xAB, 0xFF};
    CHECK(to_hex(bytes) == "007fabff");
    std::vector<u8> parsed;
    REQUIRE(parse_hex_bytes("007FabfF", parsed));
    CHECK(parsed == bytes);
    CHECK_FALSE(parse_hex_bytes("abc", parsed));
    CHECK_FALSE(parse_hex_bytes("zz", parsed));
    CHECK(hex32(0x1234) == "0x00001234");
    CHECK(hex_compact(0) == "0x0");
    CHECK(hex_compact(0xFFA4) == "0xffa4");
}

TEST_CASE("ByteReader never reads out of bounds")
{
    const std::vector<u8> bytes{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 'a', 'b', 0, 'c'};
    const ByteReader r(bytes);
    CHECK(r.be16(0) == 0x1234u);
    CHECK(r.be32(4) == 0x9ABCDEF0u);
    CHECK(r.be64(0) == 0x123456789ABCDEF0ull);
    CHECK(r.le16(0) == 0x3412u);
    CHECK(r.le64(0) == 0xF0DEBC9A78563412ull);
    CHECK(r.u8_at(11) == u8{'c'});
    CHECK_FALSE(r.u8_at(12));
    CHECK_FALSE(r.be16(11));
    CHECK_FALSE(r.be32(9));
    CHECK_FALSE(r.be64(5));
    CHECK_FALSE(r.be32(~u64{0} - 1)); // offset + length overflows
    CHECK_FALSE(r.bytes(4, ~u64{0}));
    CHECK(r.bytes(12, 0).has_value()); // empty range at the end is fine
    CHECK(r.cstring(8, 16) == "ab");
    CHECK_FALSE(r.cstring(8, 1));      // no NUL within max_length
    CHECK_FALSE(r.cstring(11, 16));    // runs off the end
    CHECK(r.cstring_lenient(11, 16) == "c");
    CHECK(r.cstring_lenient(99, 16).empty());

    CHECK(range_ok(10, 10, 0));
    CHECK_FALSE(range_ok(10, 11, 0));
    CHECK_FALSE(range_ok(10, 2, ~u64{0}));
    CHECK(checked_add(1, 2) == 3u);
    CHECK_FALSE(checked_add(~u64{0}, 1));
    CHECK(checked_mul(0, ~u64{0}) == 0u);
    CHECK_FALSE(checked_mul(u64{1} << 33, u64{1} << 31));
}
