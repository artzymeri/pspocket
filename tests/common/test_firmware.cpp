// Installing /dev_flash from a PS3 update (D-156): a synthetic PUP whose update archive holds a dev_flash package, an
// SCE package encrypted here with made-up keys whose third section is a zlib'd ustar archive of dev_flash files, is
// installed and every file lands where it belongs. No real keys, no firmware (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/firmware.hpp"
#include "pspocket/recomp/self_decrypt.hpp"

#if defined(__APPLE__) && __has_include(<zlib.h>)
#include <CommonCrypto/CommonCryptor.h>
#include <zlib.h>

using namespace pspocket;
using namespace pspocket::recomp;

namespace {

namespace fs = std::filesystem;

template <usize N>
std::array<u8, N> made_up(u8 seed)
{
    std::array<u8, N> a{};
    for (usize i = 0; i < N; i++) a[i] = static_cast<u8>(seed + i * 41);
    return a;
}

void aes128_ctr(const u8* key, const u8* iv, u8* data, usize size)
{
    CCCryptorRef c = nullptr;
    REQUIRE(CCCryptorCreateWithMode(kCCEncrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding, iv, key, 16, nullptr, 0, 0,
                kCCModeOptionCTR_BE, &c) == kCCSuccess);
    usize moved = 0;
    REQUIRE(CCCryptorUpdate(c, data, size, data, size, &moved) == kCCSuccess);
    CCCryptorRelease(c);
}

// A ustar archive of (name, type, contents).
std::vector<u8> ustar(const std::vector<std::tuple<std::string, char, std::string>>& entries)
{
    std::vector<u8> out;
    for (const auto& [name, type, data] : entries) {
        std::array<u8, 512> h{};
        std::memcpy(h.data(), name.data(), std::min<usize>(name.size(), 100));
        std::memcpy(h.data() + 100, "0000644", 7);
        const std::string size = std::format("{:011o}", data.size());
        std::memcpy(h.data() + 124, size.data(), 11);
        std::memcpy(h.data() + 136, "00000000000", 11);
        h[156] = static_cast<u8>(type);
        std::memcpy(h.data() + 257, "ustar", 6);
        std::memcpy(h.data() + 263, "00", 2);
        std::memset(h.data() + 148, ' ', 8);
        u32 sum = 0;
        for (const u8 b : h) sum += b;
        const std::string chk = std::format("{:06o}", sum);
        std::memcpy(h.data() + 148, chk.data(), 6);
        h[154] = 0;
        out.insert(out.end(), h.begin(), h.end());
        out.insert(out.end(), data.begin(), data.end());
        out.resize((out.size() + 511) / 512 * 512, 0);
    }
    out.resize(out.size() + 1024, 0);
    return out;
}

// An SCE package of three sections, the third zlib'd, each encrypted with its own key and counter; the metadata info
// encrypted with `key`.
std::vector<u8> sce_package(const PackageKey& key, const std::vector<u8>& third)
{
    std::vector<std::vector<u8>> sections = {std::vector<u8>(64, 0x11), std::vector<u8>(32, 0x22)};
    std::vector<u8> z(compressBound(static_cast<uLong>(third.size())));
    uLongf zsize = static_cast<uLongf>(z.size());
    REQUIRE(compress2(z.data(), &zsize, third.data(), static_cast<uLong>(third.size()), 9) == Z_OK);
    z.resize(zsize);
    sections.push_back(z);
    const u32 n = 3;
    const usize meta = 0x20; // metadata offset 0 -> the info right after the SCE header
    const usize head = meta + 0x40;
    const usize header_size = (head + 0x20 + n * 0x30 + 2 * n * 16 + 0xF) & ~usize{0xF};
    std::vector<u8> s(header_size, 0);
    ps_st32(s.data(), 0x53434500u);
    ps_st32(s.data() + 4, 2);
    ps_st16(s.data() + 8, 0);
    ps_st16(s.data() + 0x0A, 3); // a package
    ps_st32(s.data() + 0x0C, 0);
    ps_st64(s.data() + 0x10, header_size);
    const auto meta_key = made_up<16>(0x31), meta_iv = made_up<16>(0x47);
    std::copy(meta_key.begin(), meta_key.end(), s.begin() + static_cast<std::ptrdiff_t>(meta));
    std::copy(meta_iv.begin(), meta_iv.end(), s.begin() + static_cast<std::ptrdiff_t>(meta + 0x20));
    usize moved = 0;
    REQUIRE(CCCrypt(kCCEncrypt, kCCAlgorithmAES, 0, key.erk.data(), 32, key.riv.data(), s.data() + meta, 0x40,
                s.data() + meta, 0x40, &moved) == kCCSuccess);
    ps_st64(s.data() + head, header_size);
    ps_st32(s.data() + head + 0x0C, n);
    ps_st32(s.data() + head + 0x10, 2 * n);
    for (u32 k = 0; k < n; k++) {
        std::vector<u8> data = sections[k];
        const auto skey = made_up<16>(static_cast<u8>(0x60 + k)), siv = made_up<16>(static_cast<u8>(0xA0 + k));
        aes128_ctr(skey.data(), siv.data(), data.data(), data.size());
        const usize at = (s.size() + 0xF) & ~usize{0xF};
        s.resize(at);
        s.insert(s.end(), data.begin(), data.end());
        const usize sh = head + 0x20 + k * 0x30;
        ps_st64(s.data() + sh, at);
        ps_st64(s.data() + sh + 0x08, data.size());
        ps_st32(s.data() + sh + 0x20, 3);
        ps_st32(s.data() + sh + 0x24, 2 * k);
        ps_st32(s.data() + sh + 0x28, 2 * k + 1);
        ps_st32(s.data() + sh + 0x2C, k == 2 ? 2 : 1);
        const usize keys = head + 0x20 + n * 0x30 + 2 * k * 16;
        std::copy(skey.begin(), skey.end(), s.begin() + static_cast<std::ptrdiff_t>(keys));
        std::copy(siv.begin(), siv.end(), s.begin() + static_cast<std::ptrdiff_t>(keys + 16));
    }
    aes128_ctr(meta_key.data(), meta_iv.data(), s.data() + head, header_size - head);
    return s;
}

// A PUP: the header, two file entries (0x100 version.txt, 0x300 the update archive), the files.
std::vector<u8> pup(const std::string& version, const std::vector<u8>& update)
{
    const u64 header = 0x30 + 2 * 0x20 + 2 * 0x20;
    std::vector<u8> p(header, 0);
    std::memcpy(p.data(), "SCEUF\0\0\0", 8);
    ps_st64(p.data() + 0x08, 1);
    ps_st64(p.data() + 0x10, 99999);
    ps_st64(p.data() + 0x18, 2);
    ps_st64(p.data() + 0x20, header);
    const u64 v_at = header, u_at = header + version.size();
    ps_st64(p.data() + 0x28, version.size() + update.size());
    ps_st64(p.data() + 0x30, 0x100);
    ps_st64(p.data() + 0x38, v_at);
    ps_st64(p.data() + 0x40, version.size());
    ps_st64(p.data() + 0x50, 0x300);
    ps_st64(p.data() + 0x58, u_at);
    ps_st64(p.data() + 0x60, update.size());
    p.insert(p.end(), version.begin(), version.end());
    p.insert(p.end(), update.begin(), update.end());
    return p;
}

std::string read(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("firmware: /dev_flash installs from a PUP's dev_flash packages (D-156)")
{
    PackageKey key;
    key.erk = made_up<32>(0x05);
    key.riv = made_up<16>(0x17);
    key.present = true;
    const std::vector<u8> files = ustar({{"dev_flash/sys", '5', ""},
        {"dev_flash/sys/hello.txt", '0', "hello from a synthetic update"},
        {"dev_flash/vsh/data.bin", '0', std::string(2000, 'x')},
        {"not_dev_flash/skip.txt", '0', "outside /dev_flash"}});
    const std::vector<u8> package = sce_package(key, files);
    const std::vector<u8> update = ustar({{"dev_flash_000.tar.aa.2026_01_01_000000",
        '0', std::string(package.begin(), package.end())}, {"other.pkg", '0', "not a dev_flash package"}});
    const std::vector<u8> image = pup("9.99\nmore\n", update);

    const fs::path dir = fs::temp_directory_path() / std::format("pspocket-firmware-{}", getpid());
    fs::create_directories(dir);
    std::ofstream(dir / "PS3UPDAT.PUP", std::ios::binary)
        .write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    int calls = 0;
    const auto r = install_firmware((dir / "PS3UPDAT.PUP").string(), (dir / "dev_flash").string(),
        [&](double, const std::string&) { calls++; }, &key);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->version == "9.99");
    CHECK(r->packages == 1);
    CHECK(r->files == 2);
    CHECK(calls >= 2);
    CHECK(read(dir / "dev_flash/sys/hello.txt") == "hello from a synthetic update");
    CHECK(read(dir / "dev_flash/vsh/data.bin") == std::string(2000, 'x'));
    CHECK_FALSE(fs::exists(dir / "dev_flash/skip.txt"));
    CHECK_FALSE(fs::exists(dir / "dev_flash/not_dev_flash"));

    // Another key does not open it; a damaged PUP says so.
    PackageKey other = key;
    other.erk[0] ^= 1;
    CHECK_FALSE(install_firmware((dir / "PS3UPDAT.PUP").string(), (dir / "other").string(), {}, &other));
    std::vector<u8> cut(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(image.size() / 2));
    std::ofstream(dir / "cut.PUP", std::ios::binary)
        .write(reinterpret_cast<const char*>(cut.data()), static_cast<std::streamsize>(cut.size()));
    const auto damaged = install_firmware((dir / "cut.PUP").string(), (dir / "cut").string(), {}, &key);
    REQUIRE_FALSE(damaged);
    CHECK(damaged.error().message.find("incomplete") != std::string::npos);
    CHECK_FALSE(read_pup(std::vector<u8>(64, 0)));
    CHECK_FALSE(read_tar(std::vector<u8>(1024, 1)));
    fs::remove_all(dir);
}
#endif
