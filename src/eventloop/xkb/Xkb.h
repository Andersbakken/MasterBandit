#pragma once

#include <InputTypes.h>

#include <cstdint>
#include <string>

struct xkb_keymap;
struct xkb_state;

namespace mb::xkb {

// xkb keysym → MasterBandit Key enum. Returns Key_unknown for keysyms outside
// the ASCII/Latin-1 printable range that don't have a named mapping.
Key keysymToKey(uint32_t sym);

// Level-0 (unmodified base) keysym for the active layout group of `state`.
// Used so binding identity is "physical key", not "keysym under current mods" —
// avoids breaking ctrl+alt+letter chords on AltGr / Mode_switch layouts.
// Returns XKB_KEY_NoSymbol (0) when state/keymap is missing or the lookup
// has no symbol.
uint32_t baseKeysymForKeycode(xkb_state *state, xkb_keymap *keymap, uint32_t keycode);

// Active-modifier mask from an xkb_state, in MasterBandit Modifier bits.
// AltModifier is always paired with OptionAsAltModifier on Linux (no OS-level
// Unicode composition layer to gate against, unlike macOS).
uint32_t stateToModifiers(xkb_state *state);

// UTF-8 representation of the codepoint this keycode produces under the
// current xkb state. Empty for control / non-printable codepoints.
std::string keyName(xkb_state *state, int keycode);

// Level-1 (shifted) codepoint for `keycode` in the current layout group of
// `state`. 0 if unavailable.
uint32_t shiftedKeyCodepoint(xkb_state *state, xkb_keymap *keymap, int keycode);

// Layout-0 level-0 codepoint from a default-rules ("us") keymap — the kitty
// `base_layout_key` / `alternate_key` value. 0 if unavailable.
uint32_t baseLayoutKeyCodepoint(xkb_keymap *defaultKeymap, int keycode);

} // namespace mb::xkb
