/*
 * EDGE geometry on the GPU (D-988): one job of Sony's EDGE geometry as a GPU work item.
 *
 * An SPU job of a known EDGE build runs on its SPU for what the rest of the machine sees (its ring allocation, the
 * order of its DMA, the RSX commands in its command buffer hole); its heavy stages record instead of computing
 * (runtime/spu/replace/edge_gpu.cpp), into a blob: this header, then the data the stages would have read (the
 * vertex streams, the skinning matrices and weights, the decompressed indices), big-endian as local store held them.
 * The Metal backend runs the blob's work (pspocket/core/edge_kernels.inc) when the RSX reaches the draw that
 * reads its output, and produces what the job would have put into its ring allocation: the kept indices (u16,
 * big-endian) at its start and the vertex stream after them, plus the count for an indirect draw.
 *
 * Plain C with 32-bit fields only: the same declaration is read by C++ (host-endian, little) and by the Metal
 * shaders, whose source gets it as text (PS_EDGE_JOB_DECLS stringified, so no file is generated: the iOS app compiles
 * the runtime's sources as they are, D-014).
 */
#ifndef PSPOCKET_CORE_EDGE_JOB_H
#define PSPOCKET_CORE_EDGE_JOB_H

#define PS_EDGE_JOB_DECLS(U32, S32)                                                                                    \
    /* Output attribute formats (as EDGE's stream descriptions number them, where seen; PS_EDGE_FMT_I16N is the    \
     * predefined output format 2's tangent) and attribute sources (EDGE's attribute ids). */                        \
    enum {                                                                                                          \
        PS_EDGE_JOB_MAGIC = 0x45444731, /* "EDG1" */                                                               \
        PS_EDGE_FMT_F32 = 2,                                                                                        \
        PS_EDGE_FMT_X11Y11Z10N = 6,                                                                                 \
        PS_EDGE_FMT_FIXED = 8,       /* input: bit-packed fixed point */                                            \
        PS_EDGE_FMT_UNIT_VECTOR = 9, /* input: two components of 10 bits, the third from them, signs, selector */   \
        PS_EDGE_FMT_I16N = 0x104,                                                                                   \
        PS_EDGE_ATTR_POSITION = 1,                                                                                  \
        PS_EDGE_ATTR_NORMAL = 2,                                                                                    \
        PS_EDGE_ATTR_TANGENT = 3,                                                                                   \
        PS_EDGE_ATTR_TABLE = 0x100, /* the values of a table the SPU made (`table`), D-1006 */                      \
        PS_EDGE_MAX_OUT = 8                                                                                         \
    };                                                                                                              \
    typedef struct ps_edge_out_attr {                                                                               \
        U32 source; /* PS_EDGE_ATTR_* (0: unused) */                                                                \
        U32 format; /* PS_EDGE_FMT_* */                                                                             \
        U32 components;                                                                                             \
        U32 offset; /* bytes into the output vertex */                                                              \
        U32 table;  /* PS_EDGE_ATTR_TABLE: its four floats a vertex in the blob (big-endian, 16 bytes a vertex) */  \
    } ps_edge_out_attr;                                                                                             \
    typedef struct ps_edge_job {                                                                                    \
        U32 magic;                                                                                                  \
        U32 size; /* bytes of the blob, header included */                                                          \
        U32 num_vertexes;                                                                                           \
        U32 num_indexes;                                                                                            \
        /* Positions: bit-packed fixed point, a vertex every pos_stride bytes from pos_data (offsets into the       \
         * blob): the attribute's first bit in its vertex, each component's bits and fraction bits, the stream's    \
         * fixed offsets (added before the scale). */                                                              \
        U32 pos_data;                                                                                               \
        U32 pos_stride;                                                                                             \
        U32 pos_bit;                                                                                                \
        U32 pos_bits[3];                                                                                            \
        U32 pos_frac[3];                                                                                            \
        S32 pos_bias[3];                                                                                            \
        /* Normals and tangents: unit vectors, 3 bytes each, from uv_data every uv_stride bytes; their byte in the  \
         * vertex, or ~0: none. */                                                                                 \
        U32 uv_data;                                                                                                \
        U32 uv_stride;                                                                                              \
        U32 nrm_offset;                                                                                             \
        U32 tan_offset;                                                                                             \
        /* tables (D-1006): the job's stages before the GPU's ran on the SPU; positions, normals and tangents are   \
         * its tables of four floats a vertex (16 bytes a vertex, big-endian) at pos_data, nrm_table, tan_table     \
         * (normals and tangents there when nrm_offset and tan_offset are not ~0). */                               \
        U32 tables;                                                                                                 \
        U32 nrm_table;                                                                                              \
        U32 tan_table;                                                                                              \
        /* Skinning: 0 none, 1 four bones ((weight, bone) x 4 a vertex), 4 one bone (a byte a vertex); the matrices \
         * 3x4 floats, rows, big-endian. */                                                                        \
        U32 skin;                                                                                                   \
        U32 matrices;                                                                                               \
        U32 matrix_count;                                                                                           \
        U32 weights;                                                                                                \
        /* The screen transform (EDGE's vertex output for culling): C = M1 * M0' as the SPU made it, the viewport's \
         * scales and offsets (+0.5 in x and y where the job adds it), the rectangle and the depth range. */       \
        float combined[16];                                                                                         \
        float scale[4];                                                                                             \
        float offset[4];                                                                                            \
        float bound_min[4];                                                                                         \
        float bound_max[4];                                                                                         \
        /* Culling: the indices (u16 big-endian, num_indexes of them) at `indices`; flip: xor into the area's sign; \
         * culled[keep * 2 + outside]: what the mode's arithmetic makes of the facing and frustum tests; nopix: the \
         * "no pixel" test on. cull_mode 0: nothing culled. */                                                     \
        U32 indices;                                                                                                \
        U32 cull_mode;                                                                                              \
        U32 flip;                                                                                                   \
        U32 nopix;                                                                                                  \
        U32 culled[4];                                                                                              \
        /* Output: the vertex stream after the indices in the allocation, out_stride bytes a vertex. */            \
        U32 out_vertex;                                                                                             \
        U32 out_stride;                                                                                             \
        U32 out_count;                                                                                              \
        U32 pad0;                                                                                                   \
        ps_edge_out_attr out[8];                                                                                    \
    } ps_edge_job;

#ifndef __METAL_VERSION__
#include <stdint.h>
PS_EDGE_JOB_DECLS(uint32_t, int32_t)
#endif

#endif /* PSPOCKET_CORE_EDGE_JOB_H */
