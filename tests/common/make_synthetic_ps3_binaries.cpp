// Writes the synthetic PS3 binaries used by the ps3inspect end-to-end test (tests/common/ps3inspect_e2e.cmake).
// Output stays in the build directory; these are synthetic, but *.elf / *.self are gitignored anyway.
//
//   make_synthetic_ps3_binaries <dir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "synthetic.hpp"

using namespace pspocket;

namespace {

bool write(const std::filesystem::path& path, std::span<const u8> bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

bool write_text(const std::filesystem::path& path, std::string_view text)
{
    return write(path, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: make_synthetic_ps3_binaries <dir>\n");
        return 1;
    }
    const std::filesystem::path dir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", dir.string().c_str());
        return 1;
    }
    const auto exec = test::build_ppu_exec();
    const auto prx = test::build_ppu_prx();
    const auto spu = test::build_spu_elf({"standalone_job", "custom", 0, true});
    bool ok = write(dir / "synthetic_exec.elf", exec.bytes);
    ok = write(dir / "synthetic_prx.prx", prx.bytes) && ok;
    ok = write(dir / "synthetic_spu.elf", spu.bytes) && ok;
    ok = write(dir / "synthetic_retail.self", test::wrap_in_self(exec.bytes)) && ok;
    ok = write_text(dir / "junk.bin", "this is not a PS3 binary\n") && ok;
    ok = write_text(dir / "extra_names.txt", "# names for the synthetic executable's own exports\n"
                                             "[synthLibExport]\nsynthetic_export_func\nsynthetic_export_var\n") && ok;
    return ok ? 0 : 1;
}
