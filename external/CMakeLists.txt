set(CMAKE_FOLDER "external")
list(APPEND CMAKE_MESSAGE_CONTEXT external)

# libretro API header (vendored, single header)
add_library(libretro_api INTERFACE)
add_library(game::libretro_api ALIAS libretro_api)
target_include_directories(libretro_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/libretro/include)

# FFmpeg (LGPL feature set from the vcpkg manifest: avcodec + swscale)
add_library(ffmpeg_libs INTERFACE)
add_library(game::ffmpeg ALIAS ffmpeg_libs)
target_include_directories(ffmpeg_libs INTERFACE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(ffmpeg_libs INTERFACE ${FFMPEG_LIBRARY_DIRS})
# openDAQ vendors a CMake target named "bcrypt" which would shadow the Windows
# system library of the same name that static avutil needs - disambiguate.
set(FFMPEG_LINK_LIBRARIES ${FFMPEG_LIBRARIES})
list(TRANSFORM FFMPEG_LINK_LIBRARIES REPLACE "^bcrypt$" "bcrypt.lib")
target_link_libraries(ffmpeg_libs INTERFACE ${FFMPEG_LINK_LIBRARIES})

# SDL2 (static or shared target name depending on triplet)
add_library(sdl2_lib INTERFACE)
add_library(game::sdl2 ALIAS sdl2_lib)
target_link_libraries(sdl2_lib INTERFACE $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>)

# miniaudio (header-only via vcpkg)
add_library(miniaudio_header INTERFACE)
add_library(game::miniaudio ALIAS miniaudio_header)
target_include_directories(miniaudio_header INTERFACE ${MINIAUDIO_INCLUDE_DIR})
