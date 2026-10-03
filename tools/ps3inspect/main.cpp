// ps3inspect: Phase 0 inventory of PS3 binaries (plan section 10, docs/phase0.md).
//
// Reads decrypted PPU executables (EBOOT.elf), PRX modules and SPU ELFs and reports headers, segments, the process
// parameters, imports by module with NIDs resolved to names, exports, the function count estimate, embedded SPU
// programs and middleware signatures. SELF files are recognized and reported as encrypted (decrypt them with RPCS3
// first); this tool never decrypts anything.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "pspocket/core/log.hpp"
#include "pspocket/recomp/bytes.hpp"
#include "pspocket/recomp/inventory.hpp"
#include "pspocket/recomp/json_writer.hpp"
#include "report.hpp"

using namespace pspocket;
using namespace pspocket::recomp;

namespace {

// Exit codes. With several files the most severe wins: I/O and usage errors, then bad input, then encryption.
constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;     // bad arguments or I/O failure
constexpr int kExitBadInput = 2;  // a file is malformed or of an unknown type
constexpr int kExitEncrypted = 3; // a file is an encrypted SELF (decrypt it with RPCS3)

constexpr u64 kMaxFileSize = 2ull << 30;

constexpr const char* kUsage = R"(usage: ps3inspect [options] <file>...

Inventory of PS3 binaries for Phase 0 (docs/phase0.md): decrypted PPU executables (EBOOT.elf), PRX modules
and SPU ELFs. SELF/SPRX files are recognized and reported as encrypted: decrypt them with RPCS3 first.

options:
  --json <out.json>     also write the full report as JSON
  --names <file>        load extra NID names ([module] headers, `name` or `0xNNNNNNNN name` lines); repeatable
  --strings             list printable strings
  --min-string <N>      minimum string length for --strings (default 6)
  --spu-extract <dir>   write every embedded SPU image to <dir> (derived game data: keep it out of git)
  --quiet               no text report on stdout (errors still go to stderr)
  -h, --help            this help

exit status: 0 ok, 1 usage or I/O error, 2 malformed or unknown input, 3 encrypted SELF (needs RPCS3 decryption)
)";

struct Options {
    std::string json_path;
    std::vector<std::string> names_files;
    bool strings = false;
    usize min_string = 6;
    std::string spu_extract_dir;
    bool quiet = false;
    std::vector<std::string> files;
};

bool parse_args(int argc, char** argv, Options& o, int& exit_code)
{
    for (int i = 1; i < argc; i++) {
        const std::string_view a = argv[i];
        auto need_value = [&](std::string_view flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ps3inspect: %.*s needs a value\n", static_cast<int>(flag.size()), flag.data());
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            exit_code = kExitOk;
            return false;
        } else if (a == "--json") {
            const char* v = need_value(a);
            if (!v) return exit_code = kExitUsage, false;
            o.json_path = v;
        } else if (a == "--names") {
            const char* v = need_value(a);
            if (!v) return exit_code = kExitUsage, false;
            o.names_files.emplace_back(v);
        } else if (a == "--strings") {
            o.strings = true;
        } else if (a == "--min-string") {
            const char* v = need_value(a);
            if (!v) return exit_code = kExitUsage, false;
            char* end = nullptr;
            const unsigned long n = std::strtoul(v, &end, 10);
            if (end == v || *end != '\0' || n == 0 || n > 4096) {
                std::fprintf(stderr, "ps3inspect: --min-string needs a number from 1 to 4096\n");
                return exit_code = kExitUsage, false;
            }
            o.min_string = n;
        } else if (a == "--spu-extract") {
            const char* v = need_value(a);
            if (!v) return exit_code = kExitUsage, false;
            o.spu_extract_dir = v;
        } else if (a == "--quiet") {
            o.quiet = true;
        } else if (a.starts_with("-") && a != "-") {
            std::fprintf(stderr, "ps3inspect: unknown option %s\n%s", argv[i], kUsage);
            return exit_code = kExitUsage, false;
        } else {
            o.files.emplace_back(a);
        }
    }
    if (o.files.empty()) {
        std::fputs(kUsage, stderr);
        exit_code = kExitUsage;
        return false;
    }
    return true;
}

bool read_file(const std::string& path, std::vector<u8>& out, std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = std::format("cannot read {}: {}", path, ec.message());
        return false;
    }
    if (size > kMaxFileSize) {
        error = std::format("{} is larger than {} bytes", path, kMaxFileSize);
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::format("cannot open {}", path);
        return false;
    }
    out.resize(static_cast<usize>(size));
    if (size != 0 && !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
        error = std::format("cannot read {}", path);
        return false;
    }
    return true;
}

std::string sanitize(std::string_view name)
{
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        out.push_back(ok ? c : '_');
        if (out.size() == 64) break;
    }
    return out;
}

bool extract_spu(const FileReport& report, std::span<const u8> data, const std::filesystem::path& dir)
{
    const std::string stem = sanitize(std::filesystem::path(report.path).filename().string());
    bool ok = true;
    for (usize i = 0; i < report.spu_programs.size(); i++) {
        const SpuImage& img = report.spu_programs[i].image;
        if (!range_ok(data.size(), img.offset, img.size)) continue;
        std::string file = std::format("{}.spu{:03}.{}", stem, i, to_hex(img.sha1).substr(0, 12));
        if (!img.name.empty()) file += "." + sanitize(img.name);
        file += ".elf";
        const auto path = dir / file;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data() + img.offset), static_cast<std::streamsize>(img.size));
        if (!out) {
            PS_LOG_ERROR("ps3inspect", "cannot write {}", path.string());
            ok = false;
        }
    }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    Options opts;
    int exit_code = kExitOk;
    if (!parse_args(argc, argv, opts, exit_code)) return exit_code;

    // NID database: the embedded one plus any --names files.
    NidDb db = NidDb::builtin();
    for (const auto& f : opts.names_files) {
        auto added = db.load_file(f);
        if (!added) {
            PS_LOG_ERROR("ps3inspect", "{}", added.error().message);
            return kExitUsage;
        }
        if (!opts.quiet) std::fprintf(stdout, "loaded %zu names from %s\n", *added, f.c_str());
    }

    if (!opts.spu_extract_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opts.spu_extract_dir, ec);
        if (ec) {
            PS_LOG_ERROR("ps3inspect", "cannot create {}: {}", opts.spu_extract_dir, ec.message());
            return kExitUsage;
        }
        PS_LOG_WARN("ps3inspect", "--spu-extract writes SPU programs taken from game binaries to {}: that is derived game data. "
                                  "Keep the directory out of git (CLAUDE.md rule 1; inventory/ is gitignored).",
            opts.spu_extract_dir);
    }

    InspectOptions inspect;
    inspect.nids = &db;
    inspect.collect_strings = opts.strings;
    inspect.min_string_length = opts.min_string;

    ps3inspect::ReportOptions report_opts;
    report_opts.strings = opts.strings;

    JsonWriter json;
    json.begin_object();
    json.field("tool", "ps3inspect").field("format_version", 1);
    json.key("nid_database").begin_object().field("entries", db.size()).field("modules", db.module_count()).end_object();
    json.key("files").begin_array();

    auto worsen = [&](int code) {
        if (exit_code == kExitOk || (code != kExitOk && code < exit_code)) exit_code = code;
    };

    if (!opts.quiet) std::fprintf(stdout, "NID database: %zu names in %zu modules\n\n", db.size(), db.module_count());
    for (const auto& path : opts.files) {
        std::vector<u8> data;
        std::string error;
        if (!read_file(path, data, error)) {
            PS_LOG_ERROR("ps3inspect", "{}", error);
            worsen(kExitUsage);
            continue;
        }
        const FileReport report = inspect_bytes(path, data, inspect);

        if (!opts.quiet) {
            const std::string text = ps3inspect::render_text(report, report_opts);
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fputc('\n', stdout);
        }
        for (const auto& e : report.errors) PS_LOG_ERROR("ps3inspect", "{}: {}", path, e);
        ps3inspect::write_json(json, report, report_opts);

        if (!opts.spu_extract_dir.empty() && !report.spu_programs.empty()) {
            if (!extract_spu(report, data, opts.spu_extract_dir)) worsen(kExitUsage);
        }

        if (!report.errors.empty() || report.kind == FileKind::Unknown || report.kind == FileKind::OtherElf) {
            worsen(kExitBadInput);
        } else if (report.encrypted()) {
            worsen(kExitEncrypted);
        }
    }
    json.end_array();
    json.field("exit_status", exit_code);
    json.end_object();

    if (!opts.json_path.empty()) {
        std::ofstream out(opts.json_path, std::ios::binary | std::ios::trunc);
        out << json.str() << '\n';
        if (!out) {
            PS_LOG_ERROR("ps3inspect", "cannot write {}", opts.json_path);
            return kExitUsage;
        }
    }
    return exit_code;
}
