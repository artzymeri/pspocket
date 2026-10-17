// rsx-replay: an RSX capture (pspocket-boot --rsx-capture, pspocket/rsx/capture.hpp, D-1053) run through the RSX and
// the Metal backend alone: no game, no lv2, the same frames every run.
//
//   rsx-replay CAPTURE_DIR [--frames DIR] [--draws FILE] [--describe N] [--quiet] [--verbose] [--pages FILE]
//              [--sync A-B] [--trace 1]
//
// Prints one line a flip: its index in the capture and a hash of the presented frame (the backend's output, read
// back), then a summary. --frames DIR saves each as DIR/frame-NNN.png. --draws FILE writes, after every draw and
// clear, a hash of each colour target it drew to as guest memory holds it once written back (the backend flushed
// over the target: slow), one line each: "N draw|clear EA WxH HASH" (scripts/rsx_bisect.py compares two runs'
// lines to find the first that differs). --describe N prints what draw or clear N had bound: its targets, programs,
// depth, stencil and blend state, textures. --pages FILE writes, at each flip, a hash of every 4 KB page of local
// memory as the GPU has it (written back first): two runs' files say which targets parted. --sync A-B writes back
// what draws A to B drew, as --draws does, without the hashes (a wait for the GPU after each: which waits make a race
// go). --trace 1 is the backend's frame trace from the first command (the capture's first frame). A new target starts
// with what memory holds (the capture wrote the targets back when it started; D-1058); with
// PSPOCKET_REPLAY_UNDEFINED_TARGETS=1 it is undefined, as in a fast run. The backend runs as the environment says:
// PSPOCKET_RSX_EXACT=1, and every
// switch of its fast paths (PSPOCKET_RSX_NO_CARRY, PSPOCKET_RSX_NO_SURFACE_POOL, ...). Exit 0: replayed with no
// deviation; 1: a deviation (the RSX processed other words than the capture's); 2: not a capture this build reads.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#import <Metal/Metal.h>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/core/png.hpp"
#include "pspocket/rsx/capture.hpp"
#include "pspocket/rsx/metal/metal_backend.hpp"
#include "pspocket/rsx/write_tracker.hpp"

using namespace pspocket;
using namespace pspocket::rsx;

namespace {

u64 fnv(const u8* p, usize n)
{
    u64 h = 1469598103934665603ull;
    for (usize i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// The backend, and after each draw or clear a hash of the colour targets it drew to (--draws).
class Observer final : public Backend {
public:
    Observer(metal::MetalBackend& inner, GuestMemory& mem, std::FILE* out, u64 describe)
        : b_(inner), mem_(mem), out_(out), describe_(describe)
    {
    }
    void attach(Rsx& rsx) override { b_.attach(rsx); }
    void draw(const RsxState& s) override
    {
        b_.draw(s);
        note("draw", s);
    }
    void clear(u32 mask, const RsxState& s) override
    {
        b_.clear(mask, s);
        note("clear", s);
    }
    void flip(const FlipRequest& r, const RsxState& s) override
    {
        if (pages_) { // --pages: local memory as the GPU has it, before the flip's present (a sync point a frame)
            b_.flush_memory(PS_MEM_RSX_LOCAL_BASE, local_size_, "rsx-replay --pages");
            flips_++;
            for (u32 a = 0; a < local_size_; a += 4096) {
                const u8* p = mem_.host(PS_MEM_RSX_LOCAL_BASE + a);
                std::fprintf(pages_, "%llu %#010x %016llx\n", static_cast<unsigned long long>(flips_),
                    PS_MEM_RSX_LOCAL_BASE + a, static_cast<unsigned long long>(fnv(p, 4096)));
            }
        }
        b_.flip(r, s);
    }
    void set_sync(u64 lo, u64 hi)
    {
        sync_lo_ = lo;
        sync_hi_ = hi;
    }
    void set_pages(std::FILE* f, u32 local_size)
    {
        pages_ = f;
        local_size_ = local_size;
    }
    void draw_skipped(const RsxState& s, std::string_view why) override { b_.draw_skipped(s, why); }
    bool tracing() const override { return b_.tracing(); }
    void trace_note(std::string_view what) override { b_.trace_note(what); }
    void sync() override { b_.sync(); }
    void invalidate_memory(u32 a, u32 n) override { b_.invalidate_memory(a, n); }
    bool flush_memory(u32 a, u32 n, const char* why) override { return b_.flush_memory(a, n, why); }
    bool copy_on_gpu(u32 s, u32 sp, u32 d, u32 dp, u32 l, u32 c) override { return b_.copy_on_gpu(s, sp, d, dp, l, c); }
    bool flush_memory_later(u32 a, u32 n, u32 wa, u32 wn, std::function<void()> then) override
    {
        return b_.flush_memory_later(a, n, wa, wn, std::move(then));
    }
    void defer(u32 a, u32 n, std::function<void()> action) override { b_.defer(a, n, std::move(action)); }
    void before_access(u32 a, u32 n, bool w) override { b_.before_access(a, n, w); }
    void run_deferred(bool wait) override { b_.run_deferred(wait); }
    bool deferring() const override { return b_.deferring(); }
    void set_zpass_counting(bool on) override { b_.set_zpass_counting(on); }
    void clear_zpass() override { b_.clear_zpass(); }
    bool report_zpass(u32 a, bool whole) override { return b_.report_zpass(a, whole); }
    bool report_pending(u32 a) override { return b_.report_pending(a); }
    bool predicate_on_gpu(u32 a) override { return b_.predicate_on_gpu(a); }
    void end_predicate() override { b_.end_predicate(); }

private:
    void note(const char* what, const RsxState& s)
    {
        n_++;
        if (n_ == describe_) describe(what, s);
        const bool sync = n_ >= sync_lo_ && n_ <= sync_hi_;
        if (!out_ && !sync) return;
        const SurfaceInfo si = s.surface();
        std::array<u32, kMaxColorTargets> idx{};
        const u32 count = si.active_targets(idx);
        const u32 w = si.clip_x + si.clip_width, h = si.clip_y + si.clip_height;
        for (u32 i = 0; i < count; i++) {
            const ColorBuffer& c = si.color[idx[i]];
            if (!c.pitch) continue;
            const u32 ea = (c.dma == loc::kDmaFrameBuffer ? PS_MEM_RSX_LOCAL_BASE : 0) + c.offset;
            const u32 bytes = c.pitch * h;
            if (!mem_.is_committed(ea) || !mem_.is_committed(ea + bytes - 1)) continue;
            b_.flush_memory(ea, bytes, "rsx-replay --draws");
            if (!out_) continue;
            std::fprintf(out_, "%llu %s %#010x %ux%u %016llx\n", static_cast<unsigned long long>(n_), what, ea, w, h,
                static_cast<unsigned long long>(fnv(mem_.host(ea), bytes)));
        }
    }
    // --describe N: what draw or clear N had bound.
    void describe(const char* what, const RsxState& s) const
    {
        const SurfaceInfo si = s.surface();
        std::printf("%s %llu: surface format %u depth format %u type %u aa %u clip %ux%u at %u,%u, target mode %u\n",
            what, static_cast<unsigned long long>(n_), si.color_format, si.depth_format, si.type, si.antialias,
            si.clip_width, si.clip_height, si.clip_x, si.clip_y, si.target);
        for (u32 i = 0; i < kMaxColorTargets; i++) {
            if (si.color[i].pitch)
                std::printf("  colour %u: offset %#x pitch %#x dma %#x\n", i, si.color[i].offset, si.color[i].pitch, si.color[i].dma);
        }
        std::printf("  depth: offset %#x pitch %#x dma %#x%s\n", si.depth.offset, si.depth.pitch, si.depth.dma,
            si.depth_float ? " float" : "");
        const ShaderProgramInfo fp = s.shader_program();
        std::printf("  fragment program %#x (location %u, control %#x), vertex program start %u\n", fp.offset, fp.location,
            fp.control, s.transform_program_start());
        const DepthStencilInfo ds = s.depth_stencil();
        std::printf("  depth test %d func %#x write %d, stencil %d, bounds %d\n", ds.depth_test, ds.depth_func,
            ds.depth_write, ds.stencil_test, ds.depth_bounds_test);
        const BlendInfo bl = s.blend();
        std::printf("  blend %d (mrt %#x) src %#x/%#x dst %#x/%#x eq %#x/%#x; colour mask %02x %02x %02x %02x\n", bl.enable,
            bl.enable_mrt, bl.sfactor_rgb, bl.sfactor_a, bl.dfactor_rgb, bl.dfactor_a, bl.equation_rgb, bl.equation_a,
            s.color_mask().mask[0], s.color_mask().mask[1], s.color_mask().mask[2], s.color_mask().mask[3]);
        for (u32 t = 0; t < 16; t++) {
            const TextureInfo ti = s.fragment_texture(t);
            if (!ti.enabled) continue;
            std::printf("  texture %u: offset %#x location %u format %#x %ux%u pitch %#x mips %u\n", t, ti.offset,
                ti.location, ti.format, ti.width, ti.height, ti.pitch, ti.mipmaps);
        }
    }
    metal::MetalBackend& b_;
    GuestMemory& mem_;
    std::FILE* out_;
    u64 describe_ = 0;
    u64 n_ = 0;
    std::FILE* pages_ = nullptr;
    u64 sync_lo_ = ~0ull, sync_hi_ = 0;
    u32 local_size_ = 0;
    u64 flips_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: rsx-replay CAPTURE_DIR [--frames DIR] [--draws FILE] [--describe N] [--quiet] [--verbose]\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::string frames_dir, draws_path, pages_path;
    u64 sync_lo = ~0ull, sync_hi = 0;
    u64 trace_frame = 0;
    bool quiet = false, verbose = false;
    u64 describe = 0;
    for (int i = 2; i < argc; i++) {
        const std::string_view a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames_dir = argv[++i];
        else if (a == "--draws" && i + 1 < argc) draws_path = argv[++i];
        else if (a == "--quiet") quiet = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--trace" && i + 1 < argc) trace_frame = std::strtoull(argv[++i], nullptr, 0); // the backend's frame trace
        else if (a == "--pages" && i + 1 < argc) pages_path = argv[++i];
        else if (a == "--sync" && i + 1 < argc) { // draws A-B written back as --draws does, without the hashes
            char* end = nullptr;
            sync_lo = std::strtoull(argv[++i], &end, 0);
            sync_hi = end && *end == '-' ? std::strtoull(end + 1, nullptr, 0) : sync_lo;
        }
        else if (a == "--describe" && i + 1 < argc) describe = std::strtoull(argv[++i], nullptr, 0);
        else {
            std::fprintf(stderr, "rsx-replay: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (quiet) log::set_min_level(log::Level::Warn);
    CaptureInfo info;
    std::string error;
    if (!read_capture_info(dir, info, &error)) {
        std::fprintf(stderr, "rsx-replay: %s\n", error.c_str());
        return 2;
    }
    if (!info.complete) std::fprintf(stderr, "rsx-replay: %s is an incomplete capture: replaying what it has\n", dir.c_str());
    if (!frames_dir.empty()) std::filesystem::create_directories(frames_dir);
    std::FILE* draws = draws_path.empty() ? nullptr : std::fopen(draws_path.c_str(), "w");
    @autoreleasepool {
        GuestMemory mem;
        if (!mem.reserve()) {
            std::fprintf(stderr, "rsx-replay: cannot reserve guest memory\n");
            return 2;
        }
        WriteTracker tracker(mem); // hash mode: replay writes memory itself
        metal::MetalBackendConfig mc;
        u32 out_w = 1280, out_h = 720;
        for (const auto& b : info.display_buffers)
            if (b && b->width) {
                out_w = b->width;
                out_h = b->height;
                break;
            }
        mc.trace_frame = trace_frame;
        mc.load_new_targets = !std::getenv("PSPOCKET_REPLAY_UNDEFINED_TARGETS"); // D-1058
        mc.output_width = out_w;
        mc.output_height = out_h;
        std::mutex lines_mutex;
        std::vector<std::string> lines;
        u64 presented = 0;
        mc.capture_every = 1;
        mc.capture = [&](const u8* bgra, u32 w, u32 h, u64 flip) {
            std::lock_guard lock(lines_mutex);
            const u64 n = ++presented;
            lines.push_back(std::format("flip {} ({}) hash {:016x}", n, flip, fnv(bgra, usize{w} * h * 4)));
            if (!frames_dir.empty())
                write_png_rgb(std::format("{}/frame-{:03}.png", frames_dir, n), bgra, w, h, w * 4, true);
        };
        auto backend = std::make_unique<metal::MetalBackend>(mem, tracker, std::move(mc));
        if (!backend->valid()) {
            std::fprintf(stderr, "rsx-replay: no Metal device\n");
            return 2;
        }
        Observer observer(*backend, mem, draws, describe);
        std::FILE* pages = pages_path.empty() ? nullptr : std::fopen(pages_path.c_str(), "w");
        if (pages) observer.set_pages(pages, info.config.local_memory_size);
        observer.set_sync(sync_lo, sync_hi);
        Rsx rsx(mem, observer, info.config);
        ReplayResult result;
        ReplayOptions ro;
        ro.verbose = verbose;
        if (!replay_capture(dir, mem, tracker, rsx, ro, result, &error)) {
            std::fprintf(stderr, "rsx-replay: %s\n", error.c_str());
            return 2;
        }
        backend->sync();
        if (draws) std::fclose(draws);
        if (pages) std::fclose(pages);
        {
            std::lock_guard lock(lines_mutex);
            for (const std::string& l : lines) std::printf("%s\n", l.c_str());
        }
        {
            const AlarmCounts al = rsx.alarms().snapshot();
            std::string line;
            for (usize i = 0; i < al.v.size(); i++)
                if (al.v[i]) line += std::format(" {} {}", kAlarmNames[i], al.v[i]);
            std::printf("alarms:%s\n", line.empty() ? " none" : line.c_str());
        }
        std::printf("replayed %s: %llu records (%llu written early), %llu flips, %llu words; %llu deviations%s%s\n",
            dir.c_str(), static_cast<unsigned long long>(result.events), static_cast<unsigned long long>(result.early),
            static_cast<unsigned long long>(result.flips),
            static_cast<unsigned long long>(result.words), static_cast<unsigned long long>(result.deviations),
            result.deviations ? ", the first: " : "", result.first_deviation.c_str());
        if (result.path_deviations)
            std::printf("%llu records ended elsewhere in the FIFO than the capture's, the first: %s\n",
                static_cast<unsigned long long>(result.path_deviations), result.first_path_deviation.c_str());
        return result.deviations ? 1 : 0;
    }
}
