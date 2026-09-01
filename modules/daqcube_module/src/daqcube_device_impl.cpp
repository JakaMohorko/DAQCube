#include <daqcube_module/daqcube_device_impl.h>
#include <daqcube_module/embedded_resources.h>

#include <coreobjects/callable_info_factory.h>
#include <coreobjects/coercer_factory.h>
#include <coreobjects/permission_mask_builder_factory.h>
#include <coreobjects/permissions_builder_factory.h>
#include <coreobjects/property_factory.h>
#include <coreobjects/property_object_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <coretypes/procedure_factory.h>
#include <opendaq/binary_data_packet_factory.h>
#include <opendaq/custom_log.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/device_info_factory.h>
#include <opendaq/device_type_factory.h>
#include <opendaq/work_factory.h>

#include <game_engine_shared/audio_metadata.h>
#include <game_engine_shared/video_metadata.h>

extern "C"
{
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

BEGIN_NAMESPACE_DAQCUBE_MODULE

namespace audio_meta = game_engine::audio_meta;
namespace video_meta = game_engine::video_meta;

namespace
{
    // One row per Settings.Game selection entry - the single source of truth
    // for per-game facts (the minimal version of PLAN.md's game registry).
    struct GameInfo
    {
        const char* name;         // Settings.Game selection entry
        unsigned players;         // seats (channels flagged Available); the
                                  // other channels spectate the video feed
        unsigned recommendedWidth;   // encode size used while OutputWidth/Height
        unsigned recommendedHeight;  // are 0 (a 2x upscale of the native geometry)
        bool needsContent;        // requires Settings.RomPath
        int coreResourceId;       // embedded core dll (embedded_resources.h)
        const char* coreFileName;
        // optional embedded companions (0 = none): a support file the core
        // expects in its system directory, and default content used while
        // Settings.RomPath is empty (making needsContent moot)
        int supportResourceId;
        const char* supportFileName;
        int defaultContentResourceId;
        const char* defaultContentFileName;
    };

    constexpr GameInfo Games[] = {
        {"MrBoom", 4, 640, 400, false, DAQGAME_RES_CORE_MRBOOM, "mrboom_libretro.dll", 0, nullptr, 0, nullptr},
        {"SNES", 2, 512, 448, true, DAQGAME_RES_CORE_SNES9X, "snes9x_libretro.dll", 0, nullptr, 0, nullptr},
        // prboom needs its resource wad next to the core (system dir); the
        // Freedoom IWAD ships embedded so Doom starts with no RomPath at all
        {"Doom", 1, 640, 400, false, DAQGAME_RES_CORE_PRBOOM, "prboom_libretro.dll",
         DAQGAME_RES_WAD_PRBOOM, "prboom.wad", DAQGAME_RES_WAD_FREEDOOM, "freedoom1.wad"},
    };

    const GameInfo& gameInfo(Int selectionIndex)
    {
        const auto index = static_cast<size_t>(selectionIndex);
        return Games[index < std::size(Games) ? index : 0];
    }

    // MAC address of the first operational physical adapter - identifies the
    // host PC across sessions, unlike a made-up serial
    std::string primaryMacAddress()
    {
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, adapters, &size) != NO_ERROR)
            return {};

        const IP_ADAPTER_ADDRESSES* fallback = nullptr;
        for (const auto* adapter = adapters; adapter; adapter = adapter->Next)
        {
            if (adapter->PhysicalAddressLength != 6 || adapter->OperStatus != IfOperStatusUp)
                continue;
            if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211)
            {
                fallback = adapter;
                break;
            }
            if (!fallback)
                fallback = adapter;
        }
        if (!fallback)
            return {};

        char text[18];
        const auto* mac = fallback->PhysicalAddress;
        std::snprintf(text, sizeof(text), "%02X-%02X-%02X-%02X-%02X-%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return text;
    }
}

DaqCubeDeviceImpl::DaqCubeDeviceImpl(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId)
    : Device(ctx, parent, localId)
{
    this->deviceInfo = CreateDeviceInfo();
    thisWeak = objPtr;
    initProperties();
    initComponents();
    initPermissions();
}

DaqCubeDeviceImpl::~DaqCubeDeviceImpl()
{
    std::scoped_lock lock(sessionSync);
    session.reset();  // stops the core host and joins the session thread
}

DeviceTypePtr DaqCubeDeviceImpl::CreateType()
{
    return DeviceType("DAQCube",
                      "DAQCube",
                      "Game host device driving libretro game cores",
                      "daqcube");
}

DeviceInfoPtr DaqCubeDeviceImpl::CreateDeviceInfo()
{
    auto info = DeviceInfo("daqcube://localhost");
    info.setName("DAQCube");
    info.setManufacturer("openDAQ");
    info.setModel("DAQCube");
    const auto mac = primaryMacAddress();
    info.setSerialNumber(mac.empty() ? "DAQCube_0" : mac);
    info.setDeviceType(CreateType());
    return info;
}

void DaqCubeDeviceImpl::initProperties()
{
    // everyone may read the session state; the controls live in a nested
    // "Settings" object so remote multiplayer can protect that one object
    // (only player1/admin get write+execute on it) while the rest of the
    // device stays readable
    objPtr.addProperty(StringPropertyBuilder("SessionState", "Stopped").setReadOnly(True).build());

    auto settingsTemplate = PropertyObject();
    auto gameNames = List<IString>();
    for (const auto& game : Games)
        gameNames.pushBack(game.name);
    settingsTemplate.addProperty(SelectionProperty("Game", gameNames, 0));
    // SNES mode runs the embedded snes9x core on a user-supplied ROM file
    // (a path on the host PC); Mr.Boom is contentless and ignores this
    settingsTemplate.addProperty(StringProperty("RomPath", ""));
    // Headless=true: no window on the host PC, the video signal is the only output
    settingsTemplate.addProperty(BoolProperty("Headless", False));
    // VP8 (libvpx realtime) is the default for quality per bit; MJPEG stays as
    // the intra-only fallback with zero decoder state
    settingsTemplate.addProperty(SelectionProperty("VideoCodec", List<IString>("VP8", "MJPEG"), 0));
    settingsTemplate.addProperty(IntPropertyBuilder("BitrateKbps", 4000).setMinValue(250).setMaxValue(50000).build());
    settingsTemplate.addProperty(IntPropertyBuilder("JpegQuality", 80).setMinValue(1).setMaxValue(100).build());
    // 0 = the selected game's recommended encode size (a 2x upscale of its
    // native geometry - libretro cores render at their own resolution, so
    // higher fidelity means scaling up before the lossy encode). Explicit
    // values override the recommendation and survive game switches;
    // dimensions are rounded down to even for 4:2:0.
    settingsTemplate.addProperty(IntPropertyBuilder("OutputWidth", 0)
                                     .setMinValue(0)
                                     .setMaxValue(static_cast<Int>(game_engine::ipc::MaxFrameWidth))
                                     .build());
    settingsTemplate.addProperty(IntPropertyBuilder("OutputHeight", 0)
                                     .setMinValue(0)
                                     .setMaxValue(static_cast<Int>(game_engine::ipc::MaxFrameHeight))
                                     .build());
    // encode every Nth core frame - the core's rate is fixed, this thins the
    // stream when bandwidth matters (1 = native rate)
    settingsTemplate.addProperty(IntPropertyBuilder("FrameDivisor", 1).setMinValue(1).setMaxValue(6).build());
    settingsTemplate.addProperty(FunctionPropertyBuilder("Start", ProcedureInfo()).setReadOnly(True).build());
    settingsTemplate.addProperty(FunctionPropertyBuilder("Stop", ProcedureInfo()).setReadOnly(True).build());
    objPtr.addProperty(ObjectProperty("Settings", settingsTemplate));

    // object-type property defaults are cloned - the procedures must be set
    // on the actual instance the property holds
    settings = objPtr.getPropertyValue("Settings");
    const auto protectedSettings = settings.asPtr<IPropertyObjectProtected>(true);
    protectedSettings.setProtectedPropertyValue("Start", Procedure([this] { startGame(); }));
    protectedSettings.setProtectedPropertyValue("Stop", Procedure([this] { stopGame(); }));

    // picking a game seats its player count; the encode size follows the game
    // only through the OutputWidth/Height = 0 default, resolved at Start
    settings.getOnPropertyValueWrite("Game") +=
        [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        { updatePlayerAvailability(gameInfo(args.getValue()).players); };
}

void DaqCubeDeviceImpl::initPermissions()
{
    // The access model ships with the module (hosts only define the users):
    // everyone reads the device tree (video signal included), admin has full
    // access, and only player1 (and admin) adjusts the Settings object -
    // starting/stopping the game included. The per-player slot permissions
    // live in PlayerChannelImpl. In-process use is unaffected; these apply to
    // remote (config protocol) users.
    objPtr.getPermissionManager().setPermissions(
        PermissionsBuilder()
            .inherit(false)
            .assign("everyone", PermissionMaskBuilder().read())
            .assign("admin", PermissionMaskBuilder().read().write().execute())
            .build());

    settings.getPermissionManager().setPermissions(
        PermissionsBuilder()
            .inherit(true)
            .assign("player1", PermissionMaskBuilder().read().write().execute())
            .build());
}

void DaqCubeDeviceImpl::initComponents()
{
    // one channel per player: keyboard state in, that player's video out
    for (unsigned i = 0; i < playerImpls.size(); i++)
    {
        auto channel = createAndAddChannel<PlayerChannelImpl>(
            this->ioFolder,
            StringPtr("Player" + std::to_string(i + 1)),
            i,
            [this](unsigned playerIndex, uint32_t buttons) { onPlayerButtons(playerIndex, buttons); });
        playerImpls[i] = static_cast<PlayerChannelImpl*>(channel.getObject());
    }

    updatePlayerAvailability(gameInfo(settings.getPropertyValue("Game")).players);
}

void DaqCubeDeviceImpl::updatePlayerAvailability(unsigned playerCount)
{
    for (unsigned i = 0; i < playerImpls.size(); i++)
        if (playerImpls[i])
            playerImpls[i]->setAvailable(i < playerCount);
}

void DaqCubeDeviceImpl::startGame()
{
    std::scoped_lock lock(sessionSync);

    if (session && session->running())
    {
        LOG_W("Start ignored: a game session is already running")
        return;
    }
    session.reset();

    const GameInfo& game = gameInfo(settings.getPropertyValue("Game"));

    GameSession::Config config;
    std::string error;
    // extraction skips files already on disk with the right size, so only the
    // first Start (per module version and game) pays for the writes
    std::string supportPath;  // extracted next to the core = its system dir
    if (!extractEmbeddedResource(DAQGAME_RES_CORE_HOST, "core_host.exe", config.hostExePath, error) ||
        !extractEmbeddedResource(game.coreResourceId, game.coreFileName, config.corePath, error) ||
        (game.supportResourceId && !extractEmbeddedResource(game.supportResourceId, game.supportFileName, supportPath, error)))
    {
        LOG_E("Failed to extract the embedded core host: {}", error)
        DAQ_THROW_EXCEPTION(GeneralErrorException, "Failed to extract the embedded core host: {}", error);
    }

    // content: an explicit RomPath wins; otherwise the game's embedded default
    // content (Doom's Freedoom IWAD), otherwise content is required or absent.
    // Contentless games (Mr.Boom) ignore RomPath entirely.
    const bool acceptsContent = game.needsContent || game.defaultContentResourceId;
    const std::string romPath = settings.getPropertyValue("RomPath");
    if (!romPath.empty() && acceptsContent)
    {
        // the file stays on the host PC and is validated here so a remote
        // Start fails with a clear message instead of a dead core host
        if (!std::filesystem::exists(std::filesystem::path(romPath)))
            DAQ_THROW_EXCEPTION(InvalidParameterException, "ROM not found on the host: {}", romPath);
        config.contentPath = romPath;
    }
    else if (game.defaultContentResourceId)
    {
        if (!extractEmbeddedResource(game.defaultContentResourceId, game.defaultContentFileName, config.contentPath, error))
        {
            LOG_E("Failed to extract the embedded game content: {}", error)
            DAQ_THROW_EXCEPTION(GeneralErrorException, "Failed to extract the embedded game content: {}", error);
        }
    }
    else if (game.needsContent)
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException, "{} requires Settings.RomPath (a ROM file on the host PC)", game.name);
    }
    config.window = !static_cast<Bool>(settings.getPropertyValue("Headless"));
    config.codec = static_cast<Int>(settings.getPropertyValue("VideoCodec")) == 0 ? "vp8" : "mjpeg";
    config.bitrateKbps = static_cast<int>(static_cast<Int>(settings.getPropertyValue("BitrateKbps")));
    config.jpegQuality = static_cast<int>(static_cast<Int>(settings.getPropertyValue("JpegQuality")));
    // 0 = the selected game's recommended encode size
    const auto outputWidth = static_cast<unsigned>(static_cast<Int>(settings.getPropertyValue("OutputWidth")));
    const auto outputHeight = static_cast<unsigned>(static_cast<Int>(settings.getPropertyValue("OutputHeight")));
    config.outputWidth = outputWidth > 0 ? outputWidth : game.recommendedWidth;
    config.outputHeight = outputHeight > 0 ? outputHeight : game.recommendedHeight;
    config.frameDivisor = static_cast<unsigned>(static_cast<Int>(settings.getPropertyValue("FrameDivisor")));
    config.onEncodedFrame = [this](const uint8_t* data, size_t size) { sendVideoPacket(data, size); };
    config.onAudioSamples = [this](const uint8_t* pcm, size_t bytes) { sendAudioPacket(pcm, bytes); };
    config.onEnded = [this](bool crashed, const std::string& message) { onSessionEnded(crashed, message); };

    auto newSession = std::make_unique<GameSession>();
    if (!newSession->start(config, error))
    {
        LOG_E("Failed to start the game session: {}", error)
        DAQ_THROW_EXCEPTION(GeneralErrorException, "Failed to start the game session: {}", error);
    }

    session = std::move(newSession);

    videoDescriptor = buildVideoDescriptor(config);
    audioDescriptor = session->coreAudioSampleRate() > 0 ? buildAudioDescriptor() : nullptr;
    for (auto* player : playerImpls)
    {
        player->getVideoSignal().setDescriptor(videoDescriptor);
        if (audioDescriptor.assigned())
            player->getAudioSignal().setDescriptor(audioDescriptor);
    }

    // apply input state that arrived while no session was running
    for (unsigned i = 0; i < lastButtons.size(); i++)
        session->setPlayerInput(i, lastButtons[i].load(std::memory_order_relaxed));

    setSessionState("Running");
    LOG_I("Game session running: {} {}x{} @ {} fps (encoding {}x{} {})",
          game.name,
          session->nativeWidth(),
          session->nativeHeight(),
          session->coreFps(),
          session->encodedWidth(),
          session->encodedHeight(),
          config.codec)
}

void DaqCubeDeviceImpl::stopGame()
{
    std::scoped_lock lock(sessionSync);
    if (!session)
        return;
    session.reset();
    setSessionState("Stopped");
    LOG_I("Game session stopped")
}

void DaqCubeDeviceImpl::onPlayerButtons(unsigned playerIndex, uint32_t buttons)
{
    if (playerIndex >= lastButtons.size())
        return;
    lastButtons[playerIndex].store(buttons, std::memory_order_relaxed);

    std::scoped_lock lock(sessionSync);
    if (session)
        session->setPlayerInput(playerIndex, buttons);
}

void DaqCubeDeviceImpl::onSessionEnded(bool crashed, const std::string& message)
{
    if (crashed)
    {
        LOG_E("Game session ended: {}", message)
    }
    else
    {
        LOG_I("Game session ended: {}", message)
    }

    // deferred: this runs on the session thread, which a concurrently running
    // Stop procedure (holding the property lock) may be joining right now
    const auto state = String(crashed ? "Crashed" : "Stopped");
    auto weak = thisWeak;
    context.getScheduler().scheduleWork(Work(
        [weak, state]
        {
            PropertyObjectPtr obj = weak.assigned() ? weak.getRef() : nullptr;
            if (obj.assigned())
                obj.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue("SessionState", state);
        }));
}

void DaqCubeDeviceImpl::sendVideoPacket(const uint8_t* data, size_t size)
{
    if (!videoDescriptor.assigned() || size == 0)
        return;
    // all players share one feed so far - the same packet goes to every
    // channel (packets are immutable and refcounted; no copies involved)
    auto packet = BinaryDataPacket(nullptr, videoDescriptor, size);
    std::memcpy(packet.getRawData(), data, size);
    for (auto* player : playerImpls)
        player->getVideoSignal().sendPacket(packet);
}

void DaqCubeDeviceImpl::sendAudioPacket(const uint8_t* pcm, size_t bytes)
{
    if (!audioDescriptor.assigned() || bytes == 0)
        return;
    auto packet = BinaryDataPacket(nullptr, audioDescriptor, bytes);
    std::memcpy(packet.getRawData(), pcm, bytes);
    for (auto* player : playerImpls)
        player->getAudioSignal().sendPacket(packet);
}

DataDescriptorPtr DaqCubeDeviceImpl::buildAudioDescriptor() const
{
    // same convention as video: binary stream + self-describing metadata, so
    // the AudioOutput device configures playback from the descriptor alone
    auto meta = Dict<IString, IString>();
    meta.set(audio_meta::SampleFormat, "s16");
    meta.set(audio_meta::SampleRate, std::to_string(static_cast<int>(session->coreAudioSampleRate())));
    meta.set(audio_meta::Channels, "2");

    return DataDescriptorBuilder().setName("Audio").setSampleType(SampleType::Binary).setMetadata(meta).build();
}

DataDescriptorPtr DaqCubeDeviceImpl::buildVideoDescriptor(const GameSession::Config& config) const
{
    // Video metadata convention: binary stream, metadata mirrors AVCodecParameters
    // so the replay FB configures its decoder straight from the descriptor.
    // Derived from the config actually in force, not a re-read of the
    // (concurrently writable) properties.
    const bool vp8 = config.codec == "vp8";
    const auto divisor = std::max(config.frameDivisor, 1u);
    const AVRational framerate = av_d2q(session->coreFps() / static_cast<double>(divisor), 1'000'000);

    auto meta = Dict<IString, IString>();
    meta.set(video_meta::CodecId, config.codec);
    meta.set(video_meta::Width, std::to_string(session->encodedWidth()));
    meta.set(video_meta::Height, std::to_string(session->encodedHeight()));
    meta.set(video_meta::PixFmt, av_get_pix_fmt_name(vp8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUVJ420P));
    meta.set(video_meta::Framerate, std::to_string(framerate.num) + "/" + std::to_string(framerate.den));
    meta.set(video_meta::BitRate, vp8 ? std::to_string(config.bitrateKbps * 1000) : "0");

    return DataDescriptorBuilder().setName("Video").setSampleType(SampleType::Binary).setMetadata(meta).build();
}

void DaqCubeDeviceImpl::setSessionState(const StringPtr& state)
{
    objPtr.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue("SessionState", state);
}

END_NAMESPACE_DAQCUBE_MODULE
