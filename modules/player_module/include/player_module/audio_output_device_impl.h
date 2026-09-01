#pragma once
#include <player_module/common.h>
#include <opendaq/channel_impl.h>
#include <opendaq/channel_ptr.h>
#include <opendaq/device_impl.h>

#include <miniaudio.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

BEGIN_NAMESPACE_PLAYER_MODULE

// RAII miniaudio context shared by the module, the devices and their channels
// (mirrors the SDK audio device example's MiniaudioContext)
class MiniaudioContext
{
public:
    MiniaudioContext();
    ~MiniaudioContext();
    MiniaudioContext(const MiniaudioContext&) = delete;
    MiniaudioContext& operator=(const MiniaudioContext&) = delete;

    ma_context* getPtr() { return &context; }

private:
    ma_context context;
};

// The playback half of an AudioOutput device: a single input port accepting
// the game device's binary PCM audio signal. The playback format comes from
// the descriptor metadata (audio_meta convention); packets flow through a
// bounded ring the miniaudio callback drains (underruns play silence, a
// too-full ring drops its oldest frames to bound latency). Playback failures
// (no audio hardware) degrade to counting packets with a warning.
class AudioOutputChannelImpl final : public Channel
{
public:
    explicit AudioOutputChannelImpl(const ContextPtr& ctx,
                                    const ComponentPtr& parent,
                                    const StringPtr& localId,
                                    std::shared_ptr<MiniaudioContext> maContext,
                                    bool useDefaultDevice,
                                    const ma_device_id& deviceId);
    ~AudioOutputChannelImpl() override;

    static FunctionBlockTypePtr CreateType();

protected:
    void onPacketReceived(const InputPortPtr& port) override;
    void onDisconnected(const InputPortPtr& port) override;

private:
    void initProperties();
    void configurePlayback(int sampleRate, int channels);
    void stopPlayback();
    void pushPcm(const uint8_t* pcm, size_t bytes);
    static void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount);

    InputPortPtr inputPort;
    std::shared_ptr<MiniaudioContext> maContext;
    bool useDefaultDevice;
    ma_device_id deviceId;

    ma_device maDevice{};
    bool playbackStarted = false;
    int activeSampleRate = 0;
    int activeChannels = 0;

    // packet thread -> miniaudio callback handoff (circular, whole frames)
    std::mutex ringSync;
    std::vector<uint8_t> ring;
    size_t ringHead = 0;
    size_t ringSize = 0;

    std::atomic<float> volume{1.0f};

    // diagnostic (read-only "FramesReceived", published at most once a second)
    uint64_t framesReceived = 0;
    std::chrono::steady_clock::time_point lastCounterPublish{};
};

// Audio playback as an openDAQ device (modeled after the SDK's audio device
// example, in the output direction): `daqaudioout://default` or
// `daqaudioout://<index>` from the enumerated playback device list. The
// device owns one "Output" channel that does the actual work.
class AudioOutputDeviceImpl final : public Device
{
public:
    explicit AudioOutputDeviceImpl(const ContextPtr& ctx,
                                   const ComponentPtr& parent,
                                   const StringPtr& localId,
                                   std::shared_ptr<MiniaudioContext> maContext,
                                   const StringPtr& connectionString);

    static DeviceTypePtr CreateType();
    static DeviceInfoPtr CreateDeviceInfo(const std::string& connectionString, const std::string& name);

    // "daqaudioout://default" -> -1, "daqaudioout://<index>" -> index
    static int ParsePlaybackIndex(const std::string& connectionString);

private:
    ChannelPtr channel;
};

END_NAMESPACE_PLAYER_MODULE
