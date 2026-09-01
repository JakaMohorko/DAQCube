/*
 * Minimal headless libretro frontend.
 *
 * Loads a libretro core dll, drives it frame by frame and hands out
 * RGB24-converted framebuffers. The libretro callback API is global per
 * process, so only one Core may be started at a time.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace retro_host
{
    struct FrameRGB24
    {
        unsigned width = 0;
        unsigned height = 0;
        uint64_t index = 0;  // frame counter at the time of capture
        std::vector<uint8_t> pixels;  // width * height * 3, row-major RGB
    };

    struct AvInfo
    {
        double fps = 0.0;
        double sampleRate = 0.0;
        unsigned baseWidth = 0;
        unsigned baseHeight = 0;
        unsigned maxWidth = 0;
        unsigned maxHeight = 0;
    };

    // (port, device, index, id) -> button state, see RETRO_DEVICE_ID_JOYPAD_*
    using InputStateFn = std::function<int16_t(unsigned port, unsigned device, unsigned index, unsigned id)>;

    // interleaved stereo s16 PCM produced by the core during runFrame()
    using AudioSinkFn = std::function<void(const int16_t* frames, size_t frameCount)>;

    class Core
    {
    public:
        Core() = default;
        ~Core();
        Core(const Core&) = delete;
        Core& operator=(const Core&) = delete;

        // Loads the core dll and resolves the libretro entry points.
        bool load(const std::string& dllPath, std::string& error);

        // retro_api_version(); valid after load()
        unsigned apiVersion() const;

        // Core display name from retro_get_system_info(); valid after load()
        const std::string& coreName() const { return name; }

        // Registers callbacks, runs retro_init and retro_load_game (no content).
        bool start(std::string& error);

        // Same, but loads a content file (ROM). Honors the core's need_fullpath:
        // the file is either passed by path or read into memory first.
        bool start(const std::string& contentPath, std::string& error);

        void setInputState(InputStateFn fn) { inputState = std::move(fn); }

        // receives the core's audio (interleaved stereo s16 at avInfo().sampleRate)
        void setAudioSink(AudioSinkFn fn) { audioSink = std::move(fn); }

        // retro_run(); the most recent video refresh is available via lastFrame()
        void runFrame();

        const FrameRGB24& lastFrame() const { return frame; }
        uint64_t framesRun() const { return frameCounter; }
        const AvInfo& avInfo() const { return av; }

        // retro_unload_game + retro_deinit (safe to call when not started)
        void stop();

        // FreeLibrary (stop() is called first if needed)
        void unload();

        // Enables core log output to stderr
        void setVerbose(bool enabled) { verbose = enabled; }

    private:
        static Core* active;

        // libretro callbacks (static, routed to `active`)
        static bool onEnvironment(unsigned cmd, void* data);
        static void onVideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch);
        static void onAudioSample(int16_t left, int16_t right);
        static size_t onAudioSampleBatch(const int16_t* data, size_t frames);
        static void onInputPoll();
        static int16_t onInputState(unsigned port, unsigned device, unsigned index, unsigned id);

        void convertFrame(const void* data, unsigned width, unsigned height, size_t pitch);

        void* lib = nullptr;
        std::string name;
        bool isStarted = false;
        bool verbose = false;
        int pixelFormat = 0;  // enum retro_pixel_format
        std::string coreDir;  // the core dll's directory (set by load())
        std::string systemDir;
        std::string saveDir;
        InputStateFn inputState;
        AudioSinkFn audioSink;
        FrameRGB24 frame;
        uint64_t frameCounter = 0;
        AvInfo av;

        struct Api;
        Api* api = nullptr;
    };
}
