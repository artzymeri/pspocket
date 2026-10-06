// The PAMF demultiplexer (dmux_pamf.hpp, D-194). Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellDmuxPamf.cpp,
// dmux_pamf_base and its elementary streams, ported function by function; "LLE" marks libdmuxpamf's quirks RPCS3
// keeps. Differences: where RPCS3 ensure()s an access unit queue invariant (a release of the wrong size would corrupt
// LLE's queue), pspocket logs it and leaves the queue as it is.

#include "dmux_pamf.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"

namespace pspocket::hle::dmux {
namespace {

constexpr std::string_view kChan = "cellDmuxPamf";

constexpr u32 PACK_SIZE = 0x800;
constexpr u32 PACK_STUFFING_LENGTH_OFFSET = 0xd, PES_PACKET_LENGTH_OFFSET = 0x4, PES_HEADER_DATA_LENGTH_OFFSET = 0x8,
              PTS_DTS_FLAG_OFFSET = 0x7, PACKET_START_CODE_PREFIX = 1;
constexpr u32 M2V_PIC_START = 0x100, AVC_AU_DELIMITER = 0x109, M2V_SEQUENCE_HEADER = 0x1b3, M2V_SEQUENCE_END = 0x1b7,
              PACK_START = 0x1ba, SYSTEM_HEADER = 0x1bb, PRIVATE_STREAM_2 = 0x1bf, PROG_END = 0x1b9;
constexpr u32 kNone = 0xFFFFFFFF; // umax: an invalid stream header

u32 be32(const u8* p) { return ps_ld32(p); }
u16 be16(const u8* p) { return ps_ld16(p); }

// A partial access unit to write to the queue: up to three bytes from the previous PES packet, then this one's data.
struct Chunk {
    std::vector<u8> cached_data;
    std::span<const u8> data;
    usize size() const { return cached_data.size() + data.size(); }
};

// RPCS3 output_queue: the access unit ring in the game's memory. It knows which parts are used, not where each access
// unit is; positions are offsets into the buffer (RPCS3 keeps pointers; wrap 0 is "not wrapped").
class OutputQueue {
public:
    explicit OutputQueue(std::span<u8> buffer) : buffer_(buffer) {}

    void pop_back(u32 au_size)
    {
        if (au_size > back_) {
            PS_LOG_ERROR(kChan, "access unit queue: pop_back({:#x}) past the start ({:#x}); ignored", au_size, back_);
            return;
        }
        back_ -= au_size;
    }
    void pop_back(const u8* au_addr)
    {
        if (au_addr < buffer_.data() || au_addr >= buffer_.data() + buffer_.size()) {
            PS_LOG_ERROR(kChan, "access unit queue: pop_back at an address outside it; ignored");
            return;
        }
        const u32 at = static_cast<u32>(au_addr - buffer_.data());
        if (at > back_) wrap_ = 0; // nothing behind back any more: unwrap
        back_ = at;
    }
    void pop_front(u32 au_size)
    {
        if (u64{front_} + au_size > buffer_.size()) {
            PS_LOG_ERROR(kChan, "access unit queue: pop_front({:#x}) past the end; ignored", au_size);
            return;
        }
        front_ += au_size;
        if (wrap_ != 0 && wrap_ <= front_) { // front reached where back wrapped: unwrap
            if (wrap_ != front_) PS_LOG_ERROR(kChan, "access unit queue: a release of the wrong size");
            front_ = 0;
            wrap_ = 0;
        }
    }
    const u8* peek_back(u32 au_size) const { return buffer_.data() + back_ - au_size; }
    void clear() { wrap_ = front_ = back_ = 0; }

    void push_unchecked(const Chunk& c)
    {
        std::copy(c.cached_data.begin(), c.cached_data.end(), buffer_.data() + back_);
        std::copy(c.data.begin(), c.data.end(), buffer_.data() + back_ + c.cached_data.size());
        back_ += static_cast<u32>(c.size());
    }
    // False when it does not fit: with nothing behind back, an access unit larger than the maximum (fatal); else the
    // room before front (+ 0x10, RPCS3: "SPU shenanigans probably").
    bool push(const Chunk& c, PamfDemuxer* ctx, void (*fatal)(PamfDemuxer*))
    {
        if (wrap_ == 0) {
            if (c.size() > buffer_.size() - back_) {
                PS_LOG_ERROR(kChan, "Access unit larger than specified maximum access unit size");
                fatal(ctx);
                return false;
            }
        } else if (c.size() + 0x10 > front_ - back_) {
            return false;
        }
        push_unchecked(c);
        return true;
    }
    // LLE always checks the room between back and the end, even when back has wrapped.
    bool prepare_next_au(u32 au_max_size)
    {
        if (buffer_.size() - back_ < au_max_size) {
            if (wrap_ != 0) return false; // cannot wrap twice while units sit behind back
            wrap_ = back_;
            back_ = 0;
        }
        return true;
    }
    usize free_size() const { return wrap_ != 0 ? front_ - back_ : buffer_.size() - back_; }
    usize size() const { return buffer_.size(); }

private:
    std::span<u8> buffer_;
    u32 back_ = 0, front_ = 0, wrap_ = 0;
};

} // namespace

std::pair<StreamTypeIndex, u32> stream_id_to_type_channel(u16 stream_id, u16 private_stream_id)
{
    if ((stream_id & 0xf0) == 0xe0) return {STREAM_TYPE_INDEX_VIDEO, stream_id & 0x0f};
    if ((stream_id & 0xff) != 0xbd) return {STREAM_TYPE_INDEX_INVALID, 0};
    switch (private_stream_id & 0xf0) {
    case 0x40: return {STREAM_TYPE_INDEX_LPCM, private_stream_id & 0x0f};
    case 0x30: return {STREAM_TYPE_INDEX_AC3, private_stream_id & 0x0f};
    case 0x00: return {STREAM_TYPE_INDEX_ATRACX, private_stream_id & 0x0f};
    case 0x20: return {STREAM_TYPE_INDEX_USER_DATA, private_stream_id & 0x0f};
    default: return {STREAM_TYPE_INDEX_INVALID, 0};
    }
}

// RPCS3 elementary_stream: the data section of PES packets cut into access units by each type's parser.
struct PamfDemuxer::ElementaryStream {
    ElementaryStream(u8 channel, u32 au_max_size, PamfDemuxer& ctx, u32 user_data, u8 au_specific_info_size,
        std::span<u8> au_queue_buffer)
        : channel(channel & 0xf)
        , au_max_size(au_max_size == 0xFFFFFFFF || au_max_size > au_queue_buffer.size() ? 0x800 : au_max_size)
        , ctx(ctx)
        , au_specific_info_size(au_specific_info_size)
        , user_data(user_data)
        , au_queue(au_queue_buffer)
    {
    }
    virtual ~ElementaryStream() = default;

    virtual std::pair<u8, u8> stream_id() const = 0;
    // The size of a private stream's header, or kNone for an invalid one.
    virtual u32 parse_stream_header(std::span<const u8> pes_packet_data, s8 pts_dts_flag) = 0;
    // Cuts access units out of `stream` at their delimiters, setting au_chunk: the bytes parsed.
    virtual u32 parse_stream(std::span<const u8> stream) = 0;

    void set_pes_packet_data(std::span<const u8> data)
    {
        if (pes_packet_data) PS_LOG_ERROR(kChan, "a PES packet set while the last is still being processed");
        pes_packet_data = data;
        stream_chunk = data;
    }
    void set_pts(u64 v) { pts = v; }
    void set_dts(u64 v) { dts = v; }
    void set_rap() { rap = true; }

    bool process_pes_packet_data();
    void release_au(u32 au_size) { au_queue.pop_front(au_size); }
    void flush_es();
    void reset_es(u8* au_addr)
    {
        if (!au_addr) {
            reset();
            au_queue.clear();
        } else {
            au_queue.pop_back(au_addr);
        }
    }
    void discard_access_unit()
    {
        au_queue.pop_back(current_au.accumulated_size - static_cast<u32>(au_chunk.size()));
        reset();
        cache.clear();
    }

    template <u32 extra_header_size_unk_mask>
    u32 parse_audio_stream_header(std::span<const u8> data)
    {
        u32 extra = 0; // RPCS3: never seen other than zero
        if (!au_size_unk) { // LLE uses the user data size member as a bool here
            if (data.size() < 4) return kNone;
            extra = be32(data.data()) & extra_header_size_unk_mask;
            au_size_unk = 1;
        }
        return extra + 4;
    }

    const u8 channel;
    const u32 au_max_size;
    u32 au_size_unk = 0; // user data: the current unit's size; other private streams: a bool (LLE)
    std::array<u8, 0x10> au_specific_info_buf{}; // LPCM: the first 0x10 bytes of the PES packet data

    enum class AuState : u8 { none, incomplete, commenced, complete, size_mismatch, m2v_sequence };
    struct AccessUnit {
        AuState state = AuState::none;
        bool rap = false;
        bool timestamps_rap_set = false;
        u8 size_info_offset = 0; // compressed audio: where the size field is after the sync word
        u16 parsed_size = 0;
        u32 accumulated_size = 0;
        u64 pts = ~0ull, dts = ~0ull;
        std::array<u8, 0x10> au_specific_info_buf{};
    } current_au;

    Chunk au_chunk;
    std::vector<u8> cache; // the last three bytes of the PES packet: maybe the start of a delimiter

private:
    PamfDemuxer& ctx;
    enum class State : u8 { initial, pushing_au_queue, notifying_au_found, preparing_for_next_au } state = State::initial;
    const u8 au_specific_info_size;
    const u32 user_data;
    std::optional<std::span<const u8>> pes_packet_data;
    std::span<const u8> stream_chunk;
    u64 pts = ~0ull, dts = ~0ull;
    bool rap = false;
    OutputQueue au_queue;

    bool found()
    {
        const auto [id, priv] = stream_id();
        return ctx.on_au_found(id, priv, user_data,
            {au_queue.peek_back(current_au.accumulated_size), current_au.accumulated_size}, current_au.pts,
            current_au.dts, current_au.rap, au_specific_info_size, current_au.au_specific_info_buf);
    }
    void reset()
    {
        state = State::initial;
        pes_packet_data.reset();
        au_size_unk = 0;
        pts = dts = ~0ull;
        rap = false;
        au_chunk.data = {};
        au_chunk.cached_data.clear();
        current_au = {};
    }
    void set_au_timestamps_rap()
    {
        current_au.pts = pts;
        current_au.dts = dts;
        current_au.rap = rap;
        pts = dts = ~0ull;
        rap = false;
        current_au.timestamps_rap_set = true;
    }
};

void PamfDemuxer::ElementaryStream::flush_es()
{
    if (current_au.accumulated_size != 0) {
        if (au_queue.free_size() < cache.size()) PS_LOG_ERROR(kChan, "flush: no room for the cached bytes");
        else au_queue.push_unchecked({{}, cache});
        current_au.accumulated_size += static_cast<u32>(cache.size());
        found();
    }
    reset();
    const auto [id, priv] = stream_id();
    while (!ctx.on_flush_done(id, priv, user_data)) {
    } // repeated until it is sent (LLE)
}

bool PamfDemuxer::ElementaryStream::process_pes_packet_data()
{
    if (!pes_packet_data) {
        PS_LOG_ERROR(kChan, "process_pes_packet_data() without a PES packet");
        return true;
    }
    for (;;) {
        switch (state) {
        case State::initial:
            if (stream_chunk.empty()) {
                pes_packet_data.reset();
                return true;
            }
            stream_chunk = stream_chunk.subspan(parse_stream(stream_chunk));
            current_au.accumulated_size += static_cast<u32>(au_chunk.size());
            // The beginning of a new access unit: its timestamps and random access flag.
            if (!current_au.timestamps_rap_set &&
                (current_au.state == AuState::commenced || current_au.state == AuState::m2v_sequence ||
                    (current_au.state == AuState::complete && au_chunk.cached_data.empty())))
                set_au_timestamps_rap();
            state = State::pushing_au_queue;
            [[fallthrough]];
        case State::pushing_au_queue:
            if (au_chunk.size() != 0) {
                if (!au_queue.push(au_chunk, &ctx, [](PamfDemuxer* c) { c->on_fatal_error(); })) {
                    ctx.on_au_queue_full();
                    return false;
                }
                au_chunk.data = {};
                au_chunk.cached_data.clear();
            }
            // The distance between two delimiters exceeds the size in the stream's info header: LLE drops the first
            // byte of the PES packet data and starts over, and removes the whole access unit from the queue.
            if (current_au.state == AuState::size_mismatch) {
                pes_packet_data = pes_packet_data->subspan(1);
                stream_chunk = *pes_packet_data;
                au_queue.pop_back(current_au.accumulated_size);
                current_au.accumulated_size = 0;
                state = State::initial;
                continue;
            }
            state = State::notifying_au_found;
            [[fallthrough]];
        case State::notifying_au_found:
            if (current_au.state == AuState::complete && !found()) return false;
            state = State::preparing_for_next_au;
            [[fallthrough]];
        case State::preparing_for_next_au:
            if (current_au.state == AuState::complete) {
                if (!au_queue.prepare_next_au(au_max_size)) {
                    ctx.on_au_queue_full();
                    return false;
                }
                current_au = {};
            }
            state = State::initial;
        }
    }
}

template <bool avc>
struct PamfDemuxer::VideoStream final : ElementaryStream {
    VideoStream(u8 ch, u32 max, PamfDemuxer& ctx, u32 user_data, std::span<u8> buffer)
        : ElementaryStream(ch, max, ctx, user_data, 0, buffer)
    {
    }
    std::pair<u8, u8> stream_id() const override { return {static_cast<u8>(0xe0 | channel), 0}; }
    u32 parse_stream_header(std::span<const u8>, s8) override { return 0; }

    static bool delimiter(u32 code)
    {
        return avc ? code == AVC_AU_DELIMITER
                   : code == M2V_PIC_START || code == M2V_SEQUENCE_HEADER || code == M2V_SEQUENCE_END;
    }
    bool in_au() const { return current_au.state != AuState::none && (avc || current_au.state != AuState::m2v_sequence); }

    u32 parse_stream(std::span<const u8> in) override
    {
        if (in_au()) current_au.state = AuState::incomplete;
        // The cache from the last section and the start of this one (0xff: no false M2V picture start code).
        std::array<u8, 8> buf;
        buf.fill(0xff);
        std::copy(cache.begin(), cache.end(), buf.begin());
        std::copy_n(in.begin(), std::min<usize>(3, in.size()), buf.begin() + cache.size()); // LLE reads three
        usize begin = 0, it = 0;
        s32 cache_idx = 0;
        [&] {
            for (; cache_idx < static_cast<s32>(cache.size()); cache_idx++) {
                const u32 code = be32(buf.data() + cache_idx);
                if (!delimiter(code)) continue;
                if (in_au()) {
                    // The sequence end code belongs to the access unit. LLE moves the stream position, not the cache
                    // index, which corrupts the end of the unit (RPCS3: "not accurate", LLE always adds four).
                    if (!avc && code == M2V_SEQUENCE_END) {
                        PS_LOG_WARN(kChan, "M2V sequence end code in cache");
                        it += std::min<usize>(4, in.size());
                    }
                    current_au.state = AuState::complete;
                    return;
                }
                // A delimiter here with no unit begun: LLE outputs the whole cache, even from cache_idx > 0.
                current_au.state = avc || code == M2V_PIC_START ? AuState::commenced : AuState::m2v_sequence;
            }
            for (; it + 4 <= in.size(); it++) {
                const u32 code = be32(in.data() + it);
                if (!delimiter(code)) continue;
                if (in_au()) {
                    it += !avc && code == M2V_SEQUENCE_END ? 4 : 0;
                    current_au.state = AuState::complete;
                    return;
                }
                begin = avc || current_au.state == AuState::none ? it : begin;
                current_au.state = avc || code == M2V_PIC_START ? AuState::commenced : AuState::m2v_sequence;
            }
        }();
        if (current_au.state != AuState::none) {
            au_chunk.data = in.subspan(begin, it - begin);
            au_chunk.cached_data.insert(au_chunk.cached_data.end(), cache.begin(), cache.begin() + cache_idx);
        }
        cache.erase(cache.begin(), cache.begin() + cache_idx);
        // Not completed: the last bytes may hold the start of a delimiter.
        if (current_au.state != AuState::complete) cache.insert(cache.end(), in.begin() + static_cast<std::ptrdiff_t>(std::min(it, in.size())), in.end());
        return static_cast<u32>(current_au.state != AuState::complete || it > in.size() ? in.size() : it);
    }
};

struct PamfDemuxer::LpcmStream final : ElementaryStream {
    LpcmStream(u8 ch, u32 max, PamfDemuxer& ctx, u32 user_data, std::span<u8> buffer)
        : ElementaryStream(ch, max, ctx, user_data, 3, buffer)
    {
    }
    std::pair<u8, u8> stream_id() const override { return {0xbd, static_cast<u8>(0x40 | channel)}; }
    u32 parse_stream_header(std::span<const u8> data, s8) override
    {
        if (data.size() < 1 + 0x10) return kNone; // not checked on LLE
        std::memcpy(au_specific_info_buf.data(), data.data() + 1, au_specific_info_buf.size());
        return parse_audio_stream_header<0x7ff>(data);
    }
    u32 parse_stream(std::span<const u8> in) override
    {
        if (current_au.state == AuState::none) current_au.au_specific_info_buf = au_specific_info_buf;
        if (au_max_size - current_au.accumulated_size > in.size()) {
            au_chunk.data = in;
            current_au.state = current_au.state == AuState::none ? AuState::commenced : AuState::incomplete;
        } else {
            au_chunk.data = in.first(au_max_size - current_au.accumulated_size);
            current_au.state = AuState::complete;
        }
        return static_cast<u32>(au_chunk.data.size());
    }
};

template <bool ac3>
struct PamfDemuxer::AudioStream final : ElementaryStream {
    static constexpr u16 SYNC_WORD = ac3 ? 0x0b77 : 0x0fd0;
    static constexpr u8 ATRACX_ATS_HEADER_SIZE = 8;
    static constexpr u16 AC3_FRMSIZE_TABLE[3][38] = {
        {0x40, 0x40, 0x50, 0x50, 0x60, 0x60, 0x70, 0x70, 0x80, 0x80, 0xa0, 0xa0, 0xc0, 0xc0, 0xe0, 0xe0, 0x100, 0x100,
            0x140, 0x140, 0x180, 0x180, 0x1c0, 0x1c0, 0x200, 0x200, 0x280, 0x280, 0x300, 0x300, 0x380, 0x380, 0x400,
            0x400, 0x480, 0x480, 0x500, 0x500},
        {0x45, 0x46, 0x57, 0x58, 0x68, 0x69, 0x79, 0x7a, 0x8b, 0x8c, 0xae, 0xaf, 0xd0, 0xd1, 0xf3, 0xf4, 0x116, 0x117,
            0x15c, 0x15d, 0x1a1, 0x1a2, 0x1e7, 0x1e8, 0x22d, 0x22e, 0x2b8, 0x2b9, 0x343, 0x344, 0x3cf, 0x3d0, 0x45a,
            0x45b, 0x4e5, 0x4e6, 0x571, 0x572},
        {0x60, 0x60, 0x78, 0x78, 0x90, 0x90, 0xa8, 0xa8, 0xc0, 0xc0, 0xf0, 0xf0, 0x120, 0x120, 0x150, 0x150, 0x180,
            0x180, 0x1e0, 0x1e0, 0x240, 0x240, 0x2a0, 0x2a0, 0x300, 0x300, 0x3c0, 0x3c0, 0x480, 0x480, 0x540, 0x540,
            0x600, 0x600, 0x6c0, 0x6c0, 0x780, 0x780},
    };

    AudioStream(u8 ch, u32 max, PamfDemuxer& ctx, u32 user_data, std::span<u8> buffer)
        : ElementaryStream(ch, max, ctx, user_data, 0, buffer)
    {
    }
    std::pair<u8, u8> stream_id() const override { return {0xbd, static_cast<u8>((ac3 ? 0x30 : 0x00) | channel)}; }
    u32 parse_stream_header(std::span<const u8> data, s8) override { return parse_audio_stream_header<0xffff>(data); }

    static u16 parse_au_size(u16 data)
    {
        if constexpr (ac3) {
            if (const u8 fscod = data >> 14, frmsizecod = data >> 8 & 0x3f; fscod < 3 && frmsizecod < 38)
                return static_cast<u16>(AC3_FRMSIZE_TABLE[fscod][frmsizecod] * 2);
        } else if ((data & 0x3ff) < 0x200) {
            return static_cast<u16>(((data & 0x3ff) + 1) * 8 + ATRACX_ATS_HEADER_SIZE);
        }
        return 0;
    }

    u32 parse_stream(std::span<const u8> in) override
    {
        if (current_au.state != AuState::none) current_au.state = AuState::incomplete;
        std::array<u8, 8> buf{};
        std::copy(cache.begin(), cache.end(), buf.begin());
        const usize peek = std::min<usize>(1, in.size());
        std::copy_n(in.begin(), peek, buf.begin() + cache.size());
        usize begin = 0, it = 0;
        s32 cache_idx = 0;
        // A delimiter (sync word), or the size field the given number of halfwords after one.
        const auto at = [&](u16 tmp, u32 au_size_here) -> bool {
            if (current_au.size_info_offset != 0) {
                if (--current_au.size_info_offset == 0) current_au.parsed_size = parse_au_size(tmp);
            } else if (tmp == SYNC_WORD) {
                if (current_au.state == AuState::none) {
                    current_au.size_info_offset = ac3 ? 4 : 2;
                    current_au.state = AuState::commenced;
                    return false;
                }
                if (au_size_here >= current_au.parsed_size) {
                    current_au.state = au_size_here == current_au.parsed_size ? AuState::complete : AuState::size_mismatch;
                    return true;
                }
            }
            return false;
        };
        [&] {
            const s32 cache_last = static_cast<s32>(cache.size() + peek) - 2;
            for (; cache_idx <= cache_last; cache_idx++)
                if (at(be16(buf.data() + cache_idx), current_au.accumulated_size + static_cast<u32>(cache_idx))) return;
            // Until the unit reaches the size in its header, the stream need not be searched.
            if (current_au.state != AuState::none && current_au.size_info_offset == 0 &&
                current_au.accumulated_size + cache.size() < current_au.parsed_size) {
                const usize skip = current_au.parsed_size - current_au.accumulated_size - cache.size();
                it += in.size() >= 4 ? std::min(skip, in.size() - 4) : 0;
            }
            for (; it + 4 <= in.size(); it++) { // LLE: sizeof(u32), though the delimiter is two bytes
                const bool none = current_au.state == AuState::none;
                if (at(be16(in.data() + it),
                        static_cast<u32>(current_au.accumulated_size + it - begin + cache.size())))
                    return;
                if (none && current_au.state == AuState::commenced) begin = it;
            }
        }();
        if (current_au.state != AuState::none) {
            au_chunk.data = in.subspan(begin, it - begin);
            au_chunk.cached_data.insert(au_chunk.cached_data.end(), cache.begin(), cache.begin() + cache_idx);
        }
        cache.erase(cache.begin(), cache.begin() + cache_idx);
        if (current_au.state != AuState::complete && current_au.state != AuState::size_mismatch)
            cache.insert(cache.end(), in.begin() + static_cast<std::ptrdiff_t>(it), in.end());
        return static_cast<u32>(current_au.state != AuState::complete ? in.size() : it);
    }
};

struct PamfDemuxer::UserDataStream final : ElementaryStream {
    UserDataStream(u8 ch, u32 max, PamfDemuxer& ctx, u32 user_data, std::span<u8> buffer)
        : ElementaryStream(ch, max, ctx, user_data, 0, buffer)
    {
    }
    std::pair<u8, u8> stream_id() const override { return {0xbd, static_cast<u8>(0x20 | channel)}; }
    u32 parse_stream_header(std::span<const u8> data, s8 pts_dts_flag) override
    {
        if (pts_dts_flag < 0) { // a PTS field
            if (data.size() < 2 + 4) return kNone; // not checked on LLE
            au_size_unk = be32(data.data() + 2) - 4;
            return 10;
        }
        return 2;
    }
    u32 parse_stream(std::span<const u8> in) override
    {
        if (au_size_unk > in.size()) {
            au_chunk.data = in;
            au_size_unk -= static_cast<u32>(in.size());
            current_au.state = AuState::commenced; // always commenced for user data
        } else {
            au_chunk.data = in.first(au_size_unk);
            au_size_unk = 0;
            current_au.state = AuState::complete;
        }
        return static_cast<u32>(in.size()); // always the whole section
    }
};

PamfDemuxer::PamfDemuxer() = default; // out of line: ElementaryStream is complete only here
PamfDemuxer::~PamfDemuxer() = default;

bool PamfDemuxer::enable_es(u32 stream_id, u32 private_stream_id, bool is_avc, std::span<u8> au_queue_buffer,
    u32 au_max_size, bool raw, u32 user_data)
{
    const auto [type_idx, ch] = stream_id_to_type_channel(static_cast<u16>(stream_id), static_cast<u16>(private_stream_id));
    if (type_idx == STREAM_TYPE_INDEX_INVALID || elementary_streams[type_idx][ch]) return false;
    raw_es = raw;
    pack_es_type_idx = static_cast<u8>(type_idx);
    const u8 c = static_cast<u8>(ch);
    auto& slot = elementary_streams[type_idx][ch];
    switch (type_idx) {
    case STREAM_TYPE_INDEX_VIDEO:
        if (is_avc) slot = std::make_unique<VideoStream<true>>(c, au_max_size, *this, user_data, au_queue_buffer);
        else slot = std::make_unique<VideoStream<false>>(c, au_max_size, *this, user_data, au_queue_buffer);
        return true;
    case STREAM_TYPE_INDEX_LPCM: slot = std::make_unique<LpcmStream>(c, au_max_size, *this, user_data, au_queue_buffer); return true;
    case STREAM_TYPE_INDEX_AC3: slot = std::make_unique<AudioStream<true>>(c, au_max_size, *this, user_data, au_queue_buffer); return true;
    case STREAM_TYPE_INDEX_ATRACX: slot = std::make_unique<AudioStream<false>>(c, au_max_size, *this, user_data, au_queue_buffer); return true;
    case STREAM_TYPE_INDEX_USER_DATA: slot = std::make_unique<UserDataStream>(c, au_max_size, *this, user_data, au_queue_buffer); return true;
    default: return false;
    }
}

bool PamfDemuxer::disable_es(u32 stream_id, u32 private_stream_id)
{
    const auto [type_idx, ch] = stream_id_to_type_channel(static_cast<u16>(stream_id), static_cast<u16>(private_stream_id));
    if (type_idx == STREAM_TYPE_INDEX_INVALID || !elementary_streams[type_idx][ch]) return false;
    elementary_streams[type_idx][ch].reset();
    return true;
}

bool PamfDemuxer::release_au(u32 stream_id, u32 private_stream_id, u32 au_size) const
{
    const auto [type_idx, ch] = stream_id_to_type_channel(static_cast<u16>(stream_id), static_cast<u16>(private_stream_id));
    if (type_idx == STREAM_TYPE_INDEX_INVALID || !elementary_streams[type_idx][ch]) return false;
    elementary_streams[type_idx][ch]->release_au(au_size);
    return true;
}

bool PamfDemuxer::flush_es(u32 stream_id, u32 private_stream_id)
{
    const auto [type_idx, ch] = stream_id_to_type_channel(static_cast<u16>(stream_id), static_cast<u16>(private_stream_id));
    if (type_idx == STREAM_TYPE_INDEX_INVALID || !elementary_streams[type_idx][ch]) return false;
    state = State::initial;
    elementary_streams[type_idx][ch]->flush_es();
    return true;
}

void PamfDemuxer::set_stream(std::span<const u8> s, bool continuity)
{
    if (!continuity)
        for (auto& type : elementary_streams)
            for (auto& es : type)
                if (es) es->discard_access_unit();
    state = State::initial;
    // Not checked on LLE, which would read stale memory: valid PAMF streams are whole packs.
    if ((s.size() & 0x7ff) != 0) PS_LOG_WARN(kChan, "Invalid stream size");
    stream = s;
    demux_done_notified = false;
}

void PamfDemuxer::reset_stream()
{
    for (auto& type : elementary_streams)
        for (auto& es : type)
            if (es) es->discard_access_unit();
    state = State::initial;
    stream.reset();
}

bool PamfDemuxer::reset_es(u32 stream_id, u32 private_stream_id, u8* au_addr)
{
    const auto [type_idx, ch] = stream_id_to_type_channel(static_cast<u16>(stream_id), static_cast<u16>(private_stream_id));
    if (type_idx == STREAM_TYPE_INDEX_INVALID || !elementary_streams[type_idx][ch]) return false;
    if (!au_addr) state = State::initial;
    elementary_streams[type_idx][ch]->reset_es(au_addr);
    return true;
}

u32 PamfDemuxer::enabled_es_count() const
{
    u32 n = 0;
    for (const auto& type : elementary_streams)
        for (const auto& es : type) n += es != nullptr;
    return n;
}

bool PamfDemuxer::process_next_pack()
{
    if (!stream) {
        demux_done_notified = demux_done_notified || on_demux_done();
        return true;
    }
    switch (state) {
    case State::initial: {
        // The next pack start code or program end code.
        std::span<const u8> pack;
        for (;;) {
            if (stream->size() < PACK_STUFFING_LENGTH_OFFSET + 1) {
                stream.reset();
                demux_done_notified = on_demux_done();
                return true;
            }
            pack = stream->first(std::min<usize>(stream->size(), PACK_SIZE));
            stream = stream->subspan(std::min<usize>(stream->size(), PACK_SIZE));
            // A raw elementary stream: no MPEG-PS parsing, straight to the stream's parser.
            if (raw_es) {
                if (pack_es_type_idx < 5 && elementary_streams[pack_es_type_idx][0])
                    elementary_streams[pack_es_type_idx][0]->set_pes_packet_data(pack);
                pack_es_channel = 0;
                state = State::elementary_stream;
                return true;
            }
            // LLE searches the whole section for a start code but reads from its beginning (RPCS3).
            const u32 code = be32(pack.data());
            if (code == PACK_START) break;
            if (code == PROG_END) {
                if (!on_prog_end()) state = State::prog_end;
                return true;
            }
            PS_LOG_WARN(kChan, "No start code found at the beginning of the current section");
        }
        // The pack header.
        const u8 pack_stuffing_length = pack[PACK_STUFFING_LENGTH_OFFSET] & 7;
        if (PACK_STUFFING_LENGTH_OFFSET + 1 + pack_stuffing_length + PES_HEADER_DATA_LENGTH_OFFSET + 1 > pack.size()) {
            PS_LOG_ERROR(kChan, "Invalid pack stuffing length");
            return false;
        }
        std::span<const u8> pes = pack.subspan(PACK_STUFFING_LENGTH_OFFSET + 1 + pack_stuffing_length);
        if ((be32(pes.data()) >> 8) != PACKET_START_CODE_PREFIX) {
            PS_LOG_ERROR(kChan, "Invalid start code after pack header");
            return false;
        }
        // A system header, and the private stream 2 that may follow it.
        if (be32(pes.data()) == SYSTEM_HEADER) {
            const u32 system_header_length = be16(pes.data() + PES_PACKET_LENGTH_OFFSET) + PES_PACKET_LENGTH_OFFSET + 2;
            if (system_header_length + PES_HEADER_DATA_LENGTH_OFFSET + 1 > pes.size()) {
                PS_LOG_ERROR(kChan, "Invalid system header length");
                return false;
            }
            pes = pes.subspan(system_header_length);
            // The SPU reads the next start code 16-byte aligned (always so in valid PAMF streams).
            const u32 offset_low = static_cast<u32>(pes.data() - pack.data()) & 0xf;
            pes = std::span<const u8>(pes.data() - offset_low, pes.size() + offset_low);
            const u32 code = be32(pes.data());
            if (code >> 8 != PACKET_START_CODE_PREFIX) {
                PS_LOG_ERROR(kChan, "Invalid start code after system header");
                return false;
            }
            if (code == PRIVATE_STREAM_2) {
                // Its first two bytes name a video stream whose next access unit is a random access point.
                const u32 length = be16(pes.data() + PES_PACKET_LENGTH_OFFSET) + PES_PACKET_LENGTH_OFFSET + 2;
                if (length + PES_HEADER_DATA_LENGTH_OFFSET + 1 > pes.size()) {
                    PS_LOG_ERROR(kChan, "Invalid private stream 2 length");
                    return false;
                }
                if (const u8 ch = be16(pes.data() + PES_PACKET_LENGTH_OFFSET + 2) & 0xf; elementary_streams[0][ch])
                    elementary_streams[0][ch]->set_rap();
                pes = pes.subspan(length);
            }
        }
        // The PES packet: LLE parses only the first of a pack.
        const u32 start_code = be32(pes.data());
        if (start_code >> 8 != PACKET_START_CODE_PREFIX) {
            PS_LOG_ERROR(kChan, "Invalid start code");
            return false;
        }
        const u32 pes_packet_length = be16(pes.data() + PES_PACKET_LENGTH_OFFSET) + PES_PACKET_LENGTH_OFFSET + 2;
        const u32 pes_header_data_length = pes[PES_HEADER_DATA_LENGTH_OFFSET] + PES_HEADER_DATA_LENGTH_OFFSET + 1u;
        if (pes_packet_length > pes.size() || pes_packet_length <= pes_header_data_length) {
            PS_LOG_ERROR(kChan, "Invalid pes packet length");
            return false;
        }
        const std::span<const u8> data = pes.subspan(pes_header_data_length, pes_packet_length - pes_header_data_length);
        const auto [type_idx, ch] = stream_id_to_type_channel(static_cast<u16>(start_code), data[0]);
        if (type_idx == STREAM_TYPE_INDEX_INVALID) {
            PS_LOG_ERROR(kChan, "Invalid stream type");
            return false;
        }
        pack_es_type_idx = static_cast<u8>(type_idx);
        pack_es_channel = static_cast<u8>(ch);
        if (const auto& es = elementary_streams[type_idx][ch]) {
            const s8 pts_dts_flag = static_cast<s8>(pes[PTS_DTS_FLAG_OFFSET]);
            // The timestamps are sign-extended from s32 on LLE ("they probably forgot about integer promotion"); bit
            // 32 is lost.
            const auto timestamp = [&](u32 at) {
                const u32 a = (pes[at] >> 1) & 0x7f, b = (be16(pes.data() + at + 1) >> 1) & 0x7fff,
                          c = (be16(pes.data() + at + 3) >> 1) & 0x7fff;
                return static_cast<u64>(static_cast<s64>(static_cast<s32>(a << 30 | b << 15 | c)));
            };
            if (pts_dts_flag < 0) es->set_pts(timestamp(9));
            if (pts_dts_flag & 0x40) es->set_dts(timestamp(14));
            const u64 header_size = es->parse_stream_header(data, pts_dts_flag);
            if (header_size > data.size()) {
                PS_LOG_ERROR(kChan, "Invalid stream header size");
                return false;
            }
            es->set_pes_packet_data(data.subspan(header_size));
        }
        state = State::elementary_stream;
        [[fallthrough]];
    }
    case State::elementary_stream: {
        const auto& es = pack_es_type_idx < 5 ? elementary_streams[pack_es_type_idx][pack_es_channel] : elementary_streams[0][0];
        if (pack_es_type_idx >= 5 || !es || es->process_pes_packet_data()) state = State::initial;
        return true;
    }
    case State::prog_end:
        if (on_prog_end()) state = State::initial;
        return true;
    }
    return true;
}

} // namespace pspocket::hle::dmux
