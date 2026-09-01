/*
 * Keyboard -> RetroPad key bindings
 *
 * Every RetroPad button is a string property on the player channel holding a
 * comma-separated list of key names (as published by the keyboard State
 * signal descriptor's "Bit.<Name>" metadata). Several channels can share one
 * keyboard signal with disjoint bindings - that is single-PC multiplayer.
 *
 * The defaults bind WASD and the arrow keys to the D-pad, Space to B (drops
 * bombs in Mr.Boom), Enter to START and Q/E to the shoulder buttons, on
 * every player.
 */

#pragma once

#include <libretro.h>

#include <string>
#include <vector>

namespace daq::modules::daqcube_module
{
    struct KeyBinding
    {
        const char* propertyName;  // Player FB property holding the key list
        int button;                // RETRO_DEVICE_ID_JOYPAD_*
        const char* defaultKeys;   // comma-separated key names
    };

    inline constexpr KeyBinding KeyBindings[] = {
        {"KeyUp", RETRO_DEVICE_ID_JOYPAD_UP, "W,Up"},
        {"KeyDown", RETRO_DEVICE_ID_JOYPAD_DOWN, "S,Down"},
        {"KeyLeft", RETRO_DEVICE_ID_JOYPAD_LEFT, "A,Left"},
        {"KeyRight", RETRO_DEVICE_ID_JOYPAD_RIGHT, "D,Right"},
        {"KeyB", RETRO_DEVICE_ID_JOYPAD_B, "Space"},
        {"KeyA", RETRO_DEVICE_ID_JOYPAD_A, "X"},
        {"KeyY", RETRO_DEVICE_ID_JOYPAD_Y, "Z"},
        {"KeyX", RETRO_DEVICE_ID_JOYPAD_X, "C"},
        {"KeyStart", RETRO_DEVICE_ID_JOYPAD_START, "Return"},
        {"KeySelect", RETRO_DEVICE_ID_JOYPAD_SELECT, "RightShift"},
        {"KeyL", RETRO_DEVICE_ID_JOYPAD_L, "Q"},
        {"KeyR", RETRO_DEVICE_ID_JOYPAD_R, "E"},
    };

    // "W, Up" -> {"W", "Up"} (whitespace trimmed, empty entries dropped)
    inline std::vector<std::string> parseKeyList(const std::string& keys)
    {
        std::vector<std::string> names;
        size_t start = 0;
        while (start <= keys.size())
        {
            size_t end = keys.find(',', start);
            if (end == std::string::npos)
                end = keys.size();
            size_t first = start;
            size_t last = end;
            while (first < last && (keys[first] == ' ' || keys[first] == '\t'))
                first++;
            while (last > first && (keys[last - 1] == ' ' || keys[last - 1] == '\t'))
                last--;
            if (last > first)
                names.emplace_back(keys.substr(first, last - first));
            start = end + 1;
        }
        return names;
    }
}
