#include <player_module/player_fb_impl.h>
#include <player_module/keyboard_layout.h>

#include <game_engine_shared/daq_string.h>
#include <game_engine_shared/video_metadata.h>

#include <coreobjects/callable_info_factory.h>
#include <coreobjects/property_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <coreobjects/unit_factory.h>
#include <coretypes/procedure_factory.h>
#include <coretypes/ratio_factory.h>
#include <opendaq/custom_log.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/data_packet_ptr.h>
#include <opendaq/data_rule_factory.h>
#include <opendaq/event_packet_ids.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/event_packet_ptr.h>
#include <opendaq/function_block_type_factory.h>
#include <opendaq/packet_factory.h>

#include <SDL.h>
#ifdef _WIN32
    #include <SDL_main.h>  // SDL_RegisterApp
#endif

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>

BEGIN_NAMESPACE_PLAYER_MODULE

namespace video_meta = game_engine::video_meta;
using game_engine::toStd;

namespace
{
    constexpr size_t MaxQueuedPackets = 4;

    DataDescriptorPtr buildStateDescriptor()
    {
        // Struct descriptor (two uint64 words = the 128-bit key bitmap); the
        // key name -> bit index mapping is published as descriptor metadata.
        auto meta = Dict<IString, IString>();
        meta.set("Layout", "ANSI_US");
        for (unsigned bit = 0; bit < AnsiKeyCount; bit++)
            meta.set(game_engine::KeyBitMetadataPrefix + std::string(AnsiKeys[bit].name), std::to_string(bit));

        const auto low = DataDescriptorBuilder().setName("KeysLow").setSampleType(SampleType::UInt64).build();
        const auto high = DataDescriptorBuilder().setName("KeysHigh").setSampleType(SampleType::UInt64).build();

        return DataDescriptorBuilder()
            .setName("KeyboardState")
            .setSampleType(SampleType::Struct)
            .setStructFields(List<IDataDescriptor>(low, high))
            .setMetadata(meta)
            .build();
    }

    DataDescriptorPtr buildTimeDescriptor()
    {
        return DataDescriptorBuilder()
            .setName("Time")
            .setSampleType(SampleType::Int64)
            .setRule(ExplicitDataRule())
            .setTickResolution(Ratio(1, 1'000'000))
            .setOrigin("1970-01-01T00:00:00Z")
            .setUnit(Unit("s", -1, "seconds", "time"))
            .build();
    }

    int64_t nowMicroseconds()
    {
        using namespace std::chrono;
        return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    }

    // VP8 packets reference earlier frames, so skipping ahead is only sound at
    // a keyframe. MJPEG packets all stand alone. Unknown codecs never skip
    // ahead - safe (just laggier) instead of corrupting an inter-frame stream.
    bool isKeyframe(const std::string& codec, const uint8_t* data, size_t size)
    {
        if (codec == "vp8")
            return video_meta::isVp8Keyframe(data, size);
        return codec == "mjpeg";
    }

    // deprecated yuvj* aliases make sws_getCachedContext miss its cache (the
    // context stores the normalized format) and warn on every frame - map
    // them to the plain format + full range up front
    AVPixelFormat normalizePixelFormat(AVPixelFormat format, bool& fullRange)
    {
        switch (format)
        {
            case AV_PIX_FMT_YUVJ420P:
                fullRange = true;
                return AV_PIX_FMT_YUV420P;
            case AV_PIX_FMT_YUVJ422P:
                fullRange = true;
                return AV_PIX_FMT_YUV422P;
            case AV_PIX_FMT_YUVJ444P:
                fullRange = true;
                return AV_PIX_FMT_YUV444P;
            default:
                return format;
        }
    }

    // FFmpeg decoder + RGB24 conversion stage, configured from stream metadata
    class Decoder
    {
    public:
        ~Decoder() { close(); }

        bool open(const std::string& codecName, int width, int height, std::string& errorOut)
        {
            close();
            const AVCodec* codec = avcodec_find_decoder_by_name(codecName.c_str());
            if (!codec)
            {
                errorOut = "no decoder for codec '" + codecName + "'";
                return false;
            }
            ctx = avcodec_alloc_context3(codec);
            ctx->width = width;
            ctx->height = height;
            if (const int err = avcodec_open2(ctx, codec, nullptr); err < 0)
            {
                errorOut = "avcodec_open2 failed: " + std::to_string(err);
                close();
                return false;
            }
            packet = av_packet_alloc();
            frame = av_frame_alloc();
            return true;
        }

        bool opened() const { return ctx != nullptr; }

        // decodes one encoded packet; sink(rgb24, width, height, stride) is
        // called for each complete frame
        template <typename Sink>
        bool decode(const std::vector<uint8_t>& encoded, const Sink& sink)
        {
            if (av_new_packet(packet, static_cast<int>(encoded.size())) < 0)
                return false;
            std::memcpy(packet->data, encoded.data(), encoded.size());
            const int sendResult = avcodec_send_packet(ctx, packet);
            av_packet_unref(packet);
            if (sendResult < 0)
                return false;

            bool produced = false;
            while (avcodec_receive_frame(ctx, frame) == 0)
            {
                bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;
                const AVPixelFormat srcFormat = normalizePixelFormat(static_cast<AVPixelFormat>(frame->format), fullRange);
                const bool newContext = !sws || srcFormat != lastSrcFormat || frame->width != lastWidth ||
                                        frame->height != lastHeight;
                sws = sws_getCachedContext(sws,
                                           frame->width,
                                           frame->height,
                                           srcFormat,
                                           frame->width,
                                           frame->height,
                                           AV_PIX_FMT_RGB24,
                                           SWS_BILINEAR,
                                           nullptr,
                                           nullptr,
                                           nullptr);
                if (!sws)
                    continue;
                if (newContext)
                {
                    const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
                    sws_setColorspaceDetails(sws, coefficients, fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16);
                    lastSrcFormat = srcFormat;
                    lastWidth = frame->width;
                    lastHeight = frame->height;
                }

                const size_t stride = static_cast<size_t>(frame->width) * 3;
                rgb.resize(stride * frame->height);
                uint8_t* dstData[1] = {rgb.data()};
                const int dstStride[1] = {static_cast<int>(stride)};
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstData, dstStride);

                sink(rgb.data(), frame->width, frame->height, static_cast<int>(stride));
                produced = true;
            }
            return produced;
        }

        void close()
        {
            if (frame)
                av_frame_free(&frame);
            if (packet)
                av_packet_free(&packet);
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
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        SwsContext* sws = nullptr;
        AVPixelFormat lastSrcFormat = AV_PIX_FMT_NONE;
        int lastWidth = 0;
        int lastHeight = 0;
        std::vector<uint8_t> rgb;
    };

    // The player's single SDL window: shows the video (or a focus indicator
    // while no stream plays) and is the keyboard capture surface - capture
    // requires focus, keys are the caller's business via pumpEvents.
    class Window
    {
    public:
        bool open(int scale)
        {
            close();
#ifdef _WIN32
            // every module dll carries its own static SDL; without a distinct
            // window class name they all register "SDL_app" and window
            // creation fails against a foreign WndProc
            SDL_RegisterApp("DAQGamePlayer", 0, nullptr);
#endif
            if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
                return false;
            window = SDL_CreateWindow(TitleUnfocused,
                                      SDL_WINDOWPOS_CENTERED,
                                      SDL_WINDOWPOS_CENTERED,
                                      IndicatorWidth * scale / 3,
                                      IndicatorHeight * scale / 3,
                                      SDL_WINDOW_RESIZABLE);
            if (window)
                renderer = SDL_CreateRenderer(window, -1, 0);
            if (!renderer)
            {
                close();
                return false;
            }
            // when spawned inside another process's GUI (e.g. the openDAQ TK
            // app) Windows leaves a background thread's new window behind the
            // foreground one; raise it so it is visible and can take focus
            SDL_RaiseWindow(window);
            focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
            SDL_SetWindowTitle(window, focused ? TitleFocused : TitleUnfocused);
            return true;
        }

        bool isOpen() const { return renderer != nullptr; }
        bool isFocused() const { return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0; }

        void present(const uint8_t* rgb24, int width, int height, int stride, int scale)
        {
            if (!renderer)
                return;
            if (!texture || textureWidth != width || textureHeight != height || scale != appliedScale)
            {
                if (texture)
                    SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, width, height);
                textureWidth = width;
                textureHeight = height;
                SDL_RenderSetLogicalSize(renderer, width, height);
                SDL_SetWindowSize(window, width * scale, height * scale);
                appliedScale = scale;
            }
            if (!texture)
                return;
            SDL_UpdateTexture(texture, nullptr, rgb24, stride);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        // no video yet: the old capture-window indicator - green when focused,
        // brighter as more keys are held
        void presentIndicator(unsigned pressedKeys)
        {
            if (!renderer || texture)
                return;
            const bool focused = isFocused();
            const auto pressed = static_cast<uint8_t>(std::min(pressedKeys * 40u, 160u));
            SDL_SetRenderDrawColor(renderer,
                                   focused ? 30 : 70,
                                   focused ? static_cast<uint8_t>(90 + pressed) : 70,
                                   focused ? 40 : 70,
                                   255);
            SDL_RenderClear(renderer);
            SDL_RenderPresent(renderer);
        }

        // pumps this module's SDL event queue; key events go to onKey, false
        // is returned when the user closed the window
        template <typename OnKey, typename OnFocusLost>
        bool pumpEvents(const OnKey& onKey, const OnFocusLost& onFocusLost)
        {
            bool keepOpen = true;
            SDL_Event event;
            while (window && SDL_PollEvent(&event))
            {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
                    keepOpen = false;
                else if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && event.key.repeat == 0)
                    onKey(event.key.keysym.scancode, event.type == SDL_KEYDOWN);
                else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                    setFocused(true);
                else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                {
                    setFocused(false);
                    onFocusLost();
                }
            }
            return keepOpen;
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
            textureWidth = 0;
            textureHeight = 0;
            appliedScale = 0;
            if (SDL_WasInit(SDL_INIT_VIDEO))
            {
                SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
#ifdef _WIN32
                SDL_UnregisterApp();
#endif
            }
        }

    private:
        static constexpr int IndicatorWidth = 360;
        static constexpr int IndicatorHeight = 160;
        static constexpr const char* TitleFocused = "DAQ Jam Player";
        static constexpr const char* TitleUnfocused = "DAQ Jam Player - click here to play";

        // the title only says "click to play" while the window really lacks
        // keyboard focus, so it stops nagging once the game is playable
        void setFocused(bool value)
        {
            if (value == focused)
                return;
            focused = value;
            if (window)
                SDL_SetWindowTitle(window, focused ? TitleFocused : TitleUnfocused);
        }

        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* texture = nullptr;
        int textureWidth = 0;
        int textureHeight = 0;
        int appliedScale = 0;
        bool focused = false;
    };
}

PlayerFbImpl::PlayerFbImpl(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId)
    : FunctionBlock(CreateType(), ctx, parent, localId)
{
    initProperties();
    playerThread = std::thread{&PlayerFbImpl::playerLoop, this};
}

std::atomic<PlayerFbImpl*> PlayerFbImpl::sdlOwner{nullptr};

PlayerFbImpl::~PlayerFbImpl()
{
    stopRequested = true;
    queueSignal.notify_all();
    if (playerThread.joinable())
        playerThread.join();

    // hand SDL back so a later Player FB (or a Restart on one) can open a window
    PlayerFbImpl* expected = this;
    sdlOwner.compare_exchange_strong(expected, nullptr);
}

FunctionBlockTypePtr PlayerFbImpl::CreateType()
{
    return FunctionBlockType("Player",
                             "Player",
                             "Game player: replays the game video and captures the keyboard while focused");
}

void PlayerFbImpl::initProperties()
{
    // No output signals until this is filled: the tag is baked into the signal
    // ids so several clients' keyboards get distinct global ids on the server.
    // It can be set once - changing it would silently re-identify signals that
    // may already be connected.
    objPtr.addProperty(StringProperty("PlayerTag", ""));
    objPtr.getOnPropertyValueWrite("PlayerTag") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        { applyPlayerTag(static_cast<StringPtr>(args.getValue()).toStdString()); };

    const auto heartbeatProp =
        IntPropertyBuilder("HeartbeatRateHz", 10).setMinValue(0).setMaxValue(1000).setUnit(Unit("Hz")).build();
    objPtr.addProperty(heartbeatProp);
    objPtr.getOnPropertyValueWrite("HeartbeatRateHz") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args) { heartbeatRateHz = static_cast<int>(static_cast<Int>(args.getValue())); };

    // Video replay is opt-in: the "Video" input port exists only while this is
    // true, so a capture-only player carries no port to mis-wire (and never
    // forms the in-process keyboard+video signal cycle the SDK refuses)
    objPtr.addProperty(BoolProperty("EnableVideo", False));
    objPtr.getOnPropertyValueWrite("EnableVideo") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        { applyEnableVideo(static_cast<Bool>(args.getValue())); };

    // Packet ordering is guaranteed by the transport, so timestamps are pure
    // overhead unless a use case needs them explicitly.
    objPtr.addProperty(BoolProperty("EnableTimeSignal", False));
    objPtr.getOnPropertyValueWrite("EnableTimeSignal") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        {
            timeSignalEnabled = static_cast<Bool>(args.getValue());
            applyTimeSignalSetting();
        };

    objPtr.addProperty(IntPropertyBuilder("WindowScale", 3).setMinValue(1).setMaxValue(8).build());
    objPtr.getOnPropertyValueWrite("WindowScale") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        { windowScale = static_cast<int>(static_cast<Int>(args.getValue())); };

    // stream metadata mirrored from the connected signal's descriptor
    objPtr.addProperty(StringPropertyBuilder("Codec", "").setReadOnly(True).build());
    objPtr.addProperty(IntPropertyBuilder("StreamWidth", 0).setReadOnly(True).build());
    objPtr.addProperty(IntPropertyBuilder("StreamHeight", 0).setReadOnly(True).build());
    objPtr.addProperty(StringPropertyBuilder("Framerate", "").setReadOnly(True).build());

    // decode progress diagnostic, updated about once a second while playing
    objPtr.addProperty(IntPropertyBuilder("FramesDecoded", 0).setReadOnly(True).build());

    // reopens the window (with capture and rendering) after the user closed it
    objPtr.addProperty(FunctionPropertyBuilder("Restart", ProcedureInfo()).setReadOnly(True).build());
    objPtr.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue(
        "Restart",
        Procedure([this]
        {
            restartRequested = true;
            queueSignal.notify_all();
        }));
}

void PlayerFbImpl::applyPlayerTag(const std::string& tag)
{
    if (tag == playerTag)
        return;
    if (!playerTag.empty())
        DAQ_THROW_EXCEPTION(InvalidStateException, "PlayerTag can only be set once (signals are identified by it)");
    if (tag.empty())
        return;
    for (const char c : tag)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            DAQ_THROW_EXCEPTION(InvalidParameterException, "PlayerTag may only contain letters, digits, '-' and '_'");
    }

    std::scoped_lock lock(emitSync);
    playerTag = tag;
    // the tag lands in the local (and so the global) id; the name stays
    // "State"/"Time" so consumers keep finding the signals by name
    stateSignal = createAndAddSignal("State_" + tag, buildStateDescriptor());
    stateSignal.setName("State");
    timeSignal = createAndAddSignal("Time_" + tag, buildTimeDescriptor(), false);
    timeSignal.setName("Time");
    if (timeSignalEnabled)
        stateSignal.setDomainSignal(timeSignal);
}

void PlayerFbImpl::applyEnableVideo(bool enable)
{
    if (enable == inputPort.assigned())
        return;

    if (enable)
    {
        inputPort = createAndAddInputPort("Video", PacketReadyNotification::SchedulerQueueWasEmpty);
        return;
    }

    // removal disconnects the port; an unassigned descriptor resets the
    // mirrored stream properties and drops everything in flight, so the
    // window falls back to the indicator
    removeInputPort(inputPort);
    inputPort = nullptr;
    handleDescriptorChanged(nullptr);
}

void PlayerFbImpl::applyTimeSignalSetting()
{
    std::scoped_lock lock(emitSync);
    if (!stateSignal.assigned())
        return;
    if (timeSignalEnabled)
        stateSignal.setDomainSignal(timeSignal);
    else
        stateSignal.setDomainSignal(nullptr);
}

void PlayerFbImpl::emitState(const KeyboardState& state)
{
    std::scoped_lock lock(emitSync);
    if (!stateSignal.assigned())
        return;

    DataPacketPtr dataPacket;
    if (timeSignalEnabled)
    {
        auto domainPacket = DataPacket(timeSignal.getDescriptor(), 1);
        *static_cast<int64_t*>(domainPacket.getRawData()) = nowMicroseconds();
        dataPacket = DataPacketWithDomain(domainPacket, stateSignal.getDescriptor(), 1);
        std::memcpy(dataPacket.getRawData(), state.data(), KeyboardState::ByteSize);
        timeSignal.sendPacket(domainPacket);
    }
    else
    {
        dataPacket = DataPacket(stateSignal.getDescriptor(), 1);
        std::memcpy(dataPacket.getRawData(), state.data(), KeyboardState::ByteSize);
    }
    stateSignal.sendPacket(dataPacket);
}

void PlayerFbImpl::onPacketReceived(const InputPortPtr& port)
{
    const auto connection = port.getConnection();
    if (!connection.assigned())
        return;

    for (auto packet = connection.dequeue(); packet.assigned(); packet = connection.dequeue())
    {
        if (packet.getType() == PacketType::Event)
        {
            const EventPacketPtr eventPacket = packet;
            if (eventPacket.getEventId() == event_packet_id::DATA_DESCRIPTOR_CHANGED)
                handleDescriptorChanged(eventPacket.getParameters().get(event_packet_param::DATA_DESCRIPTOR));
        }
        else if (packet.getType() == PacketType::Data)
        {
            const DataPacketPtr dataPacket = packet;
            const auto* data = static_cast<const uint8_t*>(dataPacket.getRawData());
            const auto size = dataPacket.getRawDataSize();
            if (!data || size == 0)
                continue;

            std::scoped_lock lock(queueSync);
            // when behind, the backlog is dropped as a whole - but only once a
            // keyframe arrives to restart decode from; dropping arbitrary
            // packets would corrupt an inter-frame codec until the next
            // keyframe (for MJPEG every packet is one, and the backlog stays
            // bounded by the encoder's one-second keyframe interval either way)
            if (packetQueue.size() >= MaxQueuedPackets && isKeyframe(streamConfig.codec, data, size))
                packetQueue.clear();
            packetQueue.emplace_back(data, data + size);
        }
    }
    queueSignal.notify_all();
}

void PlayerFbImpl::handleDescriptorChanged(const DataDescriptorPtr& descriptor)
{
    StreamConfig config;
    if (descriptor.assigned() && descriptor.getMetadata().assigned())
    {
        for (const auto& [key, value] : descriptor.getMetadata())
        {
            const std::string keyStr = toStd(key);
            const std::string valueStr = toStd(value);
            if (keyStr == video_meta::CodecId)
                config.codec = valueStr;
            else if (keyStr == video_meta::Width)
                config.width = std::atoi(valueStr.c_str());
            else if (keyStr == video_meta::Height)
                config.height = std::atoi(valueStr.c_str());
            else if (keyStr == video_meta::Framerate)
                config.framerate = valueStr;
        }
    }

    const auto protectedObj = objPtr.asPtr<IPropertyObjectProtected>(true);
    protectedObj.setProtectedPropertyValue("Codec", String(config.codec));
    protectedObj.setProtectedPropertyValue("StreamWidth", static_cast<Int>(config.width));
    protectedObj.setProtectedPropertyValue("StreamHeight", static_cast<Int>(config.height));
    protectedObj.setProtectedPropertyValue("Framerate", String(config.framerate));

    // normal when connecting before the game starts - the placeholder
    // descriptor carries no metadata until the device configures the stream
    // (nullptr descriptor = video disabled, nothing to wait for)
    if (config.codec.empty() && inputPort.assigned())
        LOG_I("connected signal descriptor has no '{}' metadata (yet) - waiting", video_meta::CodecId)

    std::scoped_lock lock(queueSync);
    config.version = streamConfig.version + 1;
    streamConfig = std::move(config);
    packetQueue.clear();  // stale packets predate the new descriptor
    queueSignal.notify_all();
}

void PlayerFbImpl::playerLoop()
{
    using namespace std::chrono;

    Decoder decoder;
    Window window;
    StreamConfig config;         // local snapshot
    KeyboardState keys;
    bool windowWanted = true;    // false after the user closes the window
    bool windowFailed = false;
    uint64_t decoded = 0;
    auto lastHeartbeat = steady_clock::now();
    auto lastIndicator = steady_clock::now();
    auto lastCounterUpdate = steady_clock::now();

    const auto publishCounter = [&]
    {
        if (stopRequested)
            return;
        objPtr.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue("FramesDecoded",
                                                                               static_cast<Int>(decoded));
    };

    const auto onKey = [&](SDL_Scancode scancode, bool down)
    {
        const int bit = scancodeToBit(scancode);
        if (bit >= 0)
        {
            keys.set(static_cast<unsigned>(bit), down);
            emitState(keys);
        }
    };
    const auto onFocusLost = [&]
    {
        // release everything when focus is lost so no key gets stuck
        if (keys.pressedCount() > 0)
        {
            keys = KeyboardState{};
            emitState(keys);
        }
    };

    emitState(keys);  // initial all-released state (no-op until PlayerTag is set)

    while (!stopRequested)
    {
        const int heartbeat = heartbeatRateHz;
        // short wait while a window is open (this loop is also the keyboard
        // capture pump); with no window there is nothing to pump, so idle at
        // the heartbeat cadence instead of waking 500 times a second (packet
        // arrival, Restart and destruction all notify the condvar anyway)
        const auto wait = window.isOpen()
                              ? milliseconds(2)
                              : milliseconds(heartbeat > 0 ? std::clamp(1000 / heartbeat, 2, 50) : 50);

        std::vector<uint8_t> encoded;
        {
            std::unique_lock lock(queueSync);
            queueSignal.wait_for(lock, wait, [this] { return stopRequested || !packetQueue.empty(); });
            if (stopRequested)
                break;

            if (streamConfig.version != config.version)
            {
                config = streamConfig;
                decoder.close();
                windowFailed = false;
            }

            if (!packetQueue.empty())
            {
                encoded = std::move(packetQueue.front());
                packetQueue.pop_front();
            }
        }

        if (restartRequested.exchange(false))
        {
            window.close();
            windowWanted = true;
            windowFailed = false;
        }

        if (windowWanted && !window.isOpen() && !windowFailed)
        {
            PlayerFbImpl* expected = nullptr;
            if (sdlOwner.load(std::memory_order_acquire) != this && !sdlOwner.compare_exchange_strong(expected, this))
            {
                // SDL video is single-threaded per process and another FB's
                // thread owns it (Restart retries, in case that FB is gone)
                windowFailed = true;
                LOG_W("another Player FB owns the SDL window in this process - decoding without display or capture")
            }
            else if (!window.open(windowScale.load()))
            {
                windowFailed = true;  // headless box - keep decoding, no window/capture
                LOG_W("no player window ({}) - decoding without display or capture", SDL_GetError())
            }
        }

        if (window.isOpen() && !window.pumpEvents(onKey, onFocusLost))
        {
            onFocusLost();  // closing the window ends capture - drop held keys
            window.close();
            windowWanted = false;
            LOG_I("player window closed - call Restart to reopen")
        }

        const auto now = steady_clock::now();
        if (heartbeat > 0 && now - lastHeartbeat >= microseconds(1'000'000 / heartbeat))
        {
            emitState(keys);
            lastHeartbeat = now;
        }

        if (!encoded.empty())
        {
            if (!decoder.opened() && !config.codec.empty())
            {
                std::string error;
                if (!decoder.open(config.codec, config.width, config.height, error))
                {
                    LOG_E("decoder setup failed: {}", error)
                    config.codec.clear();  // don't retry until the next descriptor
                }
                else
                {
                    LOG_I("decoding {} {}x{} @ {}", config.codec, config.width, config.height, config.framerate)
                }
            }
            if (decoder.opened())
            {
                decoder.decode(encoded,
                               [&](const uint8_t* rgb, int width, int height, int stride)
                               {
                                   decoded++;
                                   window.present(rgb, width, height, stride, windowScale.load());
                               });
            }
        }
        else if (window.isOpen() && now - lastIndicator >= milliseconds(50))
        {
            window.presentIndicator(keys.pressedCount());
            lastIndicator = now;
        }

        if (now - lastCounterUpdate >= seconds(1))
        {
            publishCounter();
            lastCounterUpdate = now;
        }
    }

    publishCounter();
    window.close();
}

END_NAMESPACE_PLAYER_MODULE
