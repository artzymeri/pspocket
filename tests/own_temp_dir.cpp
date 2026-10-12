// Every test process works in a temporary directory of its own, TMPDIR/pspocket-test-<pid>, removed when it ends. Test
// runs at the same time (the gate beside library agents' runs, D-924) shared the fixed names tests make under the
// temporary directory and removed each other's files; std::filesystem::temp_directory_path() now answers this one.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

namespace {

struct OwnTempDir {
    std::filesystem::path dir;

    OwnTempDir()
    {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) return;
        dir = base / ("pspocket-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            dir.clear();
            return;
        }
        ::setenv("TMPDIR", dir.c_str(), 1);
    }
    ~OwnTempDir()
    {
        std::error_code ec;
        if (!dir.empty()) std::filesystem::remove_all(dir, ec);
    }
    OwnTempDir(const OwnTempDir&) = delete;
    OwnTempDir& operator=(const OwnTempDir&) = delete;
};

const OwnTempDir own_temp_dir;

} // namespace
