#pragma once

// Disc images built in code for the disc tests (no game data): a minimal ISO 9660 writer, a PS3-shaped disc and its
// PARAM.SFO, and a disc with a Joliet tree (D-162). Shared by test_disc.cpp and the parser fuzzing (test_fuzz_disc.cpp,
// D-603).

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/disc/iso9660.hpp"

namespace pspocket::disc_test {

using pspocket::disc::kSectorSize;

inline void put_le32(std::vector<u8>& b, usize at, u32 v)
{
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<u8>(v >> (8 * i));
}
inline void put_be32(std::vector<u8>& b, usize at, u32 v)
{
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<u8>(v >> (8 * (3 - i)));
}
inline void put_both32(std::vector<u8>& b, usize at, u32 v)
{
    put_le32(b, at, v);
    put_be32(b, at + 4, v);
}

inline std::vector<u8> dir_record(const std::string& name, u32 lba, u32 size, u8 flags)
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

inline std::vector<u8> pattern(usize size, u32 seed)
{
    std::vector<u8> b(size);
    std::mt19937 rng(seed);
    for (auto& x : b) x = static_cast<u8>(rng());
    return b;
}

inline std::vector<u8> make_sfo()
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

// A disc whose primary tree cuts a long name to 31 characters (as Assassin's Creed's: DATAPS3_FLASHBACK_PARADISE.FORG;1)
// and whose Joliet tree holds it whole, in UCS-2: the file `bytes` at sector 40, the primary tree at 20, the Joliet
// one at 30, the supplementary descriptor at 17 and the terminator at 18.
inline IsoBuilder joliet_disc(const std::string& long_name, const std::vector<u8>& bytes)
{
    IsoBuilder b;
    b.file(40, bytes);
    // Primary tree at 20: the cut name.
    b.write_records(20, {dir_record(std::string(1, '\0'), 20, kSectorSize, 0x02),
                            dir_record(std::string(1, '\1'), 20, kSectorSize, 0x02),
                            dir_record("DATAPS3_FLASHBACK_PARADISE.FORG;1", 40, static_cast<u32>(bytes.size()), 0)});
    // Joliet tree at 30: the whole name, UCS-2 big-endian.
    std::string ucs2;
    for (const char c : long_name + ";1") ucs2 += std::string{'\0', c};
    b.write_records(30, {dir_record(std::string(1, '\0'), 30, kSectorSize, 0x02),
                            dir_record(std::string(1, '\1'), 30, kSectorSize, 0x02),
                            dir_record(ucs2, 40, static_cast<u32>(bytes.size()), 0)});
    b.pvd(20, kSectorSize, "JOLIET");
    // The terminator pvd() wrote at 17 moves to 18; the supplementary descriptor takes 17.
    std::memcpy(b.sector(18), b.sector(17), kSectorSize);
    u8* svd = b.sector(17);
    std::memcpy(svd, b.sector(16), kSectorSize);
    svd[0] = 2;
    svd[88] = '%';
    svd[89] = '/';
    svd[90] = 'E';
    const auto root = dir_record(std::string(1, '\0'), 30, kSectorSize, 0x02);
    std::memcpy(svd + 156, root.data(), 34);
    return b;
}

} // namespace pspocket::disc_test
