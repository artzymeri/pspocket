// Firmware modules (LLE, D-157). Reference: RPCS3 rpcs3/Emu/Cell/PPUModule.cpp (ppu_load_prx: segments and the
// SCE_PPURELA relocations; ppu_load_exports; ppu_load_imports) and rpcs3/Emu/Cell/lv2/sys_prx.cpp.

#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/loader.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/npdrm.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/firmware.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/ppu_module.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "refs.hpp"

namespace pspocket::loader {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kChan = "lle";

constexpr u32 kLibraryEntrySize = 0x2C;     // sizeof(ppu_prx_module_info), when an entry's size byte is 0

using Linker = std::function<u32(std::string_view library, u32 nid)>;

bool read_file(const fs::path& p, std::vector<u8>& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string guest_string(u8* base, u32 addr, u32 max = 128)
{
    std::string s;
    for (u32 i = 0; i < max; i++) {
        const char c = static_cast<char>(ps_read8(base, addr + i));
        if (!c) break;
        s += c;
    }
    return s;
}

// The decrypted ELF of a firmware module file (.sprx: a SELF).
recomp::Result<std::vector<u8>> module_elf(const fs::path& file)
{
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return recomp::make_error("cannot read {}", file.string());
    return recomp::decrypt_self(bytes, recomp::builtin_self_keys());
}

// Whether pspocket's host modules have any function of `library`.
bool host_has(std::string_view library)
{
    for (const hle::Function& f : hle::registry().functions())
        if (f.module == library) return true;
    return false;
}

// The syscalls a module's code makes: an `sc` with `li r11, N` within the eight instructions before it (D-175).
std::set<u32> scan_syscalls(const recomp::ElfFile& elf, std::span<const u8> bytes)
{
    std::set<u32> out;
    for (const recomp::ProgramHeader& ph : elf.segments()) {
        if (ph.type != 1 || !(ph.flags & 1) || !ph.data_in_file) continue;
        const u8* code = bytes.data() + ph.offset;
        const u64 words = ph.filesz / 4;
        for (u64 i = 0; i < words; i++) {
            if (ps_ld32(code + 4 * i) != 0x44000002) continue; // sc
            for (u64 j = 1; j <= 8 && j <= i; j++) {
                const u32 w = ps_ld32(code + 4 * (i - j));
                if ((w & 0xFFFF0000u) == 0x39600000u) { // addi r11, 0, N
                    out.insert(w & 0xFFFF);
                    break;
                }
            }
        }
    }
    return out;
}

constexpr std::string_view kIndexHeader = "pspocket-lle-index 4";

// A module's key: its file name for sys/external, "internal/" and the name for sys/internal (codec cores and services
// other modules load, D-177; libvpost.sprx is in both, as different modules).
constexpr std::string_view kInternal = "internal/";

fs::path module_path(const fs::path& flash, std::string_view key)
{
    return key.starts_with(kInternal) ? flash / "sys" / "internal" / key.substr(kInternal.size())
                                      : flash / "sys" / "external" / key;
}

fs::path flash_path(const std::string& dir)
{
    fs::path p(dir);
    return p.has_filename() ? p : p.parent_path();
}

// Segments and relocations (ppu_load_prx), the module info and the exports (ppu_load_exports). Returns the imports'
// range.
bool place(const recomp::ElfFile& elf, std::span<const u8> bytes, hle::HleProcess& hle, PrxModule* out,
    std::pair<u32, u32>* imports, std::string* error, std::span<const u32> fixed = {})
{
    u8* base = hle.kernel().base();
    // Each PT_LOAD at its fixed address (a firmware module, D-178), else where main memory has room (a game's own).
    std::vector<u32> where(fixed.begin(), fixed.end());
    if (fixed.empty()) {
        for (const u32 memsz : recomp::prx_load_sizes(elf)) {
            const u32 addr = memsz ? hle.main_memory().alloc(memsz) : 0;
            if (memsz && !addr) {
                *error = std::format("no main memory for a {:#x}-byte segment", memsz);
                return false;
            }
            where.push_back(addr);
        }
    }
    auto placed = recomp::place_prx(elf, bytes, where);
    if (!placed) {
        *error = placed.error().message;
        return false;
    }
    GuestMemory& mem = hle.kernel().memory();
    for (const recomp::PlacedSegment& seg : placed->segments) {
        if (!seg.addr) continue;
        if (!fixed.empty() && !mem.commit(seg.addr, seg.memsz)) {
            *error = std::format("cannot map {:#x} bytes at {:#010x}", seg.memsz, seg.addr);
            return false;
        }
        std::memcpy(base + seg.addr, seg.bytes.data(), seg.memsz);
        out->segments.emplace_back(seg.addr, seg.memsz);
        u64 filesz = seg.memsz; // the PT_LOAD this segment came from (one each, in order)
        usize load_index = 0, placed_index = static_cast<usize>(&seg - placed->segments.data());
        for (const recomp::ProgramHeader& ph : elf.segments()) {
            if (ph.type != 1) continue;
            if (load_index++ == placed_index) {
                filesz = ph.filesz;
                break;
            }
        }
        out->segment_filesz.push_back(filesz);
        out->segment_executable.push_back(seg.executable);
    }
    for (const std::string& e : placed->errors) PS_LOG_ERROR(kChan, "{}", e);
    out->relocations = placed->relocations;
    out->relocation_errors = static_cast<u32>(placed->errors.size());
    const recomp::ProgramHeader* first = nullptr;
    for (const recomp::ProgramHeader& ph : elf.segments())
        if (ph.type == 1) {
            first = &ph;
            break;
        }
    // The module info (ppu_prx_library_info: attributes, version, name[28], toc, exports and imports ranges) sits in
    // the first segment at the first program header's p_paddr - p_offset.
    const u32 info = placed->segments[0].addr + static_cast<u32>(first->paddr - first->offset);
    out->name = guest_string(base, info + 4, 28);
    out->attributes = ps_read16(base, info);
    out->version = {ps_read8(base, info + 2), ps_read8(base, info + 3)};
    out->toc = ps_read32(base, info + 0x20);
    const u32 exports_start = ps_read32(base, info + 0x24), exports_end = ps_read32(base, info + 0x28);
    *imports = {ps_read32(base, info + 0x2C), ps_read32(base, info + 0x30)};
    out->exports = {exports_start, exports_end};
    out->imports = *imports;
    // Exports (ppu_prx_module_info: u8 size, u8, u16 version, u16 attributes, u16 num_func, u16 num_var, u16
    // num_tlsvar, u8, u8, u8[2], name, nids, addrs, ...). Attribute 0x0001: a library; 0x8000 alone: the nameless
    // management library (module_start, module_stop); neither: ignored.
    for (u32 at = exports_start; at < exports_end;) {
        const u8 size = ps_read8(base, at);
        const u16 attributes = ps_read16(base, at + 4);
        const u16 num_func = ps_read16(base, at + 6), num_var = ps_read16(base, at + 8);
        const u32 name = ps_read32(base, at + 0x10), nids = ps_read32(base, at + 0x14), addrs = ps_read32(base, at + 0x18);
        at += size ? size : kLibraryEntrySize;
        const bool library = attributes & 0x0001, management = !library && (attributes & 0x8000);
        if (management) {
            for (u32 i = 0; i < num_func; i++) {
                const u32 nid = ps_read32(base, nids + 4 * i), descriptor = ps_read32(base, addrs + 4 * i);
                if (nid == recomp::kNidModuleStart) out->start = descriptor;
                if (nid == recomp::kNidModuleStop) out->stop = descriptor;
            }
        }
        if (!library) continue;
        const std::string lib = guest_string(base, name);
        for (u32 i = 0; i < num_func; i++)
            out->functions[{lib, ps_read32(base, nids + 4 * i)}] = ps_read32(base, addrs + 4 * i);
        for (u32 i = num_func; i < u32{num_func} + num_var; i++)
            out->variables[{lib, ps_read32(base, nids + 4 * i)}] = ps_read32(base, addrs + 4 * i);
    }
    return true;
}

// Imports (ppu_load_imports: the same entries, nids and addrs the functions' NIDs and stub slots, vnids and vstubs the
// variables' NIDs and reference tables): each slot gets the linked descriptor; with attribute 0x2000 a function's own
// reference table follows its NIDs; each variable's references get its address.
void link_imports(std::pair<u32, u32> imports, hle::HleProcess& hle, PrxModule& module, const Linker& link,
    const Linker& link_variable)
{
    GuestMemory& mem = hle.kernel().memory();
    u8* base = mem.base();
    for (u32 at = imports.first; at < imports.second;) {
        const u8 size = ps_read8(base, at);
        const u16 attributes = ps_read16(base, at + 4);
        const u16 num_func = ps_read16(base, at + 6), num_var = ps_read16(base, at + 8);
        const std::string library = guest_string(base, ps_read32(base, at + 0x10));
        const u32 nids = ps_read32(base, at + 0x14), slots = ps_read32(base, at + 0x18);
        const u32 vnids = ps_read32(base, at + 0x1C), vstubs = ps_read32(base, at + 0x20);
        for (u32 i = 0; i < num_func; i++) {
            const u32 nid = ps_read32(base, nids + 4 * i);
            const u32 descriptor = link(library, nid);
            if (!descriptor) {
                PS_LOG_ERROR(kChan, "{}: its import {}::{:#010x} is not linked", module.name, library, nid);
                continue;
            }
            ps_write32(base, slots + 4 * i, descriptor);
            if (attributes & 0x2000)
                if (const u32 refs = ps_read32(base, nids + 4 * (num_func + i)))
                    patch_refs(mem, refs, descriptor, std::format("{}: {}::{:#010x}", module.name, library, nid));
        }
        for (u32 i = 0; i < num_var; i++) {
            const u32 nid = ps_read32(base, vnids + 4 * i), refs = ps_read32(base, vstubs + 4 * i);
            if (const u32 addr = link_variable ? link_variable(library, nid) : 0) {
                patch_refs(mem, refs, addr, std::format("{}: variable {}::{:#010x}", module.name, library, nid));
                module.variables_linked++;
            } else {
                PS_LOG_ERROR(kChan, "{}: its imported variable {}::{:#010x} is not linked", module.name, library, nid);
            }
        }
        at += size ? size : kLibraryEntrySize;
    }
}

} // namespace

LleModules::LleModules(std::string dev_flash_dir) : dev_flash_(std::move(dev_flash_dir)) {}
LleModules::~LleModules() = default;

bool LleModules::index(std::string* error)
{
    library_file_.clear();
    file_libraries_.clear();
    files_.clear();
    const fs::path flash = flash_path(dev_flash_);
    const fs::path external = flash / "sys" / "external";
    const fs::path cache = flash.parent_path() / "lle-index.txt";
    std::error_code ec;
    if (!fs::is_directory(external, ec)) {
        if (error) *error = "no firmware modules in " + external.string();
        return false;
    }
    // A library both folders export goes to sys/external's module, whatever order the files are read in.
    auto add = [&](const std::string& file, const std::string& library) {
        const auto [it, added] = library_file_.emplace(library, file);
        if (!added && it->second.starts_with(kInternal) && !file.starts_with(kInternal)) it->second = file;
        file_libraries_[file].push_back(library);
    };
    // Kept beside /dev_flash: the modules are decrypted once, not at every boot. Each module file: its libraries, what
    // they export, what it imports and the syscalls its code makes (D-175).
    std::error_code cache_ec;
    const fs::path internal = flash / "sys" / "internal";
    const auto newest = std::max(fs::last_write_time(external, ec),
        fs::is_directory(internal, cache_ec) ? fs::last_write_time(internal, cache_ec) : fs::file_time_type::min());
    if (const auto cached = fs::last_write_time(cache, cache_ec); !cache_ec && cached >= newest) {
        std::ifstream in(cache);
        std::string line;
        if (std::getline(in, line) && line == kIndexHeader) {
            std::string file;
            while (std::getline(in, line)) {
                std::istringstream words(line);
                std::string kind, name;
                words >> kind;
                if (kind == "F") {
                    words >> file;
                    files_[file];
                    continue;
                }
                if (file.empty()) continue;
                FileInfo& info = files_[file];
                if (kind == "S") {
                    for (u32 n; words >> n;) info.syscalls.insert(n);
                    continue;
                }
                if (kind == "Z") {
                    for (u32 n; words >> n;) info.load_sizes.push_back(n);
                    continue;
                }
                if (kind == "H") {
                    words >> info.sha256;
                    continue;
                }
                words >> name;
                if (kind == "L") {
                    add(file, name);
                    continue;
                }
                auto& table = kind == "EF" ? info.export_functions : kind == "EV" ? info.export_variables
                    : kind == "IF" ? info.import_functions : info.import_variables;
                auto& nids = table[name];
                for (std::string nid; words >> nid;) nids.insert(static_cast<u32>(std::stoul(nid, nullptr, 16)));
            }
            if (!library_file_.empty()) {
                lay_out();
                return true;
            }
        }
        files_.clear();
        library_file_.clear();
        file_libraries_.clear();
    }
    size_t modules = 0;
    std::vector<std::pair<std::string, fs::path>> files; // key, path: sys/external first, so its exports win
    for (const auto& [dir, prefix] : {std::pair{external, std::string()}, std::pair{internal, std::string(kInternal)}}) {
        std::vector<fs::path> found;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".sprx") found.push_back(e.path());
        std::sort(found.begin(), found.end());
        for (const fs::path& f : found) files.emplace_back(prefix + f.filename().string(), f);
    }
    for (const auto& [key, path] : files) {
        std::vector<u8> raw;
        if (!read_file(path, raw)) continue;
        auto elf_bytes = recomp::decrypt_self(raw, recomp::builtin_self_keys());
        if (!elf_bytes) continue;
        auto elf = recomp::ElfFile::parse(*elf_bytes);
        if (!elf) continue;
        auto module = recomp::analyze_ppu_module(*elf);
        if (!module) continue;
        modules++;
        const std::string& file = key;
        FileInfo& info = files_[file];
        for (const recomp::ExportedLibrary& lib : module->exports) {
            if (lib.name.empty() || lib.management || lib.skipped) continue;
            add(file, lib.name);
            for (const auto& f : lib.functions) info.export_functions[lib.name].insert(f.nid);
            for (const auto& v : lib.variables) info.export_variables[lib.name].insert(v.nid);
        }
        for (const recomp::ImportedLibrary& lib : module->imports) {
            for (const auto& f : lib.functions) info.import_functions[lib.name].insert(f.nid);
            for (const auto& v : lib.variables) info.import_variables[lib.name].insert(v.nid);
        }
        info.syscalls = scan_syscalls(*elf, *elf_bytes);
        info.load_sizes = recomp::prx_load_sizes(*elf);
        info.sha256 = recomp::to_hex(recomp::Sha256::hash(std::span<const u8>(raw)));
    }
    lay_out();
    if (std::ofstream out(cache); out) {
        out << kIndexHeader << '\n';
        const auto nids = [&](std::string_view kind, const std::map<std::string, std::set<u32>, std::less<>>& table) {
            for (const auto& [lib, set] : table) {
                out << kind << ' ' << lib;
                for (const u32 nid : set) out << std::format(" {:08x}", nid);
                out << '\n';
            }
        };
        for (const auto& [file, info] : files_) {
            out << "F " << file << '\n';
            for (const std::string& library : file_libraries_[file]) out << "L " << library << '\n';
            nids("EF", info.export_functions);
            nids("EV", info.export_variables);
            nids("IF", info.import_functions);
            nids("IV", info.import_variables);
            out << 'S';
            for (const u32 n : info.syscalls) out << ' ' << n;
            out << "\nZ";
            for (const u32 n : info.load_sizes) out << ' ' << n;
            out << "\nH " << info.sha256 << '\n';
        }
    }
    PS_LOG_INFO(kChan, "{} of {} firmware modules read: {} libraries", modules, files.size(), library_file_.size());
    if (library_file_.empty() && error) *error = "no firmware module could be read (a build without the keys?)";
    return !library_file_.empty();
}

void LleModules::lay_out()
{
    // Every module in key order, fixed in the PRX region (D-178): the same addresses for every game.
    if (files_.empty()) return;
    std::vector<std::vector<u32>> sizes;
    for (const auto& [key, info] : files_) sizes.push_back(info.load_sizes);
    const auto layout = recomp::prx_fixed_layout(sizes, PS_MEM_PRX_BASE, PS_MEM_PRX_SIZE);
    if (layout.empty()) {
        PS_LOG_ERROR(kChan, "the firmware's modules do not fit in the PRX region: they go to main memory, untranslated");
        for (auto& [key, info] : files_) info.addrs.clear();
        return;
    }
    usize i = 0;
    for (auto& [key, info] : files_) info.addrs = layout[i++];
}

std::string LleModules::identity_text() const
{
    std::string text;
    for (const auto& [key, info] : files_) text += recomp::firmware_identity_line(key, info.sha256, info.addrs);
    return text;
}

u32 LleModules::layout_end() const
{
    u64 end = PS_MEM_PRX_BASE;
    for (const auto& [key, info] : files_)
        for (usize i = 0; i < info.addrs.size() && i < info.load_sizes.size(); i++)
            if (info.addrs[i]) end = std::max<u64>(end, u64{info.addrs[i]} + info.load_sizes[i]);
    return static_cast<u32>(std::min<u64>(end, PS_MEM_PRX_BASE + u64{PS_MEM_PRX_SIZE}));
}

std::span<const u32> LleModules::fixed_addresses(std::string_view file) const
{
    const auto it = files_.find(file);
    return it == files_.end() ? std::span<const u32>{} : std::span<const u32>(it->second.addrs);
}

std::optional<std::string> LleModules::file_for(std::string_view library) const
{
    const auto it = library_file_.find(library);
    if (it == library_file_.end()) return std::nullopt;
    return it->second;
}

// A library forced to its module: by the caller (force), or one the host made without Sony's module to compare with
// (D-563, Registry::firmware_first), which goes to the firmware's module when one is installed.
bool LleModules::forced(std::string_view library) const
{
    if (std::find(forced_.begin(), forced_.end(), library) != forced_.end()) return true;
    if (!hle::registry().is_firmware_first(library)) return false;
    const auto file = file_for(library);
    return file && !always_native(*file);
}

bool LleModules::chooses(std::string_view library) const
{
    const auto file = file_for(library);
    if (!file) return false;
    if (forced(library)) return true;
    if (decided_) return chosen_.contains(*file);
    return runs_whole(*file);
}

bool LleModules::always_native(std::string_view file) { return recomp::firmware_module_native(file); }

std::vector<LleModules::LibraryNeeds> LleModules::needs_of(const recomp::PpuModule& module)
{
    std::vector<LibraryNeeds> needs;
    for (const recomp::ImportedLibrary& lib : module.imports) {
        LibraryNeeds n{lib.name, {}, {}};
        for (const auto& fn : lib.functions) n.functions.push_back(fn.nid);
        for (const auto& var : lib.variables) n.variables.push_back(var.nid);
        needs.push_back(std::move(n));
    }
    return needs;
}

void LleModules::decide(const std::vector<LibraryNeeds>& imports, const std::function<bool(u32)>& syscall_bound)
{
    decided_ = true;
    chosen_.clear();
    refused_.clear();
    gaps_.clear();
    host_linked_.clear();
    choose("the game", imports, {}, syscall_bound);
}

std::set<std::string, std::less<>> LleModules::choice_for(const std::vector<LibraryNeeds>& imports)
{
    quiet_ = true;
    decide(imports, [](u32) { return true; }); // syscalls only name gaps: they do not change the choice
    std::set<std::string, std::less<>> chosen = std::move(chosen_);
    quiet_ = false;
    decided_ = false;
    chosen_.clear();
    refused_.clear();
    gaps_.clear();
    host_linked_.clear();
    return chosen;
}

void LleModules::extend(std::string_view who, const std::vector<LibraryNeeds>& imports, std::string_view file,
    const std::function<bool(u32)>& syscall_bound)
{
    if (decided_) choose(who, imports, file, syscall_bound);
}

std::vector<LleModules::LibraryNeeds> LleModules::with_host_imports(const std::vector<LibraryNeeds>& imports) const
{
    std::vector<LibraryNeeds> all = imports;
    for (const LibraryNeeds& need : imports) {
        for (const hle::HostImport& i : hle::registry().imports()) {
            if (i.module != need.library || !file_for(i.library)) continue;
            auto it = std::find_if(all.begin(), all.end(), [&](const LibraryNeeds& n) { return n.library == i.library; });
            if (it == all.end()) it = all.insert(all.end(), LibraryNeeds{std::string(i.library), {}, {}});
            std::vector<u32>& nids = i.variable ? it->variables : it->functions;
            if (std::find(nids.begin(), nids.end(), i.nid) == nids.end()) nids.push_back(i.nid);
        }
    }
    return all;
}

void LleModules::link_host_imports(const std::vector<LibraryNeeds>& imports, hle::HleProcess& hle)
{
    for (const LibraryNeeds& need : imports) {
        if (chooses(need.library)) continue; // its firmware module imports for itself
        for (const hle::HostImport& i : hle::registry().imports()) {
            if (i.module != need.library || hle.imported(i.library, i.nid)) continue;
            if (!chooses(i.library)) {
                PS_LOG_DEBUG(kChan, "{} imports {}::{:#010x}: no firmware module gives it", i.module, i.library, i.nid);
                continue;
            }
            std::string why;
            const PrxModule* m = load(i.library, hle, &why);
            if (!m) {
                PS_LOG_ERROR(kChan, "{} needs {}: {}", i.module, i.library, why);
                continue;
            }
            const auto& exports = i.variable ? m->variables : m->functions;
            const auto it = exports.find({std::string(i.library), i.nid});
            if (it == exports.end()) {
                PS_LOG_ERROR(kChan, "{} imports {}::{:#010x}, which {} does not export", i.module, i.library, i.nid, m->file);
                continue;
            }
            hle.link_import(i.library, i.nid, it->second);
        }
    }
}

void LleModules::choose(std::string_view who, const std::vector<LibraryNeeds>& game_imports, std::string_view root_file,
    const std::function<bool(u32)>& syscall_bound)
{
    const std::vector<LibraryNeeds> imports = with_host_imports(game_imports);
    const auto on_host = [](std::string_view library, u32 nid) { return hle::registry().find(library, nid) != nullptr; };
    // A function the host made without Sony's module to compare it with (D-594) counts as one it lacks wherever the
    // library's module can serve instead: adding one never changes what a system with the firmware gets.
    const auto unverified = [](std::string_view library, u32 nid) { return hle::registry().is_unverified(library, nid); };
    // A file whose libraries something already linked to the host stays there (a library has one implementation).
    const auto host_bound = [&](std::string_view file) {
        const auto it = file_libraries_.find(file);
        return it != file_libraries_.end() && std::any_of(it->second.begin(), it->second.end(),
            [&](const std::string& l) { return host_linked_.contains(l); });
    };
    const auto can_join = [&](std::string_view file) {
        return !always_native(file) && !refused_.contains(file) && !host_bound(file);
    };
    // The roots: the files of the import libraries the host cannot serve (it lacks one of their imported functions, or
    // a variable is imported that the host does not export, D-503), and `root_file`. What was chosen before stays: it is linked.
    std::set<std::string, std::less<>> candidates = chosen_, roots;
    std::map<std::string, std::vector<u32>, std::less<>> lacking; // library -> imported functions the host lacks
    if (!root_file.empty() && files_.contains(root_file) && (chosen_.contains(root_file) || can_join(root_file)))
        roots.emplace(root_file);
    for (const LibraryNeeds& need : imports) {
        const auto file = file_for(need.library);
        if (file && forced(need.library)) {
            roots.insert(*file);
            continue;
        }
        std::vector<u32> missing;
        bool unverified_import = false;
        for (const u32 nid : need.functions) {
            if (!on_host(need.library, nid)) missing.push_back(nid);
            else if (unverified(need.library, nid)) unverified_import = true;
        }
        const bool variables_on_host = std::all_of(need.variables.begin(), need.variables.end(),
            [&](u32 nid) { return hle::registry().find_variable(need.library, nid) != nullptr; });
        // An unverified function goes to the module as a missing one would; where none can serve, the host's does.
        if (missing.empty() && variables_on_host && !(file && unverified_import)) continue;
        if (file && (chosen_.contains(*file) || can_join(*file))) roots.insert(*file);
        else if (!missing.empty()) lacking[need.library] = std::move(missing);
    }
    candidates.insert(roots.begin(), roots.end());
    // What each candidate needs, as RPCS3 links a module (PPUModule.cpp ppu_load_imports): a function from a candidate,
    // else the host, else a module that can join the candidates; one with none of these links to the host's
    // unimplemented stub (logged when called) and is listed here, as is a syscall lv2 lacks. A variable likewise: from
    // a candidate, else the host when it exports it (D-503, D-593: a codec core's operation table, say), else a module
    // that can join; one nothing exports stays unlinked, as lv2 leaves it on a PS3 (libadec's DTS and WMA Pro cores are
    // in no retail firmware, D-177), and is listed too. A forced library's module serves both rather than the host
    // (D-563). Only a module missing from the index is refused.
    std::map<std::string, std::vector<std::string>, std::less<>> gaps;
    for (bool changed = true; changed;) {
        changed = false;
        gaps.clear();
        for (auto it = candidates.begin(); it != candidates.end();) {
            const std::string file = *it;
            const auto info = files_.find(file);
            std::string why;
            std::vector<std::string> joins;
            std::vector<std::string>& gap = gaps[file];
            if (info == files_.end()) why = "not in the module index";
            for (const auto& [library, nids] : info == files_.end() ? decltype(info->second.import_functions){}
                                                                      : info->second.import_functions) {
                const auto dep = file_for(library);
                for (const u32 nid : nids) {
                    if (dep && candidates.contains(*dep)) {
                        const auto& exported = files_[*dep].export_functions;
                        const auto e = exported.find(library);
                        if (e == exported.end() || !e->second.contains(nid))
                            gap.push_back(std::format("{}::{:#010x} (not exported by {})", library, nid, *dep));
                        continue;
                    }
                    // A forced library's module joins rather than the host serving it (D-563), and so does the module
                    // of a function the host has unverified (D-594).
                    const bool to_module = dep && can_join(*dep) && (forced(library) || unverified(library, nid));
                    if (!to_module && on_host(library, nid)) continue;
                    if (dep && can_join(*dep)) {
                        joins.push_back(*dep);
                        continue;
                    }
                    gap.push_back(std::format("{}::{:#010x}", library, nid));
                }
            }
            for (const auto& [library, nids] : info == files_.end() ? decltype(info->second.import_variables){}
                                                                      : info->second.import_variables) {
                const auto dep = file_for(library);
                for (const u32 nid : nids) {
                    const bool to_module = dep && can_join(*dep) && forced(library);
                    if (dep && candidates.contains(*dep)) {
                        const auto& exported = files_[*dep].export_variables;
                        const auto e = exported.find(library);
                        if (e == exported.end() || !e->second.contains(nid))
                            gap.push_back(std::format("variable {}::{:#010x} (not exported by {})", library, nid, *dep));
                    } else if (!to_module && hle::registry().find_variable(library, nid)) {
                        // the host's (load_file links it)
                    } else if (dep && can_join(*dep)) {
                        joins.push_back(*dep);
                    } else {
                        gap.push_back(std::format("variable {}::{:#010x} (no module exports it)", library, nid));
                    }
                }
            }
            if (info != files_.end())
                for (const u32 n : info->second.syscalls)
                    if (!syscall_bound(n))
                        gap.push_back(std::format("syscall {} ({})", n, lv2::syscall_name(n) ? lv2::syscall_name(n) : "unnamed"));
            const bool keep = chosen_.contains(file) || std::any_of(file_libraries_[file].begin(),
                file_libraries_[file].end(), [&](const std::string& l) { return forced(l); });
            if (!why.empty() && !keep) {
                refused_[file] = why;
                gaps.erase(file);
                it = candidates.erase(it);
                changed = true;
                continue;
            }
            if (!why.empty() && !chosen_.contains(file) && !quiet_)
                PS_LOG_WARN(kChan, "{} is forced to run as it is although {}", file, why);
            for (const std::string& d : joins)
                if (candidates.insert(d).second) changed = true;
            ++it;
        }
    }
    // Only what the roots reach: a module that joined for one refused later goes again.
    std::set<std::string, std::less<>> reached;
    std::vector<std::string> todo(roots.begin(), roots.end());
    while (!todo.empty()) {
        const std::string f = std::move(todo.back());
        todo.pop_back();
        if (!candidates.contains(f) || !reached.insert(f).second) continue;
        const auto info = files_.find(f);
        if (info == files_.end()) continue;
        for (const auto* table : {&info->second.import_functions, &info->second.import_variables})
            for (const auto& entry : *table)
                if (const auto dep = file_for(entry.first); dep && candidates.contains(*dep)) todo.push_back(*dep);
    }
    std::vector<std::string> added;
    for (const std::string& f : reached)
        if (chosen_.insert(f).second) added.push_back(f);
    // The libraries the game, this module and the modules chosen now link to the host stay there.
    for (const LibraryNeeds& need : imports)
        if (const auto file = file_for(need.library); !file || !chosen_.contains(*file)) host_linked_.insert(need.library);
    for (const std::string& f : added)
        for (const auto* table : {&files_[f].import_functions, &files_[f].import_variables})
            for (const auto& entry : *table)
                if (const auto file = file_for(entry.first); !file || !chosen_.contains(*file))
                    host_linked_.insert(entry.first);
    // Libraries of refused modules stay on the host: what is imported from them that the host lacks is listed too.
    for (const LibraryNeeds& need : imports) {
        const auto file = file_for(need.library);
        if (!file || !refused_.contains(*file)) continue;
        std::vector<u32> missing;
        for (const u32 nid : need.functions)
            if (!on_host(need.library, nid)) missing.push_back(nid);
        if (!missing.empty()) lacking[need.library] = std::move(missing);
    }
    std::string added_list;
    for (const std::string& f : added) added_list += (added_list.empty() ? "" : ", ") + f;
    if (!quiet_)
        PS_LOG_INFO(kChan, "system libraries for {}: {} firmware module(s) {}{}{}", who, added.size(),
            decided_ && who != "the game" ? "join" : "run as they are", added_list.empty() ? "" : ": ", added_list);
    for (const std::string& f : added) {
        auto list = std::move(gaps[f]);
        if (list.empty()) continue;
        if (quiet_) {
            gaps_[f] = std::move(list);
            continue;
        }
        std::string shown;
        for (usize i = 0; i < list.size() && i < 6; i++) shown += (i ? ", " : "") + list[i];
        PS_LOG_WARN(kChan, "{} runs as it is; {} of what it needs has no implementation here ({}{}): calls to it fail", f,
            list.size(), shown, list.size() > 6 ? ", ..." : "");
        gaps_[f] = std::move(list);
    }
    if (quiet_) return;
    for (const auto& [file, why] : refused_)
        if (roots.contains(file) || candidates.contains(file) || who == "the game")
            PS_LOG_WARN(kChan, "{} stays on the host: {}", file, why);
    for (const auto& [library, nids] : lacking) {
        std::string list;
        for (usize i = 0; i < nids.size() && i < 8; i++) list += std::format("{}{:#010x}", i ? " " : "", nids[i]);
        const auto stubs = std::count_if(nids.begin(), nids.end(),
            [&](u32 nid) { return hle::find_rpcs3_stub(library, nid) != nullptr; });
        PS_LOG_WARN(kChan, "{}: {} imports {} function(s) the host lacks ({}{}): calls to them fail{}", library, who,
            nids.size(), list, nids.size() > 8 ? " ..." : "",
            stubs ? std::format(", except {} answered as RPCS3's stubs (D-182)", stubs) : std::string());
    }
}

bool LleModules::runs_whole(std::string_view file) const
{
    const auto it = file_libraries_.find(file);
    return it != file_libraries_.end() &&
        std::none_of(it->second.begin(), it->second.end(), [](const std::string& l) { return host_has(l); });
}

const PrxModule* LleModules::load(std::string_view library, hle::HleProcess& hle, std::string* error)
{
    const auto file = file_for(library);
    if (!file) {
        if (error) *error = std::format("no firmware module exports {}", library);
        return nullptr;
    }
    return load_file(*file, hle, error);
}

const PrxModule* LleModules::load_file(const std::string& file, hle::HleProcess& hle, std::string* error)
{
    for (const auto& m : loaded_)
        if (m->file == file) return m.get();
    auto elf_bytes = module_elf(module_path(flash_path(dev_flash_), file));
    if (!elf_bytes) {
        if (error) *error = std::format("{}: {}", file, elf_bytes.error().message);
        return nullptr;
    }
    auto elf = recomp::ElfFile::parse(*elf_bytes);
    if (!elf) {
        if (error) *error = std::format("{}: {}", file, elf.error().message);
        return nullptr;
    }
    auto module = std::make_unique<PrxModule>();
    module->file = file;
    std::pair<u32, u32> imports;
    std::string why;
    if (!place(*elf, *elf_bytes, hle, module.get(), &imports, &why, fixed_addresses(file))) {
        if (error) *error = std::format("{}: {}", file, why);
        return nullptr;
    }
    // Known before its imports are linked, so modules that import from each other find each other.
    PrxModule* m = module.get();
    loaded_.push_back(std::move(module));
    // From a firmware module the host does not replace (loaded now if need be), else the host's stub; a variable
    // likewise, else the host's variable (D-503).
    const auto from_module = [&](std::string_view library, u32 nid, bool variable) -> u32 {
        if (!chooses(library)) return 0;
        std::string dep_error;
        const PrxModule* dep = load(library, hle, &dep_error);
        if (!dep) {
            PS_LOG_ERROR(kChan, "{} needs {}: {}", m->name, library, dep_error);
            return 0;
        }
        const auto& exports = variable ? dep->variables : dep->functions;
        const auto it = exports.find({std::string(library), nid});
        if (it != exports.end()) return it->second;
        PS_LOG_ERROR(kChan, "{} imports {}::{:#010x}, which {} does not export", m->name, library, nid, dep->file);
        return 0;
    };
    link_imports(imports, hle, *m,
        [&](std::string_view library, u32 nid) -> u32 {
            if (const u32 d = from_module(library, nid, false)) return d;
            return hle.link_function(library, nid, {});
        },
        [&](std::string_view library, u32 nid) {
            if (const u32 v = from_module(library, nid, true)) return v;
            return chooses(library) ? 0u : hle.link_variable(library, nid); // the host's (D-503)
        });
    if (m->start) starts_.push_back(m->start);
    if (on_load_) on_load_(*m);
    PS_LOG_INFO(kChan, "{} ({}): {} segments at {:#x}, {} relocations ({} bad), {} functions exported, module_start {:#x}",
        m->name, file, m->segments.size(), m->segments.empty() ? 0 : m->segments[0].first, m->relocations,
        m->relocation_errors, m->functions.size(), m->start);
    return m;
}

// ---- modules the game loads while it runs (D-159) ----------------------------------------------------------------

namespace {

constexpr u32 kPrxErrorAlreadyStarted = 0x80011133,
              kPrxErrorNotStarted = 0x80011134, kPrxErrorUnsupportedType = 0x80011148,
              kPrxErrorIllegalLibrary = 0x8001113D, kPrxErrorNotRemovable = 0x80011138;
constexpr u32 kOk = 0, kNoEntry = 0x80010006, kNoSuchObject = 0x80010005; // CELL_ENOENT, CELL_ESRCH

} // namespace

RuntimeModules::RuntimeModules(hle::HleProcess& hle, std::shared_ptr<const Executable> exe, LleModules* lle)
    : hle_(hle), exe_(std::move(exe)), lle_(lle)
{
}

RuntimeModules::~RuntimeModules() = default;

RuntimeModules::Entry* RuntimeModules::find(u32 id)
{
    for (const auto& e : entries_)
        if (e->id == id && e->state != State::Unloaded) return e.get();
    return nullptr;
}

const PrxModule* RuntimeModules::module(u32 id)
{
    std::lock_guard lock(mutex_);
    const Entry* e = find(id);
    return e ? e->module : nullptr;
}

// A module's import: from a module the game loaded before, else a firmware module the host does not replace, else the
// host's stub.
u32 RuntimeModules::link_function(std::string_view library, u32 nid, std::string_view who)
{
    for (const auto& e : entries_) {
        if (!e->module || e->state == State::Unloaded) continue;
        if (const auto it = e->module->functions.find({std::string(library), nid}); it != e->module->functions.end())
            return it->second;
    }
    if (lle_ && lle_->chooses(library)) {
        std::string why;
        if (const PrxModule* m = lle_->load(library, hle_, &why)) {
            if (const auto it = m->functions.find({std::string(library), nid}); it != m->functions.end()) return it->second;
        } else {
            PS_LOG_ERROR(kChan, "{} needs {}: {}", who, library, why);
        }
    }
    return hle_.link_function(library, nid, {});
}

u32 RuntimeModules::link_variable(std::string_view library, u32 nid, std::string_view who)
{
    for (const auto& e : entries_) {
        if (!e->module || e->state == State::Unloaded) continue;
        if (const auto it = e->module->variables.find({std::string(library), nid}); it != e->module->variables.end())
            return it->second;
    }
    if (lle_ && lle_->chooses(library)) {
        std::string why;
        if (const PrxModule* m = lle_->load(library, hle_, &why)) {
            if (const auto it = m->variables.find({std::string(library), nid}); it != m->variables.end()) return it->second;
        } else {
            PS_LOG_ERROR(kChan, "{} needs {}: {}", who, library, why);
        }
    }
    return hle_.link_variable(library, nid); // the host's, or 0 (D-503)
}

u32 RuntimeModules::load(std::string_view path, u32* id) { return load_at(path, 0, id); }

u32 RuntimeModules::load_at(std::string_view path, u64 offset, u32* id)
{
    std::lock_guard lock(mutex_);
    auto e = std::make_unique<Entry>();
    e->path = std::string(path);
    constexpr std::string_view kFirmware = "/dev_flash/sys/external/", kFirmwareInternal = "/dev_flash/sys/internal/";
    const std::string file = (path.starts_with(kFirmwareInternal) ? std::string(kInternal) : std::string()) +
        fs::path(e->path).filename().string();
    const auto syscall_bound = [this](u32 n) { return hle_.kernel().syscall_bound(n); };
    if (offset && (path.starts_with(kFirmware) || path.starts_with(kFirmwareInternal))) {
        PS_LOG_ERROR(kChan, "{} at offset {:#x}: firmware modules load from their own files only", path, offset);
        return kPrxErrorUnsupportedType;
    }
    if (path.starts_with(kFirmware) || path.starts_with(kFirmwareInternal)) {
        // A firmware module by its path: as it is when chosen for the game (D-175), or when the host replaces none of it
        // (it joins, with the modules it needs); else empty (the host's functions serve its libraries).
        if (lle_ && lle_->decided() && !lle_->chosen(file) && lle_->runs_whole(file))
            lle_->extend(path, {}, file, syscall_bound);
        if (lle_ && (lle_->decided() ? lle_->chosen(file) : lle_->runs_whole(file))) {
            std::string why;
            e->module = lle_->load_file(file, hle_, &why);
            if (!e->module) {
                PS_LOG_ERROR(kChan, "{}: {}", path, why);
                return kPrxErrorIllegalLibrary;
            }
        } else {
            PS_LOG_INFO(kChan, "{}: the host serves its libraries (an empty module)", path);
        }
    } else {
        std::vector<u8> bytes;
        if (!hle_.kernel().vfs_read_file(path, bytes)) return kNoEntry;
        if (offset) { // a module inside an archive (sys_prx_load_module_by_fd)
            if (offset >= bytes.size()) return kPrxErrorUnsupportedType;
            bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        std::vector<u8> elf;
        if (bytes.size() >= 4 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F') {
            elf = std::move(bytes);
        } else {
            // A PSN game's NPDRM module opens with its licence (D-582): the newest licence key the game has shown, as
            // RPCS3's sys_prx takes last_key(), or the player's RAP.
            const recomp::NpdrmLicence licence = hle::module_licence(hle_.kernel());
            auto decrypted = recomp::decrypt_self(bytes, recomp::builtin_self_keys(), &licence);
            if (!decrypted) {
                PS_LOG_ERROR(kChan, "{}: cannot decrypt: {}", path, decrypted.error().message);
                return kPrxErrorUnsupportedType;
            }
            elf = std::move(*decrypted);
        }
        // The firmware modules its imports need, chosen as for the game's (D-175); its identity and, if it was
        // translated, its slot (D-238).
        std::vector<LleModules::LibraryNeeds> needs;
        std::string identity;
        std::vector<u32> slot, sizes;
        if (const auto parsed = recomp::ElfFile::parse(elf)) {
            identity = recomp::prx_identity(*parsed);
            sizes = recomp::prx_load_sizes(*parsed);
            if (translations_) slot = translations_->slot(identity);
            if (lle_ && lle_->decided())
                if (const auto info = recomp::analyze_ppu_module(*parsed)) {
                    needs = LleModules::needs_of(*info);
                    lle_->extend(path, needs, {}, syscall_bound);
                }
        }
        if (!slot.empty()) {
            if (const std::string refused = slot_refused(slot, sizes); !refused.empty()) {
                PS_LOG_WARN(kChan, "{}: not at its slot ({}): it runs under the interpreter, from main memory", path,
                    refused);
                slot.clear();
            }
        }
        e->owned = std::make_unique<PrxModule>();
        e->owned->file = file;
        e->owned->identity = identity;
        std::string why;
        const std::string who(path);
        bool matches = false;
        if (!load_prx(
                elf, hle_, [&](std::string_view library, u32 nid) { return link_function(library, nid, who); },
                e->owned.get(), &why,
                [&](std::string_view library, u32 nid) { return link_variable(library, nid, who); }, slot,
                [&](const PrxModule& placed) {
                    if (!slot.empty()) matches = translations_->matches(identity, placed);
                })) {
            PS_LOG_ERROR(kChan, "{}: {}", path, why);
            return kPrxErrorIllegalLibrary;
        }
        if (!slot.empty() && !(matches && !e->owned->code_patched) && translations_->enabled(identity)) {
            // Its translation runs at the slot since an earlier load (a range is never turned off), and this copy is
            // not what it was made from (linking now patches its code): this one goes to main memory instead.
            PS_LOG_WARN(kChan, "{}: {}, and its translation runs at its slot since an earlier load: this copy goes to "
                               "main memory, under the interpreter", path,
                matches ? "linking wrote into its code" : "its code is not the code translated");
            auto fresh = std::make_unique<PrxModule>();
            fresh->file = file;
            fresh->identity = identity;
            if (!load_prx(
                    elf, hle_, [&](std::string_view library, u32 nid) { return link_function(library, nid, who); },
                    fresh.get(), &why,
                    [&](std::string_view library, u32 nid) { return link_variable(library, nid, who); })) {
                PS_LOG_ERROR(kChan, "{}: {}", path, why);
                return kPrxErrorIllegalLibrary;
            }
            e->owned = std::move(fresh);
            slot.clear();
            matches = false;
        }
        e->module = e->owned.get();
        if (lle_) lle_->link_host_imports(needs, hle_);
        if (!slot.empty() && matches && !e->owned->code_patched) {
            translations_->enable(identity, *e->owned);
        } else if (!slot.empty()) {
            PS_LOG_WARN(kChan, "{}: {}: it runs under the interpreter", path,
                matches ? "linking wrote into its code" : "its code is not the code translated");
        } else if (!capture_dir_.empty() && !(translations_ && !translations_->slot(identity).empty())) {
            capture(identity, elf, path, offset); // not translated yet (one with a slot was, and is in its library)
        }
        PS_LOG_INFO(kChan, "{} ({}): {} segments at {:#x}, {} relocations ({} bad), {} functions exported", path,
            e->module->name, e->module->segments.size(), e->module->segments.empty() ? 0 : e->module->segments[0].first,
            e->module->relocations, e->module->relocation_errors, e->module->functions.size());
    }
    e->id = next_id_++;
    *id = e->id;
    entries_.push_back(std::move(e));
    return kOk;
}

u32 RuntimeModules::start(u32 id, u32* entry)
{
    std::lock_guard lock(mutex_);
    Entry* e = find(id);
    if (!e) return kNoSuchObject;
    if (e->state == State::Started) return kPrxErrorAlreadyStarted;
    *entry = 0;
    if (!e->module) return kOk;
    // Its exports into the executable's imports of them (RPCS3 lv2_prx::load_exports): the slots held the host's
    // stubs, which log the call and fail.
    u32 functions = 0, variables = 0;
    if (exe_) {
        GuestMemory& mem = hle_.kernel().memory();
        for (const auto& lib : exe_->module().imports) {
            for (const auto& fn : lib.functions) {
                const auto it = e->module->functions.find({lib.name, fn.nid});
                if (it == e->module->functions.end()) continue;
                ps_write32(mem.base(), fn.slot, it->second);
                if (fn.refs) patch_refs(mem, *fn.refs, it->second, std::format("{}::{:#010x}", lib.name, fn.nid));
                functions++;
            }
            for (const auto& var : lib.variables) {
                const auto it = e->module->variables.find({lib.name, var.nid});
                if (it == e->module->variables.end()) continue;
                patch_refs(mem, var.ref_table, it->second, std::format("{}::{:#010x}", lib.name, var.nid));
                variables++;
            }
        }
    }
    PS_LOG_INFO(kChan, "{} starts: {} of the game's imported functions and {} variables now go to it", e->path, functions,
        variables);
    // A firmware module started already (at boot, or before a module that needed it, D-175) is not started again.
    *entry = !e->owned && lle_ && lle_->started(e->module->start) ? 0 : e->module->start;
    if (!e->owned && lle_ && *entry) lle_->mark_started(*entry);
    return kOk;
}

std::vector<u32> RuntimeModules::take_dependency_starts()
{
    std::lock_guard lock(mutex_);
    return lle_ ? lle_->take_starts() : std::vector<u32>{};
}

void RuntimeModules::started(u32 id, s32 result)
{
    std::lock_guard lock(mutex_);
    Entry* e = find(id);
    if (!e) return;
    // SYS_PRX_RESIDENT (0) stays; otherwise the module goes (RPCS3 _sys_prx_start_module, cmd 2).
    e->state = result == 0 ? State::Started : State::Unloaded;
}

u32 RuntimeModules::stop(u32 id, u32* entry)
{
    std::lock_guard lock(mutex_);
    Entry* e = find(id);
    if (!e) return kNoSuchObject;
    if (e->state != State::Started) return kPrxErrorNotStarted;
    *entry = e->module ? e->module->stop : 0;
    return kOk;
}

void RuntimeModules::stopped(u32 id, s32 /*result*/)
{
    std::lock_guard lock(mutex_);
    if (Entry* e = find(id)) e->state = State::Stopped;
}

u32 RuntimeModules::unload(u32 id)
{
    std::lock_guard lock(mutex_);
    Entry* e = find(id);
    if (!e) return kNoSuchObject;
    if (e->state == State::Started) return kPrxErrorNotRemovable;
    // Its memory stays: code of it may still sit in a caller's frames or in function pointers the game kept.
    e->state = State::Unloaded;
    return kOk;
}

u32 RuntimeModules::id_by_address(u32 addr)
{
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_) {
        if (e->state == State::Unloaded || !e->module) continue;
        for (const auto& [base, size] : e->module->segments)
            if (size && addr >= base && addr - base < size) return e->id;
    }
    return 0;
}

std::vector<u32> RuntimeModules::list()
{
    std::lock_guard lock(mutex_);
    std::vector<u32> ids;
    for (const auto& e : entries_)
        if (e->state != State::Unloaded) ids.push_back(e->id);
    return ids;
}

bool RuntimeModules::info(u32 id, hle::ModuleInfo& out)
{
    std::lock_guard lock(mutex_);
    const Entry* e = find(id);
    if (!e || e->state == State::Unloaded) return false;
    out = {};
    out.path = e->path;
    if (const PrxModule* m = e->module) { // an empty module (the host serves it) has no more to say
        out.name = m->name;
        out.version = m->version;
        out.attributes = m->attributes;
        out.start = m->start;
        out.stop = m->stop;
        for (usize i = 0; i < m->segments.size(); i++) {
            const u64 filesz = i < m->segment_filesz.size() ? m->segment_filesz[i] : m->segments[i].second;
            out.segments.push_back({m->segments[i].first, filesz, m->segments[i].second});
        }
        out.exports = m->exports;
        out.imports = m->imports;
    }
    return true;
}

u32 RuntimeModules::id_by_name(std::string_view name)
{
    std::lock_guard lock(mutex_);
    for (const auto& e : entries_)
        if (e->state != State::Unloaded && e->module && e->module->name == name) return e->id;
    return 0;
}

bool load_prx(std::span<const u8> elf_bytes, hle::HleProcess& hle, const std::function<u32(std::string_view, u32)>& link,
    PrxModule* out, std::string* error, const std::function<u32(std::string_view, u32)>& link_variable,
    std::span<const u32> fixed, const std::function<void(const PrxModule&)>& placed)
{
    auto elf = recomp::ElfFile::parse(elf_bytes);
    if (!elf) {
        if (error) *error = elf.error().message;
        return false;
    }
    std::pair<u32, u32> imports;
    std::string why;
    if (!place(*elf, elf_bytes, hle, out, &imports, &why, fixed)) {
        if (error) *error = why;
        return false;
    }
    if (placed) placed(*out);
    // Reference tables patch instructions (lis/addi pairs): a translation of the code as placed would keep the
    // immediates it had (D-238).
    std::vector<u32> written;
    {
        const WatchRefs watch(&written);
        link_imports(imports, hle, *out, link, link_variable);
    }
    for (const u32 addr : written)
        for (usize i = 0; i < out->segments.size(); i++)
            if (out->segment_executable[i] && addr - out->segments[i].first < out->segments[i].second)
                out->code_patched = true;
    return true;
}

std::string RuntimeModules::slot_refused(const std::vector<u32>& slot, const std::vector<u32>& sizes)
{
    u32 low = ~0u;
    for (const u32 a : slot)
        if (a) low = std::min(low, a);
    if (lle_ && low < lle_->layout_end())
        return std::format("the firmware's modules now reach its slot ({:#010x} under {:#010x})", low, lle_->layout_end());
    for (const auto& other : entries_) {
        if (other->state == State::Unloaded || !other->module) continue;
        for (usize i = 0; i < slot.size() && i < sizes.size(); i++) {
            if (!slot[i] || !sizes[i]) continue;
            for (const auto& [base, size] : other->module->segments)
                if (size && slot[i] < base + size && base < slot[i] + sizes[i])
                    return std::format("{} is loaded there", other->path);
        }
    }
    return {};
}

void RuntimeModules::capture(const std::string& identity, std::span<const u8> elf, std::string_view path, u64 offset)
{
    const fs::path dir(capture_dir_);
    std::error_code ec;
    if (identity.empty() || fs::exists(dir / (identity + ".elf"), ec)) return; // saved before
    fs::create_directories(dir, ec);
    // The ELF without what followed it in an archive: to the furthest of its segments' and section table's ends.
    usize size = elf.size();
    if (auto parsed = recomp::ElfFile::parse(elf, {.symbols = false})) size = static_cast<usize>(parsed->extent());
    const fs::path tmp = dir / (identity + ".elf.tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(size));
        if (!out) return;
    }
    fs::rename(tmp, dir / (identity + ".elf"), ec);
    std::ofstream(dir / (identity + ".txt")) << std::format("path {}\noffset {:#x}\n", path, offset);
    PS_LOG_INFO(kChan, "{}: not translated yet: saved for the next preparation ({})", path, identity.substr(0, 16));
}

} // namespace pspocket::loader
