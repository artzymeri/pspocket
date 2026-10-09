#pragma once

// Shared helpers for the lv2 tests: a kernel over fresh guest memory, host threads with lv2 identities that make
// syscalls through the real PPU `sc` hook (ps_ppu_syscall -> ps_lv2_syscall -> dispatcher), a guest scratch
// allocator, a host-function executor for kernel-created threads, and a log capture. Code that runs on a kernel thread
// or a test's own std::thread checks with THREAD_CHECK / THREAD_REQUIRE (thread_check.hpp, D-585), never with Catch2's
// CHECK and REQUIRE, which are not thread-safe.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/ppu/runtime.h"

#include "internal.hpp" // white-box access (wait-queue lengths) for ordering tests
#include "thread_check.hpp"

namespace pspocket::lv2::test {

// Syscall numbers used by the tests (recomp/common/data/syscalls.txt; checked against the name table in
// test_dispatch.cpp).
enum Sc : u32 {
    kGetPid = 1,
    kProcessExit3 = 3,
    kGetNumberOfObject = 12,
    kGetSdkVersion = 25,
    kGetParamSfo = 30,
    kThreadExit = 41,
    kThreadYield = 43,
    kThreadJoin = 44,
    kThreadDetach = 45,
    kThreadGetJoinState = 46,
    kThreadSetPriority = 47,
    kThreadGetPriority = 48,
    kThreadGetStackInfo = 49,
    kThreadCreate = 52,
    kThreadStart = 53,
    kThreadRename = 56,
    kTimerCreate = 70,
    kTimerDestroy = 71,
    kTimerGetInfo = 72,
    kTimerStart = 73,
    kTimerStop = 74,
    kTimerConnect = 75,
    kTimerDisconnect = 76,
    kFlagCreate = 82,
    kFlagDestroy = 83,
    kFlagWait = 85,
    kFlagTryWait = 86,
    kFlagSet = 87,
    kSemCreate = 90,
    kSemDestroy = 91,
    kSemWait = 92,
    kSemTryWait = 93,
    kSemPost = 94,
    kLwMutexCreate = 95,
    kLwMutexDestroy = 96,
    kLwMutexLock = 97,
    kLwMutexUnlock = 98,
    kLwMutexTryLock = 99,
    kMutexCreate = 100,
    kMutexDestroy = 101,
    kMutexLock = 102,
    kMutexTryLock = 103,
    kMutexUnlock = 104,
    kCondCreate = 105,
    kCondDestroy = 106,
    kCondWait = 107,
    kCondSignal = 108,
    kCondSignalAll = 109,
    kCondSignalTo = 110,
    kLwCondCreate = 111,
    kLwCondDestroy = 112,
    kLwCondQueueWait = 113,
    kSemGetValue = 114,
    kLwCondSignal = 115,
    kLwCondSignalAll = 116,
    kLwMutexUnlock2 = 117,
    kFlagClear = 118,
    kRwCreate = 120,
    kRwDestroy = 121,
    kRwRLock = 122,
    kRwTryRLock = 123,
    kRwRUnlock = 124,
    kRwWLock = 125,
    kRwTryWLock = 126,
    kRwWUnlock = 127,
    kQueueCreate = 128,
    kQueueDestroy = 129,
    kQueueReceive = 130,
    kQueueTryReceive = 131,
    kFlagCancel = 132,
    kQueueDrain = 133,
    kPortCreate = 134,
    kPortDestroy = 135,
    kPortConnectLocal = 136,
    kPortDisconnect = 137,
    kPortSend = 138,
    kFlagGet = 139,
    kPortConnectIpc = 140,
    kUsleep = 141,
    kTimeGetCurrent = 145,
    kTimebaseFrequency = 147,
    kSystemTime = 149,
    kContainerCreate = 341,
    kContainerDestroy = 342,
    kContainerGetSize = 343,
    kMemAllocate = 348,
    kMemFree = 349,
    kMemAllocateFromContainer = 350,
    kMemPageAttribute = 351,
    kMemUserSize = 352,
    kMmAllocateAddress = 330,
    kMmFreeAddress = 331,
    kMmAllocateShm = 332,
    kMmFreeShm = 329,
    kMmMap = 334,
    kMmUnmap = 335,
    kMmSearchAndMap = 337,
    kTtyRead = 402,
    kTtyWrite = 403,
    kFsOpen = 801,
    kFsRead = 802,
    kFsWrite = 803,
    kFsClose = 804,
    kFsOpendir = 805,
    kFsReaddir = 806,
    kFsClosedir = 807,
    kFsStat = 808,
    kFsFstat = 809,
    kFsMkdir = 811,
    kFsRename = 812,
    kFsRmdir = 813,
    kFsUnlink = 814,
    kFsLseek = 818,
    kFsFsync = 820,
    kFsTruncate = 831,
    kFsFtruncate = 832,
};

inline constexpr s32 rc(CellError e) { return static_cast<s32>(e); }

// Makes a syscall from `ctx` exactly as guest code does: r11 = number, r3.. = arguments, through ps_ppu_syscall.
template <class... A>
u64 syscall(PPUContext& ctx, u32 number, A... args)
{
    static_assert(sizeof...(A) <= 8);
    const u64 values[] = {static_cast<u64>(args)..., 0};
    for (usize i = 0; i < sizeof...(A); i++) ctx.gpr[3 + i] = values[i];
    ctx.gpr[11] = number;
    ps_ppu_syscall(&ctx, Kernel::current()->base());
    return ctx.gpr[3];
}

// Kernel + guest memory. Guest scratch for syscall arguments lives at kScratch (bump-allocated, thread-safe).
struct Fixture {
    static constexpr u32 kScratch = 0x00100000;
    static constexpr u32 kScratchSize = 0x00400000;

    GuestMemory mem;
    std::unique_ptr<Kernel> kernel;
    std::atomic<u32> next{kScratch};

    explicit Fixture(Config config = {})
    {
        REQUIRE(mem.reserve());
        REQUIRE(mem.commit(kScratch, kScratchSize));
        kernel = std::make_unique<Kernel>(mem, std::move(config));
        kernel->install();
    }
    ~Fixture()
    {
        kernel.reset();
        pspocket::test::report_thread_checks(); // what failed on the kernel's threads and the test's own
    }

    u8* base() const { return mem.base(); }

    u32 alloc(u32 size, u32 align = 16)
    {
        u32 cur = next.load();
        for (;;) {
            const u32 a = (cur + align - 1) & ~(align - 1);
            if (next.compare_exchange_weak(cur, a + size)) {
                THREAD_REQUIRE(a + size <= kScratch + kScratchSize);
                std::memset(base() + a, 0, size);
                return a;
            }
        }
    }

    u32 cstr(std::string_view s)
    {
        const u32 a = alloc(static_cast<u32>(s.size() + 1), 1);
        std::memcpy(base() + a, s.data(), s.size());
        base()[a + s.size()] = 0;
        return a;
    }

    u32 r32(u32 a) const { return ps_read32(base(), a); }
    u16 r16(u32 a) const { return ps_read16(base(), a); }
    u64 r64(u32 a) const { return ps_read64(base(), a); }
    void w32(u32 a, u32 v) const { ps_write32(base(), a, v); }
    void w64(u32 a, u64 v) const { ps_write64(base(), a, v); }
    // A PPU store after its sync (lwsync, then stw): a flag a running lv2 thread reads, set after the data it announces.
    void w32_release(u32 a, u32 v) const
    {
        __atomic_store_n(reinterpret_cast<u32*>(base() + a), ps_bswap32(v), __ATOMIC_RELEASE);
    }
};

// A host thread with an lv2 identity (Kernel::attach_thread). Construct it on the thread that uses it.
struct Caller {
    Kernel& kernel;
    PPUContext ctx{};
    PpuThread* thread = nullptr;

    Caller(Kernel& k, std::string_view name = "test", s32 prio = 1000) : kernel(k)
    {
        ps_ppu_context_init(&ctx);
        thread = k.attach_thread(ctx, name, prio);
    }
    ~Caller() { kernel.detach_thread(thread); }
    Caller(const Caller&) = delete;
    Caller& operator=(const Caller&) = delete;

    u32 id() const { return thread->id; }

    template <class... A>
    s32 call(u32 number, A... args)
    {
        return static_cast<s32>(syscall(ctx, number, args...));
    }
    template <class... A>
    u64 call64(u32 number, A... args)
    {
        return syscall(ctx, number, args...);
    }
};

// Executor for kernel-created threads: entry addresses map to host functions that receive the thread's context.
struct HostEntries {
    std::mutex mutex;
    std::map<u32, std::function<void(PPUContext&)>> fns;

    void add(u32 entry, std::function<void(PPUContext&)> fn)
    {
        std::lock_guard lock(mutex);
        fns[entry] = std::move(fn);
    }

    static void execute(void* user, PPUContext& ctx, u8*, u32 entry)
    {
        auto* self = static_cast<HostEntries*>(user);
        std::function<void(PPUContext&)> fn;
        {
            std::lock_guard lock(self->mutex);
            const auto it = self->fns.find(entry);
            THREAD_REQUIRE(it != self->fns.end()); // on the kernel's thread
            fn = it->second;
        }
        fn(ctx);
    }

    Config config(Config c = {})
    {
        c.executor = &HostEntries::execute;
        c.executor_user = this;
        return c;
    }
};

// Captures log lines while alive.
struct LogCapture {
    struct Line {
        log::Level level;
        std::string channel;
        std::string message;
    };
    std::mutex mutex;
    std::vector<Line> lines;

    LogCapture()
    {
        log::set_sink(
            [](void* user, log::Level level, std::string_view channel, std::string_view message) {
                auto* self = static_cast<LogCapture*>(user);
                std::lock_guard lock(self->mutex);
                self->lines.push_back({level, std::string(channel), std::string(message)});
            },
            this);
    }
    ~LogCapture() { log::set_sink(nullptr, nullptr); }

    bool contains(std::string_view channel, std::string_view text)
    {
        std::lock_guard lock(mutex);
        for (const Line& l : lines) {
            if (l.channel == channel && l.message.find(text) != std::string::npos) return true;
        }
        return false;
    }
};

// Polls `pred` until it is true or `timeout` passes.
template <class Pred>
bool eventually(Pred&& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

inline s64 elapsed_ms(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

// Attribute blocks in guest memory.
inline u32 mutex_attr(Fixture& f, u32 protocol, u32 recursive, u32 pshared = SYS_SYNC_NOT_PROCESS_SHARED, u64 key = 0,
    s32 flags = 0)
{
    const u32 a = f.alloc(40);
    f.w32(a, protocol);
    f.w32(a + 4, recursive);
    f.w32(a + 8, pshared);
    f.w32(a + 12, SYS_SYNC_NOT_ADAPTIVE);
    f.w64(a + 16, key);
    f.w32(a + 24, static_cast<u32>(flags));
    f.w64(a + 32, 0x6D75746578000000ull); // "mutex"
    return a;
}

inline u32 cond_attr(Fixture& f)
{
    const u32 a = f.alloc(24);
    f.w32(a, SYS_SYNC_NOT_PROCESS_SHARED);
    return a;
}

// rwlock / semaphore / event flag attributes share the first fields: protocol, pshared, ipc_key, flags, (type).
inline u32 sync_attr(Fixture& f, u32 protocol, u32 type = 0)
{
    const u32 a = f.alloc(32);
    f.w32(a, protocol);
    f.w32(a + 4, SYS_SYNC_NOT_PROCESS_SHARED);
    f.w32(a + 20, type);
    return a;
}

inline u32 queue_attr(Fixture& f, u32 protocol, u32 type = SYS_PPU_QUEUE)
{
    const u32 a = f.alloc(16);
    f.w32(a, protocol);
    f.w32(a + 4, type);
    return a;
}

// Number of threads queued on an object (white box), for tests that need a deterministic queue order.
template <class T>
usize queued(Kernel& k, u32 id)
{
    const auto obj = k.impl().objects.get<T>(id);
    THREAD_REQUIRE(obj);
    if constexpr (std::is_same_v<T, Cond>) {
        std::lock_guard lock(obj->mutex->lock);
        return obj->waiters.size();
    } else if constexpr (std::is_same_v<T, RwLock>) {
        std::lock_guard lock(obj->lock);
        return obj->read_waiters.size() + obj->write_waiters.size();
    } else if constexpr (std::is_same_v<T, LwMutex>) {
        std::lock_guard lock(obj->lock);
        return obj->sleepers.size();
    } else {
        std::lock_guard lock(obj->lock);
        return obj->waiters.size();
    }
}

} // namespace pspocket::lv2::test
