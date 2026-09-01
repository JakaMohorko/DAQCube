#include <daqcube_module/daqcube_module_impl.h>
#include <daqcube_module/daqcube_device_impl.h>

#include <coretypes/version_info_factory.h>
#include <opendaq/custom_log.h>

#include <utility>

BEGIN_NAMESPACE_DAQCUBE_MODULE

DaqCubeModule::DaqCubeModule(ContextPtr context)
    : Module("DAQCube",
             daq::VersionInfo(GAME_MODULE_MAJOR_VERSION, GAME_MODULE_MINOR_VERSION, GAME_MODULE_PATCH_VERSION),
             std::move(context),
             "DAQCube")
{
}

ListPtr<IDeviceInfo> DaqCubeModule::onGetAvailableDevices()
{
    // the game host is always exactly one local (pseudo-)device
    return List<IDeviceInfo>(DaqCubeDeviceImpl::CreateDeviceInfo());
}

DictPtr<IString, IDeviceType> DaqCubeModule::onGetAvailableDeviceTypes()
{
    auto types = Dict<IString, IDeviceType>();

    const auto type = DaqCubeDeviceImpl::CreateType();
    types.set(type.getId(), type);

    return types;
}

DevicePtr DaqCubeModule::onCreateDevice(const StringPtr& connectionString,
                                              const ComponentPtr& parent,
                                              const PropertyObjectPtr& /*config*/)
{
    if (!connectionString.assigned() || connectionString == "")
        return nullptr;

    return createWithImplementation<IDevice, DaqCubeDeviceImpl>(context, parent, StringPtr("DAQCube"));
}

END_NAMESPACE_DAQCUBE_MODULE
