// The HLE module list. Add a module here when its first function is implemented.

#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {

void register_cellAudio(Registry& r);
void register_cellGame(Registry& r);
void register_cellGcmSys(Registry& r);
void register_cellNetCtl(Registry& r);
void register_cellRtc(Registry& r);
void register_cellSheap(Registry& r);
void register_cellSpurs(Registry& r);
void register_cellSysmodule(Registry& r);
void register_cellSysutil(Registry& r);
void register_sceNp(Registry& r);
void register_sysPrxForUser(Registry& r);
void register_sys_fs(Registry& r);
void register_sys_io(Registry& r);
void register_sys_net(Registry& r);

void register_all_modules(Registry& r)
{
    register_cellAudio(r);
    register_cellGame(r);
    register_cellGcmSys(r);
    register_cellNetCtl(r);
    register_cellRtc(r);
    register_cellSheap(r);
    register_cellSpurs(r);
    register_cellSysmodule(r);
    register_cellSysutil(r);
    register_sceNp(r);
    register_sysPrxForUser(r);
    register_sys_fs(r);
    register_sys_io(r);
    register_sys_net(r);
}

} // namespace pspocket::hle
