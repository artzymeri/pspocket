#pragma once

#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/result.hpp"

// PS3 library NIDs and the lv2 syscall table.
//
// A NID identifies an imported or exported function or variable: the first 4 bytes of
// SHA-1(name + 67 59 65 99 04 25 04 90 56 64 27 49 94 89 74 1A), read as a little-endian u32
// (RPCS3 rpcs3/Emu/Cell/PPUModule.cpp, ppu_generate_id).
//
// The built-in database comes from recomp/common/data/nids.txt, harvested from RPCS3's module registrations by
// recomp/common/scripts/harvest_nids.py and embedded at build time. Extra names files in the same format can be
// loaded at runtime (ps3inspect --names).
namespace pspocket::recomp {

u32 nid_of(std::string_view name);

enum class NidMatch : u8 {
    Exact,       // the name is registered for this NID in the requested module
    OtherModule, // not in this module, but another module registers the NID (name is a best guess)
    Unknown,     // no name known
};

std::string_view nid_match_name(NidMatch match);

struct NidResolution {
    std::string_view name; // empty when Unknown
    NidMatch match = NidMatch::Unknown;
};

class NidDb {
public:
    // The database parsed from the embedded nids.txt (parsed once, on first use).
    static const NidDb& builtin();

    // Names file format: `[module]` headers (`[]` is the nameless module-management library), then one entry
    // per line, either `name` (NID computed with nid_of) or `0xNNNNNNNN name`. `#` starts a comment.
    // Returns the number of entries added; on a malformed line nothing after it is added and the error names
    // source:line.
    Result<usize> load_text(std::string_view text, std::string_view source_name = "<text>");
    Result<usize> load_file(const std::filesystem::path& path);

    // Adds one entry; duplicates (same module, NID and name) are ignored.
    void add(std::string_view module, u32 nid, std::string_view name);

    // First name registered for (module, nid).
    std::optional<std::string_view> find(std::string_view module, u32 nid) const;
    // Every distinct name registered for nid in any module, in registration order.
    std::vector<std::string_view> find_any(u32 nid) const;
    // Exact match first, then a module-agnostic guess.
    NidResolution resolve(std::string_view module, u32 nid) const;

    bool has_module(std::string_view module) const;
    usize size() const { return entries_; }
    usize module_count() const { return modules_.size(); }

    // Returned string_views stay valid for the lifetime of the database (names live in a deque).
private:
    std::deque<std::string> names_;
    std::map<std::string, std::unordered_map<u32, std::vector<u32>>, std::less<>> modules_;
    std::unordered_map<u32, std::vector<u32>> any_;
    usize entries_ = 0;
};

// lv2 syscall number <-> name (recomp/common/data/syscalls.txt, from RPCS3 rpcs3/Emu/Cell/lv2/lv2.cpp).
class SyscallTable {
public:
    static const SyscallTable& builtin();

    // Format: `number name` per line (decimal), `#` comments.
    Result<usize> load_text(std::string_view text, std::string_view source_name = "<text>");

    std::optional<std::string_view> name(u32 number) const;
    std::optional<u32> number(std::string_view name) const;
    usize size() const { return by_number_.size(); }

private:
    std::map<u32, std::string> by_number_;
    std::map<std::string, u32, std::less<>> by_name_;
};

} // namespace pspocket::recomp
