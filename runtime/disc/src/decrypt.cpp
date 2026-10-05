// ps_disc_decrypt_executable (D-121): the disc's EBOOT.BIN, decrypted with the keys the build carries.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
    const std::filesystem::path partial = out.string() + ".part";
    {
        std::ofstream f(partial, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(elf->data()), static_cast<std::streamsize>(elf->size()));
        if (!f) return fail(error, error_size, "cannot write " + partial.string());
    }
    std::filesystem::rename(partial, out, ec); // whole or not at all: the app takes a present file as ready
    if (ec) return fail(error, error_size, "cannot write " + out.string() + ": " + ec.message());
    return 0;
}
