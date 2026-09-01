/*
 * Dependency gate: every external dependency is included and its
 * init path is exercised. If this suite passes, the toolchain and all
 * third-party libraries are proven usable from this repository.
 */

#include <gtest/gtest.h>

#include <SDL.h>
#include <miniaudio.h>
#include <libretro.h>
#include <retro_host/retro_host.h>
#include <game_engine_shared/video_metadata.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cstdlib>
#include <filesystem>
#include <string>

TEST(Dependencies, SdlInit)
{
    ASSERT_EQ(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), 0) << SDL_GetError();
    SDL_Quit();
}

TEST(Dependencies, FfmpegMjpegEncoderInit)
{
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    ASSERT_NE(encoder, nullptr);

    AVCodecContext* codecCtx = avcodec_alloc_context3(encoder);
    ASSERT_NE(codecCtx, nullptr);

    codecCtx->width = 640;
    codecCtx->height = 400;
    codecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    codecCtx->time_base = {1, 60};

    ASSERT_EQ(avcodec_open2(codecCtx, encoder, nullptr), 0);
    avcodec_free_context(&codecCtx);
}

TEST(Dependencies, FfmpegVp8EncoderInit)
{
    // libvpx comes from the vcpkg "vpx" feature; the VP8 decoder is built in
    const AVCodec* encoder = avcodec_find_encoder_by_name("libvpx");
    ASSERT_NE(encoder, nullptr);

    AVCodecContext* codecCtx = avcodec_alloc_context3(encoder);
    ASSERT_NE(codecCtx, nullptr);

    codecCtx->width = 640;
    codecCtx->height = 400;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx->time_base = {1, 60};
    codecCtx->bit_rate = 4'000'000;

    ASSERT_EQ(avcodec_open2(codecCtx, encoder, nullptr), 0);
    avcodec_free_context(&codecCtx);

    ASSERT_NE(avcodec_find_decoder(AV_CODEC_ID_VP8), nullptr);
}

TEST(Dependencies, FfmpegMjpegDecoderInit)
{
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    ASSERT_NE(decoder, nullptr);

    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
    ASSERT_NE(codecCtx, nullptr);
    ASSERT_EQ(avcodec_open2(codecCtx, decoder, nullptr), 0);
    avcodec_free_context(&codecCtx);
}

TEST(Dependencies, FfmpegSwscaleInit)
{
    SwsContext* swsCtx = sws_getContext(640, 400, AV_PIX_FMT_RGB24,
                                        320, 200, AV_PIX_FMT_YUVJ420P,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    ASSERT_NE(swsCtx, nullptr);
    sws_freeContext(swsCtx);
}

TEST(Dependencies, MiniaudioContextInit)
{
    // The null backend needs no audio hardware, so this also passes on CI
    ma_backend backends[] = {ma_backend_null};
    ma_context context;
    ASSERT_EQ(ma_context_init(backends, 1, nullptr, &context), MA_SUCCESS);
    ma_context_uninit(&context);
}

static std::string findMrboomCore()
{
    if (const char* env = std::getenv("MRBOOM_CORE"); env && *env)
        return env;

    const std::filesystem::path candidates[] = {
        std::filesystem::path(GAME_CORES_DIR) / "mrboom_libretro.dll",
        std::filesystem::path("mrboom_libretro.dll"),
    };
    for (const auto& candidate : candidates)
        if (std::filesystem::exists(candidate))
            return candidate.string();
    return {};
}

TEST(Dependencies, LibretroCoreInit)
{
    const std::string corePath = findMrboomCore();
    if (corePath.empty())
        GTEST_SKIP() << "mrboom core not found - run tools/fetch_cores.ps1 or set MRBOOM_CORE";

    retro_host::Core core;
    std::string error;
    ASSERT_TRUE(core.load(corePath, error)) << error;
    ASSERT_EQ(core.apiVersion(), RETRO_API_VERSION);
    ASSERT_TRUE(core.start(error)) << error;

    // one video frame through the whole callback chain
    core.runFrame();
    EXPECT_GT(core.avInfo().fps, 0.0);
    EXPECT_GT(core.lastFrame().width, 0u);
    EXPECT_GT(core.lastFrame().height, 0u);

    core.stop();
    core.unload();
}

TEST(Dependencies, VideoMetadataConvention)
{
    // The descriptor metadata fields mirror FFmpeg AVCodecParameters names
    EXPECT_STREQ(game_engine::video_meta::CodecId, "codec_id");
    EXPECT_STREQ(game_engine::video_meta::PixFmt, "pix_fmt");
    EXPECT_STREQ(avcodec_get_name(AV_CODEC_ID_MJPEG), "mjpeg");
}
