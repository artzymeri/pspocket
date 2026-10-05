// ps_disc_decrypt_executable (D-121): the disc's EBOOT.BIN, decrypted with the keys the build carries.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "pspocket/disc/disc.h"
#include "pspocket/disc/iso9660.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

namespace {

int fail(char* error, size_t size, const std::string& message)
{
    if (error && size) std::snprintf(error, size, "%s", message.c_str());
    return 1;
}

} // namespace

extern "C" int ps_disc_decrypt_executable(const char* image_path, const char* elf_path, char* error, size_t error_size)
{
    using namespace pspocket;
    if (!image_path || !elf_path) return fail(error, error_size, "missing paths");
    std::string why;
    std::shared_ptr<disc::Source> source = disc::open_file_source(image_path, &why);
    if (!source) return fail(error, error_size, why);
    auto iso = disc::Iso9660::open(source, &why);
    if (!iso) return fail(error, error_size, "not an ISO 9660 image: " + why);
    const auto eboot = iso->stat("PS3_GAME/USRDIR/EBOOT.BIN");
    if (!eboot) return fail(error, error_size, "no PS3_GAME/USRDIR/EBOOT.BIN on the disc");
    const auto self = iso->read_all(*eboot, 256u << 20);
    if (!self) return fail(error, error_size, "cannot read EBOOT.BIN from the disc (is the image still disc-encrypted?)");
    auto elf = recomp::decrypt_self(*self, recomp::builtin_self_keys());
    if (!elf) return fail(error, error_size, "EBOOT.BIN: " + elf.error().message);
    const std::filesystem::path out(elf_path);
    std::error_code ec;
    // The missing folders, made from the deepest one that exists: iOS's sandbox does not let the app look at the
    // folders above its container, which create_directories may (the phone could not write game/<TITLE_ID>/).
    std::vector<std::filesystem::path> missing;
    for (auto p = out.parent_path(); !p.empty() && !std::filesystem::is_directory(p, ec); p = p.parent_path()) {
        missing.push_back(p);
        if (p == p.parent_path()) break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (::mkdir(it->c_str(), 0755) != 0 && errno != EEXIST) {
            return fail(error, error_size, "cannot make " + it->string() + ": " + std::strerror(errno));
        }
    }
    const std::filesystem::path partial = out.string() + ".part";
    {
        std::FILE* f = std::fopen(partial.c_str(), "wb");
        if (!f) return fail(error, error_size, "cannot write " + partial.string() + ": " + std::strerror(errno));
        const bool written = std::fwrite(elf->data(), 1, elf->size(), f) == elf->size();
        const int write_errno = errno;
        if (std::fclose(f) != 0 || !written) {
            std::remove(partial.c_str());
            return fail(error, error_size, "cannot write " + partial.string() + ": " + std::strerror(written ? errno : write_errno));
        }
    }
    std::filesystem::rename(partial, out, ec); // whole or not at all: the app takes a present file as ready
    if (ec) return fail(error, error_size, "cannot write " + out.string() + ": " + ec.message());
    return 0;
}
