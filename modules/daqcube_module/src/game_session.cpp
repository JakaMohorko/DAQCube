#include <daqcube_module/game_session.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace daq::modules::daqcube_module
{
    namespace ipc = game_engine::ipc;
    using namespace std::chrono;

    namespace
    {
        // Video encoder with a resize/pixel-format stage in front (libswscale).
        // Two codecs: VP8 (libvpx, realtime mode, keyframe every second) for
        // quality per bit, and MJPEG (intra-only, every packet a full JPEG) as
        // the zero-state fallback.
        class VideoEncoder
        {
        public:
            ~VideoEncoder() { close(); }

            bool open(const GameSession::Config& config, unsigned width, unsigned height, double fps, std::string& errorOut)
            {
                const bool mjpeg = config.codec == "mjpeg";
                const AVCodec* codec = mjpeg ? avcodec_find_encoder(AV_CODEC_ID_MJPEG)
                                             : avcodec_find_encoder_by_name("libvpx");
                if (!codec)
                {
                    errorOut = "encoder for '" + config.codec + "' not available";
                    return false;
                }
                fullRange = mjpeg;

                ctx = avcodec_alloc_context3(codec);
                ctx->width = static_cast<int>(width);
                ctx->height = static_cast<int>(height);
                ctx->pix_fmt = AV_PIX_FMT_YUV420P;
                ctx->time_base = av_d2q(1.0 / (fps > 0.0 ? fps : 60.0), 1'000'000);
                if (mjpeg)
                {
                    // yuv420p + full range instead of the deprecated yuvj420p alias
                    // (which makes swscale warn on every frame); decoders still
                    // report the stream as yuvj420p, matching the descriptor
                    ctx->color_range = AVCOL_RANGE_JPEG;
                    ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
                    // quality 1..100 -> MJPEG qscale 31..2 (lower is better)
                    const int qscale = 2 + (100 - std::clamp(config.jpegQuality, 1, 100)) * 29 / 99;
                    ctx->flags |= AV_CODEC_FLAG_QSCALE;
                    ctx->global_quality = FF_QP2LAMBDA * qscale;
                }
                else
                {
                    ctx->bit_rate = static_cast<int64_t>(std::max(config.bitrateKbps, 250)) * 1000;
                    // a keyframe every second bounds both a late joiner's wait and
                    // how far the replay queue must decode before it may skip ahead
                    ctx->gop_size = static_cast<int>(fps > 0.0 ? fps : 60.0);
                    ctx->thread_count = 0;  // auto
                    // realtime: no frame lookahead, speed over compression density
                    av_opt_set(ctx->priv_data, "deadline", "realtime", 0);
                    av_opt_set_int(ctx->priv_data, "cpu-used", 8, 0);
                    av_opt_set_int(ctx->priv_data, "lag-in-frames", 0, 0);
                }

                if (const int err = avcodec_open2(ctx, codec, nullptr); err < 0)
                {
                    errorOut = "avcodec_open2 failed: " + std::to_string(err);
                    close();
                    return false;
                }

                yuvFrame = av_frame_alloc();
                yuvFrame->format = ctx->pix_fmt;
                yuvFrame->width = ctx->width;
                yuvFrame->height = ctx->height;
                yuvFrame->color_range = ctx->color_range;
                yuvFrame->quality = ctx->global_quality;
                if (av_frame_get_buffer(yuvFrame, 0) < 0)
                {
                    errorOut = "av_frame_get_buffer failed";
                    close();
                    return false;
                }
                packet = av_packet_alloc();
                return true;
            }

            // encodes one RGB24 frame; sink is called with the complete JPEG
            bool encode(const uint8_t* rgb24, unsigned srcWidth, unsigned srcHeight,
                        const std::function<void(const uint8_t*, size_t)>& sink)
            {
                const bool newContext = !sws || srcWidth != lastSrcWidth || srcHeight != lastSrcHeight;
                sws = sws_getCachedContext(sws,
                                           static_cast<int>(srcWidth),
                                           static_cast<int>(srcHeight),
                                           AV_PIX_FMT_RGB24,
                                           ctx->width,
                                           ctx->height,
                                           AV_PIX_FMT_YUV420P,
                                           SWS_BILINEAR,
                                           nullptr,
                                           nullptr,
                                           nullptr);
                if (!sws || av_frame_make_writable(yuvFrame) < 0)
                    return false;
                if (newContext)
                {
                    // full-range YUV for MJPEG (JPEG color range), limited for VP8
                    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
                    sws_setColorspaceDetails(sws, coefficients, 1, coefficients, fullRange ? 1 : 0, 0, 1 << 16, 1 << 16);
                    lastSrcWidth = srcWidth;
                    lastSrcHeight = srcHeight;
                }

                const uint8_t* srcData[1] = {rgb24};
                const int srcStride[1] = {static_cast<int>(srcWidth) * 3};
                sws_scale(sws, srcData, srcStride, 0, static_cast<int>(srcHeight), yuvFrame->data, yuvFrame->linesize);
                yuvFrame->pts = nextPts++;

                if (avcodec_send_frame(ctx, yuvFrame) < 0)
                    return false;
                bool produced = false;
                while (avcodec_receive_packet(ctx, packet) == 0)
                {
                    sink(packet->data, static_cast<size_t>(packet->size));
                    av_packet_unref(packet);
                    produced = true;
                }
                return produced;
            }

            void close()
            {
                if (packet)
                    av_packet_free(&packet);
                if (yuvFrame)
                    av_frame_free(&yuvFrame);
                if (ctx)
                    avcodec_free_context(&ctx);
                if (sws)
                {
                    sws_freeContext(sws);
                    sws = nullptr;
                }
            }

        private:
            AVCodecContext* ctx = nullptr;
            SwsContext* sws = nullptr;
            AVFrame* yuvFrame = nullptr;
            AVPacket* packet = nullptr;
            int64_t nextPts = 0;
            unsigned lastSrcWidth = 0;
            unsigned lastSrcHeight = 0;
            bool fullRange = false;
        };

        std::string uniqueShmName()
        {
            static std::atomic<uint32_t> counter{0};
            return "Local\\DAQCube." + std::to_string(GetCurrentProcessId()) + "." +
                   std::to_string(counter.fetch_add(1));
        }
    }

    bool GameSession::start(Config config, std::string& errorOut)
    {
        if (sessionActive.load())
        {
            errorOut = "session already running";
            return false;
        }
        stop();  // reap a previously ended session
        stopRequested = false;

        const auto shmName = uniqueShmName();
        if (!shm.create(shmName, errorOut))
            return false;

        std::string commandLine = "\"" + config.hostExePath + "\" --shm " + shmName + " --core \"" + config.corePath + "\"";
        if (!config.contentPath.empty())
            commandLine += " --content \"" + config.contentPath + "\"";
        if (config.window)
            commandLine += " --window";

        STARTUPINFOA startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo{};
        // CREATE_NO_WINDOW suppresses the child's console; its SDL window (if
        // requested) is unaffected
        if (!CreateProcessA(nullptr,
                            commandLine.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            CREATE_NO_WINDOW,
                            nullptr,
                            nullptr,
                            &startupInfo,
                            &processInfo))
        {
            errorOut = "failed to launch core host (" + std::to_string(GetLastError()) + "): " + config.hostExePath;
            shm.close();
            return false;
        }
        CloseHandle(processInfo.hThread);
        process = processInfo.hProcess;

        // wait for the host to load the core and report Running
        ipc::ControlBlock* block = shm.block();
        const auto deadline = steady_clock::now() + seconds(15);
        for (;;)
        {
            const auto status = static_cast<ipc::HostStatus>(block->status.load(std::memory_order_acquire));
            if (status == ipc::HostStatus::Running)
                break;
            if (status == ipc::HostStatus::Error)
            {
                errorOut = std::string("core host failed: ") + block->error;
                stop();
                return false;
            }
            if (!hostAlive())
            {
                errorOut = "core host exited during startup";
                stop();
                return false;
            }
            if (steady_clock::now() > deadline)
            {
                errorOut = "core host startup timed out";
                stop();
                return false;
            }
            std::this_thread::sleep_for(milliseconds(10));
        }

        fps = block->coreFps;
        audioSampleRate = block->audioSampleRate;
        baseWidth = block->baseWidth;
        baseHeight = block->baseHeight;
        // 4:2:0 chroma subsampling needs even dimensions
        outWidth = (config.outputWidth > 0 ? config.outputWidth : baseWidth) & ~1u;
        outHeight = (config.outputHeight > 0 ? config.outputHeight : baseHeight) & ~1u;
        if (outWidth == 0 || outHeight == 0)
        {
            errorOut = "core reported no video geometry";
            stop();
            return false;
        }

        sessionActive = true;
        sessionThread = std::thread{&GameSession::sessionLoop, this, std::move(config)};
        return true;
    }

    void GameSession::stop()
    {
        stopRequested = true;
        if (sessionThread.joinable())
            sessionThread.join();

        if (shm.block())
            shm.block()->command.store(static_cast<uint32_t>(ipc::Command::Stop), std::memory_order_release);
        terminateHost();
        shm.close();
        sessionActive = false;
    }

    void GameSession::setPlayerInput(unsigned playerIndex, uint32_t buttons)
    {
        if (playerIndex >= ipc::MaxPlayers)
            return;
        if (auto* block = shm.block(); block && sessionActive.load(std::memory_order_acquire))
            block->inputs[playerIndex].store(buttons, std::memory_order_relaxed);
    }

    bool GameSession::hostAlive() const
    {
        return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    void GameSession::terminateHost()
    {
        if (!process)
            return;
        // give the host a moment to exit cleanly on the Stop command
        if (WaitForSingleObject(process, 3000) == WAIT_TIMEOUT)
            TerminateProcess(static_cast<HANDLE>(process), 1);
        CloseHandle(process);
        process = nullptr;
    }

    void GameSession::sessionLoop(Config config)
    {
        ipc::ControlBlock* block = shm.block();

        VideoEncoder encoder;
        std::string error;
        const unsigned divisor = std::max(config.frameDivisor, 1u);
        if (!encoder.open(config, outWidth, outHeight, fps / divisor, error))
        {
            sessionActive = false;
            if (config.onEnded)
                config.onEnded(true, "video encoder failed: " + error);
            return;
        }

        std::vector<uint8_t> rgb;
        std::vector<uint8_t> pcm;
        uint64_t lastFrameSequence = 0;
        uint64_t framesSeen = 0;
        uint64_t lastHeartbeat = 0;
        auto lastProgress = steady_clock::now();
        auto lastWatchdogCheck = steady_clock::now();

        while (!stopRequested.load(std::memory_order_acquire))
        {
            // audio drains every pass - PCM is cheap and latency matters
            if (config.onAudioSamples)
            {
                pcm.clear();
                if (shm.readAudio(pcm) > 0)
                    config.onAudioSamples(pcm.data(), pcm.size());
            }

            // the core dictates its frame rate; the divisor thins the stream to
            // trade smoothness for bandwidth - thinned frames are consumed
            // without copying their pixels out of the shared memory ring
            const bool encodeThis = framesSeen % divisor == 0;
            uint32_t width = 0;
            uint32_t height = 0;
            const bool gotFrame = encodeThis ? shm.readLatestFrame(lastFrameSequence, rgb, width, height)
                                             : shm.skipToLatestFrame(lastFrameSequence);
            if (gotFrame)
            {
                framesSeen++;
                if (encodeThis)
                {
                    encoder.encode(rgb.data(), width, height,
                                   [&](const uint8_t* data, size_t size)
                                   {
                                       if (config.onEncodedFrame)
                                           config.onEncodedFrame(data, size);
                                   });
                }
                continue;  // drain back-to-back frames without sleeping
            }

            const auto now = steady_clock::now();
            if (now - lastWatchdogCheck >= milliseconds(250))
            {
                lastWatchdogCheck = now;

                const auto status = static_cast<ipc::HostStatus>(block->status.load(std::memory_order_acquire));
                if (status == ipc::HostStatus::Stopped || status == ipc::HostStatus::Error || !hostAlive())
                {
                    sessionActive = false;
                    if (config.onEnded)
                    {
                        if (status == ipc::HostStatus::Stopped)
                            config.onEnded(false, "game stopped by the host window");
                        else if (status == ipc::HostStatus::Error)
                            config.onEnded(true, std::string("core host error: ") + block->error);
                        else
                            config.onEnded(true, "core host process died");
                    }
                    return;
                }

                const uint64_t heartbeat = block->heartbeat.load(std::memory_order_relaxed);
                if (heartbeat != lastHeartbeat)
                {
                    lastHeartbeat = heartbeat;
                    lastProgress = now;
                }
                else if (now - lastProgress > seconds(10))
                {
                    sessionActive = false;
                    if (config.onEnded)
                        config.onEnded(true, "core host stopped making progress (hung)");
                    return;
                }
            }

            std::this_thread::sleep_for(milliseconds(1));
        }
    }
}
