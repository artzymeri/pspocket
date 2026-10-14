#pragma once

// The vertex program and constant upload methods, inline: the FIFO reader's argument runs call them straight for the
// third of GTA V's command words they are (D-955); methods.cpp has the other handlers.

#include "pspocket/core/log.hpp"
#include "pspocket/rsx/rsx.hpp"

namespace pspocket::rsx {

[[gnu::always_inline]] inline void Rsx::m_transform_program(u32 reg, u32 value)
{
    // RPCS3 nv4097.cpp set_transform_program: words land at load * 4 + (index % 4); the load pointer advances by
    // one instruction after the fourth word.
    const u32 index = reg - m::NV4097_SET_TRANSFORM_PROGRAM;
    const u32 load = state_.regs[m::NV4097_SET_TRANSFORM_PROGRAM_LOAD];
    const u32 slot = load * 4 + (index % 4);
    if (slot < state_.transform_program.size()) [[likely]] {
        state_.transform_program[slot] = value;
    } else {
        program_upload_past_end(reg);
    }
    if (index % 4 == 3) state_.regs[m::NV4097_SET_TRANSFORM_PROGRAM_LOAD] = load + 1;
    state_.dirty_bits |= dirty::kVertexProgram;
}

[[gnu::always_inline]] inline void Rsx::m_transform_constant(u32 reg, u32 value)
{
    // RPCS3 nv4097.cpp set_transform_constant: index / 4 is the constant, index % 4 the component; writes past the
    // usable [0, 467] range are ignored.
    const u32 index = reg - m::NV4097_SET_TRANSFORM_CONSTANT;
    const u32 slot = state_.regs[m::NV4097_SET_TRANSFORM_CONSTANT_LOAD] + index / 4;
    if (slot < kTransformConstantCount) {
        state_.transform_constants[slot][index % 4] = bits_to_float(value);
        state_.dirty_bits |= dirty::kVertexConstants;
    } else [[unlikely]] {
        PS_LOG_TRACE("rsx", "transform constant {} out of range, ignored", slot);
    }
}

} // namespace pspocket::rsx
