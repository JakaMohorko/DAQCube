set(CMAKE_FOLDER "shared")
list(APPEND CMAKE_MESSAGE_CONTEXT shared)

# Header-only conventions shared between modules (video metadata descriptor fields)
add_library(game_engine_shared INTERFACE)
add_library(game::shared ALIAS game_engine_shared)
target_include_directories(game_engine_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Shared-memory protocol between the DAQCube module and core_host.exe
add_library(core_ipc STATIC
    include/game_engine_shared/core_ipc.h
    src/core_ipc.cpp
)
add_library(game::core_ipc ALIAS core_ipc)
target_include_directories(core_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(core_ipc PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Minimal libretro frontend host, shared by core_host.exe and the test suite
add_library(retro_host STATIC
    include/retro_host/retro_host.h
    src/retro_host.cpp
)
add_library(game::retro_host ALIAS retro_host)
target_include_directories(retro_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(retro_host PUBLIC game::libretro_api)
set_target_properties(retro_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
