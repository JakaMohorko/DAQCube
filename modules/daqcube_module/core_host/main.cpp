/*
 * Loads a libretro core dll, runs it self-paced at the core's reported frame
 * rate, reads per-player RetroPad input state from the shared memory block
 * created by the module and publishes RGB24 framebuffers into its frame ring.
 * With --window it also blits every frame into an SDL window on the host PC
 * (the non-headless mode); closing that window stops the session.
 *
 * The process is bundled into the DAQCube module dll as a resource and
 * extracted at runtime - it is not shipped on its own.
 *
 * Usage: core_host --shm <mapping name> --core <core dll> [--content <rom>] [--window] [--scale <n>] [--verbose]
 */

#include <game_engine_shared/core_ipc.h>
#include <retro_host/retro_host.h>

#include <libretro.h>

#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace
{
    namespace ipc = game_engine::ipc;

    struct Options
    {
        std::string shmName;
        std::string corePath;
        std::string contentPath;  // ROM file for content cores (empty = contentless)
        bool window = false;
        int scale = 3;
        bool verbose = false;
    };

    bool parseArgs(int argc, char** argv, Options& opts)
    {
        for (int i = 1; i < argc; i++)
        {
            const std::string arg = argv[i];
            if (arg == "--shm" && i + 1 < argc)
                opts.shmName = argv[++i];
            else if (arg == "--core" && i + 1 < argc)
                opts.corePath = argv[++i];
            else if (arg == "--content" && i + 1 < argc)
                opts.contentPath = argv[++i];
            else if (arg == "--window")
                opts.window = true;
            else if (arg == "--scale" && i + 1 < argc)
                opts.scale = std::atoi(argv[++i]);
            else if (arg == "--verbose")
                opts.verbose = true;
            else
            {
                std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
                return false;
            }
        }
        if (opts.shmName.empty() || opts.corePath.empty())
        {
            std::fprintf(stderr,
                         "usage: core_host --shm <name> --core <dll> [--content <rom>] [--window] [--scale <n>] [--verbose]\n");
            return false;
        }
        opts.scale = std::max(1, std::min(opts.scale, 8));
        return true;
    }

    void reportError(ipc::ControlBlock* block, const std::string& message)
    {
        std::fprintf(stderr, "core_host: %s\n", message.c_str());
        if (!block)
            return;
        std::snprintf(block->error, sizeof(block->error), "%s", message.c_str());
        block->status.store(static_cast<uint32_t>(ipc::HostStatus::Error), std::memory_order_release);
    }

    struct Window
    {
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* texture = nullptr;
        unsigned textureWidth = 0;
        unsigned textureHeight = 0;

        bool open(const std::string& title, unsigned width, unsigned height, int scale)
        {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
                return false;
            window = SDL_CreateWindow(title.c_str(),
                                      SDL_WINDOWPOS_CENTERED,
                                      SDL_WINDOWPOS_CENTERED,
                                      static_cast<int>(width) * scale,
                                      static_cast<int>(height) * scale,
                                      SDL_WINDOW_RESIZABLE);
            if (!window)
                return false;
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
            if (!renderer)
                renderer = SDL_CreateRenderer(window, -1, 0);
            if (!renderer)
                return false;
            SDL_RenderSetLogicalSize(renderer, static_cast<int>(width), static_cast<int>(height));
            return true;
        }

        void present(const retro_host::FrameRGB24& frame)
        {
            if (!renderer || frame.width == 0)
                return;
            if (!texture || textureWidth != frame.width || textureHeight != frame.height)
            {
                if (texture)
                    SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_RGB24,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            static_cast<int>(frame.width),
                                            static_cast<int>(frame.height));
                textureWidth = frame.width;
                textureHeight = frame.height;
                SDL_RenderSetLogicalSize(renderer, static_cast<int>(frame.width), static_cast<int>(frame.height));
            }
            if (!texture)
                return;
            SDL_UpdateTexture(texture, nullptr, frame.pixels.data(), static_cast<int>(frame.width) * 3);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        void close()
        {
            if (texture)
                SDL_DestroyTexture(texture);
            if (renderer)
                SDL_DestroyRenderer(renderer);
            if (window)
                SDL_DestroyWindow(window);
            texture = nullptr;
            renderer = nullptr;
            window = nullptr;
            if (SDL_WasInit(SDL_INIT_VIDEO))
                SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        }
    };
}

int main(int argc, char** argv)
{
    SDL_SetMainReady();

    Options opts;
    if (!parseArgs(argc, argv, opts))
        return 2;

    ipc::SharedMemory shm;
    std::string error;
    if (!shm.open(opts.shmName, error))
    {
        std::fprintf(stderr, "core_host: %s\n", error.c_str());
        return 1;
    }
    ipc::ControlBlock* block = shm.block();

    retro_host::Core core;
    core.setVerbose(opts.verbose);
    if (!core.load(opts.corePath, error) || !core.start(opts.contentPath, error))
    {
        reportError(block, error);
        return 1;
    }

    core.setInputState(
        [block](unsigned port, unsigned device, unsigned /*index*/, unsigned id) -> int16_t
        {
            if (device != RETRO_DEVICE_JOYPAD || port >= ipc::MaxPlayers)
                return 0;
            const uint32_t mask = block->inputs[port].load(std::memory_order_relaxed);
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
                return static_cast<int16_t>(mask & 0xFFFF);
            return (id < 16 && (mask >> id) & 1) ? 1 : 0;
        });
    core.setAudioSink(
        [&shm](const int16_t* frames, size_t frameCount)
        { shm.writeAudio(reinterpret_cast<const uint8_t*>(frames), frameCount * ipc::AudioFrameBytes); });

    const auto& av = core.avInfo();
    block->coreFps = av.fps > 0.0 ? av.fps : 60.0;
    block->audioSampleRate = av.sampleRate;
    block->baseWidth = av.baseWidth;
    block->baseHeight = av.baseHeight;

    Window window;
    if (opts.window && !window.open(core.coreName(), std::max(av.baseWidth, 1u), std::max(av.baseHeight, 1u), opts.scale))
        std::fprintf(stderr, "core_host: no window (%s) - continuing headless\n", SDL_GetError());

    block->status.store(static_cast<uint32_t>(ipc::HostStatus::Running), std::memory_order_release);

    using clock = std::chrono::steady_clock;
    const auto frameDuration = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / block->coreFps));
    auto nextFrame = clock::now();
    bool stop = false;

    while (!stop)
    {
        if (block->command.load(std::memory_order_acquire) == static_cast<uint32_t>(ipc::Command::Stop))
            break;

        SDL_Event event;
        while (window.window && SDL_PollEvent(&event))
            if (event.type == SDL_QUIT)
                stop = true;
        if (stop)
            break;

        const uint64_t framesBefore = core.framesRun();
        core.runFrame();

        const auto& frame = core.lastFrame();
        if (frame.width > 0 && frame.index > framesBefore)
        {
            shm.publishFrame(frame.pixels.data(), frame.width, frame.height);
            window.present(frame);
        }
        block->heartbeat.fetch_add(1, std::memory_order_relaxed);

        nextFrame += frameDuration;
        const auto now = clock::now();
        if (nextFrame > now)
            std::this_thread::sleep_until(nextFrame);
        else if (now - nextFrame > std::chrono::seconds(1))
            nextFrame = now;  // fell far behind (debugger, suspend) - don't sprint to catch up
    }

    core.stop();
    core.unload();
    window.close();
    block->status.store(static_cast<uint32_t>(ipc::HostStatus::Stopped), std::memory_order_release);
    return 0;
}
