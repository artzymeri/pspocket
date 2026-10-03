#include "pspocket/recomp/nid.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <tuple>

#include "embedded_data.hpp"
#include "pspocket/recomp/hash.hpp"

namespace pspocket::recomp {

namespace {

// Suffix appended to the symbol name before hashing (RPCS3 rpcs3/Emu/Cell/PPUModule.cpp, ppu_generate_id).
constexpr u8 kNidSuffix[16] = {0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                               0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A};

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// '#' starts a comment at the start of a line or after whitespace.
std::string_view strip_comment(std::string_view line)
{
    for (usize i = 0; i < line.size(); i++) {
        if (line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) return line.substr(0, i);
    }
    return line;
}

std::vector<std::string_view> split_ws(std::string_view s)
{
    std::vector<std::string_view> out;
    usize i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        const usize start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') i++;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// Calls fn(line_number, line) for each line of text.
template <class Fn>
void for_each_line(std::string_view text, Fn&& fn)
{
    u32 line_no = 0;
    while (!text.empty()) {
        const usize nl = text.find('\n');
        const std::string_view line = nl == std::string_view::npos ? text : text.substr(0, nl);
        line_no++;
        if (!fn(line_no, line)) return;
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
}

std::optional<u32> parse_nid(std::string_view token)
{
    if (token.size() < 3 || token.size() > 10 || token[0] != '0' || (token[1] != 'x' && token[1] != 'X')) {
        return std::nullopt;
    }
    u32 value = 0;
    const auto digits = token.substr(2);
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (ec != std::errc() || ptr != digits.data() + digits.size()) return std::nullopt;
    return value;
}

Result<std::string> read_text_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return make_error("cannot open {}", path.string());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return make_error("cannot read {}", path.string());
    return text;
}

} // namespace

u32 nid_of(std::string_view name)
{
    Sha1 sha;
    sha.update(name);
    sha.update(std::span<const u8>(kNidSuffix));
    const Sha1Digest d = sha.finish();
    return static_cast<u32>(d[0]) | (static_cast<u32>(d[1]) << 8) | (static_cast<u32>(d[2]) << 16) |
           (static_cast<u32>(d[3]) << 24);
}

std::string_view nid_match_name(NidMatch match)
{
    switch (match) {
    case NidMatch::Exact: return "exact";
    case NidMatch::OtherModule: return "other_module";
    case NidMatch::Unknown: return "unknown";
    }
    return "unknown";
}

// ---- NidDb ----

const NidDb& NidDb::builtin()
{
    static const NidDb db = [] {
        NidDb d;
        const auto loaded = d.load_text(embedded::nids_txt(), "nids.txt (embedded)");
        if (!loaded) PS_FATAL("nid", "embedded NID database is malformed: {}", loaded.error().message);
        return d;
    }();
    return db;
}

Result<usize> NidDb::load_text(std::string_view text, std::string_view source_name)
{
    // Parse everything first so a malformed file leaves the database unchanged.
    std::vector<std::tuple<std::string, u32, std::string>> staged;
    std::optional<std::string> module;
    std::optional<Error> error;

    for_each_line(text, [&](u32 line_no, std::string_view raw) {
        const std::string_view line = trim(strip_comment(raw));
        if (line.empty()) return true;
        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 2) {
                error = make_error("{}:{}: malformed module header '{}'", source_name, line_no, line);
                return false;
            }
            const std::string_view name = trim(line.substr(1, line.size() - 2));
            if (name.find_first_of("[]") != std::string_view::npos) {
                error = make_error("{}:{}: malformed module header '{}'", source_name, line_no, line);
                return false;
            }
            module = std::string(name);
            return true;
        }
        if (!module) {
            error = make_error("{}:{}: entry before any [module] header", source_name, line_no);
            return false;
        }
        const auto tokens = split_ws(line);
        if (tokens.size() == 1) {
            staged.emplace_back(*module, nid_of(tokens[0]), std::string(tokens[0]));
        } else if (tokens.size() == 2) {
            const auto nid = parse_nid(tokens[0]);
            if (!nid) {
                error = make_error("{}:{}: '{}' is not a NID (expected 0xNNNNNNNN)", source_name, line_no, tokens[0]);
                return false;
            }
            staged.emplace_back(*module, *nid, std::string(tokens[1]));
        } else {
            error = make_error("{}:{}: expected `name` or `0xNNNNNNNN name`", source_name, line_no);
            return false;
        }
        return true;
    });
    if (error) return *error;

    const usize before = entries_;
    for (const auto& [mod, nid, name] : staged) add(mod, nid, name);
    return entries_ - before;
}

Result<usize> NidDb::load_file(const std::filesystem::path& path)
{
    auto text = read_text_file(path);
    if (!text) return text.error();
    return load_text(*text, path.string());
}

void NidDb::add(std::string_view module, u32 nid, std::string_view name)
{
    auto it = modules_.find(module);
    if (it == modules_.end()) it = modules_.emplace(std::string(module), std::unordered_map<u32, std::vector<u32>>{}).first;
    auto& list = it->second[nid];
    for (const u32 index : list) {
        if (names_[index] == name) return;
    }
    names_.emplace_back(name);
    const auto index = static_cast<u32>(names_.size() - 1);
    list.push_back(index);
    entries_++;

    auto& any = any_[nid];
    for (const u32 other : any) {
        if (names_[other] == name) return;
    }
    any.push_back(index);
}

std::optional<std::string_view> NidDb::find(std::string_view module, u32 nid) const
{
    const auto it = modules_.find(module);
    if (it == modules_.end()) return std::nullopt;
    const auto entry = it->second.find(nid);
    if (entry == it->second.end() || entry->second.empty()) return std::nullopt;
    return std::string_view(names_[entry->second.front()]);
}

std::vector<std::string_view> NidDb::find_any(u32 nid) const
{
    std::vector<std::string_view> out;
    const auto it = any_.find(nid);
    if (it == any_.end()) return out;
    out.reserve(it->second.size());
    for (const u32 index : it->second) out.emplace_back(names_[index]);
    return out;
}

NidResolution NidDb::resolve(std::string_view module, u32 nid) const
{
    if (const auto exact = find(module, nid)) return {*exact, NidMatch::Exact};
    const auto it = any_.find(nid);
    if (it != any_.end() && !it->second.empty()) return {names_[it->second.front()], NidMatch::OtherModule};
    return {};
}

bool NidDb::has_module(std::string_view module) const { return modules_.find(module) != modules_.end(); }

// ---- SyscallTable ----

const SyscallTable& SyscallTable::builtin()
{
    static const SyscallTable table = [] {
        SyscallTable t;
        const auto loaded = t.load_text(embedded::syscalls_txt(), "syscalls.txt (embedded)");
        if (!loaded) PS_FATAL("nid", "embedded syscall table is malformed: {}", loaded.error().message);
        return t;
    }();
    return table;
}

Result<usize> SyscallTable::load_text(std::string_view text, std::string_view source_name)
{
    std::vector<std::pair<u32, std::string>> staged;
    std::optional<Error> error;
    for_each_line(text, [&](u32 line_no, std::string_view raw) {
        const std::string_view line = trim(strip_comment(raw));
        if (line.empty()) return true;
        const auto tokens = split_ws(line);
        u32 number = 0;
        const auto [ptr, ec] = tokens.empty() ? std::from_chars_result{nullptr, std::errc::invalid_argument}
                                              : std::from_chars(tokens[0].data(), tokens[0].data() + tokens[0].size(), number);
        if (tokens.size() != 2 || ec != std::errc() || ptr != tokens[0].data() + tokens[0].size()) {
            error = make_error("{}:{}: expected `number name`", source_name, line_no);
            return false;
        }
        staged.emplace_back(number, std::string(tokens[1]));
        return true;
    });
    if (error) return *error;
    for (auto& [number, name] : staged) {
        by_name_.emplace(name, number); // the first number wins for names RPCS3 lists twice
        by_number_[number] = std::move(name);
    }
    return staged.size();
}

std::optional<std::string_view> SyscallTable::name(u32 number) const
{
    const auto it = by_number_.find(number);
    if (it == by_number_.end()) return std::nullopt;
    return std::string_view(it->second);
}

std::optional<u32> SyscallTable::number(std::string_view name) const
{
    const auto it = by_name_.find(name);
    if (it == by_name_.end()) return std::nullopt;
    return it->second;
}

} // namespace pspocket::recomp
