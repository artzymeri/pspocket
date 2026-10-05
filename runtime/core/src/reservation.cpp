// The shared 128-byte-line reservation system (plan section 6, "Atomics"; contract in pspocket/core/reservation.h).
//
// Design
// - Version table: kSlots slots, each on its own 128-byte host cache line (Apple silicon lines are 128 bytes), so
//   two hot guest lines never false-share a slot unless they hash to the same one. Guest line -> slot is a
//   multiplicative hash of the line index, which scatters neighbouring guest lines. 32768 slots x 128 bytes = 4 MB
//   of zero-initialised BSS; only touched slots cost physical memory.
// - Each slot is a seqlock and a stripe lock at once: the version is even when the slot is free and odd while a
//   store holds it. A store takes the lock with CAS(version -> version + 1), which also checks the caller's
//   reservation version, so a stale reservation fails before it touches memory. Success releases with +1 (net +2);
//   failure releases with -1 (net 0: nothing was written, other reservations stay valid). ps_res_notify_write adds
//   2, which keeps the parity, so the lock holder's release still lands on an even value and no bump is lost.
// - Snapshots (ps_res_acquire) are seqlock reads: version (acquire), 16 relaxed 8-byte loads, acquire fence,
//   version again and the 16 words again. A store in progress (odd) or one that completed in between (changed) makes
//   the reader retry, so a snapshot never contains half of a conditional store; a second read of different bytes
//   (plain stores landed during the first) retries too, so a snapshot never shows a plain store without the stores
//   its writer ordered before it (D-130).
// - Plain guest stores don't take the lock. Conditional stores therefore compare the whole line with the
//   snapshot (catching plain stores that happened before) and then write each changed 8-byte word with a CAS. A
//   CAS that fails means a plain store hit that word after the comparison: the bytes it changed are kept and ours
//   are merged around them, which is the outcome of the plain store being ordered after ours. Words the
//   conditional store doesn't change are never written, so a concurrent plain store to them is never lost.
// - Wake-ups (ps_res_wait_change) use a parking table of mutex + condition variable buckets, because waits need
//   timeouts (C++20 atomic wait has none, and os_sync_wait_on_address needs iOS 17.4). Each slot counts its
//   parked waiters; a store only touches the bucket when that count is non-zero (Dekker-style seq_cst pairing of
//   "waiters++ then read version" against "bump version then read waiters"), so the uncontended path costs one
//   extra load.
//
// Guest memory is accessed with __atomic builtins (relaxed unless stated), which compile to the same LDR/STR as
// the volatile accessors of be.h on ARM64 and keep ThreadSanitizer quiet about the seqlock data reads.

#include "pspocket/core/reservation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "pspocket/core/types.hpp"

using namespace pspocket;

namespace {

constexpr u32 kSlotBits = 15;
constexpr u32 kSlots = 1u << kSlotBits;
constexpr u32 kBucketBits = 8;
constexpr u32 kBuckets = 1u << kBucketBits;
constexpr u32 kWords = PS_RES_LINE_SIZE / 8;

struct alignas(128) Slot {
    std::atomic<u64> version{0};
    std::atomic<u32> waiters{0};
};

struct Bucket {
    std::mutex mutex;
    std::condition_variable cv;
};

Slot g_slots[kSlots];
Bucket g_buckets[kBuckets];

static_assert(sizeof(Slot) == 128);

inline u32 slot_index(u32 addr)
{
    const u32 line = addr >> 7;
    return (line * 0x9E3779B1u) >> (32 - kSlotBits);
}

inline Slot& slot_of(u32 addr) { return g_slots[slot_index(addr)]; }
inline Bucket& bucket_of(u32 index) { return g_buckets[index & (kBuckets - 1)]; }

inline u64* line_words(u8* base, u32 addr) { return reinterpret_cast<u64*>(base + PS_RES_LINE_ADDR(addr)); }

inline u64 load_word(const u64* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

inline bool line_equals(const u64* line, const u64* expected)
{
    for (u32 i = 0; i < kWords; i++) {
        if (load_word(line + i) != expected[i]) return false;
    }
    return true;
}

// 0xFF in every byte where a and b differ.
inline u64 byte_diff_mask(u64 a, u64 b)
{
    u64 x = a ^ b;
    x |= (x >> 4) & 0x0F0F0F0F0F0F0F0Full;
    x |= (x >> 2) & 0x3333333333333333ull;
    x |= (x >> 1) & 0x5555555555555555ull;
    return (x & 0x0101010101010101ull) * 0xFF;
}

// Replaces `expected` with `desired` in *word. If a plain store changed the word after the line comparison, the
// bytes it changed win (it is ordered after this store) and the remaining bytes take `desired`.
inline void cas_merge_word(u64* word, u64 expected, u64 desired)
{
    u64 cur = expected;
    u64 want = desired;
    while (!__atomic_compare_exchange_n(word, &cur, want, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        const u64 theirs = byte_diff_mask(cur, expected);
        want = (desired & ~theirs) | (cur & theirs);
        if (want == cur) return;
    }
}

inline void cpu_relax()
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

// Spin briefly, then give the core away: the lock holder may have been preempted.
inline void backoff(u32& spins)
{
    if (++spins < 64) {
        cpu_relax();
    } else {
        std::this_thread::yield();
    }
}

// Takes the slot lock if the version is still `version`. While another store holds the lock taken from the same
// version, waits for its outcome: if that store fails (it wrote nothing) the version returns and this one may
// still succeed, so a failing attempt never fails a valid reservation.
inline bool try_lock(Slot& slot, u64 version)
{
    if (version & 1) return false;
    u32 spins = 0;
    for (;;) {
        u64 cur = version;
        if (slot.version.compare_exchange_strong(cur, version + 1, std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return true;
        }
        if (cur != version + 1) return false; // changed (a store succeeded or a write was notified)
        backoff(spins);
    }
}

inline void lock_any(Slot& slot)
{
    u32 spins = 0;
    for (;;) {
        u64 v = slot.version.load(std::memory_order_relaxed);
        if (!(v & 1) &&
            slot.version.compare_exchange_weak(v, v + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }
        backoff(spins);
    }
}

inline void wake_waiters(Slot& slot)
{
    if (slot.waiters.load(std::memory_order_seq_cst) == 0) return;
    Bucket& bucket = bucket_of(static_cast<u32>(&slot - g_slots));
    { std::lock_guard lock(bucket.mutex); }
    bucket.cv.notify_all();
}

// Release after a successful store: version + 1 (now even, net +2), then wake waiters.
inline void unlock_bumped(Slot& slot)
{
    slot.version.fetch_add(1, std::memory_order_seq_cst);
    wake_waiters(slot);
}

// Release after a failed store: nothing was written, so the version returns to its value before the lock (plus
// any ps_res_notify_write bumps that happened meanwhile).
inline void unlock_unchanged(Slot& slot) { slot.version.fetch_sub(1, std::memory_order_release); }

} // namespace

extern "C" uint64_t ps_res_acquire(uint8_t* base, uint32_t addr, void* out128)
{
    Slot& slot = slot_of(addr);
    const u64* line = line_words(base, addr);
    u32 spins = 0;
    for (;;) {
        const u64 v1 = slot.version.load(std::memory_order_acquire);
        if (!(v1 & 1)) {
            u64 snap[kWords];
            for (u32 i = 0; i < kWords; i++) snap[i] = load_word(line + i);
            std::atomic_thread_fence(std::memory_order_acquire);
            // Plain stores don't move the version, and the 16 loads are not ordered among themselves: a line read while
            // the PPU writes it (new commands, lwsync, then the word that publishes them) could hold the last store next
            // to older values of the ones before it. A second read after the fence that finds the same bytes makes the
            // snapshot one the writer's order allows: a store seen in the first read has the stores ordered before it
            // visible to the second (D-130).
            if (slot.version.load(std::memory_order_relaxed) == v1 && line_equals(line, snap)) {
                std::memcpy(out128, snap, PS_RES_LINE_SIZE);
                return v1;
            }
        }
        backoff(spins);
    }
}

extern "C" uint64_t ps_res_version(uint32_t addr)
{
    return slot_of(addr).version.load(std::memory_order_acquire) & ~u64{1};
}

extern "C" bool ps_res_store_line(uint8_t* base, uint32_t addr, uint64_t version, const void* expected128,
    const void* data128)
{
    Slot& slot = slot_of(addr);
    if (!try_lock(slot, version)) return false;
    std::atomic_thread_fence(std::memory_order_release); // the lock is visible before any data store

    u64 expected[kWords];
    u64 data[kWords];
    std::memcpy(expected, expected128, PS_RES_LINE_SIZE);
    std::memcpy(data, data128, PS_RES_LINE_SIZE);

    u64* line = line_words(base, addr);
    if (!line_equals(line, expected)) {
        unlock_unchanged(slot);
        return false;
    }
    for (u32 i = 0; i < kWords; i++) {
        if (data[i] != expected[i]) cas_merge_word(line + i, expected[i], data[i]);
    }
    unlock_bumped(slot);
    return true;
}

extern "C" void ps_res_store_line_unconditional(uint8_t* base, uint32_t addr, const void* data128)
{
    Slot& slot = slot_of(addr);
    u64 data[kWords];
    std::memcpy(data, data128, PS_RES_LINE_SIZE);

    lock_any(slot);
    std::atomic_thread_fence(std::memory_order_release);
    u64* line = line_words(base, addr);
    for (u32 i = 0; i < kWords; i++) __atomic_store_n(line + i, data[i], __ATOMIC_RELAXED);
    unlock_bumped(slot);
}

extern "C" bool ps_res_store_conditional(uint8_t* base, uint32_t addr, uint32_t size, uint64_t value,
    uint64_t version, const void* expected128)
{
    if ((size != 4 && size != 8) || (addr & (size - 1)) != 0) return false;

    Slot& slot = slot_of(addr);
    if (!try_lock(slot, version)) return false;
    std::atomic_thread_fence(std::memory_order_release);

    u64 expected[kWords];
    std::memcpy(expected, expected128, PS_RES_LINE_SIZE);

    u64* line = line_words(base, addr);
    if (!line_equals(line, expected)) {
        unlock_unchanged(slot);
        return false;
    }

    // Build the new 8-byte word in guest byte order from the snapshot and the big-endian value.
    const u32 word_index = (addr & (PS_RES_LINE_SIZE - 1)) >> 3;
    const u64 old_word = expected[word_index];
    u64 new_word = old_word;
    if (size == 8) {
        const u64 be = __builtin_bswap64(value);
        std::memcpy(&new_word, &be, 8);
    } else {
        const u32 be = __builtin_bswap32(static_cast<u32>(value));
        std::memcpy(reinterpret_cast<u8*>(&new_word) + (addr & 4), &be, 4);
    }
    if (new_word != old_word) cas_merge_word(line + word_index, old_word, new_word);
    unlock_bumped(slot);
    return true;
}

extern "C" bool ps_res_wait_change(uint32_t addr, uint64_t version, uint64_t timeout_ns)
{
    const u32 index = slot_index(addr);
    Slot& slot = g_slots[index];
    Bucket& bucket = bucket_of(index);
    // Clamp so the deadline can't overflow the clock's signed representation (~31 years is "forever").
    constexpr u64 kMaxTimeoutNs = 1'000'000'000'000'000'000ull;
    if (timeout_ns > kMaxTimeoutNs) timeout_ns = kMaxTimeoutNs;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(static_cast<s64>(timeout_ns));

    u32 spins = 0;
    for (;;) {
        const u64 cur = slot.version.load(std::memory_order_acquire);
        if (cur & 1) {
            // A store holds the line and finishes within a few hundred nanoseconds: its result decides.
            backoff(spins);
            if (timeout_ns && std::chrono::steady_clock::now() >= deadline) return false;
            continue;
        }
        if (cur != version) return true;

        slot.waiters.fetch_add(1, std::memory_order_seq_cst);
        bool timed_out = false;
        {
            std::unique_lock lock(bucket.mutex);
            while (slot.version.load(std::memory_order_seq_cst) == version) {
                if (timeout_ns == 0) {
                    bucket.cv.wait(lock);
                } else if (bucket.cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                    timed_out = slot.version.load(std::memory_order_seq_cst) == version;
                    break;
                }
            }
        }
        slot.waiters.fetch_sub(1, std::memory_order_relaxed);
        if (timed_out) return false;
        spins = 0;
    }
}

extern "C" void ps_res_notify_write(uint32_t addr, uint32_t size)
{
    if (size == 0) return;
    const u64 first = PS_RES_LINE_ADDR(addr);
    const u64 end = u64{addr} + size;
    const u64 lines = (end - first + PS_RES_LINE_SIZE - 1) / PS_RES_LINE_SIZE;
    if (lines >= kSlots) {
        // Larger than the table: every slot is covered anyway.
        for (Slot& slot : g_slots) {
            slot.version.fetch_add(2, std::memory_order_seq_cst);
            wake_waiters(slot);
        }
        return;
    }
    for (u64 line = first; line < end; line += PS_RES_LINE_SIZE) {
        Slot& slot = slot_of(static_cast<u32>(line));
        slot.version.fetch_add(2, std::memory_order_seq_cst);
        wake_waiters(slot);
    }
}
