#include <player_module/audio_output_device_impl.h>

#include <game_engine_shared/audio_metadata.h>
#include <game_engine_shared/daq_string.h>

#include <coreobjects/property_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <opendaq/custom_log.h>
#include <opendaq/data_descriptor_ptr.h>
#include <opendaq/data_packet_ptr.h>
#include <opendaq/device_info_factory.h>
#include <opendaq/device_type_factory.h>
#include <opendaq/event_packet_ids.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/event_packet_ptr.h>
#include <opendaq/function_block_type_factory.h>

#ifdef _WIN32
    #include <combaseapi.h>
#endif

#include <algorithm>
#include <cstring>

BEGIN_NAMESPACE_PLAYER_MODULE

namespace audio_meta = game_engine::audio_meta;
using game_engine::toStd;

namespace
{
    // WASAPI wants COM initialized on threads doing device init/enumeration
    // (same guard as the SDK audio device example)
    class ComGuard
    {
    public:
        ComGuard()
        {
#ifdef _WIN32
            shouldUninit = CoInitializeEx(nullptr, COINIT_MULTITHREADED) == S_OK;
#endif
        }
        ~ComGuard()
        {
#ifdef _WIN32
            if (shouldUninit)
                CoUninitialize();
#endif
        }

    private:
        bool shouldUninit = false;
    };
}

MiniaudioContext::MiniaudioContext()
{
    ComGuard guard;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
        throw std::runtime_error("failed to initialize the miniaudio context");
}

MiniaudioContext::~MiniaudioContext()
{
    ComGuard guard;
    ma_context_uninit(&context);
}

// --- channel -----------------------------------------------------------------

AudioOutputChannelImpl::AudioOutputChannelImpl(const ContextPtr& ctx,
                                               const ComponentPtr& parent,
                                               const StringPtr& localId,
                                               std::shared_ptr<MiniaudioContext> maContext,
                                               bool useDefaultDevice,
                                               const ma_device_id& deviceId)
    : Channel(CreateType(), ctx, parent, localId)
    , maContext(std::move(maContext))
    , useDefaultDevice(useDefaultDevice)
    , deviceId(deviceId)
{
    initProperties();
    inputPort = createAndAddInputPort("Audio", PacketReadyNotification::SchedulerQueueWasEmpty);
}

AudioOutputChannelImpl::~AudioOutputChannelImpl()
{
    stopPlayback();
}

FunctionBlockTypePtr AudioOutputChannelImpl::CreateType()
{
    return FunctionBlockType("AudioOutputChannel",
                             "AudioOutputChannel",
                             "Audio playback channel: binary PCM audio signal in, sound out");
}

void AudioOutputChannelImpl::initProperties()
{
    // playback format mirrored from the connected signal's descriptor
    objPtr.addProperty(IntPropertyBuilder("SampleRate", 0).setReadOnly(True).build());
    objPtr.addProperty(IntPropertyBuilder("Channels", 0).setReadOnly(True).build());
    // false when miniaudio could not open the playback device (packets are
    // still received and counted)
    objPtr.addProperty(BoolPropertyBuilder("PlaybackActive", False).setReadOnly(True).build());
    objPtr.addProperty(IntPropertyBuilder("FramesReceived", 0).setReadOnly(True).build());

    objPtr.addProperty(FloatPropertyBuilder("Volume", 1.0).setMinValue(0.0).setMaxValue(1.0).build());
    objPtr.getOnPropertyValueWrite("Volume") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        { volume = static_cast<float>(static_cast<Float>(args.getValue())); };
}

void AudioOutputChannelImpl::onPacketReceived(const InputPortPtr& port)
{
    const auto connection = port.getConnection();
    if (!connection.assigned())
        return;

    for (auto packet = connection.dequeue(); packet.assigned(); packet = connection.dequeue())
    {
        if (packet.getType() == PacketType::Event)
        {
            const EventPacketPtr eventPacket = packet;
            if (eventPacket.getEventId() != event_packet_id::DATA_DESCRIPTOR_CHANGED)
                continue;

            int sampleRate = 0;
            int channels = 0;
            std::string format;
            const DataDescriptorPtr descriptor = eventPacket.getParameters().get(event_packet_param::DATA_DESCRIPTOR);
            if (descriptor.assigned() && descriptor.getMetadata().assigned())
            {
                for (const auto& [key, value] : descriptor.getMetadata())
                {
                    const std::string keyStr = toStd(key);
                    if (keyStr == audio_meta::SampleRate)
                        sampleRate = std::atoi(toStd(value).c_str());
                    else if (keyStr == audio_meta::Channels)
                        channels = std::atoi(toStd(value).c_str());
                    else if (keyStr == audio_meta::SampleFormat)
                        format = toStd(value);
                }
            }

            if (sampleRate > 0 && channels > 0 && format == "s16")
                configurePlayback(sampleRate, channels);
            else if (descriptor.assigned())
                LOG_I("connected signal descriptor has no s16 PCM metadata (yet) - waiting")
        }
        else if (packet.getType() == PacketType::Data)
        {
            const DataPacketPtr dataPacket = packet;
            const auto* pcm = static_cast<const uint8_t*>(dataPacket.getRawData());
            const auto bytes = dataPacket.getRawDataSize();
            if (pcm && bytes > 0)
                pushPcm(pcm, bytes);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (framesReceived > 0 && now - lastCounterPublish >= std::chrono::seconds(1))
    {
        lastCounterPublish = now;
        objPtr.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue("FramesReceived",
                                                                               static_cast<Int>(framesReceived));
    }
}

void AudioOutputChannelImpl::onDisconnected(const InputPortPtr& /*port*/)
{
    // keep the playback device running - it renders silence on an empty ring
    std::scoped_lock lock(ringSync);
    ringHead = 0;
    ringSize = 0;
}

void AudioOutputChannelImpl::configurePlayback(int sampleRate, int channels)
{
    if (playbackStarted && sampleRate == activeSampleRate && channels == activeChannels)
        return;

    stopPlayback();
    activeSampleRate = sampleRate;
    activeChannels = channels;

    {
        // half a second of buffered audio bounds the added latency
        std::scoped_lock lock(ringSync);
        ring.assign(static_cast<size_t>(sampleRate) * channels * 2 / 2, 0);
        ringHead = 0;
        ringSize = 0;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = useDefaultDevice ? nullptr : &deviceId;
    config.playback.format = ma_format_s16;
    config.playback.channels = static_cast<ma_uint32>(channels);
    config.sampleRate = static_cast<ma_uint32>(sampleRate);
    config.dataCallback = &AudioOutputChannelImpl::dataCallback;
    config.pUserData = this;

    ComGuard guard;
    if (ma_device_init(maContext->getPtr(), &config, &maDevice) != MA_SUCCESS)
    {
        LOG_W("miniaudio playback init failed - counting packets without playback")
    }
    else if (ma_device_start(&maDevice) != MA_SUCCESS)
    {
        LOG_W("miniaudio playback start failed - counting packets without playback")
        ma_device_uninit(&maDevice);
    }
    else
    {
        playbackStarted = true;
        LOG_I("playing {} Hz {}ch s16 PCM", sampleRate, channels)
    }

    const auto protectedObj = objPtr.asPtr<IPropertyObjectProtected>(true);
    protectedObj.setProtectedPropertyValue("SampleRate", static_cast<Int>(sampleRate));
    protectedObj.setProtectedPropertyValue("Channels", static_cast<Int>(channels));
    protectedObj.setProtectedPropertyValue("PlaybackActive", playbackStarted ? True : False);
}

void AudioOutputChannelImpl::stopPlayback()
{
    if (!playbackStarted)
        return;
    ComGuard guard;
    ma_device_uninit(&maDevice);  // waits for the callback to drain
    playbackStarted = false;
}

void AudioOutputChannelImpl::pushPcm(const uint8_t* pcm, size_t bytes)
{
    const size_t frameBytes = activeChannels > 0 ? static_cast<size_t>(activeChannels) * 2 : 4;
    framesReceived += bytes / frameBytes;

    std::scoped_lock lock(ringSync);
    if (ring.empty())
        return;  // no descriptor yet - nothing to play the data with

    if (bytes >= ring.size())
    {
        // more than a whole ring in one packet: keep only the newest ringful
        pcm += bytes - ring.size();
        bytes = ring.size();
        ringHead = 0;
        ringSize = 0;
    }
    // drop the oldest frames when full - latency stays bounded
    const size_t overflow = ringSize + bytes > ring.size() ? ringSize + bytes - ring.size() : 0;
    ringHead = (ringHead + overflow) % ring.size();
    ringSize -= overflow;

    size_t writePos = (ringHead + ringSize) % ring.size();
    const size_t firstChunk = std::min(bytes, ring.size() - writePos);
    std::memcpy(ring.data() + writePos, pcm, firstChunk);
    if (firstChunk < bytes)
        std::memcpy(ring.data(), pcm + firstChunk, bytes - firstChunk);
    ringSize += bytes;
}

void AudioOutputChannelImpl::dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount)
{
    auto* self = static_cast<AudioOutputChannelImpl*>(device->pUserData);
    auto* out = static_cast<uint8_t*>(output);
    const size_t wanted = static_cast<size_t>(frameCount) * self->activeChannels * 2;

    size_t copied = 0;
    {
        std::scoped_lock lock(self->ringSync);
        copied = std::min(wanted, self->ringSize);
        const size_t firstChunk = std::min(copied, self->ring.size() - self->ringHead);
        std::memcpy(out, self->ring.data() + self->ringHead, firstChunk);
        if (firstChunk < copied)
            std::memcpy(out + firstChunk, self->ring.data(), copied - firstChunk);
        self->ringHead = (self->ringHead + copied) % self->ring.size();
        self->ringSize -= copied;
    }
    std::memset(out + copied, 0, wanted - copied);  // underrun = silence

    const float vol = self->volume.load(std::memory_order_relaxed);
    if (vol < 1.0f)
    {
        auto* samples = static_cast<int16_t*>(output);
        for (size_t i = 0; i < wanted / 2; i++)
            samples[i] = static_cast<int16_t>(samples[i] * vol);
    }
}

// --- device --------------------------------------------------------------------

AudioOutputDeviceImpl::AudioOutputDeviceImpl(const ContextPtr& ctx,
                                             const ComponentPtr& parent,
                                             const StringPtr& localId,
                                             std::shared_ptr<MiniaudioContext> maContext,
                                             const StringPtr& connectionString)
    : Device(ctx, parent, localId)
{
    const int index = ParsePlaybackIndex(connectionString);

    bool useDefault = index < 0;
    ma_device_id id{};
    std::string name = "default playback device";
    if (!useDefault)
    {
        ComGuard guard;
        ma_device_info* playbackInfos = nullptr;
        ma_uint32 playbackCount = 0;
        if (ma_context_get_devices(maContext->getPtr(), &playbackInfos, &playbackCount, nullptr, nullptr) != MA_SUCCESS ||
            static_cast<ma_uint32>(index) >= playbackCount)
            DAQ_THROW_EXCEPTION(NotFoundException, "no playback device with index {}", index);
        id = playbackInfos[index].id;
        name = playbackInfos[index].name;
    }

    channel = createAndAddChannel<AudioOutputChannelImpl>(this->ioFolder, "Output", std::move(maContext), useDefault, id);

    auto info = DeviceInfo(connectionString);
    info.setName("AudioOutput (" + name + ")");
    info.setDeviceType(CreateType());
    this->deviceInfo = info;
}

DeviceTypePtr AudioOutputDeviceImpl::CreateType()
{
    return DeviceType("AudioOutput",
                      "AudioOutput",
                      "Plays a binary PCM audio signal on this PC's speakers",
                      "daqaudioout");
}

DeviceInfoPtr AudioOutputDeviceImpl::CreateDeviceInfo(const std::string& connectionString, const std::string& name)
{
    auto info = DeviceInfo(connectionString);
    info.setName("AudioOutput (" + name + ")");
    info.setDeviceType(CreateType());
    return info;
}

int AudioOutputDeviceImpl::ParsePlaybackIndex(const std::string& connectionString)
{
    const std::string prefix = "daqaudioout://";
    if (connectionString.rfind(prefix, 0) != 0)
        DAQ_THROW_EXCEPTION(InvalidParameterException, "AudioOutput connection strings start with {}", prefix);
    const std::string rest = connectionString.substr(prefix.size());
    if (rest.empty() || rest == "default")
        return -1;
    return std::atoi(rest.c_str());
}

END_NAMESPACE_PLAYER_MODULE
