#pragma once

// RSX capture and replay (D-1053): everything the RSX reads over a few frames of a run, written to a folder, and the
// RSX with a backend driven from that folder alone, no game, the same way every time. A frame that shows wrong (the
// sea's black block, a flicker a detector flagged) becomes a file the backend can be run on over and over, with its
// fast paths switched on and off one by one (scripts/rsx_bisect.py).
//
// What a capture holds (all of it game output: inventory/, never the repository):
// - manifest.txt: the context layout, local memory's size, where the FIFO lives, the IO map, the display buffers, the
//   regions watched (local memory, the RSX's context memory, the IO-mapped main memory), the page size, the frames;
// - state.bin: the register file, the vertex program and its constants, the vertex registers (RsxState), taken where
//   the capture starts: at a step boundary of the RSX thread after the flip --rsx-capture-from names, with no packet
//   or call open;
// - image.bin: every non-zero page of the regions then (address, blob), after the backend wrote back what only the GPU
//   held (Backend::flush_memory), so the targets drawn before the capture start as memory holds them;
// - events.bin: one record each time the RSX thread is about to process commands: the words processed so far, the put
//   register, the pages written since the last record (address, blob: found through the write tracker's watches, so
//   only pages a store reached are hashed), the flips asked for outside the FIFO and FIFO resets it took then, and the
//   flips it made; the FIFO's words themselves are in the pages (the command buffer is memory too), so jumps, calls
//   and returns replay as they ran;
// - blobs.bin: each page content once (content-addressed by its hash).
// Labels, semaphores and reports are memory: their values arrive in the records as the CPU (or the GPU's completion)
// wrote them. The capture stops after its frames, or when its blobs pass the size cap (its manifest then says
// "incomplete").
//
// Replay (replay_capture): commits the regions, writes the image, restores the IO map, the display buffers, the state
// and get, then for each record writes its pages, put, the flips asked for and the resets, and runs the RSX for as many
// words as the capture did until the next record (Rsx::run_until_idle); when it blocks short of them, the next record's
// pages first (a store the capture's RSX saw within a step: D-1054). The backend runs in the caller's mode (fast,
// or PSPOCKET_RSX_EXACT=1). Pages a store reached during a step reach the replay at the next record, as the capture
// saw them: a race between the CPU and the RSX within one step is replayed one way, always the same.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/rsx/rsx.hpp"
#include "pspocket/rsx/write_tracker.hpp"

namespace pspocket::rsx {

struct CaptureOptions {
    std::string dir;
    u64 from_flip = 0;            // the capture starts after this flip (the RSX's count)
    u32 frames = 1;               // and records this many flips
    u64 max_bytes = 2048ull << 20; // blobs past this: the capture stops, incomplete
    bool armed = true;            // false: from_flip waits for arm() (a trigger: a detector that saw the frame wrong)
};

class CaptureWriter {
public:
    CaptureWriter(GuestMemory& mem, WriteTracker& tracker, CaptureOptions options);
    ~CaptureWriter();
    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    // Rsx's hooks, on its thread.
    void before_step(Rsx& rsx);
    void note_flip_requests(const std::vector<std::pair<u32, u32>>& requests);
    void note_fifo_reset(u32 get, u32 put);
    void after_flip(Rsx& rsx);

    // A trigger (D-1054): start after flip `flip` (or the next step boundary, if the RSX is past it) and record the
    // frames asked for. Once only; any thread. False when the capture was armed already.
    bool arm(u64 flip);

    enum class Phase : u8 { Waiting, Recording, Done, Failed };
    Phase phase() const { return phase_.load(std::memory_order_acquire); }
    // One line: what was recorded and its size, or why it failed.
    std::string summary() const;

private:
    struct Page {
        u32 ea = 0;
        u64 hash = 0;
        u64 generation = 0;
    };
    struct Block {
        u32 ea = 0, size = 0;
        WriteTracker::Watch watch;
        std::vector<Page> pages;
    };
    struct Event {
        u64 words = 0;
        u32 put = 0, get = 0;
        std::vector<std::pair<u32, u64>> pages; // ea, blob hash
        std::vector<std::pair<u32, u32>> flip_requests;
        bool reset = false;
        u32 reset_get = 0, reset_put = 0;
        u32 flips = 0; // flips made while this record's commands ran
    };

    bool start(Rsx& rsx);
    void harvest(std::vector<std::pair<u32, u64>>& out);
    void check_around(Rsx& rsx, std::vector<std::pair<u32, u64>>& out);
    u64 store_blob(const u8* data, u32 size); // its hash; written once
    void write_event(const Event& e);
    void finish(Rsx& rsx, bool complete, std::string why);
    void fail(Rsx& rsx, std::string why);

    GuestMemory& mem_;
    WriteTracker& tracker_;
    CaptureOptions options_;
    std::atomic<Phase> phase_{Phase::Waiting};
    std::atomic<u64> start_at_{~0ull}; // the flip the capture starts after: options_.from_flip, or arm()'s
    std::vector<Block> blocks_;
    std::vector<std::pair<u32, u32>> regions_; // ea, size
    std::unordered_map<u64, u64> blob_offsets_; // hash -> offset in blobs.bin
    u64 blob_bytes_ = 0;
    u32 page_size_ = 0;
    std::FILE* blobs_ = nullptr;
    std::FILE* events_ = nullptr;
    Event current_;
    bool have_current_ = false;
    bool finishing_ = false; // the last flip made: the next step boundary closes the capture
    u64 events_written_ = 0, pages_written_ = 0, flips_ = 0, start_words_ = 0, started_after_ = 0;
    u32 io_signature_ = 0;
    std::string why_;
    bool check_ = false; // PSPOCKET_RSX_CAPTURE_CHECK
    u64 missed_ = 0;
};

struct ReplayOptions {
    bool verbose = false;
};

struct ReplayResult {
    u64 events = 0, flips = 0, words = 0;
    u64 deviations = 0; // records after which the RSX had not processed the words the capture had
    u64 early = 0;      // records whose pages were written early: the RSX blocked on a store the capture saw mid-step
    std::string first_deviation;
    u64 path_deviations = 0; // records after which the words were the capture's but get was not (another path taken)
    std::string first_path_deviation;
};

// The capture's manifest and state, for the replay harness to build its Rsx with (its layout, local memory, FIFO
// location).
struct CaptureInfo {
    Config config;
    std::vector<std::pair<u32, u32>> regions; // ea, size
    std::vector<std::pair<u32, u32>> io_map;  // io, ea (1 MB pages)
    std::array<std::optional<DisplayBuffer>, 8> display_buffers{};
    u32 get = 0, put = 0, page_size = 0, frames = 0;
    u64 from_flip = 0;
    bool complete = false;
};
bool read_capture_info(const std::string& dir, CaptureInfo& info, std::string* error);

// Replays a capture into `rsx` (made with info.config over `mem`, no thread started). `tracker`: the backend's, told of
// every page written. Returns false (with `error`) when the folder is not a capture this build reads.
bool replay_capture(const std::string& dir, GuestMemory& mem, WriteTracker& tracker, Rsx& rsx,
    const ReplayOptions& options, ReplayResult& result, std::string* error);

} // namespace pspocket::rsx
