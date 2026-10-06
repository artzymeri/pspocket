// pspocket-firmware: installs /dev_flash from a PS3 system software update (PS3UPDAT.PUP), as the app does on the
// phone (D-156). The update is the user's own download; what it installs stays out of the repository (rule 1).
//
//   pspocket-firmware PS3UPDAT.PUP DEV_FLASH_DIR
//   pspocket-firmware --modules DEV_FLASH_DIR
//   pspocket-firmware --decrypt DEV_FLASH_DIR OUT_DIR
//
// Prints the update's version and, per dev_flash package, its name; then how many files and bytes went in. --modules
// lists the firmware modules (sys/external/*.sprx) as the LLE loader sees them (D-157): each module's libraries, and
// what it imports from others, functions and variables. --decrypt writes every module of sys/external and
// sys/internal (.sprx, .self) as an ELF to OUT_DIR/external and OUT_DIR/internal, for ps3inspect and
// scripts/firmware-report.py (D-176); OUT_DIR belongs under inventory/ (rule 1).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/recomp/elf.hpp"
#include "pspocket/recomp/firmware.hpp"
#include "pspocket/recomp/ppu_module.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

namespace {

int list_modules(const std::string& dev_flash)
{
    namespace fs = std::filesystem;
    using namespace pspocket;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(dev_flash) / "sys" / "external", ec))
        if (e.path().extension() == ".sprx") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "pspocket-firmware: no modules in %s/sys/external\n", dev_flash.c_str());
        return 1;
    }
    size_t variable_imports = 0;
    for (const fs::path& path : files) {
        std::ifstream in(path, std::ios::binary);
        const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto elf_bytes = recomp::decrypt_self(bytes, recomp::builtin_self_keys());
        if (!elf_bytes) {
            std::printf("%s: %s\n", path.filename().c_str(), elf_bytes.error().message.c_str());
            continue;
        }
        auto elf = recomp::ElfFile::parse(*elf_bytes);
        if (!elf) continue;
        auto module = recomp::analyze_ppu_module(*elf);
        if (!module) {
            std::printf("%s: %s\n", path.filename().c_str(), module.error().message.c_str());
            continue;
        }
        std::string exports, imports;
        for (const auto& lib : module->exports)
            if (!lib.name.empty() && !lib.management)
                exports += std::format(" {} ({} functions, {} variables)", lib.name, lib.functions.size(), lib.variables.size());
        for (const auto& lib : module->imports) {
            imports += std::format(" {} ({}{})", lib.name, lib.functions.size(),
                lib.variables.empty() ? "" : std::format(", {} variables", lib.variables.size()));
            variable_imports += lib.variables.size();
        }
        std::printf("%s:\n  exports%s\n  imports%s\n", path.filename().c_str(), exports.c_str(), imports.c_str());
    }
    std::printf("%zu modules; %zu imported variables\n", files.size(), variable_imports);
    return 0;
}

int decrypt_modules(const std::string& dev_flash, const std::string& out_dir)
{
    namespace fs = std::filesystem;
    using namespace pspocket;
    size_t written = 0, failed = 0;
    for (const char* sub : {"external", "internal"}) {
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(dev_flash) / "sys" / sub, ec))
            if (e.path().extension() == ".sprx" || e.path().extension() == ".self") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        const fs::path out = fs::path(out_dir) / sub;
        fs::create_directories(out, ec);
        for (const fs::path& path : files) {
            std::ifstream in(path, std::ios::binary);
            const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto elf = recomp::decrypt_self(bytes, recomp::builtin_self_keys());
            if (!elf) {
                std::printf("%s/%s: %s\n", sub, path.filename().c_str(), elf.error().message.c_str());
                failed++;
                continue;
            }
            std::ofstream(out / (path.filename().string() + ".elf"), std::ios::binary)
                .write(reinterpret_cast<const char*>(elf->data()), static_cast<std::streamsize>(elf->size()));
            written++;
        }
    }
    std::printf("%zu modules decrypted into %s, %zu failed\n", written, out_dir.c_str(), failed);
    return written ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--modules") return list_modules(argv[2]);
    if (argc == 4 && std::string(argv[1]) == "--decrypt") return decrypt_modules(argv[2], argv[3]);
    if (argc != 3) {
        std::fputs("usage: pspocket-firmware PS3UPDAT.PUP DEV_FLASH_DIR\n       pspocket-firmware --modules DEV_FLASH_DIR\n"
                   "       pspocket-firmware --decrypt DEV_FLASH_DIR OUT_DIR\n",
            stderr);
        return 64;
    }
    const auto r = pspocket::recomp::install_firmware(argv[1], argv[2], [](double fraction, const std::string& what) {
        std::printf("[%3.0f%%] %s\n", 100 * fraction, what.c_str());
        std::fflush(stdout);
    });
    if (!r) {
        std::fprintf(stderr, "pspocket-firmware: %s\n", r.error().message.c_str());
        return 1;
    }
    std::printf("firmware %s: %zu packages, %zu files, %.1f MB into %s\n", r->version.c_str(), r->packages, r->files,
        static_cast<double>(r->bytes) / 1048576.0, argv[2]);
    return 0;
}
