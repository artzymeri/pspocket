// cellVoice (libvoice): voice chat ports. There is no headset, microphone or network voice here (D-024), as in RPCS3:
// the library initializes and keeps the ports the game creates as data. Data written to an input port goes nowhere,
// an output port has nothing to read, the port attributes report a detached microphone, and connecting ports records
// nothing. Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellVoice.cpp/.h (voice_manager).
//
// Implemented: every function RPCS3 registers. Notify event queues registered by key (cellVoiceSetNotifyEventQueue)
// get SERVICE_ATTACHED when the service starts and SERVICE_DETACHED when it stops or the library ends, as in RPCS3;
// no port ever attaches. Mute flags, volumes, thresholds and bit rates are kept in the port's parameters.
//
// Port IDs are RPCS3's: the low byte is a creation counter modulo 0xA0 (the first port gets 1), the next byte the
// lowest of 1..128 that makes the ID unused; a PS3 puts a varying value there. Functions ignore the upper 16 bits of
// an ID they are given.
//
// Differences from RPCS3: pointers are checked before use (D-042). Where RPCS3 reads or writes guest memory without
// a check (the port parameters, the new port's ID, the bit rate, the port info, the size read; a PS3 takes a data
// storage exception there), an address that is not guest memory is logged with the call site and refused with
// CELL_VOICE_ERROR_ADDRESS_INVALID at the point where RPCS3 would fault.

#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellVoice";

enum : u32 {
    CELL_VOICE_ERROR_LIBVOICE_NOT_INIT = 0x80310801,
    CELL_VOICE_ERROR_LIBVOICE_INITIALIZED = 0x80310802,
    CELL_VOICE_ERROR_ARGUMENT_INVALID = 0x80310805,
    CELL_VOICE_ERROR_TOPOLOGY = 0x80310807,
    CELL_VOICE_ERROR_RESOURCE_INSUFFICIENT = 0x80310808,
    CELL_VOICE_ERROR_ADDRESS_INVALID = 0x8031080a,
    CELL_VOICE_ERROR_SERVICE_DETACHED = 0x8031080b,
    CELL_VOICE_ERROR_DEVICE_NOT_PRESENT = 0x80310812,
    CELL_VOICE_ERROR_NOT_IMPLEMENTED = 0x80310809,
    CELL_VOICE_ERROR_EVENT_QUEUE = 0x8031080f,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case CELL_VOICE_ERROR_LIBVOICE_NOT_INIT: return "CELL_VOICE_ERROR_LIBVOICE_NOT_INIT";
    case CELL_VOICE_ERROR_LIBVOICE_INITIALIZED: return "CELL_VOICE_ERROR_LIBVOICE_INITIALIZED";
    case CELL_VOICE_ERROR_ARGUMENT_INVALID: return "CELL_VOICE_ERROR_ARGUMENT_INVALID";
    case CELL_VOICE_ERROR_TOPOLOGY: return "CELL_VOICE_ERROR_TOPOLOGY";
    case CELL_VOICE_ERROR_RESOURCE_INSUFFICIENT: return "CELL_VOICE_ERROR_RESOURCE_INSUFFICIENT";
    case CELL_VOICE_ERROR_ADDRESS_INVALID: return "CELL_VOICE_ERROR_ADDRESS_INVALID";
    case CELL_VOICE_ERROR_SERVICE_DETACHED: return "CELL_VOICE_ERROR_SERVICE_DETACHED";
    case CELL_VOICE_ERROR_DEVICE_NOT_PRESENT: return "CELL_VOICE_ERROR_DEVICE_NOT_PRESENT";
    case CELL_VOICE_ERROR_NOT_IMPLEMENTED: return "CELL_VOICE_ERROR_NOT_IMPLEMENTED";
    case CELL_VOICE_ERROR_EVENT_QUEUE: return "CELL_VOICE_ERROR_EVENT_QUEUE";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

// One call's log line, "name(arguments): result", at info for the calls that change the library's state and at debug
// for those a game makes every frame (reads, writes, queries).
template <class... A>
s32 logged(s32 result, log::Level level, std::format_string<A...> fmt, A&&... args)
{
    if (level >= log::min_level()) {
        log::print(level, kChan, "{}: {}", std::format(fmt, std::forward<A>(args)...), result_name(result));
    }
    return result;
}

constexpr log::Level kState = log::Level::Info;
constexpr log::Level kPolled = log::Level::Debug;

// CellVoicePortType.
enum : u32 {
    CELLVOICE_PORTTYPE_IN_MIC = 0,
    CELLVOICE_PORTTYPE_IN_PCMAUDIO = 1,
    CELLVOICE_PORTTYPE_IN_VOICE = 2,
    CELLVOICE_PORTTYPE_OUT_PCMAUDIO = 3,
    CELLVOICE_PORTTYPE_OUT_VOICE = 4,
    CELLVOICE_PORTTYPE_OUT_SECONDARY = 5,
};
constexpr u32 CELLVOICE_PCM_INTEGER_LITTLE_ENDIAN = 5; // the last CellVoicePcmDataType
constexpr u32 CELLVOICE_MAX_PORT = 128;
// The bit rates a voice port can have (CellVoiceBitRate), exactly.
constexpr u32 kBitRates[] = {3850, 4650, 5700, 7300, 14400, 16000, 22533};
// What cellVoiceGetBitRate writes before it looks the port up, so what a refused call leaves (RPCS3: "constant value
// for errors, meaning unknown").
constexpr u32 kBitRateUnknown = 0x4f323285;

// CellVoicePortParam (28 bytes): be32 portType; be16 threshold, bMute; be f32 volume; then a union at 12: voice
// {be32 bitrate}, pcmaudio {be32 bufSize; CellVoicePCMFormat {u8 numChannels, sampleAlignment; be32 dataType at 20,
// sampleRate at 24}}, device {be32 playerId}.
constexpr u32 kPortParamSize = 28, kParamThreshold = 4, kParamMute = 6, kParamVolume = 8, kParamBitrate = 12,
              kParamPcmDataType = 20;
// CellVoiceEvent: what the notify queues get.
constexpr u64 CELLVOICE_EVENT_SERVICE_ATTACHED = 1 << 3, CELLVOICE_EVENT_SERVICE_DETACHED = 1 << 4;
// RPCS3 cellVoiceCreateNotifyEventQueue: the first key tried, of 10; the queue holds 64 events.
constexpr u64 kNotifyKeyBase = 0x80004D494F323285ull;
// RPCS3 builds a source like an lv2 event port ID: lv2_event_port::id_base, lv2_obj's id_step and id_count.
constexpr u32 kEventPortIdBase = 0x0e000000, kEventPortIdStep = 0x100, kEventPortIdCount = 8192;
// CellVoiceInitParam: be32 eventMask, version; be s32 appType; reserved[20].
constexpr u32 kInitParamLogged = 12;

// RPCS3 voice_manager::port_t: the parameters the port was created with (its state field is never read).
struct Port {
    std::array<u8, kPortParamSize> info{}; // CellVoicePortParam as the game passed it (guest byte order)

    u32 type() const { return ps_ld32(info.data()); }
    u32 bitrate() const { return ps_ld32(info.data() + kParamBitrate); }
    u32 pcm_data_type() const { return ps_ld32(info.data() + kParamPcmDataType); }
    bool input() const { return type() < CELLVOICE_PORTTYPE_OUT_PCMAUDIO; } // what an IPort argument must be
    bool output() const { return type() > CELLVOICE_PORTTYPE_IN_VOICE; }    // and an OPort
    bool voice() const { return type() == CELLVOICE_PORTTYPE_IN_VOICE || type() == CELLVOICE_PORTTYPE_OUT_VOICE; }
};

// RPCS3 voice_manager.
struct VoiceState {
    std::mutex mutex;
    bool initialized = false;     // is_init
    bool service_started = false; // voice_service_started: from cellVoiceStart to cellVoiceStop or cellVoiceEnd
    u8 id_ctr = 0;                // the creation counter, the low byte of port IDs
    u32 port_source = 0;          // the next made-up event source
    std::map<u16, Port> ports;
    std::map<u64, std::vector<u64>> queue_keys; // a notify queue's key -> the sources it was registered with

    Port* port(u32 id)
    {
        const auto it = ports.find(static_cast<u16>(id)); // the upper 16 bits are ignored
        return it == ports.end() ? nullptr : &it->second;
    }

    // RPCS3 voice_manager::reset.
    void reset()
    {
        id_ctr = 0;
        port_source = 0;
        ports.clear();
        queue_keys.clear();
    }
};

VoiceState& voice()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellVoice called without an HLE process");
    return p->state<VoiceState>();
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// Sends `event` from every registered source to its queue, if the queue still exists (RPCS3).
void notify(VoiceState& s, u64 event)
{
    lv2::Kernel& kernel = HleProcess::current()->kernel();
    for (const auto& [key, sources] : s.queue_keys) {
        const auto q = kernel.event_queue_by_key(key);
        if (!q) continue;
        for (const u64 source : sources) q->send(lv2::Event{source, event, 0, 0});
    }
}

// ------------------------------------------------------------------------------------------- init, end, start, stop

// cellVoiceInit(CellVoiceInitParam*): the parameters (event mask, version, application type) are not read, as in
// RPCS3; they are only logged.
s32 cellVoiceInit(PPUContext& ctx, u32 arg)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return logged(error(CELL_VOICE_ERROR_LIBVOICE_INITIALIZED), kState, "cellVoiceInit({:#x})", arg);
    if (!arg) return logged(error(CELL_VOICE_ERROR_ARGUMENT_INVALID), kState, "cellVoiceInit(pArg 0)");
    s.initialized = true;
    if (guest_readable(thread_of(ctx), arg, kInitParamLogged)) {
        u8* base = guest_base(ctx);
        return logged(lv2::CELL_OK, kState,
            "cellVoiceInit(pArg {:#x} {{eventMask {:#x}, version {}, appType {:#x}}}): no voice devices, ports are "
            "kept as data",
            arg, ps_read32(base, arg), ps_read32(base, arg + 4), ps_read32(base, arg + 8));
    }
    return logged(lv2::CELL_OK, kState, "cellVoiceInit(pArg {:#x}): no voice devices, ports are kept as data", arg);
}

// cellVoiceInitEx(CellVoiceInitParam*): as cellVoiceInit (RPCS3 reads nothing either).
s32 cellVoiceInitEx(PPUContext& ctx, u32 arg) { return cellVoiceInit(ctx, arg); }

// A started service sends SERVICE_DETACHED to the notify queues first.
s32 cellVoiceEnd(PPUContext&)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return logged(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState, "cellVoiceEnd()");
    const usize ports = s.ports.size();
    if (std::exchange(s.service_started, false)) notify(s, CELLVOICE_EVENT_SERVICE_DETACHED);
    s.reset();
    s.initialized = false;
    return logged(lv2::CELL_OK, kState, "cellVoiceEnd() ({} ports deleted)", ports);
}

// RPCS3 VoiceStart: a second start is a no-op; the first sends SERVICE_ATTACHED to the notify queues.
s32 voice_start(VoiceState& s)
{
    if (!std::exchange(s.service_started, true)) notify(s, CELLVOICE_EVENT_SERVICE_ATTACHED);
    return lv2::CELL_OK;
}

s32 cellVoiceStart(PPUContext&)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return logged(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState, "cellVoiceStart()");
    return logged(voice_start(s), kState, "cellVoiceStart()");
}

// cellVoiceStartEx(CellVoiceStartParam*): its memory container is not used (RPCS3).
s32 cellVoiceStartEx(PPUContext&, u32 arg)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return logged(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState, "cellVoiceStartEx()");
    if (!arg) return logged(error(CELL_VOICE_ERROR_ARGUMENT_INVALID), kState, "cellVoiceStartEx(pArg 0)");
    return logged(voice_start(s), kState, "cellVoiceStartEx({:#x})", arg);
}

// Stopping a service that isn't started is a no-op; stopping sends SERVICE_DETACHED to the notify queues.
s32 cellVoiceStop(PPUContext&)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return logged(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState, "cellVoiceStop()");
    if (std::exchange(s.service_started, false)) notify(s, CELLVOICE_EVENT_SERVICE_DETACHED);
    return logged(lv2::CELL_OK, kState, "cellVoiceStop()");
}

// ------------------------------------------------------------------------------------------------ notify queues

// cellVoiceCreateNotifyEventQueue(u32* id, u64* key): a PPU event queue (FIFO, 64 events) on the first free key of 10
// from 0x80004D494F323285 (RPCS3).
s32 cellVoiceCreateNotifyEventQueue(PPUContext& ctx, u32 id, u32 key)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return logged(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState, "cellVoiceCreateNotifyEventQueue()");
    if (!usable(ctx, key, 8, "cellVoiceCreateNotifyEventQueue: key")) return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 16); // sys_event_queue_attribute_t {protocol, type, name[8] = 0}
    ps_write32(base, attr.addr(), lv2::SYS_SYNC_FIFO);
    ps_write32(base, attr.addr() + 4, lv2::SYS_PPU_QUEUE);
    for (u64 i = 0; i < 10; i++) {
        const u64 key_value = kNotifyKeyBase + i;
        const s32 r = lv2::sys_event_queue_create(thread_of(ctx), id, attr.addr(), key_value, 0x40);
        if (r == lv2::err(lv2::CELL_EEXIST)) continue;
        if (r != lv2::CELL_OK) return logged(r, kState, "cellVoiceCreateNotifyEventQueue({:#x}, {:#x})", id, key);
        ps_write64(base, key, key_value);
        return logged(r, kState, "cellVoiceCreateNotifyEventQueue: queue {:#x}, key {:#x}", ps_read32(base, id),
            key_value);
    }
    return logged(error(CELL_VOICE_ERROR_EVENT_QUEUE), kState, "cellVoiceCreateNotifyEventQueue({:#x}, {:#x})", id, key);
}

// cellVoiceSetNotifyEventQueue(u64 key, u64 source): a queue that exists, by key; the same key may be set again with
// another source. Source 0 makes one up, like an event port with no name (RPCS3: (pid + 1) << 32 | port ID).
s32 cellVoiceSetNotifyEventQueue(PPUContext& ctx, u64 key, u64 source)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) {
        return logged(r, kState, "cellVoiceSetNotifyEventQueue(key {:#x}, source {:#x})", key, source);
    };
    if (!s.initialized) return done(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT));
    if (!HleProcess::current()->kernel().event_queue_by_key(key)) return done(error(CELL_VOICE_ERROR_EVENT_QUEUE));
    if (!source) {
        const u64 pid = static_cast<u32>(lv2::sys_process_getpid(thread_of(ctx)));
        source = ((pid + 1) << 32) | (kEventPortIdBase + s.port_source * kEventPortIdStep);
        s.port_source = (s.port_source + 1) % kEventPortIdCount;
    }
    s.queue_keys[key].push_back(source);
    return done(lv2::CELL_OK);
}

s32 cellVoiceRemoveNotifyEventQueue(PPUContext&, u64 key)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, kState, "cellVoiceRemoveNotifyEventQueue(key {:#x})", key); };
    if (!s.initialized) return done(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT));
    if (!s.queue_keys.erase(key)) return done(error(CELL_VOICE_ERROR_EVENT_QUEUE));
    return done(lv2::CELL_OK);
}

// ------------------------------------------------------------------------------------------------------- ports

// cellVoiceCreatePort(u32* portId, const CellVoicePortParam* pArg). A PCM port needs a known sample type, a voice
// port one of the exact bit rates; at most 128 ports exist.
s32 cellVoiceCreatePort(PPUContext& ctx, u32 port_id, u32 arg)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) {
        return logged(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState, "cellVoiceCreatePort(pArg {:#x})", arg);
    }
    if (!arg) return logged(error(CELL_VOICE_ERROR_ARGUMENT_INVALID), kState, "cellVoiceCreatePort(pArg 0)");
    if (!usable(ctx, arg, kPortParamSize, "cellVoiceCreatePort: the CellVoicePortParam")) {
        return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
    }
    Port port;
    std::memcpy(port.info.data(), guest_base(ctx) + arg, kPortParamSize);
    bool valid = false;
    switch (port.type()) {
    case CELLVOICE_PORTTYPE_IN_PCMAUDIO:
    case CELLVOICE_PORTTYPE_OUT_PCMAUDIO: valid = port.pcm_data_type() <= CELLVOICE_PCM_INTEGER_LITTLE_ENDIAN; break;
    case CELLVOICE_PORTTYPE_IN_VOICE:
    case CELLVOICE_PORTTYPE_OUT_VOICE:
        for (const u32 rate : kBitRates) valid = valid || port.bitrate() == rate;
        break;
    case CELLVOICE_PORTTYPE_IN_MIC:
    case CELLVOICE_PORTTYPE_OUT_SECONDARY: valid = true; break;
    }
    if (!valid) {
        return logged(error(CELL_VOICE_ERROR_ARGUMENT_INVALID), kState,
            "cellVoiceCreatePort(pArg {:#x} {{type {}, word at 12 {:#x}, PCM data type {}}})", arg, port.type(),
            port.bitrate(), port.pcm_data_type());
    }
    if (s.ports.size() >= CELLVOICE_MAX_PORT) {
        return logged(error(CELL_VOICE_ERROR_RESOURCE_INSUFFICIENT), kState, "cellVoiceCreatePort(type {}): {} ports",
            port.type(), s.ports.size());
    }
    if (!usable(ctx, port_id, 4, "cellVoiceCreatePort: portId")) return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
    s.id_ctr = static_cast<u8>((s.id_ctr + 1) % 0xa0);
    // Fewer than 128 ports exist, so one of these 128 IDs is free.
    for (u32 high = 1; high <= CELLVOICE_MAX_PORT; high++) {
        const u16 id = static_cast<u16>((high << 8) | s.id_ctr);
        if (!s.ports.try_emplace(id, port).second) continue;
        ps_write32(guest_base(ctx), port_id, id);
        return logged(lv2::CELL_OK, kState, "cellVoiceCreatePort(type {}, word at 12 {:#x}): port {:#06x}", port.type(),
            port.bitrate(), id);
    }
    PS_FATAL(kChan, "cellVoiceCreatePort: no free ID for counter {:#x} with {} ports", s.id_ctr, s.ports.size());
}

s32 cellVoiceDeletePort(PPUContext&, u32 port_id)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) {
        return logged(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState, "cellVoiceDeletePort({:#x})", port_id);
    }
    if (!s.ports.erase(static_cast<u16>(port_id))) {
        return logged(error(CELL_VOICE_ERROR_TOPOLOGY), kState, "cellVoiceDeletePort({:#x})", port_id);
    }
    return logged(lv2::CELL_OK, kState, "cellVoiceDeletePort({:#x})", port_id);
}

// The checks of connecting and disconnecting (RPCS3, a TODO for both): `ips` must be an input port (mic, PCM or voice
// in) and `ops` an output port. No connection is recorded: no data flows between ports here.
s32 check_topology(VoiceState& s, u32 ips, u32 ops)
{
    if (!s.initialized) return error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    const Port* in = s.port(ips);
    if (!in || !in->input()) return error(CELL_VOICE_ERROR_TOPOLOGY);
    const Port* out = s.port(ops);
    if (!out || !out->output()) return error(CELL_VOICE_ERROR_TOPOLOGY);
    return lv2::CELL_OK;
}

s32 cellVoiceConnectIPortToOPort(PPUContext&, u32 ips, u32 ops)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(check_topology(s, ips, ops), kState, "cellVoiceConnectIPortToOPort({:#x}, {:#x})", ips, ops);
}

s32 cellVoiceDisconnectIPortFromOPort(PPUContext&, u32 ips, u32 ops)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(check_topology(s, ips, ops), kState, "cellVoiceDisconnectIPortFromOPort({:#x}, {:#x})", ips, ops);
}

// cellVoiceGetPortInfo(portId, CellVoiceBasePortInfo*): only while the service runs; RPCS3 fills in only the port type
// (be32 at 0; a TODO for the state, edges and sizes), and so does this.
s32 cellVoiceGetPortInfo(PPUContext& ctx, u32 port_id, u32 info)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, kPolled, "cellVoiceGetPortInfo({:#x}, {:#x})", port_id, info); };
    if (!s.initialized) return done(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT));
    const Port* port = s.port(port_id);
    if (!port) return done(error(CELL_VOICE_ERROR_TOPOLOGY));
    if (!s.service_started) return done(error(CELL_VOICE_ERROR_SERVICE_DETACHED));
    if (!usable(ctx, info, 4, "cellVoiceGetPortInfo: pInfo")) return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
    ps_write32(guest_base(ctx), info, port->type());
    return done(lv2::CELL_OK);
}

// cellVoiceSetPortAttr / cellVoiceGetPortAttr(portId, attr, void* attrValue): every attribute belongs to a device, and
// RPCS3 reports the microphone detached (CELL_VOICE_ERROR_DEVICE_NOT_PRESENT, which it doesn't log as an error) for
// any existing port. The value is neither read nor written.
s32 port_attr(VoiceState& s, u32 port_id)
{
    if (!s.initialized) return error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    if (!s.port(port_id)) return error(CELL_VOICE_ERROR_TOPOLOGY);
    return error(CELL_VOICE_ERROR_DEVICE_NOT_PRESENT);
}

s32 cellVoiceSetPortAttr(PPUContext&, u32 port_id, u32 attr, u32 value)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(port_attr(s, port_id), kPolled, "cellVoiceSetPortAttr({:#x}, attr {}, value {:#x})", port_id, attr,
        value);
}

s32 cellVoiceGetPortAttr(PPUContext&, u32 port_id, u32 attr, u32 value)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(port_attr(s, port_id), kPolled, "cellVoiceGetPortAttr({:#x}, attr {}, value {:#x})", port_id, attr,
        value);
}

// cellVoiceGetBitRate(portId, u32* bitrate): RPCS3 writes kBitRateUnknown before it looks the port up (no null check
// either), then the voice port's bit rate.
s32 cellVoiceGetBitRate(PPUContext& ctx, u32 port_id, u32 bitrate)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, kPolled, "cellVoiceGetBitRate({:#x}, {:#x})", port_id, bitrate); };
    if (!s.initialized) return done(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT));
    if (!usable(ctx, bitrate, 4, "cellVoiceGetBitRate: bitrate")) return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
    u8* base = guest_base(ctx);
    ps_write32(base, bitrate, kBitRateUnknown);
    const Port* port = s.port(port_id);
    if (!port || !port->voice()) return done(error(CELL_VOICE_ERROR_TOPOLOGY));
    ps_write32(base, bitrate, port->bitrate());
    return done(lv2::CELL_OK);
}

// cellVoiceReadFromOPort(ops, void* data, u32* size): an output port never has data; the size read is 0.
s32 cellVoiceReadFromOPort(PPUContext& ctx, u32 ops, u32 data, u32 size)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) {
        return logged(r, kPolled, "cellVoiceReadFromOPort({:#x}, data {:#x}, size {:#x})", ops, data, size);
    };
    if (!s.initialized) return done(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT));
    const Port* out = s.port(ops);
    if (!out || !out->output()) return done(error(CELL_VOICE_ERROR_TOPOLOGY));
    if (size) {
        if (!usable(ctx, size, 4, "cellVoiceReadFromOPort: size")) return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
        ps_write32(guest_base(ctx), size, 0);
    }
    return done(lv2::CELL_OK);
}

// cellVoiceWriteToIPort(ips, const void* data, u32* size): the data goes nowhere. Neither it nor the size is touched
// (RPCS3), so the size the game passed reads as all of it written.
s32 cellVoiceWriteToIPort(PPUContext&, u32 ips, u32 data, u32 size)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) {
        return logged(r, kPolled, "cellVoiceWriteToIPort({:#x}, data {:#x}, size {:#x})", ips, data, size);
    };
    if (!s.initialized) return done(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT));
    const Port* in = s.port(ips);
    if (!in || !in->input()) return done(error(CELL_VOICE_ERROR_TOPOLOGY));
    return done(lv2::CELL_OK);
}

// The Ex writes (with a count of lost frames, or of frame gaps): as cellVoiceWriteToIPort.
s32 cellVoiceWriteToIPortEx(PPUContext& ctx, u32 ips, u32 data, u32 size, u32)
{
    return cellVoiceWriteToIPort(ctx, ips, data, size);
}

s32 cellVoiceWriteToIPortEx2(PPUContext& ctx, u32 ips, u32 data, u32 size, u32)
{
    return cellVoiceWriteToIPort(ctx, ips, data, size);
}

// ------------------------------------------------------------------------------------------ port parameters

// The checks of the per-port calls: initialised, then a port with this ID (CELL_VOICE_ERROR_TOPOLOGY).
s32 port_checks(VoiceState& s, u32 port_id, Port** port = nullptr)
{
    if (!s.initialized) return error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT);
    Port* p = s.port(port_id);
    if (!p) return error(CELL_VOICE_ERROR_TOPOLOGY);
    if (port) *port = p;
    return lv2::CELL_OK;
}

// Pausing, resuming and resetting a port: accepted for an existing one; no data flows to pause (RPCS3).
s32 cellVoicePausePort(PPUContext&, u32 port_id)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(port_checks(s, port_id), kState, "cellVoicePausePort({:#x})", port_id);
}

s32 cellVoiceResumePort(PPUContext&, u32 port_id)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(port_checks(s, port_id), kState, "cellVoiceResumePort({:#x})", port_id);
}

s32 cellVoiceResetPort(PPUContext&, u32 port_id)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(port_checks(s, port_id), kState, "cellVoiceResetPort({:#x})", port_id);
}

s32 all_ports(const char* fn)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(s.initialized ? lv2::CELL_OK : error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState, "{}()", fn);
}

s32 cellVoicePausePortAll(PPUContext&) { return all_ports("cellVoicePausePortAll"); }
s32 cellVoiceResumePortAll(PPUContext&) { return all_ports("cellVoiceResumePortAll"); }

// RPCS3: accepted, and no port's mute flag changes.
s32 cellVoiceSetMuteFlagAll(PPUContext&, u16 muted)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(s.initialized ? lv2::CELL_OK : error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState,
        "cellVoiceSetMuteFlagAll({})", muted);
}

s32 cellVoiceSetMuteFlag(PPUContext&, u32 port_id, u16 muted)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    Port* port = nullptr;
    const s32 r = port_checks(s, port_id, &port);
    if (r == lv2::CELL_OK) ps_st16(port->info.data() + kParamMute, muted);
    return logged(r, kPolled, "cellVoiceSetMuteFlag({:#x}, {})", port_id, muted);
}

s32 cellVoiceGetMuteFlag(PPUContext& ctx, u32 port_id, u32 muted)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    Port* port = nullptr;
    const s32 r = port_checks(s, port_id, &port);
    if (r != lv2::CELL_OK) return logged(r, kPolled, "cellVoiceGetMuteFlag({:#x})", port_id);
    if (!usable(ctx, muted, 2, "cellVoiceGetMuteFlag: bMuted")) return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
    ps_write16(guest_base(ctx), muted, ps_ld16(port->info.data() + kParamMute));
    return lv2::CELL_OK;
}

// cellVoiceSetVolume(portId, float volume): volume in f1.
s32 cellVoiceSetVolume(PPUContext& ctx, u32 port_id)
{
    const f32 volume = static_cast<f32>(ctx.fpr[1]);
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    Port* port = nullptr;
    const s32 r = port_checks(s, port_id, &port);
    if (r == lv2::CELL_OK) ps_st32(port->info.data() + kParamVolume, std::bit_cast<u32>(volume));
    return logged(r, kPolled, "cellVoiceSetVolume({:#x}, {})", port_id, volume);
}

// cellVoiceGetVolume(portId, float* volume): like cellVoiceGetBitRate, RPCS3 writes its "unknown" constant first.
s32 cellVoiceGetVolume(PPUContext& ctx, u32 port_id, u32 volume)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return logged(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kPolled, "cellVoiceGetVolume()");
    if (!usable(ctx, volume, 4, "cellVoiceGetVolume: volume")) return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
    u8* base = guest_base(ctx);
    ps_write32(base, volume, kBitRateUnknown);
    const Port* port = s.port(port_id);
    if (!port) return logged(error(CELL_VOICE_ERROR_TOPOLOGY), kPolled, "cellVoiceGetVolume({:#x})", port_id);
    ps_write32(base, volume, ps_ld32(port->info.data() + kParamVolume));
    return lv2::CELL_OK;
}

bool valid_bitrate(u32 rate)
{
    for (const u32 r : kBitRates)
        if (r == rate) return true;
    return false;
}

// cellVoiceSetBitRate(portId, bitrate): a voice port, one of the exact bit rates.
s32 cellVoiceSetBitRate(PPUContext&, u32 port_id, u32 bitrate)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, kState, "cellVoiceSetBitRate({:#x}, {})", port_id, bitrate); };
    if (!s.initialized) return done(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT));
    Port* port = s.port(port_id);
    if (!port || !port->voice()) return done(error(CELL_VOICE_ERROR_TOPOLOGY));
    if (!valid_bitrate(bitrate)) return done(error(CELL_VOICE_ERROR_ARGUMENT_INVALID));
    ps_st32(port->info.data() + kParamBitrate, bitrate);
    return done(lv2::CELL_OK);
}

// cellVoiceUpdatePort(portId, const CellVoicePortParam*): the mute flag, volume, threshold and (for a voice port) bit
// rate; the type and the rest stay (RPCS3, which takes the bit rate unchecked).
s32 cellVoiceUpdatePort(PPUContext& ctx, u32 port_id, u32 arg)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    const auto done = [&](s32 r) { return logged(r, kState, "cellVoiceUpdatePort({:#x}, {:#x})", port_id, arg); };
    if (!s.initialized) return done(error(CELL_VOICE_ERROR_LIBVOICE_NOT_INIT));
    if (!arg) return done(error(CELL_VOICE_ERROR_ARGUMENT_INVALID));
    Port* port = s.port(port_id);
    if (!port) return done(error(CELL_VOICE_ERROR_TOPOLOGY));
    if (!usable(ctx, arg, kPortParamSize, "cellVoiceUpdatePort: the CellVoicePortParam")) {
        return error(CELL_VOICE_ERROR_ADDRESS_INVALID);
    }
    const u8* src = guest_base(ctx) + arg;
    std::memcpy(port->info.data() + kParamThreshold, src + kParamThreshold, 8); // threshold, bMute, volume
    if (port->voice()) std::memcpy(port->info.data() + kParamBitrate, src + kParamBitrate, 4);
    return done(lv2::CELL_OK);
}

// cellVoiceGetSignalState(portId, attr, void* attrValue): the microphone is detached (RPCS3).
s32 cellVoiceGetSignalState(PPUContext&, u32 port_id, u32 attr, u32 value)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    s32 r = port_checks(s, port_id);
    if (r == lv2::CELL_OK) r = error(CELL_VOICE_ERROR_DEVICE_NOT_PRESENT);
    return logged(r, kPolled, "cellVoiceGetSignalState({:#x}, attr {}, value {:#x})", port_id, attr, value);
}

s32 cellVoiceDebugTopology(PPUContext&)
{
    VoiceState& s = voice();
    std::lock_guard lock(s.mutex);
    return logged(error(s.initialized ? CELL_VOICE_ERROR_NOT_IMPLEMENTED : CELL_VOICE_ERROR_LIBVOICE_NOT_INIT), kState,
        "cellVoiceDebugTopology()");
}

} // namespace

void register_cellVoice(Registry& r)
{
    r.add("cellVoice", {
        PS_HLE_FUNC(0xC7CF1182, cellVoiceInit),
        PS_HLE_FUNC(0xE0E1AE12, cellVoiceEnd),
        PS_HLE_FUNC(0x0A563878, cellVoiceStart),
        PS_HLE_FUNC(0xD3A84BE1, cellVoiceStop),
        PS_HLE_FUNC(0x2DE54871, cellVoiceCreatePort),
        PS_HLE_FUNC(0x9F70C475, cellVoiceDeletePort),
        PS_HLE_FUNC(0xAE6A21D5, cellVoiceConnectIPortToOPort),
        PS_HLE_FUNC(0x18D3DF30, cellVoiceDisconnectIPortFromOPort),
        PS_HLE_FUNC(0x54AC3519, cellVoiceGetPortInfo),
        PS_HLE_FUNC(0x9D0F4AF1, cellVoiceSetPortAttr),
        PS_HLE_FUNC(0xF629ED67, cellVoiceGetPortAttr),
        PS_HLE_FUNC(0xBEF53A2B, cellVoiceGetBitRate),
        PS_HLE_FUNC(0x36472C57, cellVoiceReadFromOPort),
        PS_HLE_FUNC(0x3DAD26E7, cellVoiceWriteToIPort),
        PS_HLE_FUNC(0x2A01013E, cellVoiceCreateNotifyEventQueue),
        PS_HLE_FUNC(0x20BAFE31, cellVoiceDebugTopology),
        PS_HLE_FUNC(0x474609E2, cellVoiceGetMuteFlag),
        PS_HLE_FUNC(0xD6811AA7, cellVoiceGetSignalState),
        PS_HLE_FUNC(0x762DC193, cellVoiceGetVolume),
        PS_HLE_FUNC(0xB1A2C38F, cellVoiceInitEx),
        PS_HLE_FUNC(0x87C71B06, cellVoicePausePort),
        PS_HLE_FUNC(0xD14E784D, cellVoicePausePortAll),
        PS_HLE_FUNC(0xDD000886, cellVoiceRemoveNotifyEventQueue),
        PS_HLE_FUNC(0xFF0FA43A, cellVoiceResetPort),
        PS_HLE_FUNC(0x7BF17B15, cellVoiceResumePort),
        PS_HLE_FUNC(0x7F3963F7, cellVoiceResumePortAll),
        PS_HLE_FUNC(0x7E60ADC6, cellVoiceSetBitRate),
        PS_HLE_FUNC(0xDDE35A0C, cellVoiceSetMuteFlag),
        PS_HLE_FUNC(0xD4D80EA5, cellVoiceSetMuteFlagAll),
        PS_HLE_FUNC(0x35D84910, cellVoiceSetNotifyEventQueue),
        PS_HLE_FUNC(0xD5AE37D8, cellVoiceSetVolume),
        PS_HLE_FUNC(0x94D51F92, cellVoiceStartEx),
        PS_HLE_FUNC(0x2F24FEA3, cellVoiceUpdatePort),
        PS_HLE_FUNC(0x30F0B5AB, cellVoiceWriteToIPortEx),
        PS_HLE_FUNC(0xC4F573B8, cellVoiceWriteToIPortEx2),
    });
}

} // namespace pspocket::hle
