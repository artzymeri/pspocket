#pragma once

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"

// Read-only access to disc images (plan section 9: "read straight from the ISO through an ISO9660 reader").
// PS3 game discs carry an ISO 9660 file system; files over 4 GB are split into multi-extent records. Sector 0 of a
// PS3 disc holds the region table that says which sector ranges a retail (Redump) image keeps encrypted.
namespace pspocket::disc {

inline constexpr u32 kSectorSize = 2048;

// Random-access byte source behind an image (a file, or memory in tests).
class Source {
public:
    virtual ~Source() = default;
    virtual u64 size() const = 0;
    // Reads exactly `size` bytes at `offset`; false on short read or error.
    virtual bool read(u64 offset, void* dst, usize size) const = 0;
};

std::unique_ptr<Source> open_file_source(const std::string& path, std::string* error);
std::unique_ptr<Source> memory_source(std::vector<u8> bytes);

struct Extent {
    u32 lba = 0;
    u32 size = 0;
};

struct Entry {
    std::string name; // without the ";1" version suffix
    bool directory = false;
    u64 size = 0;
    std::vector<Extent> extents; // a file over 4 GB has several, in order
};

// PS3 disc region table (sector 0). Regions alternate plain, encrypted, plain, ... A decrypted dump keeps the table,
// so whether data is actually encrypted is checked on content (see looks_like_self()).
struct Region {
    u32 first_sector = 0;
    u32 last_sector = 0;
    bool encrypted = false;
};

class Iso9660 {
public:
    // Parses the primary volume descriptor. Returns nullopt and sets *error for anything that isn't ISO 9660.
    static std::optional<Iso9660> open(std::shared_ptr<Source> source, std::string* error);

    const std::string& volume_id() const { return volume_id_; }
    u64 volume_bytes() const { return volume_bytes_; }
    u64 image_bytes() const { return source_->size(); }

    // Path lookup from the root, case-insensitive, '/'-separated ("PS3_GAME/USRDIR/EBOOT.BIN").
    std::optional<Entry> stat(std::string_view path) const;
    // Directory listing (excluding "." and ".."); nullopt if the path isn't a directory.
    std::optional<std::vector<Entry>> list(std::string_view path) const;

    // Reads part of a file; false on error or out-of-range.
    bool read(const Entry& file, u64 offset, void* dst, usize size) const;
    std::optional<std::vector<u8>> read_all(const Entry& file, u64 max_size) const;

    // Copies a file out to `out_path` (parent directories created). progress(bytes_done, bytes_total) may be null.
    bool extract(const Entry& file, const std::string& out_path, const std::function<void(u64, u64)>& progress,
        std::string* error) const;

    // The PS3 region table from sector 0, if it parses as one.
    std::vector<Region> ps3_regions() const;
    // True if the sector range [lba, lba + count) touches a region the table marks encrypted.
    bool in_encrypted_region(u32 lba, u32 count) const;

private:
    explicit Iso9660(std::shared_ptr<Source> source) : source_(std::move(source)) {}
    std::optional<std::vector<Entry>> read_directory(const Entry& dir) const;

    std::shared_ptr<Source> source_;
    std::string volume_id_;
    u64 volume_bytes_ = 0;
    Entry root_;
    bool joliet_ = false; // the tree read is the Joliet one: full names in UCS-2 (D-162)
};

// PARAM.SFO ("\0PSF", little-endian): the keys the app shell needs (TITLE_ID, TITLE, APP_VER, VERSION, CATEGORY,
// PS3_SYSTEM_VER, ...). Integers are rendered in decimal.
struct Sfo {
    std::vector<std::pair<std::string, std::string>> entries;
    std::string get(std::string_view key) const;
};
std::optional<Sfo> parse_sfo(std::span<const u8> bytes);

// First bytes of a decrypted-at-disc-level executable: "SCE\0" (SELF/SPRX) or "\x7fELF" (already decrypted ELF).
bool looks_like_self(std::span<const u8> head);
bool looks_like_elf(std::span<const u8> head);

} // namespace pspocket::disc
