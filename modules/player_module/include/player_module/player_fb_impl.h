#pragma once
#include <player_module/common.h>
#include <game_engine_shared/keyboard_state.h>
#include <opendaq/function_block_impl.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

BEGIN_NAMESPACE_PLAYER_MODULE

using game_engine::KeyboardState;

// One game player on a client PC: a single SDL window that replays the game
// video stream and captures the keyboard while focused (all keys released on
// focus loss). The keyboard capture FB and the video replay FB of earlier
// phases merged into this one block.
//
// - no output signals until the PlayerTag string property is set; the tag is
//   baked into the signal ids so several clients' keyboards get distinct
//   global ids on the host (colliding ids are rejected by the server)
// - keyboard: a State packet on every key transition plus a full-state
//   heartbeat (HeartbeatRateHz, 0 disables); optional time signal
// - video is opt-in: the "Video" input port exists only while EnableVideo is
//   true (default false - a capture-only player has no port to mis-wire, and
//   in-process it avoids the keyboard+video signal cycle the SDK refuses).
//   The port accepts the game device's binary video signal; the FFmpeg
//   decoder is configured entirely from the descriptor metadata, which is
//   mirrored into read-only properties; packets flow through a bounded
//   queue that skips ahead only at keyframes (VP8-safe)
// - closing the window stops rendering and capture; Restart reopens it
class PlayerFbImpl final : public FunctionBlock
{
public:
    explicit PlayerFbImpl(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId);
    ~PlayerFbImpl() override;

    static FunctionBlockTypePtr CreateType();

protected:
    void onPacketReceived(const InputPortPtr& port) override;

private:
    // stream parameters parsed from the video signal descriptor metadata
    struct StreamConfig
    {
        std::string codec;
        std::string framerate;
        int width = 0;
        int height = 0;
        uint64_t version = 0;  // bumped on every descriptor change
    };

    void initProperties();
    void applyPlayerTag(const std::string& tag);
    void applyEnableVideo(bool enable);
    void applyTimeSignalSetting();
    void emitState(const KeyboardState& state);
    void handleDescriptorChanged(const DataDescriptorPtr& descriptor);
    void playerLoop();

    InputPortConfigPtr inputPort;  // assigned only while EnableVideo is true
    SignalConfigPtr stateSignal;
    SignalConfigPtr timeSignal;
    std::string playerTag;
    std::mutex emitSync;

    // input port thread -> player thread handoff
    std::mutex queueSync;
    std::condition_variable queueSignal;
    std::deque<std::vector<uint8_t>> packetQueue;
    StreamConfig streamConfig;  // guarded by queueSync

    // SDL video (window + event pump) is single-threaded per process, and each
    // FB pumps from its own thread - only the first FB to claim this may open
    // a window; the others decode/capture-less with a warning instead of
    // corrupting SDL state
    static std::atomic<PlayerFbImpl*> sdlOwner;

    std::thread playerThread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> restartRequested{false};
    std::atomic<int> windowScale{3};
    std::atomic<int> heartbeatRateHz{10};
    std::atomic<bool> timeSignalEnabled{false};
};

END_NAMESPACE_PLAYER_MODULE
