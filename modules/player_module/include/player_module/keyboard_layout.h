#pragma once

#include <SDL_scancode.h>

#include <array>
#include <cstdint>

// Standard ANSI US (104-key) layout. The bit index of a key in the keyboard
// state bitmap is its position in this list; the same mapping is published in
// the State signal's descriptor metadata as "Bit.<Name>" entries.
#define KEYBOARD_CAPTURE_KEY_LIST(KEY) \
    KEY(Escape, SDL_SCANCODE_ESCAPE) \
    KEY(F1, SDL_SCANCODE_F1) \
    KEY(F2, SDL_SCANCODE_F2) \
    KEY(F3, SDL_SCANCODE_F3) \
    KEY(F4, SDL_SCANCODE_F4) \
    KEY(F5, SDL_SCANCODE_F5) \
    KEY(F6, SDL_SCANCODE_F6) \
    KEY(F7, SDL_SCANCODE_F7) \
    KEY(F8, SDL_SCANCODE_F8) \
    KEY(F9, SDL_SCANCODE_F9) \
    KEY(F10, SDL_SCANCODE_F10) \
    KEY(F11, SDL_SCANCODE_F11) \
    KEY(F12, SDL_SCANCODE_F12) \
    KEY(PrintScreen, SDL_SCANCODE_PRINTSCREEN) \
    KEY(ScrollLock, SDL_SCANCODE_SCROLLLOCK) \
    KEY(Pause, SDL_SCANCODE_PAUSE) \
    KEY(Grave, SDL_SCANCODE_GRAVE) \
    KEY(D1, SDL_SCANCODE_1) \
    KEY(D2, SDL_SCANCODE_2) \
    KEY(D3, SDL_SCANCODE_3) \
    KEY(D4, SDL_SCANCODE_4) \
    KEY(D5, SDL_SCANCODE_5) \
    KEY(D6, SDL_SCANCODE_6) \
    KEY(D7, SDL_SCANCODE_7) \
    KEY(D8, SDL_SCANCODE_8) \
    KEY(D9, SDL_SCANCODE_9) \
    KEY(D0, SDL_SCANCODE_0) \
    KEY(Minus, SDL_SCANCODE_MINUS) \
    KEY(Equals, SDL_SCANCODE_EQUALS) \
    KEY(Backspace, SDL_SCANCODE_BACKSPACE) \
    KEY(Tab, SDL_SCANCODE_TAB) \
    KEY(Q, SDL_SCANCODE_Q) \
    KEY(W, SDL_SCANCODE_W) \
    KEY(E, SDL_SCANCODE_E) \
    KEY(R, SDL_SCANCODE_R) \
    KEY(T, SDL_SCANCODE_T) \
    KEY(Y, SDL_SCANCODE_Y) \
    KEY(U, SDL_SCANCODE_U) \
    KEY(I, SDL_SCANCODE_I) \
    KEY(O, SDL_SCANCODE_O) \
    KEY(P, SDL_SCANCODE_P) \
    KEY(LeftBracket, SDL_SCANCODE_LEFTBRACKET) \
    KEY(RightBracket, SDL_SCANCODE_RIGHTBRACKET) \
    KEY(Backslash, SDL_SCANCODE_BACKSLASH) \
    KEY(CapsLock, SDL_SCANCODE_CAPSLOCK) \
    KEY(A, SDL_SCANCODE_A) \
    KEY(S, SDL_SCANCODE_S) \
    KEY(D, SDL_SCANCODE_D) \
    KEY(F, SDL_SCANCODE_F) \
    KEY(G, SDL_SCANCODE_G) \
    KEY(H, SDL_SCANCODE_H) \
    KEY(J, SDL_SCANCODE_J) \
    KEY(K, SDL_SCANCODE_K) \
    KEY(L, SDL_SCANCODE_L) \
    KEY(Semicolon, SDL_SCANCODE_SEMICOLON) \
    KEY(Apostrophe, SDL_SCANCODE_APOSTROPHE) \
    KEY(Return, SDL_SCANCODE_RETURN) \
    KEY(LeftShift, SDL_SCANCODE_LSHIFT) \
    KEY(Z, SDL_SCANCODE_Z) \
    KEY(X, SDL_SCANCODE_X) \
    KEY(C, SDL_SCANCODE_C) \
    KEY(V, SDL_SCANCODE_V) \
    KEY(B, SDL_SCANCODE_B) \
    KEY(N, SDL_SCANCODE_N) \
    KEY(M, SDL_SCANCODE_M) \
    KEY(Comma, SDL_SCANCODE_COMMA) \
    KEY(Period, SDL_SCANCODE_PERIOD) \
    KEY(Slash, SDL_SCANCODE_SLASH) \
    KEY(RightShift, SDL_SCANCODE_RSHIFT) \
    KEY(LeftCtrl, SDL_SCANCODE_LCTRL) \
    KEY(LeftGui, SDL_SCANCODE_LGUI) \
    KEY(LeftAlt, SDL_SCANCODE_LALT) \
    KEY(Space, SDL_SCANCODE_SPACE) \
    KEY(RightAlt, SDL_SCANCODE_RALT) \
    KEY(RightGui, SDL_SCANCODE_RGUI) \
    KEY(Menu, SDL_SCANCODE_APPLICATION) \
    KEY(RightCtrl, SDL_SCANCODE_RCTRL) \
    KEY(Insert, SDL_SCANCODE_INSERT) \
    KEY(Home, SDL_SCANCODE_HOME) \
    KEY(PageUp, SDL_SCANCODE_PAGEUP) \
    KEY(Delete, SDL_SCANCODE_DELETE) \
    KEY(End, SDL_SCANCODE_END) \
    KEY(PageDown, SDL_SCANCODE_PAGEDOWN) \
    KEY(Up, SDL_SCANCODE_UP) \
    KEY(Down, SDL_SCANCODE_DOWN) \
    KEY(Left, SDL_SCANCODE_LEFT) \
    KEY(Right, SDL_SCANCODE_RIGHT) \
    KEY(NumLock, SDL_SCANCODE_NUMLOCKCLEAR) \
    KEY(KpDivide, SDL_SCANCODE_KP_DIVIDE) \
    KEY(KpMultiply, SDL_SCANCODE_KP_MULTIPLY) \
    KEY(KpMinus, SDL_SCANCODE_KP_MINUS) \
    KEY(KpPlus, SDL_SCANCODE_KP_PLUS) \
    KEY(KpEnter, SDL_SCANCODE_KP_ENTER) \
    KEY(Kp1, SDL_SCANCODE_KP_1) \
    KEY(Kp2, SDL_SCANCODE_KP_2) \
    KEY(Kp3, SDL_SCANCODE_KP_3) \
    KEY(Kp4, SDL_SCANCODE_KP_4) \
    KEY(Kp5, SDL_SCANCODE_KP_5) \
    KEY(Kp6, SDL_SCANCODE_KP_6) \
    KEY(Kp7, SDL_SCANCODE_KP_7) \
    KEY(Kp8, SDL_SCANCODE_KP_8) \
    KEY(Kp9, SDL_SCANCODE_KP_9) \
    KEY(Kp0, SDL_SCANCODE_KP_0) \
    KEY(KpPeriod, SDL_SCANCODE_KP_PERIOD)

namespace daq::modules::player_module
{
    struct KeyInfo
    {
        const char* name;
        SDL_Scancode scancode;
    };

    inline constexpr KeyInfo AnsiKeys[] = {
#define KEYBOARD_CAPTURE_KEY_ENTRY(NAME, SCANCODE) {#NAME, SCANCODE},
        KEYBOARD_CAPTURE_KEY_LIST(KEYBOARD_CAPTURE_KEY_ENTRY)
#undef KEYBOARD_CAPTURE_KEY_ENTRY
    };

    inline constexpr unsigned AnsiKeyCount = sizeof(AnsiKeys) / sizeof(AnsiKeys[0]);
    static_assert(AnsiKeyCount == 104, "ANSI US layout must have 104 keys");

    // scancode -> bit index (-1 for keys outside the ANSI layout)
    inline int scancodeToBit(SDL_Scancode scancode)
    {
        static const auto lookup = []
        {
            std::array<int16_t, SDL_NUM_SCANCODES> table{};
            table.fill(-1);
            for (unsigned bit = 0; bit < AnsiKeyCount; bit++)
                table[AnsiKeys[bit].scancode] = static_cast<int16_t>(bit);
            return table;
        }();
        if (scancode < 0 || scancode >= SDL_NUM_SCANCODES)
            return -1;
        return lookup[scancode];
    }
}

