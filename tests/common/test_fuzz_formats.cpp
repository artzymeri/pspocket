// Mutation fuzzing of recomp/common's parsers of game files (D-603, mutation_fuzz.hpp): ELF executables, PRX modules
// and SPU programs with their module tables, relocations and embedded programs; SELF headers and decryption, the
// decrypted metadata included; NPD files (SDATA and EDATA) and their LZ blocks; PS3 packages installed into a
// temporary dev_hdd0. The inputs are synthetic.cpp's (made-up keys only, CLAUDE.md rule 1); each mutated copy must be
// read or refused. Longer: PSPOCKET_FUZZ_ITERS=<n> (and PSPOCKET_FUZZ_SEED) build/host/tests/common/test_recomp_common
// "[fuzz]".

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "mutation_fuzz.hpp"
#include "pspocket/core/be.h"
#include "pspocket/recomp/crypto.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/guest_image.hpp"
#include "pspocket/recomp/inventory.hpp"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/recomp/pkg.hpp"
#include "pspocket/recomp/ppu_module.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/sce.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "pspocket/recomp/spu_scan.hpp"
#include "synthetic.hpp"

using namespace pspocket;
using namespace pspocket::recomp;
namespace fuzz = pspocket::test::fuzz;
namespace fs = std::filesystem;

namespace {

template <usize N>
std::array<u8, N> made_up(u8 seed)
{
    std::array<u8, N> a{};
    for (usize i = 0; i < N; i++) a[i] = static_cast<u8>(seed + i * 37);
    return a;
}

std::vector<u8> pattern(usize size, u32 seed)
{
    std::vector<u8> v(size);
    u32 x = seed;
    for (u8& b : v) {
        x = x * 1103515245u + 12345u;
        b = static_cast<u8>(x >> 16);
    }
    return v;
}

// The fields of an ELF's header, program headers and section headers (32- or 64-bit), and the words of its small
// LOAD segments (where a PPU image's module tables, stubs and descriptors are).
std::vector<fuzz::Field> elf_fields(std::span<const u8> d)
{
    std::vector<fuzz::Field> f;
    if (d.size() < 0x34 || d[0] != 0x7F || d[1] != 'E') return f;
    const bool is64 = d[4] == 2;
    if (is64) {
        fuzz::append(f, {{0x18, 8}, {0x20, 8}, {0x28, 8}, {0x30, 4}, {0x34, 2}, {0x36, 2}, {0x38, 2}, {0x3A, 2},
                            {0x3C, 2}, {0x3E, 2}});
    } else {
        fuzz::append(f, {{0x18, 4}, {0x1C, 4}, {0x20, 4}, {0x28, 2}, {0x2A, 2}, {0x2C, 2}, {0x2E, 2}, {0x30, 2},
                            {0x32, 2}});
    }
    const u64 phoff = is64 ? ps_ld64(d.data() + 0x20) : ps_ld32(d.data() + 0x1C);
    const u64 shoff = is64 ? ps_ld64(d.data() + 0x28) : ps_ld32(d.data() + 0x20);
    const u16 phnum = ps_ld16(d.data() + (is64 ? 0x38 : 0x2C)), shnum = ps_ld16(d.data() + (is64 ? 0x3C : 0x30));
    const usize phsize = is64 ? 56 : 32, shsize = is64 ? 64 : 40;
    for (u16 i = 0; i < phnum && phoff + (i + 1) * phsize <= d.size(); i++) {
        const usize p = static_cast<usize>(phoff + i * phsize);
        if (is64) {
            fuzz::append(f, {{p, 4}, {p + 4, 4}, {p + 8, 8}, {p + 16, 8}, {p + 32, 8}, {p + 40, 8}, {p + 48, 8}});
            const u64 off = ps_ld64(d.data() + p + 8), filesz = ps_ld64(d.data() + p + 32);
            if (filesz <= 0x1000 && off + filesz <= d.size())
                fuzz::append(f, fuzz::fields(static_cast<usize>(off), static_cast<usize>(filesz / 4)));
        } else {
            fuzz::append(f, {{p, 4}, {p + 4, 4}, {p + 8, 4}, {p + 16, 4}, {p + 20, 4}, {p + 28, 4}});
        }
    }
    for (u16 i = 0; i < shnum && shoff + (i + 1) * shsize <= d.size(); i++) {
        const usize s = static_cast<usize>(shoff + i * shsize);
        if (is64) fuzz::append(f, {{s, 4}, {s + 4, 4}, {s + 16, 8}, {s + 24, 8}, {s + 32, 8}, {s + 40, 4}, {s + 56, 8}});
        else fuzz::append(f, {{s, 4}, {s + 4, 4}, {s + 16, 4}, {s + 20, 4}, {s + 24, 4}, {s + 36, 4}});
    }
    return f;
}

// What the toolchain and the loader read of an ELF: its tables, the module's imports and exports, the relocations, the
// embedded SPU programs, a PRX's placement, the inventory. True when the ELF parsed.
bool read_elf(std::span<const u8> bytes)
{
    auto elf = ElfFile::parse(bytes);
    if (!elf) return false;
    (void)elf->extent();
    for (const SectionHeader& s : elf->sections()) (void)elf->section_data(s);
    for (const ProgramHeader& p : elf->segments()) (void)elf->segment_data(p);
    if (elf->is_ppu()) {
        if (auto image = GuestImage::from_elf(*elf)) {
            for (const auto& seg : image->segments()) {
                (void)image->read32(seg.vaddr);
                (void)image->read_cstring(seg.vaddr, 64);
            }
            if (auto module = analyze_ppu_module(*elf, *image)) resolve_names(*module, NidDb::builtin());
        }
        (void)scan_spu_images(*elf);
        if (elf->header().type == 0xFFA4) { // a PRX: placed in a small region, as the preparation places one
            (void)prx_identity(*elf);
            const std::vector<std::vector<u32>> sizes{prx_load_sizes(*elf)};
            const auto layout = prx_fixed_layout(sizes, 0x30000000, 0x01000000);
            if (!layout.empty()) (void)place_prx(*elf, bytes, layout[0]);
        }
    }
    if (elf->is_spu()) (void)measure_spu_image(bytes);
    InspectOptions options;
    options.collect_strings = true;
    (void)inspect_bytes("fuzz", bytes, options);
    return true;
}

// ---- SELF

SelfKey app_key(u32 revision)
{
    SelfKey k;
    k.type = 4;
    k.revision = revision;
    k.erk = made_up<32>(0x23);
    k.riv = made_up<16>(0x45);
    return k;
}

// The fields of a SELF's plain headers: the SCE and extended headers, the app info, the segment and control infos and
// the ELF header and program headers it carries.
std::vector<fuzz::Field> self_fields(std::span<const u8> d)
{
    std::vector<fuzz::Field> f = fuzz::fields(0, 8);
    fuzz::append(f, {{0x08, 2}, {0x0A, 2}});
    fuzz::append(f, fuzz::fields(0x10, 11, 8));
    auto info = parse_self(d);
    if (!info || !info->ext) return f;
    const SelfExtHeader& ext = *info->ext;
    if (ext.app_info_offset + 0x20 <= d.size()) fuzz::append(f, fuzz::fields(static_cast<usize>(ext.app_info_offset), 8));
    for (usize i = 0; i < info->segments.size(); i++) {
        const usize at = static_cast<usize>(ext.segment_info_offset + i * 0x20);
        if (at + 0x20 <= d.size()) fuzz::append(f, {{at, 8}, {at + 8, 8}, {at + 16, 4}, {at + 20, 4}, {at + 24, 4}, {at + 28, 4}});
    }
    if (ext.control_info_offset + ext.control_info_size <= d.size())
        fuzz::append(f, fuzz::fields(static_cast<usize>(ext.control_info_offset), static_cast<usize>(ext.control_info_size / 4)));
    if (ext.elf_offset < d.size()) {
        for (fuzz::Field e : elf_fields(d.subspan(static_cast<usize>(ext.elf_offset)))) {
            if (e.offset >= 0x40) continue; // the header only: the program headers are at phdr_offset
            e.offset += static_cast<usize>(ext.elf_offset);
            f.push_back(e);
        }
    }
    for (usize i = 0; i < info->segments.size(); i++) {
        const usize p = static_cast<usize>(ext.phdr_offset + i * 56);
        if (p + 56 <= d.size()) fuzz::append(f, {{p, 4}, {p + 8, 8}, {p + 16, 8}, {p + 32, 8}, {p + 40, 8}});
    }
    return f;
}

// A retail SELF's metadata header and section headers, corrupted in their plain form: the metadata info opens with
// the SELF key, its key and counter give the AES-128-CTR stream over the rest (as decrypt_self reads them).
std::vector<fuzz::Field> self_metadata_fields(std::span<const u8> d, const SelfKey& key)
{
    std::vector<fuzz::Field> f;
    const usize meta = ps_ld32(d.data() + 0x0C) + 0x20;
    if (meta + 0x40 + 0x20 > d.size()) return f;
    u8 info[0x40];
    REQUIRE(crypto::aes_cbc_decrypt(key.erk, key.riv.data(), d.data() + meta, 0x40, info));
    const usize head = meta + 0x40;
    const usize span = std::min<usize>(d.size() - head, 0x20 + 16 * 0x30);
    std::vector<u8> zeros(span), stream(span);
    REQUIRE(crypto::aes_ctr(std::span<const u8>(info, 16), info + 0x20, zeros.data(), span, stream.data()));
    const auto masked = [&](usize at, u8 size) {
        if (at + size > span) return;
        f.push_back({head + at, size, false, fuzz::load_field(stream, at, size)});
    };
    masked(0x00, 8); // signature input length
    masked(0x0C, 4); // section count
    masked(0x10, 4); // key count
    masked(0x14, 4); // optional header size
    u32 sections = ps_ld32(d.data() + head + 0x0C) ^ static_cast<u32>(fuzz::load_field(stream, 0x0C, 4));
    sections = std::min<u32>(sections, 16);
    for (u32 k = 0; k < sections; k++) {
        const usize s = 0x20 + k * 0x30;
        masked(s + 0x00, 8); // data offset
        masked(s + 0x08, 8); // data size
        for (usize w = 0x10; w < 0x30; w += 4) masked(s + w, 4); // type, program index, hashed, SHA-1 index, encrypted,
                                                                 // key index, IV index, compressed
    }
    return f;
}

// ---- NPD

NpdKeys made_up_npd_keys()
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

// The NPD and EDAT headers' fields, and the blocks' metadata (plain for NPD version 1's compressed files).
std::vector<fuzz::Field> npd_fields(std::span<const u8> d)
{
    std::vector<fuzz::Field> f = {{0x04, 4}, {0x08, 4}, {0x0C, 4}, {0x70, 8}, {0x78, 8}, {0x80, 4}, {0x84, 4}, {0x88, 8}};
    for (usize at = 0x100; at + 0x20 <= std::min<usize>(d.size(), 0x100 + 16 * 0x20); at += 0x20)
        fuzz::append(f, {{at + 0x10, 8}, {at + 0x18, 4}, {at + 0x1C, 4}});
    return f;
}

// Opens an NPD file from memory and reads all of it, then pieces. True when it opened.
bool read_npd(const std::vector<u8>& file, const NpdKeys& keys, const std::optional<Key128>& edata_key,
    const NpdrmKeys& npdrm, fuzz::Rng& rng)
{
    (void)parse_npd_header(file);
    (void)npd_hashes_valid(file, "DATA.EDAT", edata_key.value_or(Key128{}), npdrm);
    auto f = NpdFile::open(
        [&file](u64 offset, void* dst, usize size) {
            if (offset > file.size() || size > file.size() - offset) return false;
            if (size) std::memcpy(dst, file.data() + offset, size);
            return true;
        },
        file.size(), keys, edata_key, npdrm);
    if (!f) return false;
    const u64 size = (*f)->size();
    if (size <= (4u << 20)) {
        std::vector<u8> all(static_cast<usize>(size));
        (void)(*f)->read(0, all.data(), all.size());
    }
    for (int i = 0; i < 4 && size; i++) {
        const u64 at = rng.below(size);
        std::vector<u8> piece(static_cast<usize>(std::min<u64>(size - at, 1 + rng.below(0x900))));
        (void)(*f)->read(at, piece.data(), piece.size());
    }
    u8 past = 0;
    (void)(*f)->read(size, &past, 1);
    return true;
}

// ---- packages

fs::path temp_dir(std::string_view what)
{
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-fuzz-{}-{}", what, static_cast<long>(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& path, std::span<const u8> bytes)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

constexpr std::string_view kContent = "UP0001-NPXX00000_00-0000000000000001";

// A package's header fields, its metadata packets' ids and sizes, and its entry table in plain form (a retail
// package's AES-128-CTR stream as the mask, from the key and the counter block the header holds).
std::vector<fuzz::Field> pkg_fields(std::span<const u8> d, const Key128& key)
{
    std::vector<fuzz::Field> f = {{0x04, 2}, {0x06, 2}, {0x08, 4}, {0x0C, 4}, {0x10, 4}, {0x14, 4}, {0x18, 8},
        {0x20, 8}, {0x28, 8}};
    const u32 meta = ps_ld32(d.data() + 0x08), packets = ps_ld32(d.data() + 0x0C);
    usize at = meta;
    for (u32 i = 0; i < packets && at + 8 <= d.size(); i++) {
        fuzz::append(f, {{at, 4}, {at + 4, 4}, {at + 8, 4}});
        at += 8 + ps_ld32(d.data() + at + 4);
    }
    const usize data = static_cast<usize>(ps_ld64(d.data() + 0x20));
    const u32 count = ps_ld32(d.data() + 0x14);
    const usize table = std::min<usize>(count * 0x20, d.size() - std::min(d.size(), data));
    std::vector<u8> zeros(table), stream(table);
    REQUIRE(crypto::aes_ctr(key, d.data() + 0x70, zeros.data(), table, stream.data()));
    for (usize e = 0; e + 0x20 <= table; e += 0x20) {
        for (const auto [off, size] : {std::pair<usize, u8>{0x00, 4}, {0x04, 4}, {0x08, 8}, {0x10, 8}, {0x18, 4}})
            f.push_back({data + e + off, size, false, fuzz::load_field(stream, e + off, size)});
    }
    return f;
}

} // namespace

TEST_CASE("fuzz: ELF executables, PRX modules and SPU programs, their tables and relocations", "[recomp][fuzz]")
{
    test::PrxOptions vars;
    vars.variables = true;
    const auto add = [](std::vector<fuzz::Sample>& to, std::string name, std::vector<u8> bytes) {
        auto fields = elf_fields(bytes);
        to.push_back({std::move(name), std::move(bytes), std::move(fields)});
    };
    std::vector<fuzz::Sample> samples;
    add(samples, "ppu exec", test::build_ppu_exec().bytes);
    add(samples, "ppu exec without sections", test::build_ppu_exec({false, false, false}).bytes);
    add(samples, "ppu prx", test::build_ppu_prx().bytes);
    add(samples, "ppu prx with variables", test::build_ppu_prx(vars).bytes);
    add(samples, "spu elf", test::build_spu_elf({"job", "edgezlib", 0, true}).bytes);
    fuzz::Tally tally("elf");
    fuzz::run(tally, samples, 1500, [](const std::vector<u8>& input, fuzz::Rng&) { return read_elf(input); });
}

TEST_CASE("fuzz: SELF headers, and SELFs decrypted with made-up keys", "[recomp][fuzz]")
{
    const auto exec = test::build_ppu_exec().bytes;
    const SelfKey key = app_key(0x0A);
    const std::vector<SelfKey> keys{app_key(0x09), key};
    test::SelfOptions debug;
    debug.debug = true;
    test::SelfEncryption e;
    e.key = key;
    e.revision = 0x0A;
    std::vector<fuzz::Sample> samples;
    for (auto [name, bytes] : {std::pair<std::string, std::vector<u8>>{"self", test::wrap_in_self(exec)},
             {"debug self", test::wrap_in_self(exec, debug)}, {"sprx", test::wrap_in_self(test::build_ppu_prx().bytes)}}) {
        auto fields = self_fields(bytes);
        samples.push_back({name, std::move(bytes), std::move(fields)});
    }
    std::vector<u8> encrypted = test::encrypt_self(exec, e);
    if (!encrypted.empty()) { // a build without zlib makes none
        auto fields = self_fields(encrypted);
        fuzz::append(fields, self_metadata_fields(encrypted, key));
        samples.push_back({"encrypted self", std::move(encrypted), std::move(fields)});
    }
    const NpdrmLicence licence{.klicensee = made_up<16>(0x77), .rap = {}, .keys = nullptr};
    // The headers of all of them; the ELF out of the two that decrypt (the others' metadata is filler).
    fuzz::Tally headers("self headers"), decrypted("self decrypt");
    const auto decrypt = [&](const std::vector<u8>& input) {
        auto elf = decrypt_self(input, keys, &licence);
        if (elf) (void)read_elf(*elf);
        return static_cast<bool>(elf);
    };
    fuzz::run(headers, samples, 600, [&](const std::vector<u8>& input, fuzz::Rng&) {
        auto info = parse_self(input);
        (void)decrypt(input);
        return static_cast<bool>(info);
    });
    std::erase_if(samples, [](const fuzz::Sample& x) { return x.name != "debug self" && x.name != "encrypted self"; });
    fuzz::run(decrypted, samples, 1200, [&](const std::vector<u8>& input, fuzz::Rng&) { return decrypt(input); });
}

TEST_CASE("fuzz: NPD files, SDATA and EDATA, every block layout", "[recomp][fuzz]")
{
    const NpdKeys keys = made_up_npd_keys();
    const NpdrmKeys npdrm = made_up_npdrm_keys();
    const std::vector<u8> plain = pattern(0x2345, 9);
    struct Variant {
        std::string name;
        test::NpdOptions options;
    };
    std::vector<Variant> variants;
    variants.push_back({"sdata", {}});
    variants.push_back({"sdata compressed", {.extra_flags = kNpdCompressedFlag}});
    variants.push_back({"sdata v1 compressed (plain metadata)", {.version = 1, .extra_flags = kNpdCompressedFlag}});
    variants.push_back({"sdata hash before each block", {.extra_flags = kNpdFlag20}});
    variants.push_back({"sdata hmac", {.extra_flags = kNpdFlag10}});
    variants.push_back({"sdata plain blocks", {.extra_flags = kNpdFlag02}});
    variants.push_back({"sdata debug data compressed", {.version = 1, .extra_flags = kNpdDebugDataFlag | kNpdCompressedFlag}});
    variants.push_back({"sdata v4 encrypted key", {.version = 4, .extra_flags = kNpdEncryptedKeyFlag}});
    test::NpdOptions edata;
    edata.edata = true;
    edata.key = made_up<16>(0x77);
    edata.file_name = "DATA.EDAT";
    edata.npdrm = &npdrm;
    variants.push_back({"edata free", edata});
    std::vector<fuzz::Sample> samples;
    for (const Variant& v : variants) {
        std::vector<u8> bytes = test::build_npd(plain, keys, v.options);
        auto fields = npd_fields(bytes);
        samples.push_back({v.name, std::move(bytes), std::move(fields)});
    }
    fuzz::Tally tally("npd");
    fuzz::run(tally, samples, 600, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
        const bool edata_file = input.size() > 0x84 && !(ps_ld32(input.data() + 0x80) & kNpdSdatFlag);
        return read_npd(input, keys, edata_file ? std::optional<Key128>(made_up<16>(0x77)) : std::nullopt, npdrm, rng);
    });
}

TEST_CASE("fuzz: NPD's LZ decoder on damaged blocks", "[recomp][fuzz]")
{
    // A block as the decoder takes it: a head byte (0x80 or less: compressed; above: the length and the bytes plain),
    // then the range coder's stream. There is no encoder here: the compressed samples are the first streams of a
    // pattern that the decoder takes whole (what it makes of them does not matter). Each input is decoded into a block
    // of 0x1000 bytes (whether it was taken) and into one of a random size; nothing may be written past either.
    const auto decode = [](const std::vector<u8>& input, u32 size) {
        std::vector<u8> out(size + 16, 0xEE);
        const int n = npd_decompress(out.data(), input.data(), input.size(), size);
        REQUIRE(n <= static_cast<int>(size));
        REQUIRE(std::all_of(out.begin() + size, out.end(), [](u8 b) { return b == 0xEE; }));
        return n;
    };
    std::vector<fuzz::Sample> samples;
    for (u32 i = 0; i < 3; i++) {
        std::vector<u8> block;
        for (u32 seed = 30 + 100 * i; block.empty(); seed++) {
            std::vector<u8> candidate = pattern(64 + 200 * i, seed);
            candidate[0] = static_cast<u8>(i * 0x20);
            if (decode(candidate, 0x1000) > 0) block = std::move(candidate);
        }
        samples.push_back({std::format("block {}", i), std::move(block), {{1, 4}}});
    }
    std::vector<u8> plain = pattern(300, 33);
    plain[0] = 0x81;
    ps_st32(plain.data() + 1, static_cast<u32>(plain.size() - 5));
    samples.push_back({"plain block", std::move(plain), {{1, 4}}});
    fuzz::Tally tally("npd lz");
    fuzz::run(tally, samples, 1500, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
        (void)decode(input, static_cast<u32>(1 + rng.below(0x1000)));
        return decode(input, 0x1000) >= 0;
    });
}

TEST_CASE("fuzz: PS3 packages installed into a temporary dev_hdd0", "[recomp][fuzz]")
{
    PkgKeys keys;
    keys.ps3 = made_up<16>(0x31);
    keys.idu = made_up<16>(0x47);
    keys.present = true;
    const NpdKeys npd_keys = made_up_npd_keys();
    test::PkgSpec game;
    game.content_id = std::string(kContent);
    game.key = keys.ps3;
    game.counter = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
    const auto sfo = test::build_param_sfo({{"CATEGORY", "HG"}, {"TITLE_ID", "NPXX00000"}, {"APP_VER", "01.00"},
        {"TITLE", "A synthetic game"}});
    game.items = {{"PARAM.SFO", kPkgEntryRegular, sfo}, {"USRDIR", kPkgEntryFolder, {}},
        {"USRDIR/EBOOT.BIN", kPkgEntryNpdrm, pattern(0x1234, 1)}, {"USRDIR/data", kPkgEntryFolder, {}},
        {"USRDIR/data/a.bin", kPkgEntryRegular, pattern(3000, 2)},
        {"USRDIR/data/b.sdat", kPkgEntrySdat, test::build_sdata(pattern(0x900, 3), npd_keys)},
        {"ICON0.PNG", kPkgEntryRegular | kPkgEntryOverwrite, pattern(1000, 4)}};
    test::PkgSpec update = game;
    update.package_flags = kPkgFlagPatch;
    update.items = {{"PARAM.SFO", kPkgEntryRegular | kPkgEntryOverwrite,
        test::build_param_sfo({{"CATEGORY", "GD"}, {"TITLE_ID", "NPXX00000"}, {"APP_VER", "01.01"}})}};
    std::vector<fuzz::Sample> samples;
    for (const auto& [name, spec] : {std::pair<std::string, const test::PkgSpec*>{"game", &game}, {"update", &update}}) {
        std::vector<u8> bytes = test::build_pkg(*spec);
        auto fields = pkg_fields(bytes, keys.ps3);
        samples.push_back({name, std::move(bytes), std::move(fields)});
    }
    const fs::path root = temp_dir("pkg");
    fuzz::Tally tally("pkg");
    u64 n = 0;
    fuzz::run(tally, samples, 300, [&](const std::vector<u8>& input, fuzz::Rng&) {
        const fs::path dir = root / std::to_string(n++ % 8);
        fs::remove_all(dir);
        fs::create_directories(dir / "hdd0");
        write_file(dir / "game.pkg", input);
        (void)read_pkg_info((dir / "game.pkg").string());
        auto r = install_pkg((dir / "game.pkg").string(), (dir / "hdd0").string(), {}, keys, npd_keys);
        return static_cast<bool>(r);
    });
    fs::remove_all(root);
}
