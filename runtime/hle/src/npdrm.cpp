// The licence keys a game has shown the system (D-581). See pspocket/hle/npdrm.hpp.

#include "pspocket/hle/npdrm.hpp"

#include <algorithm>
#include <format>
#include <string>

#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/kernel.hpp"

namespace pspocket::hle {

void NpdrmKeyRing::install(const recomp::Key128& key)
{
    std::lock_guard lock(mutex_);
    keys_[installed_++ % kSize] = key; // RPCS3 install_decryption_key: dec_keys[pos++ % 16]
}

std::vector<recomp::Key128> NpdrmKeyRing::newest_first() const
{
    std::lock_guard lock(mutex_);
    std::vector<recomp::Key128> out;
    for (u64 i = 0; i < std::min<u64>(installed_, kSize); i++) out.push_back(keys_[(installed_ - i - 1) % kSize]);
    return out;
}

std::optional<recomp::Key128> NpdrmKeyRing::newest() const
{
    std::lock_guard lock(mutex_);
    if (!installed_) return std::nullopt;
    return keys_[(installed_ - 1) % kSize];
}

NpdrmKeyRing* npdrm_key_ring()
{
    HleProcess* p = HleProcess::current();
    return p ? &p->state<NpdrmKeyRing>() : nullptr;
}

recomp::NpdrmLicence process_licence(const lv2::Kernel& kernel, std::optional<recomp::Key128> klicensee)
{
    recomp::NpdrmLicence licence;
    licence.klicensee = klicensee;
    licence.rap = [&kernel](std::string_view content_id) -> recomp::Result<recomp::Key128> {
        const std::string path = recomp::rap_guest_path(content_id);
        if (path.empty())
            return recomp::make_error("the content ID \"{}\" is not valid, so it has no licence file", content_id);
        std::vector<u8> bytes;
        if (!kernel.vfs_read_file(path, bytes))
            return recomp::make_error("no licence {} (the player's .rap file for this content)", path);
        if (bytes.size() < 16)
            return recomp::make_error("the licence {} is {} bytes; a RAP file is 16 (RPCS3: it exists but is invalid)",
                path, bytes.size());
        recomp::Key128 rap{};
        std::copy(bytes.begin(), bytes.begin() + 16, rap.begin());
        return rap;
    };
    return licence;
}

recomp::NpdrmLicence module_licence(const lv2::Kernel& kernel)
{
    const NpdrmKeyRing* ring = npdrm_key_ring();
    return process_licence(kernel, ring ? ring->newest() : std::nullopt);
}

} // namespace pspocket::hle
