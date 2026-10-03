#pragma once

// RSX / libgcm constants: method numbers, command-header encoding, DMA context handles, formats.
//
// Values come from RPCS3, rpcs3/Emu/RSX/gcm_enums.h (CellGcmMethod, Method, CellGcmLocation and the format enums),
// which in turn mirror Sony's cell/gcm headers. Method constants are register indices, i.e. the method's byte
// address in the 64 KB method space shifted right by 2, exactly as RPCS3 stores them.

#include "pspocket/core/types.hpp"

namespace pspocket::rsx {

// ---- command header encoding (RSXFIFO.cpp, gcm_enums.h `Method`) -----------------------------------------------

namespace cmd {
inline constexpr u32 kOldJumpMask = 0xe0000003;
inline constexpr u32 kOldJump = 0x20000000;
inline constexpr u32 kOldJumpOffsetMask = 0x1ffffffc;
inline constexpr u32 kNewJumpMask = 0xe0000003;
inline constexpr u32 kNewJump = 0x00000001;
inline constexpr u32 kNewJumpOffsetMask = 0xfffffffc;
inline constexpr u32 kCallMask = 0x00000003;
inline constexpr u32 kCall = 0x00000002;
inline constexpr u32 kCallOffsetMask = 0x1ffffffc;
inline constexpr u32 kReturnMask = 0xffff0003;
inline constexpr u32 kReturn = 0x00020000;
inline constexpr u32 kNonMethodMask = 0xa0030003; // any of these bits set: not a method header
inline constexpr u32 kNonIncrementMask = 0xe0030003;
inline constexpr u32 kNonIncrement = 0x40000000;
inline constexpr u32 kCountShift = 18;
inline constexpr u32 kCountMask = 0x7ff;
inline constexpr u32 kMethodMask = 0x0000fffc;

constexpr u32 method_header(u32 reg, u32 count, bool non_increment = false)
{
    return (reg << 2) | (count << kCountShift) | (non_increment ? kNonIncrement : 0u);
}
constexpr u32 old_jump(u32 offset) { return kOldJump | (offset & kOldJumpOffsetMask); }
constexpr u32 new_jump(u32 offset) { return kNewJump | (offset & kNewJumpOffsetMask); }
constexpr u32 call(u32 offset) { return kCall | (offset & kCallOffsetMask); }
constexpr u32 ret() { return kReturn; }
} // namespace cmd

// ---- DMA context handles and locations (CellGcmLocation) -------------------------------------------------------

namespace loc {
inline constexpr u32 kLocal = 0;
inline constexpr u32 kMain = 1;
inline constexpr u32 kDmaFrameBuffer = 0xFEED0000; // local memory
inline constexpr u32 kDmaHostBuffer = 0xFEED0001;  // main memory (IO mapped)
inline constexpr u32 kDmaReportLocal = 0x66626660;
inline constexpr u32 kDmaReportMain = 0xBAD68000;
inline constexpr u32 kDmaNotifyMain0 = 0x6660420F; // ..0x66604208 for NOTIFY_MAIN_7
inline constexpr u32 kDmaNotifyMain7 = 0x66604208;
inline constexpr u32 kDmaToMemoryGetNotify0 = 0x66604207;
inline constexpr u32 kDmaSemaphoreRW = 0x66606660;
inline constexpr u32 kDmaSemaphoreR = 0x66616661;
inline constexpr u32 kDmaDeviceRW = 0x56616660;
inline constexpr u32 kDmaDeviceR = 0x56616661;
} // namespace loc

// ---- method numbers (register indices) --------------------------------------------------------------------------

namespace m {
// NV406E: channel DMA (semaphores, reference)
inline constexpr u32 NV406E_SET_REFERENCE = 0x0050 >> 2;
inline constexpr u32 NV406E_SET_CONTEXT_DMA_SEMAPHORE = 0x0060 >> 2;
inline constexpr u32 NV406E_SEMAPHORE_OFFSET = 0x0064 >> 2;
inline constexpr u32 NV406E_SEMAPHORE_ACQUIRE = 0x0068 >> 2;
inline constexpr u32 NV406E_SEMAPHORE_RELEASE = 0x006c >> 2;

// NV4097: 3D (Curie)
inline constexpr u32 NV4097_SET_OBJECT = 0x0000 >> 2;
inline constexpr u32 NV4097_NO_OPERATION = 0x0100 >> 2;
inline constexpr u32 NV4097_NOTIFY = 0x0104 >> 2;
inline constexpr u32 NV4097_WAIT_FOR_IDLE = 0x0110 >> 2;
inline constexpr u32 NV4097_PM_TRIGGER = 0x0140 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_NOTIFIES = 0x0180 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_A = 0x0184 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_B = 0x0188 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_COLOR_B = 0x018c >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_STATE = 0x0190 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_COLOR_A = 0x0194 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_ZETA = 0x0198 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_VERTEX_A = 0x019c >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_VERTEX_B = 0x01a0 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_SEMAPHORE = 0x01a4 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_REPORT = 0x01a8 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_CLIP_ID = 0x01ac >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_CULL_DATA = 0x01b0 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_COLOR_C = 0x01b4 >> 2;
inline constexpr u32 NV4097_SET_CONTEXT_DMA_COLOR_D = 0x01b8 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_CLIP_HORIZONTAL = 0x0200 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_CLIP_VERTICAL = 0x0204 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_FORMAT = 0x0208 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_PITCH_A = 0x020c >> 2;
inline constexpr u32 NV4097_SET_SURFACE_COLOR_AOFFSET = 0x0210 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_ZETA_OFFSET = 0x0214 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_COLOR_BOFFSET = 0x0218 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_PITCH_B = 0x021c >> 2;
inline constexpr u32 NV4097_SET_SURFACE_COLOR_TARGET = 0x0220 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_PITCH_Z = 0x022c >> 2;
inline constexpr u32 NV4097_INVALIDATE_ZCULL = 0x0234 >> 2;
inline constexpr u32 NV4097_SET_CYLINDRICAL_WRAP = 0x0238 >> 2;
inline constexpr u32 NV4097_SET_CYLINDRICAL_WRAP1 = 0x023c >> 2;
inline constexpr u32 NV4097_SET_SURFACE_PITCH_C = 0x0280 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_PITCH_D = 0x0284 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_COLOR_COFFSET = 0x0288 >> 2;
inline constexpr u32 NV4097_SET_SURFACE_COLOR_DOFFSET = 0x028c >> 2;
inline constexpr u32 NV4097_SET_WINDOW_OFFSET = 0x02b8 >> 2;
inline constexpr u32 NV4097_SET_WINDOW_CLIP_TYPE = 0x02bc >> 2;
inline constexpr u32 NV4097_SET_WINDOW_CLIP_HORIZONTAL = 0x02c0 >> 2;
inline constexpr u32 NV4097_SET_WINDOW_CLIP_VERTICAL = 0x02c4 >> 2;
inline constexpr u32 NV4097_SET_DITHER_ENABLE = 0x0300 >> 2;
inline constexpr u32 NV4097_SET_ALPHA_TEST_ENABLE = 0x0304 >> 2;
inline constexpr u32 NV4097_SET_ALPHA_FUNC = 0x0308 >> 2;
inline constexpr u32 NV4097_SET_ALPHA_REF = 0x030c >> 2;
inline constexpr u32 NV4097_SET_BLEND_ENABLE = 0x0310 >> 2;
inline constexpr u32 NV4097_SET_BLEND_FUNC_SFACTOR = 0x0314 >> 2;
inline constexpr u32 NV4097_SET_BLEND_FUNC_DFACTOR = 0x0318 >> 2;
inline constexpr u32 NV4097_SET_BLEND_COLOR = 0x031c >> 2;
inline constexpr u32 NV4097_SET_BLEND_EQUATION = 0x0320 >> 2;
inline constexpr u32 NV4097_SET_COLOR_MASK = 0x0324 >> 2;
inline constexpr u32 NV4097_SET_STENCIL_TEST_ENABLE = 0x0328 >> 2;
inline constexpr u32 NV4097_SET_STENCIL_MASK = 0x032c >> 2;
inline constexpr u32 NV4097_SET_STENCIL_FUNC = 0x0330 >> 2;
inline constexpr u32 NV4097_SET_STENCIL_FUNC_REF = 0x0334 >> 2;
inline constexpr u32 NV4097_SET_STENCIL_FUNC_MASK = 0x0338 >> 2;
inline constexpr u32 NV4097_SET_STENCIL_OP_FAIL = 0x033c >> 2;
inline constexpr u32 NV4097_SET_STENCIL_OP_ZFAIL = 0x0340 >> 2;
inline constexpr u32 NV4097_SET_STENCIL_OP_ZPASS = 0x0344 >> 2;
inline constexpr u32 NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE = 0x0348 >> 2;
inline constexpr u32 NV4097_SET_BACK_STENCIL_MASK = 0x034c >> 2;
inline constexpr u32 NV4097_SET_BACK_STENCIL_FUNC = 0x0350 >> 2;
inline constexpr u32 NV4097_SET_BACK_STENCIL_FUNC_REF = 0x0354 >> 2;
inline constexpr u32 NV4097_SET_BACK_STENCIL_FUNC_MASK = 0x0358 >> 2;
inline constexpr u32 NV4097_SET_BACK_STENCIL_OP_FAIL = 0x035c >> 2;
inline constexpr u32 NV4097_SET_BACK_STENCIL_OP_ZFAIL = 0x0360 >> 2;
inline constexpr u32 NV4097_SET_BACK_STENCIL_OP_ZPASS = 0x0364 >> 2;
inline constexpr u32 NV4097_SET_SHADE_MODE = 0x0368 >> 2;
inline constexpr u32 NV4097_SET_BLEND_ENABLE_MRT = 0x036c >> 2;
inline constexpr u32 NV4097_SET_COLOR_MASK_MRT = 0x0370 >> 2;
inline constexpr u32 NV4097_SET_LOGIC_OP_ENABLE = 0x0374 >> 2;
inline constexpr u32 NV4097_SET_LOGIC_OP = 0x0378 >> 2;
inline constexpr u32 NV4097_SET_BLEND_COLOR2 = 0x037c >> 2;
inline constexpr u32 NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE = 0x0380 >> 2;
inline constexpr u32 NV4097_SET_DEPTH_BOUNDS_MIN = 0x0384 >> 2;
inline constexpr u32 NV4097_SET_DEPTH_BOUNDS_MAX = 0x0388 >> 2;
inline constexpr u32 NV4097_SET_CLIP_MIN = 0x0394 >> 2;
inline constexpr u32 NV4097_SET_CLIP_MAX = 0x0398 >> 2;
inline constexpr u32 NV4097_SET_CONTROL0 = 0x03b0 >> 2;
inline constexpr u32 NV4097_SET_LINE_WIDTH = 0x03b8 >> 2;
inline constexpr u32 NV4097_SET_LINE_SMOOTH_ENABLE = 0x03bc >> 2;
inline constexpr u32 NV4097_SET_ANISO_SPREAD = 0x03c0 >> 2; // [16]
inline constexpr u32 NV4097_SET_SCISSOR_HORIZONTAL = 0x08c0 >> 2;
inline constexpr u32 NV4097_SET_SCISSOR_VERTICAL = 0x08c4 >> 2;
inline constexpr u32 NV4097_SET_FOG_MODE = 0x08cc >> 2;
inline constexpr u32 NV4097_SET_FOG_PARAMS = 0x08d0 >> 2; // [2]
inline constexpr u32 NV4097_SET_SHADER_PROGRAM = 0x08e4 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_TEXTURE_OFFSET = 0x0900 >> 2; // [4] stride 8 registers
inline constexpr u32 NV4097_SET_VERTEX_TEXTURE_FORMAT = 0x0904 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_TEXTURE_ADDRESS = 0x0908 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_TEXTURE_CONTROL0 = 0x090c >> 2;
inline constexpr u32 NV4097_SET_VERTEX_TEXTURE_CONTROL3 = 0x0910 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_TEXTURE_FILTER = 0x0914 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_TEXTURE_IMAGE_RECT = 0x0918 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_TEXTURE_BORDER_COLOR = 0x091c >> 2;
inline constexpr u32 NV4097_SET_VIEWPORT_HORIZONTAL = 0x0a00 >> 2;
inline constexpr u32 NV4097_SET_VIEWPORT_VERTICAL = 0x0a04 >> 2;
inline constexpr u32 NV4097_SET_POINT_CENTER_MODE = 0x0a0c >> 2;
inline constexpr u32 NV4097_ZCULL_SYNC = 0x0a1c >> 2;
inline constexpr u32 NV4097_SET_VIEWPORT_OFFSET = 0x0a20 >> 2; // [4]
inline constexpr u32 NV4097_SET_VIEWPORT_SCALE = 0x0a30 >> 2;  // [4]
inline constexpr u32 NV4097_SET_POLY_OFFSET_POINT_ENABLE = 0x0a60 >> 2;
inline constexpr u32 NV4097_SET_POLY_OFFSET_LINE_ENABLE = 0x0a64 >> 2;
inline constexpr u32 NV4097_SET_POLY_OFFSET_FILL_ENABLE = 0x0a68 >> 2;
inline constexpr u32 NV4097_SET_DEPTH_FUNC = 0x0a6c >> 2;
inline constexpr u32 NV4097_SET_DEPTH_MASK = 0x0a70 >> 2;
inline constexpr u32 NV4097_SET_DEPTH_TEST_ENABLE = 0x0a74 >> 2;
inline constexpr u32 NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR = 0x0a78 >> 2;
inline constexpr u32 NV4097_SET_POLYGON_OFFSET_BIAS = 0x0a7c >> 2;
inline constexpr u32 NV4097_SET_VERTEX_DATA_SCALED4S_M = 0x0a80 >> 2; // [32]
inline constexpr u32 NV4097_SET_TEXTURE_CONTROL2 = 0x0b00 >> 2;       // [16]
inline constexpr u32 NV4097_SET_TEX_COORD_CONTROL = 0x0b40 >> 2;      // [10]
inline constexpr u32 NV4097_SET_TRANSFORM_PROGRAM = 0x0b80 >> 2;      // [32]
inline constexpr u32 NV4097_SET_SPECULAR_ENABLE = 0x1428 >> 2;
inline constexpr u32 NV4097_SET_TWO_SIDE_LIGHT_EN = 0x142c >> 2;
inline constexpr u32 NV4097_CLEAR_ZCULL_SURFACE = 0x1438 >> 2;
inline constexpr u32 NV4097_SET_PERFORMANCE_PARAMS = 0x1450 >> 2;
inline constexpr u32 NV4097_SET_FLAT_SHADE_OP = 0x1454 >> 2;
inline constexpr u32 NV4097_SET_EDGE_FLAG = 0x145c >> 2;
inline constexpr u32 NV4097_SET_USER_CLIP_PLANE_CONTROL = 0x1478 >> 2;
inline constexpr u32 NV4097_SET_POLYGON_STIPPLE = 0x147c >> 2;
inline constexpr u32 NV4097_SET_POLYGON_STIPPLE_PATTERN = 0x1480 >> 2; // [32]
inline constexpr u32 NV4097_SET_VERTEX_DATA3F_M = 0x1500 >> 2;         // [64] (16 x 4, w unused)
inline constexpr u32 NV4097_SET_VERTEX_DATA_ARRAY_OFFSET = 0x1680 >> 2; // [16]
inline constexpr u32 NV4097_INVALIDATE_VERTEX_CACHE_FILE = 0x1710 >> 2;
inline constexpr u32 NV4097_INVALIDATE_VERTEX_FILE = 0x1714 >> 2;
inline constexpr u32 NV4097_PIPE_NOP = 0x1718 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_DATA_BASE_OFFSET = 0x1738 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_DATA_BASE_INDEX = 0x173c >> 2;
inline constexpr u32 NV4097_SET_VERTEX_DATA_ARRAY_FORMAT = 0x1740 >> 2; // [16]
inline constexpr u32 NV4097_CLEAR_REPORT_VALUE = 0x17c8 >> 2;
inline constexpr u32 NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE = 0x17cc >> 2;
inline constexpr u32 NV4097_GET_REPORT = 0x1800 >> 2;
inline constexpr u32 NV4097_SET_ZCULL_STATS_ENABLE = 0x1804 >> 2;
inline constexpr u32 NV4097_SET_BEGIN_END = 0x1808 >> 2;
inline constexpr u32 NV4097_ARRAY_ELEMENT16 = 0x180c >> 2;
inline constexpr u32 NV4097_ARRAY_ELEMENT32 = 0x1810 >> 2;
inline constexpr u32 NV4097_DRAW_ARRAYS = 0x1814 >> 2;
inline constexpr u32 NV4097_INLINE_ARRAY = 0x1818 >> 2;
inline constexpr u32 NV4097_SET_INDEX_ARRAY_ADDRESS = 0x181c >> 2;
inline constexpr u32 NV4097_SET_INDEX_ARRAY_DMA = 0x1820 >> 2;
inline constexpr u32 NV4097_DRAW_INDEX_ARRAY = 0x1824 >> 2;
inline constexpr u32 NV4097_SET_FRONT_POLYGON_MODE = 0x1828 >> 2;
inline constexpr u32 NV4097_SET_BACK_POLYGON_MODE = 0x182c >> 2;
inline constexpr u32 NV4097_SET_CULL_FACE = 0x1830 >> 2;
inline constexpr u32 NV4097_SET_FRONT_FACE = 0x1834 >> 2;
inline constexpr u32 NV4097_SET_POLY_SMOOTH_ENABLE = 0x1838 >> 2;
inline constexpr u32 NV4097_SET_CULL_FACE_ENABLE = 0x183c >> 2;
inline constexpr u32 NV4097_SET_TEXTURE_CONTROL3 = 0x1840 >> 2; // [16]
inline constexpr u32 NV4097_SET_VERTEX_DATA2F_M = 0x1880 >> 2;  // [32]
inline constexpr u32 NV4097_SET_VERTEX_DATA2S_M = 0x1900 >> 2;  // [16]
inline constexpr u32 NV4097_SET_VERTEX_DATA4UB_M = 0x1940 >> 2; // [16]
inline constexpr u32 NV4097_SET_VERTEX_DATA4S_M = 0x1980 >> 2;  // [32]
inline constexpr u32 NV4097_SET_TEXTURE_OFFSET = 0x1a00 >> 2;   // [16] stride 8 registers
inline constexpr u32 NV4097_SET_TEXTURE_FORMAT = 0x1a04 >> 2;
inline constexpr u32 NV4097_SET_TEXTURE_ADDRESS = 0x1a08 >> 2;
inline constexpr u32 NV4097_SET_TEXTURE_CONTROL0 = 0x1a0c >> 2;
inline constexpr u32 NV4097_SET_TEXTURE_CONTROL1 = 0x1a10 >> 2;
inline constexpr u32 NV4097_SET_TEXTURE_FILTER = 0x1a14 >> 2;
inline constexpr u32 NV4097_SET_TEXTURE_IMAGE_RECT = 0x1a18 >> 2;
inline constexpr u32 NV4097_SET_TEXTURE_BORDER_COLOR = 0x1a1c >> 2;
inline constexpr u32 NV4097_SET_VERTEX_DATA4F_M = 0x1c00 >> 2; // [64]
inline constexpr u32 NV4097_SET_COLOR_KEY_COLOR = 0x1d00 >> 2; // [16]
inline constexpr u32 NV4097_SET_SHADER_CONTROL = 0x1d60 >> 2;
inline constexpr u32 NV4097_SET_INDEXED_CONSTANT_READ_LIMITS = 0x1d64 >> 2;
inline constexpr u32 NV4097_SET_SEMAPHORE_OFFSET = 0x1d6c >> 2;
inline constexpr u32 NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE = 0x1d70 >> 2;
inline constexpr u32 NV4097_TEXTURE_READ_SEMAPHORE_RELEASE = 0x1d74 >> 2;
inline constexpr u32 NV4097_SET_ZMIN_MAX_CONTROL = 0x1d78 >> 2;
inline constexpr u32 NV4097_SET_ANTI_ALIASING_CONTROL = 0x1d7c >> 2;
inline constexpr u32 NV4097_SET_SURFACE_COMPRESSION = 0x1d80 >> 2;
inline constexpr u32 NV4097_SET_ZCULL_EN = 0x1d84 >> 2;
inline constexpr u32 NV4097_SET_SHADER_WINDOW = 0x1d88 >> 2;
inline constexpr u32 NV4097_SET_ZSTENCIL_CLEAR_VALUE = 0x1d8c >> 2;
inline constexpr u32 NV4097_SET_COLOR_CLEAR_VALUE = 0x1d90 >> 2;
inline constexpr u32 NV4097_CLEAR_SURFACE = 0x1d94 >> 2;
inline constexpr u32 NV4097_SET_CLEAR_RECT_HORIZONTAL = 0x1d98 >> 2;
inline constexpr u32 NV4097_SET_CLEAR_RECT_VERTICAL = 0x1d9c >> 2;
inline constexpr u32 NV4097_SET_CLIP_ID_TEST_ENABLE = 0x1da4 >> 2;
inline constexpr u32 NV4097_SET_RESTART_INDEX_ENABLE = 0x1dac >> 2;
inline constexpr u32 NV4097_SET_RESTART_INDEX = 0x1db0 >> 2;
inline constexpr u32 NV4097_SET_LINE_STIPPLE = 0x1db4 >> 2;
inline constexpr u32 NV4097_SET_LINE_STIPPLE_PATTERN = 0x1db8 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_DATA1F_M = 0x1e40 >> 2; // [16]
inline constexpr u32 NV4097_SET_TRANSFORM_EXECUTION_MODE = 0x1e94 >> 2;
inline constexpr u32 NV4097_SET_RENDER_ENABLE = 0x1e98 >> 2;
inline constexpr u32 NV4097_SET_TRANSFORM_PROGRAM_LOAD = 0x1e9c >> 2;
inline constexpr u32 NV4097_SET_TRANSFORM_PROGRAM_START = 0x1ea0 >> 2;
inline constexpr u32 NV4097_SET_ZCULL_CONTROL0 = 0x1ea4 >> 2;
inline constexpr u32 NV4097_SET_ZCULL_CONTROL1 = 0x1ea8 >> 2;
inline constexpr u32 NV4097_SET_SCULL_CONTROL = 0x1eac >> 2;
inline constexpr u32 NV4097_SET_POINT_SIZE = 0x1ee0 >> 2;
inline constexpr u32 NV4097_SET_POINT_PARAMS_ENABLE = 0x1ee4 >> 2;
inline constexpr u32 NV4097_SET_POINT_SPRITE_CONTROL = 0x1ee8 >> 2;
inline constexpr u32 NV4097_SET_TRANSFORM_TIMEOUT = 0x1ef8 >> 2;
inline constexpr u32 NV4097_SET_TRANSFORM_CONSTANT_LOAD = 0x1efc >> 2;
inline constexpr u32 NV4097_SET_TRANSFORM_CONSTANT = 0x1f00 >> 2; // [32]
inline constexpr u32 NV4097_SET_FREQUENCY_DIVIDER_OPERATION = 0x1fc0 >> 2;
inline constexpr u32 NV4097_SET_ATTRIB_COLOR = 0x1fc4 >> 2;
inline constexpr u32 NV4097_SET_ATTRIB_TEX_COORD = 0x1fc8 >> 2;
inline constexpr u32 NV4097_SET_ATTRIB_TEX_COORD_EX = 0x1fcc >> 2;
inline constexpr u32 NV4097_SET_ATTRIB_UCLIP0 = 0x1fd0 >> 2;
inline constexpr u32 NV4097_SET_ATTRIB_UCLIP1 = 0x1fd4 >> 2;
inline constexpr u32 NV4097_INVALIDATE_L2 = 0x1fd8 >> 2;
inline constexpr u32 NV4097_SET_REDUCE_DST_COLOR = 0x1fe0 >> 2;
inline constexpr u32 NV4097_SET_NO_PARANOID_TEXTURE_FETCHES = 0x1fe8 >> 2;
inline constexpr u32 NV4097_SET_SHADER_PACKER = 0x1fec >> 2;
inline constexpr u32 NV4097_SET_VERTEX_ATTRIB_INPUT_MASK = 0x1ff0 >> 2;
inline constexpr u32 NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK = 0x1ff4 >> 2;
inline constexpr u32 NV4097_SET_TRANSFORM_BRANCH_BITS = 0x1ff8 >> 2;

// NV0039: memory to memory format
inline constexpr u32 NV0039_SET_OBJECT = 0x2000 >> 2;
inline constexpr u32 NV0039_SET_CONTEXT_DMA_NOTIFIES = 0x2180 >> 2;
inline constexpr u32 NV0039_SET_CONTEXT_DMA_BUFFER_IN = 0x2184 >> 2;
inline constexpr u32 NV0039_SET_CONTEXT_DMA_BUFFER_OUT = 0x2188 >> 2;
inline constexpr u32 NV0039_OFFSET_IN = 0x230c >> 2;
inline constexpr u32 NV0039_OFFSET_OUT = 0x2310 >> 2;
inline constexpr u32 NV0039_PITCH_IN = 0x2314 >> 2;
inline constexpr u32 NV0039_PITCH_OUT = 0x2318 >> 2;
inline constexpr u32 NV0039_LINE_LENGTH_IN = 0x231c >> 2;
inline constexpr u32 NV0039_LINE_COUNT = 0x2320 >> 2;
inline constexpr u32 NV0039_FORMAT = 0x2324 >> 2;
inline constexpr u32 NV0039_BUFFER_NOTIFY = 0x2328 >> 2;

// NV3062: context surfaces 2D
inline constexpr u32 NV3062_SET_OBJECT = 0x6000 >> 2;
inline constexpr u32 NV3062_SET_CONTEXT_DMA_NOTIFIES = 0x6180 >> 2;
inline constexpr u32 NV3062_SET_CONTEXT_DMA_IMAGE_SOURCE = 0x6184 >> 2;
inline constexpr u32 NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN = 0x6188 >> 2;
inline constexpr u32 NV3062_SET_COLOR_FORMAT = 0x6300 >> 2;
inline constexpr u32 NV3062_SET_PITCH = 0x6304 >> 2;
inline constexpr u32 NV3062_SET_OFFSET_SOURCE = 0x6308 >> 2;
inline constexpr u32 NV3062_SET_OFFSET_DESTIN = 0x630c >> 2;

// NV309E: context surface swizzled
inline constexpr u32 NV309E_SET_OBJECT = 0x8000 >> 2;
inline constexpr u32 NV309E_SET_CONTEXT_DMA_NOTIFIES = 0x8180 >> 2;
inline constexpr u32 NV309E_SET_CONTEXT_DMA_IMAGE = 0x8184 >> 2;
inline constexpr u32 NV309E_SET_FORMAT = 0x8300 >> 2;
inline constexpr u32 NV309E_SET_OFFSET = 0x8304 >> 2;

// NV308A: image from CPU
inline constexpr u32 NV308A_SET_OBJECT = 0xa000 >> 2;
inline constexpr u32 NV308A_SET_CONTEXT_DMA_NOTIFIES = 0xa180 >> 2;
inline constexpr u32 NV308A_SET_CONTEXT_COLOR_KEY = 0xa184 >> 2;
inline constexpr u32 NV308A_SET_CONTEXT_CLIP_RECTANGLE = 0xa188 >> 2;
inline constexpr u32 NV308A_SET_CONTEXT_PATTERN = 0xa18c >> 2;
inline constexpr u32 NV308A_SET_CONTEXT_ROP = 0xa190 >> 2;
inline constexpr u32 NV308A_SET_CONTEXT_BETA1 = 0xa194 >> 2;
inline constexpr u32 NV308A_SET_CONTEXT_BETA4 = 0xa198 >> 2;
inline constexpr u32 NV308A_SET_CONTEXT_SURFACE = 0xa19c >> 2;
inline constexpr u32 NV308A_SET_COLOR_CONVERSION = 0xa2f8 >> 2;
inline constexpr u32 NV308A_SET_OPERATION = 0xa2fc >> 2;
inline constexpr u32 NV308A_SET_COLOR_FORMAT = 0xa300 >> 2;
inline constexpr u32 NV308A_POINT = 0xa304 >> 2;
inline constexpr u32 NV308A_SIZE_OUT = 0xa308 >> 2;
inline constexpr u32 NV308A_SIZE_IN = 0xa30c >> 2;
inline constexpr u32 NV308A_COLOR = 0xa400 >> 2; // [0x700]: 0xa400..0xbffc
inline constexpr u32 NV308A_COLOR_COUNT = 0x700;

// NV3089: scaled image from memory
inline constexpr u32 NV3089_SET_OBJECT = 0xc000 >> 2;
inline constexpr u32 NV3089_SET_CONTEXT_DMA_NOTIFIES = 0xc180 >> 2;
inline constexpr u32 NV3089_SET_CONTEXT_DMA_IMAGE = 0xc184 >> 2;
inline constexpr u32 NV3089_SET_CONTEXT_PATTERN = 0xc188 >> 2;
inline constexpr u32 NV3089_SET_CONTEXT_ROP = 0xc18c >> 2;
inline constexpr u32 NV3089_SET_CONTEXT_BETA1 = 0xc190 >> 2;
inline constexpr u32 NV3089_SET_CONTEXT_BETA4 = 0xc194 >> 2;
inline constexpr u32 NV3089_SET_CONTEXT_SURFACE = 0xc198 >> 2;
inline constexpr u32 NV3089_SET_COLOR_CONVERSION = 0xc2fc >> 2;
inline constexpr u32 NV3089_SET_COLOR_FORMAT = 0xc300 >> 2;
inline constexpr u32 NV3089_SET_OPERATION = 0xc304 >> 2;
inline constexpr u32 NV3089_CLIP_POINT = 0xc308 >> 2;
inline constexpr u32 NV3089_CLIP_SIZE = 0xc30c >> 2;
inline constexpr u32 NV3089_IMAGE_OUT_POINT = 0xc310 >> 2;
inline constexpr u32 NV3089_IMAGE_OUT_SIZE = 0xc314 >> 2;
inline constexpr u32 NV3089_DS_DX = 0xc318 >> 2;
inline constexpr u32 NV3089_DT_DY = 0xc31c >> 2;
inline constexpr u32 NV3089_IMAGE_IN_SIZE = 0xc400 >> 2;
inline constexpr u32 NV3089_IMAGE_IN_FORMAT = 0xc404 >> 2;
inline constexpr u32 NV3089_IMAGE_IN_OFFSET = 0xc408 >> 2;
inline constexpr u32 NV3089_IMAGE_IN = 0xc40c >> 2;

// lv1 driver methods (the real cellGcmSys uses these; RPCS3 rsx_methods.cpp binds them)
inline constexpr u32 GCM_SET_DRIVER_OBJECT = 0xe000 >> 2;
inline constexpr u32 GCM_FLIP_HEAD = 0xe920 >> 2;        // [2]: head 0/1
inline constexpr u32 GCM_DRIVER_QUEUE = 0xe940 >> 2;     // [8]
inline constexpr u32 GCM_SET_USER_COMMAND = 0xeb00 >> 2; // [2]
// RPCS3's HLE cellGcmSetFlip writes this (GCM_FLIP_COMMAND); kept so an HLE cellGcmSys can do the same.
inline constexpr u32 GCM_FLIP_COMMAND = 0xfeac >> 2;

inline constexpr u32 kMethodCount = 0x10000 / 4;
} // namespace m

// ---- semaphore/label/report layout (RPCS3 sys_rsx.h RsxReports, cellGcmSys.cpp) --------------------------------

namespace reports {
inline constexpr u32 kSemaphoreCount = 1024; // u32 each; labels are every 4th (16 bytes apart)
inline constexpr u32 kLabelStride = 0x10;
inline constexpr u32 kNotifyOffset = 0x1000; // RsxNotify[64], 16 bytes each
inline constexpr u32 kNotifyCount = 64;
inline constexpr u32 kReportOffset = 0x1400; // RsxReport[2048], 16 bytes each {u64 timestamp, u32 value, u32 pad}
inline constexpr u32 kReportCount = 2048;
inline constexpr u32 kSize = kReportOffset + kReportCount * 16;

// Report types (NV4097_GET_REPORT arg >> 24, NV4097_CLEAR_REPORT_VALUE arg)
inline constexpr u32 kZpassPixelCount = 1;
inline constexpr u32 kZcullStats = 2;
inline constexpr u32 kZcullStats1 = 3;
inline constexpr u32 kZcullStats2 = 4;
inline constexpr u32 kZcullStats3 = 5;
} // namespace reports

// ---- enums ------------------------------------------------------------------------------------------------------

enum class Primitive : u8 {
    Points = 1,
    Lines = 2,
    LineLoop = 3,
    LineStrip = 4,
    Triangles = 5,
    TriangleStrip = 6,
    TriangleFan = 7,
    Quads = 8,
    QuadStrip = 9,
    Polygon = 10,
};

// Vertex attribute element types (NV4097_SET_VERTEX_DATA_ARRAY_FORMAT bits 0-2; 0 decodes as UB256, gcm_enums.h
// to_vertex_base_type).
enum class VertexType : u8 {
    S1 = 1,    // signed normalized 16-bit
    F = 2,     // float32
    SF = 3,    // float16
    UB = 4,    // unsigned normalized 8-bit
    S32K = 5,  // signed 16-bit, not normalized
    CMP = 6,   // packed 11:11:10 signed normalized, w = 1
    UB256 = 7, // unsigned 8-bit, not normalized
};

constexpr u32 vertex_type_size(VertexType t)
{
    switch (t) {
    case VertexType::S1: return 2;
    case VertexType::F: return 4;
    case VertexType::SF: return 2;
    case VertexType::UB: return 1;
    case VertexType::S32K: return 2;
    case VertexType::CMP: return 4;
    case VertexType::UB256: return 1;
    }
    return 0;
}

// Size in bytes of one attribute with `size` components (CMP is one 32-bit word whatever the size).
constexpr u32 vertex_attribute_bytes(VertexType t, u32 size)
{
    return t == VertexType::CMP ? 4 : vertex_type_size(t) * size;
}

enum class SurfaceColorFormat : u8 {
    X1R5G5B5_Z1R5G5B5 = 1,
    X1R5G5B5_O1R5G5B5 = 2,
    R5G6B5 = 3,
    X8R8G8B8_Z8R8G8B8 = 4,
    X8R8G8B8_O8R8G8B8 = 5,
    A8R8G8B8 = 8,
    B8 = 9,
    G8B8 = 10,
    F_W16Z16Y16X16 = 11,
    F_W32Z32Y32X32 = 12,
    F_X32 = 13,
    X8B8G8R8_Z8B8G8R8 = 14,
    X8B8G8R8_O8B8G8R8 = 15,
    A8B8G8R8 = 16,
};

// Bytes per pixel of a color surface format; 0 = invalid.
constexpr u32 surface_color_bpp(u32 fmt)
{
    switch (fmt) {
    case 1: case 2: case 3: return 2;
    case 4: case 5: case 8: case 14: case 15: case 16: return 4;
    case 9: return 1;
    case 10: return 2;
    case 11: return 8;
    case 12: return 16;
    case 13: return 4;
    default: return 0;
    }
}

enum class SurfaceDepthFormat : u8 { Z16 = 1, Z24S8 = 2 };

enum class SurfaceTarget : u8 {
    None = 0,
    A = 1,
    B = 2,
    AB = 0x13,
    ABC = 0x17,
    ABCD = 0x1f,
};

// Clear mask bits (NV4097_CLEAR_SURFACE)
namespace clear {
inline constexpr u32 kDepth = 0x01;
inline constexpr u32 kStencil = 0x02;
inline constexpr u32 kRed = 0x10;
inline constexpr u32 kGreen = 0x20;
inline constexpr u32 kBlue = 0x40;
inline constexpr u32 kAlpha = 0x80;
inline constexpr u32 kColor = 0xf0;
inline constexpr u32 kDepthStencil = 0x03;
} // namespace clear

// Comparison functions (depth, stencil, alpha): 0x200 | {never, less, equal, lequal, greater, notequal, gequal,
// always}. Alpha and stencil functions may be written without the 0x200 bit (to_comparison_function ors it in).
enum class CompareFunc : u16 {
    Never = 0x200,
    Less = 0x201,
    Equal = 0x202,
    LEqual = 0x203,
    Greater = 0x204,
    NotEqual = 0x205,
    GEqual = 0x206,
    Always = 0x207,
};

enum class StencilOp : u16 {
    Zero = 0x0000,
    Invert = 0x150A,
    Keep = 0x1E00,
    Replace = 0x1E01,
    Incr = 0x1E02,
    Decr = 0x1E03,
    IncrWrap = 0x8507,
    DecrWrap = 0x8508,
};

enum class BlendEquation : u16 {
    Add = 0x8006,
    Min = 0x8007,
    Max = 0x8008,
    Subtract = 0x800A,
    ReverseSubtract = 0x800B,
    ReverseSubtractSigned = 0xF005,
    AddSigned = 0xF006,
    ReverseAddSigned = 0xF007,
};

enum class BlendFactor : u16 {
    Zero = 0,
    One = 1,
    SrcColor = 0x0300,
    OneMinusSrcColor = 0x0301,
    SrcAlpha = 0x0302,
    OneMinusSrcAlpha = 0x0303,
    DstAlpha = 0x0304,
    OneMinusDstAlpha = 0x0305,
    DstColor = 0x0306,
    OneMinusDstColor = 0x0307,
    SrcAlphaSaturate = 0x0308,
    ConstantColor = 0x8001,
    OneMinusConstantColor = 0x8002,
    ConstantAlpha = 0x8003,
    OneMinusConstantAlpha = 0x8004,
};

enum class CullFace : u16 { Front = 0x0404, Back = 0x0405, FrontAndBack = 0x0408 };
enum class FrontFace : u16 { CW = 0x0900, CCW = 0x0901 };
enum class PolygonMode : u16 { Point = 0x1B00, Line = 0x1B01, Fill = 0x1B02 };
enum class ShadeMode : u16 { Flat = 0x1D00, Smooth = 0x1D01 };

// Texture formats (NV4097_SET_TEXTURE_FORMAT bits 8-15 with the LN/UN flags masked off)
namespace tex {
inline constexpr u32 B8 = 0x81;
inline constexpr u32 A1R5G5B5 = 0x82;
inline constexpr u32 A4R4G4B4 = 0x83;
inline constexpr u32 R5G6B5 = 0x84;
inline constexpr u32 A8R8G8B8 = 0x85;
inline constexpr u32 COMPRESSED_DXT1 = 0x86;
inline constexpr u32 COMPRESSED_DXT23 = 0x87;
inline constexpr u32 COMPRESSED_DXT45 = 0x88;
inline constexpr u32 G8B8 = 0x8B;
inline constexpr u32 COMPRESSED_B8R8_G8R8 = 0x8D;
inline constexpr u32 COMPRESSED_R8B8_R8G8 = 0x8E;
inline constexpr u32 R6G5B5 = 0x8F;
inline constexpr u32 DEPTH24_D8 = 0x90;
inline constexpr u32 DEPTH24_D8_FLOAT = 0x91;
inline constexpr u32 DEPTH16 = 0x92;
inline constexpr u32 DEPTH16_FLOAT = 0x93;
inline constexpr u32 X16 = 0x94;
inline constexpr u32 Y16_X16 = 0x95;
inline constexpr u32 R5G5B5A1 = 0x97;
inline constexpr u32 COMPRESSED_HILO8 = 0x98;
inline constexpr u32 COMPRESSED_HILO_S8 = 0x99;
inline constexpr u32 W16_Z16_Y16_X16_FLOAT = 0x9A;
inline constexpr u32 W32_Z32_Y32_X32_FLOAT = 0x9B;
inline constexpr u32 X32_FLOAT = 0x9C;
inline constexpr u32 D1R5G5B5 = 0x9D;
inline constexpr u32 D8R8G8B8 = 0x9E;
inline constexpr u32 Y16_X16_FLOAT = 0x9F;

inline constexpr u32 kLinearFlag = 0x20;   // CELL_GCM_TEXTURE_LN: linear (pitch) layout; clear = swizzled
inline constexpr u32 kUnnormalized = 0x40; // CELL_GCM_TEXTURE_UN: coordinates in texels

inline constexpr u32 kRemapIdentity = 0xAAE4;

enum class Wrap : u8 {
    Wrap = 1,
    Mirror = 2,
    ClampToEdge = 3,
    Border = 4,
    Clamp = 5,
    MirrorOnceClampToEdge = 6,
    MirrorOnceBorder = 7,
    MirrorOnceClamp = 8,
};

enum class MinFilter : u8 {
    Nearest = 1,
    Linear = 2,
    NearestNearest = 3,
    LinearNearest = 4,
    NearestLinear = 5,
    LinearLinear = 6,
    ConvolutionMin = 7,
};

enum class MagFilter : u8 { Nearest = 1, Linear = 2, ConvolutionMag = 4 };
} // namespace tex

} // namespace pspocket::rsx
