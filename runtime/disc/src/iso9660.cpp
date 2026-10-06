#include "pspocket/disc/iso9660.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace pspocket::disc {
namespace {

// ---- sources ----

class FileSource final : public Source {
public:
    FileSource(int fd, u64 size) : fd_(fd), size_(size) {}
    ~FileSource() override { ::close(fd_); }
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    u64 size() const override { return size_; }

    bool read(u64 offset, void* dst, usize size) const override
    {
        if (offset > size_ || size > size_ - offset) return false;
        auto* out = static_cast<u8*>(dst);
        while (size > 0) {
            const ssize_t n = ::pread(fd_, out, size, static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            out += n;
            offset += static_cast<u64>(n);
            size -= static_cast<usize>(n);
        }
        return true;
    }

private:
    int fd_;
    u64 size_;
};

class MemorySource final : public Source {
public:
    explicit MemorySource(std::vector<u8> bytes) : bytes_(std::move(bytes)) {}
    u64 size() const override { return bytes_.size(); }
    bool read(u64 offset, void* dst, usize size) const override
    {
        if (offset > bytes_.size() || size > bytes_.size() - offset) return false;
        std::memcpy(dst, bytes_.data() + offset, size);
        return true;
    }

private:
    std::vector<u8> bytes_;
};

// ---- ISO 9660 helpers (ECMA-119) ----

u32 le32(const u8* p) { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; }
u32 be32(const u8* p) { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | u32{p[3]}; }
u16 le16(const u8* p) { return static_cast<u16>(p[0] | p[1] << 8); }

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); i++)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

std::string trim_right(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

constexpr u8 kFlagDirectory = 0x02;
constexpr u8 kFlagMultiExtent = 0x80; // "this is not the final record of the file"

struct Record {
    u32 lba = 0;
    u32 size = 0;
    u8 flags = 0;
    std::string name;
    bool self_or_parent = false;
};

// A Joliet name: UCS-2 big-endian, as UTF-8 (RPCS3 Loader/ISO.cpp iso_read_directory_entry).
std::string from_ucs2(const u8* p, usize bytes)
{
    std::string s;
    for (usize i = 0; i + 1 < bytes; i += 2) {
        const u32 c = u32{p[i]} << 8 | p[i + 1];
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else if (c < 0x800) {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            s += static_cast<char>(0xE0 | (c >> 12));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return s;
}

// One directory record. `avail` bytes are readable at rec. False for malformed records. `ucs2`: a Joliet tree's.
bool parse_record(const u8* rec, usize avail, Record& out, bool ucs2 = false)
{
    if (avail < 34) return false;
    const u8 length = rec[0];
    if (length < 34 || length > avail) return false;
    const u8 name_len = rec[32];
    if (name_len == 0 || 33u + name_len > length) return false;
    out.lba = le32(rec + 2);
    out.size = le32(rec + 10);
    out.flags = rec[25];
    out.self_or_parent = name_len == 1 && (rec[33] == 0 || rec[33] == 1);
    if (ucs2 && !out.self_or_parent) out.name = from_ucs2(rec + 33, name_len);
    else out.name.assign(reinterpret_cast<const char*>(rec + 33), name_len);
    if (const auto semi = out.name.find(';'); semi != std::string::npos) out.name.resize(semi);
    if (!out.name.empty() && out.name.back() == '.') out.name.pop_back(); // "NAME." for extensionless files
    return true;
}

std::vector<std::string_view> split_path(std::string_view path)
{
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (!part.empty() && part != ".") parts.push_back(part);
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    return parts;
}

} // namespace

std::unique_ptr<Source> open_file_source(const std::string& path, std::string* error)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = "open " + path + ": " + std::strerror(errno);
        return nullptr;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        if (error) *error = "stat " + path + ": " + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<FileSource>(fd, static_cast<u64>(st.st_size));
}

std::unique_ptr<Source> memory_source(std::vector<u8> bytes) { return std::make_unique<MemorySource>(std::move(bytes)); }

std::optional<Iso9660> Iso9660::open(std::shared_ptr<Source> source, std::string* error)
{
    auto fail = [&](std::string message) -> std::optional<Iso9660> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (!source) return fail("no source");

    // Volume descriptors start at sector 16, up to the terminator (255): the primary one (type 1) and, when there is
    // one, the Joliet supplementary one (type 2, escape sequence %/@, %/C or %/E), whose tree is read instead: its
    // names are whole, where the primary tree's are cut to 31 characters (Assassin's Creed opens
    // dataps3_flashback_paradise.forge, which the primary tree calls DATAPS3_FLASHBACK_PARADISE.FORG; D-162). RPCS3
    // Loader/ISO.cpp also reads the last of the two.
    u8 vd[kSectorSize];
    std::optional<Record> primary_root, joliet_root;
    std::string volume_id;
    u64 volume_bytes = 0;
    for (u32 sector = 16; sector < 16 + 64; sector++) {
        if (!source->read(u64{sector} * kSectorSize, vd, sizeof(vd))) return fail("image too small for ISO 9660");
        if (std::memcmp(vd + 1, "CD001", 5) != 0) return fail("no ISO 9660 volume descriptor at sector 16");
        if (vd[0] == 255) break;
        const bool joliet = vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E');
        if (vd[0] != 1 && !joliet) continue;
        if (le16(vd + 128) != kSectorSize) return fail("unsupported logical block size");
        Record root;
        if (!parse_record(vd + 156, 34, root) || !(root.flags & kFlagDirectory)) {
            if (vd[0] == 1) return fail("bad root directory record");
            continue; // a Joliet tree that cannot be read: the primary one serves
        }
        if (vd[0] == 1) {
            primary_root = root;
            volume_id = trim_right(std::string(reinterpret_cast<const char*>(vd + 40), 32));
            volume_bytes = u64{le32(vd + 80)} * kSectorSize;
        } else {
            joliet_root = root;
        }
    }
    if (!primary_root) return fail("no primary volume descriptor");
    const Record& root = joliet_root ? *joliet_root : *primary_root;
    Iso9660 iso(std::move(source));
    iso.volume_id_ = volume_id;
    iso.volume_bytes_ = volume_bytes;
    iso.joliet_ = joliet_root.has_value();
    iso.root_.name = "/";
    iso.root_.directory = true;
    iso.root_.size = root.size;
    iso.root_.extents.push_back({root.lba, root.size});
    return iso;
}

std::optional<std::vector<Entry>> Iso9660::read_directory(const Entry& dir) const
{
    if (!dir.directory || dir.extents.empty()) return std::nullopt;
    const Extent extent = dir.extents.front();
    if (extent.size == 0 || extent.size > (64u << 20)) return std::nullopt; // no real directory is this large
    std::vector<u8> data(extent.size);
    if (!source_->read(u64{extent.lba} * kSectorSize, data.data(), data.size())) return std::nullopt;

    std::vector<Entry> entries;
    bool previous_continues = false; // the last file record had the multi-extent flag
    usize pos = 0;
    while (pos < data.size()) {
        if (data[pos] == 0) { // records never cross a sector boundary: the rest of this sector is padding
            pos = (pos / kSectorSize + 1) * kSectorSize;
            continue;
        }
        const usize sector_end = std::min<usize>(data.size(), (pos / kSectorSize + 1) * kSectorSize);
        Record rec;
        if (!parse_record(data.data() + pos, sector_end - pos, rec, joliet_)) return std::nullopt;
        pos += data[pos];
        if (rec.self_or_parent) continue;

        const bool is_dir = (rec.flags & kFlagDirectory) != 0;
        if (previous_continues && !is_dir && !entries.empty() && entries.back().name == rec.name) {
            Entry& file = entries.back(); // next extent of a file over 4 GB
            file.size += rec.size;
            file.extents.push_back({rec.lba, rec.size});
        } else {
            Entry entry;
            entry.name = rec.name;
            entry.directory = is_dir;
            entry.size = rec.size;
            entry.extents.push_back({rec.lba, rec.size});
            entries.push_back(std::move(entry));
        }
        previous_continues = !is_dir && (rec.flags & kFlagMultiExtent) != 0;
    }
    return entries;
}

std::optional<Entry> Iso9660::stat(std::string_view path) const
{
    Entry current = root_;
    for (const std::string_view part : split_path(path)) {
        const auto entries = read_directory(current);
        if (!entries) return std::nullopt;
        bool found = false;
        for (const Entry& e : *entries) {
            if (iequals(e.name, part)) {
                current = e;
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    return current;
}

std::optional<std::vector<Entry>> Iso9660::list(std::string_view path) const
{
    const auto dir = stat(path);
    if (!dir || !dir->directory) return std::nullopt;
    return read_directory(*dir);
}

bool Iso9660::read(const Entry& file, u64 offset, void* dst, usize size) const
{
    if (file.directory || offset > file.size || size > file.size - offset) return false;
    auto* out = static_cast<u8*>(dst);
    u64 extent_start = 0; // file offset where the current extent begins
    for (const Extent& extent : file.extents) {
        const u64 extent_end = extent_start + extent.size;
        if (size > 0 && offset < extent_end) {
            const u64 within = offset - extent_start;
            const usize n = static_cast<usize>(std::min<u64>(size, extent.size - within));
            if (!source_->read(u64{extent.lba} * kSectorSize + within, out, n)) return false;
            out += n;
            offset += n;
            size -= n;
        }
        extent_start = extent_end;
    }
    return size == 0;
}

std::optional<std::vector<u8>> Iso9660::read_all(const Entry& file, u64 max_size) const
{
    if (file.directory || file.size > max_size) return std::nullopt;
    std::vector<u8> bytes(static_cast<usize>(file.size));
    if (!read(file, 0, bytes.data(), bytes.size())) return std::nullopt;
    return bytes;
}

bool Iso9660::extract(const Entry& file, const std::string& out_path, const std::function<void(u64, u64)>& progress,
    std::string* error) const
{
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (file.directory) return fail(file.name + " is a directory");

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(out_path).parent_path(), ec);
    const std::string tmp = out_path + ".part";
    std::FILE* out = std::fopen(tmp.c_str(), "wb");
    if (!out) return fail("create " + tmp + ": " + std::strerror(errno));

    std::vector<u8> buffer(4u << 20);
    u64 done = 0;
    while (done < file.size) {
        const usize n = static_cast<usize>(std::min<u64>(buffer.size(), file.size - done));
        if (!read(file, done, buffer.data(), n)) {
            std::fclose(out);
            std::remove(tmp.c_str());
            return fail("read error in " + file.name + " at offset " + std::to_string(done));
        }
        if (std::fwrite(buffer.data(), 1, n, out) != n) {
            std::fclose(out);
            std::remove(tmp.c_str());
            return fail("write error for " + out_path);
        }
        done += n;
        if (progress) progress(done, file.size);
    }
    if (std::fclose(out) != 0) return fail("close " + tmp);
    std::filesystem::rename(tmp, out_path, ec);
    if (ec) return fail("rename to " + out_path + ": " + ec.message());
    return true;
}

std::vector<Region> Iso9660::ps3_regions() const
{
    // Sector 0: u32 BE count of plain regions, u32 BE unused, then a (first, last) BE pair per plain region. The gaps
    // between plain regions are the encrypted ones. (Layout as used by IRD files and disc dumpers.)
    u8 s0[kSectorSize];
    if (!source_->read(0, s0, sizeof(s0))) return {};
    const u32 plain_count = be32(s0);
    if (plain_count == 0 || plain_count > 64) return {};
    std::vector<Region> regions;
    u32 next_expected = 0;
    for (u32 i = 0; i < plain_count; i++) {
        const u32 first = be32(s0 + 8 + i * 8);
        const u32 last = be32(s0 + 12 + i * 8);
        if (first < next_expected || last < first) return {}; // not a region table
        if (first > next_expected) regions.push_back({next_expected, first - 1, true});
        regions.push_back({first, last, false});
        next_expected = last + 1;
    }
    if (regions.empty() || regions.front().first_sector != 0) return {};
    return regions;
}

bool Iso9660::in_encrypted_region(u32 lba, u32 count) const
{
    const u64 last = u64{lba} + (count ? count - 1 : 0);
    for (const Region& r : ps3_regions())
        if (r.encrypted && u64{r.first_sector} <= last && lba <= r.last_sector) return true;
    return false;
}

std::string Sfo::get(std::string_view key) const
{
    for (const auto& [k, v] : entries)
        if (k == key) return v;
    return {};
}

std::optional<Sfo> parse_sfo(std::span<const u8> b)
{
    // PSF: magic "\0PSF", version, key table offset, data table offset, entry count; 16-byte index entries.
    if (b.size() < 20 || std::memcmp(b.data(), "\0PSF", 4) != 0) return std::nullopt;
    const u32 key_table = le32(b.data() + 8);
    const u32 data_table = le32(b.data() + 12);
    const u32 count = le32(b.data() + 16);
    if (count > 1024 || 20 + u64{count} * 16 > b.size() || key_table > b.size() || data_table > b.size())
        return std::nullopt;

    Sfo sfo;
    for (u32 i = 0; i < count; i++) {
        const u8* e = b.data() + 20 + i * 16;
        const u32 key_off = key_table + le16(e);
        const u16 fmt = le16(e + 2);
        const u32 len = le32(e + 4);
        const u32 data_off = data_table + le32(e + 12);
        if (key_off >= b.size() || data_off > b.size() || len > b.size() - data_off) return std::nullopt;

        std::string key;
        for (u32 p = key_off; p < b.size() && b[p] != 0; p++) key.push_back(static_cast<char>(b[p]));
        std::string value;
        if (fmt == 0x0404 && len >= 4) {
            value = std::to_string(le32(b.data() + data_off));
        } else {
            value.assign(reinterpret_cast<const char*>(b.data() + data_off), len);
            while (!value.empty() && value.back() == '\0') value.pop_back();
        }
        sfo.entries.emplace_back(std::move(key), std::move(value));
    }
    return sfo;
}

bool looks_like_self(std::span<const u8> head) { return head.size() >= 4 && std::memcmp(head.data(), "SCE\0", 4) == 0; }

bool looks_like_elf(std::span<const u8> head)
{
    return head.size() >= 4 && std::memcmp(head.data(), "\x7f" "ELF", 4) == 0;
}

} // namespace pspocket::disc
