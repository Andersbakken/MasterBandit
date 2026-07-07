#pragma once

#include <cstdint>

// macOS device-dependent modifier flag masks (IOKit NX_DEVICE*KEYMASK). Unlike
// the generic NSEventModifierFlag* bits, these distinguish the left and right
// instance of each modifier, and NSEvent.modifierFlags carries them in its low
// bits. Values match the option masks already used in Window_cocoa.mm.
enum : uint64_t
{
    kMBDeviceLCtrl  = 0x00000001,
    kMBDeviceLShift = 0x00000002,
    kMBDeviceRShift = 0x00000004,
    kMBDeviceLCmd   = 0x00000008,
    kMBDeviceRCmd   = 0x00000010,
    kMBDeviceLAlt   = 0x00000020,
    kMBDeviceRAlt   = 0x00000040,
    kMBDeviceRCtrl  = 0x00002000,
    kMBFlagCapsLock = 0x00010000, // NSEventModifierFlagCapsLock (no L/R variant)
    kMBFlagFunction = 0x00800000, // NSEventModifierFlagFunction
};

// The modifier flag mask for a modifier key's macOS virtual keyCode, or 0 if
// keyCode is not a known modifier key. Left/right variants get distinct device
// masks so a chorded release (one of two held modifiers) is not mistaken for a
// press.
inline uint64_t macModifierMaskForKeyCode(unsigned short keyCode)
{
    switch (keyCode) {
        case 0x38: return kMBDeviceLShift; // left shift
        case 0x3C: return kMBDeviceRShift; // right shift
        case 0x3B: return kMBDeviceLCtrl;  // left control
        case 0x3E: return kMBDeviceRCtrl;  // right control
        case 0x3A: return kMBDeviceLAlt;   // left option
        case 0x3D: return kMBDeviceRAlt;   // right option
        case 0x37: return kMBDeviceLCmd;   // left command
        case 0x36: return kMBDeviceRCmd;   // right command
        case 0x39: return kMBFlagCapsLock; // caps lock (toggle)
        case 0x3F: return kMBFlagFunction; // fn
        default: return 0;
    }
}

// True if the modifier key with the given virtual keyCode is currently down,
// per the NSEvent modifier-flag mask. False for a non-modifier keyCode.
inline bool macModifierKeyIsDown(unsigned short keyCode, uint64_t modifierFlags)
{
    uint64_t mask = macModifierMaskForKeyCode(keyCode);
    return mask != 0 && (modifierFlags & mask) != 0;
}
