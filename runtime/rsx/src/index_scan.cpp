#include "pspocket/rsx/index_scan.hpp"

#include <algorithm>

#include "pspocket/core/be.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace pspocket::rsx {

namespace {

// Indices [from, to) one at a time.
void scan_range(const u8* p, u32 from, u32 to, u32 size, u32 base, bool restart_on, u32 restart_index, IndexScan& out,
    std::vector<u32>& restarts)
{
    const u32 all_ones = size == 2 ? 0xffffu : 0xffffffffu;
    for (u32 i = from; i < to; i++) {
        const u32 raw = size == 2 ? ps_ld16(p + i * 2) : ps_ld32(p + i * 4);
        out.has_all_ones |= raw == all_ones;
        if (restart_on && raw == restart_index) {
            restarts.push_back(i); // not a vertex: it must not widen the vertex range
            continue;
        }
        const u32 vi = (raw + base) & 0xfffff;
        out.min_vi = std::min(out.min_vi, vi);
        out.max_vi = std::max(out.max_vi, vi);
    }
}

} // namespace

void scan_indices_be_scalar(const u8* indices, u32 count, u32 size, u32 base, bool restart_on, u32 restart_index,
    IndexScan& out, std::vector<u32>& restarts)
{
    scan_range(indices, 0, count, size, base, restart_on, restart_index, out, restarts);
}

void scan_indices_be(const u8* indices, u32 count, u32 size, u32 base, bool restart_on, u32 restart_index,
    IndexScan& out, std::vector<u32>& restarts)
{
#if defined(__aarch64__)
    // Blocks of 32 indices (16 when 32-bit): one look for a restart index in the block, which is then scanned one
    // index at a time, in order, so the positions stay ascending; the vector lanes only see indices that reach a
    // vertex. An index of all ones is the largest there is: the largest index seen, restart or not, tells it.
    u32 i = 0;
    bool any = false; // a block went through the vector lanes
    if (size == 2) {
        // A 16-bit index never equals a restart index above 0xFFFF: none is looked for.
        const bool look = restart_on && restart_index <= 0xffff;
        const uint16x8_t vrestart = vdupq_n_u16(static_cast<u16>(restart_index));
        uint16x8_t top = vdupq_n_u16(0); // the largest index, restart or not
        const auto load = [&](u32 at) { return vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(indices + at * 2))); };
        if (base <= 0xfffff - 0xffff) {
            // index + base stays below 2^20: the smallest and largest vertex are the smallest and largest index's,
            // found in the 16-bit lanes themselves.
            uint16x8_t lo = vdupq_n_u16(0xffff), hi = vdupq_n_u16(0);
            for (; i + 32 <= count; i += 32) {
                const uint16x8_t a = load(i), b = load(i + 8), c = load(i + 16), d = load(i + 24);
                top = vmaxq_u16(top, vmaxq_u16(vmaxq_u16(a, b), vmaxq_u16(c, d)));
                if (look && vmaxvq_u16(vorrq_u16(vorrq_u16(vceqq_u16(a, vrestart), vceqq_u16(b, vrestart)),
                                vorrq_u16(vceqq_u16(c, vrestart), vceqq_u16(d, vrestart))))) {
                    scan_range(indices, i, i + 32, 2, base, true, restart_index, out, restarts);
                    continue;
                }
                lo = vminq_u16(lo, vminq_u16(vminq_u16(a, b), vminq_u16(c, d)));
                hi = vmaxq_u16(hi, vmaxq_u16(vmaxq_u16(a, b), vmaxq_u16(c, d)));
                any = true;
            }
            if (any) {
                out.min_vi = std::min(out.min_vi, u32{vminvq_u16(lo)} + base);
                out.max_vi = std::max(out.max_vi, u32{vmaxvq_u16(hi)} + base);
            }
        } else {
            const uint32x4_t vbase = vdupq_n_u32(base), vmask = vdupq_n_u32(0xfffff);
            uint32x4_t lo = vdupq_n_u32(~0u), hi = vdupq_n_u32(0);
            for (; i + 32 <= count; i += 32) {
                const uint16x8_t a = load(i), b = load(i + 8), c = load(i + 16), d = load(i + 24);
                top = vmaxq_u16(top, vmaxq_u16(vmaxq_u16(a, b), vmaxq_u16(c, d)));
                if (look && vmaxvq_u16(vorrq_u16(vorrq_u16(vceqq_u16(a, vrestart), vceqq_u16(b, vrestart)),
                                vorrq_u16(vceqq_u16(c, vrestart), vceqq_u16(d, vrestart))))) {
                    scan_range(indices, i, i + 32, 2, base, true, restart_index, out, restarts);
                    continue;
                }
                for (const uint16x8_t v : {a, b, c, d}) {
                    const uint32x4_t l = vandq_u32(vaddq_u32(vmovl_u16(vget_low_u16(v)), vbase), vmask);
                    const uint32x4_t h = vandq_u32(vaddq_u32(vmovl_high_u16(v), vbase), vmask);
                    lo = vminq_u32(lo, vminq_u32(l, h));
                    hi = vmaxq_u32(hi, vmaxq_u32(l, h));
                }
                any = true;
            }
            if (any) {
                out.min_vi = std::min(out.min_vi, vminvq_u32(lo));
                out.max_vi = std::max(out.max_vi, vmaxvq_u32(hi));
            }
        }
        out.has_all_ones |= vmaxvq_u16(top) == 0xffff;
    } else {
        const uint32x4_t vrestart = vdupq_n_u32(restart_index), vbase = vdupq_n_u32(base), vmask = vdupq_n_u32(0xfffff);
        uint32x4_t top = vdupq_n_u32(0), lo = vdupq_n_u32(~0u), hi = vdupq_n_u32(0);
        const auto load = [&](u32 at) { return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(indices + at * 4))); };
        for (; i + 16 <= count; i += 16) {
            const uint32x4_t a = load(i), b = load(i + 4), c = load(i + 8), d = load(i + 12);
            top = vmaxq_u32(top, vmaxq_u32(vmaxq_u32(a, b), vmaxq_u32(c, d)));
            if (restart_on && vmaxvq_u32(vorrq_u32(vorrq_u32(vceqq_u32(a, vrestart), vceqq_u32(b, vrestart)),
                                  vorrq_u32(vceqq_u32(c, vrestart), vceqq_u32(d, vrestart))))) {
                scan_range(indices, i, i + 16, 4, base, true, restart_index, out, restarts);
                continue;
            }
            for (const uint32x4_t v : {a, b, c, d}) {
                const uint32x4_t vi = vandq_u32(vaddq_u32(v, vbase), vmask);
                lo = vminq_u32(lo, vi);
                hi = vmaxq_u32(hi, vi);
            }
            any = true;
        }
        if (any) {
            out.min_vi = std::min(out.min_vi, vminvq_u32(lo));
            out.max_vi = std::max(out.max_vi, vmaxvq_u32(hi));
        }
        out.has_all_ones |= vmaxvq_u32(top) == 0xffffffffu;
    }
    scan_range(indices, i, count, size, base, restart_on, restart_index, out, restarts);
#else
    scan_range(indices, 0, count, size, base, restart_on, restart_index, out, restarts);
#endif
}

} // namespace pspocket::rsx
