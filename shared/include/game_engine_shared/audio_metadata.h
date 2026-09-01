/*
 * Audio signal metadata convention, mirroring the video one.
 *
 * The game audio signal is a plain binary stream of interleaved PCM frames.
 * The signal's data descriptor metadata carries the format, so any consumer
 * (the AudioOutput device) can configure playback straight from the
 * descriptor, without out-of-band information.
 */

#pragma once

namespace game_engine::audio_meta
{
    // FFmpeg-style sample format name; only "s16" (interleaved signed 16-bit
    // little-endian PCM) is produced so far - libretro cores emit exactly that
    inline constexpr const char* SampleFormat = "sample_fmt";

    // Frames per second as a decimal integer string, e.g. "44100"
    inline constexpr const char* SampleRate = "sample_rate";

    // Interleaved channel count as a decimal integer string ("2" = stereo)
    inline constexpr const char* Channels = "channels";
}
