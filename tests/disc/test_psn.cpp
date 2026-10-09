// PSN games through the app's C API (pspocket/disc/psn.h, D-580 to D-583). Made-up licences only.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/disc/psn.h"

using namespace pspocket;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(std::string_view what)
{
    const fs::path root = fs::temp_directory_path() / std::format("pspocket-psn-{}-{}", what, static_cast<long>(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& path, const std::vector<u8>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<u8> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

constexpr const char* kContent = "UP0001-NPXX00000_00-0000000000000001";

} // namespace

TEST_CASE("ps_psn_rap_path: where pspocket looks for a content ID's licence", "[disc][psn]")
{
    char out[512] = {};
    REQUIRE(ps_psn_rap_path("/x/hdd0", kContent, out, sizeof out) == 0);
    CHECK(std::string(out) == "/x/hdd0/home/00000001/exdata/UP0001-NPXX00000_00-0000000000000001.rap");
    CHECK(ps_psn_rap_path("/x/hdd0", "../evil", out, sizeof out) == 1);
    CHECK(ps_psn_rap_path("/x/hdd0", kContent, out, 20) == 1); // does not fit
    CHECK(ps_psn_rap_path(nullptr, kContent, out, sizeof out) == 1);
}

TEST_CASE("ps_psn_install_rap: a player's .rap goes where pspocket looks for it; other files are refused", "[disc][psn]")
{
    const fs::path root = temp_dir("rap");
    const fs::path hdd0 = root / "hdd0", picked = root / "picked";
    std::vector<u8> rap(16);
    for (usize i = 0; i < rap.size(); i++) rap[i] = static_cast<u8>(0x40 + i);
    write_file(picked / (std::string(kContent) + ".rap"), rap);
    char error[256] = {};
    REQUIRE(ps_psn_install_rap(hdd0.c_str(), (picked / (std::string(kContent) + ".rap")).c_str(), error, sizeof error) ==
        0);
    char where[512] = {};
    REQUIRE(ps_psn_rap_path(hdd0.c_str(), kContent, where, sizeof where) == 0);
    CHECK(read_file(where) == rap);

    // Again, with another licence and the extension in capitals: the new one replaces it.
    std::vector<u8> other(16, 0x99);
    write_file(picked / "UP0001-NPXX00000_00-0000000000000001.RAP", other);
    REQUIRE(ps_psn_install_rap(hdd0.c_str(), (picked / "UP0001-NPXX00000_00-0000000000000001.RAP").c_str(), error,
                sizeof error) == 0);
    CHECK(read_file(where) == other);

    const auto refused = [&](const std::string& name, const std::vector<u8>& bytes) {
        write_file(picked / name, bytes);
        std::memset(error, 0, sizeof error);
        const int r = ps_psn_install_rap(hdd0.c_str(), (picked / name).c_str(), error, sizeof error);
        return r == 1 ? std::string(error) : std::string("installed");
    };
    CHECK(refused("licence.bin", rap).find("named <CONTENT_ID>.rap") != std::string::npos);
    CHECK(refused("bad id!.rap", rap).find("is not a content ID") != std::string::npos);
    CHECK(refused(std::string(kContent).substr(0, 20) + ".rap", std::vector<u8>(15, 1)).find("15 bytes") !=
        std::string::npos);
    CHECK(refused(std::string(kContent).substr(0, 21) + ".rap", std::vector<u8>(17, 1)).find("17 bytes") !=
        std::string::npos);
    CHECK(ps_psn_install_rap(hdd0.c_str(), (picked / "missing.rap").c_str(), error, sizeof error) == 1);
    CHECK(std::string(error).find("cannot read") != std::string::npos);
    fs::remove_all(root);
}
