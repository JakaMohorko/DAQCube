#pragma once

#include <game_engine_shared/core_ipc.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace daq::modules::daqcube_module
{
    // One running game: owns the core_host.exe child process, the shared
    // memory block, and the session thread that pulls framebuffers from the
    // frame ring, encodes them (VP8 or MJPEG) and hands the encoded frames to
    // the device. The same thread doubles as the watchdog: a dead or hung host
    // process ends the session and notifies the device. Pure C++/Win32 +
    // FFmpeg - no openDAQ types, so it stays unit-testable on its own.
    class GameSession
    {
    public:
        struct Config
        {
            std::string hostExePath;
            std::string corePath;
            std::string contentPath;  // ROM file for content cores (empty = contentless)
            bool window = false;  // open an SDL window on the host PC (Headless=false)
            std::string codec = "vp8";  // FFmpeg encoder: "vp8" (libvpx) or "mjpeg"
            int jpegQuality = 80;   // 1..100, MJPEG only
            int bitrateKbps = 4000; // VP8 only
            unsigned outputWidth = 0;   // 0 = core native (rounded down to even for 4:2:0)
            unsigned outputHeight = 0;
            unsigned frameDivisor = 1;  // encode every Nth core frame (bandwidth limit)

            // called from the session thread with each complete encoded frame
            std::function<void(const uint8_t* data, size_t size)> onEncodedFrame;

            // called from the session thread with raw interleaved stereo s16
            // PCM as it drains from the core (at coreAudioSampleRate())
            std::function<void(const uint8_t* pcm, size_t bytes)> onAudioSamples;

            // called once from the session thread when the session ends on its
            // own (host exited, crashed or hung); never called by stop()
            std::function<void(bool crashed, const std::string& message)> onEnded;
        };

        GameSession() = default;
        ~GameSession() { stop(); }
        GameSession(const GameSession&) = delete;
        GameSession& operator=(const GameSession&) = delete;

        // spawns the host, waits until the core reports Running (or Error/timeout)
        // and starts the session thread
        bool start(Config config, std::string& errorOut);

        // asks the host to stop, terminates it if it does not comply, joins the
        // session thread; safe to call repeatedly and after onEnded fired
        void stop();

        bool running() const { return sessionActive.load(std::memory_order_acquire); }

        // core-reported stream parameters, valid after a successful start()
        double coreFps() const { return fps; }
        double coreAudioSampleRate() const { return audioSampleRate; }  // 0 = no audio
        unsigned nativeWidth() const { return baseWidth; }
        unsigned nativeHeight() const { return baseHeight; }
        unsigned encodedWidth() const { return outWidth; }
        unsigned encodedHeight() const { return outHeight; }

        // RetroPad button bitmask, forwarded to the host's shared input block
        void setPlayerInput(unsigned playerIndex, uint32_t buttons);

    private:
        void sessionLoop(Config config);
        bool hostAlive() const;
        void terminateHost();

        game_engine::ipc::SharedMemory shm;
        void* process = nullptr;  // HANDLE
        std::thread sessionThread;
        std::atomic<bool> sessionActive{false};
        std::atomic<bool> stopRequested{false};

        double fps = 60.0;
        double audioSampleRate = 0.0;
        unsigned baseWidth = 0;
        unsigned baseHeight = 0;
        unsigned outWidth = 0;
        unsigned outHeight = 0;
    };
}
