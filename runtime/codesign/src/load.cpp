#include "pspocket/codesign/load.hpp"

#if defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "pspocket/codesign/macho.hpp"
#endif

namespace pspocket::codesign {

bool register_signature(const std::string& path, std::string* steps)
{
#if defined(__APPLE__)
    std::string s;
    bool ok = false;
    std::ifstream in(path, std::ios::binary);
    const std::vector<u8> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto layout = read_layout(bytes);
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        s = std::string("cannot open: ") + std::strerror(errno);
    } else if (!layout || layout->signature_size == 0) {
        s = "no signature";
    } else {
        // The signature's place in the file, as dyld passes it (LiveContainer's checkCodeSignature does the same).
        fsignatures_t sigs{};
        sigs.fs_file_start = 0;
        sigs.fs_blob_start = reinterpret_cast<void*>(static_cast<uintptr_t>(layout->signature_offset));
        sigs.fs_blob_size = layout->signature_size;
        ok = fcntl(fd, F_ADDFILESIGS_RETURN, &sigs) != -1;
        s = ok ? "signature registered" : std::string("F_ADDFILESIGS_RETURN refused (") + std::strerror(errno) + ")";
#if defined(F_CHECK_LV)
        fchecklv_t lv{};
        char message[1024] = {};
        lv.lv_file_start = 0;
        lv.lv_error_message_size = sizeof message;
        lv.lv_error_message = message;
        if (fcntl(fd, F_CHECK_LV, &lv) == -1) {
            ok = false;
            s += std::string(", library validation refused: ") + (message[0] ? message : std::strerror(errno));
        } else {
            s += ", library validation passed";
        }
#endif
    }
    if (fd >= 0) close(fd);
    if (steps) *steps += s;
    return ok;
#else
    (void)path;
    (void)steps;
    return true;
#endif
}

} // namespace pspocket::codesign
