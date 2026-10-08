// A game's own modules translated ahead of time (D-238). See pspocket/loader/game_modules.hpp.

#include "pspocket/loader/game_modules.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/ppu/prx_dispatch.hpp"
#include "pspocket/recomp/hash.hpp"

namespace pspocket::loader {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kChan = "modules";

bool read_text(const fs::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

GameModules::~GameModules() { runtime_.set_game_translations(nullptr); }

std::unique_ptr<GameModules> GameModules::load(const std::string& dir, const std::string& executable_sha256,
    RuntimeModules& modules, const LleModules* lle, const GuestMemory& memory, std::string* why)
{
    auto refuse = [&](std::string reason) -> std::unique_ptr<GameModules> {
        if (why) *why = std::move(reason);
        return nullptr;
    };
    const fs::path library = fs::path(dir) / "game.dylib";
    std::error_code ec;
    if (!fs::exists(library, ec)) return refuse("none");
    std::string text;
    if (!read_text(fs::path(dir) / "identity.txt", text)) return refuse(dir + ": no identity.txt");
    auto layout = recomp::parse_game_module_layout(text);
    if (!layout) return refuse(dir + "/identity.txt: " + layout.error().message);
    if (layout->executable_sha256 != executable_sha256)
        return refuse("made for another executable (prepare the game again)");
    if (lle && layout->lowest() < lle->layout_end())
        return refuse(std::format("the firmware's modules now reach its slots ({:#010x} under {:#010x}): prepare the "
                                  "game again", layout->lowest(), lle->layout_end()));
    std::unique_ptr<GameModules> g(new GameModules(memory, modules));
    g->layout_ = std::move(*layout);
    std::ifstream checks(fs::path(dir) / "modules.txt");
    for (std::string line; std::getline(checks, line);) {
        std::istringstream words(line);
        std::string kind, identity, addr, size, sha;
        if (!(words >> kind >> identity >> addr >> size >> sha) || kind != "M") continue;
        g->code_[identity].push_back({static_cast<u32>(std::stoul(addr, nullptr, 16)),
            static_cast<u32>(std::stoul(size, nullptr, 16)), sha});
    }
    const recomp::Sha256Digest identity = recomp::Sha256::hash(std::string_view(text));
    game::LoadOptions options;
    options.executable_sha256 = identity.data();
    options.check_image_words = false; // each module is checked as it loads
    std::string error;
    if (!game::load_library(library.string(), memory, nullptr, &error, options))
        return refuse(error + " (prepare the game again)");
    modules.set_game_translations(g.get());
    PS_LOG_INFO(kChan, "modules library {}: {} of the game's modules translated", library.string(), g->code_.size());
    return g;
}

std::vector<u32> GameModules::slot(std::string_view identity) const
{
    const auto* m = layout_.find(identity);
    return m && code_.contains(identity) ? m->addrs : std::vector<u32>{};
}

bool GameModules::matches(std::string_view identity, const PrxModule& placed) const
{
    const auto it = code_.find(identity);
    if (it == code_.end()) return false;
    for (const Segment& seg : it->second) {
        const bool mapped = memory_.is_committed(seg.addr) && memory_.is_committed(seg.addr + seg.size - 1);
        const std::string sha = mapped ? recomp::to_hex(recomp::Sha256::hash(
            std::span<const u8>(memory_.base() + seg.addr, seg.size))) : std::string();
        if (sha != seg.sha256) {
            PS_LOG_WARN(kChan, "{} ({}): its code at {:#010x} is not what was translated", placed.name,
                identity.substr(0, 16), seg.addr);
            return false;
        }
    }
    return true;
}

void GameModules::enable(std::string_view identity, const PrxModule& module)
{
    const auto it = code_.find(identity);
    if (it == code_.end()) return;
    for (const Segment& seg : it->second) ppu::prx_dispatch_enable(seg.addr, seg.addr + seg.size);
    {
        std::lock_guard lock(mutex_);
        enabled_.emplace(identity);
    }
    translated_++;
    PS_LOG_INFO(kChan, "{} ({}) runs translated", module.name, identity.substr(0, 16));
}

} // namespace pspocket::loader

namespace pspocket::loader {

bool GameModules::enabled(std::string_view identity) const
{
    std::lock_guard lock(mutex_);
    return enabled_.find(identity) != enabled_.end();
}

} // namespace pspocket::loader
