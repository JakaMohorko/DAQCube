#pragma once
#include <daqcube_module/common.h>
#include <daqcube_module/retropad_mapping.h>
#include <game_engine_shared/keyboard_state.h>
#include <opendaq/channel_impl.h>
#include <opendaq/signal_config_ptr.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

BEGIN_NAMESPACE_DAQCUBE_MODULE

// One fixed player channel under the DAQCube device: the single input
// port accepts a keyboard state signal, and the "Video" output signal carries
// that player's encoded game video (the device pushes the same packet to all
// player channels while everyone shares one feed). Incoming keyboard bitmaps
// are translated to a RetroPad button mask through the descriptor's
// "Bit.<Name>" metadata and forwarded to the device, which writes them into
// the core host's shared input block.
//
// The key bindings are per-player properties (Key<Button> =
// comma-separated key names), so several players can share one keyboard
// signal with disjoint bindings - single-PC multiplayer.
class PlayerChannelImpl final : public Channel
{
public:
    // onButtons(playerIndex, retroPadMask) is called on the input port's
    // notification thread whenever the translated button mask changes
    using ButtonsCallback = std::function<void(unsigned playerIndex, uint32_t buttons)>;

    explicit PlayerChannelImpl(const ContextPtr& ctx,
                               const ComponentPtr& parent,
                               const StringPtr& localId,
                               unsigned playerIndex,
                               ButtonsCallback onButtons);

    static FunctionBlockTypePtr CreateType();

    // read-only "Available" property, driven by the device's selected game
    void setAvailable(bool available);

    // this player's encoded video / PCM audio outputs; the device sets the
    // descriptors when a session starts and pushes the packets
    SignalConfigPtr getVideoSignal() const { return videoSignal; }
    SignalConfigPtr getAudioSignal() const { return audioSignal; }

protected:
    void onPacketReceived(const InputPortPtr& port) override;
    void onDisconnected(const InputPortPtr& port) override;

private:
    void initProperties();
    void rebuildBitMapping();
    void processState(const game_engine::KeyboardState& state);

    unsigned playerIndex;
    ButtonsCallback onButtons;
    InputPortPtr inputPort;
    SignalConfigPtr videoSignal;
    SignalConfigPtr audioSignal;

    // guards the binding state below (port thread vs. property writes)
    std::mutex mappingSync;
    // key name lists per binding slot, parsed from the Key<Button> properties
    std::array<std::vector<std::string>, std::size(KeyBindings)> boundKeys;
    DataDescriptorPtr lastDescriptor;
    // keyboard bit index -> RETRO_DEVICE_ID_JOYPAD_* id (-1 = unmapped),
    // rebuilt from descriptor metadata + bindings
    std::array<int8_t, 128> bitToButton{};
    bool mappingValid = false;

    uint32_t lastButtons = 0;  // port thread only

    // input-path diagnostic (read-only "PacketsReceived" property, updated at
    // most once a second from the port thread)
    uint64_t packetsReceived = 0;
    std::chrono::steady_clock::time_point lastCounterPublish{};
};

END_NAMESPACE_DAQCUBE_MODULE
