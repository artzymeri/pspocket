// SPU programs built in tests (no game data): a program at LS 0 with data after it, written as a 32-bit big-endian SPU
// ELF, and an SPU thread group of one thread running it (the libc tests, D-516 and D-904).
#pragma once

#include <cstring>
#include <string_view>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/spu/asm.hpp"

namespace pspocket::test {

// An SPU program at LS 0 with data after it, as a 32-bit big-endian SPU ELF (one PT_LOAD at LS 0, a PT_NOTE).
struct SpuProgram {
    std::vector<u32> words;
    void emit(u32 insn) { words.push_back(insn); }
    void load32(u32 rt, u32 v)
    {
        namespace as = pspocket::recomp::spu::as;
        emit(as::ilhu(rt, static_cast<s32>(v >> 16)));
        emit(as::iohl(rt, static_cast<s32>(v & 0xFFFF)));
    }
    // Bytes at LS `lsa` (the program grows to reach it).
    void data(u32 lsa, const std::vector<u8>& bytes)
    {
        const usize need = (lsa + bytes.size() + 3) / 4;
        if (words.size() < need) words.resize(need, 0);
        for (usize i = 0; i < bytes.size(); i++) {
            u32& w = words[(lsa + i) / 4];
            const u32 shift = 8 * (3 - ((lsa + static_cast<u32>(i)) & 3));
            w = (w & ~(0xFFu << shift)) | (u32{bytes[i]} << shift);
        }
    }
    u32 elf_size() const { return 0x100 + static_cast<u32>(words.size() * 4); }
    void write_elf(u8* b, u32 ea) const
    {
        const u32 size = static_cast<u32>(words.size() * 4);
        std::memset(b + ea, 0, 0x200 + size);
        static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
        std::memcpy(b + ea, kIdent, 16);
        ps_write16(b, ea + 0x10, 2);
        ps_write16(b, ea + 0x12, 0x17);
        ps_write32(b, ea + 0x14, 1);
        ps_write32(b, ea + 0x18, 0);
        ps_write32(b, ea + 0x1C, 0x34);
        ps_write16(b, ea + 0x28, 0x34);
        ps_write16(b, ea + 0x2A, 0x20);
        ps_write16(b, ea + 0x2C, 2);
        const u32 ph = ea + 0x34;
        ps_write32(b, ph + 0x00, 1);
        ps_write32(b, ph + 0x04, 0x100);
        ps_write32(b, ph + 0x10, size);
        ps_write32(b, ph + 0x14, size + 0x100);
        ps_write32(b, ph + 0x18, 7);
        ps_write32(b, ph + 0x20, 4);
        ps_write32(b, ph + 0x24, 0x80);
        ps_write32(b, ph + 0x30, 0x20);
        for (usize i = 0; i < words.size(); i++) ps_write32(b, ea + 0x100 + 4 * static_cast<u32>(i), words[i]);
    }
};

inline std::vector<u8> bytes_of(std::string_view s)
{
    std::vector<u8> v(s.begin(), s.end());
    v.push_back(0);
    return v;
}

inline std::vector<u8> quad32(u32 v)
{
    return {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}

inline std::vector<u8> quad64(u64 v)
{
    std::vector<u8> q(16, 0);
    for (u32 i = 0; i < 8; i++) q[i] = static_cast<u8>(v >> (56 - 8 * i));
    return q;
}

// A group of one SPU thread running `p` (not started): the ELF and the call's structures in the 0x2000 bytes of guest
// scratch memory at `scratch` (which must be committed, with room for the ELF past +0x400).
struct SpuGroup {
    u32 group = 0, thread = 0;
};

inline SpuGroup make_spu_group(lv2::PpuThread& t, u8* b, u32 scratch, const SpuProgram& p)
{
    const u32 img = scratch, group = scratch + 0x100, thread = scratch + 0x108, attr = scratch + 0x200,
              thread_attr = scratch + 0x240, thread_args = scratch + 0x280, name = scratch + 0x300,
              elf = scratch + 0x400;
    p.write_elf(b, elf);
    SpuGroup g;
    if (lv2::_sys_spu_image_import(t, img, elf, p.elf_size(), 0) != lv2::CELL_OK) return g;
    std::memcpy(b + name, "test", 5);
    ps_write32(b, attr, 5);
    ps_write32(b, attr + 4, name);
    ps_write32(b, attr + 8, 0);
    if (lv2::sys_spu_thread_group_create(t, group, 1, 100, attr) != lv2::CELL_OK) return g;
    g.group = ps_read32(b, group);
    ps_write32(b, thread_attr, name);
    ps_write32(b, thread_attr + 4, 5);
    ps_write32(b, thread_attr + 8, 0);
    std::memset(b + thread_args, 0, 32);
    if (lv2::sys_spu_thread_initialize(t, thread, g.group, 0, img, thread_attr, thread_args) != lv2::CELL_OK) return g;
    g.thread = ps_read32(b, thread);
    return g;
}

} // namespace pspocket::test
