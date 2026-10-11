#pragma once

// pspocket's own cellAdec cores on FFmpeg for the codecs whose firmware core serves first (D-577): MP3 and MP3 Surround
// (adec_mp3.cpp), MPEG audio layers I and II (adec_mpeg.cpp), MPEG-4 and MPEG-2 AAC (adec_aac.cpp), AC-3 and Dolby
// Digital Plus (adec_ac3.cpp), ATRAC3 and ATRAC3 multichannel (adec_atrac3.cpp), TrueHD (adec_truehd.cpp), DTS
// (adec_dts.cpp) and WMA (adec_wma.cpp). The front end (cellAdec.cpp) opens one only where no firmware module gives the
// codec's core; the cores' own modules' tables are pspocket's where none is installed (adec_tables.cpp).
//
// The common part (adec_ffmpeg.cpp) is shaped as adec_atracx.cpp, RPCS3's port of libatxdec's PPU side
// (rpcs3/Emu/Cell/Modules/cellAtracXdec.cpp): the core's context in the game's memory at its handle, a command queue of
// four, lv2 mutexes and conditions, and a PPU thread of the decoder's ("HLE MP3 decoder", ...) that decodes one access
// unit at a time once the game took the previous one's PCM; after every decode AUDONE, then PCMOUT, an error notified
// first. RPCS3 runs Sony's modules for these codecs and has no core of its own: each codec's parameters and information
// block are RPCS3's structures (rpcs3/Emu/Cell/Modules/cellAdec.h), and what fills them is written in the codec's file.
//
// FfContext (0x200 bytes at the core's handle, pspocket's layout: Sony's is not known), then the output buffer:
//   +0x000 the command queue: 4 commands of 0x40 (type, pcm_handle, au_start, au_size, then the start's parameters)
//   +0x100 front, back, size; +0x10C the queue mutex and its "not empty" cond; +0x114 the output mutex, its "consumed"
//   cond and output_locked; +0x120 the run_thread mutex and flag; +0x128 the four notifications (function, argument);
//   +0x148 the thread (u64); +0x150 its TLS block; +0x154 the cellAdec type; +0x158 the output's address; +0x15C the
//   PCM waiting to be taken (bytes); +0x160 the last PTS out (upper, lower) and its frame's length, +0x16C the length of
//   the frame being given out (90 kHz ticks, for the cores' own PTS correction); +0x170 the sequence's parameters;
//   +0x1A0 whether the notifications are guest functions (opened through the exported table, D-591).

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "adec.hpp"
#include "pspocket/core/types.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <functional>
#endif

namespace pspocket::hle::adec::ffcore {

namespace fx {
constexpr u32 kCmdSize = 0x40, c_type = 0x00, c_pcm_handle = 0x04, c_au_start = 0x08, c_au_size = 0x0C, c_param = 0x10,
              kParamMax = 0x30;
constexpr u32 cmd_front = 0x100, cmd_back = 0x104, cmd_size = 0x108, queue_mutex = 0x10C, queue_not_empty = 0x110,
              output_mutex = 0x114, output_consumed = 0x118, output_locked = 0x11C, run_thread_mutex = 0x120,
              run_thread = 0x124, notify = 0x128, thread_id = 0x148, tls = 0x150, type = 0x154, output = 0x158,
              pcm_size = 0x15C, prev_pts = 0x160, prev_duration = 0x168, duration = 0x16C, param = 0x170,
              notify_guest = 0x1A0, size = 0x200;
} // namespace fx

// What a codec made of one access unit.
struct Decoded {
    u32 size = 0;     // the PCM's bytes in the output
    s32 error = 0;    // CELL_OK, or the codec's error: notified first, then given with the PCM (none, or what came)
    s8 pts_type = -1; // the correctPtsValueType the front end gets (adec.hpp)
    u32 duration = 0; // the frame's length in 90 kHz ticks, for the cores' own PTS correction (AdecCore::correct_pts)
};

// One codec's decoding, on the decoder's thread: a sequence's start, then its access units.
class Codec {
public:
    virtual ~Codec() = default;
    // A sequence with the game's parameters (Spec::param_size bytes, as the game wrote them; Spec::check passed them on
    // the game's thread). CELL_OK, or an error (logged): the sequence's decodes then fail with the codec's error.
    virtual s32 start(const u8* param) = 0;
    // The access unit at `au` (`size` bytes of guest memory, bytes_needed(size) of them readable) into the output at
    // `out` (Spec::output_bytes) and the information block at `bsi` (Spec::bsi_size bytes, zeroed).
    virtual Decoded decode(u8* base, u32 au, u32 size, u32 out, u32 bsi) = 0;
    // The bytes from the unit's start the codec reads: its size (ATRAC3 multichannel reads its tracks' blocks, which
    // may run past the size its caller gives, D-621).
    virtual u32 bytes_needed(u32 size) const { return size; }
};

// A codec's constants and parts.
struct Spec {
    s32 type;              // the cellAdec type
    std::string_view name; // for logs and the decoder thread's name ("HLE <name> decoder")
    u32 param_size;        // the parameters StartSeq takes, copied into its command (at most fx::kParamMax)
    u32 bsi_size;          // the information block (getBsiInfoSize)
    u32 output_bytes;      // the output buffer: the largest access unit's PCM
    u32 version;           // getVersion
    u32 busy;              // the codec's error for a full command queue
    // StartSeq's check of the parameters on the game's thread: CELL_OK or the error cellAdecStartSeq gives.
    s32 (*check)(const u8* param);
    std::unique_ptr<Codec> (*make)();
};

#if PSPOCKET_HAVE_FFMPEG

// Each codec file's Spec.
extern const Spec kMp3, kMpegL1, kMpegL2, kM4Aac, kM4Aac2ch, kM4Aac2chMod, kAc3, kAtrac3;
extern const Spec kDdp, kM2Aac, kMp3s, kAtrac3Multi;                                 // D-621
extern const Spec kTrueHd, kDts, kDtsHd, kDtsHdCore, kDtsLbr, kWma, kWmaPro, kWmaLsl; // D-622

// The Spec of a cellAdec type, or null.
const Spec* spec_of(s32 type);

// ---- what the codecs share

// Decoded audio: planar floats per channel, with FFmpeg's channel of each (AVChannel) and the rate; `bits` the
// resolution FFmpeg says it decoded (bits_per_raw_sample: a lossless stream's), 0 where it says none.
struct Planar {
    std::vector<std::vector<float>> ch;
    std::vector<int> ids;
    u32 samples = 0;
    u32 rate = 0;
    u32 bits = 0;
    u32 channels() const { return static_cast<u32>(ch.size()); }
    void clear()
    {
        ch.clear();
        ids.clear();
        samples = 0;
        rate = 0;
        bits = 0;
    }
};

// One of FFmpeg's decoders, opened for a sequence.
class FfDecoder {
public:
    FfDecoder() = default;
    ~FfDecoder() { close(); }
    FfDecoder(const FfDecoder&) = delete;
    FfDecoder& operator=(const FfDecoder&) = delete;

    // Opens FFmpeg's decoder of that name ("mp3float", ...) afresh (`name` for the logs); `setup` fills the context
    // (channels, rate, extradata, options) before avcodec_open2. False, logged, when FFmpeg refuses.
    bool open(const char* decoder, std::string_view name, const std::function<void(AVCodecContext*)>& setup = {});
    void close();
    bool is_open() const { return av_ != nullptr; }
    // One packet (copied, with FFmpeg's padding): every frame it gives appended to `out`, at most `max_samples` samples
    // per channel (more is left out, logged). FFmpeg's planar or interleaved floats, 16- and 32-bit samples become
    // planar floats (exactly, for samples of up to 24 bits: the lossless codecs' 16 and 24, D-620). 0, or FFmpeg's
    // error for the packet.
    int decode(const u8* data, u32 size, Planar& out, u32 max_samples);
    AVCodecContext* context() { return av_; }

private:
    static bool convertible(int format);
    static float sample_of(const AVFrame* f, u32 c, u32 i, u32 channels);

    AVCodecContext* av_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::vector<u8> buffer_;
    std::string_view name_;
};

// The PCM formats the cores write: big-endian and interleaved. S24 is a 24-bit sample in a 32-bit word's low three
// bytes, as libatxdec writes it (adec_atracx.cpp).
enum class Format : u8 { F32, S16, S24, S32 };
constexpr u32 bytes_of(Format f) { return f == Format::S16 ? 2 : 4; }

// Writes `in` interleaved to the guest at `out`: output channel c of each sample from `in`'s channel map[c] (-1:
// silence), times `gain`, converted as libatxdec converts (adec_atracx.cpp: floats held below 1, integers floored and
// held in range). Returns the bytes written.
u32 write_pcm(u8* base, u32 out, const Planar& in, std::span<const int> map, Format fmt, float gain = 1.0f);

// The identity map for `n` channels.
std::vector<int> straight(u32 n);

// A frame of `samples` at `rate` in 90 kHz ticks, rounded (as the front end's fixed steps are: 2351 for 1152 samples
// at 44.1 kHz, 4180 for 2048).
constexpr u32 ticks(u32 samples, u32 rate) { return rate ? static_cast<u32>((u64{samples} * 90000 + rate / 2) / rate) : 0; }

// A once-per-process log line for a codec's quirk (`key` names it).
void log_once(std::string_view key, std::string_view line);

// ---- the downmixes the cores share (D-620)

// To two channels: each side its front at 1, the centre and its surrounds at 0.7071 (a back centre at 0.5 to both), the
// LFE left out, each side over its coefficients' sum so a full-scale input cannot clip (the AAC cores', D-578). A
// stream of two channels or fewer is left as it is.
void stereo_downmix(Planar& p);
// The output a CellAdecChannel `channel_number` asks for, as the MPEG audio cores read it (D-577): MONO one channel (a
// stereo downmix's two averaged), STEREO two (the stereo downmix; a mono stream in both), anything else the stream's
// own channels.
void apply_channel_number(Planar& p, u32 channel_number);
// Past `max` channels, the first `max` kept (logged once): the cores' outputs hold that many.
void limit_channels(Planar& p, u32 max, std::string_view name);
// The channels of `p` in order as WAVE_FORMAT_EXTENSIBLE's mask names them (front left 0x1, right 0x2, centre 0x4, LFE
// 0x8, back left 0x10, right 0x20, ..., side left 0x200, right 0x400: FFmpeg's own channel numbers as bits), a channel
// past them none.
u32 channel_mask(const Planar& p);

// The information block of the cores whose own Sony's is not known and RPCS3 has none of (TrueHD, the DTS cores, the
// WMA cores, D-622): pspocket's layout, 0x40 bytes of big-endian words. A common head, then each codec's own words.
namespace ginfo {
constexpr u32 sampling_freq = 0x00, // the PCM's rate (Hz)
    number_of_channels = 0x04,      // the PCM's channels
    channel_mask = 0x08,            // the PCM's channels in order (channel_mask above)
    samples = 0x0C,                 // per channel, in this PCM
    stream_channels = 0x10,         // the stream's own channels, before a downmix
    stream_channel_mask = 0x14,     // and their mask
    bits_per_sample = 0x18,         // the stream's resolution where it codes one (else 0)
    bit_rate = 0x1C,                // bits per second where the stream says (else 0)
    is_bsi_valid = 0x20,            // 1 once a unit decoded
    error_code = 0x24,              // the unit's error, else 0
    own = 0x28,                     // the codec's own words, to 0x40
    size = 0x40;
} // namespace ginfo
// The common head for `pcm` (the output) and the stream's own channels (before a downmix).
void write_ginfo(u8* base, u32 bsi, const Planar& pcm, u32 stream_channels, u32 stream_mask, u32 bits, u32 bit_rate);

// The parameters of TrueHD's and the DTS cores (D-622), Sony's not being known: laid out as MPEG audio's
// CellAdecParamMpmc (D-577), libadec's one multichannel structure without a codec's own fields: channelNumber (the
// output, apply_channel_number), downmix and lfeUpSample (flags, kept as the MPEG audio cores keep them: the LFE is
// always decoded at the full rate). StartSeq's check: a layout up to 3/4 with the LFE and flags of 0 or 1, else
// libadec's ARG.
namespace lprm {
constexpr u32 channel_number = 0x0, downmix = 0x4, lfe_up_sample = 0x8, size = 0xC;
}
s32 check_layout_params(const u8* p, std::string_view name);

// Bits read most significant first from host bytes; past the end reads zeros (and `overrun` says so).
class Bits {
public:
    Bits(const u8* data, u32 size) : data_(data), size_(size) {}
    u32 get(u32 n)
    {
        u32 v = 0;
        for (u32 i = 0; i < n; i++, pos_++) {
            const u32 byte = pos_ / 8;
            if (byte >= size_) {
                overrun = true;
                v <<= 1;
                continue;
            }
            v = v << 1 | ((data_[byte] >> (7 - pos_ % 8)) & 1u);
        }
        return v;
    }
    bool flag() { return get(1) != 0; }
    void skip(u32 n) { pos_ += n; }
    u32 pos() const { return pos_; }
    bool overrun = false;

private:
    const u8* data_;
    u32 size_;
    u32 pos_ = 0;
};

// An MPEG audio frame header (ISO/IEC 11172-3 2.4.1.3; 13818-3 for the lower rates).
struct MpegHeader {
    u32 word = 0;
    bool sync = false;    // the 11-bit sync (MPEG-2.5's included; 12 bits for MPEG-1 and 2)
    u32 version = 0;      // the header's two bits: 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5, 1 reserved
    u32 layer = 0;        // 1, 2 or 3; 0 for the reserved code
    bool crc = false;     // a CRC follows the header (protection_bit 0)
    u32 bitrate_index = 0, rate_index = 0;
    bool padding = false, private_bit = false;
    u32 mode = 0, mode_extension = 0; // mode: 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
    bool copyright = false, original = false;
    u32 emphasis = 0;
    u32 bitrate = 0;     // kbit/s; 0 for free format or a bad index
    u32 rate = 0;        // Hz; 0 for a reserved index
    u32 frame_bytes = 0; // the whole frame with its padding; 0 when the fields do not give it
    u32 samples = 0;     // per channel
    u32 channels() const { return mode == 3 ? 1 : 2; }
};
MpegHeader parse_mpeg_header(u32 word);

// CRC-16 as MPEG audio protects its headers (ISO/IEC 11172-3 2.4.3.1: x^16 + x^15 + x^2 + 1, from all ones), over
// `bits` bits from `data`, continuing `crc`.
u16 mpeg_crc(const u8* data, u32 bits, u16 crc = 0xFFFF);

#endif

} // namespace pspocket::hle::adec::ffcore
