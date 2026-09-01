#include <retro_host/retro_host.h>

#include <libretro.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace retro_host
{

namespace
{
    void* openLibrary(const std::string& path)
    {
#ifdef _WIN32
        return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
        return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    }

    void closeLibrary(void* lib)
    {
#ifdef _WIN32
        FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
        dlclose(lib);
#endif
    }

    void* getSymbol(void* lib, const char* symbol)
    {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), symbol));
#else
        return dlsym(lib, symbol);
#endif
    }

    void RETRO_CALLCONV logStderr(enum retro_log_level level, const char* fmt, ...)
    {
        static const char* levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        std::fprintf(stderr, "[core %s] ", level <= RETRO_LOG_ERROR ? levels[level] : "?");
        va_list args;
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
    }

    void RETRO_CALLCONV logNull(enum retro_log_level, const char*, ...)
    {
    }
}

struct Core::Api
{
    void (RETRO_CALLCONV *setEnvironment)(retro_environment_t) = nullptr;
    void (RETRO_CALLCONV *setVideoRefresh)(retro_video_refresh_t) = nullptr;
    void (RETRO_CALLCONV *setAudioSample)(retro_audio_sample_t) = nullptr;
    void (RETRO_CALLCONV *setAudioSampleBatch)(retro_audio_sample_batch_t) = nullptr;
    void (RETRO_CALLCONV *setInputPoll)(retro_input_poll_t) = nullptr;
    void (RETRO_CALLCONV *setInputState)(retro_input_state_t) = nullptr;
    void (RETRO_CALLCONV *init)() = nullptr;
    void (RETRO_CALLCONV *deinit)() = nullptr;
    unsigned (RETRO_CALLCONV *apiVersion)() = nullptr;
    void (RETRO_CALLCONV *getSystemInfo)(struct retro_system_info*) = nullptr;
    void (RETRO_CALLCONV *getSystemAvInfo)(struct retro_system_av_info*) = nullptr;
    void (RETRO_CALLCONV *run)() = nullptr;
    bool (RETRO_CALLCONV *loadGame)(const struct retro_game_info*) = nullptr;
    void (RETRO_CALLCONV *unloadGame)() = nullptr;
};

Core* Core::active = nullptr;

Core::~Core()
{
    unload();
    delete api;
}

bool Core::load(const std::string& dllPath, std::string& error)
{
    if (lib)
    {
        error = "a core is already loaded";
        return false;
    }
    if (!std::filesystem::exists(dllPath))
    {
        error = "core not found: " + dllPath;
        return false;
    }

    lib = openLibrary(dllPath);
    if (!lib)
    {
        error = "failed to load library: " + dllPath;
        return false;
    }

    // the core's own directory doubles as its system/save directory - support
    // files (e.g. prboom.wad) are extracted next to the core dll, and the
    // extraction dir under LOCALAPPDATA is writable for saves/configs
    std::error_code ec;
    const auto dir = std::filesystem::absolute(std::filesystem::path(dllPath), ec).parent_path();
    coreDir = ec ? std::filesystem::current_path().string() : dir.string();

    if (!api)
        api = new Api();

    struct SymbolEntry
    {
        const char* name;
        void** target;
    };
    const SymbolEntry symbols[] = {
        {"retro_set_environment", reinterpret_cast<void**>(&api->setEnvironment)},
        {"retro_set_video_refresh", reinterpret_cast<void**>(&api->setVideoRefresh)},
        {"retro_set_audio_sample", reinterpret_cast<void**>(&api->setAudioSample)},
        {"retro_set_audio_sample_batch", reinterpret_cast<void**>(&api->setAudioSampleBatch)},
        {"retro_set_input_poll", reinterpret_cast<void**>(&api->setInputPoll)},
        {"retro_set_input_state", reinterpret_cast<void**>(&api->setInputState)},
        {"retro_init", reinterpret_cast<void**>(&api->init)},
        {"retro_deinit", reinterpret_cast<void**>(&api->deinit)},
        {"retro_api_version", reinterpret_cast<void**>(&api->apiVersion)},
        {"retro_get_system_info", reinterpret_cast<void**>(&api->getSystemInfo)},
        {"retro_get_system_av_info", reinterpret_cast<void**>(&api->getSystemAvInfo)},
        {"retro_run", reinterpret_cast<void**>(&api->run)},
        {"retro_load_game", reinterpret_cast<void**>(&api->loadGame)},
        {"retro_unload_game", reinterpret_cast<void**>(&api->unloadGame)},
    };

    for (const auto& symbol : symbols)
    {
        *symbol.target = getSymbol(lib, symbol.name);
        if (!*symbol.target)
        {
            error = std::string("missing libretro symbol: ") + symbol.name;
            closeLibrary(lib);
            lib = nullptr;
            return false;
        }
    }

    retro_system_info info{};
    api->getSystemInfo(&info);
    name = info.library_name ? info.library_name : "";
    return true;
}

unsigned Core::apiVersion() const
{
    return (lib && api && api->apiVersion) ? api->apiVersion() : 0;
}

bool Core::start(std::string& error)
{
    return start({}, error);
}

bool Core::start(const std::string& contentPath, std::string& error)
{
    if (!lib)
    {
        error = "no core loaded";
        return false;
    }
    if (isStarted)
    {
        error = "core already started";
        return false;
    }
    if (active)
    {
        error = "another core instance is active in this process";
        return false;
    }

    active = this;
    systemDir = coreDir;
    saveDir = coreDir;

    api->setEnvironment(&Core::onEnvironment);
    api->init();
    api->setVideoRefresh(&Core::onVideoRefresh);
    api->setAudioSample(&Core::onAudioSample);
    api->setAudioSampleBatch(&Core::onAudioSampleBatch);
    api->setInputPoll(&Core::onInputPoll);
    api->setInputState(&Core::onInputState);

    // Contentless cores (Mr.Boom) get a null game_info; content cores get the
    // file by path or, when need_fullpath is false, loaded into memory
    retro_game_info gameInfo{};
    const retro_game_info* game = nullptr;
    std::vector<uint8_t> contentData;
    if (!contentPath.empty())
    {
        retro_system_info sysInfo{};
        api->getSystemInfo(&sysInfo);
        gameInfo.path = contentPath.c_str();
        if (!sysInfo.need_fullpath)
        {
            std::ifstream file(std::filesystem::path(contentPath), std::ios::binary);
            if (!file)
            {
                error = "content file not readable: " + contentPath;
                api->deinit();
                active = nullptr;
                return false;
            }
            contentData.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            gameInfo.data = contentData.data();
            gameInfo.size = contentData.size();
        }
        game = &gameInfo;
    }

    if (!api->loadGame(game))
    {
        error = game ? "retro_load_game failed for content: " + contentPath
                     : "retro_load_game(no content) failed - the core may require a content file";
        api->deinit();
        active = nullptr;
        return false;
    }

    retro_system_av_info avInfo{};
    api->getSystemAvInfo(&avInfo);
    av.fps = avInfo.timing.fps;
    av.sampleRate = avInfo.timing.sample_rate;
    av.baseWidth = avInfo.geometry.base_width;
    av.baseHeight = avInfo.geometry.base_height;
    av.maxWidth = avInfo.geometry.max_width;
    av.maxHeight = avInfo.geometry.max_height;

    isStarted = true;
    return true;
}

void Core::runFrame()
{
    if (!isStarted)
        throw std::logic_error("Core::runFrame() called before start()");
    frameCounter++;
    api->run();
}

void Core::stop()
{
    if (!isStarted)
        return;
    api->unloadGame();
    api->deinit();
    isStarted = false;
    if (active == this)
        active = nullptr;
}

void Core::unload()
{
    stop();
    if (lib)
    {
        closeLibrary(lib);
        lib = nullptr;
    }
}

bool Core::onEnvironment(unsigned cmd, void* data)
{
    Core* self = active;
    if (!self)
        return false;

    switch (cmd)
    {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            self->pixelFormat = *static_cast<const int*>(data);
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            static_cast<retro_log_callback*>(data)->log = self->verbose ? &logStderr : &logNull;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *static_cast<const char**>(data) = self->systemDir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char**>(data) = self->saveDir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;
            return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool*>(data) = false;
            return true;
        default:
            // Unhandled queries: report "not supported" so the core uses defaults
            return false;
    }
}

void Core::onVideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch)
{
    Core* self = active;
    if (!self || !data)  // NULL means duplicate frame, keep the previous one
        return;
    self->convertFrame(data, width, height, pitch);
}

void Core::convertFrame(const void* data, unsigned width, unsigned height, size_t pitch)
{
    frame.width = width;
    frame.height = height;
    frame.index = frameCounter;
    frame.pixels.resize(static_cast<size_t>(width) * height * 3);

    const auto* src = static_cast<const uint8_t*>(data);
    uint8_t* dst = frame.pixels.data();

    // the format is fixed for the whole frame - dispatching once and keeping
    // the pixel loops branch-free lets the compiler vectorize them (writing
    // through uint8_t* would otherwise force a pixelFormat reload per pixel)
    if (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888)
    {
        for (unsigned y = 0; y < height; y++)
        {
            const auto* row = reinterpret_cast<const uint32_t*>(src + y * pitch);
            for (unsigned x = 0; x < width; x++)
            {
                const uint32_t px = row[x];
                *dst++ = static_cast<uint8_t>((px >> 16) & 0xFF);
                *dst++ = static_cast<uint8_t>((px >> 8) & 0xFF);
                *dst++ = static_cast<uint8_t>(px & 0xFF);
            }
        }
    }
    else if (pixelFormat == RETRO_PIXEL_FORMAT_RGB565)
    {
        for (unsigned y = 0; y < height; y++)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(src + y * pitch);
            for (unsigned x = 0; x < width; x++)
            {
                const uint16_t px = row[x];
                *dst++ = static_cast<uint8_t>(((px >> 11) & 0x1F) << 3);
                *dst++ = static_cast<uint8_t>(((px >> 5) & 0x3F) << 2);
                *dst++ = static_cast<uint8_t>((px & 0x1F) << 3);
            }
        }
    }
    else  // RETRO_PIXEL_FORMAT_0RGB1555 (the libretro default)
    {
        for (unsigned y = 0; y < height; y++)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(src + y * pitch);
            for (unsigned x = 0; x < width; x++)
            {
                const uint16_t px = row[x];
                *dst++ = static_cast<uint8_t>(((px >> 10) & 0x1F) << 3);
                *dst++ = static_cast<uint8_t>(((px >> 5) & 0x1F) << 3);
                *dst++ = static_cast<uint8_t>((px & 0x1F) << 3);
            }
        }
    }
}

void Core::onAudioSample(int16_t left, int16_t right)
{
    Core* self = active;
    if (!self || !self->audioSink)
        return;
    const int16_t frame[2] = {left, right};
    self->audioSink(frame, 1);
}

size_t Core::onAudioSampleBatch(const int16_t* data, size_t frames)
{
    Core* self = active;
    if (self && self->audioSink && data && frames > 0)
        self->audioSink(data, frames);
    return frames;
}

void Core::onInputPoll()
{
}

int16_t Core::onInputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
    Core* self = active;
    if (!self || !self->inputState)
        return 0;
    return self->inputState(port, device, index, id);
}

}
