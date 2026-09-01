set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
cmake_minimum_required(VERSION 3.25)

set(REPO_NAME OpenDAQGameEngine)
set(REPO_OPTION_PREFIX DAQ_GAME)

# Use the local vcpkg clone (external/vcpkg) unless a toolchain was given explicitly
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/external/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
endif()

# Static third-party libs (dynamic CRT): the deliverable is just the module
# dlls, copied next to a separately shipped openDAQ runtime - they must not
# drag SDL2/FFmpeg dlls along.
if (WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
endif()

list(APPEND CMAKE_MESSAGE_CONTEXT ${REPO_NAME})
add_subdirectory(cmake)

opendaq_read_file_contents("${CMAKE_CURRENT_LIST_DIR}/module_version" module_version)
opendaq_get_version_major_minor_patch("${module_version}" ${REPO_OPTION_PREFIX}_VERSION)

opendaq_common_early_setup()

project(${REPO_NAME} VERSION ${${REPO_OPTION_PREFIX}_VERSION} LANGUAGES C CXX)

if (PROJECT_IS_TOP_LEVEL)
    message(STATUS "Building ${REPO_NAME} version ${${REPO_OPTION_PREFIX}_VERSION} standalone")
else()
    message(STATUS "Building ${REPO_NAME} version ${${REPO_OPTION_PREFIX}_VERSION} as submodule")
endif()

# options
opendaq_setup_common_build_options()
opendaq_setup_project_specific_build_options(${REPO_OPTION_PREFIX})
option(${REPO_OPTION_PREFIX}_ENABLE_TESTS "Enable ${REPO_NAME} testing" ${PROJECT_IS_TOP_LEVEL})
# The native streaming client/server modules ship from a separate openDAQ
# build (the opendaq python package on launcher PCs, or a dedicated SDK
# build) - building them here duplicates dlls that get shadowed at runtime.
# Turn this ON only when an in-tree test needs the native protocol out of
# build/bin.
option(${REPO_OPTION_PREFIX}_ENABLE_NATIVE_MODULES "Build the openDAQ native streaming client/server modules with the fetched SDK" OFF)

opendaq_common_compile_targets_settings()
opendaq_setup_compiler_flags(${REPO_OPTION_PREFIX})

# Everything (SDK dlls, modules, test executables) lands in one bin dir
# so module discovery and dll resolution work without extra copy steps.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# External dependencies from the vcpkg manifest
find_package(SDL2 CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)

# openDAQ: use an installed SDK matching the pinned ref, otherwise fetch and build it.
# The ref is either a release tag (vX.Y.Z) or a branch; only a tag can match an
# installed SDK - a branch ref always fetches.
opendaq_read_file_contents("${CMAKE_CURRENT_LIST_DIR}/opendaq_ref" OPENDAQ_REF)
if (OPENDAQ_REF MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$")
    string(REGEX REPLACE "^v" "" OPENDAQ_PINNED_VERSION "${OPENDAQ_REF}")
else()
    set(OPENDAQ_PINNED_VERSION "")
endif()
if (NOT TARGET "${OPENDAQ_SDK_TARGET_NAMESPACE}::${OPENDAQ_SDK_TARGET_NAME}")
    if (PROJECT_IS_TOP_LEVEL AND OPENDAQ_PINNED_VERSION)
        find_package(${OPENDAQ_SDK_NAME} ${OPENDAQ_PINNED_VERSION} GLOBAL)
    endif()
    if (NOT ${OPENDAQ_SDK_NAME}_FOUND)
        include(FetchContent)
        set(OPENDAQ_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
        # Test utils would pick up the vcpkg GTest package and trip over a
        # target-name bug in the SDK's external/CMakeLists.txt; we don't need them.
        set(OPENDAQ_ENABLE_TEST_UTILS OFF CACHE BOOL "" FORCE)
        # The native streaming protocol + client/server modules build only on
        # request (see the ENABLE_NATIVE_MODULES option above) - at runtime the
        # launchers load them from the opendaq python package, and patched
        # builds come from the openDAQ fork's own build tree.
        set(OPENDAQ_ENABLE_NATIVE_STREAMING ${${REPO_OPTION_PREFIX}_ENABLE_NATIVE_MODULES} CACHE BOOL "" FORCE)
        set(DAQMODULES_OPENDAQ_CLIENT_MODULE ${${REPO_OPTION_PREFIX}_ENABLE_NATIVE_MODULES} CACHE BOOL "" FORCE)
        set(DAQMODULES_OPENDAQ_SERVER_MODULE ${${REPO_OPTION_PREFIX}_ENABLE_NATIVE_MODULES} CACHE BOOL "" FORCE)

        message(STATUS "Fetching ${OPENDAQ_SDK_NAME} ${OPENDAQ_REF}")
        FetchContent_Declare(
            ${OPENDAQ_SDK_NAME}
            GIT_REPOSITORY https://github.com/openDAQ/openDAQ.git
            GIT_TAG ${OPENDAQ_REF}
            GIT_PROGRESS   ON
            SOURCE_DIR ${FETCHCONTENT_EXTERNALS_DIR}/src/${OPENDAQ_SDK_NAME}
        )
        FetchContent_MakeAvailable(${OPENDAQ_SDK_NAME})
    else()
        message(STATUS "Found installed ${OPENDAQ_SDK_NAME} version: ${${OPENDAQ_SDK_NAME}_VERSION}")
    endif()
endif()

add_subdirectory(external)
add_subdirectory(shared)
add_subdirectory(modules)

if (${REPO_OPTION_PREFIX}_ENABLE_TESTS)
    message(STATUS "Unit tests in ${REPO_NAME} are ENABLED")
    enable_testing()
    add_subdirectory(tests)
else()
    message(STATUS "Unit tests in ${REPO_NAME} are DISABLED")
endif()
