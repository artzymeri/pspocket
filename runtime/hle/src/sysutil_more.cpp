// libsysutil's sub-libraries made pspocket's own (D-925): each file's registration (sysutil_sub.hpp).

#include "pspocket/hle/hle.hpp"
#include "sysutil_sub.hpp"

namespace pspocket::hle {

void register_sysutil_more(Registry& r) { sysutil::sub::register_print(r); }

} // namespace pspocket::hle
