#pragma once
#include <player_module/common.h>
#include <opendaq/module_impl.h>

#include <memory>
#include <mutex>

BEGIN_NAMESPACE_PLAYER_MODULE

class MiniaudioContext;

// Client-side module: the Player FB (keyboard capture + video replay) and the
// AudioOutput device (game audio -> this PC's speakers). The miniaudio
// context is created lazily so the module still loads - and the Player FB
// still works - on machines without any audio backend.
class PlayerModule final : public Module
{
public:
    explicit PlayerModule(ContextPtr context);

    DictPtr<IString, IFunctionBlockType> onGetAvailableFunctionBlockTypes() override;
    FunctionBlockPtr onCreateFunctionBlock(const StringPtr& id,
                                           const ComponentPtr& parent,
                                           const StringPtr& localId,
                                           const PropertyObjectPtr& config) override;

    ListPtr<IDeviceInfo> onGetAvailableDevices() override;
    DictPtr<IString, IDeviceType> onGetAvailableDeviceTypes() override;
    DevicePtr onCreateDevice(const StringPtr& connectionString,
                             const ComponentPtr& parent,
                             const PropertyObjectPtr& config) override;

private:
    std::shared_ptr<MiniaudioContext> miniaudioContext();  // lazy, may return nullptr

    std::mutex sync;
    std::shared_ptr<MiniaudioContext> maContext;
    bool maContextFailed = false;
    int deviceCounter = 0;
};

END_NAMESPACE_PLAYER_MODULE
