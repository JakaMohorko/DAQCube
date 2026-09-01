/*
 * Keyboard state wire contract between the Player FB (producer) and the
 * game device's player channels (consumer).
 *
 * The state travels as a 128-bit bitmap (two 64-bit words, 16 bytes per
 * sample). Bit indices are defined by the producer's key layout and are
 * published in the State signal's descriptor metadata as "Bit.<Name>"
 * entries, so any consumer can decode the bitmap from the descriptor alone.
 */

#pragma once

#include <array>
#include <bitset>
#include <cstdint>

namespace game_engine
{
    // descriptor metadata key prefix: "Bit.<KeyName>" -> bit index (decimal string)
    inline constexpr const char* KeyBitMetadataPrefix = "Bit.";

    struct KeyboardState
    {
        static constexpr unsigned BitCount = 128;
        static constexpr size_t ByteSize = 16;

        std::array<uint64_t, 2> words{};

        void set(unsigned bit, bool down)
        {
            if (bit >= BitCount)
                return;
            const uint64_t mask = uint64_t(1) << (bit % 64);
            if (down)
                words[bit / 64] |= mask;
            else
                words[bit / 64] &= ~mask;
        }

        bool test(unsigned bit) const
        {
            if (bit >= BitCount)
                return false;
            return (words[bit / 64] >> (bit % 64)) & 1;
        }

        unsigned pressedCount() const
        {
            return static_cast<unsigned>(std::bitset<64>(words[0]).count() + std::bitset<64>(words[1]).count());
        }

        const void* data() const
        {
            return words.data();
        }

        bool operator==(const KeyboardState& other) const
        {
            return words == other.words;
        }

        bool operator!=(const KeyboardState& other) const
        {
            return !(*this == other);
        }
    };
}
