# DAQ Jam - Mr.Boom session setup

One PC hosts the game with `host.py`; every player (the host player included)
joins from their own PC with the openDAQ Python GUI and wires up their seat.
Mr.Boom seats four players: `player1..player4`.

## What everyone needs

- Windows PC on the office network (same subnet as the host lets discovery work)
- Python 3.11+ with the openDAQ package: `pip install opendaq` (3.40.3)
- The client module dll from the host's build: copy `Player-64-0.module.dll`
  (from the host PC's `build\bin`) into your Python's openDAQ modules folder:

  ```
  <Python>\Lib\site-packages\opendaq\modules\
  ```

  Find the folder with:
  `python -c "import opendaq, os; print(os.path.join(os.path.dirname(opendaq.__file__), 'modules'))"`

## Host setup (one PC)

1. Build the repo once (see README) so `build\bin` holds the module dlls, or
   get the two dlls from someone who has.
2. Make sure inbound TCP **7420** is allowed through the firewall on the
   host PC (Windows Firewall *and* any corporate endpoint firewall).
3. Start the host from the repo root - Mr.Boom is the default game (see
   *Choosing the game* below for SNES and Doom):

   ```powershell
   python launchers/host.py --modules build/bin
   ```

   Useful flags: `--windowed` also shows the game in a window on the host PC,
   `--lobby-password <pw>` changes the shared player password (default
   `daqjam`).
4. The console prints `DAQ Jam host up (serial ...)`. Leave it running.
   **One host per PC** - a second `host.py` on the same machine fails with
   "already has an OpenDAQNativeStreaming server capability".

The host player plays too: they follow the player steps below on the same PC
(the host process and a GUI client coexist fine).

## Choosing the game

The host picks the game; players just join. Seats per game: **Mr.Boom 4**,
**SNES 2**, **Doom 1** (everyone without a seat still gets the video/audio
feed as a spectator). Two ways to select:

**At launch, with `host.py` flags:**

```powershell
# Mr.Boom (default) - contentless, seats 4
python launchers/host.py --modules build/bin

# SNES - seats 2; needs a ROM file on THIS (host) PC
python launchers/host.py --modules build/bin --game snes --rom "C:\roms\game.sfc"

# Doom - 1 seat + spectators; boots the embedded Freedoom IWAD, no file needed
python launchers/host.py --modules build/bin --game doom
```

**Later, from the GUI** (only player1 or admin - the `Settings` object is
theirs): select the game device, open `Settings` and set

- `Game` = `MrBoom` / `SNES` / `Doom`
- `RomPath` = full path to the content file **on the host PC**
  - SNES: required, a `.sfc`/`.smc` ROM
  - Doom: optional - another WAD instead of the embedded Freedoom one
  - Mr.Boom: ignored (the game is contentless)

then execute `Stop` (if a session is running) and `Start`.

About the ROM path: the file is read by the host process and never sent over
the network, so only the host PC needs it - players never supply a path. A
wrong path fails `Start` with "ROM not found on the host: ...". Switching
games re-seats the channels; in SNES mode only `Player1`/`Player2` have
active seats.

## Player setup (every player, in the openDAQ GUI)

1. **Launch the GUI:**

   ```powershell
   python -m opendaq
   ```

2. **Connect to the host:** click *Add device*. The host shows up via
   discovery as **DAQCube**; if it does not (different subnet), type
   the address `daq.nd://<host-ip>` yourself. In the connect dialog's
   **General** tab enter:
   - `Username` = your assigned seat: `player1` ... `player4`
   - `Password` = the lobby password (default `daqjam`)

   After connecting, the device tree shows the game device with exactly one
   channel - **your** `PlayerN` seat (the access model hides the others).

3. **Add your Player function block:** click *Add function block*, pick
   **Player**. Then set its properties:
   - `PlayerTag` = your username (e.g. `player2`) - required, one-shot, and
     must be unique across players; the `State` signal appears once set, and
     the small capture window opens
   - `EnableVideo` = `true` - the `Video` input port appears

4. **Wire your seat (two connections):**
   - your `PlayerN` channel's **Keyboard** input port -> connect the Player
     FB's **State** signal
   - the Player FB's **Video** input port -> connect the `PlayerN` channel's
     **Video** signal

5. **Sound (optional):** click *Add device* again and connect to
   `daqaudioout://default` (the AudioOutput device for your speakers). Wire
   its `Output` channel's **Audio** input port -> your `PlayerN` channel's
   **Audio** signal. The `Volume` property on the Output channel is yours to
   taste.

6. **Start the game** (player1 or admin only): in the device tree select the
   game device, open its `Settings` object and execute **Start**.
   `SessionState` flips to `Running` and video appears in every player's
   window.

7. **Play:** focus your Player window (the one from step 3).
   - **Enter** = join / start the round
   - **Space** = drop bomb
   - **WASD / arrows** = move

   Keys release automatically when the window loses focus. Bindings live on
   your channel (`KeyUp`, `KeyB`, ...) if you want to remap.

Prefer zero clicks? `python launchers/client.py --user playerN --server
daq.nd://<host-ip>` does steps 1-5 automatically (player1 also auto-starts
the game).

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| Host not discovered in *Add device* | Different subnet - type `daq.nd://<host-ip>` directly (mDNS does not cross subnets) |
| Connect fails / times out | Inbound TCP 7420 blocked on the host - Windows Firewall rule for the **active** profile, and corporate endpoint firewalls (e.g. Trend Micro) filter independently |
| "already has an OpenDAQNativeStreaming server capability" | Another `host.py` is already running on that PC - stop it first |
| `PlayerTag` write fails | The tag is one-shot; add a fresh Player FB to change it. Duplicate tags are rejected by the host on connect |
| No video after Start | `EnableVideo` not set before connecting the Video port, or the port is wired to another player's channel |
| Keys do nothing in-game | The Player window is not focused, or the State signal is not connected to **your** channel's Keyboard port |
| Second Player FB in one GUI shows no window | By design - one SDL window per process; it logs a warning and runs windowless |
| `Start` fails with "ROM not found on the host" | `RomPath` must be a path on the **host** PC (the machine running `host.py`), not on a player's PC |
