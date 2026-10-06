// The system library's host side (D-178). See pspocket/loader/system.hpp.

#include "pspocket/loader/system.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/ppu/prx_dispatch.hpp"
#include "pspocket/recomp/hash.hpp"

namespace pspocket::loader {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kChan = "system";

} // namespace

std::unique_ptr<SystemLibrary> SystemLibrary::load(const std::string& dir, LleModules& lle, const GuestMemory& memory,
    std::string* why)
{
    const fs::path library = fs::path(dir) / "game.dylib";
    std::error_code ec;
    if (!fs::exists(library, ec)) {
        if (why) *why = std::format("no system library in {}: the firmware's modules run under the interpreter", dir);
        return nullptr;
    }
    std::unique_ptr<SystemLibrary> s(new SystemLibrary(memory));
    std::ifstream checks(fs::path(dir) / "modules.txt");
    for (std::string line; std::getline(checks, line);) {
        std::istringstream words(line);
        std::string kind, key, addr, size, sha;
        if (!(words >> kind >> key >> addr >> size >> sha) || kind != "M") continue;
        s->modules_[key].push_back({static_cast<u32>(std::stoul(addr, nullptr, 16)),
            static_cast<u32>(std::stoul(size, nullptr, 16)), sha});
    }
    if (s->modules_.empty()) {
        if (why) *why = std::format("{}: no modules.txt", dir);
        return nullptr;
    }
    const recomp::Sha256Digest identity = recomp::Sha256::hash(std::string_view(lle.identity_text()));
    game::LoadOptions options;
    options.executable_sha256 = identity.data();
    options.check_image_words = false; // its modules are checked one by one as they load
    std::string error;
    if (!game::load_library(library.string(), memory, nullptr, &error, options)) {
        if (why) *why = error + " (prepare the system software again)";
        return nullptr;
    }
    SystemLibrary* self = s.get();
    lle.set_on_load([self](const PrxModule& m) { self->check(m); });
    PS_LOG_INFO(kChan, "system library {}: {} modules translated, {} functions", library.string(), s->modules_.size(),
        ppu::prx_dispatch_registered());
    return s;
}

void SystemLibrary::check(const PrxModule& module)
{
    const auto it = modules_.find(module.file);
    if (it == modules_.end()) return; // one pspocket serves itself, or not translated
    for (const Segment& seg : it->second) {
        const bool mapped = memory_.is_committed(seg.addr) && memory_.is_committed(seg.addr + seg.size - 1);
        const std::string sha = mapped ? recomp::to_hex(recomp::Sha256::hash(
            std::span<const u8>(memory_.base() + seg.addr, seg.size))) : std::string();
        if (sha != seg.sha256) {
            refused_++;
            PS_LOG_WARN(kChan, "{}: its code at {:#010x} is not what was translated: it runs under the interpreter",
                module.file, seg.addr);
            return;
        }
    }
    for (const Segment& seg : it->second) ppu::prx_dispatch_enable(seg.addr, seg.addr + seg.size);
    translated_++;
    PS_LOG_INFO(kChan, "{} runs translated", module.file);
}

} // namespace pspocket::loader
