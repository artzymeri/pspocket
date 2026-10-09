// SELF decryption (D-121): a synthetic PPU executable wrapped in a SELF, compressed and encrypted here with made-up
// keys the way a retail SELF is (metadata info AES-256-CBC, metadata and segments AES-128-CTR, segments zlib), comes
// back out as its ELF. No real keys and no game data (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include "pspocket/core/be.h"
#include "pspocket/recomp/crypto.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "synthetic.hpp"

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

void aes128_ctr(const u8* key, const u8* iv, u8* data, usize size)
{
    REQUIRE(crypto::aes_ctr(std::span<const u8>(key, 16), iv, data, size, data));
}

void put32(std::vector<u8>& b, usize at, u32 v) { ps_st32(b.data() + at, v); }
void put64(std::vector<u8>& b, usize at, u64 v) { ps_st64(b.data() + at, v); }

struct Encrypted {
    std::vector<u8> self;
    std::vector<u8> elf;
};

// The synthetic executable as a retail SELF: wrap_in_self's headers, then real metadata (one section per segment
// with bytes, each with its own key and counter) and the segments compressed and encrypted.
Encrypted encrypted_self(const SelfKey& key, u16 revision)
{
    Encrypted out;
    out.elf = test::build_ppu_exec().bytes;
    out.self = test::wrap_in_self(out.elf, {.debug = false, .npdrm = false, .npdrm_license = 3, .self_type = 4});
    std::vector<u8>& s = out.self;
    auto elf = parse_elf_header(out.elf);
    REQUIRE(elf);
    auto phdrs = parse_program_headers(out.elf, *elf, elf->phoff, 0);
    REQUIRE(phdrs);
    std::vector<u32> with_bytes;
    for (u32 i = 0; i < phdrs->size(); i++) {
        if ((*phdrs)[i].filesz) with_bytes.push_back(i);
    }
    const u32 n = static_cast<u32>(with_bytes.size());
    REQUIRE(n > 0);

    const usize meta = ps_ld32(s.data() + 0x0C) + 0x20; // the metadata info
    const usize header_size = (meta + 0x40 + 0x20 + n * 0x30 + 2 * n * 16 + 0xF) & ~usize{0xF};
    s.resize(header_size);
    std::fill(s.begin() + static_cast<std::ptrdiff_t>(meta), s.end(), u8{0});
    ps_st16(s.data() + 0x08, revision);
    put64(s, 0x10, header_size);

    // Metadata info: the key and counter for the rest, then AES-256-CBC with the SELF key.
    const auto meta_key = made_up<16>(0x51), meta_iv = made_up<16>(0x62);
    std::copy(meta_key.begin(), meta_key.end(), s.begin() + static_cast<std::ptrdiff_t>(meta));
    std::copy(meta_iv.begin(), meta_iv.end(), s.begin() + static_cast<std::ptrdiff_t>(meta + 0x20));
    REQUIRE(crypto::aes_cbc_encrypt(key.erk, key.riv.data(), s.data() + meta, 0x40, s.data() + meta));

    // Metadata header, section headers, data keys; then each segment, compressed and encrypted.
    const usize head = meta + 0x40;
    put64(s, head + 0x00, header_size);
    put32(s, head + 0x08, 1);
    put32(s, head + 0x0C, n);
    put32(s, head + 0x10, 2 * n);
    for (u32 k = 0; k < n; k++) {
        const ProgramHeader& ph = (*phdrs)[with_bytes[k]];
        std::vector<u8> data(compressBound(static_cast<uLong>(ph.filesz)));
        uLongf size = static_cast<uLongf>(data.size());
        REQUIRE(compress2(data.data(), &size, out.elf.data() + ph.offset, static_cast<uLong>(ph.filesz), 9) == Z_OK);
        data.resize(size);
        const auto seg_key = made_up<16>(static_cast<u8>(0x10 + 2 * k)), seg_iv = made_up<16>(static_cast<u8>(0x90 + 2 * k));
        aes128_ctr(seg_key.data(), seg_iv.data(), data.data(), data.size());
        const usize at = (s.size() + 0xF) & ~usize{0xF};
        s.resize(at);
        s.insert(s.end(), data.begin(), data.end());

        const usize sh = head + 0x20 + k * 0x30;
        put64(s, sh + 0x00, at);
        put64(s, sh + 0x08, data.size());
        put32(s, sh + 0x10, 2);            // a segment
        put32(s, sh + 0x14, with_bytes[k]);
        put32(s, sh + 0x18, 2);
        put32(s, sh + 0x20, 3);            // encrypted
        put32(s, sh + 0x24, 2 * k);        // key
        put32(s, sh + 0x28, 2 * k + 1);    // counter
        put32(s, sh + 0x2C, 2);            // zlib
        const usize keys = head + 0x20 + n * 0x30 + 2 * k * 16;
        std::copy(seg_key.begin(), seg_key.end(), s.begin() + static_cast<std::ptrdiff_t>(keys));
        std::copy(seg_iv.begin(), seg_iv.end(), s.begin() + static_cast<std::ptrdiff_t>(keys + 16));
    }
    aes128_ctr(meta_key.data(), meta_iv.data(), s.data() + head, header_size - head);
    put64(s, 0x18, s.size() - header_size);
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

#endif
