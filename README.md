# DAQCube

Multiplayer game sessions over openDAQ. 

Developed as part of a "DAQ Jam" event at openDAQ - goal was to implement an openDAQ-based application. 

> Disclaimer: The design and architecture are my own; the implementation was written with the help of agentic AI (Claude Code).

## Contents

Contains two modules:

- **Player** - client module with two component types:
  - the **Player FB**: a single SDL2 window that replays the game video
    stream and captures the keyboard while focused. Its signals appear only
    once the `PlayerTag` string property is set (the tag is baked into the
    signal ids so clients cannot collide on the host), and its `Video` input
    port exists only while `EnableVideo` is true (default off - capture-only)
  - the **AudioOutput device** (`daqaudioout://default` or
    `daqaudioout://<index>`, modeled after the openDAQ audio device example):
    one `Output` channel whose input port accepts the game's binary PCM audio
    signal and plays it on this PC's speakers - format from the descriptor
    metadata, `Volume` property, silence on underrun
- **DAQCube** - server root device hosting libretro game cores; four
  `Player1..4` channels, each taking a keyboard state signal in and carrying
  that player's encoded `Video` and raw PCM `Audio` signals out (one shared
  feed so far - every channel gets the same packets). `Settings.Game` selects
  the game:
  - **MrBoom** - contentless Bomberman clone, seats 4 players
  - **SNES** - the snes9x core running a user-supplied ROM from
    `Settings.RomPath`, seats 2 players
  - **Doom** - the prboom core; boots the embedded Freedoom Phase 1 IWAD out
    of the box (`Settings.RomPath` optionally points at another WAD), seats 1
    player - the other channels spectate the video feed

## Prerequisites

- Windows, the MSVC toolchain (Visual Studio 2022+ *or* the free Build Tools
  with the C++ workload - the IDE itself is optional), CMake 3.25+, git
- No preinstalled openDAQ needed: the build uses an installed SDK matching
  the version pinned in `opendaq_ref` (currently v3.40.3) and otherwise
  fetches and builds openDAQ from source
- `git config --global core.longpaths true` (openDAQ has paths beyond MAX_PATH)

## Build

One-time setup:

```powershell
# bootstrap vcpkg (SDL2, FFmpeg LGPL, miniaudio, gtest)
git clone https://github.com/microsoft/vcpkg.git external/vcpkg
./external/vcpkg/bootstrap-vcpkg.bat -disableMetrics

# fetch the game cores + Doom wads (build inputs - they get embedded into
# the DAQCube module dll together with core_host.exe)
./tools/fetch_cores.ps1
```

Licensing of the embedded blobs: Mr.Boom is MIT
([mrboom-libretro](https://github.com/libretro/mrboom-libretro)), prboom
(core + `prboom.wad`) is GPL-2.0
([libretro-prboom](https://github.com/libretro/libretro-prboom)), snes9x is
freeware for personal, non-commercial use only, and
[Freedoom](https://freedoom.github.io/) is BSD-3-Clause. Fine for a game
night; the snes9x core rules out selling the built module dll, and shipping
the GPL prboom core means offering its source (the upstream repo).

### Visual Studio 2026 (primary IDE)

Generate the solution with a CMake that knows the VS 18 generator (the
VS-bundled one does; the system CMake 3.29 does not):

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset vs2026
```

Then open `build-vs/OpenDAQGameEngine.slnx`.

### Command line (Ninja)

Build from a **x64 Native Tools Command Prompt** (or any shell with vcvars64
loaded) so CMake/Ninja find the MSVC toolchain:

```bash
cmake --preset ninja-release
cmake --build build
```

The first configure builds static FFmpeg/SDL2/gtest via vcpkg (~20 min, then
binary-cached) and the first build compiles the openDAQ SDK (~15 min with
ccache). Third-party libs are linked statically (`x64-windows-static-md`
triplet) so the module dlls are self-contained.

Everything (SDK dlls, module dlls, executables) lands in `build/bin/` (Ninja)
or `build-vs/bin/<Config>/` (VS). Module dlls use the openDAQ `.module.dll`
suffix. The openDAQ native streaming client/server modules are NOT built by
default - at runtime they come from a separate openDAQ build (the `opendaq`
python package on launcher PCs shadows `build/bin` copies anyway). Configure
with `-DDAQ_GAME_ENABLE_NATIVE_MODULES=ON` when an in-tree test needs the
native protocol served from `build/bin`.

## Test

```powershell
ctest --test-dir build --output-on-failure
```

Set `DAQGAME_SNES_ROM=<path to a .sfc>` to also run the SNES end-to-end test
(skipped otherwise - the repo ships no ROMs; any freely licensed homebrew ROM
works).

The suite gates: every external dependency initializes (SDL, FFmpeg VP8 +
MJPEG encode/decode + swscale, miniaudio, a libretro core); both module dlls
load into an openDAQ instance and instantiate; the Player FB's
bitmap/descriptor/heartbeat/PlayerTag behavior; the core host shared-memory
protocol; the DAQCube device end to end (Start spawns the embedded core
host and encoded packets matching the descriptor arrive on the player
channels); and the Player FB decoding the live device stream from descriptor
metadata alone.

## Remote multiplayer

On the host PC (needs Python + the `opendaq` pip package):

```powershell
python launchers/host.py --modules build/bin
```

For an SNES session instead of Mr.Boom (the ROM is a file on the host PC and
is never sent over the network; only players 1-2 get a seat):

```powershell
python launchers/host.py --modules build/bin --game snes --rom C:\roms\game.sfc
```

For Doom (player1 plays, everyone else spectates; no ROM needed - the
embedded Freedoom IWAD is the default, `--rom` overrides it with another WAD):

```powershell
python launchers/host.py --modules build/bin --game doom
```

Users `player1..4` share the lobby password (default `daqjam`); `admin` has
full access. The access model ships inside the module: each player sees only
their own `PlayerN` channel (their keyboard input and video feed), and only
player1 (or admin) can touch the device `Settings` (game start/stop
included).

On each player's PC:

```powershell
python launchers/client.py --user player1 --server daq.nd://<host-ip>
```

This opens the local Player window: the keyboard attaches to the player's
channel via client-to-device streaming, the channel's video feed plays in
the same window, and the channel's audio feed plays on the default speakers
through a local AudioOutput device (skipped on PCs without audio). player1
starts the game automatically.

At runtime the device extracts the embedded `core_host.exe` + the selected
game core to `%LOCALAPPDATA%\DAQCube\<version>\` and spawns it. The
video stream defaults to VP8 at the game's recommended encode size (a 2x
upscale of the core's native geometry, e.g. 640x400 for Mr.Boom) and 4000
kbps; `VideoCodec`, `BitrateKbps`, `JpegQuality`, `OutputWidth/Height` and
`FrameDivisor` live under the device `Settings`.

## Video metadata convention

The game video signal is a plain binary stream; its data descriptor metadata
mirrors FFmpeg `AVCodecParameters` fields (`codec_id`, `width`, `height`,
`pix_fmt`, `framerate`, `bit_rate`). See
`shared/include/game_engine_shared/video_metadata.h`.

## License

The source in this repository is [MIT](LICENSE). Built module dlls
additionally embed third-party blobs under their own terms - see the
licensing note in the Build section (the snes9x core makes the built dll
non-commercial-only).
