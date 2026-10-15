/*
 * EDGE geometry on the GPU (D-988): one job of Sony's EDGE geometry as a GPU work item.
 *
 * An SPU job of a known EDGE build runs on its SPU for what the rest of the machine sees (its ring allocation, the
 * order of its DMA, the RSX commands in its command buffer hole); its heavy stages record instead of computing
 * (runtime/spu/replace/edge_gpu.cpp), into a blob: this header, then the data the stages would have read (the
 * vertex streams, the skinning matrices and weights, the decompressed indices), big-endian as local store held them.
 * The Metal backend runs the blob's work (runtime/rsx/metal/src/edge_kernels.inc) when the RSX reaches the draw that
 * reads its output, and produces what the job would have put into its ring allocation: the kept indices (u16,
 * big-endian) at its start and the vertex stream after them, plus the count for an indirect draw.
 *
 * Plain C with 32-bit fields only: the same declaration is read by C++ (host-endian, little) and by the Metal
 * shaders (it is part of their source).
 */
#ifndef PSPOCKET_CORE_EDGE_JOB_H
#define PSPOCKET_CORE_EDGE_JOB_H

#ifndef __METAL_VERSION__
#include <stdint.h>
#define PS_EDGE_U32 uint32_t
#define PS_EDGE_S32 int32_t
#else
#define PS_EDGE_U32 uint
#define PS_EDGE_S32 int
#endif

#define PS_EDGE_JOB_MAGIC 0x45444731u /* "EDG1" */

/* Output attribute formats (as EDGE's stream descriptions number them, where seen; PS_EDGE_FMT_I16N is the
 * predefined output format 2's tangent). */
#define PS_EDGE_FMT_F32 2u
#define PS_EDGE_FMT_X11Y11Z10N 6u
#define PS_EDGE_FMT_FIXED 8u       /* input: bit-packed fixed point */
#define PS_EDGE_FMT_UNIT_VECTOR 9u /* input: two components of 10 bits, the third from them, signs and selector */
#define PS_EDGE_FMT_I16N 0x104u

/* Attribute sources (EDGE's attribute ids). */
#define PS_EDGE_ATTR_POSITION 1u
#define PS_EDGE_ATTR_NORMAL 2u
#define PS_EDGE_ATTR_TANGENT 3u

#define PS_EDGE_MAX_OUT 4

typedef struct ps_edge_out_attr {
    PS_EDGE_U32 source; /* PS_EDGE_ATTR_* (0: unused) */
    PS_EDGE_U32 format; /* PS_EDGE_FMT_* */
    PS_EDGE_U32 components;
    PS_EDGE_U32 offset; /* bytes into the output vertex */
} ps_edge_out_attr;

typedef struct ps_edge_job {
    PS_EDGE_U32 magic;
    PS_EDGE_U32 size; /* bytes of the blob, header included */
    PS_EDGE_U32 num_vertexes;
    PS_EDGE_U32 num_indexes;

    /* Positions: bit-packed fixed point, a vertex every pos_stride bytes from pos_data (offsets into the blob). */
    PS_EDGE_U32 pos_data;
    PS_EDGE_U32 pos_stride;
    PS_EDGE_U32 pos_bit;     /* the attribute's first bit in its vertex */
    PS_EDGE_U32 pos_bits[3]; /* bits of each component (integer and fraction) */
    PS_EDGE_U32 pos_frac[3]; /* fraction bits */
    PS_EDGE_S32 pos_bias[3]; /* the stream's fixed offsets, added before the scale */

    /* Normals and tangents: unit vectors, 3 bytes each, from uv_data every uv_stride bytes (0: none). */
    PS_EDGE_U32 uv_data;
    PS_EDGE_U32 uv_stride;
    PS_EDGE_U32 nrm_offset; /* byte in the vertex, or ~0u: none */
    PS_EDGE_U32 tan_offset; /* byte in the vertex, or ~0u: none */

    /* Skinning: 0 none, 1 four bones (weight, bone) x 4 a vertex, 4 one bone (a byte a vertex). Matrices: 3x4 floats,
     * rows, big-endian. */
    PS_EDGE_U32 skin;
    PS_EDGE_U32 matrices;
    PS_EDGE_U32 matrix_count;
    PS_EDGE_U32 weights;

    /* The screen transform (EDGE's vertex output for culling): C = M1 * M0' as the SPU made it, the viewport's scales
     * and offsets (+0.5 already in the x and y offsets where the job adds it), the rectangle and the depth range. */
    float combined[16]; /* rows */
    float scale[4];
    float offset[4];
    float bound_min[4]; /* x, y, z */
    float bound_max[4];

    /* Culling: the indices (u16 big-endian, num_indexes of them) at indices; flip: xor into the area's sign; culled[k]
     * for k = keep * 2 + outside: what the mode's arithmetic makes of the facing and frustum tests; nopix: the "no
     * pixel" test on. mode 0: nothing culled (every index kept). */
    PS_EDGE_U32 indices;
    PS_EDGE_U32 cull_mode;
    PS_EDGE_U32 flip;
    PS_EDGE_U32 nopix;
    PS_EDGE_U32 culled[4];

    /* Output: the vertex stream after the indices (out_vertex bytes into the allocation), out_stride bytes a vertex. */
    PS_EDGE_U32 out_vertex;
    PS_EDGE_U32 out_stride;
    PS_EDGE_U32 out_count;
    PS_EDGE_U32 pad0;
    ps_edge_out_attr out[PS_EDGE_MAX_OUT];
} ps_edge_job;

#undef PS_EDGE_U32
#undef PS_EDGE_S32

#endif /* PSPOCKET_CORE_EDGE_JOB_H */
