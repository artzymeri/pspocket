// Writes synthetic executables for the pspocket-boot end-to-end tests (tests/hle/CMakeLists.txt):
//   fault.elf       _start loads from unmapped guest address 0x80000000 (the boot host must name it and the PPU
//                   address)
//   wait_queue.elf  _start calls a function that creates an event queue named "bootwait" and waits on it forever (the
//                   thread report at the timeout must name the thread, the syscall, the queue and the caller)
//
//   make_boot_programs <dir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "synthetic.hpp"

using namespace pspocket;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kText = 0x10000; // the synthetic executable's text segment starts at file offset 0

// The synthetic executable with code written at guest addresses: _start is 0x10200, and the function slots after it
// (0x10240..0x103FF) are free to overwrite.
std::vector<u8> program(std::initializer_list<std::pair<u32, std::span<const u32>>> code)
{
    std::vector<u8> bytes = test::build_ppu_exec().bytes;
    for (const auto& [addr, words] : code) {
        for (usize i = 0; i < words.size(); i++) ps_st32(bytes.data() + (addr - kText) + 4 * i, words[i]);
    }
    return bytes;
}

bool write_file(const std::filesystem::path& path, const std::vector<u8>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: make_boot_programs <dir>\n");
        return 64;
    }
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);

    const u32 fault[] = {
        as::lis(3, -0x8000), // r3 = 0x80000000 (sign-extended; the EA is its low word)
        as::lwz(4, 0, 3),    // fault
        as::li(3, 0),
        as::li(11, 22), // _sys_process_exit(0): not reached
        as::sc(),
    };

    // _start (0x10200) calls wait_forever (0x10240), which saves its LR in _start's frame, so the report's backtrace
    // is the `sc` (0x10298) and the return into _start (0x10210).
    const u32 start[] = {
        as::mflr(0),
        as::std_(0, 16, 1),
        as::stdu(1, -0x80, 1),
        as::b_rel(0x10240 - 0x1020C, true), // bl wait_forever
        as::li(3, 0),
        as::li(11, 22), // _sys_process_exit(0): not reached
        as::sc(),
    };
    const u32 wait_forever[] = {
        as::mflr(0),
        as::std_(0, 16, 1),
        as::stdu(1, -0x80, 1),
        as::li(0, 1),
        as::stw(0, 0x40, 1), // sys_event_queue_attribute_t: protocol SYS_SYNC_FIFO,
        as::stw(0, 0x44, 1), //   type SYS_PPU_QUEUE,
        as::lis(0, 0x626F),
        as::ori(0, 0, 0x6F74),
        as::stw(0, 0x48, 1), //   name "boot"
        as::lis(0, 0x7761),
        as::ori(0, 0, 0x6974),
        as::stw(0, 0x4C, 1),  //        "wait"
        as::addi(3, 1, 0x50), // &queue_id
        as::addi(4, 1, 0x40), // &attribute
        as::li(5, 0),         // no IPC key
        as::li(6, 16),        // size
        as::li(11, 128),      // sys_event_queue_create
        as::sc(),
        as::lwz(3, 0x50, 1),
        as::li(4, 0),
        as::li(5, 0),    // no timeout
        as::li(11, 130), // sys_event_queue_receive: nothing ever sends
        as::sc(),        // 0x10298
        as::addi(1, 1, 0x80),
        as::ld(0, 16, 1),
        as::mtlr(0),
        as::blr(),
    };

    const bool ok = write_file(dir / "fault.elf", program({{0x10200, fault}})) &&
        write_file(dir / "wait_queue.elf", program({{0x10200, start}, {0x10240, wait_forever}}));
    return ok ? 0 : 1;
}
