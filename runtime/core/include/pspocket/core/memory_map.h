/*
 * PS3 guest address map (32-bit effective addresses). Don't guess addresses: these come from RPCS3,
 * rpcs3/Emu/Memory/vm.cpp (vm::init, vm::get), Cell/PPUModule.cpp and Cell/lv2/sys_mmapper.cpp.
 *
 * user64k, user1m and rsx_context are allocated on demand in RPCS3 at the first free 256 MB-aligned slot
 * (vm.cpp _find_map); lv2 decides their placement when it lands (Phase 2).
 */
#ifndef PSPOCKET_CORE_MEMORY_MAP_H
#define PSPOCKET_CORE_MEMORY_MAP_H

#define PS_GUEST_SPACE_SIZE 0x100000000ull /* 4 GB */
#define PS_GUEST_PAGE_SIZE 0x1000u         /* smallest PS3 page; host commits at host-page granularity */

#define PS_MEM_MAIN_BASE 0x00010000u /* ELF image + main heap (vm::main) */
#define PS_MEM_MAIN_END 0x10000000u
#define PS_MEM_PRX_BASE 0x30000000u /* PRX and overlay modules (PPUModule.cpp) */
#define PS_MEM_PRX_SIZE 0x10000000u
#define PS_MEM_MMAPPER_BASE 0xB0000000u /* sys_mmapper */
#define PS_MEM_MMAPPER_SIZE 0x10000000u
#define PS_MEM_RSX_LOCAL_BASE 0xC0000000u /* RSX local memory ("video"), 256 MB */
#define PS_MEM_RSX_LOCAL_SIZE 0x10000000u
#define PS_MEM_STACK_BASE 0xD0000000u /* PPU thread stacks, guarded */
#define PS_MEM_STACK_SIZE 0x10000000u
#define PS_MEM_SPU_BASE 0xE0000000u /* raw SPU MMIO, SPU thread local-store mirrors */
#define PS_MEM_SPU_SIZE 0x20000000u

#endif /* PSPOCKET_CORE_MEMORY_MAP_H */
