#pragma once
#include <daqcube_module/common.h>
#include <daqcube_module/game_session.h>
#include <daqcube_module/player_channel_impl.h>
#include <daqcube_module/resource_extractor.h>

#include <opendaq/device_impl.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

BEGIN_NAMESPACE_DAQCUBE_MODULE

// Game host root device. Owns four fixed player channels (keyboard state in,
// that player's encoded video out - the same packet goes to every channel
// while all players share one feed; `Available` driven by the selected game)
// and the game session: a core_host.exe child process running the libretro
// core, controlled over shared memory. `Start`/`Stop` procedure properties
// drive the session; a watchdog flips `SessionState` if the core host crashes
// or its window is closed.
class DaqCubeDeviceImpl final : public Device
{
public:
    explicit DaqCubeDeviceImpl(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId);
    ~DaqCubeDeviceImpl() override;

    static DeviceTypePtr CreateType();
    static DeviceInfoPtr CreateDeviceInfo();

private:
    void initProperties();
    void initComponents();
    void initPermissions();

    // flips the channels' read-only "Available" flag: the first `playerCount`
    // seats are open, the rest closed (Mr.Boom seats 4, SNES 2)
    void updatePlayerAvailability(unsigned playerCount);

    void startGame();
    void stopGame();

    // input port threads (player channels)
    void onPlayerButtons(unsigned playerIndex, uint32_t buttons);
    // session thread; must not touch properties directly (a Stop procedure
    // holding the property lock joins that thread - it defers via scheduler)
    void onSessionEnded(bool crashed, const std::string& message);

    void sendVideoPacket(const uint8_t* data, size_t size);
    void sendAudioPacket(const uint8_t* pcm, size_t bytes);
    DataDescriptorPtr buildVideoDescriptor(const GameSession::Config& config) const;
    DataDescriptorPtr buildAudioDescriptor() const;
    void setSessionState(const StringPtr& state);

    // for deferred property writes from the session thread: a weak ref cannot
    // resurrect the device if the work item outlives it
    WeakRefPtr<IPropertyObject> thisWeak;

    std::mutex sessionSync;  // guards session lifetime (start/stop/input forwarding)
    std::unique_ptr<GameSession> session;

    PropertyObjectPtr settings;  // nested "Settings" object (protectable per-user)

    DataDescriptorPtr videoDescriptor;
    DataDescriptorPtr audioDescriptor;

    std::array<PlayerChannelImpl*, game_engine::ipc::MaxPlayers> playerImpls{};
    std::array<std::atomic<uint32_t>, game_engine::ipc::MaxPlayers> lastButtons{};
};

END_NAMESPACE_DAQCUBE_MODULE
