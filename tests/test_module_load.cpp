/*
 * Module gate: both openDAQ module dlls load into an instance,
 * expose their types under the agreed names, and instantiate.
 */

#include <gtest/gtest.h>

#include <opendaq/opendaq.h>

using namespace daq;

static InstancePtr createTestInstance()
{
    return InstanceBuilder().addModulePath(GAME_MODULES_DIR).build();
}

TEST(ModuleLoad, TypesAvailable)
{
    auto instance = createTestInstance();

    auto fbTypes = instance.getAvailableFunctionBlockTypes();
    ASSERT_TRUE(fbTypes.hasKey("Player"));

    auto deviceTypes = instance.getAvailableDeviceTypes();
    ASSERT_TRUE(deviceTypes.hasKey("DAQCube"));
}

TEST(ModuleLoad, CubeDeviceDiscoverable)
{
    auto instance = createTestInstance();

    bool found = false;
    for (const auto& info : instance.getAvailableDevices())
        if (info.getConnectionString() == "daqcube://localhost")
        {
            found = true;
            EXPECT_EQ(info.getDeviceType().getId(), "DAQCube");
        }
    EXPECT_TRUE(found) << "daqcube://localhost missing from the device scan";
}

TEST(ModuleLoad, InstantiatePlayer)
{
    auto instance = createTestInstance();
    auto fb = instance.addFunctionBlock("Player");
    ASSERT_TRUE(fb.assigned());
    ASSERT_EQ(fb.getFunctionBlockType().getId(), "Player");
}

TEST(ModuleLoad, InstantiateCubeDevice)
{
    auto instance = createTestInstance();
    auto device = instance.addDevice("daqcube://localhost");
    ASSERT_TRUE(device.assigned());
    ASSERT_EQ(device.getInfo().getDeviceType().getId(), "DAQCube");
}
