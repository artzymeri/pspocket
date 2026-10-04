#include "pspocket/loader/disc.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "pspocket/core/log.hpp"
#include "pspocket/disc/iso9660.hpp"

namespace pspocket::loader {
namespace {

class IsoFile final : public lv2::VfsImage::File {
public:
    IsoFile(std::shared_ptr<const disc::Iso9660> iso, disc::Entry entry) : iso_(std::move(iso)), entry_(std::move(entry))
    {
    }
    u64 size() const override { return entry_.size; }
    bool read(u64 offset, void* dst, usize size) const override { return iso_->read(entry_, offset, dst, size); }

private:
    std::shared_ptr<const disc::Iso9660> iso_;
    disc::Entry entry_; // its extents: a file over 4 GB has several
};

// Every lookup walks the directories from the root with the reader's case-insensitive match (PS3 discs may name
// files in a different case than games ask for them).
class IsoImage final : public lv2::VfsImage {
public:
    explicit IsoImage(std::shared_ptr<const disc::Iso9660> iso) : iso_(std::move(iso)) {}

    std::optional<Stat> stat(std::string_view path) const override
    {
        const auto e = iso_->stat(path);
        if (!e) return std::nullopt;
        return Stat{e->directory, e->directory ? 0 : e->size};
    }

    std::optional<std::vector<std::pair<std::string, Stat>>> list(std::string_view path) const override
    {
        const auto entries = iso_->list(path);
        if (!entries) return std::nullopt;
        std::vector<std::pair<std::string, Stat>> out;
        out.reserve(entries->size());
        for (const disc::Entry& e : *entries) out.emplace_back(e.name, Stat{e.directory, e.directory ? 0 : e.size});
        return out;
    }

    std::shared_ptr<const File> open(std::string_view path) const override
    {
        auto e = iso_->stat(path);
        if (!e || e->directory) return nullptr;
        return std::make_shared<IsoFile>(iso_, std::move(*e));
    }

private:
    std::shared_ptr<const disc::Iso9660> iso_;
};

std::nullptr_t fail(std::string* error, std::string message)
{
    if (error) *error = std::move(message);
    return nullptr;
}

} // namespace

std::shared_ptr<const lv2::VfsImage> open_disc_image(const std::string& path, DiscInfo* info, std::string* error)
{
    std::string why;
    std::shared_ptr<disc::Source> source = disc::open_file_source(path, &why);
    if (!source) return fail(error, std::format("cannot open {}: {}", path, why));
    auto opened = disc::Iso9660::open(source, &why);
    if (!opened) return fail(error, std::format("{} is not an ISO 9660 disc image: {}", path, why));
    auto iso = std::make_shared<const disc::Iso9660>(std::move(*opened));

    const auto sfo_entry = iso->stat("PS3_GAME/PARAM.SFO");
    if (!sfo_entry) return fail(error, std::format("{} has no PS3_GAME/PARAM.SFO: not a PS3 game disc", path));
    const auto sfo_bytes = iso->read_all(*sfo_entry, 1u << 20);
    if (!sfo_bytes) return fail(error, std::format("{}: cannot read PS3_GAME/PARAM.SFO", path));

    // Disc-level encryption, checked on content as ps3disc does: a readable EBOOT starts with SCE or ELF magic.
    if (const auto eboot = iso->stat("PS3_GAME/USRDIR/EBOOT.BIN")) {
        u8 head[16] = {};
        if (!iso->read(*eboot, 0, head, sizeof(head)) || !(disc::looks_like_self(head) || disc::looks_like_elf(head))) {
            return fail(error, std::format("{} is encrypted at disc level (Redump style): it needs its disc key "
                                           "first; use a decrypted image",
                                   path));
        }
    }

    DiscInfo out;
    out.volume_id = iso->volume_id();
    out.param_sfo = *sfo_bytes;
    if (const auto sfo = disc::parse_sfo(*sfo_bytes)) {
        out.title_id = sfo->get("TITLE_ID");
        out.title = sfo->get("TITLE");
    }
    PS_LOG_INFO("loader", "disc image {}: {} \"{}\" (volume {}, {:.2f} GB)", path, out.title_id, out.title,
        out.volume_id, static_cast<double>(iso->image_bytes()) / 1e9);
    if (info) *info = std::move(out);
    return std::make_shared<const IsoImage>(std::move(iso));
}

bool mount_disc_image(lv2::Kernel& kernel, const std::string& path, DiscInfo* info, std::string* error)
{
    const auto image = open_disc_image(path, info, error);
    if (!image) return false;
    if (!kernel.vfs_mount_image("/dev_bdvd", image)) {
        if (error) *error = "mounting /dev_bdvd failed";
        return false;
    }
    if (!kernel.vfs_mount_image("/app_home", image, "PS3_GAME/USRDIR")) {
        if (error) *error = "mounting /app_home (PS3_GAME/USRDIR) failed";
        return false;
    }
    return true;
}

} // namespace pspocket::loader
