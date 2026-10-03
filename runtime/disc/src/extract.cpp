// ps_disc_extract_executables: the "pull the executables out of the user's disc image" step (plan section 9 and
// Phase 0). Copies metadata and executable modules only; game assets never leave the image.

#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "pspocket/disc/disc.h"
#include "pspocket/disc/iso9660.hpp"

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace pspocket::disc {
namespace {

constexpr u64 kMaxModuleSize = 256ull << 20;

struct Reporter {
    ps_disc_callback callback;
    void* user;
    template <class... Args>
    void operator()(std::format_string<Args...> fmt, Args&&... args) const
    {
        const std::string line = std::format(fmt, std::forward<Args>(args)...);
        if (callback) callback(user, line.c_str());
    }
};

std::string lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

bool is_module_name(const std::string& name)
{
    const std::string n = lower(name);
    auto ends = [&](std::string_view ext) { return n.size() >= ext.size() && n.ends_with(ext); };
    return ends(".sprx") || ends(".prx") || ends(".self") || ends(".elf") || ends(".bin");
}

std::string sha256_hex(const Iso9660& iso, const Entry& file)
{
#if defined(__APPLE__)
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    std::vector<u8> buffer(4u << 20);
    for (u64 done = 0; done < file.size;) {
        const usize n = static_cast<usize>(std::min<u64>(buffer.size(), file.size - done));
        if (!iso.read(file, done, buffer.data(), n)) return "read-error";
        CC_SHA256_Update(&ctx, buffer.data(), static_cast<CC_LONG>(n));
        done += n;
    }
    u8 digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    std::string hex;
    for (u8 b : digest) hex += std::format("{:02x}", b);
    return hex;
#else
    (void)iso;
    (void)file;
    return "unavailable";
#endif
}

std::string classify(const Iso9660& iso, const Entry& file)
{
    u8 head[16] = {};
    if (!iso.read(file, 0, head, std::min<u64>(sizeof(head), file.size))) return "unreadable";
    if (looks_like_self(head)) return "SELF (needs RPCS3 decryption, docs/phase0.md)";
    if (looks_like_elf(head)) return "ELF (already decrypted)";
    return "not SELF/ELF";
}

struct Walk {
    const Iso9660& iso;
    const Reporter& report;
    std::vector<std::pair<std::string, Entry>> modules; // disc path, entry
};

void collect_modules(Walk& walk, const std::string& dir, int depth)
{
    if (depth > 12) return;
    const auto entries = walk.iso.list(dir);
    if (!entries) return;
    for (const Entry& e : *entries) {
        const std::string path = dir + "/" + e.name;
        if (e.directory) {
            collect_modules(walk, path, depth + 1);
        } else if (is_module_name(e.name)) {
            if (e.size > kMaxModuleSize) {
                walk.report("  skip {} ({} MB, too large for a module)", path, e.size >> 20);
                continue;
            }
            walk.modules.emplace_back(path, e);
        }
    }
}

} // namespace
} // namespace pspocket::disc

extern "C" int ps_disc_extract_executables(const char* image_path, const char* out_dir, ps_disc_callback callback,
    void* user)
{
    using namespace pspocket;
    using namespace pspocket::disc;
    const Reporter report{callback, user};
    const auto start = std::chrono::steady_clock::now();

    std::string error;
    std::shared_ptr<Source> source = open_file_source(image_path ? image_path : "", &error);
    if (!source) {
        report("error: {}", error);
        return 3;
    }
    auto iso = Iso9660::open(source, &error);
    if (!iso) {
        report("error: not an ISO 9660 image: {}", error);
        return 1;
    }
    report("image:   {} ({:.2f} GB, volume {:.2f} GB, id '{}')", image_path, static_cast<double>(iso->image_bytes()) / 1e9,
        static_cast<double>(iso->volume_bytes()) / 1e9, iso->volume_id());

    const auto regions = iso->ps3_regions();
    if (regions.empty()) {
        report("regions: no PS3 region table in sector 0");
    } else {
        u64 encrypted_sectors = 0;
        int encrypted_count = 0;
        for (const Region& r : regions) {
            if (!r.encrypted) continue;
            encrypted_sectors += u64{r.last_sector} - r.first_sector + 1;
            encrypted_count++;
        }
        report("regions: {} ({} marked encrypted, {:.2f} GB of sectors)", regions.size(), encrypted_count,
            static_cast<double>(encrypted_sectors * kSectorSize) / 1e9);
    }

    const auto sfo_entry = iso->stat("PS3_GAME/PARAM.SFO");
    if (!sfo_entry) {
        report("error: PS3_GAME/PARAM.SFO not found: not a PS3 game disc");
        return 1;
    }
    std::string title_id;
    if (const auto bytes = iso->read_all(*sfo_entry, 1u << 20)) {
        if (const auto sfo = parse_sfo(*bytes)) {
            title_id = sfo->get("TITLE_ID");
            report("title:   {} | {} | category {} | app {} | version {} | needs firmware {}", title_id,
                sfo->get("TITLE"), sfo->get("CATEGORY"), sfo->get("APP_VER"), sfo->get("VERSION"),
                sfo->get("PS3_SYSTEM_VER"));
        } else {
            report("warning: PARAM.SFO doesn't parse");
        }
    }

    const auto eboot = iso->stat("PS3_GAME/USRDIR/EBOOT.BIN");
    if (!eboot) {
        report("error: PS3_GAME/USRDIR/EBOOT.BIN not found");
        return 1;
    }
    // Disc-level encryption check on content: a readable EBOOT starts with "SCE\0" (or "\x7fELF").
    u8 head[16] = {};
    iso->read(*eboot, 0, head, sizeof(head));
    const bool eboot_readable = looks_like_self(head) || looks_like_elf(head);
    const bool eboot_in_encrypted = iso->in_encrypted_region(eboot->extents.front().lba,
        static_cast<u32>((eboot->extents.front().size + kSectorSize - 1) / kSectorSize));
    if (!eboot_readable) {
        report("error: EBOOT.BIN doesn't start with SCE/ELF magic{}: the image is encrypted at disc level "
               "(Redump style) and needs its disc key (.dkey or IRD) before anything can be extracted",
            eboot_in_encrypted ? " and sits in an encrypted region" : "");
        return 2;
    }
    report("disc:    decrypted at disc level (EBOOT.BIN reads as {}{})", looks_like_self(head) ? "SELF" : "ELF",
        eboot_in_encrypted ? ", although the region table marks its sectors encrypted" : "");

    Walk walk{*iso, report, {}};
    collect_modules(walk, "PS3_GAME/USRDIR", 0);

    std::vector<std::pair<std::string, Entry>> files;
    if (const auto sfb = iso->stat("PS3_DISC.SFB")) files.emplace_back("PS3_DISC.SFB", *sfb);
    files.emplace_back("PS3_GAME/PARAM.SFO", *sfo_entry);
    for (auto& m : walk.modules) files.push_back(m);

    const std::string out = out_dir ? out_dir : ".";
    u64 total = 0;
    for (const auto& [path, entry] : files) {
        if (!iso->extract(entry, out + "/" + path, nullptr, &error)) {
            report("error: {}", error);
            return 3;
        }
        total += entry.size;
        report("  {:<44} {:>10} bytes  {}  sha256 {}", path, entry.size,
            path.ends_with("SFO") || path.ends_with("SFB") ? "metadata" : classify(*iso, entry),
            sha256_hex(*iso, entry));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    report("done:    {} files, {:.1f} MB, {} -> {} in {:.1f} s", files.size(), static_cast<double>(total) / 1e6,
        title_id.empty() ? "?" : title_id, out, seconds);
    return 0;
}
