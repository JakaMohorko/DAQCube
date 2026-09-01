#include <player_module/player_module_impl.h>
#include <player_module/audio_output_device_impl.h>
#include <player_module/player_fb_impl.h>

#include <coretypes/version_info_factory.h>
#include <opendaq/custom_log.h>
#include <opendaq/device_info_factory.h>

#ifdef _WIN32
    #include <combaseapi.h>
#endif

#include <string>
#include <utility>

BEGIN_NAMESPACE_PLAYER_MODULE

PlayerModule::PlayerModule(ContextPtr context)
    : Module("Player",
             daq::VersionInfo(GAME_MODULE_MAJOR_VERSION, GAME_MODULE_MINOR_VERSION, GAME_MODULE_PATCH_VERSION),
             std::move(context),
             "Player")
{
}

DictPtr<IString, IFunctionBlockType> PlayerModule::onGetAvailableFunctionBlockTypes()
{
    auto types = Dict<IString, IFunctionBlockType>();

    const auto type = PlayerFbImpl::CreateType();
    types.set(type.getId(), type);

    return types;
}

FunctionBlockPtr PlayerModule::onCreateFunctionBlock(const StringPtr& id,
                                                     const ComponentPtr& parent,
                                                     const StringPtr& localId,
                                                     const PropertyObjectPtr& /*config*/)
{
    if (id == PlayerFbImpl::CreateType().getId())
        return createWithImplementation<IFunctionBlock, PlayerFbImpl>(context, parent, localId);

    LOG_W("Function block \"{}\" not found", id);
    throw NotFoundException("Function block not found");
}

std::shared_ptr<MiniaudioContext> PlayerModule::miniaudioContext()
{
    // caller holds `sync`; failure is remembered so audio-less machines don't
    // retry the backend probe on every enumeration
    if (!maContext && !maContextFailed)
    {
        try
        {
            maContext = std::make_shared<MiniaudioContext>();
        }
        catch (const std::exception& e)
        {
            maContextFailed = true;
            LOG_W("no audio backend - AudioOutput devices unavailable ({})", e.what())
        }
    }
    return maContext;
}

ListPtr<IDeviceInfo> PlayerModule::onGetAvailableDevices()
{
    auto devices = List<IDeviceInfo>();

    std::scoped_lock lock(sync);
    const auto ctx = miniaudioContext();
    if (!ctx)
        return devices;

    ma_device_info* playbackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    if (ma_context_get_devices(ctx->getPtr(), &playbackInfos, &playbackCount, nullptr, nullptr) != MA_SUCCESS)
        return devices;

    for (ma_uint32 i = 0; i < playbackCount; i++)
        devices.pushBack(AudioOutputDeviceImpl::CreateDeviceInfo("daqaudioout://" + std::to_string(i), playbackInfos[i].name));
    return devices;
}

DictPtr<IString, IDeviceType> PlayerModule::onGetAvailableDeviceTypes()
{
    const auto type = AudioOutputDeviceImpl::CreateType();
    return Dict<IString, IDeviceType>({{type.getId(), type}});
}

DevicePtr PlayerModule::onCreateDevice(const StringPtr& connectionString,
                                       const ComponentPtr& parent,
                                       const PropertyObjectPtr& /*config*/)
{
    std::scoped_lock lock(sync);
    const auto ctx = miniaudioContext();
    if (!ctx)
        DAQ_THROW_EXCEPTION(NotFoundException, "no audio backend available on this PC");

    const std::string localId = "AudioOutput" + std::to_string(deviceCounter++);
    return createWithImplementation<IDevice, AudioOutputDeviceImpl>(context, parent, StringPtr(localId), ctx, connectionString);
}

END_NAMESPACE_PLAYER_MODULE
