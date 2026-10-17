// RSX capture and replay (pspocket/rsx/capture.hpp, D-1053).

#include "pspocket/rsx/capture.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <type_traits>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"

namespace pspocket::rsx {
namespace {

constexpr std::string_view kChan = "rsx.capture";
constexpr u32 kFormat = 1;            // the files' layout; replay refuses another
constexpr u32 kBlockPages = 4;        // pages a watch covers
constexpr u32 kEventMagic = 0x52455654; // "REVT"

static_assert(std::is_trivially_copyable_v<RegisterVertexData>);
static_assert(std::is_trivially_copyable_v<float4>);

void put_u32(std::FILE* f, u32 v) { std::fwrite(&v, 4, 1, f); }
void put_u64(std::FILE* f, u64 v) { std::fwrite(&v, 8, 1, f); }

bool all_zero(const u8* p, usize n)
{
    for (usize i = 0; i < n; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        if (w) return false;
    }
    return true;
}

// The RSX's state where the capture starts, and back.
void write_state(const std::string& path, const RsxState& s)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    const u32 counts[4] = {static_cast<u32>(s.regs.size()), static_cast<u32>(s.transform_program.size()),
        static_cast<u32>(s.transform_constants.size()), static_cast<u32>(s.vertex_registers.size())};
    f.write(reinterpret_cast<const char*>(counts), sizeof counts);
    f.write(reinterpret_cast<const char*>(s.regs.data()), sizeof(u32) * s.regs.size());
    f.write(reinterpret_cast<const char*>(s.transform_program.data()), sizeof(u32) * s.transform_program.size());
    f.write(reinterpret_cast<const char*>(s.transform_constants.data()), sizeof(float4) * s.transform_constants.size());
    f.write(reinterpret_cast<const char*>(s.vertex_registers.data()),
        sizeof(RegisterVertexData) * s.vertex_registers.size());
    f.write(reinterpret_cast<const char*>(&s.texture_cache_epoch), sizeof s.texture_cache_epoch);
}

bool read_state(const std::string& path, RsxState& s)
{
    std::ifstream f(path, std::ios::binary);
    u32 counts[4] = {};
    if (!f.read(reinterpret_cast<char*>(counts), sizeof counts)) return false;
    if (counts[0] != s.regs.size() || counts[1] != s.transform_program.size() ||
        counts[2] != s.transform_constants.size() || counts[3] != s.vertex_registers.size()) {
        return false;
    }
    f.read(reinterpret_cast<char*>(s.regs.data()), sizeof(u32) * s.regs.size());
    f.read(reinterpret_cast<char*>(s.transform_program.data()), sizeof(u32) * s.transform_program.size());
    f.read(reinterpret_cast<char*>(s.transform_constants.data()), sizeof(float4) * s.transform_constants.size());
    f.read(reinterpret_cast<char*>(s.vertex_registers.data()), sizeof(RegisterVertexData) * s.vertex_registers.size());
    f.read(reinterpret_cast<char*>(&s.texture_cache_epoch), sizeof s.texture_cache_epoch);
    s.dirty_bits = dirty::kAll;
    s.in_begin_end = false;
    s.draw.reset(s.draw.primitive);
    return static_cast<bool>(f);
}

} // namespace

// ---- writer -------------------------------------------------------------------------------------------------------

CaptureWriter::CaptureWriter(GuestMemory& mem, WriteTracker& tracker, CaptureOptions options)
    : mem_(mem), tracker_(tracker), options_(std::move(options))
{
    page_size_ = static_cast<u32>(GuestMemory::host_page_size());
}

CaptureWriter::~CaptureWriter()
{
    for (Block& b : blocks_)
        if (b.watch.active) tracker_.unwatch(b.watch);
    if (blobs_) std::fclose(blobs_);
    if (events_) std::fclose(events_);
}

u64 CaptureWriter::store_blob(const u8* data, u32 size)
{
    const u64 hash = WriteTracker::content_hash(data, size) ^ size;
    if (blob_offsets_.contains(hash)) return hash;
    blob_offsets_[hash] = blob_bytes_;
    put_u64(blobs_, hash);
    put_u32(blobs_, size);
    std::fwrite(data, 1, size, blobs_);
    blob_bytes_ += 12 + size;
    return hash;
}

bool CaptureWriter::start(Rsx& rsx)
{
    std::error_code ec;
    std::filesystem::create_directories(options_.dir, ec);
    blobs_ = std::fopen((options_.dir + "/blobs.bin").c_str(), "wb");
    events_ = std::fopen((options_.dir + "/events.bin").c_str(), "wb");
    if (!blobs_ || !events_) {
        why_ = std::format("cannot write into {}", options_.dir);
        return false;
    }
    // What only the GPU holds goes to memory first (the targets drawn before the capture).
    const Config& cfg = rsx.config_;
    regions_.clear();
    regions_.emplace_back(PS_MEM_RSX_LOCAL_BASE, cfg.local_memory_size);
    regions_.emplace_back(cfg.layout.device_addr, cfg.layout.reports_addr + 0x100000 - cfg.layout.device_addr);
    for (u32 page = 0; page < IoMap::kIoPages; page++) {
        const auto ea = rsx.io_map_.io_to_ea(page * IoMap::kPageSize);
        if (!ea) continue;
        if (!regions_.empty() && regions_.back().first + regions_.back().second == *ea && regions_.size() > 2)
            regions_.back().second += IoMap::kPageSize;
        else
            regions_.emplace_back(*ea, IoMap::kPageSize);
        io_signature_ = io_signature_ * 31 + page + *ea;
    }
    for (const auto& [ea, size] : regions_) rsx.backend_.flush_memory(ea, size, "an RSX capture's start");
    {
        std::ofstream m(options_.dir + "/manifest.txt", std::ios::trunc);
        m << "format " << kFormat << "\n";
        m << std::format("layout {:#x} {:#x} {:#x} {:#x}\n", cfg.layout.device_addr, cfg.layout.dma_control_addr,
            cfg.layout.driver_info_addr, cfg.layout.reports_addr);
        m << std::format("local_memory_size {:#x}\n", cfg.local_memory_size);
        m << std::format("fifo_location {}\n", static_cast<u32>(rsx.fifo_location_.load(std::memory_order_acquire)));
        m << std::format("page_size {}\n", page_size_);
        m << std::format("get {:#x}\nput {:#x}\n", rsx.get_.load(std::memory_order_relaxed), rsx.read_put());
        m << std::format("from_flip {}\nframes {}\n", options_.from_flip, options_.frames);
        for (const auto& [ea, size] : regions_) m << std::format("region {:#x} {:#x}\n", ea, size);
        for (u32 page = 0; page < IoMap::kIoPages; page++) {
            if (const auto ea = rsx.io_map_.io_to_ea(page * IoMap::kPageSize))
                m << std::format("io {:#x} {:#x}\n", page * IoMap::kPageSize, *ea);
        }
        for (u32 id = 0; id < 8; id++) {
            if (const auto b = rsx.display_buffer(id))
                m << std::format("display {} {:#x} {} {} {}\n", id, b->offset, b->pitch, b->width, b->height);
        }
    }
    // Watches over the committed pages, then every non-zero page as it is.
    std::ofstream image(options_.dir + "/image.bin", std::ios::binary | std::ios::trunc);
    u64 image_pages = 0;
    for (const auto& [rea, rsize] : regions_) {
        const u64 end = u64{rea} + rsize;
        for (u64 at = rea; at < end;) {
            // A block: up to kBlockPages committed pages in a row (a watch reads every byte of its range).
            if (!mem_.is_committed(static_cast<u32>(at))) {
                at += page_size_;
                continue;
            }
            Block b;
            b.ea = static_cast<u32>(at);
            while (at < end && b.pages.size() < kBlockPages && mem_.is_committed(static_cast<u32>(at))) {
                Page p;
                p.ea = static_cast<u32>(at);
                p.generation = tracker_.page_generation(p.ea);
                b.pages.push_back(p);
                at += page_size_;
            }
            b.size = static_cast<u32>(b.pages.size()) * page_size_;
            b.watch = tracker_.watch(b.ea, b.size);
            for (Page& p : b.pages) {
                const u8* data = mem_.host(p.ea);
                if (all_zero(data, page_size_)) continue;
                p.hash = store_blob(data, page_size_);
                image.write(reinterpret_cast<const char*>(&p.ea), 4);
                image.write(reinterpret_cast<const char*>(&p.hash), 8);
                image_pages++;
            }
            blocks_.push_back(std::move(b));
            if (blob_bytes_ > options_.max_bytes) {
                why_ = std::format("the memory image passed the {} MB cap", options_.max_bytes >> 20);
                return false;
            }
        }
    }
    write_state(options_.dir + "/state.bin", rsx.state_);
    start_words_ = rsx.words_.load(std::memory_order_relaxed);
    {
        std::ofstream m(options_.dir + "/manifest.txt", std::ios::app);
        m << std::format("image_pages {}\n", image_pages);
    }
    PS_LOG_INFO(kChan, "capture into {} from flip {}: {} regions, {} pages in the image ({:.1f} MB of blobs)",
        options_.dir, rsx.flips_.load(), regions_.size(), image_pages, static_cast<double>(blob_bytes_) / 1048576.0);
    return true;
}

void CaptureWriter::harvest(std::vector<std::pair<u32, u64>>& out)
{
    for (Block& b : blocks_) {
        if (!tracker_.is_dirty(b.watch)) continue;
        for (Page& p : b.pages) {
            const u64 g = tracker_.page_generation(p.ea);
            if (g == p.generation && tracker_.protection()) continue;
            p.generation = g;
            const u8* data = mem_.host(p.ea);
            const u64 hash = all_zero(data, page_size_) ? 0 : WriteTracker::content_hash(data, page_size_) ^ page_size_;
            if (hash == p.hash) continue;
            p.hash = hash ? store_blob(data, page_size_) : 0;
            out.emplace_back(p.ea, p.hash);
        }
        tracker_.unwatch(b.watch);
        b.watch = tracker_.watch(b.ea, b.size);
    }
}

void CaptureWriter::write_event(const Event& e)
{
    put_u32(events_, kEventMagic);
    put_u64(events_, e.words - start_words_);
    put_u32(events_, e.put);
    put_u32(events_, e.get);
    put_u32(events_, static_cast<u32>(e.pages.size()));
    for (const auto& [ea, hash] : e.pages) {
        put_u32(events_, ea);
        put_u64(events_, hash);
    }
    put_u32(events_, static_cast<u32>(e.flip_requests.size()));
    for (const auto& [head, value] : e.flip_requests) {
        put_u32(events_, head);
        put_u32(events_, value);
    }
    put_u32(events_, e.reset ? 1u : 0u);
    put_u32(events_, e.reset_get);
    put_u32(events_, e.reset_put);
    put_u32(events_, e.flips);
    events_written_++;
    pages_written_ += e.pages.size();
}

void CaptureWriter::before_step(Rsx& rsx)
{
    Phase ph = phase();
    if (ph == Phase::Done || ph == Phase::Failed) return;
    if (ph == Phase::Waiting) {
        if (rsx.flips_.load(std::memory_order_relaxed) < options_.from_flip) return;
        // A boundary with no packet, call or wait open.
        if (rsx.packet_remaining_ || !rsx.return_stack_.empty() ||
            rsx.block_reason_.load(std::memory_order_relaxed) != Rsx::BlockReason::None ||
            rsx.fifo_reset_.load(std::memory_order_acquire)) {
            return;
        }
        if (!start(rsx)) {
            fail(rsx, why_);
            return;
        }
        phase_.store(Phase::Recording, std::memory_order_release);
    }
    if (finishing_) {
        // The step that made the last flip has ended: its record, then a closing one with the words it processed.
        if (have_current_) write_event(current_);
        Event end;
        end.words = rsx.words_.load(std::memory_order_relaxed);
        end.put = rsx.read_put();
        end.get = rsx.get_.load(std::memory_order_relaxed);
        write_event(end);
        have_current_ = false;
        finish(rsx, true, {});
        return;
    }
    // The record of the step that ran last, then this one's: its pages, put.
    std::vector<std::pair<u32, u64>> pages;
    harvest(pages);
    // A new IO mapping: not followed (the capture would miss what it maps).
    u32 sig = 0;
    for (u32 page = 0; page < IoMap::kIoPages; page++)
        if (const auto ea = rsx.io_map_.io_to_ea(page * IoMap::kPageSize)) sig = sig * 31 + page + *ea;
    if (sig != io_signature_) {
        fail(rsx, "the IO map changed during the capture");
        return;
    }
    const u64 words = rsx.words_.load(std::memory_order_relaxed);
    const u32 put = rsx.read_put();
    if (have_current_ && pages.empty() && put == current_.put && words == current_.words &&
        current_.flip_requests.empty() && !current_.reset && current_.flips == 0) {
        return; // nothing new: the same record goes on
    }
    if (have_current_) write_event(current_);
    current_ = Event{};
    current_.words = words;
    current_.put = put;
    current_.get = rsx.get_.load(std::memory_order_relaxed);
    current_.pages = std::move(pages);
    have_current_ = true;
    if (blob_bytes_ > options_.max_bytes)
        fail(rsx, std::format("the blobs passed the {} MB cap", options_.max_bytes >> 20));
}

void CaptureWriter::note_flip_requests(const std::vector<std::pair<u32, u32>>& requests)
{
    if (phase() != Phase::Recording || !have_current_) return;
    current_.flip_requests.insert(current_.flip_requests.end(), requests.begin(), requests.end());
}

void CaptureWriter::note_fifo_reset(u32 get, u32 put)
{
    if (phase() != Phase::Recording || !have_current_) return;
    current_.reset = true;
    current_.reset_get = get;
    current_.reset_put = put;
}

void CaptureWriter::after_flip(Rsx& rsx)
{
    if (phase() != Phase::Recording || !have_current_) return;
    current_.flips++;
    if (++flips_ >= options_.frames) finishing_ = true; // the step with the last flip ends; the next one closes it
    (void)rsx;
}

void CaptureWriter::fail(Rsx& rsx, std::string why)
{
    PS_LOG_ERROR(kChan, "capture into {} stopped: {}", options_.dir, why);
    finish(rsx, false, std::move(why));
}

void CaptureWriter::finish(Rsx&, bool complete, std::string why)
{
    if (have_current_ && events_) write_event(current_);
    have_current_ = false;
    for (Block& b : blocks_)
        if (b.watch.active) tracker_.unwatch(b.watch);
    blocks_.clear();
    if (blobs_) std::fclose(blobs_);
    if (events_) std::fclose(events_);
    blobs_ = events_ = nullptr;
    why_ = why;
    {
        std::ofstream m(options_.dir + "/manifest.txt", std::ios::app);
        m << (complete ? "complete" : "incomplete") << " " << flips_ << " flips";
        if (!why.empty()) m << ": " << why;
        m << "\n";
    }
    phase_.store(complete ? Phase::Done : Phase::Failed, std::memory_order_release);
    PS_LOG_INFO(kChan, "{}", summary());
}

std::string CaptureWriter::summary() const
{
    return std::format("RSX capture {}: {} ({} flips, {} records, {} pages written, {:.1f} MB of blobs){}{}",
        options_.dir,
        phase() == Phase::Done ? "complete" : phase() == Phase::Failed ? "incomplete" : "not finished", flips_,
        events_written_, pages_written_, static_cast<double>(blob_bytes_) / 1048576.0, why_.empty() ? "" : ": ", why_);
}

// ---- reader -------------------------------------------------------------------------------------------------------

bool read_capture_info(const std::string& dir, CaptureInfo& info, std::string* error)
{
    std::ifstream m(dir + "/manifest.txt");
    if (!m) {
        if (error) *error = std::format("no capture in {} (manifest.txt)", dir);
        return false;
    }
    info = CaptureInfo{};
    u32 format = 0;
    for (std::string line; std::getline(m, line);) {
        std::istringstream s(line);
        std::string key;
        s >> key;
        const auto num = [&s]() {
            std::string v;
            s >> v;
            return static_cast<u64>(std::stoull(v, nullptr, 0));
        };
        if (key == "format") format = static_cast<u32>(num());
        else if (key == "layout") {
            info.config.layout.device_addr = static_cast<u32>(num());
            info.config.layout.dma_control_addr = static_cast<u32>(num());
            info.config.layout.driver_info_addr = static_cast<u32>(num());
            info.config.layout.reports_addr = static_cast<u32>(num());
        } else if (key == "local_memory_size") info.config.local_memory_size = static_cast<u32>(num());
        else if (key == "fifo_location") info.config.fifo_location = static_cast<FifoLocation>(num());
        else if (key == "page_size") info.page_size = static_cast<u32>(num());
        else if (key == "get") info.get = static_cast<u32>(num());
        else if (key == "put") info.put = static_cast<u32>(num());
        else if (key == "from_flip") info.from_flip = num();
        else if (key == "frames") info.frames = static_cast<u32>(num());
        else if (key == "region") {
            const u32 ea = static_cast<u32>(num());
            info.regions.emplace_back(ea, static_cast<u32>(num()));
        } else if (key == "io") {
            const u32 io = static_cast<u32>(num());
            info.io_map.emplace_back(io, static_cast<u32>(num()));
        } else if (key == "display") {
            const u32 id = static_cast<u32>(num());
            DisplayBuffer b;
            b.offset = static_cast<u32>(num());
            b.pitch = static_cast<u32>(num());
            b.width = static_cast<u32>(num());
            b.height = static_cast<u32>(num());
            if (id < 8) info.display_buffers[id] = b;
        } else if (key == "complete") info.complete = true;
    }
    if (format != kFormat) {
        if (error) *error = std::format("{}: capture format {}, this build reads {}", dir, format, kFormat);
        return false;
    }
    if (info.page_size != GuestMemory::host_page_size()) {
        if (error) *error = std::format("{}: pages of {} bytes, this host's are {}", dir, info.page_size,
            GuestMemory::host_page_size());
        return false;
    }
    info.config.unhandled_policy = FailPolicy::Log;
    return true;
}

namespace {

// blobs.bin read whole into an index.
struct Blobs {
    std::vector<u8> bytes;
    std::unordered_map<u64, std::pair<usize, u32>> at; // hash -> offset of the data, size
    bool load(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        bytes.resize(static_cast<usize>(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        for (usize o = 0; o + 12 <= bytes.size();) {
            u64 hash;
            u32 size;
            std::memcpy(&hash, bytes.data() + o, 8);
            std::memcpy(&size, bytes.data() + o + 8, 4);
            at[hash] = {o + 12, size};
            o += 12 + size;
        }
        return true;
    }
};

template <class T>
bool get(std::ifstream& f, T& v)
{
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof v));
}

} // namespace

bool replay_capture(const std::string& dir, GuestMemory& mem, WriteTracker& tracker, Rsx& rsx,
    const ReplayOptions& options, ReplayResult& result, std::string* error)
{
    const auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    CaptureInfo info;
    if (!read_capture_info(dir, info, error)) return false;
    Blobs blobs;
    if (!blobs.load(dir + "/blobs.bin")) return fail(std::format("{}: no blobs.bin", dir));
    const u32 page = info.page_size;
    // Memory: the regions committed, zero but for the image's pages.
    for (const auto& [ea, size] : info.regions) {
        const u32 lo = ea & ~(page - 1);
        const u64 hi = (u64{ea} + size + page - 1) & ~u64{page - 1};
        if (!mem.commit(lo, static_cast<u32>(hi - lo))) return fail(std::format("cannot commit {:#x}+{:#x}", lo, hi - lo));
    }
    // The notifies and reports are the RSX's own output (the GPU's counts land there as its work completes): after the
    // image the replay's RSX writes them, the capture's records do not, or when each lands would race the replay's GPU.
    const u32 out_lo = info.config.layout.reports_addr + reports::kNotifyOffset;
    const u32 out_hi = info.config.layout.reports_addr + reports::kReportOffset + 2048 * 16;
    bool keep_outputs = false;
    std::vector<u8> saved;
    const auto write_page = [&](u32 ea, u64 hash) {
        u8* dst = mem.host(ea);
        const u32 lo = std::max(ea, out_lo), hi = std::min(ea + page, out_hi);
        const bool keep = keep_outputs && lo < hi;
        if (keep) saved.assign(mem.host(lo), mem.host(lo) + (hi - lo));
        if (!hash) {
            std::memset(dst, 0, page);
        } else {
            const auto it = blobs.at.find(hash);
            if (it == blobs.at.end()) return false;
            std::memcpy(dst, blobs.bytes.data() + it->second.first, std::min(page, it->second.second));
        }
        if (keep) std::memcpy(mem.host(lo), saved.data(), saved.size());
        tracker.notify_write(ea, page);
        rsx.backend_.invalidate_memory(ea, page);
        return true;
    };
    {
        std::ifstream image(dir + "/image.bin", std::ios::binary);
        u32 ea;
        u64 hash;
        while (get(image, ea) && get(image, hash))
            if (!write_page(ea, hash)) return fail(std::format("{}: the image's page {:#x} has no blob", dir, ea));
    }
    for (const auto& [io, ea] : info.io_map) rsx.io_map_.map(io, ea, IoMap::kPageSize);
    for (u32 id = 0; id < 8; id++)
        if (info.display_buffers[id]) rsx.set_display_buffer(id, *info.display_buffers[id]);
    if (!read_state(dir + "/state.bin", rsx.state_)) return fail(std::format("{}: state.bin does not fit this build", dir));
    rsx.set_fifo_pointers(info.get, info.get);
    const u32 put_addr = info.config.layout.put_addr();
    // The records.
    std::ifstream events(dir + "/events.bin", std::ios::binary);
    struct Record {
        u64 words = 0;
        u32 put = 0, get = 0, flips = 0;
        std::vector<std::pair<u32, u64>> pages;
        std::vector<std::pair<u32, u32>> requests;
        bool reset = false;
        u32 reset_get = 0, reset_put = 0;
    };
    std::vector<Record> records;
    for (;;) {
        u32 magic = 0;
        if (!get(events, magic)) break;
        if (magic != kEventMagic) return fail(std::format("{}: events.bin damaged at record {}", dir, records.size()));
        Record r;
        u32 n = 0, reset = 0;
        get(events, r.words);
        get(events, r.put);
        get(events, r.get);
        get(events, n);
        r.pages.resize(n);
        for (auto& [ea, hash] : r.pages) {
            get(events, ea);
            get(events, hash);
        }
        get(events, n);
        r.requests.resize(n);
        for (auto& [head, value] : r.requests) {
            get(events, head);
            get(events, value);
        }
        get(events, reset);
        get(events, r.reset_get);
        get(events, r.reset_put);
        if (!get(events, r.flips)) return fail(std::format("{}: events.bin cut short", dir));
        r.reset = reset != 0;
        records.push_back(std::move(r));
    }
    const u64 base_words = rsx.words_.load(std::memory_order_relaxed);
    keep_outputs = true;
    for (usize i = 0; i < records.size(); i++) {
        const Record& r = records[i];
        for (const auto& [ea, hash] : r.pages)
            if (!write_page(ea, hash)) return fail(std::format("{}: record {}'s page {:#x} has no blob", dir, i, ea));
        ps_write32(mem.base(), put_addr, r.put);
        if (r.reset) rsx.set_fifo_pointers(r.reset_get, r.reset_put);
        for (const auto& [head, value] : r.requests) rsx.request_flip(head, value);
        const u64 target = i + 1 < records.size() ? records[i + 1].words : r.words;
        const u64 done = rsx.words_.load(std::memory_order_relaxed) - base_words;
        if (target > done || !r.requests.empty() || r.reset) {
            const u64 budget = target > done ? target - done : 0;
            rsx.run_until_idle(budget ? budget : 0);
        }
        const u64 now = rsx.words_.load(std::memory_order_relaxed) - base_words;
        if (now != target) {
            if (!result.deviations++) {
                result.first_deviation = std::format("record {}: {} words processed, the capture {}", i, now, target);
            }
            if (options.verbose) PS_LOG_WARN(kChan, "replay of {}: record {}: {} words, the capture {}", dir, i, now, target);
        }
        result.flips += r.flips;
    }
    rsx.backend_.sync();
    result.events = records.size();
    result.words = rsx.words_.load(std::memory_order_relaxed) - base_words;
    return true;
}

} // namespace pspocket::rsx
