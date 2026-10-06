#pragma once

// sceNpTrophy's files: the disc's trophy pack (TROPHY.TRP), the trophy configuration (TROPCONF.SFM, XML) and the user's
// trophy progress (TROPUSR.DAT). Host-side formats only; sceNpTrophy.cpp finds the files and calls these.
// References: RPCS3 rpcs3/Loader/TRP.cpp/.h (TRPLoader), rpcs3/Loader/TROPUSR.cpp/.h (TROPUSRLoader,
// trophy_xml_document) and rpcs3/Utilities/rXml.cpp (the pugixml calls RPCS3 reads the configuration with).

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::trophy {

// ------------------------------------------------------------------------------------------------------------ XML

// A node of an XML document as pugixml keeps it with its default options (what RPCS3's rXmlDocument::Read uses):
// elements, and text (character data and CDATA sections, entities decoded, line ends normalized to \n) that isn't
// only whitespace. Comments, processing instructions, the XML declaration and DOCTYPE are not kept.
struct XmlNode {
    bool text_node = false;
    std::string name;  // an element's name
    std::string value; // a text node's text
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;

    // pugixml xml_node::attribute(name).value() (RPCS3 rXmlNode::GetAttribute): the first such attribute's value, or
    // "" when there is none.
    std::string attribute(std::string_view attr) const;
    // pugixml xml_node::text().get() (RPCS3 rXmlNode::GetNodeContent): the value of the first text child, or "".
    std::string content() const;
};

// The document node (its children are the top-level elements), or nullopt for a malformed document (pugixml's
// load_buffer failing: an unclosed or mismatched tag, a bad attribute, no element at all).
std::optional<XmlNode> parse_xml(std::string_view data);

// RPCS3 trophy_xml_document::GetRoot: the <trophyconf> element when it is the first top-level node; otherwise the
// document itself, with an error log.
const XmlNode& trophy_root(const XmlNode& document);

// C atoi, as RPCS3 reads the numeric attributes ("000", "-1").
s32 atoi(std::string_view s);

// The whole host file at `path`, or nullopt when it can't be opened or read.
std::optional<std::vector<u8>> read_host_file(const std::filesystem::path& path);

// ------------------------------------------------------------------------------------------------------ TROPHY.TRP

// The trophy pack (RPCS3 TRPHeader, TRPEntry): a 0x40-byte header (be32 magic 0xDCA24D00, version; be64 file size;
// be32 file count, element size, dev flag; SHA-1 of the file with this field zeroed, checked from version 2 on), then
// 0x40-byte entries (char name[32]; be64 offset, size; be32 unknown), then the files.
class TrpArchive {
public:
    struct Entry {
        std::string name;
        u64 offset = 0;
        u64 size = 0;
    };

    // RPCS3 TRPLoader::LoadHeader. Nullopt (with the reason in `error`) for a bad magic, a bad SHA-1, a file too short
    // for its header or its entry table.
    static std::optional<TrpArchive> parse(std::vector<u8> file, std::string* error = nullptr);

    u32 version() const { return version_; }
    const std::vector<Entry>& entries() const { return entries_; }
    // RPCS3 TRPLoader::GetRequiredSpace: the header's file size less the header and the entry table.
    u64 required_space() const;

    // RPCS3 ContainsEntry, RemoveEntry (every entry with that name), RenameEntry (every one), for names shorter than
    // 32 characters.
    bool contains(std::string_view name) const;
    void remove(std::string_view name);
    void rename(std::string_view from, std::string_view to);

    // RPCS3 TRPLoader::Install into the host directory `dest`: the entries are written into a new temporary directory
    // next to it, with the TROPUSR.DAT `dest` already has, and on success it replaces `dest`. False (and `error`) on
    // a write failure or an entry name that would leave the directory; nothing changes then. An entry that lies
    // outside the file is skipped, as in RPCS3.
    bool install(const std::filesystem::path& dest, std::string* error = nullptr) const;

private:
    std::vector<u8> file_;
    u32 version_ = 0;
    u64 file_size_ = 0;
    u32 files_count_ = 0;
    u32 element_size_ = 0;
    std::vector<Entry> entries_;
};

// ----------------------------------------------------------------------------------------------------- TROPUSR.DAT

// The user's progress in a trophy set (RPCS3 TROPUSRLoader): a 0x30-byte header (be32 magic 0x818F54AD, 0x00010000,
// table count, 0), 0x20-byte table headers (be32 type, entry size, 1, entry count; be64 offset, 0), then the tables.
// Table 4 holds one 0x60-byte entry per trophy (type 4, size 0x50, index, 0, trophy ID, grade, platinum link ID),
// table 6 one 0x70-byte entry (type 6, size 0x60, index, 0, trophy ID, state, 0, 0, be64 timestamp1, timestamp2).
// Entries are kept as the file has them (unknown fields included), and both tables are indexed by trophy ID, as RPCS3
// assumes.
class TropUsr {
public:
    static constexpr u32 kEntry4Size = 0x60, kEntry6Size = 0x70;
    static constexpr u32 kInvalidTrophyId = 0xFFFFFFFF; // SCE_NP_TROPHY_INVALID_TROPHY_ID
    enum Grade : u32 { Unknown = 0, Platinum = 1, Gold = 2, Silver = 3, Bronze = 4 };

    struct LoadResult {
        bool discarded_existing = false; // an existing file was invalid and was generated again
        bool success = false;
    };

    // RPCS3 TROPUSRLoader::Load: reads `usr`, or, when it is missing or invalid, generates it from the trophies of the
    // configuration `conf` (all locked) and saves it.
    LoadResult load(const std::filesystem::path& usr, const std::filesystem::path& conf, std::string* error = nullptr);
    // RPCS3 TROPUSRLoader::Save: the header and both tables, written to a temporary file that then replaces `usr`.
    bool save(const std::filesystem::path& usr) const;

    u32 trophy_count() const { return static_cast<u32>(table6_.size()); }
    u32 unlocked_count() const;
    u32 grade(u32 id) const;
    bool unlocked(u32 id) const;
    u64 timestamp(u32 id) const; // timestamp2 (RPCS3: which one the game wants is unknown)
    bool unlock(u32 id, u64 timestamp1, u64 timestamp2);

    // RPCS3 TROPUSRLoader::GetUnlockedPlatinumID, after `trophy_id` was unlocked: the platinum trophy that unlocking it
    // completes (its link ID's trophies all unlocked, the platinum one still locked), or kInvalidTrophyId. The link
    // IDs are read again from `conf` first (RPCS3 does it for files written before it stored them).
    u32 unlocked_platinum_id(u32 trophy_id, const std::filesystem::path& conf);

private:
    bool generate(const std::filesystem::path& usr, const std::filesystem::path& conf, std::string* error);
    bool read(const std::filesystem::path& usr);

    std::vector<std::array<u8, kEntry4Size>> table4_;
    std::vector<std::array<u8, kEntry6Size>> table6_;
};

} // namespace pspocket::hle::trophy
