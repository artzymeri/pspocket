// Firmware modules (LLE, D-157). Reference: RPCS3 rpcs3/Emu/Cell/PPUModule.cpp (ppu_load_prx: segments and the
// SCE_PPURELA relocations; ppu_load_exports; ppu_load_imports) and rpcs3/Emu/Cell/lv2/sys_prx.cpp.

#include "pspocket/loader/lle.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/ppu_module.hpp"
#include "pspocket/recomp/self_decrypt.hpp"
#include "refs.hpp"

namespace pspocket::loader {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kChan = "lle";

constexpr u32 kSceRelocations = 0x700000A4; // PT_SCE_PPURELA
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

fs::path flash_path(const std::string& dir)
{
    fs::path p(dir);
    return p.has_filename() ? p : p.parent_path();
}

// Segments and relocations (ppu_load_prx), the module info and the exports (ppu_load_exports). Returns the imports'
// range.
bool place(const recomp::ElfFile& elf, std::span<const u8> bytes, hle::HleProcess& hle, PrxModule* out,
    std::pair<u32, u32>* imports, std::string* error)
{
    u8* base = hle.kernel().base();
    // Per PT_LOAD header, in order; an empty one keeps its index with no memory.
    std::vector<std::pair<u32, u32>> segs;
    const recomp::ProgramHeader* first = nullptr;
    for (const recomp::ProgramHeader& ph : elf.segments()) {
        if (ph.type != 1) continue;
        if (!first) first = &ph;
        if (!ph.memsz) {
            segs.emplace_back(0, 0);
            continue;
        }
        if (ph.filesz > ph.memsz || (ph.filesz && !ph.data_in_file)) {
            *error = std::format("segment {} lies outside the file", segs.size());
            return false;
        }
        const u32 addr = hle.main_memory().alloc(static_cast<u32>(ph.memsz));
        if (!addr) {
            *error = std::format("no main memory for a {:#x}-byte segment", ph.memsz);
            return false;
        }
        std::memcpy(base + addr, bytes.data() + ph.offset, static_cast<size_t>(ph.filesz));
        segs.emplace_back(addr, static_cast<u32>(ph.memsz));
        out->segments.emplace_back(addr, static_cast<u32>(ph.memsz));
    }
    if (!first || segs.empty() || !segs[0].first) {
        *error = "no first segment";
        return false;
    }
    // Relocations: {u64 offset, u16, u8 index of the value's segment (0xFF: none), u8 index of the address's segment,
    // u32 type, u64 ptr} (ppu_prx_relocation_info). Bit fields count from the most significant bit (ppu_bf_t).
    for (const recomp::ProgramHeader& ph : elf.segments()) {
        if (ph.type != kSceRelocations || !ph.data_in_file) continue;
        for (u64 i = 0; i + 24 <= ph.filesz; i += 24) {
            const u8* r = bytes.data() + ph.offset + i;
            const u64 offset = ps_ld64(r);
            const u8 index_value = r[10], index_addr = r[11];
            const u32 type = ps_ld32(r + 12);
            const u64 ptr = ps_ld64(r + 16);
            if (index_addr >= segs.size() || offset >= ((u64{segs[index_addr].second} + 0xFF) & ~u64{0xFF}) ||
                (index_value != 0xFF && (index_value >= segs.size() || !segs[index_value].first))) {
                PS_LOG_ERROR(kChan, "relocation {} lies outside its segments", i / 24);
                out->relocation_errors++;
                continue;
            }
            const u32 at = segs[index_addr].first + static_cast<u32>(offset);
            const u64 data = (index_value == 0xFF ? 0 : segs[index_value].first) + ptr;
            switch (type) {
            case 1: ps_write32(base, at, static_cast<u32>(data)); break;       // ADDR32
            case 4: ps_write16(base, at, static_cast<u16>(data)); break;       // ADDR16_LO
            case 5: ps_write16(base, at, static_cast<u16>(data >> 16)); break; // ADDR16_HI
            case 6: ps_write16(base, at, static_cast<u16>((data >> 16) + ((data & 0x8000) ? 1 : 0))); break; // _HA
            case 10: // REL24: bits 6..29 of the word
                ps_write32(base, at, (ps_read32(base, at) & ~0x03FFFFFCu) | (static_cast<u32>(data - at) & 0x03FFFFFCu));
                break;
            case 11: // REL14: bits 16..29
                ps_write32(base, at, (ps_read32(base, at) & ~0x0000FFFCu) | (static_cast<u32>(data - at) & 0x0000FFFCu));
                break;
            case 38: ps_write64(base, at, data); break;      // ADDR64
            case 44: ps_write64(base, at, data - at); break; // REL64
            case 57: // ADDR16_LO_DS: the halfword's top 14 bits
                ps_write16(base, at, static_cast<u16>((ps_read16(base, at) & 3u) | (static_cast<u16>(data) & 0xFFFCu)));
                break;
            default:
                PS_LOG_ERROR(kChan, "relocation type {} at {:#x} is unknown", type, at);
                out->relocation_errors++;
                continue;
            }
            out->relocations++;
        }
    }
    // The module info (ppu_prx_library_info: attributes, version, name[28], toc, exports and imports ranges) sits in
    // the first segment at the first program header's p_paddr - p_offset.
    const u32 info = segs[0].first + static_cast<u32>(first->paddr - first->offset);
    out->name = guest_string(base, info + 4, 28);
    out->toc = ps_read32(base, info + 0x20);
    const u32 exports_start = ps_read32(base, info + 0x24), exports_end = ps_read32(base, info + 0x28);
    *imports = {ps_read32(base, info + 0x2C), ps_read32(base, info + 0x30)};
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
    const fs::path flash = flash_path(dev_flash_);
    const fs::path external = flash / "sys" / "external";
    const fs::path cache = flash.parent_path() / "lle-index.txt";
    std::error_code ec;
    if (!fs::is_directory(external, ec)) {
        if (error) *error = "no firmware modules in " + external.string();
        return false;
    }
    auto add = [&](const std::string& file, const std::string& library) {
        library_file_.emplace(library, file);
        file_libraries_[file].push_back(library);
    };
    // Kept beside /dev_flash: the modules are decrypted once, not at every boot.
    std::error_code cache_ec;
    if (const auto cached = fs::last_write_time(cache, cache_ec); !cache_ec && cached >= fs::last_write_time(external, ec)) {
        std::ifstream in(cache);
        for (std::string file, library; in >> file >> library;) add(file, library);
        if (!library_file_.empty()) return true;
    }
    size_t modules = 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(external, ec))
        if (e.path().extension() == ".sprx") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& path : files) {
        auto elf_bytes = module_elf(path);
        if (!elf_bytes) continue;
        auto elf = recomp::ElfFile::parse(*elf_bytes);
        if (!elf) continue;
        auto module = recomp::analyze_ppu_module(*elf);
        if (!module) continue;
        modules++;
        for (const recomp::ExportedLibrary& lib : module->exports)
            if (!lib.name.empty() && !lib.management && !lib.skipped) add(path.filename().string(), lib.name);
    }
    if (std::ofstream out(cache); out)
        for (const auto& [file, libraries] : file_libraries_)
            for (const std::string& library : libraries) out << file << ' ' << library << '\n';
    PS_LOG_INFO(kChan, "{} of {} firmware modules read: {} libraries", modules, files.size(), library_file_.size());
    if (library_file_.empty() && error) *error = "no firmware module could be read (a build without the keys?)";
    return !library_file_.empty();
}

std::optional<std::string> LleModules::file_for(std::string_view library) const
{
    const auto it = library_file_.find(library);
    if (it == library_file_.end()) return std::nullopt;
    return it->second;
}

bool LleModules::chooses(std::string_view library) const
{
    const auto file = file_for(library);
    if (!file) return false;
    if (std::find(forced_.begin(), forced_.end(), library) != forced_.end()) return true;
    const auto it = file_libraries_.find(*file);
    return std::none_of(it->second.begin(), it->second.end(), [](const std::string& l) { return host_has(l); });
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
    auto elf_bytes = module_elf(flash_path(dev_flash_) / "sys" / "external" / file);
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
    if (!place(*elf, *elf_bytes, hle, module.get(), &imports, &why)) {
        if (error) *error = std::format("{}: {}", file, why);
        return nullptr;
    }
    // Known before its imports are linked, so modules that import from each other find each other.
    PrxModule* m = module.get();
    loaded_.push_back(std::move(module));
    // From a firmware module the host does not replace (loaded now if need be), else the host's stub; variables only
    // from firmware modules (the host exports none).
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
        [&](std::string_view library, u32 nid) { return from_module(library, nid, true); });
    if (m->start) starts_.push_back(m->start);
    PS_LOG_INFO(kChan, "{} ({}): {} segments at {:#x}, {} relocations ({} bad), {} functions exported, module_start {:#x}",
        m->name, file, m->segments.size(), m->segments.empty() ? 0 : m->segments[0].first, m->relocations,
        m->relocation_errors, m->functions.size(), m->start);
    return m;
}

bool load_prx(std::span<const u8> elf_bytes, hle::HleProcess& hle, const std::function<u32(std::string_view, u32)>& link,
    PrxModule* out, std::string* error, const std::function<u32(std::string_view, u32)>& link_variable)
{
    auto elf = recomp::ElfFile::parse(elf_bytes);
    if (!elf) {
        if (error) *error = elf.error().message;
        return false;
    }
    std::pair<u32, u32> imports;
    std::string why;
    if (!place(*elf, elf_bytes, hle, out, &imports, &why)) {
        if (error) *error = why;
        return false;
    }
    link_imports(imports, hle, *out, link, link_variable);
    return true;
}

} // namespace pspocket::loader
