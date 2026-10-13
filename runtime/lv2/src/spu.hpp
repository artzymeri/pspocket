#pragma once

// lv2's SPU images, thread groups and threads (D-167), shared by spu.cpp and the kernel (process exit). Reference:
// RPCS3 rpcs3/Emu/Cell/lv2/sys_spu.h (structures, constants, lv2_spu_group) and rpcs3/Emu/Cell/SPUThread.h.

#include <array>
#include <condition_variable>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "internal.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/spu/runtime.h"

namespace pspocket::lv2 {

enum : s32 {
    SYS_SPU_THREAD_GROUP_TYPE_NORMAL = 0x00,
    SYS_SPU_THREAD_GROUP_TYPE_MEMORY_FROM_CONTAINER = 0x04,
    SYS_SPU_THREAD_GROUP_TYPE_NON_CONTEXT = 0x08,
    SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT = 0x18,
    SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM = 0x20,
};
enum : u32 {
    SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT = 0x1,
    SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT = 0x2,
    SYS_SPU_THREAD_GROUP_JOIN_TERMINATED = 0x4,
    SYS_SPU_THREAD_GROUP_EVENT_RUN = 1,
    SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION = 2,
    SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE = 4,
    SYS_SPU_THREAD_EVENT_USER = 1,
    SYS_SPU_IMAGE_TYPE_USER = 0,
    SYS_SPU_IMAGE_TYPE_KERNEL = 1,
    SYS_SPU_THREAD_OPTION_ASYNC_INTR_ENABLE = 1,
    SYS_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE = 2,
};
enum : u64 {
    SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY = 0xFFFFFFFF53505500ull,
    SYS_SPU_THREAD_EVENT_USER_KEY = 0xFFFFFFFF53505501ull,
    SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION_KEY = 0xFFFFFFFF53505503ull,
    SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE_KEY = 0xFFFFFFFF53505504ull,
};
enum : s32 { SYS_SPU_SEGMENT_TYPE_COPY = 1, SYS_SPU_SEGMENT_TYPE_FILL = 2, SYS_SPU_SEGMENT_TYPE_INFO = 4 };

// spu_group_status (READY and STOPPED are never entered, as in RPCS3).
enum class SpuGroupState : u32 {
    NotInitialized,
    Initialized,
    Ready,
    Waiting,
    Suspended,
    WaitingAndSuspended,
    Running,
    Stopped,
    Destroyed,
};

// sys_spu_segment (0x18 bytes in guest memory: type, ls, size, padding, then the address or fill value as the first
// word of a doubleword at +0x10).
struct SpuSegment {
    s32 type = 0;
    u32 ls = 0;
    u32 size = 0;
    u32 addr = 0;
};

// lv2_spu_image: a kernel image (sys_spu_image_open / _sys_spu_image_import): the segment table and the file's copy
// in user memory.
class SpuImage final : public Object {
public:
    static constexpr u32 kIdBase = 0x22000000;
    u32 id_base() const override { return kIdBase; }
    SpuImage(u32 entry, u32 segs, s32 nsegs, u32 alloc_size) : entry(entry), segs(segs), nsegs(nsegs), alloc_size(alloc_size)
    {
    }
    const u32 entry;
    const u32 segs; // the segment table; also the start of the allocation
    const s32 nsegs;
    const u32 alloc_size;
};

class SpuThreadGroup;

// An SPU thread: its local store, its context and the host thread that runs it.
class SpuThread {
public:
    SpuThread(Kernel::Impl& s, SpuThreadGroup& group, u32 id, u32 spu_num, std::string name, u32 option);
    ~SpuThread();
    SpuThread(const SpuThread&) = delete;
    SpuThread& operator=(const SpuThread&) = delete;

    Kernel::Impl& s;
    SpuThreadGroup& group;
    const u32 id;      // (index in the group << 24) | (group ID & 0xFFFFFF)
    const u32 spu_num; // the SPU number the game gave (the group DMA window's index)
    const std::string name;
    const u32 option;
    std::vector<u8> info; // the image's INFO segment as initialize found it (at most 256 bytes); set before it runs
    u8* ls = nullptr;
    SPUContext ctx{};
    Waiter waiter; // sys_spu_thread_receive_event
    // Held while lv2 answers a call the SPU made in its inbound mailbox (send_event and event_flag_set_bit from the
    // check of the mailbox to the answer, receive_event's four words), and by sys_spu_thread_write_spu_mb. A PPU's word
    // lands before or after an answer, never inside it, and a reply to the event the call sent never ahead of the call's
    // own answer: the receiver runs as soon as the event is queued (D-584). RPCS3 SPUThread.cpp set_ch_value reserves the
    // answer's place in the mailbox, under the group's mutex, before it sends.
    std::mutex answer_mutex;

    // Guarded by group.lock.
    std::array<std::shared_ptr<EventQueue>, 64> ports;                // sys_spu_thread_connect_event (spup)
    std::array<std::pair<u32, std::shared_ptr<EventQueue>>, 32> spuq{}; // sys_spu_thread_bind_queue
    u64 snr_config = 0;
    bool exit_status_valid = false; // sys_spu_thread_get_exit_status
    s32 exit_status = 0;
    // Set on the SPU thread by sys_spu_thread_exit (stop 0x102), read when it stops.
    bool stopped_by_exit = false;
    s32 last_exit_status = 0;

    bool start_host();         // creates the parked host thread
    void start_run();          // lets the host thread run the context once more
    void quit_and_join();      // ends the host thread (the context must be stopped or about to stop)
    void request_quit();       // asks the host thread to end after its current run

private:
    static void* entry(void* arg);
    void body();
    void run();
    void stopped();

    std::mutex run_mutex_;
    std::condition_variable run_cv_;
    u64 starts_ = 0, runs_ = 0; // guarded by run_mutex_
    bool quit_ = false;         // guarded by run_mutex_
    void* handle_ = nullptr;    // pthread_t
    bool joinable_ = false;
    std::shared_ptr<HostThreads> host_; // live count, shared with the kernel
};

// lv2_spu_group.
class SpuThreadGroup final : public Object {
public:
    static constexpr u32 kIdBase = 0x04000100;
    u32 id_base() const override { return kIdBase; }
    SpuThreadGroup(std::string name, u32 max_num, s32 prio, s32 type, std::shared_ptr<MemoryContainer> ct, bool scheduled,
        u32 mem_size)
        : name(std::move(name)), max_num(max_num), mem_size(mem_size), type(type), container(std::move(ct)),
          has_scheduler_context(scheduled), prio(prio)
    {
        threads_map.fill(-1);
    }

    const std::string name;
    const u32 max_num;
    const u32 mem_size;
    const s32 type;
    const std::shared_ptr<MemoryContainer> container; // null: the default container
    const bool has_scheduler_context;

    // Guarded by lock (Object::lock).
    s32 prio;
    SpuGroupState state = SpuGroupState::NotInitialized;
    u32 init = 0;    // threads initialized
    s32 exit_status = 0;
    u32 join_state = 0;
    u32 running = 0; // threads not yet stopped in this run
    u32 stop_count = 0;
    bool set_terminate = false;
    bool dbg_stopped = false; // a debugger's stop (sys_dbg_stop_threads, D-937): its threads park as when suspended
    Waiter* joiner = nullptr; // a PPU thread in sys_spu_thread_group_join
    std::array<std::shared_ptr<SpuThread>, 8> threads;
    std::array<s8, 256> threads_map{}; // spu_num -> index, -1: none
    std::array<std::pair<u32, std::vector<SpuSegment>>, 8> imgs; // entry, segments
    std::array<std::array<u64, 4>, 8> args{};
    std::shared_ptr<EventQueue> ep_run, ep_exception, ep_sysmodule;
    // The group DMA window (members by spu_num, below 8).
    ps_spu_group dma{};
    // State changes: the last thread stopping (stop_count), WAITING ending, a resume.
    std::condition_variable state_cv;
};

// Process exit / shutdown: every SPU thread is asked to stop, its waits aborted, its host thread to end.
void abort_spu_threads(Kernel::Impl& s);

// For raw and isolated SPUs (raw_spu.cpp): a SELF decrypted in place (else the bytes are left as they are; an error
// when it does not decrypt), an SPU ELF's entry (none when it is not one), and its PT_LOAD segments put in a local
// store.
Result decrypt_spu_self(Kernel::Impl& s, std::vector<u8>& bytes, std::string_view what);
std::optional<u32> spu_elf_entry(std::span<const u8> file);
bool load_spu_elf(std::span<const u8> file, u8* ls, u32& entry);
// Process exit / shutdown: raw SPUs are asked to stop, interrupt threads to end.
void abort_raw_spus(Kernel::Impl& s);
// The raw SPUs the process has created (sys_dbg_get_process_info).
u32 raw_spu_count(Kernel::Impl& s);

} // namespace pspocket::lv2
