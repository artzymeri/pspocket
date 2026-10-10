// PS3 packages (D-583). See pspocket/recomp/pkg.hpp. As RPCS3's package_reader (rpcs3/Crypto/unpkg.cpp, unpkg.h),
// and its PARAM.SFO loader for an update's versions (rpcs3/Loader/PSF.cpp load).

#include "pspocket/recomp/pkg.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/crypto.hpp"
#include "pspocket/recomp/hash.hpp"

namespace pspocket::recomp {

namespace {

namespace fs = std::filesystem;

// RPCS3 unpkg.h.
constexpr usize kHeaderSize = 0x80;   // sizeof(PKGHeader), up to the counter block
constexpr u32 kMagic = 0x7F504B47;    // "\x7FPKG"
constexpr u16 kReleaseRetail = 0x8000, kReleaseDebug = 0x0000;
constexpr u16 kPlatformPs3 = 1, kPlatformPspVita = 2;
constexpr u32 kEntrySize = 0x20;      // sizeof(PKGEntry)
constexpr u32 kMaxName = 256;         // PKG_MAX_FILENAME_SIZE
constexpr u32 kMaxFiles = 0x7FFFFFFF; // PKG_MAX_FILE_COUNT
constexpr u32 kMaxInstallDirPacket = 0x1000; // a larger metadata packet 0xA is passed over
constexpr u64 kMaxSfo = 1u << 20;     // a larger PARAM.SFO is not read
constexpr usize kChunk = 1u << 20;    // the bytes read and written at a time

// The package's bytes, across the files of a split package (RPCS3 read_header's fs::make_gather). One thread at a
// time.
class Source {
public:
    bool add(const fs::path& path, std::string& why)
    {
        auto part = std::make_unique<std::ifstream>(path, std::ios::binary);
        std::error_code ec;
        const u64 size = fs::file_size(path, ec);
        if (!*part || ec) {
            why = "cannot read " + path.string();
            return false;
        }
        parts_.push_back({std::move(part), total_, size});
        total_ += size;
        return true;
    }
    u64 size() const { return total_; }
    usize count() const { return parts_.size(); }
    // Exactly `n` bytes at `offset`, or false.
    bool read(u64 offset, void* dst, usize n) const
    {
        if (offset > total_ || n > total_ - offset) return false;
        u8* to = static_cast<u8*>(dst);
        for (const Part& p : parts_) {
            if (!n) break;
            if (offset >= p.start + p.size) continue;
            const u64 at = offset - p.start;
            const usize take = static_cast<usize>(std::min<u64>(n, p.size - at));
            p.file->clear();
            p.file->seekg(static_cast<std::streamoff>(at));
            if (!p.file->read(reinterpret_cast<char*>(to), static_cast<std::streamsize>(take))) return false;
            to += take;
            offset += take;
            n -= take;
        }
        return n == 0;
    }

private:
    struct Part {
        std::unique_ptr<std::ifstream> file;
        u64 start = 0, size = 0;
    };
    std::vector<Part> parts_;
    u64 total_ = 0;
};

struct Entry {
    u32 name_offset = 0, name_size = 0;
    u64 file_offset = 0, file_size = 0;
    u32 type = 0;
};

// An open package (RPCS3 package_reader after read_header and read_metadata).
struct Package {
    std::string path;
    Source source;
    PkgInfo info;
    u64 data_offset = 0;
    std::array<u8, 16> qa_digest{};
    std::array<u8, 16> counter{}; // the data's first counter block (RPCS3's header "klicensee")
    Key128 key{};                 // set_decryption_key's, for a retail package

    // `n` bytes of the data at `offset` from its start, decrypted (RPCS3 decrypt): retail, XORed with the AES-128
    // encryption of the counter block plus the 16-byte block's number; debug, with the first 16 bytes of SHA-1 over
    // the QA digest's halves (each twice) and the block's number. RPCS3 starts the stream at the block a read starts in
    // and takes reads to start on one, as every entry of a package does; here a read may start anywhere in a block.
    // Bytes past the package's data but in its files read as RPCS3 reads them; past the files, false.
    bool read(u64 offset, u8* dst, usize n) const;
};

// The 128-bit big-endian counter block plus `n`.
std::array<u8, 16> counter_plus(const std::array<u8, 16>& counter, u64 n)
{
    std::array<u8, 16> c = counter;
    u64 carry = n;
    for (int i = 15; i >= 0 && carry; i--) {
        const u64 sum = u64{c[i]} + (carry & 0xFF);
        c[i] = static_cast<u8>(sum);
        carry = (carry >> 8) + (sum >> 8);
    }
    return c;
}

bool Package::read(u64 offset, u8* dst, usize n) const
{
    if (offset > ~data_offset || !source.read(data_offset + offset, dst, n)) return false;
    u64 block = offset / 16;
    usize skip = static_cast<usize>(offset % 16);
    if (info.retail) {
        usize done = 0;
        if (skip) { // the rest of the block the read starts in
            const std::array<u8, 16> c = counter_plus(counter, block++);
            u8 stream[16];
            if (!crypto::aes_ecb_encrypt(key, c.data(), 16, stream)) return false;
            done = std::min<usize>(16 - skip, n);
            for (usize i = 0; i < done; i++) dst[i] ^= stream[skip + i];
        }
        if (done == n) return true;
        const std::array<u8, 16> iv = counter_plus(counter, block);
        return crypto::aes_ctr(key, iv.data(), dst + done, n - done, dst + done);
    }
    u8 input[64]{}; // RPCS3's be_t<u64> input[8]: the digest's first half twice, its second half twice, ..., the block
    std::memcpy(input, qa_digest.data(), 8);
    std::memcpy(input + 8, qa_digest.data(), 8);
    std::memcpy(input + 16, qa_digest.data() + 8, 8);
    std::memcpy(input + 24, qa_digest.data() + 8, 8);
    for (usize done = 0; done < n; block++, skip = 0) {
        ps_st64(input + 56, block);
        const Sha1Digest stream = Sha1::hash(std::span<const u8>(input, sizeof input));
        const usize take = std::min<usize>(16 - skip, n - done);
        for (usize i = 0; i < take; i++) dst[done + i] ^= stream[skip + i];
        done += take;
    }
    return true;
}

// RPCS3 read_header (the split package's parts gathered) and read_metadata (the packets kept: 1, 2, 3 and 0xA).
Result<Package> open_package(const std::string& path)
{
    Package pkg;
    pkg.path = path;
    std::string why;
    if (!pkg.source.add(path, why)) return make_error("{}", why);
    u8 h[kHeaderSize]{};
    if (!pkg.source.read(0, h, sizeof h)) return make_error("{}: too short for a package", path);
    if (ps_ld32(h) != kMagic) return make_error("{}: not a PS3 package (no \"\\x7FPKG\" at its start)", path);
    const u16 release = ps_ld16(h + 4), platform = ps_ld16(h + 6);
    const u32 meta_offset = ps_ld32(h + 8), meta_count = ps_ld32(h + 0x0C), file_count = ps_ld32(h + 0x14);
    const u64 pkg_size = ps_ld64(h + 0x18), data_offset = ps_ld64(h + 0x20), data_size = ps_ld64(h + 0x28);
    PkgInfo& info = pkg.info;
    info.content_id.assign(reinterpret_cast<const char*>(h + 0x30), strnlen(reinterpret_cast<const char*>(h + 0x30), 0x30));
    // The title ID in the content ID ("UP0001-NPUB30000_00-..."), RPCS3's install folder unless the metadata names one.
    info.install_dir.assign(reinterpret_cast<const char*>(h + 0x37), strnlen(reinterpret_cast<const char*>(h + 0x37), 9));
    std::memcpy(pkg.qa_digest.data(), h + 0x60, 16);
    std::memcpy(pkg.counter.data(), h + 0x70, 16);
    if (file_count > kMaxFiles || u64{file_count} > data_size / kEntrySize)
        return make_error("{}: {} files cannot have their table in {:#x} bytes of data", path, file_count, data_size);
    if (release != kReleaseRetail && release != kReleaseDebug)
        return make_error("{}: an unknown package type {:#x}", path, release);
    if (platform == kPlatformPspVita) return make_error("{}: a PSP or PS Vita package, which pspocket does not install", path);
    if (platform != kPlatformPs3) return make_error("{}: an unknown package platform {:#x}", path, platform);
    // A package larger than its file: the parts beside it (NAME_00.pkg, NAME_01.pkg, ...).
    if (pkg_size > pkg.source.size()) {
        if (!path.ends_with("_00.pkg"))
            return make_error("{}: {:#x} bytes of a {:#x}-byte package (the first part of a split one is NAME_00.pkg)",
                path, pkg.source.size(), pkg_size);
        const std::string stem = path.substr(0, path.size() - 7);
        while (pkg.source.size() < pkg_size) {
            const std::string part = std::format("{}_{:02}.pkg", stem, pkg.source.count());
            const u64 before = pkg.source.size();
            if (!pkg.source.add(part, why)) return make_error("a part of the package is missing: {}", part);
            if (pkg.source.size() == before) return make_error("{} is empty", part);
        }
    }
    if (data_size > ~data_offset || data_offset + data_size > pkg_size)
        return make_error("{}: data at {:#x} ({:#x} bytes) past the package's end ({:#x})", path, data_offset, data_size,
            pkg_size);
    info.retail = release == kReleaseRetail;
    info.file_count = file_count;
    info.data_size = data_size;
    info.parts = static_cast<u32>(pkg.source.count());
    pkg.data_offset = data_offset;

    // The metadata packets: an id, a size, the bytes. RPCS3 reads on past a short file; here the packets end there.
    u64 at = meta_offset;
    for (u32 i = 0; i < meta_count; i++) {
        u8 packet[8];
        if (!pkg.source.read(at, packet, sizeof packet)) break;
        const u32 id = ps_ld32(packet), size = ps_ld32(packet + 4);
        at += sizeof packet;
        u8 word[4];
        if (id >= 1 && id <= 3 && size == 4 && pkg.source.read(at, word, 4)) {
            switch (id) {
            case 1: info.drm_type = ps_ld32(word); break;
            case 2: info.content_type = ps_ld32(word); break;
            default: info.package_flags = ps_ld32(word); break;
            }
        } else if (id == 0xA && size > 8 && size <= kMaxInstallDirPacket) {
            // An add-on's install folder: a string after 8 bytes.
            std::vector<char> text(size + 1, '\0');
            if (pkg.source.read(at, text.data(), size)) info.install_dir = std::string(text.data() + 8);
        }
        at += size;
    }
    return pkg;
}

// RPCS3 read_entries: the table decrypted with the package's key, and whether it is sound: each name (at most 256
// bytes) and file inside the package's files after the data's start, and no file where its name is. A wrong key
// gives a table that is not.
bool read_entries(const Package& pkg, std::vector<Entry>& entries)
{
    std::vector<u8> table(usize{pkg.info.file_count} * kEntrySize);
    if (!pkg.read(0, table.data(), table.size())) return false;
    entries.assign(pkg.info.file_count, {});
    const u64 room = pkg.source.size() - pkg.data_offset;
    for (u32 i = 0; i < pkg.info.file_count; i++) {
        const u8* e = table.data() + usize{i} * kEntrySize;
        Entry& entry = entries[i];
        entry.name_offset = ps_ld32(e);
        entry.name_size = ps_ld32(e + 4);
        entry.file_offset = ps_ld64(e + 8);
        entry.file_size = ps_ld64(e + 0x10);
        entry.type = ps_ld32(e + 0x18); // then 4 bytes of padding
        if (!entry.name_size) continue;
        if (entry.name_size > kMaxName) return false;
        if (room < entry.name_size || room - entry.name_size < entry.name_offset) return false;
        if (entry.file_size) {
            if (room < entry.file_size || room - entry.file_size < entry.file_offset) return false;
            if (entry.name_offset == entry.file_offset) return false;
        }
    }
    return true;
}

// RPCS3 leaves_directory: a leading "/", or a "." or ".." part ("/" is the only separator).
bool leaves_directory(std::string_view name)
{
    if (name.starts_with('/')) return true;
    for (std::string_view rest = name; !rest.empty();) {
        const usize sep = rest.find('/');
        const std::string_view part = rest.substr(0, sep);
        rest = sep == std::string_view::npos ? std::string_view{} : rest.substr(sep + 1);
        if (part == "." || part == "..") return true;
    }
    return false;
}

// RPCS3 extract_worker's entry types: the ones it writes as files, and folders (fill_data).
bool is_file_type(u32 type)
{
    switch (type) {
    case kPkgEntryNpdrm: case kPkgEntryNpdrmEdat: case kPkgEntryRegular: case 5: case 6: case kPkgEntrySdat:
    case 0xE: case 0x10: case 0x11: case 0x13: case 0x14: case 0x15: case 0x16: case 0x18: case 0x19:
        return true;
    default: return false;
    }
}

bool is_folder_type(u32 type) { return type == kPkgEntryFolder || type == 0x12; }

// An entry's name: name_size bytes, the NULs at its end dropped (RPCS3 trims them). One with a NUL inside is refused.
Result<std::string> entry_name(const Package& pkg, const Entry& e)
{
    std::string name(e.name_size, '\0');
    if (!pkg.read(e.name_offset, reinterpret_cast<u8*>(name.data()), name.size()))
        return make_error("{}: an entry's name ({} bytes at {:#x}) does not read", pkg.path, e.name_size, e.name_offset);
    while (!name.empty() && name.back() == '\0') name.pop_back();
    if (name.find('\0') != std::string::npos)
        return make_error("{}: an entry's name has a NUL inside (\"{}\")", pkg.path, name.c_str());
    return name;
}

// The entries with their names, in the table's order.
using Items = std::vector<std::pair<std::string, Entry>>;

// ---------------------------------------------------------------------------------------- an update's versions

using SfoStrings = std::map<std::string, std::string, std::less<>>;

u32 le32(std::span<const u8> b, u64 at) { return u32{b[at]} | u32{b[at + 1]} << 8 | u32{b[at + 2]} << 16 | u32{b[at + 3]} << 24; }
u16 le16(std::span<const u8> b, u64 at) { return static_cast<u16>(b[at] | b[at + 1] << 8); }

// The string (and char array) entries of a PARAM.SFO, with RPCS3's checks (Loader/PSF.cpp load): nullopt for a file
// it does not load, which then counts as none (a PSF 1.1 header, the tables and every entry inside the file, no key
// twice, and a CATEGORY RPCS3 knows). A string ends at its first NUL.
std::optional<SfoStrings> sfo_strings(std::span<const u8> b)
{
    const u64 size = b.size();
    if (size < 20 || le32(b, 0) != 0x46535000 /* "\0PSF" */ || le32(b, 4) != 0x101) return std::nullopt;
    const u32 key_table = le32(b, 8), data_table = le32(b, 12), count = le32(b, 16);
    if (key_table < 20 || key_table > data_table || data_table > size) return std::nullopt;
    if (u64{count} * 16 > size - 20) return std::nullopt;
    SfoStrings sfo;
    std::set<std::string, std::less<>> loaded; // the keys loaded: one twice refuses the file
    for (u32 i = 0; i < count; i++) {
        const u64 e = 20 + u64{i} * 16;
        const u32 key_off = le16(b, e), fmt = le16(b, e + 2), len = le32(b, e + 4), max = le32(b, e + 8),
                  data_off = le32(b, e + 12);
        if (key_off >= data_table - key_table) return std::nullopt;
        // The key ends at its NUL, or at the data table.
        const char* key_start = reinterpret_cast<const char*>(b.data() + key_table + key_off);
        const std::string key(key_start, strnlen(key_start, data_table - key_table - key_off));
        if (loaded.contains(key) || len > max || data_off >= size - data_table || max >= size - data_off)
            return std::nullopt;
        const u64 at = u64{data_table} + data_off;
        if (fmt == 0x0404 && max == 4 && len == 4) { // an integer
            if (size - at < 4) return std::nullopt;
            loaded.insert(key);
        } else if (fmt == 0x0204 || fmt == 0x0004) { // a string, a char array
            if (size - at < len) return std::nullopt;
            std::string value(reinterpret_cast<const char*>(b.data() + at), len);
            if (fmt == 0x0204) value.resize(strnlen(value.c_str(), value.size()));
            loaded.insert(key);
            sfo.emplace(key, std::move(value));
        } // RPCS3 passes over other formats
    }
    static constexpr std::string_view kCategories[]{"GD", "DG", "HG", "AM", "AP", "AS", "AT", "AV", "BV", "WT", "HM",
        "CB", "SF", "2P", "2G", "1P", "PP", "MN", "PE", "2D", "SD", "MS"};
    const auto category = sfo.find("CATEGORY");
    if (category == sfo.end() || std::find(std::begin(kCategories), std::end(kCategories), category->second) ==
            std::end(kCategories))
        return std::nullopt;
    return sfo;
}

std::string sfo_get(const SfoStrings& sfo, std::string_view key)
{
    const auto it = sfo.find(key);
    return it == sfo.end() ? std::string() : it->second;
}

// A version ("01.05"): RPCS3 reads it with strtod and refuses what that does not read whole. Here digits with one
// '.' (what PARAM.SFO holds), compared exactly: the whole part, then the fraction.
struct Version {
    std::string whole, fraction; // no leading zeros; no trailing zeros
};

std::optional<Version> parse_version(std::string_view text)
{
    const usize dot = text.find('.');
    std::string_view whole = text.substr(0, dot), fraction = dot == std::string_view::npos ? "" : text.substr(dot + 1);
    const auto digits = [](std::string_view s) { return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
    if (whole.empty() && fraction.empty()) return std::nullopt;
    if (!digits(whole) || !digits(fraction)) return std::nullopt;
    while (!whole.empty() && whole.front() == '0') whole.remove_prefix(1);
    while (!fraction.empty() && fraction.back() == '0') fraction.remove_suffix(1);
    return Version{std::string(whole), std::string(fraction)};
}

int compare(const Version& a, const Version& b)
{
    if (a.whole.size() != b.whole.size()) return a.whole.size() < b.whole.size() ? -1 : 1;
    if (const int c = a.whole.compare(b.whole)) return c < 0 ? -1 : 1;
    const int c = a.fraction.compare(b.fraction); // no trailing zeros: as strings, the shorter one is the smaller
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

// RPCS3 read_param_sfo: the package's PARAM.SFO (an entry with that name at the top), when one loads.
std::optional<SfoStrings> package_sfo(const Package& pkg, const Items& items)
{
    for (const auto& [name, e] : items) {
        if (name != "PARAM.SFO" || !is_file_type(e.type & 0xFF) || e.file_size > kMaxSfo) continue;
        std::vector<u8> bytes(static_cast<usize>(e.file_size));
        if (!pkg.read(e.file_offset, bytes.data(), bytes.size())) continue;
        if (auto sfo = sfo_strings(bytes)) return sfo;
    }
    return std::nullopt;
}

// RPCS3 check_target_app_version: an update (category GD with an APP_VER) must suit the game installed under
// game/<TITLE_ID>: a TARGET_APP_VER must be the installed APP_VER (and needs the game installed); without one, a
// patch (package flag 0x10) must not be older than what is installed; an add-on may be.
std::optional<Error> check_app_version(const Package& pkg, const SfoStrings& sfo, const fs::path& hdd0)
{
    const std::string title_id = sfo_get(sfo, "TITLE_ID"), app_ver = sfo_get(sfo, "APP_VER"),
                      target_ver = sfo_get(sfo, "TARGET_APP_VER");
    if (sfo_get(sfo, "CATEGORY") != "GD" || title_id.empty() || app_ver.empty()) return std::nullopt;
    const fs::path installed_path = hdd0 / "game" / title_id / "PARAM.SFO";
    std::ifstream in(installed_path, std::ios::binary);
    if (!in) {
        if (target_ver.empty()) return std::nullopt;
        return make_error("{}: an update to version {} of {} for version {}, which is not installed (no {})", pkg.path,
            app_ver, title_id, target_ver, installed_path.string());
    }
    std::vector<u8> bytes(static_cast<usize>(kMaxSfo));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<usize>(in.gcount()));
    const SfoStrings installed = sfo_strings(bytes).value_or(SfoStrings{});
    const std::string installed_ver = sfo_get(installed, "APP_VER");
    if (sfo_get(installed, "TITLE_ID") != title_id || installed_ver.empty()) return std::nullopt;
    const auto old_version = parse_version(installed_ver);
    if (!old_version) return make_error("{}: the installed {} has a version that does not read (\"{}\")", pkg.path,
        title_id, installed_ver);
    if (target_ver.empty()) {
        if (!(pkg.info.package_flags & kPkgFlagPatch)) return std::nullopt; // an add-on
        const auto new_version = parse_version(app_ver);
        if (!new_version) return make_error("{}: its version does not read (\"{}\")", pkg.path, app_ver);
        if (compare(*new_version, *old_version) >= 0) return std::nullopt;
        return make_error("{}: an update to version {} of {}, older than the version installed ({})", pkg.path, app_ver,
            title_id, installed_ver);
    }
    const auto target = parse_version(target_ver);
    if (!target) return make_error("{}: the version it updates does not read (\"{}\")", pkg.path, target_ver);
    if (compare(*target, *old_version) == 0) return std::nullopt;
    return make_error("{}: an update to version {} of {} for version {}, and version {} is installed", pkg.path, app_ver,
        title_id, target_ver, installed_ver);
}

// ---------------------------------------------------------------------------------------- installing

// The missing folders of `dir`, made from the deepest one that exists (iOS's sandbox does not let the app look at the
// folders above its container, which create_directories may: D-121).
bool make_folders(const fs::path& dir, std::string& why)
{
    std::error_code ec;
    std::vector<fs::path> missing;
    for (auto p = dir; !p.empty() && !fs::is_directory(p, ec); p = p.parent_path()) {
        missing.push_back(p);
        if (p == p.parent_path()) break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (::mkdir(it->c_str(), 0755) != 0 && errno != EEXIST) {
            why = "cannot make " + it->string() + ": " + std::strerror(errno);
            return false;
        }
        if (!fs::is_directory(*it, ec)) {
            why = "cannot make " + it->string() + ": something that is not a folder is there";
            return false;
        }
    }
    return true;
}

// RPCS3 set_install_path: where a content type's files go under /dev_hdd0, in the install folder but for themes and
// licences. The folder's name must be one name (RPCS3 allows a "/" inside; pspocket does not).
Result<fs::path> install_path(const fs::path& hdd0, const PkgInfo& info)
{
    fs::path dir;
    bool named = true;
    switch (info.content_type) {
    case kPkgContentTheme: dir = hdd0 / "theme"; named = false; break;
    case kPkgContentWidget: dir = hdd0 / "widget"; break;
    case kPkgContentLicense: dir = hdd0 / "home" / "00000001" / "exdata"; named = false; break;
    case kPkgContentVshModule: dir = hdd0 / "vsh" / "modules"; break;
    case kPkgContentAvatar: dir = hdd0 / "home" / "00000001" / "psn_avatar"; break;
    case kPkgContentVmc: dir = hdd0 / "tmp" / "vmc"; break;
    default: dir = hdd0 / "game"; break;
    }
    if (named) {
        const std::string& name = info.install_dir;
        if (name.empty() || leaves_directory(name) || name.find('/') != std::string::npos)
            return make_error("the package's install folder \"{}\" is not a folder name", name);
        dir /= name;
    }
    return dir;
}

// A path as RPCS3 fill_data resolves it: links on disk followed (weakly_canonical), else lexically.
fs::path resolve(const fs::path& path)
{
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    return ec || resolved.empty() ? path.lexically_normal() : resolved;
}

bool inside(const fs::path& base, const fs::path& path)
{
    return std::mismatch(base.begin(), base.end(), path.begin(), path.end()).first == base.end();
}

// One file: `size` bytes from `read`, written to `path` (RPCS3 writes in place: a failed write leaves a part).
bool write_file(const fs::path& path, u64 size, const std::function<bool(u64, u8*, usize)>& read, std::string& why)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        why = "cannot write " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    std::vector<u8> buffer(static_cast<usize>(std::min<u64>(size, kChunk)));
    for (u64 at = 0; at < size;) {
        const usize n = static_cast<usize>(std::min<u64>(kChunk, size - at));
        if (!read(at, buffer.data(), n)) {
            why = "cannot read " + path.filename().string() + " from the package";
            return false;
        }
        if (!out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(n))) {
            why = "cannot write " + path.string();
            return false;
        }
        at += n;
    }
    out.close();
    if (!out) {
        why = "cannot write " + path.string();
        return false;
    }
    return true;
}

// RPCS3 fill_data and extract_worker: the folders made, each file's entry chosen (the first, or a later one marked to
// overwrite), every path checked to stay in the install folder with links on disk followed, then the chosen files
// written in the table's order but those already there that are not marked to overwrite.
std::optional<Error> install_entries(const Package& pkg, const Items& items, const fs::path& dir,
    const NpdKeys& npd_keys, PkgInstalled& result, const std::function<void(std::string_view)>& say)
{
    const fs::path base = resolve(dir);
    std::map<std::string, fs::path, std::less<>> resolved_dirs;
    std::map<fs::path, usize> chosen;              // a file's path: the entry that writes it
    std::vector<std::pair<fs::path, usize>> files; // file entries in the table's order
    std::string why;
    for (usize i = 0; i < items.size(); i++) {
        const auto& [name, e] = items[i];
        const fs::path path = dir / name;
        const bool folder = is_folder_type(e.type & 0xFF);
        std::error_code ec;
        fs::path real;
        if (folder || fs::exists(path, ec)) {
            real = resolve(path);
        } else { // a file to come: its folder resolved once, then its name
            const std::string parent = path.parent_path().string();
            auto found = resolved_dirs.find(parent);
            if (found == resolved_dirs.end()) found = resolved_dirs.emplace(parent, resolve(path.parent_path())).first;
            real = found->second / path.filename();
        }
        if (real.empty() || !inside(base, real))
            return make_error("{}: the entry \"{}\" leaves the install folder ({})", pkg.path, name, real.string());
        if (folder) {
            if (!make_folders(real, why)) return make_error("{}", why);
            resolved_dirs.emplace(path.string(), real);
            result.folders++;
            say("folder " + name);
            continue;
        }
        const auto [it, added] = chosen.try_emplace(real, i);
        if (!added && (e.type & kPkgEntryOverwrite)) it->second = i;
        files.emplace_back(real, i);
    }

    for (const auto& [path, index] : files) {
        if (chosen[path] != index) continue; // another entry writes this file
        const auto& [name, e] = items[index];
        std::error_code ec;
        const bool there = fs::is_regular_file(path, ec);
        if (there && !(e.type & kPkgEntryOverwrite)) {
            result.kept++;
            say("kept " + name + " (already there, and its entry is not marked to overwrite)");
            continue;
        }
        if (!make_folders(path.parent_path(), why)) return make_error("{}", why);
        const u64 offset = e.file_offset, size = e.file_size;
        u64 written = size;
        if ((e.type & 0xFF) == kPkgEntrySdat) {
            // RPCS3 extract_worker: an SDATA file is written decrypted (DecryptEDAT).
            auto sdat = NpdFile::open(
                [&pkg, offset, size](u64 at, void* dst, usize n) {
                    return at <= size && n <= size - at && pkg.read(offset + at, static_cast<u8*>(dst), n);
                },
                size, npd_keys);
            if (!sdat) return make_error("{}: the SDATA file {} does not decrypt: {}", pkg.path, name, sdat.error().message);
            const NpdFile& file = **sdat;
            written = file.size();
            std::string error;
            if (!write_file(path, written, [&](u64 at, u8* dst, usize n) { return file.read(at, dst, n, &error); }, why))
                return make_error("{}{}", why, error.empty() ? "" : ": " + error);
        } else if (!write_file(path, size, [&](u64 at, u8* dst, usize n) { return pkg.read(offset + at, dst, n); }, why)) {
            return make_error("{}", why);
        }
        result.files++;
        result.bytes += written;
        say(std::format("{} {} ({} bytes{})", there ? "replaced" : "file", name, written,
            (e.type & 0xFF) == kPkgEntrySdat ? ", decrypted" : ""));
        if (name == "USRDIR/EBOOT.BIN" && size > 4) result.eboot = path.string();
    }
    return std::nullopt;
}

} // namespace

Result<PkgInfo> read_pkg_info(const std::string& pkg_path)
{
    auto pkg = open_package(pkg_path);
    if (!pkg) return pkg.error();
    return pkg->info;
}

Result<PkgInstalled> install_pkg(const std::string& pkg_path, const std::string& hdd0_dir,
    const std::function<void(std::string_view)>& progress, const PkgKeys& keys, const NpdKeys& npd_keys)
{
    const auto say = [&](std::string_view line) {
        if (progress) progress(line);
    };
    auto opened = open_package(pkg_path);
    if (!opened) return opened.error();
    Package& pkg = *opened;
    PkgInstalled result;
    result.info = pkg.info;

    // RPCS3 set_decryption_key: PKG_AES_KEY, else PKG_AES_KEY_IDU, whichever gives a sound table; a debug package
    // has no key.
    std::vector<Entry> entries;
    if (pkg.info.retail) {
        if (!keys.present)
            return make_error("{}: this build carries no package keys (scripts/self-keys.py had no RPCS3 source)", pkg_path);
        pkg.key = keys.ps3;
        if (!read_entries(pkg, entries)) {
            pkg.key = keys.idu;
            if (!read_entries(pkg, entries))
                return make_error("{}: its file table does not decrypt with either package key (a damaged package?)",
                    pkg_path);
            say("an in-store demo (IDU) package");
        }
    } else if (!read_entries(pkg, entries)) {
        return make_error("{}: its file table is not sound (a damaged debug package?)", pkg_path);
    }

    // The names and types, checked before anything is written (RPCS3 fill_data and extract_worker check as they go).
    Items items;
    items.reserve(entries.size());
    for (const Entry& e : entries) {
        auto name = entry_name(pkg, e);
        if (!name) return name.error();
        if (name->empty() && is_folder_type(e.type & 0xFF)) continue; // the install folder itself (RPCS3 reuses it)
        if (name->empty()) return make_error("{}: a file entry has no name", pkg_path);
        if (leaves_directory(*name))
            return make_error("{}: the entry \"{}\" would leave the install folder", pkg_path, *name);
        if (e.type & kPkgEntryPsp)
            return make_error("{}: {} is a PSP entry, which pspocket does not install", pkg_path, *name);
        if (!is_folder_type(e.type & 0xFF) && !is_file_type(e.type & 0xFF))
            return make_error("{}: the entry {} has a type RPCS3 does not install ({:#x})", pkg_path, *name, e.type);
        items.emplace_back(std::move(*name), e);
    }

    // An update must suit the game installed.
    if (const auto sfo = package_sfo(pkg, items)) {
        if (auto refused = check_app_version(pkg, *sfo, hdd0_dir)) return *refused;
    }

    auto dir = install_path(hdd0_dir, pkg.info);
    if (!dir) return make_error("{}: {}", pkg_path, dir.error().message);
    std::error_code ec;
    const bool fresh = !fs::exists(fs::symlink_status(*dir, ec)); // RPCS3 m_was_null: removed again if the install fails
    if (!fresh && !fs::is_directory(*dir, ec)) return make_error("{} is there, and is not a folder", dir->string());
    std::string why;
    if (!make_folders(*dir, why)) return make_error("{}", why);
    result.install_path = dir->string();
    say(std::format("{} ({} entries) into {}", pkg.info.content_id, items.size(), result.install_path));
    if (auto failed = install_entries(pkg, items, *dir, npd_keys, result, say)) {
        if (fresh && fs::remove_all(*dir, ec) != static_cast<std::uintmax_t>(-1) && !ec)
            return make_error("{} (the partial install was removed)", failed->message);
        return *failed;
    }
    return result;
}

} // namespace pspocket::recomp
