// The HLE module list. Add a module here when its first function is implemented.

#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {

void register_cellAudio(Registry& r);
void register_cellGame(Registry& r);
void register_cellGcmSys(Registry& r);
void register_cellHttp(Registry& r);
void register_cellNetCtl(Registry& r);
void register_cellOskDialog(Registry& r);
void register_cellResc(Registry& r);
void register_cellRtc(Registry& r);
void register_cellSaveData(Registry& r);
void register_cellSheap(Registry& r);
void register_cellSpurs(Registry& r);
void register_cellSpursQueue(Registry& r);
void register_cellSpursSemaphore(Registry& r);
void register_cellSsl(Registry& r);
void register_cellSync(Registry& r);
void register_sys_prx(Registry& r);
void register_sys_spu(Registry& r);
void register_cellSysmodule(Registry& r);
void register_cellSysutil(Registry& r);
void register_cellVoice(Registry& r);
void register_sceNp(Registry& r);
void register_sceNp2(Registry& r);
void register_sceNpCommerce2(Registry& r);
void register_sceNpSns(Registry& r);
void register_sceNpTus(Registry& r);
void register_sceNpClans(Registry& r);
void register_sceNpUtil(Registry& r);
void register_cellSearch(Registry& r);
void register_cellMusic(Registry& r);
void register_media_export(Registry& r);
void register_cellSysutilAvc2(Registry& r);
void register_cellImeJp(Registry& r);
void register_cellRec(Registry& r);
void register_cellPamf(Registry& r);
void register_cellDmux(Registry& r);
void register_sceNpTrophy(Registry& r);
void register_sysPrxForUser(Registry& r);
void register_sysPrxForUser_libc(Registry& r);
void register_sys_fs(Registry& r);
void register_sys_io(Registry& r);
void register_sys_net(Registry& r);
void register_sysutil_misc(Registry& r);
void register_devices(Registry& r);
void register_gem(Registry& r);

void register_all_modules(Registry& r)
{
    register_cellAudio(r);
    register_cellGame(r);
    register_cellGcmSys(r);
    register_cellHttp(r);
    register_cellNetCtl(r);
    register_cellOskDialog(r);
    register_cellResc(r);
    register_cellRtc(r);
    register_cellSaveData(r);
    register_cellSheap(r);
    register_cellSpurs(r);
    register_cellSpursQueue(r);
    register_cellSpursSemaphore(r);
    register_cellSsl(r);
    register_cellSync(r);
    register_sys_prx(r);
    register_sys_spu(r);
    register_cellSysmodule(r);
    register_cellSysutil(r);
    register_cellVoice(r);
    register_sceNp(r);
    register_sceNp2(r);
    register_sceNpCommerce2(r);
    register_sceNpSns(r);
    register_sceNpTus(r);
    register_sceNpClans(r);
    register_sceNpUtil(r);
    register_cellSearch(r);
    register_cellMusic(r);
    register_media_export(r);
    register_cellSysutilAvc2(r);
    register_cellImeJp(r);
    register_cellRec(r);
    register_cellPamf(r);
    register_cellDmux(r);
    register_sceNpTrophy(r);
    register_sysPrxForUser(r);
    register_sysPrxForUser_libc(r);
    register_sys_fs(r);
    register_sys_io(r);
    register_sys_net(r);
    register_sysutil_misc(r);
    register_devices(r);
    register_gem(r);
}

} // namespace pspocket::hle
