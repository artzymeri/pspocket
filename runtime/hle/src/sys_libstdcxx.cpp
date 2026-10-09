// sys_libstdcxx: the SDK's C++ library (Dinkumware's, as ppu-lv2-gcc builds it), pspocket's own (D-500). No firmware
// module has it: a game links it into itself, and RPCS3 only names it. Two parts (D-560, D-561):
// - stdcxx_streams.cpp and the files it grows: the object model the rest builds on (stdcxx.hpp), operator new and
//   delete, the exceptions, the strings, the locale and its facet base, ios_base, basic_ios, the stream buffers
//   (strstreambuf, filebuf) and streams.
// - stdcxx_facets.cpp and the files it grows: the facets (ctype, codecvt, collate, num_get/num_put, numpunct,
//   moneypunct, money_get/money_put, time_get/time_put, messages) and their locale helpers.

#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {

void register_stdcxx_streams(Registry& r);
void register_stdcxx_facets(Registry& r);

void register_sys_libstdcxx(Registry& r)
{
    register_stdcxx_streams(r);
    register_stdcxx_facets(r);
}

} // namespace pspocket::hle
