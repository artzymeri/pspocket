#pragma once

// EDGE geometry on the GPU, the SPU side (D-988): a job of a known EDGE build records its heavy stages into a GPU work
// item (pspocket/core/edge_job.h) instead of computing them. The code is in runtime/spu/replace/edge_gpu.cpp.
//
// A Recording is the work item being made for the job running on this host thread. Each stage's stand-in (a native
// implementation of the stage's function in the build's table) reads what the stage would read from local store at
// its entry, copies it into the blob, does to local store and the registers what the rest of the job reads after the
// stage (the context's pointers, the counts), and returns without the stage's work. The culling stage's stand-in keeps
// every index, so the job allocates, puts and draws the full count; the GPU culls. Whether a job can go to the GPU is
// decided at its first stage (the vertex decompression), from its configuration: anything not handled leaves the
// whole job to the SPU.
//
// The same functions build work items from captured states (spu-job-replay's probes): edge-gpu-check runs the kernels
// on them against what the SPU made.

#include <cstdint>
#include <string>
#include <vector>

#include "pspocket/core/edge_job.h"
#include "pspocket/spu/context.h"

namespace pspocket::spu::edge_gpu {

struct Recording {
    std::vector<uint8_t> blob; // the header's room, then the data
    ps_edge_job job{};
    std::string reject;        // why the GPU path cannot take the job ("" while it can)
    uint32_t stages = 0;       // bit per stage recorded: 1 decompression, 2 skinning, 4 culling, 8 compression
    uint32_t ctx = 0;          // the job's EDGE context in local store

    void clear();
    // Appends n bytes of local store from `at` (wrapping) 16-aligned with 16 bytes of padding; returns their offset.
    uint32_t append(const uint8_t* ls, uint32_t at, uint32_t n);
    // The header written into the blob: the finished work item.
    void finish();
};

// The stages of GTA V's EDGE build's job (spu_dad457863220b1cc), from the state at each function's entry. Each returns
// false (and sets r.reject) when the job is not one the GPU path handles.
bool on_decompress(Recording& r, const uint8_t* ls, const SPUContext& ctx); // 0x0DAF0
bool on_skin(Recording& r, const uint8_t* ls, const SPUContext& ctx);       // 0x07A10
bool on_cull(Recording& r, const uint8_t* ls, const SPUContext& ctx);       // 0x0EFF8
bool on_compress(Recording& r, const uint8_t* ls, const SPUContext& ctx);   // 0x0F5B8

// What each stage leaves for the rest of the job (the context's words, the return value, the link slot), done to
// ls and ctx as the stage's function would at its return; ctx->pc goes to the return address.
void leave_decompress(const Recording& r, uint8_t* ls, SPUContext& ctx);
void leave_skin(const Recording& r, uint8_t* ls, SPUContext& ctx);
void leave_cull(const Recording& r, uint8_t* ls, SPUContext& ctx);
void leave_compress(const Recording& r, uint8_t* ls, SPUContext& ctx);

} // namespace pspocket::spu::edge_gpu
