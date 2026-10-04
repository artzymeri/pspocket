// sceNpTrophy's files (see trophy_files.hpp). References: RPCS3 rpcs3/Loader/TRP.cpp (TRPLoader),
// rpcs3/Loader/TROPUSR.cpp (TROPUSRLoader, trophy_xml_document), rpcs3/Utilities/rXml.cpp and the pugixml behaviour
// it relies on.

#include "trophy_files.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/hash.hpp"

namespace pspocket::hle::trophy {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kChan = "trophy";

void set_error(std::string* error, std::string message)
{
    if (error) *error = std::move(message);
}

std::optional<std::vector<u8>> read_host_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return data;
}

// Writes `data` to a temporary file next to `path` and renames it over `path` (RPCS3 fs::pending_file).
bool write_host_file_atomic(const fs::path& path, std::span<const u8> data)
{
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) fs::remove(temp, ec);
    return !ec;
}

// --------------------------------------------------------------------------------------------------- XML parsing

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_name_start(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u == ':' || u >= 0x80;
}
bool is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
bool all_space(std::string_view s) { return std::all_of(s.begin(), s.end(), is_space); }

void append_utf8(std::string& out, u32 cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// A character reference's code point ("#65", "#x41"), or nullopt.
std::optional<u32> char_reference(std::string_view ref)
{
    if (ref.size() < 2 || ref[0] != '#') return std::nullopt;
    const bool hex = ref[1] == 'x';
    const std::string_view digits = ref.substr(hex ? 2 : 1);
    if (digits.empty()) return std::nullopt;
    u32 cp = 0;
    for (const char c : digits) {
        u32 d = 0;
        if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
        else if (hex && c >= 'a' && c <= 'f') d = static_cast<u32>(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F') d = static_cast<u32>(c - 'A' + 10);
        else return std::nullopt;
        cp = cp * (hex ? 16 : 10) + d;
        if (cp > 0x10FFFF) return std::nullopt;
    }
    return cp;
}

// pugixml's conversions with parse_default: entity and character references decoded (anything else after '&' stays
// as it is), line ends normalized to \n in text, and in attribute values every whitespace character (a \r\n pair
// counting as one) turned into a space.
std::string decode(std::string_view raw, bool attribute)
{
    std::string out;
    out.reserve(raw.size());
    for (usize i = 0; i < raw.size(); i++) {
        const char c = raw[i];
        if (c == '&') {
            const usize semi = raw.find(';', i + 1);
            if (semi != std::string_view::npos) {
                const std::string_view ref = raw.substr(i + 1, semi - i - 1);
                const char* named = ref == "lt"     ? "<"
                                    : ref == "gt"   ? ">"
                                    : ref == "amp"  ? "&"
                                    : ref == "apos" ? "'"
                                    : ref == "quot" ? "\""
                                                    : nullptr;
                if (named) {
                    out += named;
                    i = semi;
                    continue;
                }
                if (const std::optional<u32> cp = char_reference(ref)) {
                    append_utf8(out, *cp);
                    i = semi;
                    continue;
                }
            }
            out += c;
        } else if (c == '\r') {
            out += attribute ? ' ' : '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n') i++;
        } else if (attribute && is_space(c)) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

class XmlParser {
public:
    explicit XmlParser(std::string_view s) : s_(s) {}

    std::optional<XmlNode> document()
    {
        if (s_.starts_with("\xEF\xBB\xBF")) i_ = 3; // UTF-8 byte order mark
        XmlNode doc;
        if (!content(doc, true, 0)) return std::nullopt;
        const bool has_element = std::any_of(doc.children.begin(), doc.children.end(),
            [](const XmlNode& n) { return !n.text_node; });
        if (!has_element) return std::nullopt; // pugixml: status_no_document_element
        return doc;
    }

private:
    static constexpr int kMaxDepth = 256;

    bool at(std::string_view token) const { return s_.substr(i_).starts_with(token); }

    // Moves past `end`, which must follow; false at the end of the input.
    bool skip_past(std::string_view end)
    {
        const usize pos = s_.find(end, i_);
        if (pos == std::string_view::npos) return false;
        i_ = pos + end.size();
        return true;
    }

    std::string_view name()
    {
        const usize start = i_;
        if (i_ >= s_.size() || !is_name_start(s_[i_])) return {};
        while (i_ < s_.size() && is_name_char(s_[i_])) i_++;
        return s_.substr(start, i_ - start);
    }

    void skip_space()
    {
        while (i_ < s_.size() && is_space(s_[i_])) i_++;
    }

    // <!DOCTYPE ...> and other declarations, with a bracketed internal subset and quoted strings.
    bool skip_declaration()
    {
        int brackets = 0;
        for (i_ += 2; i_ < s_.size(); i_++) {
            const char c = s_[i_];
            if (c == '"' || c == '\'') {
                const usize close = s_.find(c, i_ + 1);
                if (close == std::string_view::npos) return false;
                i_ = close;
            } else if (c == '[') {
                brackets++;
            } else if (c == ']') {
                brackets--;
            } else if (c == '>' && brackets <= 0) {
                i_++;
                return true;
            }
        }
        return false;
    }

    // The children of `parent` up to its end tag (the end of the input for the document).
    bool content(XmlNode& parent, bool document_level, int depth)
    {
        for (;;) {
            if (i_ >= s_.size()) return document_level; // an element left open
            if (s_[i_] != '<') {
                const usize end = std::min(s_.find('<', i_), s_.size());
                const std::string_view raw = s_.substr(i_, end - i_);
                i_ = end;
                // pugixml keeps text only inside elements, and not when it is all whitespace (no parse_ws_pcdata).
                if (!document_level && !all_space(raw)) {
                    XmlNode text;
                    text.text_node = true;
                    text.value = decode(raw, false);
                    parent.children.push_back(std::move(text));
                }
                continue;
            }
            if (at("<!--")) {
                if (!skip_past("-->")) return false;
            } else if (at("<![CDATA[")) {
                const usize start = i_ + 9;
                if (!skip_past("]]>")) return false;
                if (!document_level) {
                    XmlNode text;
                    text.text_node = true;
                    std::string value(s_.substr(start, i_ - 3 - start));
                    // parse_eol applies to CDATA too.
                    for (usize p = 0; (p = value.find('\r', p)) != std::string::npos;) {
                        if (p + 1 < value.size() && value[p + 1] == '\n') value.erase(p, 1);
                        else value[p++] = '\n';
                    }
                    text.value = std::move(value);
                    parent.children.push_back(std::move(text));
                }
            } else if (at("<!")) {
                if (!skip_declaration()) return false;
            } else if (at("<?")) {
                if (!skip_past("?>")) return false;
            } else if (at("</")) {
                if (document_level) return false;
                i_ += 2;
                const std::string_view end_name = name();
                skip_space();
                if (end_name != parent.name || i_ >= s_.size() || s_[i_] != '>') return false;
                i_++;
                return true;
            } else {
                if (depth >= kMaxDepth) return false;
                XmlNode child;
                if (!element(child, depth + 1)) return false;
                parent.children.push_back(std::move(child));
            }
        }
    }

    // A start tag at '<', its attributes, and its content.
    bool element(XmlNode& out, int depth)
    {
        i_++;
        const std::string_view tag = name();
        if (tag.empty()) return false;
        out.name = std::string(tag);
        for (;;) {
            skip_space();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == '/') {
                if (i_ + 1 >= s_.size() || s_[i_ + 1] != '>') return false;
                i_ += 2;
                return true;
            }
            if (s_[i_] == '>') {
                i_++;
                return content(out, false, depth);
            }
            const std::string_view attr = name();
            if (attr.empty()) return false;
            skip_space();
            if (i_ >= s_.size() || s_[i_] != '=') return false;
            i_++;
            skip_space();
            if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) return false;
            const usize close = s_.find(s_[i_], i_ + 1);
            if (close == std::string_view::npos) return false;
            out.attributes.emplace_back(std::string(attr), decode(s_.substr(i_ + 1, close - i_ - 1), true));
            i_ = close + 1;
        }
    }

    std::string_view s_;
    usize i_ = 0;
};

// ----------------------------------------------------------------------------------------------- TRP and TROPUSR

constexpr u32 kTrpMagic = 0xDCA24D00;
constexpr u32 kTrpHeaderSize = 0x40, kTrpEntrySize = 0x40, kTrpNameSize = 32, kTrpSha1 = 0x1C;

constexpr u32 kTropUsrMagic = 0x818F54AD;
constexpr u32 kTropUsrHeaderSize = 0x30, kTableHeaderSize = 0x20;
// Entry fields (after the 16-byte entry header: type, size, index, 0).
constexpr u32 kE4TrophyId = 0x10, kE4Grade = 0x14, kE4Pid = 0x18;
constexpr u32 kE6TrophyId = 0x10, kE6State = 0x14, kE6Timestamp1 = 0x20, kE6Timestamp2 = 0x28;

u32 grade_of(std::string_view ttype)
{
    switch (ttype.empty() ? '\0' : ttype[0]) {
    case 'B': return TropUsr::Bronze;
    case 'S': return TropUsr::Silver;
    case 'G': return TropUsr::Gold;
    case 'P': return TropUsr::Platinum;
    }
    return TropUsr::Unknown;
}

// The parsed configuration file, or nullopt (logged).
std::optional<XmlNode> read_config(const fs::path& conf, std::string_view who)
{
    const std::optional<std::vector<u8>> data = read_host_file(conf);
    if (!data) {
        PS_LOG_ERROR(kChan, "{}: cannot read {}", who, conf.string());
        return std::nullopt;
    }
    std::optional<XmlNode> doc = parse_xml(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()));
    if (!doc) PS_LOG_ERROR(kChan, "{}: {} is not a well-formed XML document", who, conf.string());
    return doc;
}

std::atomic<u64> g_install_serial{0};

} // namespace

// ------------------------------------------------------------------------------------------------------------ XML

std::string XmlNode::attribute(std::string_view attr) const
{
    for (const auto& [key, text] : attributes)
        if (key == attr) return text;
    return {};
}

std::string XmlNode::content() const
{
    for (const XmlNode& child : children)
        if (child.text_node) return child.value;
    return {};
}

std::optional<XmlNode> parse_xml(std::string_view data) { return XmlParser(data).document(); }

const XmlNode& trophy_root(const XmlNode& document)
{
    if (!document.children.empty() && document.children.front().name == "trophyconf") return document.children.front();
    PS_LOG_ERROR(kChan, "trophy configuration: the root element is <{}>, not <trophyconf>",
        document.children.empty() ? std::string() : document.children.front().name);
    return document;
}

s32 atoi(std::string_view s)
{
    usize i = 0;
    while (i < s.size() && (is_space(s[i]) || s[i] == '\v' || s[i] == '\f')) i++;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) negative = s[i++] == '-';
    u64 value = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) value = value * 10 + static_cast<u64>(s[i] - '0');
    return static_cast<s32>(static_cast<u32>(negative ? 0 - value : value));
}

// ------------------------------------------------------------------------------------------------------ TROPHY.TRP

std::optional<TrpArchive> TrpArchive::parse(std::vector<u8> file, std::string* error)
{
    if (file.size() < kTrpHeaderSize) {
        set_error(error, std::format("{} bytes, shorter than the header", file.size()));
        return std::nullopt;
    }
    const u8* h = file.data();
    if (ps_ld32(h) != kTrpMagic) {
        set_error(error, std::format("magic {:08x}, not {:08x}", ps_ld32(h), kTrpMagic));
        return std::nullopt;
    }
    TrpArchive a;
    a.version_ = ps_ld32(h + 4);
    a.file_size_ = ps_ld64(h + 8);
    a.files_count_ = ps_ld32(h + 16);
    a.element_size_ = ps_ld32(h + 20);
    if (a.version_ >= 2) {
        if (a.file_size_ < kTrpHeaderSize) {
            set_error(error, std::format("version {} with a file size of {}", a.version_, a.file_size_));
            return std::nullopt;
        }
        if (file.size() < a.file_size_) {
            // RPCS3 can't read that much and goes on without checking.
            PS_LOG_WARN(kChan, "TROPHY.TRP: {} bytes, the header says {}: the checksum is not checked", file.size(),
                a.file_size_);
        } else {
            std::vector<u8> copy(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(a.file_size_));
            std::memset(copy.data() + kTrpSha1, 0, 20);
            const recomp::Sha1Digest sha1 = recomp::Sha1::hash(copy);
            if (std::memcmp(sha1.data(), h + kTrpSha1, 20) != 0) {
                set_error(error, "the SHA-1 checksum doesn't match");
                return std::nullopt;
            }
        }
    }
    // The entries follow the header (RPCS3 reads them as 0x40-byte records whatever the header's element size).
    if (u64{kTrpHeaderSize} + u64{a.files_count_} * kTrpEntrySize > file.size()) {
        set_error(error, std::format("{} entries don't fit in {} bytes", a.files_count_, file.size()));
        return std::nullopt;
    }
    for (u32 i = 0; i < a.files_count_; i++) {
        const u8* e = h + kTrpHeaderSize + i * kTrpEntrySize;
        const char* name = reinterpret_cast<const char*>(e);
        a.entries_.push_back({std::string(name, std::find(name, name + kTrpNameSize, '\0')), ps_ld64(e + 32),
            ps_ld64(e + 40)});
    }
    a.file_ = std::move(file);
    return a;
}

u64 TrpArchive::required_space() const
{
    return file_size_ - kTrpHeaderSize - u64{files_count_} * element_size_;
}

bool TrpArchive::contains(std::string_view name) const
{
    if (name.size() >= kTrpNameSize) return false;
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.name == name; });
}

void TrpArchive::remove(std::string_view name)
{
    if (name.size() >= kTrpNameSize) return;
    std::erase_if(entries_, [&](const Entry& e) { return e.name == name; });
}

void TrpArchive::rename(std::string_view from, std::string_view to)
{
    if (from.size() >= kTrpNameSize || to.size() >= kTrpNameSize) return;
    for (Entry& e : entries_)
        if (e.name == from) e.name = std::string(to);
}

bool TrpArchive::install(const fs::path& dest, std::string* error) const
{
    std::error_code ec;
    // RPCS3 creates the user's home directories when it starts; here the trophy directory is made when needed.
    fs::create_directories(dest.parent_path(), ec);
    // RPCS3's temporary name: "<dest>.＄temp＄<unique number>".
    fs::path temp = dest;
    temp += std::format("\xEF\xBC\x84temp\xEF\xBC\x84{}{}",
        std::chrono::steady_clock::now().time_since_epoch().count(), g_install_serial.fetch_add(1));
    if (!fs::create_directory(temp, ec) || ec) {
        set_error(error, std::format("cannot create {} ({})", temp.string(), ec.message()));
        return false;
    }
    // The progress so far stays.
    if (fs::exists(dest / "TROPUSR.DAT", ec)) {
        fs::copy_file(dest / "TROPUSR.DAT", temp / "TROPUSR.DAT", ec);
        if (ec) PS_LOG_ERROR(kChan, "cannot copy {} into the new installation ({})", (dest / "TROPUSR.DAT").string(),
            ec.message());
    }
    bool success = true;
    for (const Entry& e : entries_) {
        if (e.offset > file_.size() || e.size > file_.size() - e.offset) {
            PS_LOG_ERROR(kChan, "TROPHY.TRP: entry {} ({:#x} bytes at {:#x}) is outside the file; skipped", e.name,
                e.size, e.offset);
            continue;
        }
        if (e.name.empty() || e.name == "." || e.name == ".." || e.name.find_first_of("/\\") != std::string::npos) {
            set_error(error, std::format("entry \"{}\" would be written outside {}", e.name, temp.string()));
            success = false;
            break;
        }
        const fs::path file = temp / e.name;
        if (fs::exists(file, ec)) { // RPCS3 creates each file exclusively: a second entry with a name fails
            set_error(error, std::format("entry {} is in the pack twice", e.name));
            success = false;
            break;
        }
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file_.data() + e.offset), static_cast<std::streamsize>(e.size));
        if (!out) {
            set_error(error, std::format("cannot write {}", file.string()));
            success = false;
            break;
        }
    }
    if (success) {
        fs::remove_all(dest, ec);
        if (ec) {
            set_error(error, std::format("cannot remove {} ({})", dest.string(), ec.message()));
            success = false;
        } else {
            fs::rename(temp, dest, ec);
            if (ec) {
                set_error(error, std::format("cannot move {} to {} ({})", temp.string(), dest.string(), ec.message()));
                success = false;
            }
        }
    }
    if (!success) fs::remove_all(temp, ec);
    return success;
}

// ----------------------------------------------------------------------------------------------------- TROPUSR.DAT

TropUsr::LoadResult TropUsr::load(const fs::path& usr, const fs::path& conf, std::string* error)
{
    LoadResult res;
    std::error_code ec;
    if (fs::exists(usr, ec)) {
        if (read(usr)) {
            res.success = true;
            return res;
        }
        res.discarded_existing = true;
        PS_LOG_ERROR(kChan, "{} is not a valid TROPUSR.DAT: generated again, with every trophy locked", usr.string());
    }
    res.success = generate(usr, conf, error);
    return res;
}

bool TropUsr::read(const fs::path& usr)
{
    const std::optional<std::vector<u8>> data = read_host_file(usr);
    if (!data || data->size() < kTropUsrHeaderSize || ps_ld32(data->data()) != kTropUsrMagic) return false;
    const u8* d = data->data();
    const u64 size = data->size();
    const u32 tables = ps_ld32(d + 8);
    if (kTropUsrHeaderSize + u64{tables} * kTableHeaderSize > size) return false;
    std::vector<std::array<u8, kEntry4Size>> t4;
    std::vector<std::array<u8, kEntry6Size>> t6;
    for (u32 i = 0; i < tables; i++) {
        const u8* th = d + kTropUsrHeaderSize + i * kTableHeaderSize;
        const u32 type = ps_ld32(th);
        const u64 count = ps_ld32(th + 12);
        const u64 offset = ps_ld64(th + 16);
        if (type == 4) {
            if (offset > size || count * kEntry4Size > size - offset) return false;
            t4.assign(static_cast<usize>(count), std::array<u8, kEntry4Size>{});
            for (u64 e = 0; e < count; e++) std::memcpy(t4[e].data(), d + offset + e * kEntry4Size, kEntry4Size);
        } else if (type == 6) {
            if (offset > size || count * kEntry6Size > size - offset) return false;
            t6.assign(static_cast<usize>(count), std::array<u8, kEntry6Size>{});
            for (u64 e = 0; e < count; e++) std::memcpy(t6[e].data(), d + offset + e * kEntry6Size, kEntry6Size);
        }
    }
    table4_ = std::move(t4);
    table6_ = std::move(t6);
    return true;
}

bool TropUsr::generate(const fs::path& usr, const fs::path& conf, std::string* error)
{
    const std::optional<XmlNode> doc = read_config(conf, "TROPUSR.DAT");
    if (!doc) {
        set_error(error, std::format("cannot read the trophy configuration {}", conf.string()));
        return false;
    }
    table4_.clear();
    table6_.clear();
    for (const XmlNode& n : trophy_root(*doc).children) {
        if (n.text_node || n.name != "trophy") continue;
        std::array<u8, kEntry4Size> e4{};
        ps_st32(e4.data() + 0, 4);
        ps_st32(e4.data() + 4, kEntry4Size - 0x10);
        ps_st32(e4.data() + 8, static_cast<u32>(table4_.size()));
        ps_st32(e4.data() + kE4TrophyId, static_cast<u32>(atoi(n.attribute("id"))));
        ps_st32(e4.data() + kE4Grade, grade_of(n.attribute("ttype")));
        ps_st32(e4.data() + kE4Pid, static_cast<u32>(atoi(n.attribute("pid"))));
        std::array<u8, kEntry6Size> e6{};
        ps_st32(e6.data() + 0, 6);
        ps_st32(e6.data() + 4, kEntry6Size - 0x10);
        ps_st32(e6.data() + 8, static_cast<u32>(table6_.size()));
        ps_st32(e6.data() + kE6TrophyId, static_cast<u32>(atoi(n.attribute("id"))));
        table4_.push_back(e4);
        table6_.push_back(e6);
    }
    if (!save(usr)) {
        set_error(error, std::format("cannot write {}", usr.string()));
        return false;
    }
    return true;
}

bool TropUsr::save(const fs::path& usr) const
{
    const u64 offset4 = kTropUsrHeaderSize + 2 * kTableHeaderSize;
    const u64 offset6 = offset4 + table4_.size() * kEntry4Size;
    std::vector<u8> out(offset6 + table6_.size() * kEntry6Size, 0);
    u8* o = out.data();
    ps_st32(o + 0, kTropUsrMagic);
    ps_st32(o + 4, 0x00010000);
    ps_st32(o + 8, 2); // tables
    const auto table_header = [&](u32 index, u32 type, u32 entry_size, usize count, u64 offset) {
        u8* th = o + kTropUsrHeaderSize + index * kTableHeaderSize;
        ps_st32(th + 0, type);
        ps_st32(th + 4, entry_size - 0x10);
        ps_st32(th + 8, 1);
        ps_st32(th + 12, static_cast<u32>(count));
        ps_st64(th + 16, offset);
    };
    table_header(0, 4, kEntry4Size, table4_.size(), offset4);
    table_header(1, 6, kEntry6Size, table6_.size(), offset6);
    for (usize i = 0; i < table4_.size(); i++) {
        std::memcpy(o + offset4 + i * kEntry4Size, table4_[i].data(), kEntry4Size);
    }
    for (usize i = 0; i < table6_.size(); i++) {
        std::memcpy(o + offset6 + i * kEntry6Size, table6_[i].data(), kEntry6Size);
    }
    return write_host_file_atomic(usr, out);
}

u32 TropUsr::unlocked_count() const
{
    return static_cast<u32>(std::count_if(table6_.begin(), table6_.end(),
        [](const std::array<u8, kEntry6Size>& e) { return ps_ld32(e.data() + kE6State) != 0; }));
}

u32 TropUsr::grade(u32 id) const
{
    if (id >= table4_.size()) {
        PS_LOG_WARN(kChan, "TROPUSR.DAT: no grade for trophy {}", id);
        return Unknown;
    }
    return ps_ld32(table4_[id].data() + kE4Grade);
}

bool TropUsr::unlocked(u32 id) const
{
    if (id >= table6_.size()) {
        PS_LOG_WARN(kChan, "TROPUSR.DAT: no state for trophy {}", id);
        return false;
    }
    return ps_ld32(table6_[id].data() + kE6State) != 0;
}

u64 TropUsr::timestamp(u32 id) const
{
    if (id >= table6_.size()) {
        PS_LOG_WARN(kChan, "TROPUSR.DAT: no timestamp for trophy {}", id);
        return 0;
    }
    return ps_ld64(table6_[id].data() + kE6Timestamp2);
}

bool TropUsr::unlock(u32 id, u64 timestamp1, u64 timestamp2)
{
    if (id >= table6_.size()) {
        PS_LOG_WARN(kChan, "TROPUSR.DAT: cannot unlock trophy {}", id);
        return false;
    }
    u8* e = table6_[id].data();
    ps_st32(e + kE6State, 1);
    ps_st64(e + kE6Timestamp1, timestamp1);
    ps_st64(e + kE6Timestamp2, timestamp2);
    return true;
}

u32 TropUsr::unlocked_platinum_id(u32 trophy_id, const fs::path& conf)
{
    if (trophy_id >= table6_.size() || trophy_id >= table4_.size()) {
        PS_LOG_WARN(kChan, "TROPUSR.DAT: no trophy {} for the platinum check", trophy_id);
        return kInvalidTrophyId;
    }
    if (table6_.size() != table4_.size()) {
        PS_LOG_WARN(kChan, "TROPUSR.DAT: {} grades for {} states", table4_.size(), table6_.size());
        return kInvalidTrophyId;
    }
    const std::optional<XmlNode> doc = read_config(conf, "platinum check");
    if (!doc) return kInvalidTrophyId;
    const usize count = table4_.size();
    for (const XmlNode& n : trophy_root(*doc).children) {
        if (n.text_node || n.name != "trophy") continue;
        const u32 id = static_cast<u32>(atoi(n.attribute("id")));
        // Assumes the trophies are in ID order (RPCS3).
        if (id < count && ps_ld32(table4_[id].data() + kE4TrophyId) == id) {
            ps_st32(table4_[id].data() + kE4Pid, static_cast<u32>(atoi(n.attribute("pid"))));
        }
    }
    const u32 pid = ps_ld32(table4_[trophy_id].data() + kE4Pid);
    if (pid == kInvalidTrophyId || unlocked(pid)) return kInvalidTrophyId;
    for (usize i = 0; i < count; i++) {
        if (ps_ld32(table4_[i].data() + kE4Pid) == pid && ps_ld32(table6_[i].data() + kE6State) == 0) {
            return kInvalidTrophyId;
        }
    }
    return pid;
}

} // namespace pspocket::hle::trophy
