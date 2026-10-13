// Games that start from /dev_hdd0/game (D-939). See pspocket/loader/hdd_game.hpp.

#include "pspocket/loader/hdd_game.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include "pspocket/disc/iso9660.hpp"
#include "pspocket/lv2/kernel.hpp"

namespace pspocket::loader {
namespace {

namespace fs = std::filesystem;

void fail(std::string* error, std::string message)
{
    if (error) *error = std::move(message);
}

// The path with its links resolved as far as it exists (the iPhone's /var is /private/var), without a trailing '/'.
fs::path resolved(const std::string& path)
{
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(path), ec);
    if (ec) p = fs::path(path).lexically_normal();
    if (!p.has_filename() && p.has_parent_path()) p = p.parent_path();
    return p;
}

// The folder's PARAM.SFO; title_id stays as PARAM.SFO has it (empty when it has none).
std::optional<HddGame> read_folder(const fs::path& folder, std::string* error)
{
    HddGame g;
    g.dir = folder.filename().string();
    g.host_dir = folder.string();
    g.boot_path = "/dev_hdd0/game/" + g.dir + "/USRDIR/EBOOT.BIN";
    std::ifstream in(folder / "PARAM.SFO", std::ios::binary);
    if (!in) {
        fail(error, std::format("{} has no PARAM.SFO", g.host_dir));
        return std::nullopt;
    }
    g.param_sfo.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const auto sfo = disc::parse_sfo(g.param_sfo);
    if (!sfo) {
        fail(error, std::format("{}: its PARAM.SFO cannot be read", g.host_dir));
        return std::nullopt;
    }
    g.title_id = sfo->get("TITLE_ID");
    g.title = sfo->get("TITLE");
    g.category = sfo->get("CATEGORY");
    g.app_ver = sfo->get("APP_VER");
    return g;
}

} // namespace

hle::GameInfo HddGame::game_info() const
{
    hle::GameInfo info;
    info.boot = hle::GameInfo::Boot::Hdd;
    info.title_id = title_id.empty() ? dir : title_id;
    info.param_sfo = param_sfo;
    info.dir = dir;
    return info;
}

std::optional<HddGame> read_hdd_game(const std::string& hdd0_dir, const std::string& game_dir, std::string* error)
{
    const fs::path folder = resolved(game_dir);
    if (folder.parent_path() != resolved(hdd0_dir) / "game") {
        fail(error, std::format("{} is not a folder of {}/game (/dev_hdd0/game)", game_dir, hdd0_dir));
        return std::nullopt;
    }
    return read_folder(folder, error);
}

std::optional<HddGame> find_update(const std::string& hdd0_dir, std::string_view title_id)
{
    if (title_id.empty() || title_id.find('/') != std::string_view::npos || title_id.starts_with('.')) {
        return std::nullopt;
    }
    const fs::path folder = resolved(hdd0_dir) / "game" / std::string(title_id);
    std::error_code ec;
    if (!fs::is_regular_file(folder / "USRDIR" / "EBOOT.BIN", ec)) return std::nullopt;
    auto g = read_folder(folder, nullptr);
    // RPCS3 Emulator::Load ("Check game updates"): its TITLE_ID is the disc's, its category GD.
    if (!g || g->title_id != title_id || !g->update()) return std::nullopt;
    return g;
}

std::optional<HddGame> hdd_game_of(const std::string& hdd0_dir, const std::string& executable, std::string* error)
{
    const fs::path games = resolved(hdd0_dir) / "game";
    const fs::path relative = resolved(executable).lexically_relative(games);
    if (relative.empty() || relative.is_absolute() || std::distance(relative.begin(), relative.end()) < 2 ||
        *relative.begin() == "..") {
        return std::nullopt;
    }
    auto g = read_folder(games / *relative.begin(), error);
    if (!g) return std::nullopt;
    g->boot_path = "/dev_hdd0/game/" + relative.generic_string();
    return g;
}

bool mount_app_home(lv2::Kernel& kernel, const HddGame& game, std::string* error)
{
    const std::string usrdir = (fs::path(game.host_dir) / "USRDIR").string();
    if (!kernel.vfs_mount("/app_home", usrdir, false)) {
        fail(error, std::format("mounting /app_home ({}) failed", usrdir));
        return false;
    }
    return true;
}

} // namespace pspocket::loader
