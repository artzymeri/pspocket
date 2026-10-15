#include "pspocket/prepare/prepare.hpp"
#include "pspocket/prepare/session.hpp"
#include "pspocket/prepare/spu.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#include "pspocket/codegen/build.hpp"
#include "pspocket/codegen/toolchain.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/ppu/executable.hpp"
#include "pspocket/prepare/spu_static.hpp"
#include "jobs.hpp"

namespace pspocket::prepare {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* kChan = "prepare";

double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

using detail::fnv;
using detail::headers_define;
using detail::job_for;
using detail::read_bytes;
using detail::write_bytes;

std::string hex(const recomp::Sha256Digest& d) { return recomp::to_hex(d); }

std::string json_string(std::string_view s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out + "\"";
}

// A flat JSON object's field ("key": "text" or number), as manifest.json is written below.
std::string json_field(const std::string& json, const std::string& key)
{
    const std::string k = "\"" + key + "\":";
    size_t p = json.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    while (p < json.size() && json[p] == ' ') p++;
    if (p < json.size() && json[p] == '"') {
        const size_t end = json.find('"', p + 1);
        return end == std::string::npos ? "" : json.substr(p + 1, end - p - 1);
    }
    size_t end = p;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != '\n') end++;
    return json.substr(p, end - p);
}

} // namespace

std::string file_sha256(const std::string& path)
{
    std::string bytes;
    if (!read_bytes(path, bytes)) return "";
    return hex(recomp::Sha256::hash(std::string_view(bytes)));
}

bool read_manifest(const std::string& game_dir, Manifest* out)
{
    std::string json;
    if (!read_bytes(fs::path(game_dir) / "manifest.json", json)) return false;
    Manifest m;
    m.title_id = json_field(json, "title_id");
    m.executable_sha256 = json_field(json, "executable_sha256");
    m.made_by = json_field(json, "made_by");
    m.compiler = json_field(json, "compiler");
    m.cpu = json_field(json, "cpu");
    m.abi_hash = json_field(json, "abi_hash");
    m.translator_hash = json_field(json, "translator_hash");
    if (const std::string g = json_field(json, "translator_generation"); !g.empty()) m.translator_generation = std::atoi(g.c_str());
    m.system_generation = std::atoi(json_field(json, "system_generation").c_str());
    m.inputs_generation = std::atoi(json_field(json, "inputs_generation").c_str());
    m.made_at = std::atoll(json_field(json, "made_at").c_str());
    m.seconds = std::atof(json_field(json, "seconds").c_str());
    m.files = static_cast<size_t>(std::atoll(json_field(json, "files").c_str()));
    for (int k = 0; k < 3; k++) {
        const std::string rate = json_field(json, std::format("rate_{}", k));
        if (!rate.empty() && std::atof(rate.c_str()) > 0.05) m.rates[k] = std::atof(rate.c_str());
    }
    m.thermal = std::atoi(json_field(json, "thermal").c_str());
    if (out) *out = m;
    return !m.abi_hash.empty();
}

std::array<double, 3> prior_rates(const Manifest* own, const Manifest* last, int thermal_now)
{
    std::array<double, 3> r{1.0, 1.0, 1.0};
    const Manifest* m = own ? own : last;
    if (!m) return r;
    const double heat = thermal_slowdown(thermal_now) / thermal_slowdown(m->thermal);
    if (own) {
        for (int k = 0; k < 3; k++) r[static_cast<size_t>(k)] = own->rates[k] * heat;
    } else {
        r[0] = r[1] = last->rates[0] * heat; // the device, measured on PPU code
        r[2] = last->rates[2] * heat;
    }
    return r;
}

double thermal_slowdown(int thermal)
{
    static constexpr double kSlowdown[] = {1.0, 1.2, 1.5, 2.0};
    return kSlowdown[std::clamp(thermal, 0, 3)];
}

bool library_is_current(const std::string& game_dir, const std::string& executable_sha256, std::string* why,
    const std::string& spu_images_dir, const std::string& ppu_landings)
{
    Manifest m;
    std::error_code ec;
    auto say = [&](std::string s) {
        if (why) *why = std::move(s);
    };
    if (!fs::exists(fs::path(game_dir) / "game.dylib", ec) || !read_manifest(game_dir, &m)) {
        say("not prepared yet");
        return false;
    }
    if (m.abi_hash != std::format("{:016x}", ps_game_abi_hash)) {
        say("prepared by another version of pspocket: preparing it again");
        return false;
    }
    if (!m.cpu.empty() && m.cpu != codegen::host_cpu()) {
        say("prepared for another chip (" + m.cpu + "): preparing it again");
        return false;
    }
    if (m.executable_sha256 != executable_sha256) {
        say("prepared from another executable (an update?): preparing it again");
        return false;
    }
    if (m.translator_generation < kTranslatorGeneration) {
        say("prepared before a fix to how pspocket translates games: preparing it again");
        return false;
    }
    if (m.system_generation == 0 && m.inputs_generation < kGameInputsGeneration) {
        say("prepared before pspocket found more of the game's code to translate: preparing it again");
        return false;
    }
    if (!spu_images_dir.empty()) { // SPU programs the game loaded that the library has not got yet
        const auto made = fs::last_write_time(fs::path(game_dir) / "manifest.json", ec);
        for (const auto& e : fs::directory_iterator(spu_images_dir, ec))
            if (e.path().extension() == ".bin" && !ec && e.last_write_time(ec) > made) {
                say("the game has met SPU programs to translate: updating it");
                return false;
            }
    }
    if (!ppu_landings.empty()) { // places the PPU interpreter ran code: rewritten only when one is new (D-255)
        const auto made = fs::last_write_time(fs::path(game_dir) / "manifest.json", ec);
        std::error_code lec;
        const auto saved = fs::last_write_time(ppu_landings, lec);
        if (!ec && !lec && saved > made) {
            say("the game ran PPU code its library can now go on translated: updating it");
            return false;
        }
    }
    say(m.translator_hash == std::format("{:016x}", ps_game_translator_hash)
            ? "ready"
            : "ready (this version of pspocket translates it differently; preparing it again would update it)");
    return true;
}

Result prepare_game(const Options& o)
{
    Result r;
    const auto t0 = Clock::now();
    std::mutex progress_mutex;
    // The estimates of the steps not run yet, in seconds (refined as the steps run).
    double translate_estimate = 0, compile_estimate = 0, link_estimate = 3, sign_estimate = 1;
    auto report = [&](Progress::Phase phase, double left, size_t done, size_t total, std::string detail) {
        if (!o.progress) return;
        Progress p;
        p.phase = phase;
        p.seconds = since(t0);
        p.seconds_left = left;
        p.fraction = phase == Progress::Phase::Done ? 1.0 : left >= 0 ? p.seconds / (p.seconds + left) : 0.0;
        p.files_done = done;
        p.files_total = total;
        p.detail = std::move(detail);
        std::lock_guard lock(progress_mutex);
        o.progress(p);
    };
    auto fail = [&](std::string why) {
        r.error = std::move(why);
        r.seconds = since(t0);
        PS_LOG_ERROR(kChan, "{}: {}", o.title_id, r.error);
        report(Progress::Phase::Failed, -1, 0, 0, r.error);
        return r;
    };
    auto cancelled = [&] { return o.cancel && o.cancel->load(); };
    const unsigned workers = o.workers ? o.workers : std::max(1u, std::thread::hardware_concurrency());

    const fs::path dir = o.game_dir;
    std::error_code ec;
    fs::create_directories(dir / "objects" / "info", ec);
    if (ec) return fail(std::format("cannot make {}: {}", dir.string(), ec.message()));

    // 1. Translate: every PPU function of the executable, to C.
    std::string exe;
    if (!read_bytes(o.executable, exe)) return fail("cannot read the executable " + o.executable);
    const std::string exe_sha = hex(recomp::Sha256::hash(std::string_view(exe)));
    const double exe_mb = static_cast<double>(exe.size()) / 1e6;
    // GTA V: 28.6 MB of executable, 6 s to translate on the M4, about 2,400 cost-seconds of PPU code to compile, and
    // 1,900 of SPU code from 84 MB of lifted C (runs 094-095). Refined once the files are known.
    translate_estimate = 0.25 * exe_mb;
    double spu_c_mb = 0; // lifted SPU C: given, or about 0.78 MB per saved image (GTA V: 208 MB from 265, D-961)
    if (!o.spu_dir.empty())
        for (const auto& e : fs::recursive_directory_iterator(o.spu_dir, ec))
            if (e.is_regular_file() && e.path().extension() == ".c") spu_c_mb += static_cast<double>(e.file_size()) / 1e6;
    if (o.spu_dir.empty() && !o.spu_images_dir.empty())
        for (const auto& e : fs::directory_iterator(o.spu_images_dir, ec))
            if (e.path().extension() == ".bin") spu_c_mb += 0.78;
    // 4.6 predicted seconds a MB of SPU C since D-961's fast functions and cold twins (estimate_cost, D-986).
    compile_estimate = (85 * exe_mb * o.prior_rates[0] + 4.6 * spu_c_mb * o.prior_rates[1]) / workers;
    auto left_after_translation = [&] { return compile_estimate + link_estimate + sign_estimate; };
    report(Progress::Phase::Translating, translate_estimate + left_after_translation(), 0, 0,
        "Translating the game's code");
    fs::path ppu = o.ppu_dir;
    if (ppu.empty()) {
        const auto t1 = Clock::now();
        ppu = dir / "ppu";
        fs::remove_all(ppu, ec);
        auto loaded = recomp::ppu::load_executable(std::span<const u8>(reinterpret_cast<const u8*>(exe.data()), exe.size()));
        if (!loaded) return fail("the executable: " + loaded.error().message);
        if (loaded->seeds.empty()) return fail("the executable has no entry and no function descriptors");
        recomp::ppu::AnalyzerOptions analyzer;
        analyzer.toc = loaded->toc;
        const recomp::ppu::Program program = recomp::ppu::analyze(loaded->image, loaded->seeds, analyzer);
        if (cancelled()) return fail("cancelled");
        size_t instructions = 0;
        for (const auto& f : program.functions) instructions += f.insns.size();
        recomp::ppu::LiftOptions lift;
        lift.name = "game_ppu";
        lift.prefix = "game_ppu_";
        lift.comments = false;
        usize continuations_refused = 0;
        lift.tu_count = static_cast<u32>(std::max<size_t>(1, instructions / 12000)); // GTA V: 6.1 M in 512 units
        if (!o.ppu_landings.empty()) // where earlier sessions interpreted: continuations (D-255)
            for (u32 a : ppu::read_landings(o.ppu_landings)) lift.continuations.insert(a);
        std::string error;
        {
            recomp::ppu::LiftResult lifted = recomp::ppu::lift(loaded->image, program, lift);
            continuations_refused = lifted.stats.continuations_refused;
            if (!recomp::ppu::write_files(ppu.string(), lifted.files, error))
                return fail("cannot write the translation: " + error);
        }
        r.translate_seconds = since(t1);
        PS_LOG_INFO(kChan, "{}: {}; {} functions, {} instructions in {} units, translated in {:.1f} s", o.title_id,
            loaded->summary, program.functions.size(), instructions, lift.tu_count, r.translate_seconds);
        if (!lift.continuations.empty())
            PS_LOG_INFO(kChan, "{}: {} continuations from {}, {} refused (D-255)", o.title_id,
                lift.continuations.size() - continuations_refused, o.ppu_landings, continuations_refused);
    }
    if (cancelled()) return fail("cancelled");

    // 1b. The SPU programs: the ones the game's runs saved, and the ones the executable carries as ELF images (D-226:
    // its SPURS tasks and SPU thread images, found as the system library finds a module's, D-191), so even a game's
    // first session runs them translated. Lifted together (a fraction of a second each).
    fs::path spu_dir = o.spu_dir;
    if (spu_dir.empty()) {
        const auto t2 = Clock::now();
        std::vector<std::string> inputs;
        size_t images = 0;
        if (!o.spu_images_dir.empty() && fs::exists(o.spu_images_dir, ec)) {
            inputs.push_back(o.spu_images_dir);
            for (const auto& e : fs::directory_iterator(o.spu_images_dir, ec))
                if (e.path().extension() == ".bin") images++;
        }
        const fs::path statics = dir / "spu-static";
        fs::remove_all(statics, ec);
        fs::create_directories(statics, ec);
        SpuStaticCount found;
        if (auto elf = recomp::ElfFile::parse(std::span<const u8>(reinterpret_cast<const u8*>(exe.data()), exe.size()),
                {.symbols = false})) {
            SpuStaticOptions so;
            so.low_entry_relocatable = true;
            so.who = o.title_id;
            const std::vector<SpuSegment> segments = executable_segments(*elf);
            save_spu_images(segments, "the executable", statics, so, found);
        }
        // The disc's own SPU ELFs (D-239): each file one segment, scanned as the executable's.
        for (const std::string& elf_dir : o.spu_elf_dirs) {
            for (const auto& e : fs::directory_iterator(elf_dir, ec)) {
                if (e.path().extension() != ".elf") continue;
                std::string bytes;
                if (!read_bytes(e.path(), bytes)) continue;
                const std::vector<SpuSegment> file = {{0x10000000u, std::span<const u8>(
                    reinterpret_cast<const u8*>(bytes.data()), bytes.size())}}; // an address for the log only
                SpuStaticOptions so;
                so.low_entry_relocatable = true;
                so.who = o.title_id;
                save_spu_images(file, e.path().filename().string(), statics, so, found);
            }
        }
        if (found.programs) {
            inputs.push_back(statics.string());
            images += found.programs;
            PS_LOG_INFO(kChan, "{}: {} SPU programs in the executable's {} SPU images ({} overlays)", o.title_id,
                found.programs, found.images, found.overlays);
        }
        if (images > 0) {
            report(Progress::Phase::Translating, 0.2 * static_cast<double>(images) + left_after_translation(), 0, 0,
                std::format("Translating {} SPU programs", images));
            spu_dir = dir / "spu";
            SpuLiftReport spu;
            std::string error;
            if (!lift_spu_images(inputs, spu_dir.string(), o.spu_logs, &spu, &error, o.discover_spu))
                return fail("the SPU programs: " + error);
            r.spu_programs = spu.lifted;
        }
        r.spu_seconds = since(t2);
    }
    if (cancelled()) return fail("cancelled");

    // 2. Compile: the PPU units, the SPU programs, the library's entry.
    const codegen::TranslatedCodeSettings& settings = codegen::translated_code_settings();
    std::vector<std::string> ppu_args = settings.ppu_args, spu_args = settings.spu_args;
    ppu_args.insert(ppu_args.end(), o.extra_ppu_args.begin(), o.extra_ppu_args.end());
    spu_args.insert(spu_args.end(), o.extra_spu_args.begin(), o.extra_spu_args.end());
    std::vector<codegen::BuildJob> jobs;
    size_t spu_programs = 0;
    {
        const std::string headers = headers_define(ppu);
        for (const auto& e : fs::directory_iterator(ppu, ec))
            if (e.path().extension() == ".c") jobs.push_back(job_for(e.path(), false, headers, ppu_args, spu_args));
    }
    if (!spu_dir.empty()) {
        for (const auto& d : fs::directory_iterator(spu_dir, ec)) {
            if (d.is_regular_file() && d.path().extension() == ".c") {
                jobs.push_back(job_for(d.path(), true, "", ppu_args, spu_args)); // game_spu.c
            } else if (d.is_directory()) {
                spu_programs++;
                const std::string headers = headers_define(d.path());
                for (const auto& e : fs::directory_iterator(d.path(), ec))
                    if (e.path().extension() == ".c") jobs.push_back(job_for(e.path(), true, headers, ppu_args, spu_args));
            }
        }
    }
    const fs::path info_dir = dir / "objects" / "info";
    {
        std::string sha_list;
        const auto sha = recomp::Sha256::hash(std::string_view(exe));
        for (const u8 b : sha) sha_list += std::format("{}0x{:02x}", sha_list.empty() ? "" : ",", b);
        const std::string info = std::format(
            "/* Written by pspocket's game preparation: what this game library was made from (pspocket/game/library.h). */\n"
            "#define PS_GAME_ABI_HASH 0x{:016x}ull\n#define PS_GAME_TRANSLATOR_HASH 0x{:016x}ull\n"
            "#define PS_GAME_TITLE_ID \"{}\"\n#define PS_GAME_MADE_BY \"pspocket {}\"\n"
            "#define PS_GAME_EXECUTABLE_SHA256 {{{}}}\n#define PS_GAME_HAS_PPU 1\n#define PS_GAME_HAS_SPU {}\n"
            "#define PS_GAME_SPU_PROGRAM_COUNT {}u\n",
            ps_game_abi_hash, ps_game_translator_hash, o.title_id, PSPOCKET_VERSION, sha_list, spu_programs ? 1 : 0,
            spu_programs);
        if (!write_bytes(info_dir / "game_library_info.h", info.data(), info.size()))
            return fail("cannot write the library's description");
        codegen::BuildJob entry;
        entry.source = o.toolchain.entry_source;
        entry.include_dirs = {info_dir.string()};
        entry.defines = {std::format("PSPOCKET_INFO_HASH=0x{:016x}ull", fnv(info))};
        entry.cost = 0.05;
        entry.memory_mb = 50;
        entry.group = static_cast<int>(codegen::FileKind::Table);
        jobs.push_back(std::move(entry));
    }
    double cost_total = 0;
    for (const auto& j : jobs) cost_total += j.cost * o.prior_rates[std::clamp(j.group, 0, 2)];
    compile_estimate = cost_total / workers;

    codegen::BuildOptions b;
    b.compile.target = o.toolchain.target;
    b.compile.cpu = o.toolchain.cpu;
    b.compile.resource_dir = o.toolchain.resource_dir;
    b.compile.system_dirs = {o.toolchain.sysroot + "/include"};
    b.compile.include_dirs = o.toolchain.include_dirs;
    b.compile.defines = {"PSPOCKET_GAME_LIBRARY"};
    b.compile.extra_args = {"-fvisibility=hidden"};
    b.compile.qos_class = o.qos_class;
    b.link.platform = o.toolchain.platform;
    b.link.min_os = o.toolchain.min_os;
    b.link.install_name = "@rpath/game.dylib";
    b.link.exports = {"_pspocket_game_library"};
    b.link.libraries = {o.toolchain.sysroot + "/lib/libSystem.tbd"};
    b.workers = workers;
    b.memory_mb = o.memory_mb;
    b.object_dir = (dir / "objects").string();
    for (int k = 0; k < 3; k++) b.prior_rates[k] = o.prior_rates[k];
    std::vector<std::string> llvm_options = settings.llvm_options;
    llvm_options.insert(llvm_options.end(), o.llvm_options.begin(), o.llvm_options.end());
    b.cache_key = std::format("abi {:016x} {}", ps_game_abi_hash, codegen::compiler_version());
    for (const std::string& l : llvm_options) b.cache_key += " " + l;
    if (std::string llvm_error; !ensure_llvm_options(o.llvm_options, &llvm_error)) return fail(llvm_error);
    b.cancel = o.cancel;
    b.progress = [&](const codegen::BuildProgress& p) {
        if (o.on_file && !p.file.empty() && p.phase == codegen::BuildProgress::Phase::Compiling)
            o.on_file(p.file, p.file_group, p.file_cost, p.file_seconds, since(t0));
        switch (p.phase) {
        case codegen::BuildProgress::Phase::Compiling: {
            const double left = p.seconds_left >= 0 ? p.seconds_left : compile_estimate;
            report(Progress::Phase::Compiling, left + link_estimate + sign_estimate, p.files_done, p.files_total,
                std::format("Compiling: {} of {} files", p.files_done, p.files_total));
            break;
        }
        case codegen::BuildProgress::Phase::Linking:
            report(Progress::Phase::Linking, link_estimate + sign_estimate, p.files_done, p.files_total, "Linking");
            break;
        default: break;
        }
    };
    const fs::path unsigned_library = dir / "objects" / "game-unsigned.dylib";
    std::unique_lock compiling(compiler_mutex()); // a session's translator waits (D-236)
    const codegen::BuildResult built = codegen::build_library(jobs, {}, unsigned_library.string(), b);
    compiling.unlock();
    r.compile_seconds = built.compile_seconds;
    r.link_seconds = built.link_seconds;
    r.files_compiled = built.compiled;
    r.files_kept = built.reused;
    if (built.removed) {
        PS_LOG_INFO(kChan, "{}: {} objects of earlier preparations deleted ({:.1f} MB)", o.title_id, built.removed,
            static_cast<double>(built.removed_bytes) / 1e6);
    }
    for (int k = 0; k < 3; k++) r.rates[k] = built.compiled ? built.rates[k] : o.prior_rates[k];
    if (!built.ok) return fail(built.error);

    // 3. Sign, and put it in place.
    report(Progress::Phase::Signing, sign_estimate, jobs.size(), jobs.size(), "Signing");
    const auto t3 = Clock::now();
    std::string library_bytes;
    if (!read_bytes(unsigned_library, library_bytes)) return fail("cannot read the linked library");
    std::vector<u8> bytes(library_bytes.begin(), library_bytes.end());
    library_bytes.clear();
    std::string error;
    codesign::SignOptions sign;
    sign.identifier = o.identifier.empty() ? "com.pspocket.game." + o.title_id : o.identifier;
    sign.key = o.key;
    if (!codesign::sign(bytes, sign, &error)) return fail("signing failed: " + error);
    r.library = (dir / "game.dylib").string();
    if (!write_bytes(r.library, bytes.data(), bytes.size())) return fail("cannot write " + r.library);
    fs::remove(unsigned_library, ec);
    r.sign_seconds = since(t3);
    r.seconds = since(t0);

    // 4. What it was made from and by.
    const std::string manifest = std::format(
        "{{\n  \"title_id\": {},\n  \"executable_sha256\": \"{}\",\n  \"abi_hash\": \"{:016x}\",\n"
        "  \"translator_hash\": \"{:016x}\",\n  \"translator_generation\": {},\n{}  \"made_by\": {},\n"
        "  \"compiler\": {},\n  \"cpu\": {},\n"
        "  \"made_at\": {},\n"
        "  \"seconds\": {:.1f},\n  \"translate_seconds\": {:.1f},\n  \"compile_seconds\": {:.1f},\n"
        "  \"link_seconds\": {:.1f},\n  \"files\": {},\n  \"rate_0\": {:.3f},\n  \"rate_1\": {:.3f},\n"
        "  \"rate_2\": {:.3f},\n  \"thermal\": {},\n  \"bytes\": {}\n}}\n",
        json_string(o.title_id), exe_sha, ps_game_abi_hash, ps_game_translator_hash, kTranslatorGeneration,
        o.system_generation ? std::format("  \"system_generation\": {},\n", o.system_generation)
                            : std::format("  \"inputs_generation\": {},\n", kGameInputsGeneration),
        json_string(std::string("pspocket ") + PSPOCKET_VERSION), json_string(codegen::compiler_version()),
        json_string(o.toolchain.cpu == "native" ? codegen::host_cpu() : o.toolchain.cpu),
        static_cast<long long>(std::time(nullptr)), r.seconds, r.translate_seconds, r.compile_seconds, r.link_seconds,
        jobs.size(), r.rates[0], r.rates[1], r.rates[2], o.thermal, bytes.size());
    if (!write_bytes(dir / "manifest.json", manifest.data(), manifest.size())) return fail("cannot write the manifest");
    if (!o.keep_sources && o.ppu_dir.empty()) fs::remove_all(dir / "ppu", ec);
    PS_LOG_INFO(kChan, "{}: prepared in {:.0f} s (translate {:.1f}, compile {:.0f}, link {:.1f}, sign {:.1f}): {} files, "
                       "{} kept, {:.1f} MB, rates PPU {:.2f} SPU {:.2f} tables {:.2f}",
        o.title_id, r.seconds, r.translate_seconds, r.compile_seconds, r.link_seconds, r.sign_seconds,
        r.files_compiled, r.files_kept, static_cast<double>(bytes.size()) / 1048576.0, r.rates[0], r.rates[1],
        r.rates[2]);
    r.ok = true;
    report(Progress::Phase::Done, 0, jobs.size(), jobs.size(), "Ready");
    return r;
}

} // namespace pspocket::prepare
