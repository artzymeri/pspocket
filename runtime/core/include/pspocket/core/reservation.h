/*
 * The shared reservation system (plan section 6, "Atomics"): PPU lwarx/ldarx + stwcx./stdcx. and SPU GETLLAR +
 * PUTLLC/PUTLLUC all reserve 128-byte lines of guest memory through this one API. C-callable, so lifted code and
 * both interpreters use it directly.
 *
 * Model
 * - A reservation is a snapshot of the 128-byte line plus the line's version at snapshot time.
 * - A conditional store succeeds only if the version is unchanged AND the line's bytes still equal the snapshot
 *   (the byte comparison catches plain stores by other threads, which don't bump versions). On success it writes and
 *   bumps the version, which fails every other reservation on that line and wakes waiters.
 * - Versions live in a hashed table, so unrelated lines can share a counter. That only causes spurious failures,
 *   which the architecture allows for both stwcx. and PUTLLC; guest code retries.
 * - Guest bytes are passed and compared in guest (big-endian) order; values are in host order.
 *
 * Implementation: runtime/core/src/reservation.cpp. Stress-tested by tests/core (litmus tests).
 */
#ifndef PSPOCKET_CORE_RESERVATION_H
#define PSPOCKET_CORE_RESERVATION_H

#include <stdbool.h>
#include <stdint.h>

#define PS_RES_LINE_SIZE 128u
#define PS_RES_LINE_ADDR(addr) ((uint32_t)(addr) & ~(uint32_t)(PS_RES_LINE_SIZE - 1))

#ifdef __cplusplus
extern "C" {
#endif

/* Consistent snapshot of the line containing addr into out128 (128 bytes, guest byte order). Returns the line's
 * version. Never observes a half-written conditional store. */
uint64_t ps_res_acquire(uint8_t* base, uint32_t addr, void* out128);

/* Current version of the line containing addr. */
uint64_t ps_res_version(uint32_t addr);

/* SPU PUTLLC: if the line's version is still `version` and its bytes equal `expected128`, write `data128` (whole
 * line, guest byte order), bump the version and return true. Otherwise change nothing and return false. */
bool ps_res_store_line(uint8_t* base, uint32_t addr, uint64_t version, const void* expected128, const void* data128);

/* SPU PUTLLUC and other unconditional line writes: write the line, bump the version. */
void ps_res_store_line_unconditional(uint8_t* base, uint32_t addr, const void* data128);

/* PPU stwcx. (size 4) / stdcx. (size 8): like ps_res_store_line, but writes only `size` bytes of `value` (host
 * order; stored big-endian) at addr, which must be naturally aligned. `expected128` is the snapshot taken by
 * ps_res_acquire at lwarx/ldarx time. */
bool ps_res_store_conditional(uint8_t* base, uint32_t addr, uint32_t size, uint64_t value, uint64_t version,
    const void* expected128);

/* Blocks until the version of the line containing addr differs from `version`, or `timeout_ns` elapses
 * (0 = no timeout). Returns true if the version changed. Used for the SPU reservation-lost event and to turn
 * lwarx polling loops into waits instead of spinning. */
bool ps_res_wait_change(uint32_t addr, uint64_t version, uint64_t timeout_ns);

/* For code that writes guest memory outside the conditional-store path but must break reservations promptly
 * (DMA PUTs, HLE functions writing sync objects): bumps the versions covering [addr, addr + size) and wakes waiters.
 * Plain lifted stores don't call this; conditional stores catch them through the byte comparison. */
void ps_res_notify_write(uint32_t addr, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* PSPOCKET_CORE_RESERVATION_H */
