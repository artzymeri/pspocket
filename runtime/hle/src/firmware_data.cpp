// See firmware_data.hpp.

#include "firmware_data.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/prx_place.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

namespace pspocket::hle {
namespace {

constexpr const char* kChan = "hle";

// One module's decrypted image (empty when it could not be had), per process.
struct Images {
    std::mutex m;
    std::map<std::string, std::shared_ptr<std::vector<u8>>, std::less<>> elf;
};

std::shared_ptr<std::vector<u8>> image_of(HleProcess& p, std::string_view file, std::string_view identity)
{
    Images& im = p.state<Images>();
    std::lock_guard lock(im.m);
    const auto it = im.elf.find(file);
    if (it != im.elf.end()) return it->second;
    auto bytes = std::make_shared<std::vector<u8>>();
    im.elf.emplace(std::string(file), bytes);
    const std::string path = "/dev_flash/sys/external/" + std::string(file);
    std::vector<u8> self;
    if (!p.kernel().vfs_read_file(path, self)) {
        PS_LOG_WARN(kChan, "{} can't be read (no system software installed?)", path);
        return bytes;
    }
    auto elf_bytes = recomp::decrypt_self(self, recomp::builtin_self_keys());
    if (!elf_bytes) {
        PS_LOG_WARN(kChan, "{} doesn't open: {}", path, elf_bytes.error().message);
        return bytes;
    }
    auto elf = recomp::ElfFile::parse(*elf_bytes);
    if (!elf) {
        PS_LOG_WARN(kChan, "{}: {}", path, elf.error().message);
        return bytes;
    }
    const std::string id = recomp::prx_identity(*elf);
    if (id != identity) {
        PS_LOG_WARN(kChan, "{} is module {}, not the firmware 4.92 one whose data pspocket reads", path, id);
        return bytes;
    }
    *bytes = std::move(*elf_bytes);
    PS_LOG_INFO(kChan, "{}: data read from the installed system software (module {})", path, id);
    return bytes;
}

} // namespace

std::optional<std::vector<u8>> firmware_module_data(HleProcess& process, std::string_view file,
    std::string_view identity, u64 vaddr, u64 size)
{
    const std::shared_ptr<std::vector<u8>> bytes = image_of(process, file, identity);
    if (bytes->empty()) return std::nullopt;
    auto elf = recomp::ElfFile::parse(*bytes);
    if (!elf) return std::nullopt;
    const auto data = elf->read_vaddr(vaddr, size);
    if (!data) return std::nullopt;
    return std::vector<u8>(data->begin(), data->end());
}

} // namespace pspocket::hle
