// The SPU programs of a game, lifted for its library (route B, stage 4c, D-155): scripts/lift-spu.sh's work (D-076,
// D-081, D-099) in the pipeline, so the phone does it too.

#include "pspocket/prepare/spu.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "pspocket/core/log.hpp"
#include "pspocket/recomp/spu/analyzer.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/recomp/spu/lifter.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/runtime.h"

namespace pspocket::prepare {
namespace {

namespace fs = std::filesystem;
constexpr const char* kChan = "prepare";
constexpr u32 kLsSize = 0x40000;

struct Image {
    std::string name, kind;
    u32 base = 0, size = 0, entry = 0;
    std::set<u32> seeds;
};

bool read_text(const fs::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

u32 parse(const std::string& text) { return static_cast<u32>(std::stoul(text, nullptr, 0)); }

} // namespace

bool lift_spu_images(const std::string& dump_dir, const std::string& out_dir, const std::vector<std::string>& logs,
    SpuLiftReport* report, std::string* error, bool discover)
{
    SpuLiftReport rep;
    std::error_code ec;
    // The images: spu_<hash>_<ls>.txt (kind, guest, base, size, entry, and "seed" lines: where execution entered a
    // module) beside its .bin.
    std::map<std::string, Image> images;
    for (const auto& e : fs::directory_iterator(dump_dir, ec)) {
        const fs::path p = e.path();
        const std::string stem = p.stem().string();
        if (p.extension() != ".txt" || !stem.starts_with("spu_")) continue;
        if (!fs::exists(p.parent_path() / (stem + ".bin"), ec)) continue;
        std::string text;
        if (!read_text(p, text)) continue;
        Image img;
        img.name = stem;
        bool have_entry = false;
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) {
            const size_t sp = line.find(' ');
            if (sp == std::string::npos) continue;
            const std::string key = line.substr(0, sp), value = line.substr(sp + 1);
            try {
                if (key == "kind") img.kind = value;
                else if (key == "base") img.base = parse(value);
                else if (key == "size") img.size = parse(value);
                else if (key == "entry" && !have_entry) {
                    img.entry = parse(value);
                    have_entry = true;
                } else if (key == "seed") img.seeds.insert(parse(value));
            } catch (...) {
            }
        }
        if (have_entry && img.size) images.emplace(img.name, std::move(img));
    }
    // Seeds from runs' logs ("no lifted code at 0xADDR in program NAME") and from the programs an earlier lift made
    // here (every entry of theirs stays a seed: the seeds accumulate).
    const std::regex miss(R"(no lifted code at (0x[0-9a-fA-F]+) in program (spu_[0-9a-f]+_[0-9a-f]+))");
    for (const std::string& log : logs) {
        std::ifstream in(log);
        for (std::string line; std::getline(in, line);) {
            std::smatch m;
            if (std::regex_search(line, m, miss))
                if (auto it = images.find(m[2].str()); it != images.end()) it->second.seeds.insert(parse(m[1].str()));
        }
    }
    // The seeds an earlier lift used (NAME.seeds), not its program's entries: those are every label, also of code only
    // discovery or a constant found, which as seeds would count as code the next time and a job's own writes to it
    // would send the whole job to the interpreter (Assassin's Creed, run 115, D-204).
    for (const auto& d : fs::directory_iterator(out_dir, ec)) {
        if (!d.is_directory()) continue;
        const std::string name = d.path().filename().string();
        auto it = images.find(name);
        if (it == images.end()) continue;
        std::ifstream in(d.path() / (name + ".seeds"));
        for (std::string line; std::getline(in, line);) {
            try {
                if (!line.empty()) it->second.seeds.insert(parse(line));
            } catch (...) {
            }
        }
    }
    // Relocatable images (jobs, modules) are lifted once per hash, at the first copy's base; the other copies' seeds
    // move there by the difference of the bases.
    std::map<std::string, std::string> canonical; // hash part -> first name
    std::set<std::string> merged;
    for (auto& [name, img] : images) {
        if (img.kind != "job" && img.kind != "module") continue;
        const std::string key = name.substr(0, 20); // "spu_" and the 16 hex digits of the hash
        auto [it, first] = canonical.emplace(key, name);
        if (first) continue;
        Image& canon = images.at(it->second);
        for (const u32 s : img.seeds) canon.seeds.insert((s - img.base + canon.base) % kLsSize);
        merged.insert(name);
    }

    fs::create_directories(out_dir, ec);
    std::string decls, calls;
    std::set<std::string> lifted;
    // The relocatable programs lifted so far, by size: another image whose code words hold the same bytes is that
    // program with other data (a module whose data changes with every load: Assassin's Creed saved 56 copies of one in
    // a session, run 118), found by its code hash at run time (D-208). It is not lifted again.
    struct ByCode {
        std::vector<u32> code_words;
        u64 code_hash;
    };
    std::map<u32, std::vector<ByCode>> by_size;
    for (auto& [name, img] : images) {
        if (merged.contains(name)) {
            rep.merged++;
            continue;
        }
        std::string bin;
        read_text(fs::path(dump_dir) / (name + ".bin"), bin);
        const bool reloc = img.kind == "job" || img.kind == "module";
        if (reloc && bin.size() >= img.size) {
            const auto* bytes = reinterpret_cast<const u8*>(bin.data());
            const auto same = [&](const ByCode& p) {
                return ps_spu_code_hash(bytes, img.size, p.code_words.data()) == p.code_hash;
            };
            if (const auto it = by_size.find(img.size);
                it != by_size.end() && std::any_of(it->second.begin(), it->second.end(), same)) {
                rep.merged++;
                continue;
            }
        }
        auto image = recomp::spu::load_raw_image(
            std::span<const u8>(reinterpret_cast<const u8*>(bin.data()), bin.size()), img.base, {img.entry}, name);
        if (!image) {
            PS_LOG_WARN(kChan, "{}: {}; it stays interpreted", name, image.error().message);
            rep.failed++;
            continue;
        }
        // Seeds inside the image's code only: execution also leaves lifted code for code outside it (the SPURS syscall
        // stubs, modules a program loads), which the lifter refuses as seeds.
        for (const u32 s : img.seeds)
            if (s >= img.base && s < img.base + img.size && image->in_code(s)) image->seeds.push_back(s);
        rep.seeds += image->seeds.size();
        recomp::spu::AnalyzerOptions analyzer;
        analyzer.discover = discover;
        recomp::spu::LiftOptions lift;
        lift.name = name;
        if (img.kind == "job" || img.kind == "module") {
            // Position independent (D-081): ila/il constants are not its code addresses either.
            lift.relocatable = analyzer.relocatable = true;
            analyzer.constant_entries = false;
        }
        const recomp::spu::Program program = recomp::spu::analyze(*image, analyzer);
        const recomp::spu::LiftResult result = recomp::spu::lift(*image, program, lift);
        if (reloc) by_size[img.size].push_back({result.code_words, result.code_hash});
        std::string why;
        if (!recomp::spu::write_files((fs::path(out_dir) / name).string(), result.files, why)) {
            if (error) *error = std::format("cannot write {}: {}", name, why);
            return false;
        }
        std::string seeds;
        for (const u32 seed : image->seeds) seeds += std::format("{:#x}\n", seed);
        std::ofstream(fs::path(out_dir) / name / (name + ".seeds")) << seeds;
        decls += std::format("void {}_register(void);\n", name);
        calls += std::format("    {}_register();\n", name);
        lifted.insert(name);
        rep.lifted++;
    }
    // Programs of earlier lifts that are no longer lifted (merged copies, images gone) must not be built.
    for (const auto& d : fs::directory_iterator(out_dir, ec))
        if (d.is_directory() && !lifted.contains(d.path().filename().string())) fs::remove_all(d.path(), ec);
    const std::string game_spu = "/* The game's lifted SPU programs (runtime/prepare, D-155). Generated: do not edit or "
                                 "commit. */\n\n" +
        decls + "\nvoid pspocket_game_spu_register(void);\nvoid pspocket_game_spu_register(void)\n{\n" + calls + "}\n";
    std::ofstream(fs::path(out_dir) / "game_spu.c") << game_spu;
    PS_LOG_INFO(kChan, "lifted {} SPU programs into {} ({} failed, {} copies merged); seeds: {}", rep.lifted, out_dir,
        rep.failed, rep.merged, rep.seeds);
    if (report) *report = rep;
    return true;
}

} // namespace pspocket::prepare
