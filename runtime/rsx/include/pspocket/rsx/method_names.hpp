#pragma once

// Names of RSX methods for logs and disassembly ("NV4097_SET_TEXTURE_OFFSET[3]"), and the set of methods the
// front end knows. A method the front end does not know is "unhandled": it is logged with its name and number and,
// in dev builds, aborts (CLAUDE.md rule 5). The known set is RPCS3's method table (rsx_methods.cpp, s_methods_init)
// minus its custom FIFO-internal entries.

#include <string>

#include "pspocket/core/types.hpp"

namespace pspocket::rsx {

// "NV4097_SET_BLEND_ENABLE", "NV4097_SET_TEXTURE_FORMAT[2]", "NV308A_COLOR[17]", or "unknown" for methods with no
// name. reg is the register index (method byte address >> 2).
std::string method_name(u32 reg);

// Formatted for failure messages: "NV4097_SET_BLEND_ENABLE (0x0310)".
std::string method_label(u32 reg);

// True if the method is part of the RSX method set the front end tracks (state register or handled method).
bool is_known_method(u32 reg);

} // namespace pspocket::rsx
