#pragma once

// The PAMF demultiplexer itself: what libdmuxpamf's SPU program does (D-194). It reads an MPEG program stream in
// 2048-byte packs, finds the PES packets of the elementary streams enabled, cuts them into access units (AVC and M2V
// pictures, LPCM blocks, AC3 and ATRAC3plus frames, user data) and writes each into its stream's access unit queue,
// a ring in the game's memory, then reports it. No lv2, no threads: cellDmuxPamf.cpp drives it from the thread that
// stands in for the SPU, and the tests drive it directly.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellDmuxPamf.cpp/.h, dmux_pamf_base, which reproduces libdmuxpamf's SPU code
// down to its quirks (marked "LLE" there and here).

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::dmux {

enum StreamTypeIndex : s8 {
    STREAM_TYPE_INDEX_INVALID = -1,
    STREAM_TYPE_INDEX_VIDEO,
    STREAM_TYPE_INDEX_LPCM,
    STREAM_TYPE_INDEX_AC3,
    STREAM_TYPE_INDEX_ATRACX,
    STREAM_TYPE_INDEX_USER_DATA,
};

// RPCS3 dmuxPamfStreamIdToTypeChannel.
std::pair<StreamTypeIndex, u32> stream_id_to_type_channel(u16 stream_id, u16 private_stream_id);

class PamfDemuxer {
public:
    PamfDemuxer();
    virtual ~PamfDemuxer();

    bool enable_es(u32 stream_id, u32 private_stream_id, bool is_avc, std::span<u8> au_queue_buffer, u32 au_max_size,
        bool raw_es, u32 user_data);
    bool disable_es(u32 stream_id, u32 private_stream_id);
    bool release_au(u32 stream_id, u32 private_stream_id, u32 au_size) const;
    bool flush_es(u32 stream_id, u32 private_stream_id);
    void set_stream(std::span<const u8> stream, bool continuity);
    void reset_stream();
    bool reset_es(u32 stream_id, u32 private_stream_id, u8* au_addr);
    // Processes the next pack (or the next part of its PES packet); false on a stream the SPU program would misread.
    bool process_next_pack();

    [[nodiscard]] bool has_work() const { return stream.has_value() || !demux_done_notified; }
    [[nodiscard]] u32 enabled_es_count() const;

protected:
    // What the SPU program sends the PPU thread. A false return means the event queue was full: the demuxer stops
    // where it is and tries again on its next call (except on_flush_done, retried at once, and on_fatal_error).
    virtual bool on_au_found(u8 stream_id, u8 private_stream_id, u32 user_data, std::span<const u8> au, u64 pts, u64 dts,
        bool rap, u8 au_specific_info_size, std::array<u8, 0x10> au_specific_info) = 0;
    virtual bool on_demux_done() = 0;
    virtual void on_fatal_error() = 0;
    virtual bool on_flush_done(u8 stream_id, u8 private_stream_id, u32 user_data) = 0;
    virtual bool on_prog_end() = 0;
    virtual void on_au_queue_full() = 0; // a global flag on LLE, never reported to the PPU thread

private:
    struct ElementaryStream;
    template <bool avc>
    struct VideoStream;
    struct LpcmStream;
    template <bool ac3>
    struct AudioStream;
    struct UserDataStream;

    enum class State : u8 { initial, elementary_stream, prog_end };
    State state = State::initial;
    bool demux_done_notified = true; // the game was told the stream is consumed
    u8 pack_es_type_idx = 0xff;      // the elementary stream type of the current pack
    u8 pack_es_channel = 0;
    bool raw_es = false; // the input is one elementary stream, not an MPEG program stream
    std::optional<std::span<const u8>> stream;
    std::unique_ptr<ElementaryStream> elementary_streams[5][0x10]; // one per type and channel
};

} // namespace pspocket::hle::dmux
