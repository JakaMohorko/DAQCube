/*
 * Game engine tests: the shared-memory frame/input protocol, the keyboard ->
 * RetroPad mapping, and the DAQCube device end to end - Start extracts
 * the embedded core host, spawns it, and MJPEG video packets matching the
 * descriptor metadata arrive on the channel signal.
 */

#include <game_engine_shared/audio_metadata.h>
#include <game_engine_shared/core_ipc.h>
#include <game_engine_shared/video_metadata.h>

#include <daqcube_module/retropad_mapping.h>

#include <opendaq/opendaq.h>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

namespace ipc = game_engine::ipc;
using namespace daq;
using namespace std::chrono;

// --- shared memory protocol ------------------------------------------------

TEST(CoreIpc, CreateAndOpen)
{
    const std::string name = "Local\\DAQCubeTest.CreateAndOpen";

    ipc::SharedMemory server;
    std::string error;
    ASSERT_TRUE(server.create(name, error)) << error;
    ASSERT_NE(server.block(), nullptr);
    EXPECT_EQ(server.block()->magic, ipc::Magic);

    ipc::SharedMemory client;
    ASSERT_TRUE(client.open(name, error)) << error;
    ASSERT_NE(client.block(), nullptr);

    // both views alias the same physical block
    server.block()->inputs[2].store(0x55, std::memory_order_relaxed);
    EXPECT_EQ(client.block()->inputs[2].load(std::memory_order_relaxed), 0x55u);
}

TEST(CoreIpc, OpenMissingFails)
{
    ipc::SharedMemory client;
    std::string error;
    EXPECT_FALSE(client.open("Local\\DAQCubeTest.DoesNotExist", error));
    EXPECT_FALSE(error.empty());
}

TEST(CoreIpc, FrameRingRoundtrip)
{
    const std::string name = "Local\\DAQCubeTest.FrameRing";

    ipc::SharedMemory writer;
    ipc::SharedMemory reader;
    std::string error;
    ASSERT_TRUE(writer.create(name, error)) << error;
    ASSERT_TRUE(reader.open(name, error)) << error;

    uint64_t lastSequence = 0;
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;

    // nothing published yet
    EXPECT_FALSE(reader.readLatestFrame(lastSequence, pixels, width, height));

    std::vector<uint8_t> frame(320 * 200 * 3);
    std::iota(frame.begin(), frame.end(), uint8_t{0});
    writer.publishFrame(frame.data(), 320, 200);

    ASSERT_TRUE(reader.readLatestFrame(lastSequence, pixels, width, height));
    EXPECT_EQ(width, 320u);
    EXPECT_EQ(height, 200u);
    ASSERT_EQ(pixels.size(), frame.size());
    EXPECT_EQ(std::memcmp(pixels.data(), frame.data(), frame.size()), 0);

    // no re-read of the same frame
    EXPECT_FALSE(reader.readLatestFrame(lastSequence, pixels, width, height));

    // newer frames win; the ring survives many publishes
    for (int i = 0; i < 10; i++)
    {
        frame.assign(frame.size(), static_cast<uint8_t>(i));
        writer.publishFrame(frame.data(), 320, 200);
    }
    ASSERT_TRUE(reader.readLatestFrame(lastSequence, pixels, width, height));
    EXPECT_EQ(pixels[0], 9);
    EXPECT_FALSE(reader.readLatestFrame(lastSequence, pixels, width, height));
}

// --- keyboard -> RetroPad key bindings ----------------------------------------

TEST(KeyBindings, DefaultsCoverTheGame)
{
    using namespace daq::modules::daqcube_module;

    bool foundUp = false;
    bool foundB = false;
    bool foundStart = false;
    for (const auto& binding : KeyBindings)
    {
        const auto keys = parseKeyList(binding.defaultKeys);
        EXPECT_FALSE(keys.empty()) << binding.propertyName;
        if (binding.button == RETRO_DEVICE_ID_JOYPAD_UP)
        {
            foundUp = true;
            EXPECT_EQ(keys, (std::vector<std::string>{"W", "Up"}));
        }
        if (binding.button == RETRO_DEVICE_ID_JOYPAD_B)
        {
            foundB = true;
            EXPECT_EQ(keys, (std::vector<std::string>{"Space"}));
        }
        if (binding.button == RETRO_DEVICE_ID_JOYPAD_START)
        {
            foundStart = true;
            EXPECT_EQ(keys, (std::vector<std::string>{"Return"}));
        }
    }
    EXPECT_TRUE(foundUp && foundB && foundStart);
}

TEST(KeyBindings, ParseKeyList)
{
    using daq::modules::daqcube_module::parseKeyList;

    EXPECT_EQ(parseKeyList("W,Up"), (std::vector<std::string>{"W", "Up"}));
    EXPECT_EQ(parseKeyList(" Kp4 ,  Left "), (std::vector<std::string>{"Kp4", "Left"}));
    EXPECT_EQ(parseKeyList("Space"), (std::vector<std::string>{"Space"}));
    EXPECT_TRUE(parseKeyList("").empty());
    EXPECT_TRUE(parseKeyList(" , ,").empty());
}

// --- device end to end -------------------------------------------------------

namespace
{
    InstancePtr makeInstance()
    {
        return InstanceBuilder().addModulePath(GAME_MODULES_DIR).build();
    }

    // drains encoded video packets off the reader until minCount frames
    // arrived (or the deadline passed - callers assert on the count)
    std::vector<std::vector<uint8_t>> collectFrames(const PacketReaderPtr& reader, size_t minCount)
    {
        std::vector<std::vector<uint8_t>> frames;
        const auto deadline = steady_clock::now() + seconds(10);
        while (frames.size() < minCount && steady_clock::now() < deadline)
        {
            for (const auto& packet : reader.readAll())
            {
                if (packet.getType() != PacketType::Data)
                    continue;
                DataPacketPtr dataPacket = packet;
                const auto* data = static_cast<const uint8_t*>(dataPacket.getRawData());
                frames.emplace_back(data, data + dataPacket.getRawDataSize());
            }
            std::this_thread::sleep_for(milliseconds(5));
        }
        return frames;
    }

    // VP8 packets reference earlier frames - decode the stream in order,
    // check every decoded frame's geometry and return the decoded count
    int decodeVp8CountFrames(const std::vector<std::vector<uint8_t>>& frames, int width, int height)
    {
        const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_VP8);
        if (!decoder)
            return -1;
        AVCodecContext* ctx = avcodec_alloc_context3(decoder);
        if (avcodec_open2(ctx, decoder, nullptr) != 0)
        {
            avcodec_free_context(&ctx);
            return -1;
        }
        AVFrame* frame = av_frame_alloc();
        int framesDecoded = 0;
        for (const auto& encoded : frames)
        {
            AVPacket* packet = av_packet_alloc();
            if (av_new_packet(packet, static_cast<int>(encoded.size())) == 0)
            {
                std::memcpy(packet->data, encoded.data(), encoded.size());
                if (avcodec_send_packet(ctx, packet) == 0)
                {
                    while (avcodec_receive_frame(ctx, frame) == 0)
                    {
                        framesDecoded++;
                        EXPECT_EQ(frame->width, width);
                        EXPECT_EQ(frame->height, height);
                    }
                }
            }
            av_packet_free(&packet);
        }
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return framesDecoded;
    }
}

TEST(GameDevice, ComponentsAndProperties)
{
    auto instance = makeInstance();
    auto device = instance.addDevice("daqcube://localhost");
    ASSERT_TRUE(device.assigned());

    const std::string state = device.getPropertyValue("SessionState");
    EXPECT_EQ(state, "Stopped");

    // the controls live under the (permission-protectable) Settings object
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    EXPECT_FALSE(static_cast<Bool>(settings.getPropertyValue("Headless")));
    EXPECT_EQ(static_cast<Int>(settings.getPropertyValue("JpegQuality")), 80);
    EXPECT_FALSE(device.hasProperty("Headless"));

    // the serial number identifies the host PC by MAC address
    const std::string serial = device.getInfo().getSerialNumber();
    EXPECT_EQ(serial.size(), 17u) << serial;  // XX-XX-XX-XX-XX-XX

    // four fixed player channels: keyboard in, that player's video out; all
    // available for Mr.Boom, with per-player key bindings
    const auto channels = device.getChannels();
    ASSERT_EQ(channels.getCount(), 4u);
    for (size_t i = 0; i < 4; i++)
    {
        const auto channel = channels[i];
        EXPECT_EQ(channel.getName(), "Player" + std::to_string(i + 1));
        EXPECT_TRUE(static_cast<Bool>(channel.getPropertyValue("Available")));
        EXPECT_EQ(channel.getInputPorts().getCount(), 1u);
        // the (not yet configured) per-player video + audio signals
        const auto signals = channel.getSignals();
        ASSERT_EQ(signals.getCount(), 2u);
        EXPECT_EQ(signals[0].getName(), "Video");
        EXPECT_EQ(signals[1].getName(), "Audio");
        EXPECT_EQ(signals[0].getDescriptor().getSampleType(), SampleType::Binary);
        EXPECT_EQ(signals[1].getDescriptor().getSampleType(), SampleType::Binary);
        const std::string keyUp = channel.getPropertyValue("KeyUp");
        EXPECT_EQ(keyUp, "W,Up");
        channel.setPropertyValue("KeyB", "RightCtrl");  // bindings are per player
        const std::string keyB = channel.getPropertyValue("KeyB");
        EXPECT_EQ(keyB, "RightCtrl");
    }

    // the player channels are the device's only function blocks
    EXPECT_EQ(device.getFunctionBlocks().getCount(), 0u);
}

TEST(GameDevice, StartStreamsMjpegAndStops)
{
    auto instance = makeInstance();
    auto device = instance.addDevice("daqcube://localhost");
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);
    settings.setPropertyValue("VideoCodec", 1);  // MJPEG (the intra-only fallback)

    const auto videoSignal = device.getChannels()[0].getSignals()[0];
    auto reader = PacketReader(videoSignal);

    ProcedurePtr start = settings.getPropertyValue("Start");
    start();

    const std::string state = device.getPropertyValue("SessionState");
    ASSERT_EQ(state, "Running");

    // descriptor follows the shared video metadata convention
    const auto meta = videoSignal.getDescriptor().getMetadata();
    const std::string codec = meta.get(game_engine::video_meta::CodecId);
    const std::string widthStr = meta.get(game_engine::video_meta::Width);
    const std::string heightStr = meta.get(game_engine::video_meta::Height);
    EXPECT_EQ(codec, "mjpeg");
    const int width = std::stoi(widthStr);
    const int height = std::stoi(heightStr);
    EXPECT_GT(width, 0);
    EXPECT_GT(height, 0);

    // collect encoded frames (Mr.Boom runs at 60 fps; expect a healthy stream)
    const auto frames = collectFrames(reader, 30);
    ASSERT_GE(frames.size(), 30u) << "video packets did not arrive";

    // every MJPEG packet is a standalone JPEG; decode one and check geometry
    const auto& jpeg = frames.back();
    ASSERT_GE(jpeg.size(), 4u);
    EXPECT_EQ(jpeg[0], 0xFF);  // JPEG SOI marker
    EXPECT_EQ(jpeg[1], 0xD8);

    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* ctx = avcodec_alloc_context3(decoder);
    ASSERT_EQ(avcodec_open2(ctx, decoder, nullptr), 0);
    AVPacket* packet = av_packet_alloc();
    ASSERT_EQ(av_new_packet(packet, static_cast<int>(jpeg.size())), 0);
    std::memcpy(packet->data, jpeg.data(), jpeg.size());
    AVFrame* frame = av_frame_alloc();
    ASSERT_EQ(avcodec_send_packet(ctx, packet), 0);
    ASSERT_EQ(avcodec_receive_frame(ctx, frame), 0);
    EXPECT_EQ(frame->width, width);
    EXPECT_EQ(frame->height, height);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);

    ProcedurePtr stop = settings.getPropertyValue("Stop");
    stop();
    const std::string stoppedState = device.getPropertyValue("SessionState");
    EXPECT_EQ(stoppedState, "Stopped");

    // stopping again is a no-op
    stop();
}

TEST(GameDevice, StartStreamsVp8ByDefault)
{
    auto instance = makeInstance();
    auto device = instance.addDevice("daqcube://localhost");
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);

    const auto videoSignal = device.getChannels()[0].getSignals()[0];
    auto reader = PacketReader(videoSignal);

    ProcedurePtr start = settings.getPropertyValue("Start");
    start();

    // defaults: VP8 at a 2x upscale of the core's fixed 320x200
    const auto meta = videoSignal.getDescriptor().getMetadata();
    const std::string codec = meta.get(game_engine::video_meta::CodecId);
    EXPECT_EQ(codec, "vp8");
    const int width = std::stoi(std::string(meta.get(game_engine::video_meta::Width)));
    const int height = std::stoi(std::string(meta.get(game_engine::video_meta::Height)));
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 400);

    const auto frames = collectFrames(reader, 30);
    ASSERT_GE(frames.size(), 30u) << "video packets did not arrive";

    // the reader was attached before Start, so the first packet is the stream
    // start and must be a keyframe
    ASSERT_FALSE(frames.front().empty());
    EXPECT_TRUE(game_engine::video_meta::isVp8Keyframe(frames.front().data(), frames.front().size()));

    EXPECT_GE(decodeVp8CountFrames(frames, width, height), 30);

    ProcedurePtr stop = settings.getPropertyValue("Stop");
    stop();
}

TEST(GameDevice, SnesModeSeatsTwoPlayersAndNeedsARom)
{
    auto instance = makeInstance();
    auto device = instance.addDevice("daqcube://localhost");
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);
    settings.setPropertyValue("Game", 1);  // SNES

    // OutputWidth/Height default to 0 = "the selected game's recommended
    // size", so switching games never clobbers a user-set resolution
    EXPECT_EQ(static_cast<Int>(settings.getPropertyValue("OutputWidth")), 0);
    EXPECT_EQ(static_cast<Int>(settings.getPropertyValue("OutputHeight")), 0);
    settings.setPropertyValue("OutputWidth", 800);

    const auto channels = device.getChannels();
    ASSERT_EQ(channels.getCount(), 4u);
    for (size_t i = 0; i < 4; i++)
        EXPECT_EQ(static_cast<Bool>(channels[i].getPropertyValue("Available")), i < 2 ? True : False) << i;

    // no ROM configured - Start must refuse with a clear error, not spawn a dead host
    ProcedurePtr start = settings.getPropertyValue("Start");
    EXPECT_THROW(start(), InvalidParameterException);
    EXPECT_EQ(static_cast<std::string>(device.getPropertyValue("SessionState")), "Stopped");

    settings.setPropertyValue("RomPath", "Z:\\does\\not\\exist.sfc");
    EXPECT_THROW(start(), InvalidParameterException);

    // back to Mr.Boom: all four seats return, the explicit resolution survives
    settings.setPropertyValue("Game", 0);
    for (size_t i = 0; i < 4; i++)
        EXPECT_TRUE(static_cast<Bool>(channels[i].getPropertyValue("Available"))) << i;
    EXPECT_EQ(static_cast<Int>(settings.getPropertyValue("OutputWidth")), 800);
    EXPECT_EQ(static_cast<Int>(settings.getPropertyValue("OutputHeight")), 0);
}

// full SNES loop needs a ROM, which is not shipped with the repo - point
// DAQGAME_SNES_ROM at any .sfc file (e.g. a homebrew ROM) to run this one
TEST(GameDevice, SnesStreamsWithProvidedRom)
{
    const char* rom = std::getenv("DAQGAME_SNES_ROM");
    if (!rom || !*rom)
        GTEST_SKIP() << "set DAQGAME_SNES_ROM to a .sfc file to run the SNES end-to-end test";

    auto instance = makeInstance();
    auto device = instance.addDevice("daqcube://localhost");
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);
    settings.setPropertyValue("Game", 1);
    settings.setPropertyValue("RomPath", rom);

    const auto videoSignal = device.getChannels()[0].getSignals()[0];
    auto reader = PacketReader(videoSignal);

    ProcedurePtr start = settings.getPropertyValue("Start");
    start();
    ASSERT_EQ(static_cast<std::string>(device.getPropertyValue("SessionState")), "Running");

    const auto meta = videoSignal.getDescriptor().getMetadata();
    EXPECT_EQ(static_cast<std::string>(meta.get(game_engine::video_meta::CodecId)), "vp8");
    const int width = std::stoi(std::string(meta.get(game_engine::video_meta::Width)));
    const int height = std::stoi(std::string(meta.get(game_engine::video_meta::Height)));
    EXPECT_EQ(width, 512);
    EXPECT_EQ(height, 448);

    const auto frames = collectFrames(reader, 30);
    ASSERT_GE(frames.size(), 30u) << "SNES video packets did not arrive";
    EXPECT_GE(decodeVp8CountFrames(frames, width, height), 30);

    ProcedurePtr stop = settings.getPropertyValue("Stop");
    stop();
}

TEST(GameDevice, AudioStreamsAlongVideo)
{
    auto instance = makeInstance();
    auto device = instance.addDevice("daqcube://localhost");
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);

    const auto audioSignal = device.getChannels()[0].getSignals()[1];
    auto reader = PacketReader(audioSignal);

    ProcedurePtr start = settings.getPropertyValue("Start");
    start();

    // descriptor follows the audio metadata convention (raw interleaved PCM)
    const auto meta = audioSignal.getDescriptor().getMetadata();
    EXPECT_EQ(static_cast<std::string>(meta.get(game_engine::audio_meta::SampleFormat)), "s16");
    EXPECT_EQ(static_cast<std::string>(meta.get(game_engine::audio_meta::Channels)), "2");
    const int sampleRate = std::stoi(std::string(meta.get(game_engine::audio_meta::SampleRate)));
    EXPECT_GT(sampleRate, 0);

    // Mr.Boom produces audio continuously - expect a healthy PCM stream
    size_t bytes = 0;
    const auto deadline = steady_clock::now() + seconds(10);
    while (bytes < static_cast<size_t>(sampleRate) * 4 / 2 && steady_clock::now() < deadline)
    {
        for (const auto& packet : reader.readAll())
        {
            if (packet.getType() != PacketType::Data)
                continue;
            const DataPacketPtr dataPacket = packet;
            bytes += dataPacket.getRawDataSize();
            // whole interleaved stereo s16 frames only
            EXPECT_EQ(dataPacket.getRawDataSize() % 4, 0u);
        }
        std::this_thread::sleep_for(milliseconds(5));
    }
    EXPECT_GE(bytes, static_cast<size_t>(sampleRate) * 4 / 2) << "less than 0.5s of audio arrived";

    ProcedurePtr stop = settings.getPropertyValue("Stop");
    stop();
}

TEST(GameDevice, DoomStreamsOutOfTheBox)
{
    auto instance = makeInstance();
    auto device = instance.addDevice("daqcube://localhost");
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);
    settings.setPropertyValue("Game", 2);  // Doom

    // one seat; the other channels spectate (they still carry the video feed)
    const auto channels = device.getChannels();
    ASSERT_EQ(channels.getCount(), 4u);
    for (size_t i = 0; i < 4; i++)
        EXPECT_EQ(static_cast<Bool>(channels[i].getPropertyValue("Available")), i < 1 ? True : False) << i;

    const auto videoSignal = channels[0].getSignals()[0];
    auto reader = PacketReader(videoSignal);

    // no RomPath needed - the embedded Freedoom IWAD is the default content
    ProcedurePtr start = settings.getPropertyValue("Start");
    start();
    ASSERT_EQ(static_cast<std::string>(device.getPropertyValue("SessionState")), "Running");

    const auto meta = videoSignal.getDescriptor().getMetadata();
    EXPECT_EQ(static_cast<std::string>(meta.get(game_engine::video_meta::CodecId)), "vp8");
    const int width = std::stoi(std::string(meta.get(game_engine::video_meta::Width)));
    const int height = std::stoi(std::string(meta.get(game_engine::video_meta::Height)));
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 400);

    const auto frames = collectFrames(reader, 30);
    ASSERT_GE(frames.size(), 30u) << "Doom video packets did not arrive";
    EXPECT_GE(decodeVp8CountFrames(frames, width, height), 30);

    // spectator channels got the same stream descriptor
    const auto spectatorMeta = channels[3].getSignals()[0].getDescriptor().getMetadata();
    EXPECT_EQ(static_cast<std::string>(spectatorMeta.get(game_engine::video_meta::CodecId)), "vp8");

    ProcedurePtr stop = settings.getPropertyValue("Stop");
    stop();
}

TEST(GameDevice, StartTwiceIsIgnored)
{
    auto instance = makeInstance();
    auto device = instance.addDevice("daqcube://localhost");
    const PropertyObjectPtr settings = device.getPropertyValue("Settings");
    settings.setPropertyValue("Headless", true);

    ProcedurePtr start = settings.getPropertyValue("Start");
    start();
    start();  // logs a warning, must not spawn a second host or throw

    const std::string state = device.getPropertyValue("SessionState");
    EXPECT_EQ(state, "Running");

    ProcedurePtr stop = settings.getPropertyValue("Stop");
    stop();
}
