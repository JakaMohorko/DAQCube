cmake_minimum_required(VERSION 3.25)
opendaq_set_cmake_folder_context(TARGET_FOLDER_NAME)
project(PlayerModule VERSION ${${REPO_OPTION_PREFIX}_VERSION} LANGUAGES CXX)

set(LIB_NAME Player)
set(MODULE_HEADERS_DIR include/player_module)

set(SRC_Include
    ${MODULE_HEADERS_DIR}/common.h
    ${MODULE_HEADERS_DIR}/module_dll.h
    ${MODULE_HEADERS_DIR}/player_module_impl.h
    ${MODULE_HEADERS_DIR}/player_fb_impl.h
    ${MODULE_HEADERS_DIR}/audio_output_device_impl.h
    ${MODULE_HEADERS_DIR}/keyboard_layout.h
)

set(SRC_Srcs
    src/module_dll.cpp
    src/player_module_impl.cpp
    src/player_fb_impl.cpp
    src/audio_output_device_impl.cpp
    src/miniaudio_impl.cpp
)

add_library(${LIB_NAME} SHARED ${SRC_Include} ${SRC_Srcs})
add_library(${OPENDAQ_SDK_TARGET_NAMESPACE}::${LIB_NAME} ALIAS ${LIB_NAME})

target_link_libraries(${LIB_NAME}
    PUBLIC ${OPENDAQ_SDK_TARGET_NAMESPACE}::opendaq
    PRIVATE game::sdl2
            game::ffmpeg
            game::miniaudio
            game::shared
)

target_include_directories(${LIB_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                              $<INSTALL_INTERFACE:include>
)

target_compile_definitions(${LIB_NAME} PRIVATE
    GAME_MODULE_MAJOR_VERSION=${PROJECT_VERSION_MAJOR}
    GAME_MODULE_MINOR_VERSION=${PROJECT_VERSION_MINOR}
    GAME_MODULE_PATCH_VERSION=${PROJECT_VERSION_PATCH}
)

opendaq_set_module_properties(${LIB_NAME} ${PROJECT_VERSION_MAJOR})
