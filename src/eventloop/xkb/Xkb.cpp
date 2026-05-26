#include "Xkb.h"

#include <Utf8.h>

#include <xkbcommon/xkbcommon.h>

namespace mb::xkb {

Key keysymToKey(uint32_t sym)
{
    // Bindings store the canonical (uppercase) form for ASCII letters —
    // Key_A..Z = 0x41..0x5A, no Key_a..z. Shift is reported separately in
    // the modifier mask, so normalize lowercase here. Without this, e.g.
    // ctrl+e fails to match (xkb returns 'e' = 0x65, outside Key_A..Z).
    if (sym >= 'a' && sym <= 'z') {
        sym = sym - 'a' + 'A';
    }
    // Latin-1 and ASCII printable range maps directly
    if (sym >= 0x20 && sym <= 0x7e) {
        return static_cast<Key>(sym);
    }
    if (sym >= 0xa0 && sym <= 0xff) {
        return static_cast<Key>(sym);
    }

    switch (sym) {
        case XKB_KEY_Escape: return Key_Escape;
        case XKB_KEY_Tab: return Key_Tab;
        case XKB_KEY_ISO_Left_Tab: return Key_Backtab;
        case XKB_KEY_BackSpace: return Key_Backspace;
        case XKB_KEY_Return: return Key_Return;
        case XKB_KEY_KP_Enter: return Key_Enter;
        case XKB_KEY_Insert: return Key_Insert;
        case XKB_KEY_Delete: return Key_Delete;
        case XKB_KEY_Pause: return Key_Pause;
        case XKB_KEY_Print: return Key_Print;
        case XKB_KEY_Home: return Key_Home;
        case XKB_KEY_End: return Key_End;
        case XKB_KEY_Left: return Key_Left;
        case XKB_KEY_Up: return Key_Up;
        case XKB_KEY_Right: return Key_Right;
        case XKB_KEY_Down: return Key_Down;
        case XKB_KEY_Page_Up: return Key_PageUp;
        case XKB_KEY_Page_Down: return Key_PageDown;
        case XKB_KEY_Shift_L: return Key_Shift_L;
        case XKB_KEY_Shift_R: return Key_Shift_R;
        case XKB_KEY_Control_L: return Key_Control_L;
        case XKB_KEY_Control_R: return Key_Control_R;
        case XKB_KEY_Alt_L: return Key_Alt_L;
        case XKB_KEY_Alt_R: return Key_Alt_R;
        case XKB_KEY_Super_L: return Key_Super_L;
        case XKB_KEY_Super_R: return Key_Super_R;
        case XKB_KEY_Hyper_L: return Key_Hyper_L;
        case XKB_KEY_Hyper_R: return Key_Hyper_R;
        case XKB_KEY_Menu: return Key_Menu;
        case XKB_KEY_Help: return Key_Help;
        case XKB_KEY_Caps_Lock: return Key_CapsLock;
        case XKB_KEY_Num_Lock: return Key_NumLock;
        case XKB_KEY_Scroll_Lock: return Key_ScrollLock;
        case XKB_KEY_F1: return Key_F1;
        case XKB_KEY_F2: return Key_F2;
        case XKB_KEY_F3: return Key_F3;
        case XKB_KEY_F4: return Key_F4;
        case XKB_KEY_F5: return Key_F5;
        case XKB_KEY_F6: return Key_F6;
        case XKB_KEY_F7: return Key_F7;
        case XKB_KEY_F8: return Key_F8;
        case XKB_KEY_F9: return Key_F9;
        case XKB_KEY_F10: return Key_F10;
        case XKB_KEY_F11: return Key_F11;
        case XKB_KEY_F12: return Key_F12;
        case XKB_KEY_F13: return Key_F13;
        case XKB_KEY_F14: return Key_F14;
        case XKB_KEY_F15: return Key_F15;
        case XKB_KEY_F16: return Key_F16;
        case XKB_KEY_F17: return Key_F17;
        case XKB_KEY_F18: return Key_F18;
        case XKB_KEY_F19: return Key_F19;
        case XKB_KEY_F20: return Key_F20;
        case XKB_KEY_F21: return Key_F21;
        case XKB_KEY_F22: return Key_F22;
        case XKB_KEY_F23: return Key_F23;
        case XKB_KEY_F24: return Key_F24;
        case XKB_KEY_F25: return Key_F25;
        case XKB_KEY_F26: return Key_F26;
        case XKB_KEY_F27: return Key_F27;
        case XKB_KEY_F28: return Key_F28;
        case XKB_KEY_F29: return Key_F29;
        case XKB_KEY_F30: return Key_F30;
        case XKB_KEY_F31: return Key_F31;
        case XKB_KEY_F32: return Key_F32;
        case XKB_KEY_F33: return Key_F33;
        case XKB_KEY_F34: return Key_F34;
        case XKB_KEY_F35: return Key_F35;
        case XKB_KEY_KP_0: return Key_KP_0;
        case XKB_KEY_KP_1: return Key_KP_1;
        case XKB_KEY_KP_2: return Key_KP_2;
        case XKB_KEY_KP_3: return Key_KP_3;
        case XKB_KEY_KP_4: return Key_KP_4;
        case XKB_KEY_KP_5: return Key_KP_5;
        case XKB_KEY_KP_6: return Key_KP_6;
        case XKB_KEY_KP_7: return Key_KP_7;
        case XKB_KEY_KP_8: return Key_KP_8;
        case XKB_KEY_KP_9: return Key_KP_9;
        case XKB_KEY_KP_Decimal: return Key_KP_Decimal;
        case XKB_KEY_KP_Divide: return Key_KP_Divide;
        case XKB_KEY_KP_Multiply: return Key_KP_Multiply;
        case XKB_KEY_KP_Subtract: return Key_KP_Subtract;
        case XKB_KEY_KP_Add: return Key_KP_Add;
        case XKB_KEY_KP_Equal: return Key_KP_Equal;
        case XKB_KEY_KP_Separator: return Key_KP_Separator;
        case XKB_KEY_KP_Left: return Key_KP_Left;
        case XKB_KEY_KP_Right: return Key_KP_Right;
        case XKB_KEY_KP_Up: return Key_KP_Up;
        case XKB_KEY_KP_Down: return Key_KP_Down;
        case XKB_KEY_KP_Page_Up: return Key_KP_PageUp;
        case XKB_KEY_KP_Page_Down: return Key_KP_PageDown;
        case XKB_KEY_KP_Home: return Key_KP_Home;
        case XKB_KEY_KP_End: return Key_KP_End;
        case XKB_KEY_KP_Insert: return Key_KP_Insert;
        case XKB_KEY_KP_Delete: return Key_KP_Delete;
        case XKB_KEY_KP_Begin: return Key_KP_Begin;
        case XKB_KEY_Meta_L: return Key_Meta_L;
        case XKB_KEY_Meta_R: return Key_Meta_R;
        case XKB_KEY_ISO_Level3_Shift: return Key_AltGr;
        case XKB_KEY_ISO_Level5_Shift: return Key_ISO_Level5_Shift;
        case XKB_KEY_Mode_switch: return Key_Mode_switch;
        case XKB_KEY_Multi_key: return Key_Multi_key;
        // Media keys (XF86 keysyms) — kitty protocol codes 57428–57440.
        case XKB_KEY_XF86AudioPlay: return Key_MediaPlay;
        case XKB_KEY_XF86AudioPause: return Key_MediaPause;
        case XKB_KEY_XF86AudioStop: return Key_MediaStop;
        case XKB_KEY_XF86AudioPrev: return Key_MediaPrevious;
        case XKB_KEY_XF86AudioNext: return Key_MediaNext;
        case XKB_KEY_XF86AudioRecord: return Key_MediaRecord;
        case XKB_KEY_XF86AudioForward: return Key_MediaFastForward;
        case XKB_KEY_XF86AudioRewind: return Key_MediaRewind;
        case XKB_KEY_XF86AudioLowerVolume: return Key_VolumeDown;
        case XKB_KEY_XF86AudioRaiseVolume: return Key_VolumeUp;
        case XKB_KEY_XF86AudioMute: return Key_VolumeMute;
        default: return Key_unknown;
    }
}

uint32_t baseKeysymForKeycode(xkb_state *state, xkb_keymap *keymap, uint32_t keycode)
{
    if (!state || !keymap) {
        return XKB_KEY_NoSymbol;
    }
    xkb_layout_index_t group = xkb_state_key_get_layout(state, keycode);
    const xkb_keysym_t *syms = nullptr;
    int nsyms                = xkb_keymap_key_get_syms_by_level(keymap, keycode, group, 0, &syms);
    if (nsyms < 1 || !syms) {
        return XKB_KEY_NoSymbol;
    }
    return syms[0];
}

uint32_t stateToModifiers(xkb_state *state)
{
    uint32_t m = 0;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m |= ShiftModifier;
    }
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m |= CtrlModifier;
    }
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) {
        // No OS-level Unicode composition layer here, so Alt is always Alt
        // — pair AltModifier with OptionAsAltModifier so InputController's
        // ESC-prefix gate behaves identically to pre-split.
        m |= AltModifier | OptionAsAltModifier;
    }
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m |= MetaModifier;
    }
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m |= CapsLockModifier;
    }
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_EFFECTIVE) > 0) {
        m |= NumLockModifier;
    }
    return m;
}

std::string keyName(xkb_state *state, int keycode)
{
    if (!state) {
        return { };
    }
    uint32_t cp = xkb_state_key_get_utf32(state, static_cast<xkb_keycode_t>(keycode));
    if (cp < 0x20 || cp == 0x7f) {
        return { };
    }
    std::string result;
    utf8::append(result, cp);
    return result;
}

uint32_t shiftedKeyCodepoint(xkb_state *state, xkb_keymap *keymap, int keycode)
{
    if (!keymap || !state) {
        return 0;
    }
    // Level 1 is the shifted variant in the current layout group
    xkb_layout_index_t group = xkb_state_key_get_layout(state, static_cast<xkb_keycode_t>(keycode));
    const xkb_keysym_t *syms = nullptr;
    int nsyms                = xkb_keymap_key_get_syms_by_level(keymap, static_cast<xkb_keycode_t>(keycode), group, 1, &syms);
    if (nsyms < 1 || !syms) {
        return 0;
    }
    uint32_t cp = xkb_keysym_to_utf32(syms[0]);
    if (cp < 0x20 || cp == 0x7f) {
        return 0;
    }
    return cp;
}

uint32_t baseLayoutKeyCodepoint(xkb_keymap *defaultKeymap, int keycode)
{
    if (!defaultKeymap) {
        return 0;
    }
    // Layout 0, level 0 of the default-rules keymap → the unshifted
    // codepoint in the system base layout (kitty's "alternate_key").
    const xkb_keysym_t *syms = nullptr;
    int nsyms                = xkb_keymap_key_get_syms_by_level(defaultKeymap, static_cast<xkb_keycode_t>(keycode), 0, 0, &syms);
    if (nsyms < 1 || !syms) {
        return 0;
    }
    uint32_t cp = xkb_keysym_to_utf32(syms[0]);
    if (cp < 0x20 || cp == 0x7f) {
        return 0;
    }
    return cp;
}

} // namespace mb::xkb
