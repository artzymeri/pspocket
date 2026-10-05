#pragma once

// Mach-O pieces route B needs on the phone (D-147): a minimal arm64 dynamic library built from bytes here (stage 1's
// go/no-go test, D-150), and what the signer reads from any Mach-O (its segments and code signature load command).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::codesign {

enum class Platform : u32 { MacOS = 1, IOS = 2 }; // LC_BUILD_VERSION platforms

// An arm64 MH_DYLIB exporting one C function, `symbol` (without the leading underscore), that returns `answer` (at
// most 0xFFFF): `mov w0, #answer; ret`. No linker made it, and it depends on nothing. Its LC_CODE_SIGNATURE points at an
// empty blob at the end of __LINKEDIT, for sign() to fill. min_os is 0xMMMMmmpp (17.0: 0x00110000).
std::vector<u8> answer_dylib(std::string_view symbol, u32 answer, Platform platform, u32 min_os,
    std::string_view install_name);

// What sign() needs to know about a 64-bit Mach-O.
struct MachOLayout {
    u32 code_signature_cmd = 0; // file offset of LC_CODE_SIGNATURE (0: none)
    u32 signature_offset = 0, signature_size = 0;
    u32 linkedit_cmd = 0; // file offset of the __LINKEDIT LC_SEGMENT_64
    u64 linkedit_fileoff = 0;
    u64 text_fileoff = 0, text_filesize = 0; // __TEXT: the exec segment of the CodeDirectory
    u32 filetype = 0;                        // MH_DYLIB, MH_EXECUTE, ...
};
std::optional<MachOLayout> read_layout(const std::vector<u8>& macho, std::string* error = nullptr);

} // namespace pspocket::codesign
