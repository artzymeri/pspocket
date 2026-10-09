// A game's own executable files on its disc (D-239). See pspocket/disc/modules.hpp.

#include "pspocket/disc/modules.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/disc/iso9660.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

namespace pspocket::disc {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kChan = "disc";
constexpr u64 kMaxFile = 256ull << 20;
constexpr u16 kPrxType = 0xFFA4; // ET_SCE_PPURELEXEC
constexpr u16 kSpuMachine = 23;

bool executable_name(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const char* ext : {".sprx", ".prx", ".self", ".elf", ".bin"})
        if (name.ends_with(ext)) return true;
    return false;
}

bool write_once(const fs::path& p, std::span<const u8> bytes)
{
    std::error_code ec;
    if (fs::exists(p, ec)) return true;
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

struct Walk {
    const Iso9660& iso;
    fs::path prx_dir, spu_dir;
    DiscModules out;
};

void look_at(Walk& w, const std::string& path, const Entry& e)
{
    w.out.files++;
    u8 head[16] = {};
    if (!w.iso.read(e, 0, head, std::min<u64>(sizeof head, e.size))) return;
    if (!looks_like_self(head) && !looks_like_elf(head)) return; // data with an executable's name
    const auto raw = w.iso.read_all(e, kMaxFile);
    if (!raw) return;
    std::vector<u8> elf_bytes;
    if (looks_like_elf(head)) {
        elf_bytes = *raw;
    } else {
        // A free NPDRM module opens with NP_KLIC_FREE (D-582); one that needs a licence (a RAP) or the game's own
        // klicensee is counted: it is saved when the game loads it.
        const recomp::NpdrmLicence free_only;
        auto decrypted = recomp::decrypt_self(*raw, recomp::builtin_self_keys(), &free_only);
        if (!decrypted) {
            w.out.npdrm++;
            PS_LOG_INFO(kChan, "{}: not decrypted ({})", path, decrypted.error().message);
            return;
        }
        elf_bytes = std::move(*decrypted);
    }
    const auto elf = recomp::ElfFile::parse(elf_bytes, {.symbols = false});
    if (!elf) {
        w.out.unreadable++;
        return;
    }
    std::error_code ec;
    if (elf->is_ppu() && elf->header().type == kPrxType) {
        const std::string identity = recomp::prx_identity(*elf);
        fs::create_directories(w.prx_dir, ec);
        const usize size = static_cast<usize>(std::min<u64>(elf->extent(), elf_bytes.size()));
        if (write_once(w.prx_dir / (identity + ".elf"), std::span<const u8>(elf_bytes.data(), size))) {
            std::ofstream(w.prx_dir / (identity + ".txt")) << std::format("path /dev_bdvd/{}\noffset 0x0\n", path);
            w.out.prx++;
        }
    } else if (elf->header().machine == kSpuMachine) {
        fs::create_directories(w.spu_dir, ec);
        const std::string name = recomp::to_hex(recomp::Sha256::hash(std::span<const u8>(elf_bytes))).substr(0, 16);
        if (write_once(w.spu_dir / (name + ".elf"), elf_bytes)) {
            std::ofstream(w.spu_dir / (name + ".txt")) << std::format("path /dev_bdvd/{}\n", path);
            w.out.spu++;
        }
    } else {
        w.out.unreadable++; // another executable (a launcher, a PPU program of its own): not a module
    }
}

void walk(Walk& w, const std::string& dir, int depth)
{
    if (depth > 12) return;
    const auto entries = w.iso.list(dir);
    if (!entries) return;
    for (const Entry& e : *entries) {
        const std::string path = dir + "/" + e.name;
        if (e.directory) walk(w, path, depth + 1);
        else if (executable_name(e.name) && e.size <= kMaxFile && path != "PS3_GAME/USRDIR/EBOOT.BIN") look_at(w, path, e);
    }
}

} // namespace

bool save_disc_modules(const std::string& image_path, const std::string& prx_dir, const std::string& spu_dir,
    DiscModules* out, std::string* error)
{
    std::shared_ptr<Source> source = open_file_source(image_path, error);
    if (!source) return false;
    auto iso = Iso9660::open(std::move(source), error);
    if (!iso) return false;
    Walk w{*iso, prx_dir, spu_dir, {}};
    walk(w, "PS3_GAME/USRDIR", 0);
    PS_LOG_INFO(kChan, "{}: {} executable files looked at: {} PRX modules, {} SPU programs saved; {} need a licence, "
                       "{} other", image_path, w.out.files, w.out.prx, w.out.spu, w.out.npdrm, w.out.unreadable);
    if (out) *out = w.out;
    return true;
}

} // namespace pspocket::disc

extern "C" int ps_disc_save_modules(const char* image_path, const char* prx_dir, const char* spu_dir, char* error,
    size_t error_size)
{
    std::string why;
    pspocket::disc::DiscModules found;
    if (!image_path || !prx_dir || !spu_dir ||
        !pspocket::disc::save_disc_modules(image_path, prx_dir, spu_dir, &found, &why)) {
        if (error && error_size) std::snprintf(error, error_size, "%s", why.empty() ? "missing paths" : why.c_str());
        return -1;
    }
    return static_cast<int>(found.prx + found.spu);
}
