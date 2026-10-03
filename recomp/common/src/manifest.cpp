#include "pspocket/recomp/manifest.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>

#include "pspocket/recomp/bytes.hpp"
#include "pspocket/recomp/toml.hpp"

namespace pspocket::recomp {

namespace {

using toml::Value;

std::string lowercase(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool is_identifier(std::string_view s)
{
    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool is_title_id(std::string_view s)
{
    if (s.size() != 9) return false;
    for (usize i = 0; i < 4; i++) {
        if (s[i] < 'A' || s[i] > 'Z') return false;
    }
    for (usize i = 4; i < 9; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

bool is_app_version(std::string_view s)
{
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    return s.size() == 5 && digit(s[0]) && digit(s[1]) && s[2] == '.' && digit(s[3]) && digit(s[4]);
}

bool is_hash(std::string_view s, usize hex_digits) { return s.size() == hex_digits && is_hex_digits(s); }

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

class Reader {
public:
    explicit Reader(std::vector<std::string>& problems) : problems_(problems) {}

    void check_keys(const Value& table, std::initializer_list<std::string_view> allowed, std::string_view where)
    {
        for (usize i = 0; i < table.keys.size(); i++) {
            if (std::find(allowed.begin(), allowed.end(), table.keys[i]) == allowed.end()) {
                problems_.push_back(std::format("{}: unknown key '{}' (line {})", where, table.keys[i], table.values[i].line));
            }
        }
    }

    std::optional<std::string> string(const Value& table, std::string_view key, std::string_view where)
    {
        const Value* v = table.get(key);
        if (!v) {
            problems_.push_back(std::format("{}: missing '{}'", where, key));
            return std::nullopt;
        }
        if (v->type != Value::Type::String) {
            problems_.push_back(std::format("{}: '{}' must be a string, not a {} (line {})", where, key, toml::type_name(v->type), v->line));
            return std::nullopt;
        }
        return v->string;
    }

    std::optional<u32> address(const Value& table, std::string_view key, std::string_view where)
    {
        const Value* v = table.get(key);
        if (!v) {
            problems_.push_back(std::format("{}: missing '{}'", where, key));
            return std::nullopt;
        }
        if (v->type != Value::Type::Integer) {
            problems_.push_back(std::format("{}: '{}' must be an integer, not a {} (line {})", where, key, toml::type_name(v->type), v->line));
            return std::nullopt;
        }
        if (v->integer < 0 || v->integer > 0xFFFFFFFFll) {
            problems_.push_back(std::format("{}: '{}' = {:#x} is not a 32-bit guest address (line {})", where, key, v->integer, v->line));
            return std::nullopt;
        }
        return static_cast<u32>(v->integer);
    }

    // The tables of an array of tables; empty (and a problem) when `key` holds something else.
    std::vector<const Value*> tables(const Value& root, std::string_view key)
    {
        std::vector<const Value*> out;
        const Value* v = root.get(key);
        if (!v) return out;
        if (v->type != Value::Type::Array) {
            problems_.push_back(std::format("'{}' must be an array of tables ([[{}]]), not a {} (line {})", key, key, toml::type_name(v->type), v->line));
            return out;
        }
        for (usize i = 0; i < v->items.size(); i++) {
            if (v->items[i].type != Value::Type::Table) {
                problems_.push_back(std::format("{}[{}] must be a table (line {})", key, i, v->items[i].line));
                continue;
            }
            out.push_back(&v->items[i]);
        }
        return out;
    }

private:
    std::vector<std::string>& problems_;
};

} // namespace

std::string_view spu_action_name(SpuAction action)
{
    switch (action) {
    case SpuAction::Lift: return "lift";
    case SpuAction::Replace: return "replace";
    case SpuAction::Interpret: return "interpret";
    }
    return "lift";
}

std::vector<std::string> validate_manifest(const GameManifest& m, const ManifestOptions& options)
{
    std::vector<std::string> problems;
    if (!is_title_id(m.title_id)) problems.push_back(std::format("title_id '{}' is not a title ID like BLES00229", m.title_id));
    if (trim(m.title).empty()) problems.push_back("title is empty");
    if (!is_app_version(m.app_version)) problems.push_back(std::format("app_version '{}' is not NN.NN", m.app_version));
    if (!m.iso_sha256.empty() && !is_hash(m.iso_sha256, 64)) problems.push_back("iso_sha256 is not 64 hex digits");
    if (options.require_hashes && m.iso_sha256.empty()) problems.push_back("iso_sha256 is required");

    if (options.require_hashes && m.files.empty()) problems.push_back("no [[files]] listed");
    std::set<std::string> paths;
    for (usize i = 0; i < m.files.size(); i++) {
        const auto& f = m.files[i];
        if (f.path.empty() || f.path.front() == '/' || f.path.find('\\') != std::string::npos) {
            problems.push_back(std::format("files[{}]: path '{}' must be relative, with '/' separators", i, f.path));
        }
        if (f.path == ".." || f.path.starts_with("../") || f.path.find("/../") != std::string::npos || f.path.ends_with("/..")) {
            problems.push_back(std::format("files[{}]: path '{}' must not contain '..'", i, f.path));
        }
        if (!paths.insert(f.path).second) problems.push_back(std::format("files[{}]: duplicate path '{}'", i, f.path));
        if (!f.sha256.empty() && !is_hash(f.sha256, 64)) problems.push_back(std::format("files[{}]: sha256 is not 64 hex digits", i));
        if (options.require_hashes && f.sha256.empty()) problems.push_back(std::format("files[{}]: sha256 is required", i));
    }

    std::set<u32> override_addrs;
    std::set<std::string> override_names;
    for (usize i = 0; i < m.overrides.size(); i++) {
        const auto& o = m.overrides[i];
        if (o.addr % 4 != 0) problems.push_back(std::format("overrides[{}]: addr {:#010x} is not 4-byte aligned", i, o.addr));
        if (!is_identifier(o.name)) problems.push_back(std::format("overrides[{}]: name '{}' is not a C identifier", i, o.name));
        if (!override_addrs.insert(o.addr).second) problems.push_back(std::format("overrides[{}]: addr {:#010x} is overridden twice", i, o.addr));
        if (!override_names.insert(o.name).second) problems.push_back(std::format("overrides[{}]: name '{}' is used twice", i, o.name));
    }

    std::map<u64, std::pair<u64, usize>> ranges; // start -> (end, index)
    for (usize i = 0; i < m.patches.size(); i++) {
        const auto& p = m.patches[i];
        if (p.bytes.empty()) problems.push_back(std::format("patches[{}]: bytes is empty", i));
        if (trim(p.why).empty()) problems.push_back(std::format("patches[{}]: 'why' must explain the patch", i));
        const u64 end = u64{p.addr} + p.bytes.size();
        if (end > 0x1'0000'0000ull) problems.push_back(std::format("patches[{}]: runs past the 32-bit address space", i));
        if (!p.bytes.empty()) {
            auto next = ranges.lower_bound(p.addr);
            const bool overlaps_next = next != ranges.end() && next->first < end;
            const bool overlaps_prev = next != ranges.begin() && std::prev(next)->second.first > p.addr;
            if (overlaps_next || overlaps_prev) {
                const usize other = overlaps_next ? next->second.second : std::prev(next)->second.second;
                problems.push_back(std::format("patches[{}]: overlaps patches[{}]", i, other));
            } else {
                ranges.emplace(p.addr, std::make_pair(end, i));
            }
        }
    }

    std::set<std::string> spu_hashes;
    for (usize i = 0; i < m.spu_programs.size(); i++) {
        const auto& s = m.spu_programs[i];
        if (!is_hash(s.sha1, 40)) problems.push_back(std::format("spu_programs[{}]: sha1 is not 40 hex digits", i));
        if (trim(s.name).empty()) problems.push_back(std::format("spu_programs[{}]: name is empty", i));
        if (!spu_hashes.insert(s.sha1).second) problems.push_back(std::format("spu_programs[{}]: sha1 {} is listed twice", i, s.sha1));
    }
    return problems;
}

Result<GameManifest> parse_manifest(std::string_view toml_text, const ManifestOptions& options)
{
    auto doc = toml::parse(toml_text);
    if (!doc) return make_error("manifest: {}", doc.error().message);
    const Value& root = *doc;

    std::vector<std::string> problems;
    Reader r(problems);
    GameManifest m;

    r.check_keys(root, {"title_id", "title", "app_version", "iso_sha256", "files", "overrides", "patches", "spu_programs"}, "manifest");
    if (auto v = r.string(root, "title_id", "manifest")) m.title_id = *v;
    if (auto v = r.string(root, "title", "manifest")) m.title = *v;
    if (auto v = r.string(root, "app_version", "manifest")) m.app_version = *v;
    if (auto v = r.string(root, "iso_sha256", "manifest")) m.iso_sha256 = lowercase(*v);

    const auto files = r.tables(root, "files");
    for (usize i = 0; i < files.size(); i++) {
        const std::string where = std::format("files[{}]", i);
        r.check_keys(*files[i], {"path", "sha256"}, where);
        ManifestFile f;
        if (auto v = r.string(*files[i], "path", where)) f.path = *v;
        if (auto v = r.string(*files[i], "sha256", where)) f.sha256 = lowercase(*v);
        m.files.push_back(std::move(f));
    }

    const auto overrides = r.tables(root, "overrides");
    for (usize i = 0; i < overrides.size(); i++) {
        const std::string where = std::format("overrides[{}]", i);
        r.check_keys(*overrides[i], {"addr", "name"}, where);
        FunctionOverride o;
        if (auto v = r.address(*overrides[i], "addr", where)) o.addr = *v;
        if (auto v = r.string(*overrides[i], "name", where)) o.name = *v;
        m.overrides.push_back(std::move(o));
    }

    const auto patches = r.tables(root, "patches");
    for (usize i = 0; i < patches.size(); i++) {
        const std::string where = std::format("patches[{}]", i);
        r.check_keys(*patches[i], {"addr", "bytes", "why"}, where);
        ManifestPatch p;
        if (auto v = r.address(*patches[i], "addr", where)) p.addr = *v;
        if (auto v = r.string(*patches[i], "bytes", where)) {
            std::string compact;
            for (const char c : *v) {
                if (c != ' ' && c != '\t' && c != '_') compact.push_back(c);
            }
            if (!parse_hex_bytes(compact, p.bytes)) problems.push_back(std::format("{}: bytes '{}' is not an even number of hex digits", where, *v));
        }
        if (auto v = r.string(*patches[i], "why", where)) p.why = *v;
        m.patches.push_back(std::move(p));
    }

    const auto spu = r.tables(root, "spu_programs");
    for (usize i = 0; i < spu.size(); i++) {
        const std::string where = std::format("spu_programs[{}]", i);
        r.check_keys(*spu[i], {"sha1", "name", "action"}, where);
        SpuProgramEntry s;
        if (auto v = r.string(*spu[i], "sha1", where)) s.sha1 = lowercase(*v);
        if (auto v = r.string(*spu[i], "name", where)) s.name = *v;
        if (auto v = r.string(*spu[i], "action", where)) {
            if (*v == "lift") {
                s.action = SpuAction::Lift;
            } else if (*v == "replace") {
                s.action = SpuAction::Replace;
            } else if (*v == "interpret") {
                s.action = SpuAction::Interpret;
            } else {
                problems.push_back(std::format("{}: action '{}' is not lift, replace or interpret", where, *v));
            }
        }
        m.spu_programs.push_back(std::move(s));
    }

    if (problems.empty()) {
        for (auto& p : validate_manifest(m, options)) problems.push_back(std::move(p));
    }
    if (!problems.empty()) {
        std::string message = "manifest is invalid:";
        for (const auto& p : problems) message += "\n  " + p;
        return Error{std::move(message)};
    }
    return m;
}

Result<GameManifest> load_manifest(const std::filesystem::path& path, const ManifestOptions& options)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return make_error("cannot open {}", path.string());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto m = parse_manifest(text, options);
    if (!m) return make_error("{}: {}", path.string(), m.error().message);
    return m;
}

} // namespace pspocket::recomp
