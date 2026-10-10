// The crypto layer (D-580): AES against FIPS-197's and SP 800-38A's examples, HMAC-SHA1 against RFC 2202's, and the
// platform's AES (CommonCrypto on Apple platforms) against the portable one on random input. The hex strings are the
// standards' published examples (and one counter case computed with OpenSSL), not keys of anything.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "pspocket/recomp/crypto.hpp"

using namespace pspocket;
using namespace pspocket::recomp;

namespace {

std::vector<u8> hex(std::string_view text)
{
    std::vector<u8> out;
    for (usize i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<u8>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    return out;
}

// SP 800-38A's four plaintext blocks (F.2, F.5).
const std::vector<u8> kPlain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                                   "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");

} // namespace

TEST_CASE("AES: FIPS-197's examples for 128-, 192- and 256-bit keys, both ways", "[crypto]")
{
    INFO("AES through " << crypto::aes_library());
    const std::vector<u8> plain = hex("00112233445566778899aabbccddeeff");
    const struct {
        const char* key;
        const char* cipher;
    } cases[] = {
        {"000102030405060708090a0b0c0d0e0f", "69c4e0d86a7b0430d8cdb78070b4c55a"},
        {"000102030405060708090a0b0c0d0e0f1011121314151617", "dda97ca4864cdfe06eaf70a0ec0d7191"},
        {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "8ea2b7ca516745bfeafc49904b496089"},
    };
    for (const auto& c : cases) {
        const std::vector<u8> key = hex(c.key), want = hex(c.cipher);
        INFO(key.size() * 8 << "-bit key");
        std::vector<u8> out(16), back(16);
        REQUIRE(crypto::aes_ecb_encrypt(key, plain.data(), 16, out.data()));
        CHECK(out == want);
        REQUIRE(crypto::aes_ecb_decrypt(key, out.data(), 16, back.data()));
        CHECK(back == plain);
        REQUIRE(crypto::soft::aes_ecb_encrypt(key, plain.data(), 16, out.data()));
        CHECK(out == want);
        REQUIRE(crypto::soft::aes_ecb_decrypt(key, out.data(), 16, back.data()));
        CHECK(back == plain);
        const crypto::Aes aes(key);
        REQUIRE(aes.valid());
        aes.encrypt_block(plain.data(), out.data());
        CHECK(out == want);
    }
}

TEST_CASE("AES-CBC and AES-CTR: SP 800-38A's examples, in place too", "[crypto]")
{
    const std::vector<u8> iv = hex("000102030405060708090a0b0c0d0e0f");
    const struct {
        const char* key;
        const char* cipher;
    } cbc[] = {
        {"2b7e151628aed2a6abf7158809cf4f3c", "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                                             "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"},
        {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
            "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"
            "39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b"},
    };
    for (const auto& c : cbc) {
        const std::vector<u8> key = hex(c.key), want = hex(c.cipher);
        INFO(key.size() * 8 << "-bit CBC");
        std::vector<u8> out(kPlain.size());
        REQUIRE(crypto::aes_cbc_encrypt(key, iv.data(), kPlain.data(), kPlain.size(), out.data()));
        CHECK(out == want);
        std::vector<u8> in_place = want;
        REQUIRE(crypto::aes_cbc_decrypt(key, iv.data(), in_place.data(), in_place.size(), in_place.data()));
        CHECK(in_place == kPlain);
        REQUIRE(crypto::soft::aes_cbc_decrypt(key, iv.data(), want.data(), want.size(), out.data()));
        CHECK(out == kPlain);
    }

    // CTR (F.5.1): the counter's low byte carries into the next (...feff, ...ff00); a partial last block.
    const std::vector<u8> k = hex("2b7e151628aed2a6abf7158809cf4f3c");
    const std::vector<u8> want = hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
                                     "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
    const std::vector<u8> counter = hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    std::vector<u8> out(kPlain.size());
    REQUIRE(crypto::aes_ctr(k, counter.data(), kPlain.data(), kPlain.size(), out.data()));
    CHECK(out == want);
    std::vector<u8> part(kPlain.begin(), kPlain.begin() + 37);
    REQUIRE(crypto::aes_ctr(k, counter.data(), part.data(), part.size(), part.data()));
    CHECK(std::equal(part.begin(), part.end(), want.begin()));
    // A counter of all ones rolls over to zero across all sixteen bytes (computed with OpenSSL).
    const std::vector<u8> ones(16, 0xFF);
    const std::vector<u8> rolled_over = hex("e13338e36cb71962e00d020b4cedbd86d3dae15b04bb352fa0f59febfcb4da3e"
                                            "67da610697ed5aae4b0fa7a0dd783d29");
    out.assign(48, 0);
    REQUIRE(crypto::soft::aes_ctr(k, ones.data(), kPlain.data(), 48, out.data()));
    CHECK(out == rolled_over);
    REQUIRE(crypto::aes_ctr(k, ones.data(), kPlain.data(), 48, out.data()));
    CHECK(out == rolled_over);
}

TEST_CASE("HMAC-SHA1: RFC 2202's examples, a key longer than the block included", "[crypto]")
{
    const struct {
        std::vector<u8> key;
        std::string data;
        const char* mac;
    } cases[] = {
        {std::vector<u8>(20, 0x0b), "Hi There", "b617318655057264e28bc0b6fb378c8ef146be00"},
        {{'J', 'e', 'f', 'e'}, "what do ya want for nothing?", "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"},
        {std::vector<u8>(80, 0xaa), "Test Using Larger Than Block-Size Key - Hash Key First",
            "aa4ae5e15272d00e95705637ce8a3b55ed402112"},
    };
    for (const auto& c : cases) {
        u8 mac[20];
        crypto::hmac_sha1(c.key, reinterpret_cast<const u8*>(c.data.data()), c.data.size(), mac);
        CHECK(std::vector<u8>(mac, mac + 20) == hex(c.mac));
    }
}

TEST_CASE("AES-CMAC: RFC 4493's 40-byte example through the layer", "[crypto]")
{
    const std::vector<u8> k = hex("2b7e151628aed2a6abf7158809cf4f3c");
    u8 tag[16];
    REQUIRE(crypto::aes_cmac(k, kPlain.data(), 40, tag));
    CHECK(std::vector<u8>(tag, tag + 16) == hex("dfa66747de9ae63030ca32611497c827"));
    CHECK_FALSE(crypto::aes_cmac(std::span<const u8>(k.data(), 8), kPlain.data(), 40, tag));
}

TEST_CASE("AES: the platform's library and the portable one agree on random input; bad sizes are refused", "[crypto]")
{
    INFO("AES through " << crypto::aes_library());
    std::mt19937 rng(0x5EED);
    const auto bytes = [&rng](usize n) {
        std::vector<u8> v(n);
        for (u8& b : v) b = static_cast<u8>(rng());
        return v;
    };
    for (int round = 0; round < 300; round++) {
        const usize key_size = (round % 3 == 0) ? 16 : (round % 3 == 1) ? 24 : 32;
        const std::vector<u8> key = bytes(key_size), iv = bytes(16);
        const usize blocks = 1 + static_cast<usize>(rng() % 40);
        const std::vector<u8> plain = bytes(16 * blocks);
        std::vector<u8> a(plain.size()), b(plain.size());
        REQUIRE(crypto::aes_ecb_encrypt(key, plain.data(), plain.size(), a.data()));
        REQUIRE(crypto::soft::aes_ecb_encrypt(key, plain.data(), plain.size(), b.data()));
        CHECK((a == b)); // unexpanded: Catch2 throws printing byte vectors this long (-s)
        REQUIRE(crypto::aes_ecb_decrypt(key, a.data(), a.size(), b.data()));
        CHECK((b == plain));
        REQUIRE(crypto::aes_cbc_encrypt(key, iv.data(), plain.data(), plain.size(), a.data()));
        REQUIRE(crypto::soft::aes_cbc_encrypt(key, iv.data(), plain.data(), plain.size(), b.data()));
        CHECK((a == b));
        REQUIRE(crypto::aes_cbc_decrypt(key, iv.data(), a.data(), a.size(), b.data()));
        CHECK((b == plain));
        const usize odd = plain.size() - static_cast<usize>(rng() % 16);
        REQUIRE(crypto::aes_ctr(key, iv.data(), plain.data(), odd, a.data()));
        REQUIRE(crypto::soft::aes_ctr(key, iv.data(), plain.data(), odd, b.data()));
        CHECK(std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(odd), b.begin()));
        // A counter whose low half wraps within the data carries into its high half (D-908: CommonCrypto's alone
        // would not).
        std::vector<u8> near = iv;
        for (usize i = 8; i < 16; i++) near[i] = 0xFF;
        near[15] = static_cast<u8>(0xFF - rng() % 4);
        REQUIRE(crypto::aes_ctr(key, near.data(), plain.data(), odd, a.data()));
        REQUIRE(crypto::soft::aes_ctr(key, near.data(), plain.data(), odd, b.data()));
        CHECK(std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(odd), b.begin()));
    }
    const std::vector<u8> key = bytes(16), bad_key = bytes(20), iv = bytes(16), data = bytes(32);
    std::vector<u8> out(32);
    CHECK_FALSE(crypto::aes_ecb_encrypt(bad_key, data.data(), 16, out.data()));
    CHECK_FALSE(crypto::aes_cbc_decrypt(key, iv.data(), data.data(), 17, out.data()));
    CHECK_FALSE(crypto::soft::aes_cbc_decrypt(key, iv.data(), data.data(), 17, out.data()));
    CHECK_FALSE(crypto::aes_ctr(bad_key, iv.data(), data.data(), 5, out.data()));
    CHECK_FALSE(crypto::Aes(bad_key).valid());
    CHECK(crypto::aes_cbc_decrypt(key, iv.data(), data.data(), 0, out.data())); // nothing to do
}
