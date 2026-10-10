// Mutation fuzzing of the HLE side's parsers of a game's files (D-603, tests/common/mutation_fuzz.hpp): PARAM.SFO as
// cellGame and cellSaveData read it, a save's PARAM.SFO through cellSaveDataGetListItem, the trophy pack, its
// configuration (XML) and the user's TROPUSR.DAT, cellL10n's converters on damaged text of every encoding, the
// loader's tables (an executable's segments, imports and exports; a PRX module's relocations), and damaged TrueType
// fonts through cell_FreeType2, the FreeType cellFont opens them with (ttf_writer.hpp). Synthetic inputs only. Longer:
// PSPOCKET_FUZZ_ITERS=<n> build/host/tests/hle/test_hle "[fuzz]".

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "hle_fuzz_util.hpp"
#include "psf.hpp"          // runtime/hle/src (internal): the PARAM.SFO reader and writer
#include "trophy_files.hpp" // runtime/hle/src (internal)
#include "trophy_test_util.hpp"
#include "ttf_writer.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/process.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/nid.hpp"
#include "synthetic.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::fuzzing;
namespace psf = pspocket::hle::psf;
namespace trophy = pspocket::hle::trophy;
namespace fsys = std::filesystem;

namespace {

// A PSF's header and index fields, little-endian.
std::vector<fuzz::Field> psf_fields(const std::vector<u8>& sfo)
{
    std::vector<fuzz::Field> f = {{4, 4, true}, {8, 4, true}, {12, 4, true}, {16, 4, true}};
    const u32 count = sfo.size() >= 20 ? static_cast<u32>(fuzz::load_field(sfo, 16, 4, true)) : 0;
    for (u32 i = 0; i < count && 20 + (i + 1) * 16 <= sfo.size(); i++) {
        const usize e = 20 + usize{i} * 16;
        fuzz::append(f, {{e, 2, true}, {e + 2, 2, true}, {e + 4, 4, true}, {e + 8, 4, true}, {e + 12, 4, true}});
    }
    return f;
}

// A save's PARAM.SFO as cellSaveData writes it (D-183): its strings, integers and RPCS3's own entries.
std::vector<u8> save_sfo()
{
    psf::Registry sfo;
    sfo["CATEGORY"] = psf::string(4, "SD");
    sfo["TITLE"] = psf::string(128, "A synthetic game");
    sfo["SUB_TITLE"] = psf::string(128, "Profile");
    sfo["DETAIL"] = psf::string(1024, "A detail");
    sfo["SAVEDATA_DIRECTORY"] = psf::string(32, "BLES01807-PROFILE");
    sfo["SAVEDATA_LIST_PARAM"] = psf::string(8, "PROF");
    sfo["PARAMS"] = psf::string(1024, "");
    sfo["PARAMS2"] = psf::string(12, "");
    sfo["ATTRIBUTE"] = psf::Entry{psf::Format::Integer, 4, {}, 0};
    sfo["PARENTAL_LEVEL"] = psf::Entry{psf::Format::Integer, 4, {}, 3};
    sfo["*PROF.BIN"] = psf::Entry{psf::Format::Integer, 4, {}, 1};
    sfo["RPCS3_BLIST"] = psf::string(12, "PROF.BIN");
    return psf::save(sfo);
}

// A game's PARAM.SFO with every kind of entry: strings, a char array and integers.
std::vector<u8> game_sfo()
{
    psf::Registry sfo;
    sfo["APP_VER"] = psf::string(8, "01.00");
    sfo["ATTRIBUTE"] = psf::Entry{psf::Format::Integer, 4, {}, 0x20};
    sfo["CATEGORY"] = psf::string(4, "DG");
    sfo["LICENSE"] = psf::Entry{psf::Format::Array, 512, "A licence text", 0};
    sfo["PARENTAL_LEVEL"] = psf::Entry{psf::Format::Integer, 4, {}, 5};
    sfo["PS3_SYSTEM_VER"] = psf::string(8, "03.4000");
    sfo["TITLE"] = psf::string(128, "A synthetic game");
    sfo["TITLE_ID"] = psf::string(16, "BLES01807");
    sfo["VERSION"] = psf::string(8, "01.00");
    return psf::save(sfo);
}

// ---- cellL10n

namespace code {
enum : u32 { UTF8 = 0, UTF16 = 1, UTF32 = 2, UCS2 = 3, SJIS = 24, MSJIS = 37, kCount = 52 };
}

// Text of every script the converters know: Latin with accents, kana and kanji, hangul, simplified and traditional
// Chinese, symbols, a character outside the BMP.
constexpr std::string_view kText = "Caf\xC3\xA9 na\xC3\xAFve \xE3\x81\x82\xE3\x82\xA2\xE6\xBC\xA2\xE5\xAD\x97 "
                                   "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 \xE4\xB8\xAD\xE6\x96\x87 \xE7\xB9\x81\xE9\xAB\x94 "
                                   "\xE2\x82\xAC\xE2\x84\xA2 \xF0\x9F\x98\x80 ~^\\|";

struct L10n {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    const hle::Function* convert_str = hle::registry().find("cellL10n", recomp::nid_of("l10n_convert_str"));
    const hle::Function* convert = hle::registry().find("cellL10n", recomp::nid_of("l10n_convert"));
    const hle::Function* get_converter = hle::registry().find("cellL10n", recomp::nid_of("l10n_get_converter"));
    static constexpr u32 kLens = kData + 0x8000; // the two length words

    L10n()
    {
        add_guarded_areas(f);
        REQUIRE(convert_str);
        REQUIRE(convert);
        REQUIRE(get_converter);
    }

    s32 converter(u32 s, u32 d)
    {
        t.ctx.gpr[3] = s;
        t.ctx.gpr[4] = d;
        get_converter->fn(t.ctx, b);
        return static_cast<s32>(t.ctx.gpr[3]);
    }

    // l10n_convert_str over `in` (at the end of the input area, `src_len` code units in *src_len) into room for `cap`
    // code units of the destination's encoding (at the end of the output area, *dst_len `cap`; or NULL): the answer,
    // and the bytes written.
    u32 run(s32 cd, std::span<const u8> in, u32 src_len, u32 cap, bool null_dst, std::vector<u8>* out = nullptr)
    {
        const u32 unit = unit_of(static_cast<u32>(cd) & 0xFFFF);
        const u32 src = place_input(b, in);
        const u32 dst = null_dst ? 0 : place_output(b, cap * unit);
        ps_write32(b, kLens, src_len);
        ps_write32(b, kLens + 4, cap);
        const u64 regs[] = {static_cast<u64>(static_cast<u32>(cd)), src, kLens, dst, kLens + 4};
        for (usize i = 0; i < std::size(regs); i++) t.ctx.gpr[3 + i] = regs[i];
        convert_str->fn(t.ctx, b);
        const u32 r = static_cast<u32>(t.ctx.gpr[3]);
        if (out && !null_dst) out->assign(b + dst, b + dst + unit * std::min(cap, ps_read32(b, kLens + 4)));
        return r;
    }

    // Code units of an encoding: what *src_len and *dst_len count.
    static u32 unit_of(u32 code_)
    {
        if (code_ == code::UTF16 || code_ == code::UCS2) return 2;
        if (code_ == code::UTF32) return 4;
        return 1;
    }
};

} // namespace

TEST_CASE("fuzz: PARAM.SFO as the HLE libraries read it", "[hle][fuzz]")
{
    std::vector<fuzz::Sample> samples;
    for (auto [name, bytes] : {std::pair<std::string, std::vector<u8>>{"game sfo", game_sfo()}, {"save sfo", save_sfo()},
             {"package sfo", pspocket::test::build_param_sfo({{"CATEGORY", "GD"}, {"TITLE_ID", "NPXX00000"},
                                 {"APP_VER", "01.01"}, {"TARGET_APP_VER", "01.00"}})}}) {
        auto fields = psf_fields(bytes);
        samples.push_back({name, std::move(bytes), std::move(fields)});
    }
    fuzz::Tally tally("param.sfo (hle)");
    fuzz::run(tally, samples, 3000, [](const std::vector<u8>& input, fuzz::Rng&) {
        const std::optional<psf::Registry> sfo = psf::parse(input);
        if (!sfo) return false;
        (void)psf::get_string(*sfo, "TITLE");
        (void)psf::get_string(*sfo, "CATEGORY");
        // What a game's save rewrites: the same entries saved and read again.
        const std::vector<u8> again = psf::save(*sfo);
        (void)psf::parse(again);
        return true;
    });
}

TEST_CASE("fuzz: a save's PARAM.SFO through cellSaveDataGetListItem", "[hle][fuzz]")
{
    const TempDir root("savedata");
    const fsys::path dir = root.path / "home" / "00000001" / "savedata" / "BLES01807-PROFILE";
    fsys::create_directories(dir);
    write_host_file(dir / "PROF.BIN", bytes_of(std::string(2000, 'p')));
    hle::ProcessConfig config;
    config.game.title_id = "BLES01807";
    Fixture f(config);
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", root.path.string(), false));
    GuestThread a(f, 0, "main");
    u8* b = f.base();
    constexpr u32 kName = kData + 0x43000, kDir = kData + 0x44000, kParamOut = kData + 0x45000, kBind = kData + 0x46000,
                  kSize = kBind + 4;
    std::memcpy(b + kName, "BLES01807-PROFILE", 18);
    const std::vector<u8> sfo = save_sfo();
    fuzz::Tally tally("savedata list item");
    fuzz::run(tally, {{"save sfo", sfo, psf_fields(sfo)}}, 1500, [&](const std::vector<u8>& input, fuzz::Rng&) {
        write_host_file(dir / "PARAM.SFO", input);
        std::memset(b + kDir, 0xAA, 0x100);
        std::memset(b + kParamOut, 0xAA, 0x700);
        return a.call("cellSaveDataGetListItem", kName, kDir, kParamOut, kBind, kSize) == 0;
    });
}

TEST_CASE("fuzz: trophy packs, their configuration and the user's TROPUSR.DAT", "[hle][fuzz]")
{
    const TempDir dir("trophy");
    // The pack: as made (version 3, its SHA-1 checked) and version 1 (none), and half the version 3 inputs with the
    // SHA-1 made right again after the damage, so the parser past the check is reached too.
    const std::vector<u8> v3 = make_trp(sample_trophy_files(), 3), v1 = make_trp(sample_trophy_files(), 1);
    const auto trp_fields = [](const std::vector<u8>& t) {
        std::vector<fuzz::Field> f = {{4, 4}, {8, 8}, {16, 4}, {20, 4}, {24, 4}};
        for (usize e = 0x40; e + 0x40 <= std::min<usize>(t.size(), 0x40 + 9 * 0x40); e += 0x40)
            fuzz::append(f, {{e + 31, 1}, {e + 32, 8}, {e + 40, 8}, {e + 48, 4}});
        return f;
    };
    u64 n = 0;
    fuzz::Tally trp("trophy pack");
    fuzz::run(trp, {{"trp v3", v3, trp_fields(v3)}, {"trp v1", v1, trp_fields(v1)}}, 1200,
        [&](const std::vector<u8>& input, fuzz::Rng& rng) {
            std::vector<u8> file = input;
            if (file.size() >= 0x30 && fuzz::load_field(file, 4, 4) >= 2 && rng.one_in(2)) {
                std::memset(file.data() + 0x1C, 0, 20);
                const recomp::Sha1Digest sha1 = recomp::Sha1::hash(file);
                std::memcpy(file.data() + 0x1C, sha1.data(), sha1.size());
            }
            std::string why;
            std::optional<trophy::TrpArchive> t = trophy::TrpArchive::parse(std::move(file), &why);
            if (!t) return false;
            (void)t->required_space();
            (void)t->contains("TROPCONF.SFM");
            t->rename("TROP_01.SFM", "TROPCONF.SFM");
            const fsys::path dest = dir.path / "installed" / std::to_string(n++ % 4);
            (void)t->install(dest, &why);
            return true;
        });

    // The configuration: XML damaged anywhere, read as RPCS3's pugixml reads it, then walked as sceNpTrophy walks it.
    fuzz::Tally xml("trophy xml");
    std::vector<fuzz::Sample> confs = {{"english", bytes_of(kTropEnglish), {}}, {"default", bytes_of(kTropDefault), {}},
        {"conf", bytes_of(kTropConf), {}}};
    fuzz::run(xml, confs, 2000, [](const std::vector<u8>& input, fuzz::Rng&) {
        const auto doc = trophy::parse_xml(std::string_view(reinterpret_cast<const char*>(input.data()), input.size()));
        if (!doc) return false;
        const trophy::XmlNode& root = trophy::trophy_root(*doc);
        for (const trophy::XmlNode& node : root.children) {
            (void)trophy::atoi(node.attribute("id"));
            (void)node.attribute("ttype");
            (void)trophy::atoi(node.attribute("pid"));
            (void)node.content();
            for (const trophy::XmlNode& child : node.children) (void)child.content();
        }
        return true;
    });

    // TROPUSR.DAT: generated from the configuration once, then damaged and read back (an invalid one is made again).
    const fsys::path conf = dir.path / "TROPCONF.SFM", usr = dir.path / "TROPUSR.DAT";
    write_host_file(conf, bytes_of(kTropEnglish));
    {
        trophy::TropUsr made;
        REQUIRE(made.load(usr, conf).success);
        REQUIRE(made.unlock(1, 111, 222));
        REQUIRE(made.save(usr));
    }
    const std::vector<u8> tropusr = *trophy::read_host_file(usr);
    std::vector<fuzz::Field> usr_fields = {{8, 4}};
    for (usize t = 0x30; t < 0x70; t += 0x20) fuzz::append(usr_fields, {{t, 4}, {t + 4, 4}, {t + 8, 4}, {t + 12, 4}, {t + 16, 8}});
    for (usize e = 0x70; e + 0x18 <= tropusr.size(); e += 0x10) fuzz::append(usr_fields, {{e, 4}, {e + 4, 4}, {e + 8, 4}});
    fuzz::Tally usr_tally("tropusr.dat");
    fuzz::run(usr_tally, {{"tropusr", tropusr, usr_fields}}, 1500, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
        write_host_file(usr, input);
        trophy::TropUsr u;
        const trophy::TropUsr::LoadResult r = u.load(usr, conf);
        if (!r.success) return false;
        const u32 id = static_cast<u32>(rng.below(6));
        (void)u.grade(id);
        (void)u.unlocked(id);
        (void)u.timestamp(id);
        if (u.unlock(id, 1, 2)) (void)u.unlocked_platinum_id(id, conf);
        (void)u.unlocked_count();
        return !r.discarded_existing;
    });
}

TEST_CASE("fuzz: cellL10n's converters on damaged text of every encoding", "[hle][fuzz]")
{
    L10n l;
    const std::vector<u8> text = bytes_of(kText);
    fuzz::Tally tally("l10n");
    u32 sources = 0;
    for (u32 s = 0; s < code::kCount; s++) {
        // The sample: the text in encoding s, made by the library itself from UTF-8 (what it cannot hold dropped).
        std::vector<u8> sample;
        if (s == code::UTF8) {
            sample = text;
        } else {
            const s32 to = l.converter(code::UTF8, s);
            if (to < 0) continue;
            std::vector<u8> made;
            std::vector<u8> piece;
            // Character by character, so one the encoding lacks only drops itself.
            for (usize i = 0; i < text.size();) {
                usize n = 1;
                while (i + n < text.size() && (text[i + n] & 0xC0) == 0x80) n++;
                if (l.run(to, std::span<const u8>(text).subspan(i, n), static_cast<u32>(n), 64, false, &piece) == 0)
                    made.insert(made.end(), piece.begin(), piece.end());
                i += n;
            }
            if (made.empty()) continue;
            sample = std::move(made);
        }
        // Every converter from s; those that take the sample whole (the others lack one of its characters) say whether
        // a damaged input was taken.
        const u32 unit = L10n::unit_of(s);
        const auto room = [](const std::vector<u8>& in) { return static_cast<u32>(4 * in.size() + 16); };
        std::vector<std::pair<s32, bool>> targets;
        for (u32 d = 0; d < code::kCount; d++)
            if (const s32 cd = l.converter(s, d); cd >= 0)
                targets.push_back({cd, l.run(cd, sample, static_cast<u32>(sample.size() / unit), room(sample), false) == 0});
        if (std::none_of(targets.begin(), targets.end(), [](const auto& t) { return t.second; })) continue;
        sources++;
        fuzz::run(tally, {{std::format("code {}", s), sample, {}}}, 120, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
            const u32 units = static_cast<u32>(input.size() / unit);
            bool ok = true;
            for (const auto& [cd, takes_sample] : targets) {
                // The whole input with room enough; then the length the game gives: the input's, or less; the
                // destination's room: plenty, or very little; and a size query (no destination).
                const bool whole = l.run(cd, input, units, room(input), false) == 0;
                if (takes_sample) ok &= whole;
                const u32 src_len = rng.one_in(4) ? static_cast<u32>(rng.below(units + 1)) : units;
                const u32 cap = rng.one_in(3) ? static_cast<u32>(rng.below(16)) : room(input);
                (void)l.run(cd, input, src_len, cap, false);
                (void)l.run(cd, input, src_len, 0, true);
            }
            // l10n_convert: one character from the start, into a few bytes.
            if (!input.empty()) {
                const u32 src = place_input(l.b, input);
                const u32 dst = place_output(l.b, 8);
                ps_write32(l.b, L10n::kLens, 8);
                l.t.ctx.gpr[3] = static_cast<u32>(targets[rng.below(targets.size())].first);
                l.t.ctx.gpr[4] = src;
                l.t.ctx.gpr[5] = dst;
                l.t.ctx.gpr[6] = L10n::kLens;
                l.convert->fn(l.t.ctx, l.b);
            }
            return ok;
        });
    }
    CHECK(sources >= 30); // the Unicode forms, the CJK encodings, the code pages
}

TEST_CASE("fuzz: the loader's tables: an executable's segments, imports and exports; a PRX's relocations",
    "[hle][fuzz]")
{
    const auto exec = pspocket::test::build_ppu_exec();
    pspocket::test::PrxOptions vars;
    vars.variables = true;
    const auto prx = pspocket::test::build_ppu_prx(vars);
    // The fields: the program headers, and every word of the module tables (the exec's data segment at file 0x4000;
    // the PRX's segments and its relocation table).
    const auto table_fields = [](const std::vector<u8>& d, usize from, usize to) {
        std::vector<fuzz::Field> f = fuzz::fields(0x40, 56 * 6 / 4);
        fuzz::append(f, fuzz::fields(from, (std::min(to, d.size()) - from) / 4));
        return f;
    };
    fuzz::Tally exe_tally("loader exec");
    fuzz::run(exe_tally, {{"ppu exec", exec.bytes, table_fields(exec.bytes, 0x4000, 0x4900)}}, 400,
        [](const std::vector<u8>& input, fuzz::Rng&) {
            auto exe = loader::Executable::parse(input);
            if (!exe) return false;
            (void)(*exe)->sdk_version();
            (void)(*exe)->primary_stack_size();
            GuestMemory memory;
            REQUIRE(memory.reserve());
            lv2::Config cfg = (*exe)->kernel_config();
            auto kernel = std::make_unique<lv2::Kernel>(memory, cfg);
            kernel->install();
            auto hle = std::make_unique<hle::HleProcess>(*kernel, PS_MEM_MAIN_BASE, cfg.main_area_end);
            hle->install();
            loader::LoadReport report;
            const bool loaded = loader::load(**exe, *kernel, *hle, &report) == lv2::CELL_OK;
            hle.reset();
            kernel.reset();
            return loaded;
        });
    fuzz::Tally prx_tally("loader prx");
    fuzz::run(prx_tally, {{"ppu prx", prx.bytes, table_fields(prx.bytes, 0x100, prx.bytes.size())}}, 400,
        [](const std::vector<u8>& input, fuzz::Rng&) {
            Fixture f;
            u32 next = 0x00ABC000;
            loader::PrxModule m;
            std::string error;
            return loader::load_prx(input, *f.hle, [&](std::string_view, u32) { return next += 8; }, &m, &error,
                [&](std::string_view, u32) { return next += 8; });
        });
}

namespace {

// The fields of a TrueType font: each table record's offset and length, head's units and loca format, maxp's glyph
// count, hhea's metrics count, the cmap's subtable offset and its format 4 header, the first glyphs' headers.
std::vector<fuzz::Field> ttf_fields(const std::vector<u8>& d)
{
    std::vector<fuzz::Field> f = {{4, 2}};
    const u32 tables = static_cast<u32>(fuzz::load_field(d, 4, 2));
    for (u32 i = 0; i < tables && 12 + (i + 1) * 16 <= d.size(); i++) {
        const usize rec = 12 + usize{i} * 16;
        fuzz::append(f, {{rec + 8, 4}, {rec + 12, 4}});
        const std::string tag(reinterpret_cast<const char*>(d.data() + rec), 4);
        const usize at = static_cast<usize>(fuzz::load_field(d, rec + 8, 4));
        if (tag == "head") fuzz::append(f, {{at + 18, 2}, {at + 50, 2}, {at + 36, 2}, {at + 40, 2}});
        if (tag == "maxp") fuzz::append(f, {{at + 4, 2}, {at + 6, 2}, {at + 8, 2}, {at + 28, 2}, {at + 30, 2}});
        if (tag == "hhea") f.push_back({at + 34, 2});
        if (tag == "cmap") fuzz::append(f, {{at + 2, 2}, {at + 8, 4}, {at + 12, 2}, {at + 14, 2}, {at + 18, 2}});
        if (tag == "loca") fuzz::append(f, fuzz::fields(at, 12, 4));
        if (tag == "glyf") fuzz::append(f, {{at, 2}, {at + 10, 2}, {at + 12, 2}, {at + 14, 2}});
        if (tag == "name") fuzz::append(f, {{at + 2, 2}, {at + 4, 2}, {at + 14, 2}, {at + 16, 2}});
    }
    return f;
}

// A TrueType table's place as the directory names it (the first record of that tag, as FreeType looks it up), or
// nothing.
std::optional<std::pair<u32, u32>> ttf_table(const std::vector<u8>& d, std::string_view tag)
{
    if (d.size() < 12) return std::nullopt;
    const u32 tables = static_cast<u32>(fuzz::load_field(d, 4, 2));
    for (u32 i = 0; i < tables && 12 + (i + 1) * 16 <= d.size(); i++) {
        const usize rec = 12 + usize{i} * 16;
        if (std::string_view(reinterpret_cast<const char*>(d.data() + rec), 4) != tag) continue;
        return std::pair{static_cast<u32>(fuzz::load_field(d, rec + 8, 4)), static_cast<u32>(fuzz::load_field(d, rec + 12, 4))};
    }
    return std::nullopt;
}

// Whether a damaged font's glyph data is the sample's: glyf and loca where they were with the same bytes, and the
// same loca format. FreeType 2.2.1's simple glyph loader keeps a glyph's point count in a short (FT_Outline's
// n_points): an end point of 0x7FFF or more in the glyph's contours wraps it, and TT_Load_Simple_Glyph and
// TT_Process_Simple_Glyph write past the outline (ttgload.c). A third-party bug (D-603), Sony's FreeType's too, left
// as it is: damaged glyph data is opened, not loaded.
bool same_glyph_data(const std::vector<u8>& font, const std::vector<u8>& sample)
{
    for (const std::string_view tag : {"glyf", "loca", "head"}) {
        const auto a = ttf_table(font, tag), b = ttf_table(sample, tag);
        if (!a || !b || *a != *b || u64{a->first} + a->second > font.size()) return false;
        if (tag != "head" && !std::equal(font.begin() + a->first, font.begin() + a->first + a->second, sample.begin() + b->first))
            return false;
    }
    const u32 head = ttf_table(sample, "head")->first;
    return font.size() >= head + 52 && font[head + 50] == sample[head + 50] && font[head + 51] == sample[head + 51];
}

} // namespace

// FreeType over a game's fonts (cellFont opens them through cellFontFT and cell_FreeType2; cellFont itself needs the
// firmware's libfont.sprx for its tables, so the fonts are fuzzed here through cell_FreeType2's own API, as libfontFT
// calls it): a library on an FT_Memory that refuses what its heap cannot hold (a damaged font asks for anything), a
// face opened from memory flush against the guard, sizes, every kind of glyph load and render, the character map
// walked, kerning, names and tables.
TEST_CASE("fuzz: cell_FreeType2 over damaged TrueType fonts in memory", "[hle][fuzz]")
{
    namespace as = pspocket::recomp::ppu::as;
    if (!GuestThread::lookup("FT_New_Library")) SKIP("a build without FreeType (PSPOCKET_FREETYPE_DIR)");
    InterpretMisses interpret; // the FT_Memory's callbacks are guest code
    Fixture f;
    REQUIRE(f.hle->link_internal_functions({"cell_FreeType2"}));
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    add_guarded_areas(f);
    constexpr u32 kHeap = 0x01000000, kHeapSize = 0x00800000;
    REQUIRE(f.hle->main_memory().reserve(kHeap, kHeapSize));
    REQUIRE(f.mem.commit(kHeap, kHeapSize));
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    // FT_MemoryRec {user, alloc, free, realloc} at kMemory, its user {next, end} at kCtl; alloc(memory, size) and
    // realloc(memory, cur, new, block) bump-allocate 16-byte aligned blocks and answer 0 past the heap's end; free does
    // nothing (the heap goes back to empty between fonts).
    constexpr u32 kMemory = kData + 0x80, kCtl = kData + 0x90, kOpds = kData + 0xA0, kCode = kData + 0x100,
                  kOut = kData + 0x1000, kName = kData + 0x1100;
    as::Assembler a(kCode);
    const auto bump = [&](u32 size_reg, as::Assembler::Label fail) { // r5 = ctl; leaves the block in r6
        a.emit(as::lwz(5, 0, 3));
        a.emit(as::lwz(6, 0, 5));
        a.emit(as::lwz(7, 4, 5));
        a.emit(as::subf(8, 6, 7));      // room
        a.emit(as::addi(9, size_reg, 15));
        a.emit(as::rlwinm(9, 9, 0, 0, 27));
        a.emit(as::cmplw(0, 9, size_reg)); // a size so large the rounding wrapped
        a.blt(0, fail);
        a.emit(as::cmplw(0, 9, 8));
        a.bgt(0, fail);
        a.emit(as::add(9, 6, 9));
        a.emit(as::stw(9, 0, 5));
    };
    const u32 alloc_at = a.pc();
    {
        const auto fail = a.new_label();
        bump(4, fail);
        a.emit(as::mr(3, 6));
        a.emit(as::blr());
        a.bind(fail);
        a.emit(as::li(3, 0));
        a.emit(as::blr());
    }
    const u32 free_at = a.pc();
    a.emit(as::blr());
    const u32 realloc_at = a.pc();
    {
        const auto fail = a.new_label(), copied = a.new_label();
        a.emit(as::mr(10, 4)); // cur
        a.emit(as::mr(11, 6)); // the old block
        a.emit(as::mr(12, 5)); // new
        bump(12, fail);
        a.emit(as::cmplw(0, 12, 10)); // min(cur, new) bytes copied
        a.bge(0, copied);
        a.emit(as::mr(10, 12));
        a.bind(copied);
        a.emit(as::li(8, 0));
        const auto loop = a.here(), done = a.new_label();
        a.emit(as::cmplw(0, 8, 10));
        a.bge(0, done);
        a.emit(as::lbzx(9, 11, 8));
        a.emit(as::stbx(9, 6, 8));
        a.emit(as::addi(8, 8, 1));
        a.b(loop);
        a.bind(done);
        a.emit(as::mr(3, 6));
        a.emit(as::blr());
        a.bind(fail);
        a.emit(as::li(3, 0));
        a.emit(as::blr());
    }
    const std::vector<u8> code = a.finish();
    REQUIRE(code.size() <= kOut - kCode);
    std::memcpy(b + kCode, code.data(), code.size());
    const u32 entries[3] = {alloc_at, free_at, realloc_at};
    for (u32 i = 0; i < 3; i++) {
        ps_write32(b, kOpds + 8 * i, entries[i]);
        ps_write32(b, kOpds + 8 * i + 4, 0);
        ps_write32(b, kMemory + 4 + 4 * i, kOpds + 8 * i);
    }
    ps_write32(b, kMemory, kCtl);
    const auto call = [&](const char* name, std::initializer_list<u64> args) {
        const u32 descriptor = f.hle->link_function("cell_FreeType2", recomp::nid_of(name), name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    };
    const auto sx = [](s64 v) { return static_cast<u64>(v); };

    const std::vector<u8> font = pspocket::test::ttf::font();
    fuzz::Tally tally("cell_FreeType2");
    u64 loaded = 0; // fonts whose glyphs were loaded
    fuzz::run(tally, {{"ttf", font, ttf_fields(font)}}, 500, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
        ps_write32(b, kCtl, kHeap); // a fresh heap and library for each font
        ps_write32(b, kCtl + 4, kHeap + kHeapSize);
        ps_write32(b, kOut, 0);
        REQUIRE(call("FT_New_Library", {kMemory, kOut}) == 0);
        const u32 lib = ps_read32(b, kOut);
        REQUIRE(lib != 0);
        call("FT_Add_Default_Modules", {lib});
        const u32 at = place_input(b, input);
        ps_write32(b, kOut, 0);
        const bool opened = call("FT_New_Memory_Face", {lib, at, input.size(), 0, kOut}) == 0 && ps_read32(b, kOut);
        if (opened) {
            const u32 face = ps_read32(b, kOut);
            const u32 slot = ps_read32(b, face + 84); // FT_FaceRec's glyph
            const bool glyphs = same_glyph_data(input, font);
            loaded += glyphs;
            call("FT_Select_Charmap", {face, 0x756E6963}); // 'unic'
            if (rng.one_in(2)) call("FT_Set_Pixel_Sizes", {face, rng.below(64), 1 + rng.below(64)});
            else call("FT_Set_Char_Size", {face, sx(64 * static_cast<s64>(rng.below(48))), sx(64 * static_cast<s64>(1 + rng.below(48))), 72 + rng.below(100), rng.below(150)});
            static constexpr u32 kFlags[] = {0, 1, 2, 4, 8, 0x10, 0x20, 0x80, 0x400, 0x1000, 0x2000, 0x8000, 4 | 0x1000,
                4 | (1u << 16), 4 | (3u << 16), 4 | (4u << 16), 0x10 | 4};
            const u32 codes[] = {'A', 'g', ' ', 0x3042, static_cast<u32>(0x20 + rng.below(0x60)), 0xFFFF, 0x10000};
            for (const u32 c : codes) {
                const u32 index = call("FT_Get_Char_Index", {face, c});
                call("FT_Get_Kerning", {face, index, call("FT_Get_Char_Index", {face, 'V'}), rng.below(3), kOut});
                call("FT_Get_Glyph_Name", {face, index, kName, 1 + rng.below(64)});
                if (!glyphs) continue;
                const u32 flags = kFlags[rng.below(std::size(kFlags))];
                if (call("FT_Load_Char", {face, c, flags}) == 0 && slot && !(flags & 4))
                    call("FT_Render_Glyph", {slot, rng.below(5)});
                call("FT_Load_Glyph", {face, rng.one_in(4) ? rng.below(0x200) : index, kFlags[rng.below(std::size(kFlags))]});
            }
            u32 code = call("FT_Get_First_Char", {face, kOut});
            for (int k = 0; k < 16 && ps_read32(b, kOut); k++) code = call("FT_Get_Next_Char", {face, code, kOut});
            call("FT_Get_Postscript_Name", {face});
            for (u32 tag = 0; tag < 8; tag++) call("FT_Get_Sfnt_Table", {face, tag});
            call("FT_Done_Face", {face});
        }
        call("FT_Done_Library", {lib});
        return opened;
    }, kAreaSize);
    std::printf("fuzz %-24s %8llu with their glyphs loaded\n", tally.name().c_str(), static_cast<unsigned long long>(loaded));
    CHECK(loaded > 0);
}
