// SELF decryption (D-121, D-582): a synthetic PPU executable wrapped in a SELF, compressed and encrypted here with
// made-up keys the way a retail SELF is (metadata info AES-256-CBC, metadata and segments AES-128-CTR, segments zlib),
// and the way a PSN executable is (the NPDRM layer over the metadata info, with a klicensee or a RAP), comes back out
// as its ELF. Where the build has RPCS3's source, the NPDRM layer is set against RPCS3's own DecryptNPDRM. No real keys
// and no game data in the tests (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <optional>
#include <random>
#include <string>

#include "pspocket/core/be.h"
#include "pspocket/recomp/crypto.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "synthetic.hpp"

#if defined(PSPOCKET_HAVE_RPCS3_REF)
#include "rpcs3_ref/rpcs3_ref.hpp"
#endif

#if __has_include(<zlib.h>)
#include <zlib.h>

using namespace pspocket;
using namespace pspocket::recomp;

namespace {

// Made-up keys, from a counter: nothing real.
template <usize N>
std::array<u8, N> made_up(u8 seed)
{
    std::array<u8, N> a{};
    for (usize i = 0; i < N; i++) a[i] = static_cast<u8>(seed + i * 37);
    return a;
}

struct Encrypted {
    std::vector<u8> self;
    std::vector<u8> elf;
};

// A PSN executable's extra layer (D-582): its licence type, and the key of the AES-128-CBC layer over its metadata
// info (its content key decrypted with NP_KLIC_KEY).
struct NpdrmLayer {
    s32 license = 3;
    Key128 metadata_key{};
};

// The synthetic executable as a retail SELF (test::encrypt_self: real metadata, the segments compressed and
// encrypted); with `npdrm`, a PSN executable with the layer over its metadata info.
Encrypted encrypted_self(const SelfKey& key, u16 revision, const std::optional<NpdrmLayer>& npdrm = std::nullopt)
{
    Encrypted out;
    out.elf = test::build_ppu_exec().bytes;
    test::SelfEncryption e;
    e.key = key;
    e.revision = revision;
    if (npdrm) {
        e.npdrm = true;
        e.npdrm_license = npdrm->license;
        e.npdrm_layer_key = npdrm->metadata_key;
    }
    out.self = test::encrypt_self(out.elf, e);
    REQUIRE_FALSE(out.self.empty());
    return out;
}

SelfKey app_key(u32 revision)
{
    SelfKey k;
    k.type = 4;
    k.revision = revision;
    k.erk = made_up<32>(0x23);
    k.riv = made_up<16>(0x45);
    return k;
}

SelfKey npdrm_self_key(u32 revision)
{
    SelfKey k = app_key(revision);
    k.type = 8;
    k.erk = made_up<32>(0x34);
    return k;
}

NpdrmKeys made_up_npdrm_keys()
{
    NpdrmKeys k;
    k.rap_key = made_up<16>(0x05);
    for (int i = 0; i < 16; i++) k.rap_pbox[i] = static_cast<u8>((7 * i + 2) % 16);
    k.rap_e1 = made_up<16>(0x17);
    k.rap_e2 = made_up<16>(0x29);
    k.klic_free = made_up<16>(0x3B);
    k.klic_key = made_up<16>(0x4D);
    k.present = true;
    return k;
}

// The key of a PSN executable's layer for a content key (as RPCS3's DecryptNPDRM makes it, in the test's own words).
Key128 layer_key(const NpdrmKeys& keys, const Key128& content_key)
{
    Key128 k{};
    REQUIRE(crypto::aes_ecb_decrypt(keys.klic_key, content_key.data(), 16, k.data()));
    return k;
}

// Every byte the original ELF's headers and segments put in the file came back.
void check_same_elf(const std::vector<u8>& want, const std::vector<u8>& got)
{
    auto header = parse_elf_header(want);
    REQUIRE(header);
    CHECK(std::equal(want.begin(), want.begin() + 0x40, got.begin()));
    auto phdrs = parse_program_headers(want, *header, header->phoff, 0);
    REQUIRE(phdrs);
    for (const ProgramHeader& ph : *phdrs) {
        if (!ph.filesz) continue;
        INFO("segment at " << ph.offset);
        REQUIRE(got.size() >= ph.offset + ph.filesz);
        CHECK(std::equal(want.begin() + static_cast<std::ptrdiff_t>(ph.offset),
            want.begin() + static_cast<std::ptrdiff_t>(ph.offset + ph.filesz),
            got.begin() + static_cast<std::ptrdiff_t>(ph.offset)));
    }
}

} // namespace

TEST_CASE("a retail-style SELF decrypts into its ELF: headers and every segment (D-121)")
{
    const SelfKey key = app_key(0x0A);
    const Encrypted e = encrypted_self(key, 0x0A);
    const std::vector<SelfKey> keys{app_key(0x09), key}; // the revision picks the key
    auto elf = decrypt_self(e.self, keys);
    INFO((elf ? std::string() : elf.error().message));
    REQUIRE(elf);
    auto want = parse_elf_header(e.elf);
    auto got = parse_elf_header(*elf);
    REQUIRE(want);
    REQUIRE(got);
    CHECK(std::equal(e.elf.begin(), e.elf.begin() + 0x40, elf->begin()));
    auto phdrs = parse_program_headers(e.elf, *want, want->phoff, 0);
    REQUIRE(phdrs);
    for (const ProgramHeader& ph : *phdrs) {
        if (!ph.filesz) continue;
        INFO("segment at " << ph.offset);
        REQUIRE(elf->size() >= ph.offset + ph.filesz);
        CHECK(std::equal(e.elf.begin() + static_cast<std::ptrdiff_t>(ph.offset),
            e.elf.begin() + static_cast<std::ptrdiff_t>(ph.offset + ph.filesz),
            elf->begin() + static_cast<std::ptrdiff_t>(ph.offset)));
    }
}

TEST_CASE("a SELF without its key, with a wrong key or from PSN is refused with the reason (D-121)")
{
    const SelfKey key = app_key(0x0A);
    const Encrypted e = encrypted_self(key, 0x0A);

    auto none = decrypt_self(e.self, {});
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("carries no SELF keys") != std::string::npos);

    const std::vector<SelfKey> other_revision{app_key(0x0B)};
    auto missing = decrypt_self(e.self, other_revision);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().message.find("no key for a APP executable of key revision 0xa") != std::string::npos);

    SelfKey wrong = key;
    wrong.erk[0] ^= 1;
    const std::vector<SelfKey> wrong_keys{wrong};
    auto opened = decrypt_self(e.self, wrong_keys);
    REQUIRE_FALSE(opened);
    CHECK(opened.error().message.find("does not open the metadata") != std::string::npos);

    const auto psn = test::wrap_in_self(test::build_ppu_exec().bytes); // an NPDRM control header
    const std::vector<SelfKey> keys{key};
    auto refused = decrypt_self(psn, keys);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("PSN executable") != std::string::npos);
}

TEST_CASE("the SELF key for an executable is the first that fits its type, revision or version (RPCS3 FindSelfKey)")
{
    std::vector<SelfKey> keys(4);
    keys[0].type = 3, keys[0].version_start = 0x0001000000000000ull, keys[0].version_end = 0x0002000000000000ull;
    keys[1].type = 3, keys[1].version_start = 0x0001500000000000ull, keys[1].version_end = 0x0003000000000000ull;
    keys[2].type = 4, keys[2].revision = 0x1C;
    keys[3].type = 4, keys[3].revision = 0x1C;
    CHECK(find_self_key(keys, 3, 0, 0x0001800000000000ull) == &keys[0]);
    CHECK(find_self_key(keys, 3, 0, 0x0002800000000000ull) == &keys[1]);
    CHECK(find_self_key(keys, 3, 0, 0x0004000000000000ull) == nullptr);
    CHECK(find_self_key(keys, 4, 0x1C, 0) == &keys[2]);
    CHECK(find_self_key(keys, 4, 0x1D, 0) == nullptr);
    CHECK(find_self_key(keys, 8, 0x1C, 0) == nullptr);
}

TEST_CASE("a debug SELF gives its plain ELF back without a key")
{
    const auto elf = test::build_ppu_exec().bytes;
    const auto self = test::wrap_in_self(elf, {.debug = true, .npdrm = false, .npdrm_license = 3, .self_type = 4});
    auto out = decrypt_self(self, {});
    REQUIRE(out);
    CHECK(*out == elf);
}

TEST_CASE("a PSN executable (NPDRM SELF) decrypts with its licence: the klicensee or the player's RAP (D-582)",
    "[npdrm]")
{
    const NpdrmKeys nk = made_up_npdrm_keys();
    const SelfKey key = npdrm_self_key(0x0A);
    const std::vector<SelfKey> keys{app_key(0x0A), key};
    const std::string content(test::kSelfContentId);

    SECTION("free content: the game's klicensee, or NP_KLIC_FREE when it gives none")
    {
        const Key128 klicensee = made_up<16>(0x77);
        const Encrypted e = encrypted_self(key, 0x0A, NpdrmLayer{3, layer_key(nk, klicensee)});
        NpdrmLicence licence;
        licence.klicensee = klicensee;
        licence.keys = &nk;
        auto elf = decrypt_self(e.self, keys, &licence);
        INFO((elf ? std::string() : elf.error().message));
        REQUIRE(elf);
        check_same_elf(e.elf, *elf);

        const Encrypted by_default = encrypted_self(key, 0x0A, NpdrmLayer{3, layer_key(nk, nk.klic_free)});
        NpdrmLicence free_licence;
        free_licence.keys = &nk;
        auto plain = decrypt_self(by_default.self, keys, &free_licence);
        REQUIRE(plain);
        check_same_elf(by_default.elf, *plain);
        // The default klicensee does not open the first, nor the game's the second.
        auto wrong = decrypt_self(e.self, keys, &free_licence);
        REQUIRE_FALSE(wrong);
        CHECK(wrong.error().message.find("its licence does not open it") != std::string::npos);
        CHECK_FALSE(decrypt_self(by_default.self, keys, &licence));
    }
    SECTION("network and local content: the RIF key of the player's RAP for its content ID")
    {
        const Key128 rap = made_up<16>(0x99);
        NpdrmLicence licence;
        licence.klicensee = made_up<16>(0x01); // not used by these
        licence.keys = &nk;
        licence.rap = [&](std::string_view id) -> Result<Key128> {
            if (id == content) return rap;
            return make_error("no licence for {}", id);
        };
        for (const s32 type : {1, 2}) {
            INFO("licence type " << type);
            const Encrypted e = encrypted_self(key, 0x0A, NpdrmLayer{type, layer_key(nk, rap_to_rif(rap, nk))});
            auto elf = decrypt_self(e.self, keys, &licence);
            INFO((elf ? std::string() : elf.error().message));
            REQUIRE(elf);
            check_same_elf(e.elf, *elf);
        }
        // Another player's RAP (one for other content) does not open it; no RAP is refused with the licence's reason.
        const Encrypted e = encrypted_self(key, 0x0A, NpdrmLayer{2, layer_key(nk, rap_to_rif(rap, nk))});
        NpdrmLicence other = licence;
        other.rap = [](std::string_view) -> Result<Key128> { return made_up<16>(0x98); };
        auto wrong = decrypt_self(e.self, keys, &other);
        REQUIRE_FALSE(wrong);
        CHECK(wrong.error().message.find("its licence does not open it") != std::string::npos);
        NpdrmLicence missing = licence;
        missing.rap = [](std::string_view id) -> Result<Key128> { return make_error("no licence {}.rap", id); };
        auto none = decrypt_self(e.self, keys, &missing);
        REQUIRE_FALSE(none);
        CHECK(none.error().message.find("no licence " + content + ".rap") != std::string::npos);
    }
    SECTION("refused: no licence given, a licence type no executable has, no NPDRM keys, no NPDRM SELF key")
    {
        const Encrypted e = encrypted_self(key, 0x0A, NpdrmLayer{3, layer_key(nk, nk.klic_free)});
        auto none = decrypt_self(e.self, keys);
        REQUIRE_FALSE(none);
        CHECK(none.error().message.find("PSN executable") != std::string::npos);
        CHECK(none.error().message.find("needs its licence") != std::string::npos);

        NpdrmLicence licence;
        licence.keys = &nk;
        const Encrypted odd = encrypted_self(key, 0x0A, NpdrmLayer{4, layer_key(nk, nk.klic_free)});
        auto refused = decrypt_self(odd.self, keys, &licence);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().message.find("licence type 4") != std::string::npos);

        const NpdrmKeys absent;
        licence.keys = &absent;
        auto no_keys = decrypt_self(e.self, keys, &licence);
        REQUIRE_FALSE(no_keys);
        CHECK(no_keys.error().message.find("no NPDRM keys") != std::string::npos);

        licence.keys = &nk;
        const std::vector<SelfKey> app_only{app_key(0x0A)};
        auto no_self_key = decrypt_self(e.self, app_only, &licence);
        REQUIRE_FALSE(no_self_key);
        CHECK(no_self_key.error().message.find("no key for a NPDRM executable") != std::string::npos);
    }
}

#if defined(PSPOCKET_HAVE_RPCS3_REF)

TEST_CASE("NPDRM SELFs against RPCS3: DecryptNPDRM's layer on random metadata infos", "[npdrm][rpcs3]")
{
    const NpdrmKeys& nk = builtin_npdrm_keys();
    if (!nk.present) SKIP("this build has no NPDRM keys");
    std::mt19937 rng(0x5E1F);
    const auto random_key = [&rng] {
        Key128 k{};
        for (u8& b : k) b = static_cast<u8>(rng());
        return k;
    };
    const s32 licenses[] = {0, 1, 2, 3, 4};
    for (int round = 0; round < 300; round++) {
        const s32 license = licenses[rng() % std::size(licenses)];
        const std::string content = "UP0001-NPXX" + std::to_string(10000 + rng() % 90000) + "_00-" +
            std::to_string(rng());
        const Key128 klicensee = random_key(), rap = random_key();
        const bool have_rap = rng() % 4 != 0;
        std::array<u8, 0x40> info{};
        for (u8& b : info) b = static_cast<u8>(rng());

        NpdrmLicence licence;
        licence.klicensee = klicensee;
        licence.rap = [&](std::string_view) -> Result<Key128> {
            if (have_rap) return rap;
            return make_error("no licence");
        };
        NpdrmInfo npd;
        npd.license = license;
        npd.content_id = content;
        std::array<u8, 0x40> ours = info;
        const Result<Key128> key = npdrm_metadata_key(npd, licence);
        const u8 zero_iv[16]{};
        const bool ours_ok = key && crypto::aes_cbc_decrypt(*key, zero_iv, info.data(), info.size(), ours.data());

        std::array<u8, 0x40> theirs = info;
        const bool theirs_ok = test::rpcs3_ref::decrypt_npdrm(theirs.data(), static_cast<u32>(theirs.size()), license,
            content, klicensee, [&](std::string_view) -> std::optional<Key128> {
                if (have_rap) return rap;
                return std::nullopt;
            });
        INFO("licence type " << license << (have_rap ? ", with a RAP" : ", no RAP"));
        REQUIRE(ours_ok == theirs_ok);
        if (ours_ok) CHECK(ours == theirs);
    }
}

#endif

#endif
