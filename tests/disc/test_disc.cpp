#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#include "pspocket/disc/disc.h"
#include "pspocket/disc/iso9660.hpp"

using namespace pspocket;
using namespace pspocket::disc;
namespace fs = std::filesystem;

namespace {

// ---- a minimal ISO 9660 writer for tests ----

void put_le32(std::vector<u8>& b, usize at, u32 v)
{
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<u8>(v >> (8 * i));
}
void put_be32(std::vector<u8>& b, usize at, u32 v)
{
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<u8>(v >> (8 * (3 - i)));
}
void put_both32(std::vector<u8>& b, usize at, u32 v)
{
    put_le32(b, at, v);
    put_be32(b, at + 4, v);
}

std::vector<u8> dir_record(const std::string& name, u32 lba, u32 size, u8 flags)
{
    const usize name_len = name.size();
    usize length = 33 + name_len;
    if (length % 2) length++;
    std::vector<u8> r(length, 0);
    r[0] = static_cast<u8>(length);
    put_both32(r, 2, lba);
    put_both32(r, 10, size);
    r[25] = flags;
    r[28] = 1; // volume sequence number (both-endian 16-bit; LE byte is enough here)
    r[32] = static_cast<u8>(name_len);
    std::memcpy(r.data() + 33, name.data(), name_len);
    return r;
}

struct IsoBuilder {
    std::vector<u8> image = std::vector<u8>(64 * kSectorSize, 0);

    u8* sector(u32 lba)
    {
        if ((u64{lba} + 1) * kSectorSize > image.size()) image.resize((u64{lba} + 1) * kSectorSize, 0);
        return image.data() + u64{lba} * kSectorSize;
    }

    void write_records(u32 lba, const std::vector<std::vector<u8>>& records)
    {
        usize pos = 0;
        for (const auto& r : records) {
            if (pos % kSectorSize + r.size() > kSectorSize) pos = (pos / kSectorSize + 1) * kSectorSize;
            std::memcpy(sector(lba) + pos, r.data(), r.size());
            pos += r.size();
        }
    }

    void file(u32 lba, const std::vector<u8>& bytes)
    {
        for (usize i = 0; i < bytes.size(); i += kSectorSize) {
            const usize n = std::min<usize>(kSectorSize, bytes.size() - i);
            std::memcpy(sector(lba + static_cast<u32>(i / kSectorSize)), bytes.data() + i, n);
        }
    }

    void pvd(u32 root_lba, u32 root_size, const std::string& volume_id)
    {
        u8* pvd = sector(16);
        pvd[0] = 1;
        std::memcpy(pvd + 1, "CD001", 5);
        pvd[6] = 1;
        std::memset(pvd + 40, ' ', 32);
        std::memcpy(pvd + 40, volume_id.data(), volume_id.size());
        pvd[128] = kSectorSize & 0xFF;
        pvd[129] = kSectorSize >> 8;
        const auto root = dir_record(std::string(1, '\0'), root_lba, root_size, 0x02);
        std::memcpy(pvd + 156, root.data(), 34);
        u8* term = sector(17);
        term[0] = 255;
        std::memcpy(term + 1, "CD001", 5);
        term[6] = 1;
        const u32 total = static_cast<u32>(image.size() / kSectorSize);
        pvd[80] = static_cast<u8>(total);
        pvd[81] = static_cast<u8>(total >> 8);
        pvd[82] = static_cast<u8>(total >> 16);
        pvd[83] = static_cast<u8>(total >> 24);
    }
};

std::vector<u8> pattern(usize size, u32 seed)
{
    std::vector<u8> b(size);
    std::mt19937 rng(seed);
    for (auto& x : b) x = static_cast<u8>(rng());
    return b;
}

std::vector<u8> make_sfo()
{
    // Three entries: TITLE_ID (utf8-S), APP_VER (utf8), PARENTAL_LEVEL (int32).
    const std::vector<std::string> keys{"APP_VER", "PARENTAL_LEVEL", "TITLE_ID"};
    std::vector<u8> key_table;
    std::vector<u16> key_offsets;
    for (const auto& k : keys) {
        key_offsets.push_back(static_cast<u16>(key_table.size()));
        key_table.insert(key_table.end(), k.begin(), k.end());
        key_table.push_back(0);
    }
    while (key_table.size() % 4) key_table.push_back(0);
    const u32 key_start = 20 + 3 * 16;
    const u32 data_start = key_start + static_cast<u32>(key_table.size());
    std::vector<u8> sfo(data_start + 8 + 4 + 16, 0);
    std::memcpy(sfo.data(), "\0PSF", 4);
    put_le32(sfo, 4, 0x0101);
    put_le32(sfo, 8, key_start);
    put_le32(sfo, 12, data_start);
    put_le32(sfo, 16, 3);
    auto entry = [&](int i, u16 fmt, u32 len, u32 max, u32 off) {
        const usize e = 20 + static_cast<usize>(i) * 16;
        sfo[e] = static_cast<u8>(key_offsets[static_cast<usize>(i)]);
        sfo[e + 1] = static_cast<u8>(key_offsets[static_cast<usize>(i)] >> 8);
        sfo[e + 2] = static_cast<u8>(fmt);
        sfo[e + 3] = static_cast<u8>(fmt >> 8);
        put_le32(sfo, e + 4, len);
        put_le32(sfo, e + 8, max);
        put_le32(sfo, e + 12, off);
    };
    entry(0, 0x0204, 6, 8, 0);  // APP_VER "01.08"
    entry(1, 0x0404, 4, 4, 8);  // PARENTAL_LEVEL 9
    entry(2, 0x0204, 10, 16, 12); // TITLE_ID "BLES01807"
    std::memcpy(sfo.data() + key_start, key_table.data(), key_table.size());
    std::memcpy(sfo.data() + data_start, "01.08\0", 6);
    put_le32(sfo, data_start + 8, 9);
    std::memcpy(sfo.data() + data_start + 12, "BLES01807\0", 10);
    return sfo;
}

// A PS3-shaped disc: /PS3_DISC.SFB, /PS3_GAME/PARAM.SFO, /PS3_GAME/USRDIR/{EBOOT.BIN, LIB/GAME.SPRX, DATA.RPF},
// plus BIG.DAT split into two extents (as files over 4 GB are).
struct Ps3Disc {
    std::vector<u8> eboot, sprx, rpf, big_a, big_b, sfo, sfb;
    IsoBuilder iso;

    explicit Ps3Disc(bool disc_encrypted)
    {
        eboot = pattern(9000, 1);
        std::memcpy(eboot.data(), disc_encrypted ? "\x13\x37\xbe\xef" : "SCE\0", 4);
        sprx = pattern(3000, 2);
        std::memcpy(sprx.data(), "SCE\0", 4);
        rpf = pattern(5000, 3);
        big_a = pattern(2 * kSectorSize, 4);
        big_b = pattern(1500, 5);
        sfo = make_sfo();
        sfb = pattern(64, 6);

        // Sector 0: PS3 region table, plain [0, 99], encrypted [100, 199], plain [200, 299].
        u8* s0 = iso.sector(0);
        s0[3] = 2;
        s0[8 + 3] = 0;   // first plain: 0..99
        s0[12 + 3] = 99;
        s0[16 + 3] = 200; // second plain: 200..299
        s0[16 + 2] = 0;
        s0[20 + 2] = 1;   // 299 = 0x12B
        s0[20 + 3] = 0x2B;

        // root (18), PS3_GAME (19), USRDIR (20), LIB (21); files from 30.
        iso.write_records(18, {dir_record(std::string(1, '\0'), 18, kSectorSize, 2),
                                  dir_record(std::string(1, '\1'), 18, kSectorSize, 2),
                                  dir_record("PS3_DISC.SFB;1", 30, static_cast<u32>(sfb.size()), 0),
                                  dir_record("PS3_GAME", 19, kSectorSize, 2)});
        iso.write_records(19, {dir_record(std::string(1, '\0'), 19, kSectorSize, 2),
                                  dir_record(std::string(1, '\1'), 18, kSectorSize, 2),
                                  dir_record("PARAM.SFO;1", 31, static_cast<u32>(sfo.size()), 0),
                                  dir_record("USRDIR", 20, kSectorSize, 2)});
        iso.write_records(20, {dir_record(std::string(1, '\0'), 20, kSectorSize, 2),
                                  dir_record(std::string(1, '\1'), 19, kSectorSize, 2),
                                  dir_record("BIG.DAT;1", 40, static_cast<u32>(big_a.size()), 0x80),
                                  dir_record("BIG.DAT;1", 50, static_cast<u32>(big_b.size()), 0),
                                  dir_record("DATA.RPF;1", 60, static_cast<u32>(rpf.size()), 0),
                                  dir_record("EBOOT.BIN;1", 33, static_cast<u32>(eboot.size()), 0),
                                  dir_record("LIB", 21, kSectorSize, 2)});
        iso.write_records(21, {dir_record(std::string(1, '\0'), 21, kSectorSize, 2),
                                  dir_record(std::string(1, '\1'), 20, kSectorSize, 2),
                                  dir_record("GAME.SPRX;1", 70, static_cast<u32>(sprx.size()), 0)});
        iso.file(30, sfb);
        iso.file(31, sfo);
        iso.file(33, eboot);
        iso.file(40, big_a);
        iso.file(50, big_b);
        iso.file(60, rpf);
        iso.file(70, sprx);
        iso.pvd(18, kSectorSize, "PS3VOLUME");
    }
};

fs::path temp_dir(const std::string& name)
{
    const fs::path dir = fs::temp_directory_path() / ("pspocket-test-" + name + "-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<u8> slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::vector<u8>& bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("ISO 9660: volume, lookup, listing and reads")
{
    Ps3Disc disc(false);
    std::string error;
    auto iso = Iso9660::open(memory_source(disc.iso.image), &error);
    REQUIRE(iso);
    CHECK(iso->volume_id() == "PS3VOLUME");

    const auto eboot = iso->stat("PS3_GAME/USRDIR/EBOOT.BIN");
    REQUIRE(eboot);
    CHECK_FALSE(eboot->directory);
    CHECK(eboot->size == disc.eboot.size());
    CHECK(iso->stat("/ps3_game/usrdir/eboot.bin")); // case-insensitive, leading slash
    CHECK_FALSE(iso->stat("PS3_GAME/USRDIR/MISSING.BIN"));
    CHECK_FALSE(iso->stat("PS3_GAME/PARAM.SFO/X"));

    const auto usrdir = iso->list("PS3_GAME/USRDIR");
    REQUIRE(usrdir);
    std::vector<std::string> names;
    for (const auto& e : *usrdir) names.push_back(e.name);
    CHECK(names == std::vector<std::string>{"BIG.DAT", "DATA.RPF", "EBOOT.BIN", "LIB"});

    const auto bytes = iso->read_all(*eboot, 1u << 20);
    REQUIRE(bytes);
    CHECK(*bytes == disc.eboot);

    u8 part[100];
    REQUIRE(iso->read(*eboot, 5000, part, sizeof(part)));
    CHECK(std::memcmp(part, disc.eboot.data() + 5000, sizeof(part)) == 0);
    CHECK_FALSE(iso->read(*eboot, disc.eboot.size() - 10, part, sizeof(part))); // past the end
}

TEST_CASE("ISO 9660: multi-extent files read across extents")
{
    Ps3Disc disc(false);
    auto iso = Iso9660::open(memory_source(disc.iso.image), nullptr);
    REQUIRE(iso);
    const auto big = iso->stat("PS3_GAME/USRDIR/BIG.DAT");
    REQUIRE(big);
    CHECK(big->extents.size() == 2);
    CHECK(big->size == disc.big_a.size() + disc.big_b.size());

    std::vector<u8> expected = disc.big_a;
    expected.insert(expected.end(), disc.big_b.begin(), disc.big_b.end());
    CHECK(iso->read_all(*big, 1u << 20) == expected);

    u8 straddle[64];
    REQUIRE(iso->read(*big, disc.big_a.size() - 32, straddle, sizeof(straddle)));
    CHECK(std::memcmp(straddle, expected.data() + disc.big_a.size() - 32, sizeof(straddle)) == 0);
}

TEST_CASE("PS3 region table and PARAM.SFO")
{
    Ps3Disc disc(false);
    auto iso = Iso9660::open(memory_source(disc.iso.image), nullptr);
    REQUIRE(iso);
    const auto regions = iso->ps3_regions();
    REQUIRE(regions.size() == 3);
    CHECK((!regions[0].encrypted && regions[0].first_sector == 0 && regions[0].last_sector == 99));
    CHECK((regions[1].encrypted && regions[1].first_sector == 100 && regions[1].last_sector == 199));
    CHECK((!regions[2].encrypted && regions[2].first_sector == 200 && regions[2].last_sector == 299));
    CHECK(iso->in_encrypted_region(150, 1));
    CHECK(iso->in_encrypted_region(90, 20));
    CHECK_FALSE(iso->in_encrypted_region(10, 5));

    const auto sfo = parse_sfo(make_sfo());
    REQUIRE(sfo);
    CHECK(sfo->get("TITLE_ID") == "BLES01807");
    CHECK(sfo->get("APP_VER") == "01.08");
    CHECK(sfo->get("PARENTAL_LEVEL") == "9");
    CHECK(sfo->get("MISSING").empty());
    CHECK_FALSE(parse_sfo(std::vector<u8>{1, 2, 3}));
}

TEST_CASE("Extraction copies executables and metadata, never assets")
{
    Ps3Disc disc(false);
    const fs::path dir = temp_dir("extract");
    write_file(dir / "game.iso", disc.iso.image);

    std::vector<std::string> lines;
    const int rc = ps_disc_extract_executables((dir / "game.iso").c_str(), (dir / "out").c_str(),
        [](void* user, const char* line) { static_cast<std::vector<std::string>*>(user)->push_back(line); }, &lines);
    INFO([&] {
        std::string all;
        for (const auto& l : lines) all += l + "\n";
        return all;
    }());
    REQUIRE(rc == 0);
    CHECK(slurp(dir / "out/PS3_GAME/USRDIR/EBOOT.BIN") == disc.eboot);
    CHECK(slurp(dir / "out/PS3_GAME/USRDIR/LIB/GAME.SPRX") == disc.sprx);
    CHECK(slurp(dir / "out/PS3_GAME/PARAM.SFO") == disc.sfo);
    CHECK(slurp(dir / "out/PS3_DISC.SFB") == disc.sfb);
    CHECK_FALSE(fs::exists(dir / "out/PS3_GAME/USRDIR/DATA.RPF")); // an asset archive
    CHECK_FALSE(fs::exists(dir / "out/PS3_GAME/USRDIR/BIG.DAT"));
    bool saw_title = false;
    for (const auto& l : lines) saw_title = saw_title || l.find("BLES01807") != std::string::npos;
    CHECK(saw_title);
    fs::remove_all(dir);
}

TEST_CASE("A disc-encrypted image is reported, not extracted")
{
    Ps3Disc disc(true);
    const fs::path dir = temp_dir("encrypted");
    write_file(dir / "game.iso", disc.iso.image);
    const int rc = ps_disc_extract_executables((dir / "game.iso").c_str(), (dir / "out").c_str(), nullptr, nullptr);
    CHECK(rc == 2);
    CHECK_FALSE(fs::exists(dir / "out/PS3_GAME/USRDIR/EBOOT.BIN"));
    fs::remove_all(dir);
}

TEST_CASE("Malformed images never crash the reader")
{
    Ps3Disc disc(false);
    std::mt19937 rng(1234);
    for (int round = 0; round < 1500; round++) {
        std::vector<u8> image = disc.iso.image;
        if (round % 3 == 0) {
            image.resize(rng() % image.size());
        } else {
            for (int k = 0; k < 16; k++) image[(16 + rng() % 8) * kSectorSize + rng() % kSectorSize] = static_cast<u8>(rng());
        }
        auto iso = Iso9660::open(memory_source(std::move(image)), nullptr);
        if (!iso) continue;
        if (auto e = iso->stat("PS3_GAME/USRDIR/EBOOT.BIN")) (void)iso->read_all(*e, 1u << 20);
        (void)iso->list("PS3_GAME/USRDIR");
        (void)iso->ps3_regions();
    }
    SUCCEED("no crashes");
}

#if defined(__APPLE__)
TEST_CASE("Reads an image made by hdiutil")
{
    if (!fs::exists("/usr/bin/hdiutil")) SKIP("hdiutil not available");
    const fs::path dir = temp_dir("hdiutil");
    const auto eboot = pattern(70000, 9);
    write_file(dir / "src/PS3_GAME/USRDIR/EBOOT.BIN", eboot);
    write_file(dir / "src/PS3_GAME/PARAM.SFO", make_sfo());
    const std::string cmd = "/usr/bin/hdiutil makehybrid -quiet -iso -default-volume-name PS3VOLUME -o '" +
                            (dir / "made.iso").string() + "' '" + (dir / "src").string() + "' >/dev/null 2>&1";
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::string error;
    auto src = open_file_source((dir / "made.iso").string(), &error);
    REQUIRE(src);
    auto iso = Iso9660::open(std::move(src), &error);
    INFO(error);
    REQUIRE(iso);
    const auto e = iso->stat("PS3_GAME/USRDIR/EBOOT.BIN");
    REQUIRE(e);
    CHECK(iso->read_all(*e, 1u << 20) == eboot);
    fs::remove_all(dir);
}
#endif
