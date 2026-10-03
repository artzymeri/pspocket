// Malformed input must produce errors or warnings, never crashes, hangs or undefined behaviour. Run this suite
// under -DPSPOCKET_SANITIZE=ON (ASan + UBSan) as well: the mutations below are only useful with the sanitizers on.

#include <catch_amalgamated.hpp>

#include <random>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/inventory.hpp"
#include "pspocket/recomp/manifest.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/toml.hpp"
#include "synthetic.hpp"

using namespace pspocket;
using namespace pspocket::recomp;

namespace {

struct Sample {
    const char* name;
    std::vector<u8> bytes;
};

std::vector<Sample> samples()
{
    const auto exec = test::build_ppu_exec();
    const auto prx = test::build_ppu_prx();
    test::SelfOptions debug;
    debug.debug = true;
    return {
        {"ppu exec", exec.bytes},
        {"ppu exec without sections", test::build_ppu_exec({false, false, false}).bytes},
        {"ppu prx", prx.bytes},
        {"spu elf", test::build_spu_elf({"job", "edgezlib", 0, true}).bytes},
        {"self", test::wrap_in_self(exec.bytes)},
        {"debug self", test::wrap_in_self(exec.bytes, debug)},
        {"sprx", test::wrap_in_self(prx.bytes)},
    };
}

// Invariants every report must satisfy, whatever the input.
void check_report(const FileReport& r, std::span<const u8> data)
{
    REQUIRE(r.size == data.size());
    for (const auto& p : r.spu_programs) {
        REQUIRE(p.image.offset <= data.size());
        REQUIRE(p.image.size <= data.size() - p.image.offset);
    }
    for (const auto& s : r.strings) REQUIRE(s.offset < data.size());
    if (r.ppu) {
        for (const auto& lib : r.ppu->imports) REQUIRE(lib.name.size() <= 255);
    }
}

void inspect(std::span<const u8> data)
{
    InspectOptions options;
    options.collect_strings = true;
    options.min_string_length = 4;
    const FileReport r = inspect_bytes("fuzz", data, options);
    check_report(r, data);
}

// Offsets of 32-bit header and table fields worth smashing: the ELF header, program and section header tables,
// and the PPU library tables (where pointers and counts live).
std::vector<usize> interesting_offsets(std::span<const u8> d)
{
    std::vector<usize> out;
    for (usize i = 0; i + 4 <= std::min<usize>(d.size(), 0x300); i += 4) out.push_back(i);
    if (d.size() >= 64 && d[0] == 0x7F && d[4] == 2) {
        const u64 shoff = ps_ld64(d.data() + 40);
        for (u64 i = 0; i < 0x200 && shoff + i + 4 <= d.size(); i += 4) out.push_back(static_cast<usize>(shoff + i));
        // Library tables and descriptors of the synthetic executable (data segment at file 0x4000).
        for (usize i = 0x4000; i + 4 <= std::min<usize>(d.size(), 0x4900); i += 4) out.push_back(i);
    }
    return out;
}

} // namespace

TEST_CASE("truncated inputs are handled")
{
    for (const auto& s : samples()) {
        INFO(s.name);
        const std::span<const u8> all(s.bytes);
        // Every length through the headers, then a prime stride.
        for (usize n = 0; n <= all.size(); n += (n < 0x400 ? 1 : 61)) inspect(all.first(n));
        inspect(all);
    }
}

TEST_CASE("randomly mutated inputs are handled")
{
    std::mt19937 rng(0x5EED);
    const u32 smash_values[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFF0, 0x10000, 0x40, 0x2C};
    for (const auto& s : samples()) {
        INFO(s.name);
        const auto fields = interesting_offsets(s.bytes);
        for (int iter = 0; iter < 400; iter++) {
            std::vector<u8> d = s.bytes;
            const int kind = static_cast<int>(rng() % 3);
            if (kind == 0) {
                // Random byte flips anywhere.
                const int flips = 1 + static_cast<int>(rng() % 16);
                for (int k = 0; k < flips; k++) d[rng() % d.size()] ^= static_cast<u8>(1 + rng() % 255);
            } else if (kind == 1 && !fields.empty()) {
                // Extreme values in header and table fields.
                const int smashes = 1 + static_cast<int>(rng() % 4);
                for (int k = 0; k < smashes; k++) {
                    const usize at = fields[rng() % fields.size()];
                    ps_st32(d.data() + at, smash_values[rng() % std::size(smash_values)]);
                }
            } else {
                // Random runs of garbage.
                const usize at = rng() % d.size();
                const usize len = std::min<usize>(d.size() - at, 1 + rng() % 64);
                for (usize k = 0; k < len; k++) d[at + k] = static_cast<u8>(rng());
            }
            inspect(d);
        }
    }
}

TEST_CASE("ELF headers with hostile counts and offsets")
{
    std::mt19937 rng(42);
    for (int iter = 0; iter < 300; iter++) {
        std::vector<u8> d(64 + rng() % 4096);
        for (auto& b : d) b = static_cast<u8>(rng());
        d[0] = 0x7F;
        d[1] = 'E';
        d[2] = 'L';
        d[3] = 'F';
        const bool is64 = rng() % 2 != 0;
        d[4] = is64 ? 2 : 1;
        d[5] = 2;
        ps_st16(d.data() + 18, is64 ? 21 : 23);
        if (is64) {
            ps_st16(d.data() + 54, 56);
            ps_st16(d.data() + 58, 64);
            ps_st64(d.data() + 32, rng() % 2 ? 64 : rng()); // phoff
            ps_st16(d.data() + 56, static_cast<u16>(rng()));   // phnum
        } else {
            ps_st16(d.data() + 42, 32);
            ps_st16(d.data() + 46, 40);
            ps_st32(d.data() + 28, rng() % 2 ? 52 : static_cast<u32>(rng()));
        }
        inspect(d);
    }
    // A PPU image whose segments claim 4 GB of memory and a huge .opd section.
    auto x = test::build_ppu_exec().bytes;
    ps_st64(x.data() + 0x40 + 56 + 40, 0xFFFFFFFFull - 0x20000); // LOAD1 p_memsz: the rest of the address space
    inspect(x);
}

TEST_CASE("TOML, manifest and names parsers survive garbage")
{
    const std::string manifest = R"(title_id = "BLES00229"
title = "x"
app_version = "01.08"
iso_sha256 = ""
[[files]]
path = "a/b"
sha256 = ""
[[patches]]
addr = 0x10000
bytes = "60000000"
why = "test"
[[spu_programs]]
sha1 = "0000000000000000000000000000000000000000"
name = "n"
action = "lift"
)";
    std::mt19937 rng(7);
    const char alphabet[] = "[]{}=\"'#.,_-+0x19aZ \t\n\r\\u";
    for (int iter = 0; iter < 2000; iter++) {
        std::string t = manifest;
        const int edits = 1 + static_cast<int>(rng() % 6);
        for (int k = 0; k < edits; k++) {
            const usize at = rng() % t.size();
            switch (rng() % 3) {
            case 0: t[at] = alphabet[rng() % (sizeof(alphabet) - 1)]; break;
            case 1: t.insert(t.begin() + static_cast<std::ptrdiff_t>(at), alphabet[rng() % (sizeof(alphabet) - 1)]); break;
            default: t.erase(at, 1 + rng() % 3); break;
            }
            if (t.empty()) t = "x";
        }
        (void)toml::parse(t);
        (void)parse_manifest(t);
        NidDb db;
        (void)db.load_text(t);
        SyscallTable st;
        (void)st.load_text(t);
    }
    // Random bytes, including NULs and high bytes.
    for (int iter = 0; iter < 500; iter++) {
        std::string t(rng() % 256, '\0');
        for (auto& c : t) c = static_cast<char>(rng());
        (void)toml::parse(t);
        NidDb db;
        (void)db.load_text(t);
    }
    SUCCEED();
}
