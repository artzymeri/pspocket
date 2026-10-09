// sys_libc part 5b (D-516): the process side of the C library, under sys_libc and sys_libm.
// - exit and atexit: atexit, __cxa_atexit and Dinkumware's _Atexit register functions run last-first by exit (and by
//   Sony's __call_functions_registered_with_atexit), which then flushes stdio and ends the process through liblv2's
//   sys_process_exit (its own exit list, then lv2). _Exit and abort end it at once (abort with EXIT_FAILURE, after
//   logging where it was called); exitspawn and _Exitspawn are exit's and _Exit's way into
//   sys_game_process_exitspawn. environ is an empty list.
// - Assertions: _Assert prints Dinkumware's "<message> -- assertion failed" on stderr and aborts; _Nonfatal_Assert
//   prints it and returns; Sony's _SCE_Assert prints the strings among its first arguments and aborts.
// - The library's locks: _Mtxinit/_Mtxdst/_Mtxlock/_Mtxunlock (Dinkumware's recursive mutexes, a host one per guest
//   address, nothing written into the guest's object), _Locksyslock/_Unlocksyslock (its numbered system locks),
//   _Initlocks/_Clearlocks (nothing to set up), _Once (Dinkumware's flag word: 0, 1 while the function runs, 2 after).
//   The variables _Mutex_attr and tls_mutex_attr: a sys_mutex_attribute_t for a recursive, priority-ordered mutex.
// - Thread-local storage (D-570): Dinkumware's _Tlsalloc/_Tlsfree/_Tlsset/_Tlsget/_Destroytls and the Win32 names
//   TlsGetValue/TlsSetValue over the same keys.
// - SPU printf (the protocol of Sony's SDK: an SPU's spu_printf sends a user event on SPU port 1 whose data is the LS
//   address of its arguments, then waits in its inbound mailbox for printf's result): spu_printf_initialize makes the
//   event queue and the server thread (an lv2 thread running this library's server) and registers the attach and
//   detach callbacks with liblv2 (_sys_spu_printf_initialize); spu_printf_attach_group/thread connect port 1;
//   spu_printf_finalize destroys the queue, which ends the server. The arguments in LS: quadwords, the format's LS
//   address in the first one's preferred word, then one argument per quadword in its preferred slot (word 0 for 32
//   bits, doubleword 0 for 64), as the SPU ABI keeps variadic arguments.
// - _Printf and _Scanf: Dinkumware's engines with the caller's output and input functions
//   (pfn(arg, text, n) returning the next arg, NULL to stop; pfn(arg, c, get) getting or pushing back a character).

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "libc_format.hpp"
#include "libc_guest.hpp"
#include "libc_malloc.hpp"
#include "prx.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace libc;

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// ------------------------------------------------------------------------------------------------- exit

struct ExitState {
    std::mutex mutex;
    struct Entry {
        u32 fn, arg;
        bool with_arg;
    };
    std::vector<Entry> entries;
};

ExitState& exits() { return HleProcess::current()->state<ExitState>(); }

s32 add_exit(u32 fn, u32 arg, bool with_arg)
{
    if (!fn) return -1;
    ExitState& s = exits();
    std::lock_guard l(s.mutex);
    s.entries.push_back({fn, arg, with_arg});
    return 0;
}

s32 atexit_(PPUContext&, u32 fn) { return add_exit(fn, 0, false); }
s32 _Atexit_(PPUContext&, u32 fn) { return add_exit(fn, 0, false); }
s32 __cxa_atexit_(PPUContext&, u32 fn, u32 arg, u32) { return add_exit(fn, arg, true); }

// Runs the registered functions last-first (one registered while they run runs too).
void run_exits(PPUContext& ctx)
{
    ExitState& s = exits();
    for (;;) {
        ExitState::Entry e;
        {
            std::lock_guard l(s.mutex);
            if (s.entries.empty()) return;
            e = s.entries.back();
            s.entries.pop_back();
        }
        if (e.with_arg) call_guest(ctx, e.fn, {e.arg});
        else call_guest(ctx, e.fn, {});
    }
}

void __call_functions_registered_with_atexit_(PPUContext& ctx) { run_exits(ctx); }

[[noreturn]] void lv2_exit(PPUContext& ctx, s32 status)
{
    lv2::_sys_process_exit(thread_of(ctx), status, 0, 0);
    PS_FATAL(kChan, "_sys_process_exit returned");
}

// liblv2's sys_process_exit: its exit list, then lv2.
[[noreturn]] void liblv2_exit(PPUContext& ctx, s32 status)
{
    if (const Function* f = registry().find("sysPrxForUser", recomp::nid_of("sys_process_exit"))) {
        ctx.gpr[3] = static_cast<u64>(static_cast<s64>(status));
        f->fn(ctx, gb(ctx));
    }
    lv2_exit(ctx, status);
}

void exit_(PPUContext& ctx, s32 status)
{
    run_exits(ctx);
    stdio_flush_all(ctx);
    liblv2_exit(ctx, status);
}

void _Exit_(PPUContext& ctx, s32 status) { lv2_exit(ctx, status); }

void abort_(PPUContext& ctx)
{
    refuse(ctx, "abort(): the program ends (EXIT_FAILURE)");
    lv2_exit(ctx, 1);
}

void exitspawn_(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 size, s32 prio, u64 flags)
{
    run_exits(ctx);
    stdio_flush_all(ctx);
    game_process_exitspawn(ctx, path, argv, envp, data, size, prio, flags);
}

void _Exitspawn_(PPUContext& ctx, u32 path, u32 argv, u32 envp, u32 data, u32 size, s32 prio, u64 flags)
{
    game_process_exitspawn(ctx, path, argv, envp, data, size, prio, flags);
}

// environ: a pointer (+0) to an empty list (+4: NULL).
void environ_init(u8* b, u32 a) { ps_write32(b, a, a + 4); }

// ------------------------------------------------------------------------------------------------- assertions

std::string strings_of(PPUContext& ctx, std::initializer_list<u64> regs)
{
    std::string out;
    for (const u64 r : regs) {
        const u32 a = static_cast<u32>(r);
        if (a < 0x10000) continue;
        if (const auto s = read_string(thread_of(ctx), a, 4096)) {
            if (!out.empty()) out += ' ';
            out += *s;
        }
    }
    return out;
}

void assert_message(PPUContext& ctx, std::string_view what, const std::string& text)
{
    const std::string line = std::format("{} -- assertion failed\n", text);
    tty_out(ctx, line);
    refuse(ctx, std::format("{}: {}", what, text));
}

void _Assert_(PPUContext& ctx, u32 mesg, u32 where)
{
    assert_message(ctx, "_Assert", strings_of(ctx, {mesg, where}));
    lv2_exit(ctx, 1);
}

void _Nonfatal_Assert_(PPUContext& ctx, u32 mesg, u32 where)
{
    assert_message(ctx, "_Nonfatal_Assert", strings_of(ctx, {mesg, where}));
}

void _SCE_Assert_(PPUContext& ctx)
{
    assert_message(ctx, "_SCE_Assert", strings_of(ctx, {ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]}));
    lv2_exit(ctx, 1);
}

// ------------------------------------------------------------------------------------------------- locks

struct Locks {
    std::mutex mutex;
    std::map<u32, std::unique_ptr<std::recursive_mutex>> by_address;
    std::array<std::recursive_mutex, 8> system;
};

Locks& locks() { return HleProcess::current()->state<Locks>(); }

std::recursive_mutex& mutex_at(u32 addr)
{
    Locks& l = locks();
    std::lock_guard g(l.mutex);
    auto& m = l.by_address[addr];
    if (!m) m = std::make_unique<std::recursive_mutex>();
    return *m;
}

void _Mtxinit_(PPUContext&, u32 mtx) { mutex_at(mtx); }

void _Mtxdst_(PPUContext&, u32 mtx)
{
    Locks& l = locks();
    std::lock_guard g(l.mutex);
    l.by_address.erase(mtx);
}

void _Mtxlock_(PPUContext&, u32 mtx) { mutex_at(mtx).lock(); }
void _Mtxunlock_(PPUContext&, u32 mtx) { mutex_at(mtx).unlock(); }
void _Initlocks_(PPUContext&) {}
void _Clearlocks_(PPUContext&) {}

void _Locksyslock_(PPUContext& ctx, s32 n)
{
    if (n < 0 || n >= 8) return refuse(ctx, std::format("_Locksyslock({}): no such lock", n));
    locks().system[static_cast<usize>(n)].lock();
}

void _Unlocksyslock_(PPUContext& ctx, s32 n)
{
    if (n < 0 || n >= 8) return refuse(ctx, std::format("_Unlocksyslock({}): no such lock", n));
    locks().system[static_cast<usize>(n)].unlock();
}

// void _Once(_Once_t *flag, void (*func)(void)): func once; callers that come while it runs wait for it.
void _Once_(PPUContext& ctx, u32 flag, u32 fn)
{
    if (!range_ok(ctx, "_Once", flag, 4)) return;
    u8* b = gb(ctx);
    if (atomic_load_be32(b, flag) == 2) return;
    if (atomic_cas_be32(b, flag, 0, 1) == 0) {
        call_guest(ctx, fn, {});
        atomic_store_be32(b, flag, 2);
        return;
    }
    while (atomic_load_be32(b, flag) != 2) std::this_thread::yield();
}

// ------------------------------------------------------------------------------------------------- thread-local storage

// Dinkumware's thread-local storage (xtls.h in its Win32 model, which the PS3's library follows: TlsGetValue and
// TlsSetValue beside the _Tls functions, D-570): 64 keys (Win32's TLS_MINIMUM_AVAILABLE), each with a destructor, and a
// value per key and thread (the lv2 thread's ID), 0 until set; a key made again starts at 0 in every thread.
struct Tls {
    static constexpr u32 kKeys = 64;
    std::mutex mutex;
    std::array<bool, kKeys> used{};
    std::array<u32, kKeys> destructor{};
    std::map<u32, std::array<u32, kKeys>> values; // by lv2 thread ID
};

Tls& tls() { return HleProcess::current()->state<Tls>(); }

// int _Tlsalloc(_Tlskey_t *key, void (*destructor)(void *)): the first free key; 0, or -1 when none is free.
s32 _Tlsalloc_(PPUContext& ctx, u32 key, u32 destructor)
{
    if (!range_ok(ctx, "_Tlsalloc", key, 4)) return -1;
    Tls& t = tls();
    std::lock_guard lock(t.mutex);
    for (u32 k = 0; k < Tls::kKeys; k++) {
        if (t.used[k]) continue;
        t.used[k] = true;
        t.destructor[k] = destructor;
        for (auto& [id, v] : t.values) v[k] = 0;
        ps_write32(gb(ctx), key, k);
        return 0;
    }
    return -1;
}

// int _Tlsfree(_Tlskey_t key): 0, or -1 for a key not in use.
s32 _Tlsfree_(PPUContext&, u32 key)
{
    Tls& t = tls();
    std::lock_guard lock(t.mutex);
    if (key >= Tls::kKeys || !t.used[key]) return -1;
    t.used[key] = false;
    t.destructor[key] = 0;
    return 0;
}

// int _Tlsset(_Tlskey_t key, void *value): the calling thread's value; 0, or -1 for a key not in use.
s32 _Tlsset_(PPUContext& ctx, u32 key, u32 value)
{
    Tls& t = tls();
    std::lock_guard lock(t.mutex);
    if (key >= Tls::kKeys || !t.used[key]) return -1;
    t.values[thread_of(ctx).id][key] = value;
    return 0;
}

// void *_Tlsget(_Tlskey_t key): the calling thread's value, 0 when unset or for a key not in use.
u32 _Tlsget_(PPUContext& ctx, u32 key)
{
    Tls& t = tls();
    std::lock_guard lock(t.mutex);
    if (key >= Tls::kKeys || !t.used[key]) return 0;
    const auto it = t.values.find(thread_of(ctx).id);
    return it == t.values.end() ? 0 : it->second[key];
}

u32 TlsGetValue_(PPUContext& ctx, u32 index) { return _Tlsget_(ctx, index); }

// BOOL TlsSetValue(DWORD index, void *value): non-zero on success.
s32 TlsSetValue_(PPUContext& ctx, u32 index, u32 value) { return _Tlsset_(ctx, index, value) == 0 ? 1 : 0; }

// void _Destroytls(void): the calling thread's values end: each non-zero one with a destructor is cleared and given
// to it, in rounds while destructors set values again (four at most, POSIX's PTHREAD_DESTRUCTOR_ITERATIONS); then the
// thread has none.
void _Destroytls_(PPUContext& ctx)
{
    Tls& t = tls();
    const u32 id = thread_of(ctx).id;
    for (u32 round = 0; round < 4; round++) {
        std::vector<std::pair<u32, u32>> calls; // destructor, value
        {
            std::lock_guard lock(t.mutex);
            const auto it = t.values.find(id);
            if (it == t.values.end()) return;
            for (u32 k = 0; k < Tls::kKeys; k++) {
                if (t.used[k] && t.destructor[k] && it->second[k]) {
                    calls.emplace_back(t.destructor[k], it->second[k]);
                    it->second[k] = 0;
                }
            }
        }
        if (calls.empty()) break;
        for (const auto& [fn, value] : calls) call_guest(ctx, fn, {value});
    }
    std::lock_guard lock(t.mutex);
    t.values.erase(id);
}

// sys_mutex_attribute_t: protocol SYS_SYNC_PRIORITY, SYS_SYNC_RECURSIVE, not process-shared, not adaptive, its name.
void mutex_attr(u8* b, u32 a, const char* name)
{
    ps_write32(b, a, 2);
    ps_write32(b, a + 4, 0x10);
    ps_write32(b, a + 8, 0x200);
    ps_write32(b, a + 12, 0x2000);
    std::memcpy(b + a + 32, name, std::min<usize>(std::strlen(name), 7));
}

void mutex_attr_init(u8* b, u32 a) { mutex_attr(b, a, "_Mutex"); }
void tls_mutex_attr_init(u8* b, u32 a) { mutex_attr(b, a, "_TlsMtx"); }

// ------------------------------------------------------------------------------------------------- SPU printf

// The arguments of an SPU's printf in its local store (see the top).
class SpuFormat final : public FormatSource {
public:
    // read(lsa, n, out): n bytes of LS; false when they cannot be read.
    using Reader = std::function<bool(u32, u32, u8*)>;
    SpuFormat(Reader read, u32 first) : read_(std::move(read)), next_(first) {}
    u64 next() override { return word(true); }
    u64 next_int(u32 bits) override { return bits > 32 ? word(false) : word(true); }
    double next_double() override { return std::bit_cast<double>(word(false)); }
    std::optional<std::string> string(u32 addr, u64 max) override
    {
        std::string s;
        u8 chunk[16];
        for (u32 a = addr & 0x3FFFF; s.size() < max;) {
            const u32 at = a & ~15u;
            if (!read_(at, 16, chunk)) return std::nullopt;
            for (u32 i = a - at; i < 16 && s.size() < max; i++, a++) {
                if (!chunk[i]) return s;
                s += static_cast<char>(chunk[i]);
            }
            if (a >= 0x40000) return std::nullopt;
        }
        return s;
    }
    std::optional<std::u32string> wstring(u32 addr, u64 max) override
    {
        std::u32string s;
        u8 w[4];
        for (u32 a = addr & 0x3FFFC; s.size() < max; a += 4) {
            if (a >= 0x40000 || !read_(a, 4, w)) return std::nullopt;
            const u32 c = ps_read32(w, 0);
            if (!c) return s;
            s += static_cast<char32_t>(c);
        }
        return s;
    }
    bool store(u32, u64, u32) override { return false; } // %n into an SPU's memory is not written

private:
    u64 word(bool w32)
    {
        u8 q[8];
        const u32 at = next_;
        next_ += 16;
        if (!read_(at, 8, q)) {
            problems.push_back(std::format("the argument at LS {:#x} cannot be read", at));
            return 0;
        }
        return w32 ? ps_read32(q, 0) : ps_read64(q, 0);
    }
    Reader read_;
    u32 next_;
};

// The text of the printf whose arguments are at `arg` in the SPU thread's local store; nullopt when its format cannot
// be read.
std::optional<std::string> spu_text(PPUContext& ctx, const char* fn, SpuFormat::Reader read, u32 arg)
{
    u8 q[4];
    if (!read(arg & 0x3FFF0, 4, q)) {
        refuse(ctx, std::format("{}: the arguments at LS {:#x} cannot be read", fn, arg));
        return std::nullopt;
    }
    SpuFormat src(read, (arg & 0x3FFF0) + 16);
    const auto fmt = src.string(ps_read32(q, 0), kMaxString);
    if (!fmt) {
        refuse(ctx, std::format("{}: the format at LS {:#x} cannot be read", fn, ps_read32(q, 0)));
        return std::nullopt;
    }
    FormatResult<char> r = format<char>(*fmt, src);
    for (const std::string& p : src.problems) refuse(ctx, std::format("{}: {}", fn, p));
    if (r.failed) return std::nullopt;
    return std::move(r.text);
}

SpuFormat::Reader thread_reader(PPUContext& ctx, u32 spu)
{
    return [&ctx, spu](u32 lsa, u32 n, u8* out) {
        lv2::PpuThread& t = thread_of(ctx);
        StackVar v(ctx, t.kernel.base(), 8);
        for (u32 i = 0; i < n; i += 8) {
            const u32 k = std::min<u32>(8, n - i);
            if (lv2::sys_spu_thread_read_ls(t, spu, lsa + i, v.addr(), k == 8 ? 8 : 4) != lv2::CELL_OK) return false;
            const u64 value = ps_read64(t.kernel.base(), v.addr());
            u8 bytes[8];
            if (k == 8) ps_write64(bytes, 0, value);
            else ps_write32(bytes, 0, static_cast<u32>(value));
            std::memcpy(out + i, bytes, k);
        }
        return true;
    };
}

// A raw SPU's local store is guest memory at 0xE0000000 + 0x100000 * id.
SpuFormat::Reader raw_reader(PPUContext& ctx, u32 id)
{
    return [&ctx, id](u32 lsa, u32 n, u8* out) {
        const u32 ea = 0xE0000000u + 0x100000u * id + (lsa & 0x3FFFF);
        if (id >= 6 || !guest_range(thread_of(ctx), ea, n)) return false;
        std::memcpy(out, gb(ctx) + ea, n);
        return true;
    };
}

s32 print_spu(PPUContext& ctx, const char* fn, SpuFormat::Reader read, u32 arg)
{
    const auto text = spu_text(ctx, fn, std::move(read), arg);
    if (!text) return -1;
    tty_out(ctx, *text);
    return static_cast<s32>(text->size());
}

s32 sprint_spu(PPUContext& ctx, const char* fn, u32 buf, u64 size, SpuFormat::Reader read, u32 arg)
{
    const auto text = spu_text(ctx, fn, std::move(read), arg);
    if (!text) return -1;
    if (size) {
        const usize k = std::min<u64>(text->size(), size - 1);
        if (!range_ok(ctx, fn, buf, k + 1)) return -1;
        std::memcpy(gb(ctx) + buf, text->data(), k);
        gb(ctx)[buf + k] = 0;
    }
    return static_cast<s32>(text->size());
}

s32 spu_thread_printf_(PPUContext& ctx, u32 spu, u32 arg) { return print_spu(ctx, "spu_thread_printf", thread_reader(ctx, spu), arg); }
s32 spu_thread_sprintf_(PPUContext& ctx, u32 buf, u32 spu, u32 arg)
{
    return sprint_spu(ctx, "spu_thread_sprintf", buf, ~u64{0}, thread_reader(ctx, spu), arg);
}
s32 spu_thread_snprintf_(PPUContext& ctx, u32 buf, u32 size, u32 spu, u32 arg)
{
    return sprint_spu(ctx, "spu_thread_snprintf", buf, size, thread_reader(ctx, spu), arg);
}
s32 raw_spu_printf_(PPUContext& ctx, u32 id, u32 arg) { return print_spu(ctx, "raw_spu_printf", raw_reader(ctx, id), arg); }
s32 spu_raw_sprintf_(PPUContext& ctx, u32 buf, u32 id, u32 arg)
{
    return sprint_spu(ctx, "spu_raw_sprintf", buf, ~u64{0}, raw_reader(ctx, id), arg);
}
s32 spu_raw_snprintf_(PPUContext& ctx, u32 buf, u32 size, u32 id, u32 arg)
{
    return sprint_spu(ctx, "spu_raw_snprintf", buf, size, raw_reader(ctx, id), arg);
}

constexpr u32 kSpuPrintfPort = 1, kEventUser = 1;

struct SpuPrintfState {
    std::mutex mutex;
    u32 queue = 0, thread = 0, handler = 0;
    bool on = false;
};

SpuPrintfState& sp() { return HleProcess::current()->state<SpuPrintfState>(); }

// The server: an SPU's event (data1 its thread, data3 its arguments' LS address) printed, printf's result sent back
// to its inbound mailbox; the queue's destruction (ECANCELED) ends it.
void _pspocket_spu_printf_server(PPUContext& ctx, u64 queue)
{
    lv2::PpuThread& t = thread_of(ctx);
    for (;;) {
        if (lv2::sys_event_queue_receive(t, static_cast<u32>(queue), 0, 0) != lv2::CELL_OK) break;
        const u32 spu = static_cast<u32>(ctx.gpr[5]), arg = static_cast<u32>(ctx.gpr[7]);
        u32 handler = 0;
        {
            std::lock_guard l(sp().mutex);
            handler = sp().handler;
        }
        s32 r;
        if (handler) {
            const auto text = spu_text(ctx, "spu_printf", thread_reader(ctx, spu), arg);
            if (text) {
                StackVar buf(ctx, gb(ctx), static_cast<u32>(text->size() + 1));
                write_string(t, buf.addr(), *text);
                call_guest(ctx, handler, {buf.addr()});
                r = static_cast<s32>(text->size());
            } else {
                r = -1;
            }
        } else {
            r = print_spu(ctx, "spu_printf", thread_reader(ctx, spu), arg);
        }
        lv2::sys_spu_thread_write_spu_mb(t, spu, static_cast<u32>(r));
    }
    lv2::_sys_ppu_thread_exit(t, 0);
}

s32 spu_printf_attach_group_(PPUContext& ctx, u32 group)
{
    u32 queue;
    {
        std::lock_guard l(sp().mutex);
        if (!sp().on) return lv2::err(lv2::CELL_ESTAT);
        queue = sp().queue;
    }
    lv2::PpuThread& t = thread_of(ctx);
    StackVar port(ctx, t.kernel.base(), 8);
    return lv2::sys_spu_thread_group_connect_event_all_threads(t, group, queue, u64{1} << kSpuPrintfPort, port.addr());
}

s32 spu_printf_detach_group_(PPUContext& ctx, u32 group)
{
    {
        std::lock_guard l(sp().mutex);
        if (!sp().on) return lv2::err(lv2::CELL_ESTAT);
    }
    return lv2::sys_spu_thread_group_disconnect_event_all_threads(thread_of(ctx), group, kSpuPrintfPort);
}

s32 spu_printf_attach_thread_(PPUContext& ctx, u32 spu)
{
    u32 queue;
    {
        std::lock_guard l(sp().mutex);
        if (!sp().on) return lv2::err(lv2::CELL_ESTAT);
        queue = sp().queue;
    }
    return lv2::sys_spu_thread_connect_event(thread_of(ctx), spu, queue, kEventUser, kSpuPrintfPort);
}

s32 spu_printf_detach_thread_(PPUContext& ctx, u32 spu)
{
    {
        std::lock_guard l(sp().mutex);
        if (!sp().on) return lv2::err(lv2::CELL_ESTAT);
    }
    return lv2::sys_spu_thread_disconnect_event(thread_of(ctx), spu, kEventUser, kSpuPrintfPort);
}

// int spu_printf_initialize(int prio, void (*handler)(const char *)): the queue (127 events, FIFO) and the server
// thread at `prio`; a handler gets each text instead of the TTY.
s32 spu_printf_initialize_(PPUContext& ctx, s32 prio, u32 handler)
{
    HleProcess& p = *HleProcess::current();
    {
        std::lock_guard l(sp().mutex);
        if (sp().on) return lv2::err(lv2::CELL_EBUSY);
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* b = t.kernel.base();
    const u32 entry = p.descriptor_of("sys_libc", recomp::nid_of("_pspocket_spu_printf_server"));
    if (!entry) {
        refuse(ctx, "spu_printf_initialize: the server function was not linked (sys_libc is not imported)");
        return lv2::err(lv2::CELL_ESTAT);
    }
    StackVar v(ctx, b, 0x60); // queue attribute (+0), queue ID (+0x20), thread ID (+0x28), param (+0x30), name (+0x40)
    ps_write32(b, v.addr(), 1);     // SYS_SYNC_FIFO
    ps_write32(b, v.addr() + 4, 1); // SYS_PPU_QUEUE
    if (const lv2::Result r = lv2::sys_event_queue_create(t, v.addr() + 0x20, v.addr(), 0, 127); r != lv2::CELL_OK)
        return r;
    const u32 queue = ps_read32(b, v.addr() + 0x20);
    const u32 tls = p.alloc_tls();
    ps_write32(b, v.addr() + 0x30, entry);
    ps_write32(b, v.addr() + 0x34, tls ? tls + kTlsR13Offset : 0);
    std::memcpy(b + v.addr() + 0x40, "spu_printf_server", 18);
    if (const lv2::Result r = lv2::_sys_ppu_thread_create(t, v.addr() + 0x28, v.addr() + 0x30, queue, 0, prio, 0x4000,
            1, v.addr() + 0x40);
        r != lv2::CELL_OK) {
        if (tls) p.free_tls(tls);
        lv2::sys_event_queue_destroy(t, queue, 1);
        return r;
    }
    const u32 thread = static_cast<u32>(ps_read64(b, v.addr() + 0x28));
    {
        std::lock_guard l(sp().mutex);
        sp().queue = queue;
        sp().thread = thread;
        sp().handler = handler;
        sp().on = true;
    }
    lv2::sys_ppu_thread_start(t, thread);
    // liblv2's sys_spu_printf_attach_* then reach this library (as the SDK's spu_printf_initialize registers them).
    const u32 cb[] = {p.descriptor_of("sys_libc", recomp::nid_of("_pspocket_spu_printf_agcb")),
        p.descriptor_of("sys_libc", recomp::nid_of("_pspocket_spu_printf_dgcb")),
        p.descriptor_of("sys_libc", recomp::nid_of("_pspocket_spu_printf_atcb")),
        p.descriptor_of("sys_libc", recomp::nid_of("_pspocket_spu_printf_dtcb"))};
    if (const Function* f = registry().find("sysPrxForUser", recomp::nid_of("_sys_spu_printf_initialize"));
        f && cb[0] && cb[1] && cb[2] && cb[3]) {
        u64 saved[4];
        for (u32 i = 0; i < 4; i++) {
            saved[i] = ctx.gpr[3 + i];
            ctx.gpr[3 + i] = cb[i];
        }
        f->fn(ctx, b);
        for (u32 i = 0; i < 4; i++) ctx.gpr[3 + i] = saved[i];
    }
    return lv2::CELL_OK;
}

// spu_printf_finalize: the queue destroyed (the server ends on it), the server joined.
s32 spu_printf_finalize_(PPUContext& ctx)
{
    u32 queue, thread;
    {
        std::lock_guard l(sp().mutex);
        if (!sp().on) return lv2::err(lv2::CELL_ESTAT);
        queue = sp().queue;
        thread = sp().thread;
        sp().on = false;
        sp().queue = sp().thread = sp().handler = 0;
    }
    lv2::PpuThread& t = thread_of(ctx);
    lv2::sys_event_queue_destroy(t, queue, 1); // SYS_EVENT_QUEUE_DESTROY_FORCE
    StackVar status(ctx, t.kernel.base(), 8);
    lv2::sys_ppu_thread_join(t, thread, status.addr());
    if (const Function* f = registry().find("sysPrxForUser", recomp::nid_of("_sys_spu_printf_finalize"))) f->fn(ctx, gb(ctx));
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- _Printf, _Scanf

// int _Printf(void *(*pfn)(void *, const char *, size_t), void *arg, const char *fmt, va_list ap): the text given to
// pfn in pieces of up to 512 bytes (each call returns the next arg; NULL stops it: EOF).
template <class C>
s32 printf_through(PPUContext& ctx, const char* fn, u32 pfn, u32 arg, u32 fmt, u32 ap)
{
    VarArgs args = VarArgs::list(ctx, ap);
    std::optional<std::basic_string<C>> f;
    if constexpr (std::is_same_v<C, char>) f = read_string(thread_of(ctx), fmt);
    else f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (!f) {
        refuse(ctx, std::format("{}: the format at {:#x} runs out of guest memory", fn, fmt));
        return -1;
    }
    class Src final : public FormatSource {
    public:
        Src(PPUContext& c, VarArgs& a) : ctx_(c), a_(a) {}
        u64 next() override { return a_.next(); }
        std::optional<std::string> string(u32 addr, u64 max) override { return read_string(thread_of(ctx_), addr, max); }
        std::optional<std::u32string> wstring(u32 addr, u64 max) override
        {
            return read_wstring(thread_of(ctx_), addr, max);
        }
        bool store(u32 addr, u64 value, u32 bytes) override
        {
            if (!guest_range(thread_of(ctx_), addr, bytes)) return false;
            for (u32 k = 0; k < bytes; k++) gb(ctx_)[addr + k] = static_cast<u8>(value >> (8 * (bytes - 1 - k)));
            return true;
        }

    private:
        PPUContext& ctx_;
        VarArgs& a_;
    } src(ctx, args);
    FormatResult<C> r = format<C>(*f, src);
    for (const std::string& p : src.problems) refuse(ctx, std::format("{}: {}", fn, p));
    if (r.failed) return -1;
    constexpr usize kPiece = 512;
    const u32 unit = sizeof(C) == 1 ? 1 : 4;
    for (usize at = 0; at < r.text.size(); at += kPiece) {
        const usize n = std::min(kPiece, r.text.size() - at);
        StackVar buf(ctx, gb(ctx), static_cast<u32>(n * unit + 4));
        for (usize i = 0; i < n; i++) {
            if constexpr (sizeof(C) == 1) gb(ctx)[buf.addr() + i] = static_cast<u8>(r.text[at + i]);
            else ps_write32(gb(ctx), buf.addr() + 4 * static_cast<u32>(i), static_cast<u32>(r.text[at + i]));
        }
        arg = static_cast<u32>(call_guest(ctx, pfn, {arg, buf.addr(), n}));
        if (!arg) return -1;
    }
    return static_cast<s32>(r.text.size());
}

s32 _Printf_(PPUContext& ctx, u32 pfn, u32 arg, u32 fmt, u32 ap) { return printf_through<char>(ctx, "_Printf", pfn, arg, fmt, ap); }
s32 _WPrintf_(PPUContext& ctx, u32 pfn, u32 arg, u32 fmt, u32 ap) { return printf_through<char32_t>(ctx, "_WPrintf", pfn, arg, fmt, ap); }

// int _Scanf(int (*pfn)(void *, int, int), void *arg, const char *fmt, va_list ap): pfn(arg, 0, 1) gets a character
// (EOF at the end), pfn(arg, c, 0) pushes c back.
template <class C>
s32 scanf_through(PPUContext& ctx, const char* fn, u32 pfn, u32 arg, u32 fmt, u32 ap)
{
    VarArgs args = VarArgs::list(ctx, ap);
    std::optional<std::basic_string<C>> f;
    if constexpr (std::is_same_v<C, char>) f = read_string(thread_of(ctx), fmt);
    else f = read_wstring(thread_of(ctx), fmt, kMaxString / 4);
    if (!f) {
        refuse(ctx, std::format("{}: the format at {:#x} runs out of guest memory", fn, fmt));
        return -1;
    }
    class In final : public ScanInput {
    public:
        In(PPUContext& c, u32 p, u32 a) : ctx_(c), pfn_(p), arg_(a) {}
        s64 get() override
        {
            const s32 c = static_cast<s32>(call_guest(ctx_, pfn_, {arg_, 0, 1}));
            return c < 0 ? -1 : (sizeof(C) == 1 ? static_cast<s64>(static_cast<u8>(c)) : static_cast<s64>(static_cast<u32>(c)));
        }
        void unget(s64 c) override
        {
            if (c >= 0) call_guest(ctx_, pfn_, {arg_, static_cast<u64>(c), 0});
        }

    private:
        PPUContext& ctx_;
        u32 pfn_, arg_;
    } in(ctx, pfn, arg);
    class Out final : public ScanTarget {
    public:
        Out(PPUContext& c, VarArgs& a) : ctx_(c), a_(a) {}
        u32 next_pointer() override { return static_cast<u32>(a_.next()); }
        bool put(u32 addr, const u8* data, usize size) override
        {
            if (!guest_range(thread_of(ctx_), addr, size)) return false;
            std::memcpy(gb(ctx_) + addr, data, size);
            return true;
        }

    private:
        PPUContext& ctx_;
        VarArgs& a_;
    } out(ctx, args);
    const int r = scan<C>(*f, in, out);
    for (const std::string& p : out.problems) refuse(ctx, std::format("{}: {}", fn, p));
    return r;
}

s32 _Scanf_(PPUContext& ctx, u32 pfn, u32 arg, u32 fmt, u32 ap) { return scanf_through<char>(ctx, "_Scanf", pfn, arg, fmt, ap); }
s32 _WScanf_(PPUContext& ctx, u32 pfn, u32 arg, u32 fmt, u32 ap) { return scanf_through<char32_t>(ctx, "_WScanf", pfn, arg, fmt, ap); }

} // namespace

#define LIBC(name) ::pspocket::hle::Function{{}, #name, ::pspocket::recomp::nid_of(#name), &detail::thunk<&name##_>, false}

void register_sys_libc_process(Registry& r)
{
    for (const std::string_view lib : {std::string_view("sys_libc"), std::string_view("sys_libm")}) {
        r.add(lib, {
            LIBC(atexit), LIBC(_Atexit), LIBC(__cxa_atexit), LIBC(__call_functions_registered_with_atexit),
            LIBC(exit), LIBC(_Exit), LIBC(abort), LIBC(exitspawn), LIBC(_Exitspawn), LIBC(_Assert),
            LIBC(_Nonfatal_Assert), LIBC(_SCE_Assert), LIBC(_Mtxinit), LIBC(_Mtxdst), LIBC(_Mtxlock),
            LIBC(_Mtxunlock), LIBC(_Initlocks), LIBC(_Clearlocks), LIBC(_Locksyslock), LIBC(_Unlocksyslock),
            LIBC(_Once), LIBC(spu_thread_printf), LIBC(spu_thread_sprintf), LIBC(spu_thread_snprintf),
            LIBC(raw_spu_printf), LIBC(spu_raw_sprintf), LIBC(spu_raw_snprintf), LIBC(spu_printf_initialize),
            LIBC(spu_printf_finalize), LIBC(spu_printf_attach_group), LIBC(spu_printf_detach_group),
            LIBC(spu_printf_attach_thread), LIBC(spu_printf_detach_thread), LIBC(_Printf), LIBC(_WPrintf),
            LIBC(_Scanf), LIBC(_WScanf), LIBC(_Tlsalloc), LIBC(_Tlsfree), LIBC(_Tlsset), LIBC(_Tlsget),
            LIBC(TlsGetValue), LIBC(TlsSetValue), LIBC(_Destroytls),
        });
    }
    // The server's body and the callbacks liblv2's sys_spu_printf_attach_* call: internal (linked when the game
    // imports sys_libc), so they have descriptors whether or not the game imports the public names.
    const auto internal = [](std::string_view name, HleFn fn) {
        return ::pspocket::hle::Function{{}, name, recomp::nid_of(name), fn, true};
    };
    r.add("sys_libc", {
        internal("_pspocket_spu_printf_server", &detail::thunk<&_pspocket_spu_printf_server>),
        internal("_pspocket_spu_printf_agcb", &detail::thunk<&spu_printf_attach_group_>),
        internal("_pspocket_spu_printf_dgcb", &detail::thunk<&spu_printf_detach_group_>),
        internal("_pspocket_spu_printf_atcb", &detail::thunk<&spu_printf_attach_thread_>),
        internal("_pspocket_spu_printf_dtcb", &detail::thunk<&spu_printf_detach_thread_>),
    });
    r.add_variables("sys_libc", {
        Variable{{}, "environ", recomp::nid_of("environ"), 8, 8, &environ_init},
        Variable{{}, "_Mutex_attr", recomp::nid_of("_Mutex_attr"), 40, 8, &mutex_attr_init},
        Variable{{}, "tls_mutex_attr", recomp::nid_of("tls_mutex_attr"), 40, 8, &tls_mutex_attr_init},
    });
}
#undef LIBC

} // namespace pspocket::hle
