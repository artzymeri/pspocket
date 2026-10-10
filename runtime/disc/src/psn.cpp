// PSN games (D-580 to D-583). See pspocket/disc/psn.h.

#include "pspocket/disc/psn.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/pkg.hpp"
#include "pspocket/recomp/sce.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

namespace {

namespace fs = std::filesystem;
using pspocket::u8;

int fail(char* error, size_t size, const std::string& message)
{
    if (error && size) std::snprintf(error, size, "%s", message.c_str());
    return 1;
}

// The missing folders of `dir`, made from the deepest one that exists (as ps_disc_decrypt_executable: iOS's sandbox
// does not let the app look at the folders above its container, which create_directories may).
bool make_folders(const fs::path& dir, std::string& why)
{
    std::error_code ec;
    std::vector<fs::path> missing;
    for (auto p = dir; !p.empty() && !fs::is_directory(p, ec); p = p.parent_path()) {
        missing.push_back(p);
        if (p == p.parent_path()) break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (::mkdir(it->c_str(), 0755) != 0 && errno != EEXIST) {
            why = "cannot make " + it->string() + ": " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

// `bytes` as the file `out`, whole or not at all (written beside it, then renamed).
bool write_whole(const fs::path& out, const std::vector<u8>& bytes, std::string& why)
{
    if (!make_folders(out.parent_path(), why)) return false;
    const fs::path partial = out.string() + ".part";
    {
        std::ofstream f(partial, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) {
            std::error_code ignored;
            fs::remove(partial, ignored);
            why = "cannot write " + partial.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(partial, out, ec);
    if (ec) {
        why = "cannot write " + out.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

extern "C" int ps_psn_install_pkg(const char* pkg_path, const char* hdd0_dir, ps_disc_callback callback, void* user,
    char* error, size_t error_size)
{
    using namespace pspocket;
    if (!pkg_path || !hdd0_dir || !*hdd0_dir) return fail(error, error_size, "no package, or no /dev_hdd0 folder, given");
    const auto report = [&](std::string_view line) {
        if (callback) callback(user, std::string(line).c_str());
    };
    auto installed = recomp::install_pkg(pkg_path, hdd0_dir, report);
    if (!installed) return fail(error, error_size, installed.error().message);
    const recomp::PkgInstalled& r = *installed;
    report(std::format("installed {} files ({} bytes) and {} folders into {}{}", r.files, r.bytes, r.folders,
        r.install_path, r.kept ? std::format("; {} files already there kept", r.kept) : std::string()));
    if (r.eboot.empty()) return 0;
    // A game: which licence its executable needs (its NPD header), and whether the player's is in place.
    std::ifstream in(r.eboot, std::ios::binary);
    std::vector<u8> head(64u << 10);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<usize>(in.gcount()));
    const auto self = recomp::parse_self(head);
    const recomp::NpdrmInfo* npd = self ? self->npdrm() : nullptr;
    if (!npd) {
        report("the game's executable: " + r.eboot);
        return 0;
    }
    if (npd->license == recomp::kNpdrmLicenseFree) {
        report("the game's executable: " + r.eboot + " (free content: it needs no licence)");
        return 0;
    }
    const std::string rap = recomp::rap_host_path(hdd0_dir, npd->content_id);
    std::error_code ec;
    report("the game's executable: " + r.eboot + "; it needs the player's licence " + npd->content_id + ".rap, " +
        (rap.empty() ? std::string("which has no valid content ID") :
                       fs::is_regular_file(rap, ec) ? "which is in place (" + rap + ")" :
                                                      "which is not there yet (" + rap + ")"));
    return 0;
}

extern "C" int ps_psn_rap_path(const char* hdd0_dir, const char* content_id, char* out, size_t out_size)
{
    if (!hdd0_dir || !content_id || !out) return 1;
    const std::string path = pspocket::recomp::rap_host_path(hdd0_dir, content_id);
    if (path.empty() || path.size() + 1 > out_size) return 1;
    std::memcpy(out, path.c_str(), path.size() + 1);
    return 0;
}

extern "C" int ps_psn_install_rap(const char* hdd0_dir, const char* rap_path, char* error, size_t error_size)
{
    using namespace pspocket;
    if (!hdd0_dir || !rap_path) return fail(error, error_size, "missing paths");
    const std::string name = fs::path(rap_path).filename().string();
    std::string extension = name.size() > 4 ? name.substr(name.size() - 4) : std::string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".rap") return fail(error, error_size, name + ": a licence file is named <CONTENT_ID>.rap");
    const std::string content_id = name.substr(0, name.size() - 4);
    if (!recomp::valid_content_id(content_id)) {
        return fail(error, error_size,
            "\"" + content_id + "\" is not a content ID (1 to 48 letters, digits, '-' or '_'): a licence file is named "
            "<CONTENT_ID>.rap");
    }
    std::ifstream in(rap_path, std::ios::binary);
    if (!in) return fail(error, error_size, std::string("cannot read ") + rap_path);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() != 16) {
        return fail(error, error_size, name + " is " + std::to_string(bytes.size()) + " bytes; a licence file is 16");
    }
    std::string why;
    if (!write_whole(recomp::rap_host_path(hdd0_dir, content_id), bytes, why)) return fail(error, error_size, why);
    return 0;
}

extern "C" int ps_psn_decrypt_executable(const char* self_path, const char* hdd0_dir, const char* elf_path,
    char* error, size_t error_size)
{
    using namespace pspocket;
    if (!self_path || !elf_path) return fail(error, error_size, "missing paths");
    // A game's folder: its executable.
    fs::path source(self_path);
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        const fs::path psn = source / "USRDIR" / "EBOOT.BIN", disc = source / "PS3_GAME" / "USRDIR" / "EBOOT.BIN";
        if (fs::is_regular_file(psn, ec)) source = psn;
        else if (fs::is_regular_file(disc, ec)) source = disc;
        else return fail(error, error_size, source.string() + " holds no USRDIR/EBOOT.BIN");
    }
    constexpr std::uintmax_t kMax = 256u << 20;
    const std::uintmax_t size = fs::file_size(source, ec);
    if (ec) return fail(error, error_size, "cannot read " + source.string() + ": " + ec.message());
    if (size > kMax) return fail(error, error_size, source.string() + " is larger than any PS3 executable");
    std::ifstream in(source, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.is_open() || bytes.size() != size) return fail(error, error_size, "cannot read " + source.string());
    std::vector<u8> elf;
    if (bytes.size() >= 4 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F') {
        elf = std::move(bytes); // already plain
    } else {
        const recomp::NpdrmLicence licence = hdd0_dir && *hdd0_dir
            ? recomp::hdd0_licence(hdd0_dir)
            : recomp::NpdrmLicence{.klicensee = std::nullopt,
                  .rap = [](std::string_view content_id) -> recomp::Result<recomp::Key128> {
                      return recomp::make_error("no /dev_hdd0 folder was given to look for the licence {}.rap in",
                          content_id);
                  },
                  .keys = nullptr};
        auto decrypted = recomp::decrypt_self(bytes, recomp::builtin_self_keys(), &licence);
        if (!decrypted) return fail(error, error_size, source.filename().string() + ": " + decrypted.error().message);
        elf = std::move(*decrypted);
    }
    std::string why;
    if (!write_whole(elf_path, elf, why)) return fail(error, error_size, why);
    return 0;
}
