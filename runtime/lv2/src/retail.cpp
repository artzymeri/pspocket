// The unnamed slots a retail console's lv2 has handlers for that refuse a game (D-938, part 3), ported from the retail
// kernel's handlers (lv2_kernel.self of the 4.93 system software, read in inventory/firmware/kernel493 and never
// committed; the addresses below are the kernel's). RPCS3 names none of them and answers CELL_OK (null_func).
//
// Each handler starts with a check a game does not pass: the process's control flags at the system software's level
// (0x8000000000003560), the hypervisor repository's product mode (ss.param.product.mode, off on a retail console:
// 0x8000000000050CC8), or a subsystem the kernel keeps for the system software (Bluetooth, the USB Bluetooth audio
// driver, the media control keys, whose guards print "permission denied" on the kernel's console). A few check an
// argument first. What a game gets is below, with what the handler does past the check for the system software, which
// pspocket does not run: a root process (tests) meets the same answer, and the log says the system path is not here.

#include <array>
#include <atomic>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

enum class Gate : u8 {
    System,      // control flags at the system software's level, else CELL_ENOSYS
    SystemEperm, // the same check, CELL_EPERM
    ProductMode, // the repository's product mode, else CELL_ENOSYS
    Bluetooth,   // "BT: permission denied", CELL_ENOSYS
    UsbBtAudio,  // "USBBTAUDIO: permission denied", CELL_ENOSYS
    MediaKey,    // "MEDIACONTROLKEY: permission denied", CELL_ENOSYS
    Deprecated,  // "USBBTAUDIO (deprecate)", CELL_ENOSYS for everyone
    Always,      // no check: the answer for everyone
};

struct Slot {
    u32 number;
    const char* name; // pspocket's, for the logs
    Gate gate;
    Result answer;    // what a game gets
};

constexpr Result kEnosys = static_cast<Result>(CELL_ENOSYS);

constexpr Slot kSlots[] = {
    {24, "unnamed_24 (process creation with a caller's parameters, 0x800000000001AA54)", Gate::System, kEnosys},
    {54, "unnamed_54 (PPU scheduling quantum, 0x800000000001FCB0)", Gate::System, kEnosys},
    {55, "unnamed_55 (PPU scheduling share, 0x800000000001FD94)", Gate::System, kEnosys},
    {248, "unnamed_248 (SPU thread group's parameters, 0x800000000004A2FC)", Gate::System, kEnosys},
    {249, "unnamed_249 (SPU thread group's preemption, 0x800000000004A1C0)", Gate::System, kEnosys},
    {314, "unnamed_314 (an address space's page sizes, 0x80000000000675C0)", Gate::System, kEnosys},
    {315, "unnamed_315 (an address space's page sizes, 0x80000000000674F8)", Gate::System, kEnosys},
    {346, "unnamed_346 (0x8000000000077E04)", Gate::Always, CELL_OK},
    {354, "unnamed_354 (a memory container's child, 0x80000000000796E8)", Gate::System, kEnosys},
    {355, "unnamed_355 (system memory, 0x80000000000796E4)", Gate::System, kEnosys},
    {400, "unnamed_400 (system controller, product mode, 0x800000000000A248)", Gate::ProductMode, kEnosys},
    {401, "unnamed_401 (system controller, 0x800000000000B15C)", Gate::System, kEnosys},
    {404, "unnamed_404 (system controller, 0x800000000000B01C)", Gate::System, kEnosys},
    {405, "unnamed_405 (system controller, product mode, 0x800000000000A134)", Gate::ProductMode, kEnosys},
    {406, "unnamed_406 (system controller, product mode, 0x800000000000A078)", Gate::ProductMode, kEnosys},
    {407, "unnamed_407 (system controller, product mode, 0x8000000000009FD0)", Gate::ProductMode, kEnosys},
    {413, "unnamed_413 (system controller, 0x800000000000AEC8)", Gate::System, kEnosys},
    {414, "unnamed_414 (system controller, 0x800000000000AD88)", Gate::System, kEnosys},
    {415, "unnamed_415 (system controller, 0x800000000000A480)", Gate::System, kEnosys},
    {472, "unnamed_472 (module loading, 0x80000000000514CC)", Gate::SystemEperm, static_cast<Result>(CELL_EPERM)},
    {473, "unnamed_473 (module loading, 0x8000000000051454)", Gate::SystemEperm, static_cast<Result>(CELL_EPERM)},
    {474, "unnamed_474 (0x80000000000511EC)", Gate::Always, static_cast<Result>(CELL_EINVAL)},
    {475, "unnamed_475 (module loading, 0x80000000000511F8)", Gate::SystemEperm, static_cast<Result>(CELL_EPERM)},
    {476, "unnamed_476 (0x800000000005714C)", Gate::System, kEnosys},
    {553, "unnamed_553 (USB, 0x800000000025CF3C)", Gate::Always, kEnosys},
    {554, "unnamed_554 (USB, 0x800000000025CF88)", Gate::Always, kEnosys},
    {555, "unnamed_555 (USB, 0x800000000025CFD4)", Gate::Always, kEnosys},
    {560, "unnamed_560 (0x80000000000B444C)", Gate::System, kEnosys},
    {561, "unnamed_561 (0x80000000000B4378)", Gate::System, kEnosys},
    {562, "unnamed_562 (0x80000000000B4550)", Gate::System, kEnosys},
    {577, "unnamed_577 (pads, 0x80000000000C70A0)", Gate::System, kEnosys},
    {580, "unnamed_580 (Bluetooth, 0x80000000000EE988)", Gate::Bluetooth, kEnosys},
    {581, "unnamed_581 (Bluetooth, 0x80000000000EE764)", Gate::Bluetooth, kEnosys},
    {582, "unnamed_582 (Bluetooth, 0x80000000000EE520)", Gate::Bluetooth, kEnosys},
    {598, "unnamed_598 (Bluetooth, 0x80000000000EE49C)", Gate::Bluetooth, kEnosys},
    {599, "unnamed_599 (Bluetooth, 0x80000000000EDE44)", Gate::Bluetooth, kEnosys},
    {620, "unnamed_620 (media control keys, 0x80000000000E1158)", Gate::MediaKey, kEnosys},
    {636, "unnamed_636 (Bluetooth, 0x80000000000EEE14)", Gate::Bluetooth, kEnosys},
    {637, "unnamed_637 (Bluetooth, 0x80000000000EECFC)", Gate::Bluetooth, kEnosys},
    {638, "unnamed_638 (Bluetooth, 0x80000000000EF98C)", Gate::Bluetooth, kEnosys},
    {639, "unnamed_639 (USB Bluetooth audio, 0x80000000000FD058)", Gate::Always, kEnosys},
    {648, "unnamed_648 (USB Bluetooth audio, 0x80000000000FCF54)", Gate::Deprecated, kEnosys},
    {649, "unnamed_649 (USB Bluetooth audio, 0x80000000000FCF50)", Gate::Deprecated, kEnosys},
    {680, "unnamed_680 (USB Bluetooth audio, 0x80000000000FDAF8)", Gate::UsbBtAudio, kEnosys},
    {681, "unnamed_681 (USB Bluetooth audio, 0x80000000000FD990)", Gate::UsbBtAudio, kEnosys},
    {682, "unnamed_682 (USB Bluetooth audio, 0x80000000000FD8EC)", Gate::UsbBtAudio, kEnosys},
    {683, "unnamed_683 (USB Bluetooth audio, 0x80000000000FD7C0)", Gate::UsbBtAudio, kEnosys},
    {684, "unnamed_684 (USB Bluetooth audio, 0x80000000000FE110)", Gate::UsbBtAudio, kEnosys},
    {685, "unnamed_685 (USB Bluetooth audio, 0x80000000000FE07C)", Gate::UsbBtAudio, kEnosys},
    {686, "unnamed_686 (USB Bluetooth audio, 0x80000000000FDFE8)", Gate::UsbBtAudio, kEnosys},
    {687, "unnamed_687 (USB Bluetooth audio, 0x80000000000FDF54)", Gate::UsbBtAudio, kEnosys},
    {688, "unnamed_688 (USB Bluetooth audio, 0x80000000000FDEC0)", Gate::UsbBtAudio, kEnosys},
    {689, "unnamed_689 (USB Bluetooth audio, 0x80000000000FDE2C)", Gate::UsbBtAudio, kEnosys},
    {690, "unnamed_690 (USB Bluetooth audio, 0x80000000000FDD30)", Gate::UsbBtAudio, kEnosys},
    {691, "unnamed_691 (USB Bluetooth audio, 0x80000000000FDC34)", Gate::UsbBtAudio, kEnosys},
    {692, "unnamed_692 (USB Bluetooth audio, 0x80000000000FE2F0)", Gate::UsbBtAudio, kEnosys},
    {693, "unnamed_693 (USB Bluetooth audio, 0x80000000000FE23C)", Gate::UsbBtAudio, kEnosys},
    {694, "unnamed_694 (USB Bluetooth audio, 0x80000000000FCF4C)", Gate::Deprecated, kEnosys},
    {695, "unnamed_695 (USB Bluetooth audio, 0x80000000000FCF48)", Gate::Deprecated, kEnosys},
    {696, "unnamed_696 (USB Bluetooth audio, 0x80000000000FD0E4)", Gate::UsbBtAudio, kEnosys},
    {877, "unnamed_877 (0x8000000000231830)", Gate::System, kEnosys},
    {987, "unnamed_987 (a debug console's crash tests, 0x800000000008C880)", Gate::Always, kEnosys},
};

const Slot* slot_of(u32 number)
{
    for (const Slot& s : kSlots)
        if (s.number == number) return &s;
    return nullptr;
}

const char* console_line(Gate gate)
{
    switch (gate) {
    case Gate::Bluetooth: return "BT: permission denied";
    case Gate::UsbBtAudio: return "USBBTAUDIO: permission denied";
    case Gate::MediaKey: return "MEDIACONTROLKEY: permission denied";
    case Gate::Deprecated: return "USBBTAUDIO (deprecate)";
    default: return nullptr;
    }
}

std::array<std::atomic<bool>, kSyscallCount> g_logged{};

void gated_syscall(PpuThread& t, PPUContext& ctx)
{
    const u32 number = static_cast<u32>(ctx.gpr[11]);
    const Slot* s = slot_of(number);
    if (!s) PS_FATAL(kChan, "syscall {} is not a gated retail slot", number);
    if (!g_logged[number].exchange(true)) {
        const char* line = console_line(s->gate);
        PS_LOG_WARN(kChan, "syscall {} ({}): {}{}{}: {:#x} (first call at {:#010x}, thread {:#x}){}", number, s->name,
            s->gate == Gate::Always ? "the retail kernel's answer" : "a game does not pass the retail kernel's check",
            line ? "; the kernel's console says " : "", line ? line : "", static_cast<u32>(s->answer), ctx.cia, t.id,
            t.kernel.config().root && s->gate != Gate::Always && s->gate != Gate::Deprecated
                ? "; the system software's path past it is not pspocket's"
                : "");
    }
    ctx.gpr[3] = to_r3(s->answer);
}

} // namespace

// 679 (0x80000000000EA484): its first argument must be 1, 2 or 3 (else CELL_EINVAL); then the system software's level
// (else CELL_EPERM).
Result unnamed_679(PpuThread&, u32 op)
{
    if (op - 1 >= 3) return err(CELL_EINVAL);
    if (!g_logged[679].exchange(true)) {
        PS_LOG_WARN(kChan, "syscall 679: a game does not pass the retail kernel's check: CELL_EPERM");
    }
    return err(CELL_EPERM);
}

// 986 (0x8000000000089C00): a null argument is CELL_EFAULT; then the system software's level (else CELL_ENOSYS).
Result unnamed_986(PpuThread&, u32 arg)
{
    if (!arg) return err(CELL_EFAULT);
    if (!g_logged[986].exchange(true)) {
        PS_LOG_WARN(kChan, "syscall 986: a game does not pass the retail kernel's check: CELL_ENOSYS");
    }
    return err(CELL_ENOSYS);
}

void register_retail_syscalls(SyscallTable& table)
{
    for (const Slot& s : kSlots) table.bind_unnamed(s.number, s.name, gated_syscall);
    PS_LV2_BIND_UNNAMED(table, 679, unnamed_679);
    PS_LV2_BIND_UNNAMED(table, 986, unnamed_986);
}

} // namespace pspocket::lv2
