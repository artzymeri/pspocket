#pragma once

// EDGE geometry on the GPU (D-988): the work items SPU jobs leave for the RSX, by the address of their ring allocation
// in RSX local memory. A job of a known EDGE build records its heavy stages (pspocket/spu/edge_gpu.hpp) and, just
// before its command buffer hole goes out (the RSX may read it from then on), publishes the item under its
// allocation's address; the RSX backend takes it at the draw whose index array starts there, and has the GPU make
// what the job would have put into the allocation.
//
// PSPOCKET_EDGE_GPU: unset or 0 off; 1 or gpu the GPU path; cpu the work item's kernels run on the SPU's thread at
// once, their output written into the allocation as the job would have (a check of the recording, no GPU); ab the GPU
// path in the "on" half of PSPOCKET_GPU_AB's frames (set_ab_half), the SPUs' path in the other.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace pspocket::edge {

enum class Mode : uint8_t { off, gpu, cpu, ab };
Mode mode();
// Whether a job starting now takes the GPU path (mode cpu; mode gpu, or ab in the "on" half, once a consumer, the RSX
// backend's EDGE kernels, is ready).
bool deferred_now();
void set_consumer(bool ready);
// The backend's PSPOCKET_GPU_AB half, for mode ab.
void set_ab_half(bool on);
bool ab_half();

struct WorkItem {
    std::vector<uint8_t> blob; // ps_edge_job and its data (pspocket/core/edge_job.h)
    uint32_t alloc_ea = 0;     // the ring allocation in RSX local memory (guest address)
    uint32_t alloc_size = 0;
    uint32_t index_bytes = 0;  // the index area at its start (the full count, 16-byte aligned); the vertices follow
};

// Work items made once and used again (no allocation a job): acquire one to fill, recycle it once used.
std::unique_ptr<WorkItem> acquire();
void recycle(std::unique_ptr<WorkItem> item);

void publish(std::unique_ptr<WorkItem> item);
// The item published for the allocation at `ea`, removed; nullptr when there is none.
std::unique_ptr<WorkItem> take(uint32_t ea);
// Whether any item is waiting (a cheap test for a draw that may not be EDGE's).
bool any_waiting();

// EDGE jobs so far (of builds with a GPU path): deferred (their stages recorded) or run whole by their SPU, with their
// SPU threads' host time; the work items published and taken; jobs a stage left to the SPU.
struct Stats {
    uint64_t published, taken, replaced, jobs_deferred, ns_deferred, jobs_spu, ns_spu, rejected;
};
Stats stats();
void count_job(bool deferred, uint64_t ns);
void count_rejected();

} // namespace pspocket::edge
