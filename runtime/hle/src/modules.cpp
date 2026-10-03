// The HLE module list. Add a module here when its first function is implemented.

#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {

void register_cellNetCtl(Registry& r);
void register_cellSheap(Registry& r);
void register_cellSpurs(Registry& r);
void register_cellSysmodule(Registry& r);
void register_sceNp(Registry& r);
void register_sysPrxForUser(Registry& r);
void register_sys_net(Registry& r);

void register_all_modules(Registry& r)
{
    register_cellNetCtl(r);
    register_cellSheap(r);
    register_cellSpurs(r);
    register_cellSysmodule(r);
    register_sceNp(r);
    register_sysPrxForUser(r);
    register_sys_net(r);
}

} // namespace pspocket::hle
