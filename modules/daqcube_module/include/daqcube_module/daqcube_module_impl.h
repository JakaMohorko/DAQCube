#pragma once
#include <daqcube_module/common.h>
#include <opendaq/module_impl.h>

BEGIN_NAMESPACE_DAQCUBE_MODULE

class DaqCubeModule final : public Module
{
public:
    explicit DaqCubeModule(ContextPtr context);

    ListPtr<IDeviceInfo> onGetAvailableDevices() override;
    DictPtr<IString, IDeviceType> onGetAvailableDeviceTypes() override;
    DevicePtr onCreateDevice(const StringPtr& connectionString,
                             const ComponentPtr& parent,
                             const PropertyObjectPtr& config) override;
};

END_NAMESPACE_DAQCUBE_MODULE
