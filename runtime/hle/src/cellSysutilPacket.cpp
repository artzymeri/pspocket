// cellSysutil's packets and shared memories: libsysutil's two channels to the system software (D-534). Read from
// libsysutil.sprx (4.92) and compared with it call for call (tests/hle/test_sysutil_cxml_compare.cpp); RPCS3 has none
// of these (it registers them as stubs).
//
// A channel is 32 KB of a 64 KB shared memory lv2 keeps for the system software (key 0x8006010000000010): a 32-byte
// header (sysutil_cxml.hpp ring::), then a ring the system lays out at 0x400 (0x7C00 bytes). A packet goes one way at
// a time: the side writing it (cellSysutilPacketWrite) marks its direction (to the system for a request's ID, from it
// for ID 0), the other side reads it (cellSysutilPacketRead) until the writer's last write has been read. An lv2
// mutex guards the header and four condition variables wake the sides: data to read, room to write, and the system's
// and a request's side free again (cellSysutilPacketBegin takes a side, cellSysutilPacketEnd gives it back). A closed
// channel makes every wait answer 0x8002b007.
//
// pspocket's console has no system software. libsysutil's start for its channels is done as the system side does it
// (the shared memory created and mapped, each channel's mutex and conditions created under their IPC keys), but the
// rings are never laid out, since nothing would read or answer them, and both channels are closed as libsysutil closes
// them when it stops (packets_start). So a request runs libsysutil's code up to its handover and gets the closed
// channel's answer there, the same each time: its first send finds no room and answers 0x8002b007, and a read of a
// reply answers the same. Nothing is left waiting for a reply or for a callback the system would send.
//
// cellSysutilSharedMemoryAlloc/Free are libsysutil's helpers for the shared memories of its sub-libraries (the AV
// chat's, the channels' own): an IPC-keyed shared memory, mapped into liblv2's shared memory area (64 KB pages) or an
// address range of its own (1 MB pages), counted so the range goes with the last one.

#include <format>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil_cxml.hpp"

namespace pspocket::hle::sysutil {

namespace {

constexpr std::string_view kChan = "cellSysutil";

// The lv2 calls libsysutil makes here.
enum : u32 {
    SYS_MUTEX_CREATE = 100,
    SYS_MUTEX_LOCK = 102,
    SYS_MUTEX_UNLOCK = 104,
    SYS_COND_CREATE = 105,
    SYS_COND_WAIT = 107,
    SYS_COND_SIGNAL = 108,
    SYS_COND_SIGNAL_ALL = 109,
    SYS_TIMER_USLEEP = 141,
    SYS_MMAPPER_ALLOCATE_SHARED_MEMORY_FROM_CONTAINER_EXT = 328,
    SYS_MMAPPER_FREE_SHARED_MEMORY = 329,
    SYS_MMAPPER_ALLOCATE_ADDRESS = 330,
    SYS_MMAPPER_FREE_ADDRESS = 331,
    SYS_MMAPPER_ALLOCATE_SHARED_MEMORY = 332,
    SYS_MMAPPER_UNMAP_SHARED_MEMORY = 335,
    SYS_MMAPPER_SEARCH_AND_MAP = 337,
    SYS_MMAPPER_ALLOCATE_SHARED_MEMORY_FROM_CONTAINER = 362,
};

constexpr u32 CELL_EINVAL = 0x80010002, CELL_ESRCH = 0x80010005, CELL_EFAULT = 0x8001000D;
constexpr u64 kSysutilShmKey = 0x8006010000000010ull; // the channels' shared memory (RPCS3 SYS_MMAPPER_SYSUTIL_SHM_KEY)
constexpr u64 kChannelKey = 0x8006010000000000ull;    // the channels' lv2 objects: | 0x20 mutex, 0x30.. conditions

constexpr s32 err(u32 code) { return static_cast<s32>(code); }

// lv2's answer (libsysutil's syscall wrappers sign-extend r3's low word).
s32 sc(PPUContext& ctx, u32 number, std::initializer_list<u64> args)
{
    return static_cast<s32>(static_cast<u32>(lv2_syscall(ctx, number, args)));
}

struct PacketState {
    std::recursive_mutex mutex; // packets_start, which may make guest calls through other libraries
    u32 globals = 0;
    bool started = false;
    std::set<std::string, std::less<>> logged; // the handovers logged
};

PacketState& state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSysutil's packets used without an HLE process");
    return p->state<PacketState>();
}

// liblv2's export, which libsysutil imports: pspocket's own sysPrxForUser (the caller's r3..r10 kept).
u64 lv2_lib(PPUContext& ctx, u32 nid, std::initializer_list<u64> args)
{
    const Function* f = registry().find("sysPrxForUser", nid);
    if (!f) PS_FATAL(kChan, "sysPrxForUser {:#010x}, which libsysutil imports, is not pspocket's", nid);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    f->fn(ctx, guest_base(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// liblv2's sys_mmapper_get_shared_memory_area (RPCS3's name for sysPrxForUser 0xB5D5F64E).
constexpr u32 kNidSharedMemoryArea = 0xB5D5F64E;

// ------------------------------------------------------------------------------------------------- shared memories

// cellSysutilSharedMemoryAlloc(side, key, size, container, page size, out address, entries, entry count): the shared
// memory of `key`, created (side 0, the system's, without a container: flags 0xC000) or found (any other side), or
// made from a container (with entries, the _ext form), retried every 100 ms while lv2 has none of that key; then
// mapped into the area of its page size, which the first one takes (64 KB pages: liblv2's shared memory area; 1 MB:
// a 256 MB range of its own).
s32 shared_memory_alloc(PPUContext& ctx, s32 side, u64 key, u32 size, s32 container, u64 flags, u32 out, u32 entries,
    s32 count)
{
    Mem m(ctx);
    u32 rec = 0;
    if (flags == 0x200) rec = globals(ctx) + glob::area_64k;
    else if (flags == 0x400) rec = globals(ctx) + glob::area_1m;
    else return err(CELL_EINVAL);
    StackVar id(ctx, m.base(), 16);
    for (;;) {
        s32 r = 0;
        if (side != 0) {
            r = sc(ctx, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY, {key, size, flags, id.addr()});
        } else if (container == -1) {
            r = sc(ctx, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY, {key, size, 0xC000 | flags, id.addr()});
        } else if (!entries) {
            r = sc(ctx, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY_FROM_CONTAINER,
                {key, size, static_cast<u32>(container), flags, id.addr()});
        } else {
            r = sc(ctx, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY_FROM_CONTAINER_EXT,
                {key, size, flags, static_cast<u32>(container), entries, static_cast<u64>(static_cast<s64>(count)),
                    id.addr()});
        }
        if (r == 0) break;
        if (r != err(CELL_ESRCH)) return r;
        sc(ctx, SYS_TIMER_USLEEP, {100000});
    }
    const u32 n = m.r32(rec + 4) + 1;
    m.w32(rec + 4, n);
    if (n == 1) {
        s32 r = 0;
        if (flags == 0x200) {
            r = static_cast<s32>(static_cast<u32>(lv2_lib(ctx, kNidSharedMemoryArea, {0x20F, rec})));
        } else {
            r = sc(ctx, SYS_MMAPPER_ALLOCATE_ADDRESS, {0x10000000, 0x400, 0, rec});
        }
        if (r) return r;
    }
    return sc(ctx, SYS_MMAPPER_SEARCH_AND_MAP, {m.r32(rec), m.r32(id.addr()), 0x40000, out});
}

// cellSysutilSharedMemoryFree(side, address, page size): unmapped and freed; the last one of the 1 MB area frees its
// range, the last of the 64 KB area forgets it (liblv2 keeps it).
s32 shared_memory_free(PPUContext& ctx, u32 addr, u64 flags)
{
    Mem m(ctx);
    u32 rec = 0;
    if (flags == 0x200) rec = globals(ctx) + glob::area_64k;
    else if (flags == 0x400) rec = globals(ctx) + glob::area_1m;
    else return err(CELL_EINVAL);
    StackVar id(ctx, m.base(), 16);
    const s32 r = sc(ctx, SYS_MMAPPER_UNMAP_SHARED_MEMORY, {addr, id.addr()});
    if (r) return r;
    const u32 n = m.r32(rec + 4) - 1;
    m.w32(rec + 4, n);
    if (n == 0) {
        if (flags != 0x200) {
            const s32 f = sc(ctx, SYS_MMAPPER_FREE_ADDRESS, {m.r32(rec)});
            if (f) return f;
        }
        m.w32(rec, 0);
    }
    return sc(ctx, SYS_MMAPPER_FREE_SHARED_MEMORY, {m.r32(id.addr())});
}

// ------------------------------------------------------------------------------------------------- the channels

// sys_mutex_create / sys_cond_create with libsysutil's attributes, retried every second while lv2 answers ESRCH (a
// side that finds the system's objects waits for them). `flags`: 1 to create them (the system's side), 2 to find them.
s32 create_channel_objects(PPUContext& ctx, u32 g, u32 channel, s32 flags)
{
    Mem m(ctx);
    StackVar attr(ctx, m.base(), 48);
    const u32 a = attr.addr();
    // sys_mutex_attribute_t: protocol, recursive, pshared, adaptive, ipc_key, flags, pad, name[8].
    m.w32(a + 0, 2);      // SYS_SYNC_PRIORITY
    m.w32(a + 4, 0x20);   // SYS_SYNC_NOT_RECURSIVE
    m.w32(a + 8, 0x100);  // SYS_SYNC_PROCESS_SHARED
    m.w32(a + 12, 0x2000); // SYS_SYNC_NOT_ADAPTIVE
    m.w32(a + 16, static_cast<u32>(kChannelKey >> 32));
    m.w32(a + 20, 0x20 | channel);
    m.w32(a + 24, static_cast<u32>(flags));
    m.put(a + 32, "_s__shm", 8);
    for (;;) {
        const s32 r = sc(ctx, SYS_MUTEX_CREATE, {g + glob::mutex(channel), a});
        if (r == 0) break;
        if (r != err(CELL_ESRCH)) return r;
        sc(ctx, SYS_TIMER_USLEEP, {1000000});
    }
    // sys_cond_attribute_t: pshared, flags, ipc_key, name[8].
    static constexpr const char* kNames[] = {"_s__nem", "_s__nfl", "_s__stf", "_s__ctf"};
    for (u32 i = 0; i < 4; i++) {
        m.fill(a, 0, 48);
        m.w32(a + 0, 0x100);
        m.w32(a + 4, static_cast<u32>(flags));
        m.w32(a + 8, static_cast<u32>(kChannelKey >> 32));
        m.w32(a + 12, (0x30 + 0x10 * i) | channel);
        m.put(a + 16, kNames[i], 8);
        for (;;) {
            const s32 r =
                sc(ctx, SYS_COND_CREATE, {g + glob::cond_data(channel) + 4 * i, m.r32(g + glob::mutex(channel)), a});
            if (r == 0) break;
            if (r != err(CELL_ESRCH)) return r;
            sc(ctx, SYS_TIMER_USLEEP, {1000000});
        }
    }
    return 0;
}

// The channels closed (libsysutil's code for it at its stop): each channel's closed byte set, then every condition
// signalled to all, the request side's first; the first failure ends it.
s32 close_channels(PPUContext& ctx, u32 g)
{
    Mem m(ctx);
    for (u32 ch = 0; ch < 2; ch++) {
        m.w8(m.r32(g + glob::shm) + ring::kChannel * ch + ring::closed, 1);
        for (const u32 c : {glob::cond_request(ch), glob::cond_system(ch), glob::cond_room(ch), glob::cond_data(ch)}) {
            if (const s32 r = sc(ctx, SYS_COND_SIGNAL_ALL, {m.r32(g + c)})) return r;
        }
    }
    return 0;
}

// Before anything that needs libsysutil's start.
void ensure_started(PPUContext& ctx)
{
    PacketState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.started) packets_start(ctx);
}

// A function body; memory that is not there ends it (logged with the call site), answering CELL_EFAULT where
// libsysutil's code faults.
template <class Body>
s32 guarded(PPUContext& ctx, std::string_view what, Body&& body)
{
    try {
        return body();
    } catch (const Fault& f) {
        log_call_site(ctx, guest_base(ctx),
            std::format("{}: guest memory at {:#010x} is not there (libsysutil faults there): CELL_EFAULT", what,
                f.addr));
        return err(CELL_EFAULT);
    }
}

// ------------------------------------------------------------------------------------------------- exports

s32 cellSysutilPacketBegin(PPUContext& ctx, u32 channel, s32 id)
{
    ensure_started(ctx);
    return guarded(ctx, "cellSysutilPacketBegin", [&] { return packet_begin(ctx, channel, id); });
}

s32 cellSysutilPacketEnd(PPUContext& ctx, u32 channel, s32 id)
{
    ensure_started(ctx);
    return guarded(ctx, "cellSysutilPacketEnd", [&] { return packet_end(ctx, channel, id); });
}

s32 cellSysutilPacketRead(PPUContext& ctx, u32 channel, s32 id, u32 buffer, u32 size, u32 done)
{
    ensure_started(ctx);
    return guarded(ctx, "cellSysutilPacketRead",
        [&] { return packet_read(ctx, channel, id, buffer, size, done); });
}

s32 cellSysutilPacketWrite(PPUContext& ctx, u32 channel, s32 id, u32 data, u64 size, u8 last)
{
    ensure_started(ctx);
    return guarded(ctx, "cellSysutilPacketWrite",
        [&] { return packet_write(ctx, channel, id, data, size, last); });
}

s32 cellSysutilSharedMemoryAlloc(PPUContext& ctx, s32 side, u64 key, u32 size, s32 container, u64 flags, u32 out,
    u32 entries, s32 count)
{
    ensure_started(ctx);
    return guarded(ctx, "cellSysutilSharedMemoryAlloc",
        [&] { return shared_memory_alloc(ctx, side, key, size, container, flags, out, entries, count); });
}

s32 cellSysutilSharedMemoryFree(PPUContext& ctx, u32 side, u32 addr, u64 flags)
{
    (void)side; // libsysutil does not look at it
    ensure_started(ctx);
    return guarded(ctx, "cellSysutilSharedMemoryFree",
        [&] { return shared_memory_free(ctx, addr, flags); });
}

} // namespace

// ------------------------------------------------------------------------------------------------- shared API

u32 globals(PPUContext& ctx)
{
    PacketState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.globals) {
        s.globals = HleProcess::current()->main_memory().alloc(glob::kBlockSize, 0x10000);
        if (!s.globals) PS_FATAL(kChan, "no main memory left for libsysutil's globals ({:#x} bytes)", +glob::kBlockSize);
        ps_write32(guest_base(ctx), s.globals + glob::hinban, s.globals + glob::empty);
    }
    return s.globals;
}

s32 packets_start(PPUContext& ctx)
{
    PacketState& s = state();
    std::lock_guard lock(s.mutex);
    if (s.started) return 0;
    s.started = true;
    const u32 g = globals(ctx);
    try {
        Mem m(ctx);
        if (const s32 r = shared_memory_alloc(ctx, 0, kSysutilShmKey, 0x10000, -1, 0x200, g + glob::shm, 0, 0)) {
            PS_LOG_ERROR(kChan, "libsysutil's channels: the shared memory failed ({:#010x})", static_cast<u32>(r));
            return r;
        }
        for (u32 ch = 0; ch < 2; ch++) {
            m.w32(g + glob::packet_id(ch), 0xFFFFFFFF);
            if (const s32 r = create_channel_objects(ctx, g, ch, 1)) {
                PS_LOG_ERROR(kChan, "libsysutil's channel {}: its lv2 objects failed ({:#010x})", ch, static_cast<u32>(r));
                return r;
            }
        }
        return close_channels(ctx, g);
    } catch (const Fault& f) {
        PS_LOG_ERROR(kChan, "libsysutil's channels: guest memory at {:#010x} is not there", f.addr);
        return err(CELL_EINVAL);
    }
}

s32 packet_begin(PPUContext& ctx, u32 channel, s32 id)
{
    if (channel > 1) return err(PACKET_ERROR_CHANNEL);
    Mem m(ctx);
    const u32 g = globals(ctx);
    const u32 h = m.r32(g + glob::shm) + ring::kChannel * channel;
    if (const s32 r = sc(ctx, SYS_MUTEX_LOCK, {m.r32(g + glob::mutex(channel)), 0})) return r;
    const u32 busy = h + (id == 0 ? ring::system_busy : ring::request_busy);
    const u32 cond = g + (id == 0 ? glob::cond_system(channel) : glob::cond_request(channel));
    while (m.r8(busy)) {
        if (m.r8(h + ring::closed)) {
            sc(ctx, SYS_MUTEX_UNLOCK, {m.r32(g + glob::mutex(channel))});
            return err(PACKET_ERROR_CLOSED);
        }
        if (const s32 r = sc(ctx, SYS_COND_WAIT, {m.r32(cond), 0})) return r;
    }
    m.w8(busy, 1);
    m.w32(g + glob::packet_id(channel), static_cast<u32>(id));
    return sc(ctx, SYS_MUTEX_UNLOCK, {m.r32(g + glob::mutex(channel))});
}

s32 packet_end(PPUContext& ctx, u32 channel, s32 id)
{
    if (channel > 1) return err(PACKET_ERROR_CHANNEL);
    Mem m(ctx);
    const u32 g = globals(ctx);
    const u32 h = m.r32(g + glob::shm) + ring::kChannel * channel;
    if (const s32 r = sc(ctx, SYS_MUTEX_LOCK, {m.r32(g + glob::mutex(channel)), 0})) return r;
    if (id == 0) {
        if (const s32 r = sc(ctx, SYS_COND_SIGNAL, {m.r32(g + glob::cond_system(channel))})) return r;
        m.w8(h + ring::system_busy, 0);
    } else {
        if (const s32 r = sc(ctx, SYS_COND_SIGNAL, {m.r32(g + glob::cond_request(channel))})) return r;
        m.w8(h + ring::request_busy, 0);
    }
    m.w32(g + glob::packet_id(channel), 0xFFFFFFFF);
    return sc(ctx, SYS_MUTEX_UNLOCK, {m.r32(g + glob::mutex(channel))});
}

s32 packet_read(PPUContext& ctx, u32 channel, s32 id, u32 buffer, u32 size, u32 done)
{
    if (channel > 1) return err(PACKET_ERROR_CHANNEL);
    Mem m(ctx);
    const u32 g = globals(ctx);
    const u32 h = m.r32(g + glob::shm) + ring::kChannel * channel;
    const s32 packet = static_cast<s32>(m.r32(g + glob::packet_id(channel)));
    if (packet != id) return err(PACKET_ERROR_NOT_OPEN);
    // The reader takes the other side's direction: the system reads requests (1), a request the system's (2).
    const u32 direction = packet == 0 ? 1 : 2;
    u32 left = size;
    bool finished = false;
    while (left != 0 && !finished) {
        if (const s32 r = sc(ctx, SYS_MUTEX_LOCK, {m.r32(g + glob::mutex(channel)), 0})) return r;
        while (m.r32(h + ring::direction) != direction || m.r32(h + ring::used) == 0) {
            if (m.r8(h + ring::closed)) {
                sc(ctx, SYS_MUTEX_UNLOCK, {m.r32(g + glob::mutex(channel))});
                return err(PACKET_ERROR_CLOSED);
            }
            if (const s32 r = sc(ctx, SYS_COND_WAIT, {m.r32(g + glob::cond_data(channel)), 0})) return r;
        }
        u32 used = 0;
        do {
            const u32 rd = m.r32(h + ring::read), wr = m.r32(h + ring::write);
            u32 chunk = wr > rd ? wr - rd : m.r32(h + ring::size) - rd;
            if (chunk > left) chunk = left;
            m.copy(buffer, h + m.r32(h + ring::data) + rd, chunk);
            left -= chunk;
            buffer += chunk;
            const u32 next = m.r32(h + ring::read) + chunk;
            m.w32(h + ring::read, next);
            if (next >= m.r32(h + ring::size)) m.w32(h + ring::read, 0);
            used = m.r32(h + ring::used) - chunk;
            m.w32(h + ring::used, used);
        } while (left != 0 && used != 0);
        if (m.r32(h + ring::state) == 2 && used == 0) {
            finished = true;
            m.w32(h + ring::direction, 0);
            m.w32(h + ring::state, 0);
        }
        if (const s32 r = sc(ctx, SYS_COND_SIGNAL, {m.r32(g + glob::cond_room(channel))})) return r;
        if (const s32 r = sc(ctx, SYS_MUTEX_UNLOCK, {m.r32(g + glob::mutex(channel))})) return r;
    }
    if (done) {
        m.w8(done, finished ? 1 : 0);
    } else if (!finished) {
        return err(PACKET_ERROR_TRUNCATED);
    }
    return static_cast<s32>(size - left);
}

s32 packet_write(PPUContext& ctx, u32 channel, s32 id, u32 data, u64 size, u8 last)
{
    if (size == 0) return 0;
    if (channel > 1) return err(PACKET_ERROR_CHANNEL);
    Mem m(ctx);
    const u32 g = globals(ctx);
    const u32 h = m.r32(g + glob::shm) + ring::kChannel * channel;
    const s32 packet = static_cast<s32>(m.r32(g + glob::packet_id(channel)));
    if (packet != id) return err(PACKET_ERROR_NOT_OPEN);
    u32 left = static_cast<u32>(size);
    while (left != 0) {
        if (const s32 r = sc(ctx, SYS_MUTEX_LOCK, {m.r32(g + glob::mutex(channel)), 0})) return r;
        while (m.r32(h + ring::state) == 2 || m.r32(h + ring::used) >= m.r32(h + ring::size)) {
            if (m.r8(h + ring::closed)) {
                sc(ctx, SYS_MUTEX_UNLOCK, {m.r32(g + glob::mutex(channel))});
                return err(PACKET_ERROR_CLOSED);
            }
            if (const s32 r = sc(ctx, SYS_COND_WAIT, {m.r32(g + glob::cond_room(channel)), 0})) return r;
        }
        m.w32(h + ring::state, 1);
        m.w32(h + ring::direction, packet != 0 ? 1 : 2);
        while (m.r32(h + ring::used) < m.r32(h + ring::size)) {
            const u32 rd = m.r32(h + ring::read), wr = m.r32(h + ring::write);
            u32 chunk = rd > wr ? rd - wr : m.r32(h + ring::size) - wr;
            if (chunk > left) chunk = left;
            m.copy(h + m.r32(h + ring::data) + wr, data, chunk);
            left -= chunk;
            data += chunk;
            const u32 next = m.r32(h + ring::write) + chunk;
            m.w32(h + ring::write, next);
            if (next >= m.r32(h + ring::size)) m.w32(h + ring::write, 0);
            m.w32(h + ring::used, m.r32(h + ring::used) + chunk);
            if (left == 0) break;
        }
        if (last != 0 && left == 0) m.w32(h + ring::state, 2);
        if (const s32 r = sc(ctx, SYS_COND_SIGNAL, {m.r32(g + glob::cond_data(channel))})) return r;
        if (const s32 r = sc(ctx, SYS_MUTEX_UNLOCK, {m.r32(g + glob::mutex(channel))})) return r;
    }
    return 0;
}

void log_handover(PPUContext& ctx, std::string_view what, s32 result)
{
    {
        PacketState& s = state();
        std::lock_guard lock(s.mutex);
        if (!s.logged.emplace(what).second) return;
    }
    log_call_site(ctx, guest_base(ctx),
        std::format("{}: libsysutil hands this to the system software, which pspocket's console does not have (its "
                    "channels are closed, D-534): answered {:#010x} as libsysutil answers a closed channel",
            what, static_cast<u32>(result)));
}

void sysutil_printf(PPUContext& ctx, std::string_view fmt, std::initializer_list<u64> args)
{
    u8* b = guest_base(ctx);
    StackVar text(ctx, b, static_cast<u32>(fmt.size() + 1));
    std::memcpy(b + text.addr(), fmt.data(), fmt.size());
    std::vector<u64> a{text.addr()};
    a.insert(a.end(), args.begin(), args.end());
    const Function* f = registry().find("sysPrxForUser", recomp::nid_of("_sys_printf"));
    if (!f) PS_FATAL(kChan, "sysPrxForUser::_sys_printf, which libsysutil imports, is not pspocket's");
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    for (u32 i = 0; i < a.size() && i < 8; i++) ctx.gpr[3 + i] = a[i];
    f->fn(ctx, b);
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
}

void sysutil_error(PPUContext& ctx, std::string_view what, std::string_view file, s32 line)
{
    u8* b = guest_base(ctx);
    StackVar strings(ctx, b, static_cast<u32>(what.size() + file.size() + 2));
    const u32 w = strings.addr(), f = w + static_cast<u32>(what.size()) + 1;
    std::memcpy(b + w, what.data(), what.size());
    std::memcpy(b + f, file.data(), file.size());
    sysutil_printf(ctx, "ERROR: %s : %s - %d\n", {w, f, static_cast<u64>(static_cast<s64>(line))});
}

void register_packets(Registry& r)
{
    r.add("cellSysutil", {
        PS_HLE_FUNC(0xE186F9AC, cellSysutilPacketBegin),
        PS_HLE_FUNC(0x0B8D63AE, cellSysutilPacketEnd),
        PS_HLE_FUNC(0x2D96313F, cellSysutilPacketRead),
        PS_HLE_FUNC(0xA2720DF2, cellSysutilPacketWrite),
        PS_HLE_FUNC(0x51E7CC21, cellSysutilSharedMemoryAlloc),
        PS_HLE_FUNC(0xBC00D5EF, cellSysutilSharedMemoryFree),
    });
}

} // namespace pspocket::hle::sysutil
