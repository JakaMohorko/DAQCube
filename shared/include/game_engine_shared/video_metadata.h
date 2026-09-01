/*
 * Video signal metadata convention.
 *
 * The game video signal is a plain binary stream of encoded frames. The
 * signal's data descriptor metadata mirrors FFmpeg's AVCodecParameters
 * fields so the VideoReplay function block can configure its decoder
 * straight from the descriptor, without any out-of-band information.
 */

#pragma once

#include <cstddef>

namespace game_engine::video_meta
{
    // FFmpeg AVCodecID name as returned by avcodec_get_name(), e.g. "mjpeg"
    inline constexpr const char* CodecId = "codec_id";

    // Coded frame size in pixels (decimal integer strings)
    inline constexpr const char* Width = "width";
    inline constexpr const char* Height = "height";

    // FFmpeg AVPixelFormat name as returned by av_get_pix_fmt_name(), e.g. "yuvj420p"
    inline constexpr const char* PixFmt = "pix_fmt";

    // Target frame rate as an FFmpeg AVRational "num/den" string, e.g. "60/1"
    inline constexpr const char* Framerate = "framerate";

    // Average bitrate in bits per second; "0" when unknown/variable
    inline constexpr const char* BitRate = "bit_rate";

    // VP8 frame tag: bit 0 of the first payload byte is clear on keyframes
    inline bool isVp8Keyframe(const unsigned char* data, size_t size)
    {
        return size > 0 && (data[0] & 0x01) == 0;
    }
}
