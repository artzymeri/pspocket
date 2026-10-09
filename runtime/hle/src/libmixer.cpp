// libmixer: the surround mixer (cellSurMixer), its channel strips fed through the AAN (cellAANAddData) and its sample
// players (cellSSPlayer), pspocket's own (D-563). Reference: RPCS3 rpcs3/Emu/Cell/Modules/libmixer.cpp and libmixer.h
// (the structures, the error codes, the AAN handle and port numbers, the mixing of each strip type and of the
// players); no Sony module was at hand to compare with, so the library is marked firmware-first: a game gets the
// firmware's libmixer when the system software is installed, this one when it is not.
//
// The mixer is a client of pspocket's cellAudio, as libmixer is of libaudio: cellSurMixerCreate opens an 8-channel
// port of 16 blocks and a notify event queue, and starts the mixer's PPU thread at the game's priority. For every
// period cellAudio plays the thread mixes one block of 256 samples: the game's notify callback first (where it feeds
// the strips and buses for that block), then the sample players connected to the mixer, the channel strips' levels and
// mutes, the total level; the block goes two periods ahead of the port's read index. Its tag counts the blocks mixed.
// Not emulated, as in RPCS3: reverb (the reverb buses and send levels are accepted and dropped), the strips' and
// players' positions (a mono strip or player goes to left and right alike), the attack modes, the beep.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {

namespace {

constexpr std::string_view kModule = "libmixer";
constexpr const char* kChan = "libmixer";

enum : u32 {
    CELL_LIBMIXER_ERROR_NOT_INITIALIZED = 0x80310002,
    CELL_LIBMIXER_ERROR_INVALID_PARAMATER = 0x80310003,
    CELL_LIBMIXER_ERROR_NO_MEMORY = 0x80310005,
    CELL_LIBMIXER_ERROR_ALREADY_EXIST = 0x80310006,
    CELL_LIBMIXER_ERROR_FULL = 0x80310007,
    CELL_LIBMIXER_ERROR_NOT_EXIST = 0x80310008,
    CELL_LIBMIXER_ERROR_TYPE_MISMATCH = 0x80310009,
    CELL_LIBMIXER_ERROR_NOT_FOUND = 0x8031000A,
};

s32 error(u32 code) { return static_cast<s32>(code); }

// The strip types (CELL_SURMIXER_CHSTRIP_TYPE1A, 2A, 6A, 8A: 1 to 4), their channels, the surround buses and the
// reverb ones.
constexpr u32 kType1 = 1, kType2 = 2, kType8 = 4;
constexpr u32 kChannelsOf[5] = {0, 1, 2, 6, 8};
constexpr u32 kBuses = 8, kReverbBuses = 4, kSamples = 256, kPortBlocks = 16, kLead = 2;
constexpr u32 kAanHandle = 0x11111111; // RPCS3's
constexpr u32 kAudioNotInit = 0x80310703; // CELL_AUDIO_ERROR_NOT_INIT: cellAudioInit not called

// cellSurMixerSetParameter's parameters.
constexpr u32 kParamTotalLevelLinear = 8, kParamReverbLevelLinear = 9, kParamTotalMute = 12, kParamTotalLevel = 40,
              kParamReverbLevel = 41;
// A strip's parameters (cellSurMixerChStripSetParameter): level, expression level, center level, reverb send level,
// mute, reverb send position, position (the last three mono and stereo strips only).
constexpr u32 kStripLevel = 0, kStripExpression = 1, kStripMute = 4;

// cellSSPlayer: loop modes, states.
constexpr u32 CELL_SSPLAYER_ONESHOT = 0, CELL_SSPLAYER_ONESHOT_CONT = 2, CELL_SSPLAYER_LOOP_ON = 16;
constexpr u32 CELL_SSPLAYER_STATE_ERROR = 0xFFFFFFFF, CELL_SSPLAYER_STATE_OFF = 0x00, CELL_SSPLAYER_STATE_ON = 0x20;

struct Strip {
    f32 level = 1.0f, expression = 1.0f;
    bool mute = false;
};

// RPCS3's SSPlayer.
struct Player {
    bool created = false, connected = false, active = false;
    u32 channels = 0;
    u32 addr = 0, samples = 0, loop_start = 0, loop_mode = 0, position = 0;
    f32 level = 0.0f, speed = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Mixer {
    std::mutex mutex;
    bool created = false;
    s32 priority = 0;
    std::array<std::vector<Strip>, 5> strips; // by type 1..4
    u32 port = 0, port_addr = 0, read_index_addr = 0;
    u32 queue = 0;
    u64 queue_key = 0;
    u64 thread = 0;
    std::atomic<bool> stop{false};
    bool started = false;
    u32 cb = 0, cb_arg = 0;
    std::array<f32, kBuses * kSamples> mix{};
    u64 mixcount = 0;
    u64 start_time = 0; // guest system time (us) at the first start
    f32 total_level = 1.0f, reverb_level = 1.0f;
    bool total_mute = false;
    std::vector<Player> players;
    bool reverb_logged = false, beep_logged = false, param_logged = false;
};

Mixer& mixer() { return HleProcess::current()->state<Mixer>(); }

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// Whether [addr, addr + size) is committed guest memory, page by page (guest_readable checks one host page at most).
bool readable(PPUContext& ctx, u32 addr, u32 size)
{
    if (!addr) return false;
    lv2::PpuThread& t = thread_of(ctx);
    for (u32 at = 0; at < size; at += 0x1000)
        if (!guest_readable(t, addr + at, std::min<u32>(size - at, 0x1000))) return false;
    return true;
}

// A function of another library pspocket serves (cellAudio, sysPrxForUser), called as libmixer calls it: r3.
u64 lib_call(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = registry().find(module, recomp::nid_of(name));
    if (!f) PS_FATAL(kChan, "{}::{}, which libmixer calls, is not pspocket's", module, name);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 3;
    for (const u64 a : args) ctx.gpr[i++] = a;
    f->fn(ctx, gb(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

s32 audio_call(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return static_cast<s32>(static_cast<u32>(lib_call(ctx, "cellAudio", name, args)));
}

f32 level_from_db(f32 db) { return std::pow(10.0f, db / 20.0f); }

// ------------------------------------------------------------------------------------------------- mixing

// One sample player's 256 samples into the mix (RPCS3's: 16-bit little-endian samples, the speed's crude resampling,
// the loop modes at the end of the wave); under the mixer's lock.
void mix_player(u8* base, Mixer& m, Player& p)
{
    const f32 speed = std::fabs(p.speed);
    f32 fpos = 0.0f;
    for (s32 i = 0; i < static_cast<s32>(kSamples); i++) {
        if (!p.active) break;
        const u32 pos = p.position;
        if (pos >= p.samples) { // a start or loop offset past the wave's end: stopped rather than read past it
            p.active = false;
            break;
        }
        s32 inc = p.speed > 0.0f ? 1 : p.speed < 0.0f ? -1 : 0;
        const s32 shift = i - static_cast<s32>(fpos);
        if (shift > 0) { // slower: this sample again
            inc = 0;
            fpos += 1.0f;
            fpos += speed;
        } else if (shift < 0) { // faster: two samples into one
            i--;
            fpos -= 1.0f;
        } else {
            fpos += speed;
        }
        p.position += static_cast<u32>(inc);
        const auto sample = [&](u32 index) {
            const u32 a = p.addr + 2 * index;
            return static_cast<f32>(static_cast<s16>(base[a] | (base[a + 1] << 8))) / 32768.0f * p.level;
        };
        f32 left = 0.0f, right = 0.0f;
        if (p.channels == 1) left = right = sample(pos);
        else if (p.channels == 2) {
            left = sample(pos * 2);
            right = sample(pos * 2 + 1);
        }
        if (p.connected && i >= 0) {
            m.mix[static_cast<u32>(i) * kBuses + 0] += left;
            m.mix[static_cast<u32>(i) * kBuses + 1] += right;
        }
        if ((p.position == p.samples && p.speed > 0.0f) || (p.position == 0xFFFFFFFFu && p.speed < 0.0f)) {
            if (p.loop_mode == CELL_SSPLAYER_LOOP_ON) {
                p.position = p.loop_start;
            } else if (p.loop_mode == CELL_SSPLAYER_ONESHOT_CONT) {
                p.position -= static_cast<u32>(inc);
            } else {
                p.active = false;
                p.position = p.loop_start;
            }
        }
    }
}

// One block: the callback, the players, the total level, written two periods ahead of the read index.
void mix_block(PPUContext& ctx, Mixer& m)
{
    u8* base = gb(ctx);
    u32 cb = 0, arg = 0;
    u64 count = 0;
    {
        std::lock_guard lock(m.mutex);
        m.mix.fill(0.0f);
        cb = m.cb;
        arg = m.cb_arg;
        count = m.mixcount;
    }
    if (cb) call_guest(ctx, cb, {arg, static_cast<u32>(count), kSamples});
    std::lock_guard lock(m.mutex);
    for (Player& p : m.players)
        if (p.created && p.active && p.addr) mix_player(base, m, p);
    const f32 gain = m.total_mute ? 0.0f : m.total_level;
    // The port's read index, published by cellAudio's thread with release order after it cleared the block it played.
    const u64 read = ps_bswap64(__atomic_load_n(host_u64(base, m.read_index_addr), __ATOMIC_ACQUIRE));
    const u32 block = static_cast<u32>((read + kLead) % kPortBlocks);
    const u32 dst = m.port_addr + block * kBuses * kSamples * 4;
    for (u32 i = 0; i < kBuses * kSamples; i++) ps_writef32(base, dst + 4 * i, m.mix[i] * gain);
    m.mixcount++;
}

// The mixer's thread: a block for each period cellAudio announces, until the mixer is finalized (its queue destroyed).
void _pspocket_libmixer_thread(PPUContext& ctx, u64)
{
    Mixer& m = mixer();
    lv2::PpuThread& t = thread_of(ctx);
    while (!m.stop.load(std::memory_order_acquire)) {
        u32 queue = 0;
        {
            std::lock_guard lock(m.mutex);
            queue = m.queue;
        }
        const lv2::Result r = lv2::sys_event_queue_receive(t, queue, 0, 50'000);
        if (m.stop.load(std::memory_order_acquire)) break;
        if (r == lv2::err(lv2::CELL_ETIMEDOUT)) continue;
        if (r != lv2::CELL_OK) break; // the queue is gone
        bool started = false;
        {
            std::lock_guard lock(m.mutex);
            started = m.started;
        }
        if (started) mix_block(ctx, m);
    }
    lib_call(ctx, "sysPrxForUser", "sys_ppu_thread_exit", {0});
}

// ------------------------------------------------------------------------------------------------- the mixer

// cellSurMixerCreate(const CellSurMixerConfig *): {priority, chStrips1, chStrips2, chStrips6, chStrips8}. The port (8
// channels, 16 blocks, level 1), the notify queue, the thread.
s32 cellSurMixerCreate(PPUContext& ctx, u32 config)
{
    Mixer& m = mixer();
    u8* base = gb(ctx);
    if (!readable(ctx, config, 20)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    {
        std::lock_guard lock(m.mutex);
        if (m.created) return error(CELL_LIBMIXER_ERROR_ALREADY_EXIST);
    }
    const s32 priority = static_cast<s32>(ps_read32(base, config));
    std::array<u32, 5> counts{0, ps_read32(base, config + 4), ps_read32(base, config + 8), ps_read32(base, config + 12),
        ps_read32(base, config + 16)};
    StackVar v(ctx, base, 0x80); // CellAudioPortParam, the port number, its config, the queue ID and key, the thread ID
    const u32 param = v.addr(), portnum = v.addr() + 0x20, conf = v.addr() + 0x28, qid = v.addr() + 0x48,
              qkey = v.addr() + 0x50, tid = v.addr() + 0x58, name = v.addr() + 0x60;
    ps_write64(base, param, kBuses);
    ps_write64(base, param + 8, kPortBlocks);
    ps_write64(base, param + 16, 0);
    ps_writef32(base, param + 24, 1.0f);
    s32 r = audio_call(ctx, "cellAudioPortOpen", {param, portnum});
    if (r < 0) {
        PS_LOG_WARN(kChan, "cellSurMixerCreate: cellAudioPortOpen gives {:#x}", static_cast<u32>(r));
        const bool init = r != static_cast<s32>(kAudioNotInit);
        return error(init ? CELL_LIBMIXER_ERROR_FULL : CELL_LIBMIXER_ERROR_NOT_INITIALIZED);
    }
    const u32 port = ps_read32(base, portnum);
    audio_call(ctx, "cellAudioGetPortConfig", {port, conf});
    r = audio_call(ctx, "cellAudioCreateNotifyEventQueue", {qid, qkey});
    if (r >= 0) r = audio_call(ctx, "cellAudioSetNotifyEventQueue", {ps_read64(base, qkey)});
    if (r < 0) {
        audio_call(ctx, "cellAudioPortClose", {port});
        return error(CELL_LIBMIXER_ERROR_NO_MEMORY);
    }
    {
        std::lock_guard lock(m.mutex);
        m.created = true;
        m.priority = priority;
        for (u32 type = kType1; type <= kType8; type++) m.strips[type].assign(counts[type], Strip{});
        m.port = port;
        m.read_index_addr = ps_read32(base, conf);
        m.port_addr = ps_read32(base, conf + 0x1C);
        m.queue = ps_read32(base, qid);
        m.queue_key = ps_read64(base, qkey);
        m.stop.store(false);
        m.started = false;
        m.cb = m.cb_arg = 0;
        m.mixcount = 0;
        m.total_level = m.reverb_level = 1.0f;
        m.total_mute = false;
        m.players.clear();
    }
    std::memcpy(base + name, "_cellSurMixerThread", 20);
    const u32 entry = HleProcess::current()->link_function(kModule, recomp::nid_of("_pspocket_libmixer_thread"),
        "_pspocket_libmixer_thread");
    const u64 prio = static_cast<u64>(priority);
    r = static_cast<s32>(static_cast<u32>(
        lib_call(ctx, "sysPrxForUser", "sys_ppu_thread_create", {tid, entry, 0, prio, 0x4000, 1, name})));
    if (r < 0) {
        PS_LOG_WARN(kChan, "cellSurMixerCreate: the mixer's thread (priority {}) gives {:#x}", priority,
            static_cast<u32>(r));
        audio_call(ctx, "cellAudioRemoveNotifyEventQueue", {m.queue_key});
        lv2::sys_event_queue_destroy(thread_of(ctx), m.queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
        audio_call(ctx, "cellAudioPortClose", {port});
        std::lock_guard lock(m.mutex);
        m.created = false;
        return error(CELL_LIBMIXER_ERROR_NO_MEMORY);
    }
    {
        std::lock_guard lock(m.mutex);
        m.thread = ps_read64(base, tid);
    }
    PS_LOG_INFO(kChan, "cellSurMixerCreate(priority {}, strips {} {} {} {}): audio port {}, buffer {:#x}", priority,
        counts[1], counts[2], counts[3], counts[4], port, m.port_addr);
    return lv2::CELL_OK;
}

// The port starts before the mixer's thread may mix into it: starting it clears its ring, which would wipe a block
// mixed just before (D-571).
void start_mixing(PPUContext& ctx, Mixer& m, u32 port)
{
    audio_call(ctx, "cellAudioPortStart", {port});
    std::lock_guard lock(m.mutex);
    if (!m.created) return; // finalized meanwhile
    m.started = true;
    if (!m.start_time) m.start_time = lv2::guest_system_time_us();
}

s32 cellSurMixerStart(PPUContext& ctx)
{
    Mixer& m = mixer();
    u32 port = 0;
    {
        std::lock_guard lock(m.mutex);
        if (!m.created) return error(CELL_LIBMIXER_ERROR_NOT_INITIALIZED);
        if (m.started) return lv2::CELL_OK;
        port = m.port;
    }
    start_mixing(ctx, m, port);
    return lv2::CELL_OK;
}

// cellSurMixerPause(type): CELL_SURMIXER_CONT_PAUSE_OFF (0) plays again, ON (1) and ON_IMMEDIATE (2) pause (the port
// stops; its blocks stay).
s32 cellSurMixerPause(PPUContext& ctx, u32 type)
{
    Mixer& m = mixer();
    u32 port = 0;
    bool start = false;
    {
        std::lock_guard lock(m.mutex);
        if (!m.created) return error(CELL_LIBMIXER_ERROR_NOT_INITIALIZED);
        if (type > 2) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
        port = m.port;
        start = type == 0;
        if (start == m.started) return lv2::CELL_OK;
        if (!start) m.started = false; // no block from here on, then the port stops
    }
    if (start) start_mixing(ctx, m, port);
    else audio_call(ctx, "cellAudioPortStop", {port});
    return lv2::CELL_OK;
}

// cellSurMixerFinalize: the thread told to stop and woken (its queue destroyed) and joined, the port closed.
s32 cellSurMixerFinalize(PPUContext& ctx)
{
    Mixer& m = mixer();
    u8* base = gb(ctx);
    u32 port = 0, queue = 0;
    u64 key = 0, thread = 0;
    bool started = false;
    {
        std::lock_guard lock(m.mutex);
        if (!m.created) return error(CELL_LIBMIXER_ERROR_NOT_INITIALIZED);
        port = m.port;
        queue = m.queue;
        key = m.queue_key;
        thread = m.thread;
        started = m.started;
        m.stop.store(true, std::memory_order_release);
    }
    audio_call(ctx, "cellAudioRemoveNotifyEventQueue", {key});
    lv2::sys_event_queue_destroy(thread_of(ctx), queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE);
    StackVar status(ctx, base, 16);
    lv2::sys_ppu_thread_join(thread_of(ctx), static_cast<u32>(thread), status.addr());
    if (started) audio_call(ctx, "cellAudioPortStop", {port});
    audio_call(ctx, "cellAudioPortClose", {port});
    std::lock_guard lock(m.mutex);
    m.created = false;
    m.started = false;
    return lv2::CELL_OK;
}

s32 cellSurMixerGetAANHandle(PPUContext& ctx, u32 handle)
{
    if (!readable(ctx, handle, 4)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    ps_write32(gb(ctx), handle, kAanHandle);
    return lv2::CELL_OK;
}

// cellSurMixerChStripGetAANPortNo(port *, type, index): (type << 16) | index, as RPCS3.
s32 cellSurMixerChStripGetAANPortNo(PPUContext& ctx, u32 port, u32 type, u32 index)
{
    if (!readable(ctx, port, 4)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    ps_write32(gb(ctx), port, (type << 16) | (index & 0xFFFF));
    return lv2::CELL_OK;
}

// cellSurMixerSetNotifyCallback(func, arg): one callback (a second is ALREADY_EXIST where RPCS3 stops the process).
s32 cellSurMixerSetNotifyCallback(PPUContext&, u32 func, u32 arg)
{
    Mixer& m = mixer();
    std::lock_guard lock(m.mutex);
    if (!func) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    if (m.cb) return error(CELL_LIBMIXER_ERROR_ALREADY_EXIST);
    m.cb = func;
    m.cb_arg = arg;
    return lv2::CELL_OK;
}

s32 cellSurMixerRemoveNotifyCallback(PPUContext&, u32 func)
{
    Mixer& m = mixer();
    std::lock_guard lock(m.mutex);
    if (!m.cb || m.cb != func) return error(CELL_LIBMIXER_ERROR_NOT_EXIST);
    m.cb = m.cb_arg = 0;
    return lv2::CELL_OK;
}

// cellSurMixerSetParameter(param, value): the total level (linear or in dB), its mute, the reverb level (kept; reverb
// is not mixed).
s32 cellSurMixerSetParameter(PPUContext&, u32 param, f32 value)
{
    Mixer& m = mixer();
    std::lock_guard lock(m.mutex);
    if (!m.created) return error(CELL_LIBMIXER_ERROR_NOT_INITIALIZED);
    switch (param) {
    case kParamTotalLevelLinear: m.total_level = value; break;
    case kParamTotalLevel: m.total_level = level_from_db(value); break;
    case kParamTotalMute: m.total_mute = value != 0.0f; break;
    case kParamReverbLevelLinear: m.reverb_level = value; break;
    case kParamReverbLevel: m.reverb_level = level_from_db(value); break;
    default: return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    }
    if (m.total_level < 0.0f) m.total_level = 0.0f;
    return lv2::CELL_OK;
}

// cellSurMixerChStripSetParameter(type, index, CellSurMixerChStripParam *): {param, attribute, dBSwitch, floatVal,
// intVal}. The level and expression level (in dB when dBSwitch is set) and the mute act on what the strip adds; the
// others are accepted and kept out of the mix (center and reverb levels, positions).
s32 cellSurMixerChStripSetParameter(PPUContext& ctx, u32 type, u32 index, u32 param)
{
    Mixer& m = mixer();
    u8* base = gb(ctx);
    if (!readable(ctx, param, 20)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    std::lock_guard lock(m.mutex);
    if (!m.created) return error(CELL_LIBMIXER_ERROR_NOT_INITIALIZED);
    if (type < kType1 || type > kType8 || index >= m.strips[type].size()) {
        return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    }
    Strip& s = m.strips[type][index];
    const u32 which = ps_read32(base, param);
    const bool db = ps_read32(base, param + 8) != 0;
    const f32 fv = ps_readf32(base, param + 12);
    const s32 iv = static_cast<s32>(ps_read32(base, param + 16));
    switch (which) {
    case kStripLevel: s.level = db ? level_from_db(fv) : fv; break;
    case kStripExpression: s.expression = db ? level_from_db(fv) : fv; break;
    case kStripMute: s.mute = iv != 0; break;
    default:
        if (which > 6 || (which > 4 && type > kType2)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
        if (!m.param_logged) {
            m.param_logged = true;
            PS_LOG_INFO(kChan, "cellSurMixerChStripSetParameter: parameter {} (center, reverb, position) is not mixed",
                which);
        }
        break;
    }
    return lv2::CELL_OK;
}

// cellSurMixerSurBusAddData(busNo, offset, float *addr, samples): a block's samples added to a speaker bus (0-7);
// the reverb buses (8-11) are accepted and dropped. As RPCS3, only whole blocks (offset 0, 256 samples) mix.
s32 cellSurMixerSurBusAddData(PPUContext& ctx, u32 bus, u32 offset, u32 addr, u32 samples)
{
    Mixer& m = mixer();
    u8* base = gb(ctx);
    if (bus >= kBuses + kReverbBuses) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    if (offset != 0 || samples != kSamples) {
        PS_LOG_WARN(kChan, "cellSurMixerSurBusAddData(bus {}, offset {}, samples {}): only whole blocks mix", bus,
            offset, samples);
        return lv2::CELL_OK;
    }
    if (!readable(ctx, addr, 4 * kSamples)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    std::lock_guard lock(m.mutex);
    if (bus >= kBuses) {
        if (!m.reverb_logged) {
            m.reverb_logged = true;
            PS_LOG_INFO(kChan, "cellSurMixerSurBusAddData: reverb buses are not mixed");
        }
        return lv2::CELL_OK;
    }
    for (u32 i = 0; i < kSamples; i++) m.mix[i * kBuses + bus] += ps_readf32(base, addr + 4 * i);
    return lv2::CELL_OK;
}

s32 cellSurMixerGetCurrentBlockTag(PPUContext& ctx, u32 tag)
{
    Mixer& m = mixer();
    if (!readable(ctx, tag, 8)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    std::lock_guard lock(m.mutex);
    ps_write64(gb(ctx), tag, m.mixcount);
    return lv2::CELL_OK;
}

// cellSurMixerGetTimestamp(tag, u64 *stamp): when the block with this tag plays (RPCS3's: from the mixer's start, a
// block a period).
s32 cellSurMixerGetTimestamp(PPUContext& ctx, u64 tag, u32 stamp)
{
    Mixer& m = mixer();
    if (!readable(ctx, stamp, 8)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    std::lock_guard lock(m.mutex);
    ps_write64(gb(ctx), stamp, m.start_time + tag * kSamples * 1'000'000 / 48'000);
    return lv2::CELL_OK;
}

void cellSurMixerBeep(PPUContext&, u32 arg)
{
    Mixer& m = mixer();
    std::lock_guard lock(m.mutex);
    if (!m.beep_logged) {
        m.beep_logged = true;
        PS_LOG_INFO(kChan, "cellSurMixerBeep({:#x}): no system beep", arg);
    }
}

// ------------------------------------------------------------------------------------------------- the AAN

// cellAANAddData(handle, port, offset, float *addr, samples): a strip's block (its channels interleaved) into the mix,
// times its level and expression unless muted: mono and stereo to left and right, 5.1 to the first six buses, 7.1 to
// all eight. RPCS3's checks: the mixer's handle, a strip that exists, whole blocks.
s32 cellAANAddData(PPUContext& ctx, u32 handle, u32 aan_port, u32 offset, u32 addr, u32 samples)
{
    Mixer& m = mixer();
    u8* base = gb(ctx);
    const u32 type = aan_port >> 16, index = aan_port & 0xFFFF;
    std::lock_guard lock(m.mutex);
    const bool exists = type >= kType1 && type <= kType8 && index < m.strips[type].size();
    if (handle != kAanHandle || samples != kSamples || !exists || offset != 0) {
        PS_LOG_WARN(kChan, "cellAANAddData(handle {:#x}, port {:#x}, offset {}, samples {}): invalid", handle, aan_port,
            offset, samples);
        return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    }
    const u32 ch = kChannelsOf[type];
    if (!readable(ctx, addr, 4 * ch * kSamples)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    const Strip& s = m.strips[type][index];
    if (s.mute) return lv2::CELL_OK;
    const f32 gain = s.level * s.expression;
    for (u32 i = 0; i < kSamples; i++) {
        const auto in = [&](u32 c) { return ps_readf32(base, addr + 4 * (i * ch + c)) * gain; };
        f32* out = &m.mix[i * kBuses];
        if (type == kType1) {
            const f32 c = in(0);
            out[0] += c;
            out[1] += c;
        } else {
            for (u32 c = 0; c < ch; c++) out[c] += in(c);
        }
    }
    return lv2::CELL_OK;
}

// cellAANConnect/Disconnect(receive, receivePort, source, sourcePort): a sample player (the source) to the mixer.
s32 aan_connect(u32 source, bool connect)
{
    Mixer& m = mixer();
    std::lock_guard lock(m.mutex);
    if (source >= m.players.size() || !m.players[source].created) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    m.players[source].connected = connect;
    return lv2::CELL_OK;
}

s32 cellAANConnect(PPUContext&, u32, u32, u32 source, u32) { return aan_connect(source, true); }
s32 cellAANDisconnect(PPUContext&, u32, u32, u32 source, u32) { return aan_connect(source, false); }

// ------------------------------------------------------------------------------------------------- sample players

// cellSSPlayerCreate(handle *, CellSSPlayerConfig *): {channels (1 or 2), outputMode (0)}; the handle is its index.
s32 cellSSPlayerCreate(PPUContext& ctx, u32 handle, u32 config)
{
    Mixer& m = mixer();
    u8* base = gb(ctx);
    if (!readable(ctx, handle, 4) || !readable(ctx, config, 8)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    const u32 channels = ps_read32(base, config), mode = ps_read32(base, config + 4);
    if (mode != 0 || channels - 1 >= 2) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    std::lock_guard lock(m.mutex);
    Player p;
    p.created = true;
    p.channels = channels;
    m.players.push_back(p);
    ps_write32(base, handle, static_cast<u32>(m.players.size() - 1));
    return lv2::CELL_OK;
}

Player* player(Mixer& m, u32 handle)
{
    return handle < m.players.size() && m.players[handle].created ? &m.players[handle] : nullptr;
}

s32 cellSSPlayerRemove(PPUContext&, u32 handle)
{
    Mixer& m = mixer();
    std::lock_guard lock(m.mutex);
    Player* p = player(m, handle);
    if (!p) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    *p = Player{};
    return lv2::CELL_OK;
}

// cellSSPlayerSetWave(handle, CellSSPlayerWaveParam *, CellSSPlayerCommonParam *): {addr, format, samples, loop start,
// start} (offsets from 1, as RPCS3 reads them), {loopMode, attackMode} (one shot without it).
s32 cellSSPlayerSetWave(PPUContext& ctx, u32 handle, u32 wave, u32 common)
{
    Mixer& m = mixer();
    u8* base = gb(ctx);
    if (!readable(ctx, wave, 20) || (common && !readable(ctx, common, 8))) {
        return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    }
    std::lock_guard lock(m.mutex);
    Player* p = player(m, handle);
    if (!p) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    const u32 addr = ps_read32(base, wave), samples = ps_read32(base, wave + 8);
    const u64 bytes = u64{samples} * p->channels * 2;
    if (bytes > 0x10000000 || !readable(ctx, addr, static_cast<u32>(bytes))) {
        log_call_site(ctx, base, "cellSSPlayerSetWave: the wave is not in guest memory");
        return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    }
    p->addr = addr;
    p->samples = samples;
    p->loop_start = ps_read32(base, wave + 12) - 1;
    p->loop_mode = common ? ps_read32(base, common) : CELL_SSPLAYER_ONESHOT;
    p->position = ps_read32(base, wave + 16) - 1;
    return lv2::CELL_OK;
}

// The runtime parameters {level, speed, position {x, y, z}}.
void set_runtime(u8* base, Player& p, u32 info)
{
    p.level = ps_readf32(base, info);
    p.speed = ps_readf32(base, info + 4);
    p.x = ps_readf32(base, info + 8);
    p.y = ps_readf32(base, info + 12);
    p.z = ps_readf32(base, info + 16);
}

s32 cellSSPlayerPlay(PPUContext& ctx, u32 handle, u32 info)
{
    Mixer& m = mixer();
    if (!readable(ctx, info, 20)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    std::lock_guard lock(m.mutex);
    Player* p = player(m, handle);
    if (!p) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    p->active = true;
    set_runtime(gb(ctx), *p, info);
    return lv2::CELL_OK;
}

s32 cellSSPlayerStop(PPUContext&, u32 handle, u32)
{
    Mixer& m = mixer();
    std::lock_guard lock(m.mutex);
    Player* p = player(m, handle);
    if (!p) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    p->active = false;
    return lv2::CELL_OK;
}

s32 cellSSPlayerSetParam(PPUContext& ctx, u32 handle, u32 info)
{
    Mixer& m = mixer();
    if (!readable(ctx, info, 20)) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    std::lock_guard lock(m.mutex);
    Player* p = player(m, handle);
    if (!p) return error(CELL_LIBMIXER_ERROR_INVALID_PARAMATER);
    set_runtime(gb(ctx), *p, info);
    return lv2::CELL_OK;
}

s32 cellSSPlayerGetState(PPUContext&, u32 handle)
{
    Mixer& m = mixer();
    std::lock_guard lock(m.mutex);
    const Player* p = player(m, handle);
    if (!p) return static_cast<s32>(CELL_SSPLAYER_STATE_ERROR);
    return static_cast<s32>(p->active ? CELL_SSPLAYER_STATE_ON : CELL_SSPLAYER_STATE_OFF);
}

// ------------------------------------------------------------------------------------------------- utilities

// A level from decibels: 10^(dB/20). RPCS3 leaves the three utilities unimplemented.
f32 cellSurMixerUtilGetLevelFromDB(PPUContext&, f32 db) { return level_from_db(db); }

// A level from a decibel index: the index taken as whole decibels (an assumption: the SDK's table is not at hand).
f32 cellSurMixerUtilGetLevelFromDBIndex(PPUContext&, s32 index) { return level_from_db(static_cast<f32>(index)); }

// The frequency ratio of a note to a reference note: 2^((note - refNote) / 12), MIDI note numbers.
f32 cellSurMixerUtilNoteToRatio(PPUContext&, u32 ref_note, u32 note)
{
    return std::pow(2.0f, (static_cast<f32>(note & 0xFF) - static_cast<f32>(ref_note & 0xFF)) / 12.0f);
}

} // namespace

#define MIX(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name>, false}

void register_libmixer(Registry& r)
{
    r.add(kModule, {
        MIX(cellAANAddData), MIX(cellAANConnect), MIX(cellAANDisconnect), MIX(cellSurMixerCreate),
        MIX(cellSurMixerGetAANHandle), MIX(cellSurMixerChStripGetAANPortNo), MIX(cellSurMixerSetNotifyCallback),
        MIX(cellSurMixerRemoveNotifyCallback), MIX(cellSurMixerStart), MIX(cellSurMixerSetParameter),
        MIX(cellSurMixerFinalize), MIX(cellSurMixerSurBusAddData), MIX(cellSurMixerChStripSetParameter),
        MIX(cellSurMixerPause), MIX(cellSurMixerGetCurrentBlockTag), MIX(cellSurMixerGetTimestamp),
        MIX(cellSurMixerBeep), MIX(cellSSPlayerCreate), MIX(cellSSPlayerRemove), MIX(cellSSPlayerSetWave),
        MIX(cellSSPlayerPlay), MIX(cellSSPlayerStop), MIX(cellSSPlayerSetParam), MIX(cellSSPlayerGetState),
        MIX(cellSurMixerUtilGetLevelFromDB), MIX(cellSurMixerUtilGetLevelFromDBIndex), MIX(cellSurMixerUtilNoteToRatio),
        ::pspocket::hle::Function{{}, "_pspocket_libmixer_thread", recomp::nid_of("_pspocket_libmixer_thread"),
            &detail::thunk<&_pspocket_libmixer_thread>, true},
    });
    r.firmware_first(kModule);
}
#undef MIX

} // namespace pspocket::hle
