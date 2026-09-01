/*
 * Player FB tests: keyboard state bitmap logic, descriptor layout and
 * metadata, the PlayerTag signal gate, heartbeat packet flow, and the video
 * replay half (stream properties from descriptor metadata, live decode).
 */

#include <gtest/gtest.h>

#include <opendaq/opendaq.h>

#include <player_module/keyboard_layout.h>
#include <game_engine_shared/keyboard_state.h>
#include <game_engine_shared/video_metadata.h>

#include <chrono>
#include <string>
#include <thread>

using namespace daq;
using namespace std::chrono;
using daq::modules::player_module::AnsiKeyCount;
using daq::modules::player_module::AnsiKeys;
using game_engine::KeyboardState;
using daq::modules::player_module::scancodeToBit;

TEST(KeyboardState, SetTestClear)
{
    KeyboardState state;
    EXPECT_EQ(state.pressedCount(), 0u);

    state.set(0, true);    // Escape
    state.set(63, true);   // word boundary
    state.set(64, true);
    state.set(103, true);  // last key
    EXPECT_TRUE(state.test(0));
    EXPECT_TRUE(state.test(63));
    EXPECT_TRUE(state.test(64));
    EXPECT_TRUE(state.test(103));
    EXPECT_EQ(state.pressedCount(), 4u);

    state.set(63, false);
    EXPECT_FALSE(state.test(63));
    EXPECT_EQ(state.pressedCount(), 3u);

    // out-of-range bits are ignored
    state.set(200, true);
    EXPECT_FALSE(state.test(200));
    EXPECT_EQ(state.pressedCount(), 3u);
}

TEST(KeyboardState, ChangeDetection)
{
    KeyboardState a;
    KeyboardState b;
    EXPECT_EQ(a, b);
    b.set(42, true);
    EXPECT_NE(a, b);
    b.set(42, false);
    EXPECT_EQ(a, b);
}

TEST(KeyboardState, ScancodeMapping)
{
    // every ANSI key maps to its own bit, in list order
    for (unsigned bit = 0; bit < AnsiKeyCount; bit++)
        EXPECT_EQ(scancodeToBit(AnsiKeys[bit].scancode), static_cast<int>(bit));

    // keys outside the ANSI layout are rejected
    EXPECT_EQ(scancodeToBit(SDL_SCANCODE_UNKNOWN), -1);
    EXPECT_EQ(scancodeToBit(SDL_SCANCODE_MUTE), -1);
}

static InstancePtr createTestInstance()
{
    return InstanceBuilder().addModulePath(GAME_MODULES_DIR).build();
}

static SignalPtr findSignal(const FunctionBlockPtr& fb, const std::string& name)
{
    for (const auto& signal : fb.getSignals(search::Any()))
        if (signal.getName() == name)
            return signal;
    return nullptr;
}

TEST(PlayerFb, NoSignalsUntilPlayerTagSet)
{
    auto instance = createTestInstance();
    auto fb = instance.addFunctionBlock("Player");

    // the tag identifies this keyboard's signals globally; nothing exists before it is set
    EXPECT_EQ(fb.getPropertyValue("PlayerTag"), "");
    EXPECT_EQ(fb.getSignals(search::Any()).getCount(), 0u);

    EXPECT_ANY_THROW(fb.setPropertyValue("PlayerTag", "no spaces"));
    EXPECT_EQ(fb.getSignals(search::Any()).getCount(), 0u);

    fb.setPropertyValue("PlayerTag", "player1");
    auto state = findSignal(fb, "State");
    ASSERT_TRUE(state.assigned());
    // the tag lands in the global id (that is its whole point), the name stays "State"
    EXPECT_NE(std::string(state.getGlobalId()).find("State_player1"), std::string::npos);

    // one-shot: re-setting the same value is a no-op, changing it is refused
    EXPECT_NO_THROW(fb.setPropertyValue("PlayerTag", "player1"));
    EXPECT_ANY_THROW(fb.setPropertyValue("PlayerTag", "player2"));
    EXPECT_EQ(fb.getPropertyValue("PlayerTag"), "player1");
}

TEST(PlayerFb, DescriptorLayoutAndMetadata)
{
    auto instance = createTestInstance();
    auto fb = instance.addFunctionBlock("Player");
    fb.setPropertyValue("PlayerTag", "test");

    auto state = findSignal(fb, "State");
    ASSERT_TRUE(state.assigned());

    auto descriptor = state.getDescriptor();
    ASSERT_TRUE(descriptor.assigned());
    EXPECT_EQ(descriptor.getSampleType(), SampleType::Struct);
    EXPECT_EQ(descriptor.getStructFields().getCount(), 2u);
    EXPECT_EQ(descriptor.getSampleSize(), KeyboardState::ByteSize);

    auto meta = descriptor.getMetadata();
    ASSERT_TRUE(meta.assigned());
    EXPECT_EQ(meta.get("Layout"), "ANSI_US");
    EXPECT_EQ(meta.get("Bit.Escape"), "0");
    EXPECT_EQ(meta.get("Bit.W"), std::to_string(scancodeToBit(SDL_SCANCODE_W)));
    EXPECT_EQ(meta.get("Bit.KpPeriod"), "103");
}

TEST(PlayerFb, TimeSignalOptionalDefaultOff)
{
    auto instance = createTestInstance();
    auto fb = instance.addFunctionBlock("Player");
    fb.setPropertyValue("PlayerTag", "test");

    auto state = findSignal(fb, "State");
    ASSERT_TRUE(state.assigned());

    // ordering guarantees make time data overhead - off by default
    EXPECT_EQ(fb.getPropertyValue("EnableTimeSignal"), False);
    EXPECT_FALSE(state.getDomainSignal().assigned());

    fb.setPropertyValue("EnableTimeSignal", True);
    EXPECT_TRUE(state.getDomainSignal().assigned());
    EXPECT_EQ(state.getDomainSignal().getName(), "Time");

    fb.setPropertyValue("EnableTimeSignal", False);
    EXPECT_FALSE(state.getDomainSignal().assigned());
}

static unsigned countDataPackets(const PacketReaderPtr& reader)
{
    unsigned count = 0;
    for (const auto& packet : reader.readAll())
        if (packet.getType() == PacketType::Data)
            count++;
    return count;
}

TEST(PlayerFb, HeartbeatDeliversPackets)
{
    auto instance = createTestInstance();
    auto fb = instance.addFunctionBlock("Player");
    fb.setPropertyValue("PlayerTag", "test");
    fb.setPropertyValue("HeartbeatRateHz", 50);

    auto state = findSignal(fb, "State");
    ASSERT_TRUE(state.assigned());

    // 50 Hz: expect a healthy number of heartbeats even without focus. Poll
    // instead of a single fixed sleep - under full-suite load the player
    // thread (SDL window creation included) can start slowly
    auto reader = PacketReader(state);
    size_t count = 0;
    const auto deadline = steady_clock::now() + seconds(5);
    while (count < 10 && steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(milliseconds(50));
        count += countDataPackets(reader);
    }
    EXPECT_GE(count, 10u);
}

TEST(PlayerFb, HeartbeatZeroDisablesIdleTraffic)
{
    auto instance = createTestInstance();
    auto fb = instance.addFunctionBlock("Player");
    fb.setPropertyValue("PlayerTag", "test");
    fb.setPropertyValue("HeartbeatRateHz", 0);

    auto state = findSignal(fb, "State");
    ASSERT_TRUE(state.assigned());

    auto reader = PacketReader(state);
    std::this_thread::sleep_for(milliseconds(300));

    // without focus and without heartbeat, the wire stays quiet
    // (at most the initial all-released state may still be in flight)
    EXPECT_LE(countDataPackets(reader), 1u);
}

TEST(PlayerFb, VideoProperties)
{
    auto instance = createTestInstance();
    auto fb = instance.addFunctionBlock("Player");

    // stream metadata mirrors are read-only diagnostics
    EXPECT_EQ(static_cast<Int>(fb.getPropertyValue("WindowScale")), 3);
    const std::string codec = fb.getPropertyValue("Codec");
    EXPECT_EQ(codec, "");
    EXPECT_EQ(static_cast<Int>(fb.getPropertyValue("StreamWidth")), 0);
    EXPECT_EQ(static_cast<Int>(fb.getPropertyValue("StreamHeight")), 0);
    EXPECT_EQ(static_cast<Int>(fb.getPropertyValue("FramesDecoded")), 0);

    EXPECT_ANY_THROW(fb.setPropertyValue("Codec", "h264"));
    EXPECT_ANY_THROW(fb.setPropertyValue("FramesDecoded", 42));

    // Restart is callable even with nothing connected
    ProcedurePtr restart = fb.getPropertyValue("Restart");
    ASSERT_TRUE(restart.assigned());
    restart();

    // video is opt-in: no input port until EnableVideo is set, and disabling
    // removes it again (capture-only is the default)
    EXPECT_FALSE(static_cast<Bool>(fb.getPropertyValue("EnableVideo")));
    EXPECT_EQ(fb.getInputPorts().getCount(), 0u);
    fb.setPropertyValue("EnableVideo", true);
    ASSERT_EQ(fb.getInputPorts().getCount(), 1u);
    EXPECT_EQ(fb.getInputPorts()[0].getName(), "Video");
    fb.setPropertyValue("EnableVideo", false);
    EXPECT_EQ(fb.getInputPorts().getCount(), 0u);
}

TEST(AudioOutput, PlaysTheGameAudio)
{
    auto instance = createTestInstance();

    // the AudioOutput device type is always advertised; actual playback
    // devices depend on the machine
    bool hasType = false;
    for (const auto& [id, type] : instance.getAvailableDeviceTypes())
        if (id == "AudioOutput")
            hasType = true;
    EXPECT_TRUE(hasType);

    daq::DevicePtr audioOut;
    try
    {
        audioOut = instance.addDevice("daqaudioout://default");
    }
    catch (const std::exception&)
    {
        GTEST_SKIP() << "no audio backend on this machine";
    }

    const auto channels = audioOut.getChannels();
    ASSERT_EQ(channels.getCount(), 1u);
    const auto output = channels[0];
    EXPECT_EQ(output.getName(), "Output");
    EXPECT_EQ(output.getInputPorts().getCount(), 1u);
    EXPECT_EQ(static_cast<daq::Int>(output.getPropertyValue("SampleRate")), 0);
    output.setPropertyValue("Volume", 0.5);

    // feed it the real game audio and expect the frame counter to move
    auto device = instance.addDevice("daqcube://localhost");
    const daq::PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);
    output.getInputPorts()[0].connect(device.getChannels()[0].getSignals()[1]);

    daq::ProcedurePtr start = settings.getPropertyValue("Start");
    start();

    daq::Int framesReceived = 0;
    const auto deadline = steady_clock::now() + seconds(10);
    while (framesReceived == 0 && steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(milliseconds(100));
        framesReceived = output.getPropertyValue("FramesReceived");
    }
    EXPECT_GT(framesReceived, 0) << "no audio frames reached the AudioOutput channel";
    EXPECT_GT(static_cast<daq::Int>(output.getPropertyValue("SampleRate")), 0);
    EXPECT_EQ(static_cast<daq::Int>(output.getPropertyValue("Channels")), 2);

    daq::ProcedurePtr stop = settings.getPropertyValue("Stop");
    stop();
}

TEST(PlayerFb, DecodesTheGameStream)
{
    auto instance = createTestInstance();

    auto device = instance.addDevice("daqcube://localhost");
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);

    auto fb = instance.addFunctionBlock("Player");
    // each player channel carries its own copy of the video feed; the FB's
    // video port appears once EnableVideo is set
    fb.setPropertyValue("EnableVideo", true);
    const auto channels = device.getChannels();
    ASSERT_GE(channels.getCount(), 1u);
    const auto videoSignal = channels[0].getSignals()[0];
    fb.getInputPorts()[0].connect(videoSignal);

    ProcedurePtr start = settings.getPropertyValue("Start");
    start();

    // stream properties mirror the descriptor once the descriptor event lands,
    // and the decode counter starts moving (updated about once a second)
    Int framesDecoded = 0;
    const auto deadline = steady_clock::now() + seconds(15);
    while (framesDecoded == 0 && steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(milliseconds(100));
        framesDecoded = fb.getPropertyValue("FramesDecoded");
    }
    EXPECT_GT(framesDecoded, 0) << "no frames decoded";

    // the device default is VP8; the player FB picked it up from the descriptor
    const std::string codec = fb.getPropertyValue("Codec");
    EXPECT_EQ(codec, "vp8");
    const std::string framerate = fb.getPropertyValue("Framerate");
    EXPECT_FALSE(framerate.empty());

    const auto meta = videoSignal.getDescriptor().getMetadata();
    const std::string width = meta.get(game_engine::video_meta::Width);
    const std::string height = meta.get(game_engine::video_meta::Height);
    EXPECT_EQ(static_cast<Int>(fb.getPropertyValue("StreamWidth")), std::stoll(width));
    EXPECT_EQ(static_cast<Int>(fb.getPropertyValue("StreamHeight")), std::stoll(height));

    ProcedurePtr stop = settings.getPropertyValue("Stop");
    stop();
}
