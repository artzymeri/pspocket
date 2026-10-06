#include "pspocket/recomp/firmware.hpp"

#include <algorithm>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include "pspocket/recomp/bytes.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

namespace pspocket::recomp {
namespace {

namespace fs = std::filesystem;

// The bytes of a file, mapped (a PUP is 200 MB): unmapped when it goes.
class Mapped {
public:
    explicit Mapped(const std::string& path)
    {
        fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st{};
        if (fd_ < 0 || fstat(fd_, &st) != 0 || st.st_size <= 0) return;
        size_ = static_cast<usize>(st.st_size);
        void* p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p != MAP_FAILED) data_ = static_cast<const u8*>(p);
    }
    ~Mapped()
    {
        if (data_) munmap(const_cast<u8*>(data_), size_);
        if (fd_ >= 0) close(fd_);
    }
    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;
    std::span<const u8> bytes() const { return data_ ? std::span<const u8>(data_, size_) : std::span<const u8>(); }

private:
    int fd_ = -1;
    const u8* data_ = nullptr;
    usize size_ = 0;
};

u64 octal(const u8* text, usize n)
{
    u64 v = 0;
    for (usize i = 0; i < n && text[i]; i++) {
        if (text[i] == ' ') continue;
        if (text[i] < '0' || text[i] > '7') break;
        v = v * 8 + (text[i] - '0');
    }
    return v;
}

std::string field(const u8* text, usize n) { return std::string(reinterpret_cast<const char*>(text), strnlen(reinterpret_cast<const char*>(text), n)); }

} // namespace

const PupFile* Pup::find(u64 id) const
{
    for (const PupFile& f : files)
        if (f.id == id) return &f;
    return nullptr;
}

Result<Pup> read_pup(std::span<const u8> data)
{
    // RPCS3 PUP.h: PUPHeader (magic "SCEUF\0\0\0", package and image versions, file count, header and data lengths),
    // then the file entries (id, offset, length, 8 bytes of padding), then the hash entries.
    const ByteReader r(data);
    if (!r.in_bounds(0, 0x30) || std::memcmp(data.data(), "SCEUF", 5) != 0) return make_error("not a PS3 update (PUP)");
    Pup pup;
    pup.package_version = *r.be64(0x08);
    pup.image_version = *r.be64(0x10);
    const u64 count = *r.be64(0x18), header_length = *r.be64(0x20), data_length = *r.be64(0x28);
    if (count == 0 || count > 1024 || !r.in_bounds(0x30, count * 0x20)) return make_error("a PUP of {} files", count);
    if (header_length + data_length > data.size())
        return make_error("the PUP is {} bytes, its header says {}: incomplete?", data.size(), header_length + data_length);
    for (u64 i = 0; i < count; i++) {
        const u64 o = 0x30 + i * 0x20;
        PupFile f{*r.be64(o), *r.be64(o + 0x08), *r.be64(o + 0x10)};
        if (!r.in_bounds(f.offset, f.size)) return make_error("file {:#x} of the PUP lies outside it", f.id);
        pup.files.push_back(f);
    }
    return pup;
}

Result<std::vector<TarEntry>> read_tar(std::span<const u8> tar)
{
    // ustar (RPCS3 Loader/TAR.h TARHeader): 512-byte headers, data padded to 512, two zero blocks at the end.
    std::vector<TarEntry> out;
    u64 at = 0;
    while (at + 512 <= tar.size()) {
        const u8* h = tar.data() + at;
        if (h[0] == 0) break; // the end
        if (std::memcmp(h + 257, "ustar", 5) != 0) return make_error("not a ustar archive at {:#x}", at);
        TarEntry e;
        const std::string prefix = field(h + 345, 155), name = field(h, 100);
        e.name = prefix.empty() ? name : prefix + "/" + name;
        e.size = octal(h + 124, 12);
        e.type = static_cast<char>(h[156]);
        e.offset = at + 512;
        if (e.offset + e.size > tar.size()) return make_error("{} runs past the archive", e.name);
        out.push_back(std::move(e));
        at += 512 + (out.back().size + 511) / 512 * 512;
    }
    return out;
}

Result<FirmwareInstall> install_firmware(const std::string& pup_path, const std::string& dev_flash_dir,
    const std::function<void(double, const std::string&)>& progress, const PackageKey* given_key)
{
    const Mapped file(pup_path);
    const std::span<const u8> bytes = file.bytes();
    if (bytes.empty()) return make_error("cannot read {}", pup_path);
    auto pup = read_pup(bytes);
    if (!pup) return pup.error();
    FirmwareInstall result;
    if (const PupFile* v = pup->find(0x100)) {
        result.version = std::string(reinterpret_cast<const char*>(bytes.data() + v->offset), static_cast<usize>(v->size));
        result.version = result.version.substr(0, result.version.find_first_of("\r\n"));
    }
    if (result.version.empty()) return make_error("the PUP has no version");
    const PupFile* update = pup->find(0x300);
    if (!update) return make_error("the PUP has no update files (0x300)");
    const auto archive = bytes.subspan(static_cast<usize>(update->offset), static_cast<usize>(update->size));
    auto entries = read_tar(archive);
    if (!entries) return entries.error();
    std::vector<TarEntry> packages;
    for (const TarEntry& e : *entries)
        if (e.name.find("dev_flash_") != std::string::npos) packages.push_back(e);
    if (packages.empty()) return make_error("the PUP has no dev_flash packages");

    const PackageKey& key = given_key ? *given_key : builtin_package_key();
    std::error_code ec;
    fs::create_directories(dev_flash_dir, ec);
    for (usize i = 0; i < packages.size(); i++) {
        const TarEntry& p = packages[i];
        if (progress) progress(static_cast<double>(i) / static_cast<double>(packages.size()), p.name);
        auto sections = decrypt_sce_package(archive.subspan(static_cast<usize>(p.offset), static_cast<usize>(p.size)), key);
        if (!sections) return make_error("{}: {}", p.name, sections.error().message);
        if (sections->size() < 3) return make_error("{}: {} sections, not 3", p.name, sections->size());
        const std::vector<u8>& inner = (*sections)[2];
        auto files = read_tar(inner);
        if (!files) return make_error("{}: {}", p.name, files.error().message);
        for (const TarEntry& f : *files) {
            // "dev_flash/sys/external/libfs.sprx" -> dev_flash_dir/sys/external/libfs.sprx; nothing outside it.
            std::string name = f.name;
            if (!name.starts_with("dev_flash/")) continue;
            name = name.substr(10);
            if (name.empty() || name.find("..") != std::string::npos) continue;
            const fs::path target = fs::path(dev_flash_dir) / name;
            if (f.type == '5') {
                if (firmware_path_used(name)) fs::create_directories(target, ec);
                continue;
            }
            if (f.type != '0' && f.type != 0) continue;
            if (!firmware_path_used(name)) {
                result.files_skipped++;
                result.bytes_skipped += f.size;
                continue;
            }
            fs::create_directories(target.parent_path(), ec);
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(inner.data() + f.offset), static_cast<std::streamsize>(f.size));
            if (!out) return make_error("cannot write {}", target.string());
            result.files++;
            result.bytes += f.size;
        }
        result.packages++;
    }
    if (progress) progress(1.0, "done");
    return result;
}

bool firmware_path_used(std::string_view path)
{
    return path.starts_with("sys/") || path.starts_with("data/font/") || path.starts_with("data/cert/");
}

bool firmware_module_native(std::string_view key)
{
    if (key.starts_with("internal/")) key.remove_prefix(9);
    if (key.starts_with("liblv2") || key.starts_with("libsysutil") || key.starts_with("libfs_utility")) return true;
    static constexpr std::string_view kNative[] = {"libsysmodule.sprx", "libsre.sprx", "libgcm_sys.sprx",
        "libaudio.sprx", "libvoice.sprx", "libio.sprx", "libfs.sprx", "libfs_155.sprx", "libnet.sprx",
        "libnetctl.sprx", "libcamera.sprx", "libmic.sprx", "libgem.sprx", "libusbd.sprx", "libusbpspcm.sprx",
        "sys_audio.sprx", "sysvoice_modules.sprx", "libvoice_internal.sprx"};
    return std::find(std::begin(kNative), std::end(kNative), key) != std::end(kNative);
}

bool firmware_module_translated(std::string_view key)
{
    // liblv2coredump and liblv2dbg_for_cex are native already (liblv2*).
    return key.ends_with(".sprx") && !firmware_module_native(key);
}

std::string firmware_identity_line(std::string_view key, std::string_view sha256, std::span<const u32> addrs)
{
    std::string line = std::format("{} {}", key, sha256);
    for (const u32 a : addrs) line += std::format(" {:x}", a);
    return line + '\n';
}

u64 prune_firmware(const std::string& dev_flash_dir)
{
    u64 freed = 0;
    std::error_code ec;
    for (const char* sub : {"vsh", "bdplayer", "ps1emu", "ps2emu", "pspemu", "data/dic"}) {
        const fs::path dir = fs::path(dev_flash_dir) / sub;
        if (!fs::exists(dir, ec)) continue;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec))
            if (e.is_regular_file(ec)) freed += e.file_size(ec);
        fs::remove_all(dir, ec);
    }
    return freed;
}

} // namespace pspocket::recomp
