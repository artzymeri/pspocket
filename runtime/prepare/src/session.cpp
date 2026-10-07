// SPU programs translated while the game runs (D-236). See session.hpp.

#include "pspocket/prepare/session.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "jobs.hpp"
#include "pspocket/codegen/build.hpp"
#include "pspocket/codegen/toolchain.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/prepare/spu.hpp"
#include "pspocket/spu/runtime.h"

namespace pspocket::prepare {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using detail::read_bytes;
using detail::write_bytes;

constexpr const char* kChan = "session";

double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// An image the session saved (spu_<hash>_<ls>.txt and .bin, as hle's dump writes them).
struct Met {
    std::string name, kind;
    u64 hash = 0;
    u32 ls = 0, size = 0;
    fs::file_time_type at; // its description's time: when it was first met, give or take its seeds
};

bool parse_met(const fs::path& txt, Met& m)
{
    const std::string stem = txt.stem().string();
    if (stem.size() != 26 || !stem.starts_with("spu_") || stem[20] != '_') return false;
    std::string text;
    if (!read_bytes(txt, text)) return false;
    try {
        m.name = stem;
        m.hash = std::stoull(stem.substr(4, 16), nullptr, 16);
        m.ls = static_cast<u32>(std::stoul(stem.substr(21), nullptr, 16));
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) {
            if (line.starts_with("kind ")) m.kind = line.substr(5);
            else if (line.starts_with("size ")) m.size = static_cast<u32>(std::stoul(line.substr(5), nullptr, 0));
        }
    } catch (...) {
        return false;
    }
    return m.size != 0 && !m.kind.empty();
}

// The predicted seconds of compiling a lifted program (its folder's C files, codegen::estimate_cost).
double program_cost(const fs::path& dir)
{
    double cost = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".c") continue;
        std::string text;
        read_bytes(e.path(), text);
        const bool table = e.path().filename().string().ends_with("_reg.c");
        cost += codegen::estimate_cost(text, table ? codegen::FileKind::Table : codegen::FileKind::SpuCode).seconds;
    }
    return cost;
}

void write_game_spu(const fs::path& spu_dir, const std::vector<std::string>& programs)
{
    std::string decls, calls;
    for (const std::string& name : programs) {
        decls += std::format("void {}_register(void);\n", name);
        calls += std::format("    {}_register();\n", name);
    }
    const std::string text = "/* SPU programs translated during a session (runtime/prepare, D-236). Generated: do not "
                             "edit or commit. */\n\n" + decls +
        "\nvoid pspocket_game_spu_register(void);\nvoid pspocket_game_spu_register(void)\n{\n" + calls + "}\n";
    write_bytes(spu_dir / "game_spu.c", text.data(), text.size());
}

} // namespace

bool ensure_llvm_options(const std::vector<std::string>& extra, std::string* error)
{
    static std::mutex m;
    static bool set = false;
    static std::vector<std::string> options;
    static std::string diagnostics;
    std::vector<std::string> wanted = codegen::translated_code_settings().llvm_options;
    wanted.insert(wanted.end(), extra.begin(), extra.end());
    std::lock_guard lock(m);
    if (!set) { // LLVM's options are the process's: set once, before any compile
        set = true;
        options = wanted;
        codegen::set_llvm_options(options, &diagnostics);
    }
    if (!diagnostics.empty()) {
        if (error) *error = "LLVM's options: " + diagnostics;
        return false;
    }
    if (wanted != options) {
        if (error) *error = "LLVM's options were set already, to others: compiles cannot use both";
        return false;
    }
    return true;
}

std::mutex& compiler_mutex()
{
    static std::mutex m;
    return m;
}

SpuLibraryResult build_spu_library(const std::string& spu_dir, const std::string& work_dir, const std::string& name,
    const SpuLibraryOptions& o)
{
    SpuLibraryResult r;
    auto fail = [&](std::string why) {
        r.error = std::move(why);
        return r;
    };
    const fs::path dir = work_dir;
    const fs::path info_dir = dir / ("info-" + name);
    std::error_code ec;
    fs::create_directories(dir / "objects", ec);
    fs::create_directories(info_dir, ec);
    if (ec) return fail(std::format("cannot make {}: {}", dir.string(), ec.message()));

    const codegen::TranslatedCodeSettings& settings = codegen::translated_code_settings();
    std::vector<std::string> spu_args = settings.spu_args;
    spu_args.insert(spu_args.end(), o.extra_spu_args.begin(), o.extra_spu_args.end());
    std::vector<codegen::BuildJob> jobs;
    for (const auto& d : fs::directory_iterator(spu_dir, ec)) {
        if (d.is_regular_file() && d.path().extension() == ".c") {
            jobs.push_back(detail::job_for(d.path(), true, "", {}, spu_args)); // game_spu.c
        } else if (d.is_directory()) {
            r.programs++;
            const std::string headers = detail::headers_define(d.path());
            for (const auto& e : fs::directory_iterator(d.path(), ec))
                if (e.path().extension() == ".c") jobs.push_back(detail::job_for(e.path(), true, headers, {}, spu_args));
        }
    }
    if (!r.programs) return fail("no SPU programs in " + spu_dir);
    // What it is: SPU programs only, for no executable in particular (game::load_library checks no image words).
    const std::string info = std::format(
        "/* Written by pspocket's session translator: what this library was made from (pspocket/game/library.h). */\n"
        "#define PS_GAME_ABI_HASH 0x{:016x}ull\n#define PS_GAME_TRANSLATOR_HASH 0x{:016x}ull\n"
        "#define PS_GAME_TITLE_ID \"{}\"\n#define PS_GAME_MADE_BY \"pspocket {} (during a session)\"\n"
        "#define PS_GAME_EXECUTABLE_SHA256 {{0}}\n#define PS_GAME_HAS_PPU 0\n#define PS_GAME_HAS_SPU 1\n"
        "#define PS_GAME_SPU_PROGRAM_COUNT {}u\n",
        ps_game_abi_hash, ps_game_translator_hash, o.title_id, PSPOCKET_VERSION, r.programs);
    if (!write_bytes(info_dir / "game_library_info.h", info.data(), info.size()))
        return fail("cannot write the library's description");
    codegen::BuildJob entry;
    entry.source = o.toolchain.entry_source;
    entry.include_dirs = {info_dir.string()};
    entry.defines = {std::format("PSPOCKET_INFO_HASH=0x{:016x}ull", detail::fnv(info))};
    entry.cost = 0.05;
    entry.memory_mb = 50;
    entry.group = static_cast<int>(codegen::FileKind::Table);
    jobs.push_back(std::move(entry));
    r.files = jobs.size();

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
    b.link.install_name = "@rpath/pspocket-" + name + ".dylib"; // each its own: dyld keeps them apart
    b.link.exports = {"_pspocket_game_library"};
    b.link.libraries = {o.toolchain.sysroot + "/lib/libSystem.tbd"};
    b.workers = o.workers;
    b.memory_mb = o.memory_mb;
    b.object_dir = (dir / "objects").string();
    b.cache_key = std::format("abi {:016x} {}", ps_game_abi_hash, codegen::compiler_version());
    std::vector<std::string> llvm_options = settings.llvm_options;
    llvm_options.insert(llvm_options.end(), o.llvm_options.begin(), o.llvm_options.end());
    for (const std::string& l : llvm_options) b.cache_key += " " + l;
    if (std::string llvm_error; !ensure_llvm_options(o.llvm_options, &llvm_error)) return fail(llvm_error);
    b.cancel = o.cancel;
    const fs::path unsigned_library = dir / "objects" / (name + "-unsigned.dylib");
    codegen::BuildResult built;
    {
        std::lock_guard compiling(compiler_mutex());
        built = codegen::build_library(jobs, {}, unsigned_library.string(), b);
    }
    r.compile_seconds = built.compile_seconds;
    r.link_seconds = built.link_seconds;
    if (!built.ok) {
        r.compile_failed = built.error.find("did not compile") != std::string::npos;
        return fail(built.error);
    }

    const auto t = Clock::now();
    std::string library_bytes;
    if (!read_bytes(unsigned_library, library_bytes)) return fail("cannot read the linked library");
    std::vector<u8> bytes(library_bytes.begin(), library_bytes.end());
    std::string error;
    codesign::SignOptions sign;
    sign.identifier = o.identifier;
    sign.key = o.key;
    if (!codesign::sign(bytes, sign, &error)) return fail("signing failed: " + error);
    r.library = (dir / (name + ".dylib")).string();
    if (!write_bytes(r.library, bytes.data(), bytes.size())) return fail("cannot write " + r.library);
    fs::remove(unsigned_library, ec);
    r.sign_seconds = since(t);
    r.ok = true;
    return r;
}

SessionTranslator::SessionTranslator(SessionOptions options) : o_(std::move(options)) {}

SessionTranslator::~SessionTranslator() { stop(); }

bool SessionTranslator::start(std::string* error)
{
    std::error_code ec;
    fs::remove_all(o_.work_dir, ec);
    fs::create_directories(o_.work_dir, ec);
    if (ec) {
        if (error) *error = std::format("cannot make {}: {}", o_.work_dir, ec.message());
        return false;
    }
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void SessionTranslator::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

SessionStats SessionTranslator::stats() const
{
    std::lock_guard lock(mutex_);
    return stats_;
}

void SessionTranslator::run()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); // behind the game's threads
#endif
    const auto poll = std::chrono::duration<double>(o_.poll_seconds);
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, poll, [&] { return stopping_.load(); });
        }
        if (stopping_) return;
        if (!batch()) return;
    }
}

bool SessionTranslator::batch()
{
    if (o_.may_compile && !o_.may_compile()) return true; // later
    // The images saved since the last look, whole, without a program registered since (the game library's, an
    // earlier batch's).
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    const auto settle = std::chrono::duration_cast<fs::file_time_type::duration>(
        std::chrono::duration<double>(o_.settle_seconds));
    std::vector<Met> fresh;
    for (const auto& e : fs::directory_iterator(o_.images_dir, ec)) {
        const fs::path txt = e.path();
        if (txt.extension() != ".txt" || handled_.contains(txt.stem().string())) continue;
        const fs::path bin = fs::path(txt).replace_extension(".bin");
        const auto at = fs::last_write_time(txt, ec);
        if (ec || now - at < settle || !fs::exists(bin, ec)) continue;
        Met m;
        if (!parse_met(txt, m)) continue;
        m.at = at;
        handled_.insert(m.name);
        std::string bytes;
        read_bytes(bin, bytes);
        const bool relocatable = m.kind == "job" || m.kind == "module";
        const bool known = ps_spu_find_program(m.hash, m.ls, m.size) ||
            (relocatable && bytes.size() >= m.size &&
                ps_spu_find_program_by_code(reinterpret_cast<const u8*>(bytes.data()), m.size));
        std::lock_guard lock(mutex_);
        stats_.images++;
        if (known) stats_.known++;
        else fresh.push_back(std::move(m));
    }
    if (fresh.empty()) return true;

    // Lifted together (copies with the same code are lifted once, D-208), then built one program per library,
    // the cheapest first: a small module is not kept waiting behind a big job binary.
    const int n = ++batch_number_;
    const fs::path dir = fs::path(o_.work_dir) / std::format("batch-{}", n);
    const fs::path images = dir / "images", lifted = dir / "spu";
    fs::create_directories(images, ec);
    std::map<std::string, fs::file_time_type> met_at;
    for (const Met& m : fresh) {
        for (const char* ext : {".bin", ".txt"})
            fs::copy_file(fs::path(o_.images_dir) / (m.name + ext), images / (m.name + ext),
                fs::copy_options::overwrite_existing, ec);
        met_at[m.name] = m.at;
    }
    const auto t_lift = Clock::now();
    SpuLiftReport rep;
    std::string error;
    if (!lift_spu_images(images.string(), lifted.string(), {}, &rep, &error, true)) {
        PS_LOG_WARN(kChan, "batch {}: {} SPU images not lifted: {}", n, fresh.size(), error);
        std::lock_guard lock(mutex_);
        stats_.failed += fresh.size();
        return true;
    }
    const double lift_seconds = since(t_lift);
    struct Program {
        std::string name;
        double cost;
    };
    std::vector<Program> programs;
    for (const auto& d : fs::directory_iterator(lifted, ec))
        if (d.is_directory()) programs.push_back({d.path().filename().string(), program_cost(d.path())});
    std::sort(programs.begin(), programs.end(), [](const Program& a, const Program& b) { return a.cost < b.cost; });
    {
        std::lock_guard lock(mutex_);
        stats_.lifted += programs.size();
    }
    for (const Program& p : programs) {
        if (stopping_) return false;
        // The cost model's seconds, at the speed this session's compiles ran so far: it was fitted on GTA V's SPU code,
        // and Assassin's Creed's programs compiled 8 to 100 times faster than it says (run 219).
        const double rate = predicted_done_ > 5 ? compiled_seconds_ / predicted_done_ : 1.0;
        if (o_.max_program_seconds > 0 && p.cost * rate > o_.max_program_seconds) {
            PS_LOG_INFO(kChan, "{}: about {:.0f} s to compile; left to the next launch's preparation", p.name,
                p.cost * rate);
            std::lock_guard lock(mutex_);
            stats_.deferred++;
            continue;
        }
        while (o_.may_compile && !o_.may_compile()) { // memory, heat or the app in the background: wait
            std::unique_lock lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_.load(); })) return false;
        }
        const fs::path own = dir / ("lib-" + p.name) / "spu";
        fs::create_directories(own, ec);
        fs::rename(lifted / p.name, own / p.name, ec);
        write_game_spu(own, {p.name});
        const std::string name = std::format("spu-{}", ++library_number_);
        SpuLibraryOptions lo = o_.library;
        lo.cancel = &stopping_;
        const SpuLibraryResult lib = build_spu_library(own.string(), o_.work_dir, name, lo);
        if (!lib.ok) {
            if (stopping_) return false;
            std::lock_guard lock(mutex_);
            stats_.failed++;
            if (lib.compile_failed) { // this program's code: logged, and the others go on
                PS_LOG_WARN(kChan, "{}: {}", p.name, lib.error.substr(0, 2000));
                continue;
            }
            PS_LOG_WARN(kChan, "{}: {}; translating SPU programs during the session stops", p.name, lib.error);
            stats_.stopped_on_failure = true;
            return false;
        }
        const auto t_load = Clock::now();
        std::string why;
        if (!o_.load || !o_.load(lib.library, &why)) {
            PS_LOG_WARN(kChan, "{}: cannot load {}: {}; translating SPU programs during the session stops", p.name,
                lib.library, why);
            std::lock_guard lock(mutex_);
            stats_.failed++;
            stats_.stopped_on_failure = true;
            return false;
        }
        const double load_ms = since(t_load) * 1000;
        double waited = 0;
        if (const auto it = met_at.find(p.name); it != met_at.end())
            waited = std::chrono::duration<double>(fs::file_time_type::clock::now() - it->second).count();
        PS_LOG_INFO(kChan, "{}: translated during the session: lifted with {} others in {:.2f} s, compiled in {:.1f} s "
                           "(predicted {:.1f}), linked in {:.2f} s, signed in {:.2f} s, loaded in {:.0f} ms; met {:.1f} s ago",
            p.name, programs.size() - 1, lift_seconds, lib.compile_seconds, p.cost, lib.link_seconds, lib.sign_seconds,
            load_ms, waited);
        predicted_done_ += p.cost;
        compiled_seconds_ += lib.compile_seconds;
        std::lock_guard lock(mutex_);
        stats_.loaded++;
        stats_.batches++;
        stats_.compile_seconds += lib.compile_seconds;
        stats_.link_seconds += lib.link_seconds;
        stats_.sign_seconds += lib.sign_seconds;
        stats_.first_seen_to_loaded_max = std::max(stats_.first_seen_to_loaded_max, waited);
    }
    return true;
}

} // namespace pspocket::prepare
