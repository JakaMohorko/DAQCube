"""
DAQ Jam host launcher.

Creates the openDAQ instance with the DAQCube device as root device,
defines the users and starts the discoverable native server. The access
model itself ships inside the module: each playerN sees and drives only
their own PlayerN channel (keyboard in, that player's video out), only
player1 (and admin) touches the Settings object, and admin has full access.
This launcher only has to create users in those groups.

All players share one lobby password. Stop with Ctrl+C.

Usage:
  python host.py [--lobby-password <pw>] [--admin-password <pw>]
                 [--modules <dir>] [--windowed]
                 [--game mrboom|snes|doom] [--rom <path>]
"""

import argparse
import time

import opendaq

PLAYERS = ["player1", "player2", "player3", "player4"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lobby-password", default="daqjam", help="shared password for player1..4")
    parser.add_argument("--admin-password", default="admin", help="password for the admin user")
    parser.add_argument("--modules", default="", help="extra module directory (e.g. the repo's build/bin)")
    parser.add_argument("--windowed", action="store_true", help="open the game window on this PC too")
    parser.add_argument("--game", choices=["mrboom", "snes", "doom"], default="mrboom",
                        help="game to host (mrboom seats 4, snes 2, doom 1 + spectators)")
    parser.add_argument("--rom", default="",
                        help="content file on this PC (required for snes; doom defaults to the embedded Freedoom IWAD)")
    args = parser.parse_args()

    if args.game == "snes" and not args.rom:
        parser.error("--game snes requires --rom <path to a ROM on this PC>")

    users = [opendaq.User(name, args.lobby_password, [name]) for name in PLAYERS]
    users.append(opendaq.User("admin", args.admin_password, ["admin"]))

    builder = opendaq.InstanceBuilder()
    if args.modules:
        builder.add_module_path(args.modules)
    builder.authentication_provider = opendaq.StaticAuthenticationProvider(False, users)
    builder.set_root_device("daqcube://localhost", None)
    # server.enable_discovery() silently does nothing unless the instance has
    # a discovery server to register with
    builder.add_discovery_server("mdns")
    instance = builder.build()
    device = instance.root_device

    settings = opendaq.IPropertyObject.cast_from(device.get_property_value("Settings"))
    settings.set_property_value("Headless", not args.windowed)
    settings.set_property_value("Game", {"mrboom": 0, "snes": 1, "doom": 2}[args.game])
    if args.rom:
        settings.set_property_value("RomPath", args.rom)

    server = instance.add_server("OpenDAQNativeStreaming", None)
    server.enable_discovery()

    print("DAQ Jam host up (serial %s)" % device.info.serial_number)
    print("users: player1..4 (lobby password) + admin; discoverable via mDNS")
    print("player1 starts the game; clients connect with client.py --user playerN")

    try:
        last_state = ""
        while True:
            time.sleep(1)
            state = device.get_property_value("SessionState")
            if state != last_state:
                print("state: %s" % state, flush=True)
                last_state = state
    except KeyboardInterrupt:
        print("shutting down")


if __name__ == "__main__":
    main()
