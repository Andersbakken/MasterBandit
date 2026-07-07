#include <doctest/doctest.h>

#include <eventloop/mac/MacModifierKeys.h>

// Regression: flagsChanged derived press/release from "any modifier still set",
// so releasing one of two held modifiers reported a spurious press. The fix
// keys off the specific left/right device mask for the released key.

TEST_CASE("macModifierKeyIsDown: recognizes each modifier's device bit")
{
    CHECK(macModifierKeyIsDown(0x38, kMBDeviceLShift)); // left shift down
    CHECK(macModifierKeyIsDown(0x3C, kMBDeviceRShift)); // right shift down
    CHECK(macModifierKeyIsDown(0x3B, kMBDeviceLCtrl));
    CHECK(macModifierKeyIsDown(0x3E, kMBDeviceRCtrl));
    CHECK(macModifierKeyIsDown(0x3A, kMBDeviceLAlt));
    CHECK(macModifierKeyIsDown(0x3D, kMBDeviceRAlt));
    CHECK(macModifierKeyIsDown(0x37, kMBDeviceLCmd));
    CHECK(macModifierKeyIsDown(0x36, kMBDeviceRCmd));
}

TEST_CASE("macModifierKeyIsDown: chorded release is not a press")
{
    // Two shifts held, then release the left one: the generic shift bit is
    // still set (right shift held), but the left-shift device bit is clear.
    uint64_t rightShiftHeld = kMBDeviceRShift;
    CHECK_FALSE(macModifierKeyIsDown(0x38, rightShiftHeld)); // left shift up

    // Release shift while control is still held: shift key must read as up.
    uint64_t ctrlHeld = kMBDeviceLCtrl;
    CHECK_FALSE(macModifierKeyIsDown(0x38, ctrlHeld));
    // ...but the control key still reads as down under the same flags.
    CHECK(macModifierKeyIsDown(0x3B, ctrlHeld));
}

TEST_CASE("macModifierKeyIsDown: non-modifier keycode is never down")
{
    CHECK(macModifierMaskForKeyCode(0x00) == 0); // 'A'
    CHECK_FALSE(macModifierKeyIsDown(0x00, 0xFFFFFFFF));
}
