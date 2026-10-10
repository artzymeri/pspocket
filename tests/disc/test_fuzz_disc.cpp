// Mutation fuzzing of the disc reader (D-603, tests/common/mutation_fuzz.hpp): synthetic discs' ISO 9660 and Joliet
// trees walked and read, PARAM.SFO, the disc image as the VFS image the game's file system reads, and the scan of a
// disc's own modules. Each mutated image must open and read, or be refused. Longer: PSPOCKET_FUZZ_ITERS=<n>
// build/host/tests/disc/test_disc "[fuzz]".

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "disc_writer.hpp"
#include "mutation_fuzz.hpp"
#include "pspocket/disc/iso9660.hpp"
#include "pspocket/disc/modules.hpp"
#include "pspocket/loader/disc.hpp"
#include "synthetic.hpp"

using namespace pspocket;
using namespace pspocket::disc;
using namespace pspocket::disc_test;
namespace fuzz = pspocket::test::fuzz;
namespace fs = std::filesystem;

namespace {

// The fields of a disc's metadata: the region table (sector 0), the volume descriptors' sizes and root records, and
// every directory record in the sectors after them (its length, both copies of its extent and size, its flags and
// its name's length).
std::vector<fuzz::Field> iso_fields(const std::vector<u8>& image, u32 last_metadata_sector)
{
    std::vector<fuzz::Field> f = fuzz::fields(0, 32);
    for (u32 sector = 16; sector <= last_metadata_sector; sector++) {
        const usize s = usize{sector} * kSectorSize;
        if (s + kSectorSize > image.size()) break;
        if (std::memcmp(image.data() + s + 1, "CD001", 5) == 0) { // a volume descriptor
            fuzz::append(f, {{s, 1}, {s + 80, 4, true}, {s + 128, 2, true}, {s + 156, 1}, {s + 158, 4, true},
                                {s + 166, 4, true}, {s + 181, 1}, {s + 188, 1}});
            continue;
        }
        for (usize pos = 0; pos + 34 <= kSectorSize && image[s + pos] >= 34;) {
            const usize r = s + pos;
            fuzz::append(f, {{r, 1}, {r + 2, 4, true}, {r + 6, 4}, {r + 10, 4, true}, {r + 14, 4}, {r + 25, 1},
                                {r + 32, 1}});
            pos += image[r];
        }
    }
    return f;
}

// Everything a game's file system asks of a disc: the tree walked (to a depth, as a directory may name its parent),
// files stat'ed and read, whole and in pieces, the region table.
void walk(const Iso9660& iso, fuzz::Rng& rng)
{
    (void)iso.volume_id();
    (void)iso.ps3_regions();
    (void)iso.in_encrypted_region(static_cast<u32>(rng.below(400)), static_cast<u32>(rng.below(64)));
    u32 visited = 0;
    std::vector<std::pair<std::string, int>> todo{{"", 0}};
    while (!todo.empty() && visited < 200) {
        const auto [path, depth] = todo.back();
        todo.pop_back();
        const auto entries = iso.list(path);
        if (!entries) continue;
        for (const Entry& e : *entries) {
            if (++visited > 200) break;
            const std::string child = path.empty() ? e.name : path + "/" + e.name;
            (void)iso.stat(child);
            if (e.directory) {
                if (depth < 6) todo.push_back({child, depth + 1});
                continue;
            }
            (void)iso.read_all(e, 1u << 20);
            if (e.size) {
                u8 piece[300];
                const u64 at = rng.below(e.size);
                (void)iso.read(e, at, piece, static_cast<usize>(std::min<u64>(sizeof piece, e.size - at)));
            }
        }
    }
    (void)iso.stat("PS3_GAME/USRDIR/EBOOT.BIN");
    (void)iso.stat("dataps3_flashback_paradise.forge");
}

fs::path temp_dir(std::string_view what)
{
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-fuzz-{}-{}", what, static_cast<long>(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& path, const std::vector<u8>& bytes)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// The PS3-shaped disc's metadata (up to sector 21) as a sample of its own, the files laid back where they were after
// each mutation: most of the image is file data, which the reader only copies.
struct MetadataDisc {
    std::vector<u8> image;
    static constexpr usize kMetadataBytes = 22 * kSectorSize;
    std::vector<u8> with_files(const std::vector<u8>& metadata) const
    {
        std::vector<u8> out = metadata;
        if (out.size() < image.size()) {
            const usize keep = out.size();
            out.resize(image.size());
            std::copy(image.begin() + static_cast<std::ptrdiff_t>(std::max(keep, kMetadataBytes)), image.end(),
                out.begin() + static_cast<std::ptrdiff_t>(std::max(keep, kMetadataBytes)));
        }
        return out;
    }
};

} // namespace

TEST_CASE("fuzz: ISO 9660 and Joliet discs, walked and read", "[disc][fuzz]")
{
    const Ps3Disc disc(false);
    const IsoBuilder joliet = joliet_disc("dataps3_flashback_paradise.forge", pattern(3000, 7));
    const MetadataDisc files{disc.iso.image};
    std::vector<u8> metadata(disc.iso.image.begin(), disc.iso.image.begin() + MetadataDisc::kMetadataBytes);
    const std::vector<fuzz::Sample> whole = {
        {"ps3 disc", disc.iso.image, iso_fields(disc.iso.image, 21)},
        {"joliet disc", joliet.image, iso_fields(joliet.image, 30)},
    };
    const std::vector<fuzz::Sample> metadata_only = {{"ps3 disc metadata", metadata, iso_fields(disc.iso.image, 21)}};
    const auto read = [](std::vector<u8> image, fuzz::Rng& rng) {
        auto iso = Iso9660::open(memory_source(std::move(image)), nullptr);
        if (!iso) return false;
        walk(*iso, rng);
        return true;
    };
    fuzz::Tally tally("iso9660");
    fuzz::run(tally, whole, 1500, [&](const std::vector<u8>& input, fuzz::Rng& rng) { return read(input, rng); });
    fuzz::run(tally, metadata_only, 3000,
        [&](const std::vector<u8>& input, fuzz::Rng& rng) { return read(files.with_files(input), rng); });
}

TEST_CASE("fuzz: PARAM.SFO as the disc reader parses it", "[disc][fuzz]")
{
    std::vector<fuzz::Sample> samples;
    const auto add = [&](std::string name, std::vector<u8> sfo) {
        std::vector<fuzz::Field> f = {{4, 4, true}, {8, 4, true}, {12, 4, true}, {16, 4, true}};
        const u32 count = static_cast<u32>(fuzz::load_field(sfo, 16, 4, true));
        for (u32 i = 0; i < count; i++) {
            const usize e = 20 + usize{i} * 16;
            fuzz::append(f, {{e, 2, true}, {e + 2, 2, true}, {e + 4, 4, true}, {e + 8, 4, true}, {e + 12, 4, true}});
        }
        samples.push_back({std::move(name), std::move(sfo), std::move(f)});
    };
    add("disc sfo", make_sfo());
    add("game sfo", pspocket::test::build_param_sfo({{"APP_VER", "01.00"}, {"CATEGORY", "DG"}, {"PS3_SYSTEM_VER", "03.4000"},
                        {"TITLE", "A synthetic game"}, {"TITLE_ID", "BLES01807"}, {"VERSION", "01.00"}}));
    fuzz::Tally tally("param.sfo (disc)");
    fuzz::run(tally, samples, 3000, [](const std::vector<u8>& input, fuzz::Rng&) {
        const auto sfo = parse_sfo(input);
        if (!sfo) return false;
        (void)sfo->get("TITLE_ID");
        return true;
    });
}

TEST_CASE("fuzz: a disc image as the game's file system, and the scan of its modules", "[disc][fuzz]")
{
    // From a file, as the app and pspocket-boot open it: fewer inputs, each written out.
    const Ps3Disc disc(false);
    std::vector<fuzz::Sample> samples = {{"ps3 disc metadata",
        std::vector<u8>(disc.iso.image.begin(), disc.iso.image.begin() + MetadataDisc::kMetadataBytes),
        iso_fields(disc.iso.image, 21)}};
    const MetadataDisc files{disc.iso.image};
    const fs::path dir = temp_dir("disc");
    fuzz::Tally tally("disc image (vfs)");
    fuzz::run(tally, samples, 300, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
        write_file(dir / "game.iso", files.with_files(input));
        std::string error;
        loader::DiscInfo info;
        const auto image = loader::open_disc_image((dir / "game.iso").string(), &info, &error);
        DiscModules found;
        fs::remove_all(dir / "prx");
        fs::remove_all(dir / "spu");
        (void)save_disc_modules((dir / "game.iso").string(), (dir / "prx").string(), (dir / "spu").string(), &found,
            &error);
        if (!image) return false;
        (void)image->stat("");
        if (const auto listing = image->list("PS3_GAME/USRDIR")) {
            for (const auto& [name, st] : *listing) {
                if (st.dir) continue;
                if (const auto file = image->open("PS3_GAME/USRDIR/" + name)) {
                    std::vector<u8> piece(static_cast<usize>(std::min<u64>(file->size(), 512)));
                    const u64 at = file->size() > piece.size() ? rng.below(file->size() - piece.size()) : 0;
                    (void)file->read(at, piece.data(), piece.size());
                }
            }
        }
        return true;
    });
    fs::remove_all(dir);
}
