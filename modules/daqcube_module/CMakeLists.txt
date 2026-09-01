cmake_minimum_required(VERSION 3.25)
opendaq_set_cmake_folder_context(TARGET_FOLDER_NAME)
project(DAQCubeModule VERSION ${${REPO_OPTION_PREFIX}_VERSION} LANGUAGES CXX RC)

set(LIB_NAME DAQCube)
set(MODULE_HEADERS_DIR include/daqcube_module)

# core_host: the child process that runs the libretro core. Built first, then
# embedded into the module dll as a resource (below) - it is not shipped alone.
add_executable(core_host core_host/main.cpp)
target_link_libraries(core_host PRIVATE
    game::retro_host
    game::core_ipc
    game::sdl2
)
target_compile_definitions(core_host PRIVATE SDL_MAIN_HANDLED)

# Stage the payload with stable paths so the generated .rc can reference it and
# the resource recompiles whenever core_host or an embedded file changes. The
# game cores and their support/content wads are build inputs fetched separately
# (tools/fetch_cores.ps1); one EMBEDDED_BLOBS "<macro suffix>=<file>" entry
# covers the existence check, staging and .rc line (resource ids are the
# DAQGAME_RES_<suffix> macros in embedded_resources.h).
set(EMBEDDED_BLOBS
    CORE_MRBOOM=mrboom_libretro.dll
    CORE_SNES9X=snes9x_libretro.dll
    CORE_PRBOOM=prboom_libretro.dll
    WAD_PRBOOM=prboom.wad
    WAD_FREEDOOM=freedoom1.wad
)

set(EMBED_DIR ${CMAKE_CURRENT_BINARY_DIR}/embed)
add_custom_command(OUTPUT ${EMBED_DIR}/core_host.exe
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:core_host> ${EMBED_DIR}/core_host.exe
    DEPENDS core_host
    COMMENT "Staging core_host.exe for embedding")
set(EMBEDDED_PAYLOAD ${EMBED_DIR}/core_host.exe)
set(RESOURCES_RC_CONTENT
    "#include <daqcube_module/embedded_resources.h>\n"
    "DAQGAME_RES_CORE_HOST RCDATA \"embed/core_host.exe\"\n")

foreach(blob IN LISTS EMBEDDED_BLOBS)
    string(REPLACE "=" ";" blob_parts ${blob})
    list(GET blob_parts 0 blob_macro)
    list(GET blob_parts 1 blob_file)
    set(blob_source ${CMAKE_SOURCE_DIR}/cores/${blob_file})
    if (NOT EXISTS ${blob_source})
        message(FATAL_ERROR
            "cores/${blob_file} not found - run tools/fetch_cores.ps1 first "
            "(game cores and wads are embedded into the DAQCube module dll)")
    endif()
    add_custom_command(OUTPUT ${EMBED_DIR}/${blob_file}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${blob_source} ${EMBED_DIR}/${blob_file}
        DEPENDS ${blob_source}
        COMMENT "Staging ${blob_file} for embedding")
    list(APPEND EMBEDDED_PAYLOAD ${EMBED_DIR}/${blob_file})
    list(APPEND RESOURCES_RC_CONTENT "DAQGAME_RES_${blob_macro} RCDATA \"embed/${blob_file}\"\n")
endforeach()

add_custom_target(daqcube_embedded_payload DEPENDS ${EMBEDDED_PAYLOAD})

set(RESOURCES_RC ${CMAKE_CURRENT_BINARY_DIR}/resources.rc)
list(JOIN RESOURCES_RC_CONTENT "" RESOURCES_RC_CONTENT)
file(WRITE ${RESOURCES_RC} "${RESOURCES_RC_CONTENT}")
set_source_files_properties(${RESOURCES_RC} PROPERTIES OBJECT_DEPENDS "${EMBEDDED_PAYLOAD}")

set(SRC_Include
    ${MODULE_HEADERS_DIR}/common.h
    ${MODULE_HEADERS_DIR}/module_dll.h
    ${MODULE_HEADERS_DIR}/daqcube_module_impl.h
    ${MODULE_HEADERS_DIR}/daqcube_device_impl.h
    ${MODULE_HEADERS_DIR}/embedded_resources.h
    ${MODULE_HEADERS_DIR}/game_session.h
    ${MODULE_HEADERS_DIR}/player_channel_impl.h
    ${MODULE_HEADERS_DIR}/resource_extractor.h
    ${MODULE_HEADERS_DIR}/retropad_mapping.h
)

set(SRC_Srcs
    src/module_dll.cpp
    src/daqcube_module_impl.cpp
    src/daqcube_device_impl.cpp
    src/game_session.cpp
    src/player_channel_impl.cpp
    src/resource_extractor.cpp
)

add_library(${LIB_NAME} SHARED ${SRC_Include} ${SRC_Srcs} ${RESOURCES_RC})
add_library(${OPENDAQ_SDK_TARGET_NAMESPACE}::${LIB_NAME} ALIAS ${LIB_NAME})
add_dependencies(${LIB_NAME} daqcube_embedded_payload)

target_link_libraries(${LIB_NAME}
    PUBLIC ${OPENDAQ_SDK_TARGET_NAMESPACE}::opendaq
    PRIVATE game::ffmpeg
            game::libretro_api
            game::core_ipc
            game::shared
            iphlpapi  # MAC address for the device serial number
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
