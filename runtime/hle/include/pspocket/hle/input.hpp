#pragma once

// Controller input the host feeds to the game (sys_io's cellPad). pspocket presents one virtual PS3 controller, always
// connected on port 0, as a DualShock 3 (press-sensitive buttons, motion sensors, rumble). The iOS app will drive it
// from a touch overlay or a game controller; without input (pspocket-boot on the Mac) it stays at rest.

#include "pspocket/core/types.hpp"

namespace pspocket::hle {

class HleProcess;

struct PadInput {
    // CellPadData button[2] (CELL_PAD_CTRL_*): 0x80 left, 0x40 down, 0x20 right, 0x10 up, 0x08 start, 0x04 R3,
    // 0x02 L3, 0x01 select.
    u16 digital1 = 0;
    // CellPadData button[3]: 0x80 square, 0x40 cross, 0x20 circle, 0x10 triangle, 0x08 R1, 0x04 L1, 0x02 R2, 0x01 L2.
    u16 digital2 = 0;
    // Sticks, 0..255 with 128 at rest (button[4..7]: right x, right y, left x, left y).
    u8 right_x = 128, right_y = 128, left_x = 128, left_y = 128;
    // Pressure of the press-sensitive buttons, 0..255 (button[8..19]): right, left, up, down, triangle, circle, cross,
    // square, L1, R1, L2, R2.
    u8 pressure[12] = {};
    // Motion sensors (button[20..23]): x, y, z, gyro; RPCS3's resting values (y = 512 - 113, 1 G).
    u16 sensor[4] = {512, 399, 512, 512};
};

// Sets the state of the virtual controller (port 0) for the next cellPadGetData. Thread-safe.
void set_pad_input(HleProcess& process, const PadInput& input);

} // namespace pspocket::hle
