#pragma once

// Writes RSX command words (big-endian) into guest memory, the way libgcm's inline command macros do. Used by the
// tests to build synthetic command buffers and available to an HLE cellGcmSys that has to emit commands itself
// (RPCS3 cellGcmSys.cpp rsx::make_command / make_jump).

#include <initializer_list>
#include <span>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"
#include "pspocket/rsx/gcm.hpp"

namespace pspocket::rsx {

class CommandBuilder {
public:
    // base: guest memory base. addr: guest EA where the next word goes. offset: the FIFO offset (IO or local
    // offset) that corresponds to addr, so jumps and calls can be expressed relative to the current position.
    CommandBuilder(u8* base, u32 addr, u32 offset) : base_(base), addr_(addr), offset_(offset) {}

    u32 addr() const { return addr_; }
    u32 offset() const { return offset_; }

    CommandBuilder& word(u32 v)
    {
        ps_write32(base_, addr_, v);
        addr_ += 4;
        offset_ += 4;
        return *this;
    }

    // One method with its arguments (incrementing): reg, reg+1, ...
    CommandBuilder& method(u32 reg, std::initializer_list<u32> args) { return method(reg, std::span(args.begin(), args.size())); }
    CommandBuilder& method(u32 reg, std::span<const u32> args)
    {
        word(cmd::method_header(reg, static_cast<u32>(args.size())));
        for (u32 a : args) word(a);
        return *this;
    }
    CommandBuilder& method(u32 reg, u32 arg) { return method(reg, {arg}); }

    // All arguments go to the same register.
    CommandBuilder& method_ni(u32 reg, std::initializer_list<u32> args)
    {
        word(cmd::method_header(reg, static_cast<u32>(args.size()), true));
        for (u32 a : args) word(a);
        return *this;
    }

    CommandBuilder& nop() { return word(0); } // method header with count 0
    CommandBuilder& jump(u32 target_offset) { return word(cmd::old_jump(target_offset)); }
    CommandBuilder& jump_new(u32 target_offset) { return word(cmd::new_jump(target_offset)); }
    CommandBuilder& call(u32 target_offset) { return word(cmd::call(target_offset)); }
    CommandBuilder& ret() { return word(cmd::ret()); }

    CommandBuilder& f32(u32 reg, float v)
    {
        u32 u;
        __builtin_memcpy(&u, &v, 4);
        return method(reg, u);
    }

private:
    u8* base_;
    u32 addr_;
    u32 offset_;
};

inline u32 f2u(float v)
{
    u32 u;
    __builtin_memcpy(&u, &v, 4);
    return u;
}

inline float u2f(u32 u)
{
    float v;
    __builtin_memcpy(&v, &u, 4);
    return v;
}

} // namespace pspocket::rsx
