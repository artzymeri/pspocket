#pragma once

// EDGE geometry on the GPU (D-988): the work items SPU jobs leave for the RSX, by the address of their ring allocation
// in RSX local memory. A job of a known EDGE build records its heavy stages (pspocket/spu/edge_gpu.hpp) and, just
// before its command buffer hole goes out (the RSX may read it from then on), publishes the item under its
// allocation's address; the RSX backend takes it at the draw whose index array starts there, and has the GPU make
// what the job would have put into the allocation.
//
// PSPOCKET_EDGE_GPU: unset or 0 off; 1 or gpu the GPU path; cpu the work item's kernels run on the SPU's thread at
// once, their output written into the allocation as the job would have (a check of the recording, no GPU); ab the GPU
// path in every other PSPOCKET_EDGE_AB frames (set_ab_half, by the backend), the SPUs' path in the others; verify the
// SPUs' path, with each job's work item recorded too and run on the GPU beside it, its output compared with what the
// job put (the backend's "EDGE verify" lines).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pspocket::edge {

// How a job's work goes (D-1005): on the SPU whole, or on the GPU from one of its stages on, the stages before it run by
// the SPU as ever. From the decompression: the GPU decodes the vertex streams. From the skinning: it starts from the
// tables the SPU's decompression made (and whatever the job did to them before the skinning). From the culling: from
// the skinned tables (and whatever the job did to them after the skinning).
enum class Route : uint8_t { spu, decompress, skin, cull, count };

// Why a job's work goes the way it does: why it is left to the SPU, or why it goes to the GPU from a later stage than
// the decompression. Counted per route and reason (no text, no lock: the counters are atomics).
enum class Reason : uint8_t {
    none,
    // the configuration (EdgeGeomSpuConfigInfo in the job's context)
    input_format,       // a predefined input format (not a custom description)
    blend_shapes,       // a delta (blend shape) format
    skin_flavour,       // a skinning flavour other than none, four bones or one
    matrix_format,      // a matrix format other than 0
    output_format,      // a predefined output format the GPU path does not make
    custom_decompress,  // a custom decompression callback
    context_flag,       // the context's byte 0 top bit clear
    callback,           // a vertex-output callback other than the screen transform 0x0ECF8
    projecting,         // the callback's projecting path
    counts,             // no vertices, or too many vertices or indices
    // the vertex streams (custom descriptions)
    primary_stream,     // not one attribute of three fixed-point components, positions
    fixed_point_wide,   // fixed point of more than 24 bits a component
    secondary_attribute,// a secondary attribute other than normals and tangents as unit vectors
    stream_description, // a description out of range
    // the output
    output_attribute,   // an output attribute's format, size or source not made
    output_stride,
    // the job driver's own steps around EDGE's (GTA V's: its descriptor fields, D-1006)
    position_offset,    // a position offset added after the decompression ($83)
    colour_pass,        // a per-vertex palette pass after the culling (descriptor 0x194, with 0x1A7 0)
    blend_path,         // descriptor 0x1A4 bit 2: extra data fetched and applied before the skinning
    matrix_path,        // descriptor 0x1A4 bits 3 and 4: 0x0E398 before the skinning
    output_mode,        // descriptor 0x1A7 == 2: the output format changed after the decompression
    triangle_filter,    // descriptor 0x1A6: a triangle filter on the positions before the culling
    normal_pass,        // descriptor 0x1BC: positions and normals changed after the skinning
    // the later routes' own
    tables,             // the stage's tables not as the GPU path reads them (a binormal table, no position table)
    count
};
const char* reason_name(Reason r);
// PSPOCKET_EDGE_GPU_ROUTE=skin or cull: every job's GPU work starts no earlier than that stage (a check of the later
// routes on every job; default the decompression).
Route earliest_route();

enum class Mode : uint8_t { off, gpu, cpu, ab, verify };
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
    uint32_t index_bytes = 0;  // the index area at its start, as the job made it (16-byte aligned); the vertices follow
    uint32_t out_index_bytes = 0; // room for every index (16-byte aligned): where the GPU puts the kept ones
    // verify: the job ran on its SPU, the allocation holds its own output: its kept indices' area (the first PUT), its
    // vertices after it
    uint32_t guest_index_bytes = 0;
    uint64_t kinds = 0; // bit per Reason its route was taken for, and kind_route(route): verify's counts by kind
};
constexpr int kind_route(Route r) { return 48 + static_cast<int>(r); }
// mode verify: whether the job of this route and reason starting now is verified (its SPU's work kept and compared),
// or takes the GPU path: the first 64 of each kind, then every PSPOCKET_EDGE_VERIFY_EVERY-th (default 1: every job;
// on the phone 16 or so keeps the frame rate near the GPU path's, D-1006).
bool verify_this(Route route, Reason why);

// Work items made once and used again (no allocation a job): acquire one to fill, recycle it once used.
std::unique_ptr<WorkItem> acquire();
void recycle(std::unique_ptr<WorkItem> item);

// Publishing threads (the SPU workers) each have a ring of their own; one thread at a time takes (the RSX thread):
// no lock on either side (D-1008).
void publish(std::unique_ptr<WorkItem> item);
// The item published for the allocation at `ea`, removed; nullptr when there is none.
std::unique_ptr<WorkItem> take(uint32_t ea);
// Whether any item is waiting (a cheap test for a draw that may not be EDGE's).
bool any_waiting();

const char* route_name(Route r);

// EDGE jobs so far (of builds with a GPU path): deferred (their stages recorded) or run whole by their SPU, with their
// SPU threads' host time; the work items published and taken; the jobs by route and reason.
struct Stats {
    uint64_t published, taken, replaced, jobs_deferred, ns_deferred, jobs_spu, ns_spu;
    uint64_t label_gets[2]; // GETs of an RSX label in jobs run whole [0] and deferred [1]: the ring full, waiting
    uint64_t routes[static_cast<int>(Route::count)][static_cast<int>(Reason::count)];
};
Stats stats();
void count_job(bool deferred, uint64_t ns, uint64_t label_gets);
void count_route(Route route, Reason why);
// "decompress 640, skin 300 (colour pass 200, ...), ..., spu 12 (...)": the counts since `before`, per frame.
std::string describe_routes(const Stats& now, const Stats& before, double frames);

} // namespace pspocket::edge
