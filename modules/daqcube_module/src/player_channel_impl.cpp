#include <daqcube_module/player_channel_impl.h>
#include <daqcube_module/retropad_mapping.h>

#include <game_engine_shared/daq_string.h>
#include <game_engine_shared/keyboard_state.h>

#include <coreobjects/permission_mask_builder_factory.h>
#include <coreobjects/permissions_builder_factory.h>
#include <coreobjects/property_factory.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <opendaq/custom_log.h>
#include <opendaq/data_packet_ptr.h>
#include <opendaq/event_packet_ids.h>
#include <opendaq/event_packet_params.h>
#include <opendaq/event_packet_ptr.h>
#include <opendaq/data_descriptor_factory.h>
#include <opendaq/function_block_type_factory.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

BEGIN_NAMESPACE_DAQCUBE_MODULE

using game_engine::KeyboardState;
using game_engine::toStd;

namespace
{
    constexpr size_t BindingCount = std::size(KeyBindings);
}

PlayerChannelImpl::PlayerChannelImpl(const ContextPtr& ctx,
                                     const ComponentPtr& parent,
                                     const StringPtr& localId,
                                     unsigned playerIndex,
                                     ButtonsCallback onButtons)
    : Channel(CreateType(), ctx, parent, localId)
    , playerIndex(playerIndex)
    , onButtons(std::move(onButtons))
{
    bitToButton.fill(-1);
    initProperties();
    inputPort = createAndAddInputPort("Keyboard", PacketReadyNotification::SchedulerQueueWasEmpty);

    // placeholder binary descriptors; the device swaps in fully populated ones
    // (codec/dimensions/framerate, PCM format metadata) when a session starts
    videoSignal = createAndAddSignal("Video", DataDescriptorBuilder().setName("Video").setSampleType(SampleType::Binary).build());
    audioSignal = createAndAddSignal("Audio", DataDescriptorBuilder().setName("Audio").setSampleType(SampleType::Binary).build());

    // remote users: each player sees and drives only their own slot (the
    // other slots are invisible to them); admin has full access everywhere
    objPtr.getPermissionManager().setPermissions(
        PermissionsBuilder()
            .inherit(false)
            .assign("admin", PermissionMaskBuilder().read().write().execute())
            .assign("player" + std::to_string(playerIndex + 1), PermissionMaskBuilder().read().write().execute())
            .build());
}

FunctionBlockTypePtr PlayerChannelImpl::CreateType()
{
    return FunctionBlockType("GamePlayer",
                             "GamePlayer",
                             "Game player channel: keyboard state in, encoded game video out");
}

void PlayerChannelImpl::initProperties()
{
    objPtr.addProperty(BoolPropertyBuilder("Available", False).setReadOnly(True).build());

    // input-path diagnostic: proves keyboard packets reach this slot (useful
    // for checking a remote client's connection without pressing keys)
    objPtr.addProperty(IntPropertyBuilder("PacketsReceived", 0).setReadOnly(True).build());

    // one key list per RetroPad button; several Player FBs can share one
    // keyboard signal with disjoint bindings (single-PC multiplayer)
    for (size_t slot = 0; slot < BindingCount; slot++)
    {
        const auto& binding = KeyBindings[slot];
        boundKeys[slot] = parseKeyList(binding.defaultKeys);
        objPtr.addProperty(StringProperty(binding.propertyName, binding.defaultKeys));
        objPtr.getOnPropertyValueWrite(binding.propertyName) +=
            [this, slot](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
            {
                std::scoped_lock lock(mappingSync);
                boundKeys[slot] = parseKeyList(toStd(args.getValue().asPtrOrNull<IString>()));
                rebuildBitMapping();
            };
    }
}

void PlayerChannelImpl::setAvailable(bool available)
{
    objPtr.asPtr<IPropertyObjectProtected>().setProtectedPropertyValue("Available", available ? True : False);
}

void PlayerChannelImpl::onPacketReceived(const InputPortPtr& port)
{
    const auto connection = port.getConnection();
    if (!connection.assigned())
        return;

    for (auto packet = connection.dequeue(); packet.assigned(); packet = connection.dequeue())
    {
        if (packet.getType() == PacketType::Event)
        {
            const EventPacketPtr eventPacket = packet;
            if (eventPacket.getEventId() == event_packet_id::DATA_DESCRIPTOR_CHANGED)
            {
                std::scoped_lock lock(mappingSync);
                lastDescriptor = eventPacket.getParameters().get(event_packet_param::DATA_DESCRIPTOR);
                rebuildBitMapping();
            }
        }
        else if (packet.getType() == PacketType::Data)
        {
            const DataPacketPtr dataPacket = packet;
            const auto sampleCount = dataPacket.getSampleCount();
            const auto* raw = static_cast<const uint8_t*>(dataPacket.getRawData());
            if (sampleCount == 0 || !raw || dataPacket.getRawDataSize() < sampleCount * KeyboardState::ByteSize)
                continue;

            // only the newest state matters - skip stale samples in the packet
            KeyboardState state;
            std::memcpy(state.words.data(), raw + (sampleCount - 1) * KeyboardState::ByteSize, KeyboardState::ByteSize);
            packetsReceived += sampleCount;
            processState(state);
        }
    }

    // publish the diagnostic counter at most once a second
    const auto now = std::chrono::steady_clock::now();
    if (packetsReceived > 0 && now - lastCounterPublish >= std::chrono::seconds(1))
    {
        lastCounterPublish = now;
        objPtr.asPtr<IPropertyObjectProtected>().setProtectedPropertyValue("PacketsReceived",
                                                                           static_cast<Int>(packetsReceived));
    }
}

void PlayerChannelImpl::onDisconnected(const InputPortPtr& /*port*/)
{
    {
        std::scoped_lock lock(mappingSync);
        lastDescriptor = nullptr;
        mappingValid = false;
    }
    // release all buttons so nothing stays held when a player drops out
    if (lastButtons != 0)
    {
        lastButtons = 0;
        if (onButtons)
            onButtons(playerIndex, 0);
    }
}

// caller holds mappingSync
void PlayerChannelImpl::rebuildBitMapping()
{
    bitToButton.fill(-1);
    mappingValid = false;
    if (!lastDescriptor.assigned() || !lastDescriptor.getMetadata().assigned())
        return;

    // the keyboard signal is self-describing: "Bit.<Name>" metadata entries
    // give each key's bit index, so any 128-bit bitmap layout works here
    const std::string bitPrefix = game_engine::KeyBitMetadataPrefix;
    std::unordered_map<std::string, unsigned> bitByName;
    for (const auto& [key, value] : lastDescriptor.getMetadata())
    {
        const std::string keyStr = toStd(key);
        if (keyStr.rfind(bitPrefix, 0) != 0)
            continue;
        const unsigned long bit = std::strtoul(toStd(value).c_str(), nullptr, 10);
        if (bit < bitToButton.size())
            bitByName.emplace(keyStr.substr(bitPrefix.size()), static_cast<unsigned>(bit));
    }

    if (bitByName.empty())
    {
        LOG_W("Player{}: connected signal has no Bit.<Name> metadata - input ignored", playerIndex + 1)
        return;
    }

    unsigned mapped = 0;
    for (size_t slot = 0; slot < BindingCount; slot++)
    {
        for (const auto& keyName : boundKeys[slot])
        {
            const auto found = bitByName.find(keyName);
            if (found == bitByName.end())
            {
                LOG_W("Player{}: {} binds unknown key '{}' - ignored",
                      playerIndex + 1,
                      KeyBindings[slot].propertyName,
                      keyName)
                continue;
            }
            bitToButton[found->second] = static_cast<int8_t>(KeyBindings[slot].button);
            mapped++;
        }
    }
    mappingValid = mapped > 0;
}

void PlayerChannelImpl::processState(const game_engine::KeyboardState& state)
{
    uint32_t buttons = 0;
    {
        std::scoped_lock lock(mappingSync);
        if (!mappingValid)
            return;
        for (unsigned bit = 0; bit < bitToButton.size(); bit++)
            if (bitToButton[bit] >= 0 && state.test(bit))
                buttons |= 1u << bitToButton[bit];
    }

    if (buttons == lastButtons)
        return;
    lastButtons = buttons;
    if (onButtons)
        onButtons(playerIndex, buttons);
}

END_NAMESPACE_DAQCUBE_MODULE
