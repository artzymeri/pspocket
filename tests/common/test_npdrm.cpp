// The PSN licence layer (D-580): RAP licences to content keys, where the player's licences are looked for, and which
// key each licence type takes. Where the build has RPCS3's source, rap_to_rif and the crypto layer are compared with
// RPCS3's own code on random input (rpcs3_ref, PSPOCKET_HAVE_RPCS3_REF). Made-up keys and licences only: nothing
// real, and nothing derived from real keys, is in these tests (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>

#include "pspocket/recomp/crypto.hpp"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/recomp/npdrm.hpp"

#if defined(PSPOCKET_HAVE_RPCS3_REF)
#include "rpcs3_ref/rpcs3_ref.hpp"
#endif

using namespace pspocket;
using namespace pspocket::recomp;

namespace fsys = std::filesystem;

namespace {

// Made-up keys, from counters (RAP_PBOX a permutation, as the real one is).
NpdrmKeys made_up_npdrm_keys()
{
    NpdrmKeys k;
    for (int i = 0; i < 16; i++) {
        k.rap_key[i] = static_cast<u8>(0x31 + 7 * i);
        k.rap_pbox[i] = static_cast<u8>((5 * i + 3) % 16);
        k.rap_e1[i] = static_cast<u8>(0x52 + 11 * i);
        k.rap_e2[i] = static_cast<u8>(0x63 + 13 * i);
        k.klic_free[i] = static_cast<u8>(0x74 + 17 * i);
        k.klic_key[i] = static_cast<u8>(0x85 + 19 * i);
        k.omac_key2[i] = static_cast<u8>(0x96 + 23 * i);
        k.omac_key3[i] = static_cast<u8>(0xA7 + 29 * i);
    }
    k.present = true;
    return k;
}

Key128 key_of(u8 seed)
{
    Key128 k{};
    for (int i = 0; i < 16; i++) k[i] = static_cast<u8>(seed * 3 + i * 41);
    return k;
}

fsys::path temp_dir(std::string_view what)
{
    const fsys::path root = fsys::temp_directory_path() / std::format("pspocket-npdrm-{}-{}", what, static_cast<long>(getpid()));
    fsys::remove_all(root);
    fsys::create_directories(root);
    return root;
}

void write_file(const fsys::path& path, std::span<const u8> bytes)
{
    fsys::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

constexpr std::string_view kContent = "UP0001-NPXX00000_00-0000000000000001";

} // namespace

TEST_CASE("NPDRM: content IDs and where the player's licences are looked for (RPCS3 get_rap_file_path)", "[npdrm]")
{
    CHECK(valid_content_id(kContent));
    CHECK(valid_content_id("a_B-9"));
    CHECK(valid_content_id(std::string(0x30, 'X')));
    CHECK_FALSE(valid_content_id(std::string(0x31, 'X')));
    CHECK_FALSE(valid_content_id(""));
    CHECK_FALSE(valid_content_id("../x"));
    CHECK_FALSE(valid_content_id("UP0001 NPXX"));

    CHECK(rap_guest_path(kContent) == "/dev_hdd0/home/00000001/exdata/UP0001-NPXX00000_00-0000000000000001.rap");
    CHECK(rap_guest_path("../../x").empty());
    CHECK(rap_host_path("/a/hdd0", kContent) == "/a/hdd0/home/00000001/exdata/UP0001-NPXX00000_00-0000000000000001.rap");
    CHECK(rap_host_path("/a/hdd0//", kContent) == rap_host_path("/a/hdd0", kContent));
    CHECK(rap_host_path("/a/hdd0", "x/y").empty());
}

TEST_CASE("NPDRM: a RAP file is read as 16 bytes; a missing or short one is refused with its path", "[npdrm]")
{
    const fsys::path dir = temp_dir("rap");
    const Key128 rap = key_of(9);
    write_file(dir / "whole.rap", rap);
    std::vector<u8> longer(rap.begin(), rap.end());
    longer.push_back(0xEE);
    write_file(dir / "longer.rap", longer);
    write_file(dir / "short.rap", std::span<const u8>(rap.data(), 15));

    auto whole = read_rap_file((dir / "whole.rap").string());
    REQUIRE(whole);
    CHECK(*whole == rap);
    auto more = read_rap_file((dir / "longer.rap").string());
    REQUIRE(more);
    CHECK(*more == rap);
    auto missing = read_rap_file((dir / "missing.rap").string());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().message.find("no licence") != std::string::npos);
    CHECK(missing.error().message.find("missing.rap") != std::string::npos);
    auto shorter = read_rap_file((dir / "short.rap").string());
    REQUIRE_FALSE(shorter);
    CHECK(shorter.error().message.find("15 bytes") != std::string::npos);
    fsys::remove_all(dir);
}

TEST_CASE("NPDRM: each licence type takes its key, the klicensee or the player's RAP (RPCS3 DecryptNPDRM, ReadHeader)",
    "[npdrm]")
{
    const NpdrmKeys keys = made_up_npdrm_keys();
    const fsys::path hdd0 = temp_dir("hdd0");
    const Key128 rap = key_of(1);
    write_file(rap_host_path(hdd0.string(), kContent), rap);
    NpdrmLicence licence = hdd0_licence(hdd0.string());
    licence.keys = &keys;
    const Key128 rif = rap_to_rif(rap, keys);
    CHECK(rif != rap);
    CHECK(rap_to_rif(rap, keys) == rif); // the same every time
    CHECK(rap_to_rif(key_of(2), keys) != rif);

    // Free content: the klicensee, NP_KLIC_FREE when there is none; for an EDAT any type whose low bits are 3.
    auto free_self = npdrm_content_key(NpdrmContent::self, 3, kContent, licence);
    REQUIRE(free_self);
    CHECK(*free_self == keys.klic_free);
    NpdrmLicence with_klic = licence;
    with_klic.klicensee = key_of(7);
    CHECK(*npdrm_content_key(NpdrmContent::self, 3, kContent, with_klic) == key_of(7));
    CHECK(*npdrm_content_key(NpdrmContent::edat, 3, kContent, with_klic) == key_of(7));
    CHECK(*npdrm_content_key(NpdrmContent::edat, 7, kContent, with_klic) == key_of(7));

    // Network and local content: the RIF key of the player's RAP, whatever klicensee the game has.
    for (const s32 type : {1, 2}) {
        auto key = npdrm_content_key(NpdrmContent::self, type, kContent, with_klic);
        REQUIRE(key);
        CHECK(*key == rif);
        CHECK(*npdrm_content_key(NpdrmContent::edat, type, kContent, with_klic) == rif);
    }
    // An EDAT of type 0 takes the RAP too (RPCS3's "also used for type 1"); a SELF of another type is refused.
    CHECK(*npdrm_content_key(NpdrmContent::edat, 0, kContent, licence) == rif);
    auto odd = npdrm_content_key(NpdrmContent::self, 4, kContent, licence);
    REQUIRE_FALSE(odd);
    CHECK(odd.error().message.find("licence type 4") != std::string::npos);
    CHECK_FALSE(npdrm_content_key(NpdrmContent::self, 0, kContent, licence));

    // No RAP for that content, an invalid content ID, no licences at all, no keys in the build.
    auto missing = npdrm_content_key(NpdrmContent::self, 2, "UP0001-NPXX00000_00-0000000000000002", licence);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().message.find("home/00000001/exdata/UP0001-NPXX00000_00-0000000000000002.rap") !=
        std::string::npos);
    CHECK_FALSE(npdrm_content_key(NpdrmContent::self, 2, "../x", licence));
    NpdrmLicence none;
    none.keys = &keys;
    CHECK_FALSE(npdrm_content_key(NpdrmContent::self, 2, kContent, none));
    CHECK(*npdrm_content_key(NpdrmContent::self, 3, kContent, none) == keys.klic_free);
    NpdrmKeys absent;
    none.keys = &absent;
    auto no_keys = npdrm_content_key(NpdrmContent::self, 3, kContent, none);
    REQUIRE_FALSE(no_keys);
    CHECK(no_keys.error().message.find("no NPDRM keys") != std::string::npos);
    fsys::remove_all(hdd0);
}

#if defined(PSPOCKET_HAVE_RPCS3_REF)

TEST_CASE("NPDRM against RPCS3: the build's keys are key_vault.h's; rap_to_rif on random RAPs", "[npdrm][rpcs3]")
{
    const NpdrmKeys& keys = builtin_npdrm_keys();
    if (!keys.present) SKIP("this build has no NPDRM keys");
    CHECK(test::rpcs3_ref::same_keys(keys, builtin_npd_keys()));
    std::mt19937 rng(0xA11CE);
    for (int i = 0; i < 2000; i++) {
        Key128 rap{};
        for (u8& b : rap) b = static_cast<u8>(rng());
        if (i == 0) rap.fill(0);
        if (i == 1) rap.fill(0xFF);
        REQUIRE(rap_to_rif(rap, keys) == test::rpcs3_ref::rap_to_rif(rap));
    }
}

TEST_CASE("The crypto layer against RPCS3's PolarSSL: AES, AES-CMAC and HMAC-SHA1 on random input",
    "[crypto][rpcs3]")
{
    std::mt19937 rng(0xC0DE);
    const auto bytes = [&rng](usize n) {
        std::vector<u8> v(n);
        for (u8& b : v) b = static_cast<u8>(rng());
        return v;
    };
    for (int round = 0; round < 200; round++) {
        const usize key_size = round % 3 == 0 ? 16 : round % 3 == 1 ? 24 : 32;
        const std::vector<u8> key = bytes(key_size), iv = bytes(16), plain = bytes(16 * (1 + rng() % 20));
        std::vector<u8> ours(plain.size()), theirs(plain.size());
        crypto::aes_ecb_encrypt(key, plain.data(), 16, ours.data());
        test::rpcs3_ref::aes_ecb(true, key, plain.data(), theirs.data());
        CHECK(std::equal(ours.begin(), ours.begin() + 16, theirs.begin()));
        crypto::aes_ecb_decrypt(key, plain.data(), 16, ours.data());
        test::rpcs3_ref::aes_ecb(false, key, plain.data(), theirs.data());
        CHECK(std::equal(ours.begin(), ours.begin() + 16, theirs.begin()));
        crypto::aes_cbc_encrypt(key, iv.data(), plain.data(), plain.size(), ours.data());
        test::rpcs3_ref::aes_cbc(true, key, iv.data(), plain.data(), plain.size(), theirs.data());
        CHECK((ours == theirs)); // unexpanded: Catch2 throws printing byte vectors this long (-s)
        crypto::aes_cbc_decrypt(key, iv.data(), plain.data(), plain.size(), ours.data());
        test::rpcs3_ref::aes_cbc(false, key, iv.data(), plain.data(), plain.size(), theirs.data());
        CHECK((ours == theirs));
        const usize odd = plain.size() - rng() % 16;
        crypto::aes_ctr(key, iv.data(), plain.data(), odd, ours.data());
        test::rpcs3_ref::aes_ctr(key, iv.data(), plain.data(), odd, theirs.data());
        CHECK(std::equal(ours.begin(), ours.begin() + static_cast<std::ptrdiff_t>(odd), theirs.begin()));

        Key128 k16{};
        std::copy(key.begin(), key.begin() + 16, k16.begin());
        u8 mac_ours[20]{}, mac_theirs[20]{};
        crypto::aes_cmac(k16, plain.data(), odd, mac_ours);
        test::rpcs3_ref::aes_cmac(k16, plain.data(), odd, mac_theirs);
        CHECK(std::memcmp(mac_ours, mac_theirs, 16) == 0);
        const std::vector<u8> hkey = bytes(1 + rng() % 100);
        crypto::hmac_sha1(hkey, plain.data(), odd, mac_ours);
        test::rpcs3_ref::hmac_sha1(hkey, plain.data(), odd, mac_theirs);
        CHECK(std::memcmp(mac_ours, mac_theirs, 20) == 0);
    }
}

#endif
