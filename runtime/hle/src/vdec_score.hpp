#pragma once

// The PPU framework Sony's "s" video cores share (libsmvd4, libsmvd2, libsvc1d, libsjvtd; D-920 part 5), pspocket's
// own: what each core's PPU library does around its SPU program, with each core's own places and sizes passed in.
//
// Sony's s-cores are one design: a PPU library over an SPU task on the caller's SPURS taskset. Making an instance
// makes two SPURS queues of one 0x80-byte entry in the instance's memory (SPU to PPU, then PPU to SPU, each attached
// to an lv2 event queue) and starts the task; a call that needs the decoder pushes one command word to the task and
// pops its answer, then reads the status word the program left in the instance and turns it into the call's answer.
// The pictures wait in a queue of frame buffer slots (decoding order, a sequence number each), and the program keeps
// the two last anchor pictures' slots as references. Each slot has an information block in the instance's memory
// (the picture's addresses and size, its access units' side information and the caller's auxiliary data, a status)
// that GetPicture, GetSideInfoPic and GetAuxData read back.
//
// pspocket does the SPU program's part on the host (each core's file), so no task runs and the queues carry nothing;
// they are still made and attached as Sony's are (their errors are the core's answer, and they take an SPU port each)
// and detached when the instance is released. Kept on the host: the picture queue (slots, sequence numbers, the get
// and release counters, the references, the flush mark), whose places in the instance a caller never sees. Kept in
// guest memory at Sony's places (each core's): what a caller reads back, written field by field as Sony's program
// writes it, the gaps left as the caller's memory had them.
//
// The answers (every s-core alike, its own prefix: 0x806151xx libsmvd4, 0x806150xx libsmvd2, 0x806153xx libsvc1d,
// 0x806158xx libsjvtd): see answer() below. Nothing here is Sony's code or data (CLAUDE.md rule 1).

#include <map>
#include <memory>
#include <mutex>
#include <string_view>

#include "vdec_core_common.hpp"

#include "pspocket/hle/process.hpp"

namespace pspocket::hle::score {

// The low byte of an s-core's answers (prefix | low).
enum : u32 {
    LOW_BUSY = 0x21,      // no frame buffer slot free: DecodePicture refuses (its output 0)
    LOW_NO_PICTURE = 0x20,
    LOW_ARG = 0x41,       // a null pointer
    LOW_SEQ = 0x42,       // a type or level the core does not take
    LOW_EMPTY = 0x44,     // no picture (GetPicture, ReleasePicture, the side information)
    LOW_FATAL = 0x80,
    LOW_NO_MEMORY = 0x81, // too little memory; a stream too large for the instance
};
constexpr s32 code(u32 prefix, u32 low) { return static_cast<s32>(prefix | low); }

// The SPU programs' status bits the PPU libraries interpret.
enum : u32 {
    ST_NO_PICTURE = 0x1,         // -> 0x..20: the access unit gave no picture
    ST_WARNINGS = 0x1E,          // -> 0x..01 to 0x..0F (bit 4 -> 1, bits 1 to 3 as they are): a warning
    ST_PICTURE_WARNINGS = 0xF00, // GetPicture's: -> 0x..11 to 0x..1F (each bit ORs 0x11, 0x12, 0x14, 0x18)
    ST_EMPTY = 0x10000,          // -> 0x..44 (also GetPicture's when its block has no access unit)
    ST_ARG = 0x100000,           // -> 0x..41
    ST_SEQ = 0x200000,           // -> 0x..42
    ST_FLUSHED = 0x400000,       // -> 0x..22
    ST_BUSY = 0x800000,          // -> 0x..21
    ST_DEAD = 0x80000000,        // -> 0x..80: the program has stopped; with ST_NO_MEMORY_BIT -> 0x..81
    ST_NO_MEMORY_BIT = 0x01000000,
};

// A status word as the PPU libraries answer it (DecodePicture, FlushPicture, Discontinue, CancelDecode, ...): 0 and
// warnings alone 0 or 0x..01 to 0x..0F, then the errors in Sony's order of precedence.
inline s32 answer(u32 prefix, u32 st)
{
    if (!st) return lv2::CELL_OK;
    if (!(st & 0x80F100A1)) {
        if (!(st & ST_WARNINGS)) return lv2::CELL_OK;
        return code(prefix, ((st >> 4) & 1) | (st & 0xE));
    }
    if (st & ST_DEAD) return code(prefix, st & ST_NO_MEMORY_BIT ? LOW_NO_MEMORY : LOW_FATAL);
    if (st & 0x310000) return code(prefix, st & ST_ARG ? LOW_ARG : st & ST_SEQ ? LOW_SEQ : LOW_EMPTY);
    if (!(st & 0xC000A1)) return code(prefix, LOW_FATAL);
    if (st & 0x1) return code(prefix, 0x20);
    if (st & 0x80) return code(prefix, 0x28);
    if (st & 0x20) return code(prefix, 0x24);
    if (st & ST_FLUSHED) return code(prefix, 0x22);
    return code(prefix, st & ST_BUSY ? 0x21 : 0x00);
}

// Initialize's and CreateInstance2's: a warning is a fatal error there.
inline s32 strict_answer(u32 prefix, u32 st)
{
    if (st && !(st & 0x80F100A1)) return code(prefix, LOW_FATAL);
    return answer(prefix, st);
}

// GetPicture's, from the picture's status (the OR of its access units'): bits 8 to 11 are warnings of their own.
inline s32 picture_answer(u32 prefix, u32 st)
{
    if (st && !(st & 0x80F100A1)) {
        if (!(st & ST_PICTURE_WARNINGS)) return lv2::CELL_OK;
        u32 v = st & 0x100 ? 0x11 : 0;
        if (st & 0x200) v |= 0x12;
        if (st & 0x400) v |= 0x14;
        if (st & 0x800) v |= 0x18;
        return code(prefix, v);
    }
    return answer(prefix, st);
}

// ---- the picture queue

// The frame buffer slots and the pictures waiting in them, as an s-core's PPU library and SPU program keep them:
// a picture is ready with the next sequence number (mod 128); GetPicture takes the ready picture whose number is its
// counter's (and makes it the current one, whose side information the other calls read), ReleasePicture frees the one
// whose number is its own counter's. A slot is free when no picture waits in it and it is neither reference.
struct PictureQueue {
    static constexpr u32 kMaxSlots = 8;
    struct Slot {
        bool used = false;
        u8 seq = 0;
    };
    u32 slots = 3; // the core's
    Slot slot[kMaxSlots];
    u8 get = 0, release = 0, next_seq = 0;
    int current = -1, forward = -1, backward = -1; // -1: none
    bool flushed = false; // FlushPicture's mark: GetNumPicture answers 0x..22 until the next picture is decoded

    // Initialize and CancelDecode: no picture, no reference, the counters from 0.
    void reset()
    {
        for (Slot& s : slot) s = {};
        get = release = next_seq = 0;
        current = forward = backward = -1;
        flushed = false;
    }
    // The first free slot, -1 when none (DecodePicture's check, before it commands the program).
    int free_slot() const
    {
        for (int k = 0; k < static_cast<int>(slots); k++)
            if (!slot[k].used && k != forward && k != backward) return k;
        return -1;
    }
    // A picture decoded into `s` waits for GetPicture.
    void ready(int s)
    {
        slot[s].used = true;
        slot[s].seq = next_seq;
        next_seq = (next_seq + 1) & 0x7F;
    }
    // `s` is the newest anchor: the one before it the backward reference.
    void anchor(int s)
    {
        backward = forward;
        forward = s;
    }
    // FlushPicture: the newest anchor is no forward reference any more.
    void flush()
    {
        backward = forward;
        forward = -1;
        flushed = true;
    }
    // GetPicture: the next picture's slot (now the current one), or -1 (none: no current one either).
    int take()
    {
        current = -1;
        for (int k = 0; k < static_cast<int>(slots); k++)
            if (slot[k].used && slot[k].seq == get) current = k;
        if (current >= 0) get = (get + 1) & 0x7F;
        return current;
    }
    // ReleasePicture: false when no picture is due.
    bool release_one()
    {
        for (int k = 0; k < static_cast<int>(slots); k++)
            if (slot[k].used && slot[k].seq == release) {
                slot[k].used = false;
                release = (release + 1) & 0x7F;
                return true;
            }
        return false;
    }
    // GetNumPicture: the pictures decoded and not released.
    u32 count() const
    {
        u32 n = 0;
        for (u32 k = 0; k < slots; k++) n += slot[k].used ? 1 : 0;
        return n;
    }
};

// ---- instances

// A core's instances in this process, by their memory's address.
template <class T>
struct Instances {
    std::mutex m;
    std::map<u32, std::shared_ptr<T>> by_handle;

    static Instances& of_process(std::string_view chan)
    {
        HleProcess* p = HleProcess::current();
        if (!p) PS_FATAL(chan, "an s-core called without an HLE process");
        return p->state<Instances>();
    }
    std::shared_ptr<T> find(u32 handle)
    {
        std::lock_guard lk(m);
        const auto it = by_handle.find(handle);
        return it == by_handle.end() ? nullptr : it->second;
    }
    void add(u32 handle, std::shared_ptr<T> x)
    {
        std::lock_guard lk(m);
        by_handle[handle] = std::move(x);
    }
    bool remove(u32 handle)
    {
        std::lock_guard lk(m);
        return by_handle.erase(handle) != 0;
    }
};

// ---- the SPURS queues

// Where a core keeps its two queues in the instance's memory (each 128-byte aligned, a 0x80-byte buffer after it).
struct QueuePlaces {
    u32 to_ppu = 0, to_ppu_buffer = 0, to_spu = 0, to_spu_buffer = 0; // offsets in the instance's memory
};

// The queues an instance's making makes on the caller's taskset, in Sony's order (SPU to PPU, its lv2 event queue,
// then PPU to SPU and its own); the first error (cellSpurs's) is the core's answer, what was attached detached again.
inline s32 make_queues(PPUContext& ctx, u32 mem, u32 taskset, const QueuePlaces& q)
{
    using vdec_core::call_import;
    constexpr u32 kInit = 0x082BFB09, kAttach = 0xE5443BE7, kDetach = 0x039D70B7;
    s32 r = static_cast<s32>(call_import(ctx, "cellSpurs", kInit, {0, taskset, mem + q.to_ppu, mem + q.to_ppu_buffer,
        0x80, 1, 1}));
    if (r != lv2::CELL_OK) return r;
    r = static_cast<s32>(call_import(ctx, "cellSpurs", kAttach, {mem + q.to_ppu}));
    if (r != lv2::CELL_OK) return r;
    r = static_cast<s32>(call_import(ctx, "cellSpurs", kInit, {0, taskset, mem + q.to_spu, mem + q.to_spu_buffer,
        0x80, 1, 2}));
    if (r == lv2::CELL_OK) r = static_cast<s32>(call_import(ctx, "cellSpurs", kAttach, {mem + q.to_spu}));
    if (r != lv2::CELL_OK) call_import(ctx, "cellSpurs", kDetach, {mem + q.to_ppu});
    return r;
}

// ReleaseInstance: the queues detached from their lv2 event queues (PPU to SPU first, as Sony's).
inline void drop_queues(PPUContext& ctx, u32 mem, const QueuePlaces& q)
{
    using vdec_core::call_import;
    constexpr u32 kDetach = 0x039D70B7;
    call_import(ctx, "cellSpurs", kDetach, {mem + q.to_spu});
    call_import(ctx, "cellSpurs", kDetach, {mem + q.to_ppu});
}

// ---- what the calls copy out

// GetSideInfoPic's and GetAuxData's: the current picture's access units' blocks (`bytes` each, from `from` in its
// information block), the count to `count`, the first to `first`, the second (if any) to `second`.
inline void copy_blocks(u8* base, u32 count, u32 first, u32 second, u32 from, u32 n, u32 bytes)
{
    ps_write32(base, count, n);
    for (u32 k = 0; k < n && k < 2; k++) std::memmove(base + (k ? second : first), base + from + k * bytes, bytes);
}

// GetSideInfoSeq's: `size` bytes of the sequence information, but its gaps (the bytes Sony's copy leaves).
inline void copy_skipping(u8* base, u32 to, u32 from, u32 size, std::initializer_list<u32> gaps)
{
    for (u32 k = 0; k < size; k++) {
        bool gap = false;
        for (const u32 g : gaps) gap |= g == k;
        if (!gap) ps_write8(base, to + k, ps_read8(base, from + k));
    }
}

} // namespace pspocket::hle::score
