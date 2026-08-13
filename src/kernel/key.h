// Keyboard input. A normalised KeyboardEvent becomes {code, mods} and lands in
// a Channel<Key> (Concept.md §3.5). There are no control characters: ^C is 'c'
// with MOD_CTRL set, and the reader decides what that means (§2.3).
#pragma once

#include "channel.h"
#include "types.h"

enum : u32 {
    MOD_SHIFT = 1,
    MOD_CTRL = 2,
    MOD_ALT = 4,
    MOD_META = 8,
};

// Printable keys carry their Unicode codepoint. Named keys sit above the
// Unicode range, so the two can never collide.
enum : u32 {
    KEY_NAMED = 0x110000,
    KEY_ENTER = KEY_NAMED,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ESCAPE,
    KEY_DELETE,
    KEY_INSERT,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
};

struct Key {
    u32 code = 0;
    u32 mods = 0;

    // A codepoint that draws, with no modifier that would make it a command.
    bool printable() const {
        return code >= ' ' && code != 0x7f && code < KEY_NAMED &&
               !(mods & (MOD_CTRL | MOD_ALT | MOD_META));
    }
};

// The one keyboard queue. key() fills it; a reader task drains it.
Channel<Key> &keys();
