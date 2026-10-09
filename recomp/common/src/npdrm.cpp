// The PSN licence layer (D-580). See pspocket/recomp/npdrm.hpp.

#include "pspocket/recomp/npdrm.hpp"

#include <algorithm>
#include <fstream>

#include "pspocket/recomp/crypto.hpp"

namespace pspocket::recomp {

Key128 rap_to_rif(const Key128& rap, const NpdrmKeys& keys)
{
    // RPCS3 key_vault.cpp rap_to_rif. The rounds index the key through RAP_PBOX, a permutation of 0 to 15 (masked
    // here, so made-up tables cannot index past the key).
    Key128 key{};
    const u8 iv[16]{};
    crypto::aes_cbc_decrypt(keys.rap_key, iv, rap.data(), 16, key.data());
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 16; ++i) {
            const int p = keys.rap_pbox[i] & 0xF;
            key[p] ^= keys.rap_e1[p];
        }
        for (int i = 15; i >= 1; --i) {
            const int p = keys.rap_pbox[i] & 0xF;
            const int pp = keys.rap_pbox[i - 1] & 0xF;
            key[p] ^= key[pp];
        }
        // A subtraction of RAP_E2 with a borrow carried in RAP_PBOX's order.
        int o = 0;
        for (int i = 0; i < 16; ++i) {
            const int p = keys.rap_pbox[i] & 0xF;
            const u8 kc = static_cast<u8>(key[p] - o);
            const u8 ec2 = keys.rap_e2[p];
            if (o != 1 || kc != 0xFF) o = kc < ec2 ? 1 : 0; // else the borrow goes on (RPCS3's middle branch)
            key[p] = static_cast<u8>(kc - ec2);
        }
    }
    return key;
}

bool valid_content_id(std::string_view id)
{
    return !id.empty() && id.size() <= 0x30 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::string rap_guest_path(std::string_view content_id)
{
    if (!valid_content_id(content_id)) return {};
    return std::string(kLicenceDir) + "/" + std::string(content_id) + ".rap";
}

std::string rap_host_path(std::string_view hdd0_dir, std::string_view content_id)
{
    if (!valid_content_id(content_id)) return {};
    while (hdd0_dir.size() > 1 && hdd0_dir.back() == '/') hdd0_dir.remove_suffix(1);
    // kLicenceDir without its "/dev_hdd0".
    return std::string(hdd0_dir) + std::string(kLicenceDir.substr(9)) + "/" + std::string(content_id) + ".rap";
}

Result<Key128> read_rap_file(const std::string& host_path)
{
    std::ifstream in(host_path, std::ios::binary);
    if (!in) return make_error("no licence {} (the player's .rap file for this content)", host_path);
    Key128 rap{};
    in.read(reinterpret_cast<char*>(rap.data()), static_cast<std::streamsize>(rap.size()));
    if (in.gcount() != static_cast<std::streamsize>(rap.size())) {
        return make_error("the licence {} is {} bytes; a RAP file is 16 (RPCS3: it exists but is invalid)", host_path,
            in.gcount());
    }
    return rap;
}

NpdrmLicence hdd0_licence(std::string hdd0_dir, std::optional<Key128> klicensee)
{
    NpdrmLicence licence;
    licence.klicensee = klicensee;
    licence.rap = [dir = std::move(hdd0_dir)](std::string_view content_id) -> Result<Key128> {
        if (!valid_content_id(content_id))
            return make_error("the content ID \"{}\" is not valid, so it has no licence file", content_id);
        return read_rap_file(rap_host_path(dir, content_id));
    };
    return licence;
}

Result<Key128> npdrm_content_key(NpdrmContent kind, s32 license, std::string_view content_id,
    const NpdrmLicence& licence)
{
    const NpdrmKeys& keys = licence.npdrm_keys();
    if (!keys.present) return make_error("this build carries no NPDRM keys (scripts/self-keys.py had no RPCS3 source)");
    const bool free = kind == NpdrmContent::self ? license == kNpdrmLicenseFree : (license & 3) == kNpdrmLicenseFree;
    if (free) return licence.klicensee.value_or(keys.klic_free);
    if (kind == NpdrmContent::self && license != kNpdrmLicenseNetwork && license != kNpdrmLicenseLocal)
        return make_error("licence type {}, which no PSN executable has", license);
    if (!licence.rap) return make_error("{} needs the player's licence ({}.rap), and no licences were given", content_id,
        content_id);
    Result<Key128> rap = licence.rap(content_id);
    if (!rap) return rap.error();
    return rap_to_rif(*rap, keys);
}

} // namespace pspocket::recomp
