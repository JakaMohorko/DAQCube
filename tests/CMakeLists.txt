set(CMAKE_FOLDER "tests")
list(APPEND CMAKE_MESSAGE_CONTEXT tests)

set(TEST_APP game_engine_tests)

find_package(GTest CONFIG REQUIRED)

add_executable(${TEST_APP}
    test_app.cpp
    test_dependencies.cpp
    test_module_load.cpp
    test_player.cpp
    test_game_engine.cpp
    miniaudio_impl.cpp
)

# header-only module internals under test (keyboard_state.h, keyboard_layout.h,
# retropad_mapping.h)
target_include_directories(${TEST_APP} PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/player_module/include
    ${CMAKE_SOURCE_DIR}/modules/daqcube_module/include
)

target_link_libraries(${TEST_APP} PRIVATE
    GTest::gtest
    ${OPENDAQ_SDK_TARGET_NAMESPACE}::opendaq
    game::sdl2
    game::ffmpeg
    game::miniaudio
    game::libretro_api
    game::retro_host
    game::core_ipc
    game::shared
)

# All module dlls land in the shared runtime output dir (see root CMakeLists)
target_compile_definitions(${TEST_APP} PRIVATE
    GAME_MODULES_DIR="$<TARGET_FILE_DIR:Player>"
    GAME_CORES_DIR="${CMAKE_SOURCE_DIR}/cores"
)

add_dependencies(${TEST_APP} Player DAQCube)

add_test(NAME ${TEST_APP}
         COMMAND ${TEST_APP}
         WORKING_DIRECTORY $<TARGET_FILE_DIR:${TEST_APP}>
)
