#pragma once

// Loading code signed after the app was installed (route B, D-150): what dyld would do, done first so a refusal is
// reported step by step, as LiveContainer does.

#include <string>

namespace pspocket::codesign {

// Registers the signature of the Mach-O at `path` with the kernel (F_ADDFILESIGS_RETURN, at the signature's place in
// the file) and asks for library validation (F_CHECK_LV). Appends what happened to *steps ("signature registered,
// library validation passed", or the refusal and errno). Returns false on a refusal; dlopen then fails too, with
// less to say. Use a fresh file: the kernel keeps the first signature it saw of a file. No-op off Apple platforms.
bool register_signature(const std::string& path, std::string* steps = nullptr);

} // namespace pspocket::codesign
