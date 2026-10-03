// The HLE module list. Add a module here when its first function is implemented.

#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {

void register_cellSysmodule(Registry& r);
void register_sysPrxForUser(Registry& r);

void register_all_modules(Registry& r)
{
    register_cellSysmodule(r);
    register_sysPrxForUser(r);
}

} // namespace pspocket::hle
