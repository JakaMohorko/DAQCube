"""
DAQ Jam client launcher.

Connects to a DAQ Jam host as one of the players. One local Player FB does
both halves of the job in a single window: it captures the keyboard while
focused and replays the game video.

  - the Player FB's State signal attaches to this player's channel input
    port on the host (client-to-device streaming)
  - the host channel's per-player video signal feeds the Player FB's port
  - player1 also starts the game on connect (--no-start to skip) and stops
    it on exit

Focus the Player window to play. Stop with Ctrl+C.

Usage:
  python client.py --user player1 [--password <lobby pw>]
                   [--server daq.nd://<host-ip>] [--modules <dir>]
                   [--no-start] [--seconds <n>]
"""

import argparse
import ipaddress
import socket
import time

import opendaq


def find_by_name(components, name):
    for component in components:
        if component.name == name:
            return component
    return None


def local_ipv4_addresses():
    addresses = set()
    try:
        for entry in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addresses.add(entry[4][0])
    except OSError:
        pass
    return addresses


def ranked_connection_strings(device_info):
    """Rank the advertised addresses by likely reachability.

    A host with several network adapters (VM switches, link-local) advertises
    all of them in arbitrary order; connecting to the opaque daq:// id may
    resolve to an unreachable one. Prefer IPv4 addresses that share a subnet
    with this PC, then other routable IPv4s, and skip link-local noise. The
    caller still tries them in order - a shared virtual-switch subnet (WSL and
    Hyper-V ranges collide between PCs) can rank an unreachable address first.
    """
    candidates = []
    for capability in device_info.server_capabilities:
        if capability.protocol_name != "OpenDAQNativeConfiguration":
            continue
        for conn in capability.connection_strings:
            host = conn.split("://", 1)[1].rstrip("/").rsplit(":", 1)[0]
            try:
                address = ipaddress.ip_address(host)
            except ValueError:
                continue  # bracketed IPv6 etc.
            if address.version != 4 or address.is_link_local:
                continue
            candidates.append((host, conn))

    local = local_ipv4_addresses()

    def same_prefix(a, b, prefix):
        network = ipaddress.ip_network(f"{a}/{prefix}", strict=False)
        return ipaddress.ip_address(b) in network

    def rank(entry):
        host = entry[0]
        if any(same_prefix(l, host, 24) for l in local):
            return 0  # same /24 as one of our own interfaces
        if any(same_prefix(l, host, 16) for l in local):
            return 1
        return 2

    candidates.sort(key=rank)
    return [conn for _, conn in candidates] or [device_info.connection_string]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--user", required=True, help="player1..player4 or admin")
    parser.add_argument("--password", default="daqjam", help="lobby password")
    parser.add_argument("--server", default="", help="host connection string (default: discover the DAQCube host via mDNS)")
    parser.add_argument("--modules", default="", help="extra module directory (e.g. the repo's build/bin)")
    parser.add_argument("--no-start", action="store_true", help="player1: do not start the game on connect")
    parser.add_argument("--seconds", type=int, default=0, help="exit after N seconds (0 = run until Ctrl+C)")
    args = parser.parse_args()

    builder = opendaq.InstanceBuilder()
    if args.modules:
        builder.add_module_path(args.modules)
    # client-to-device streaming registers local signals on the server by
    # their global string id, which starts with the root device's local id -
    # it must be unique per client or the second player's keyboard collides
    builder.default_root_device_local_id = "daqjam-" + args.user
    instance = builder.build()

    if args.server:
        servers = [args.server]
    else:
        print("discovering the DAQCube host...", flush=True)
        found = [info for info in instance.available_devices if info.name == "DAQCube"]
        if not found:
            raise SystemExit("no DAQCube host found via mDNS - pass --server daq.nd://<host-ip>")
        servers = ranked_connection_strings(found[0])
        print("found host (serial %s), %d advertised addresses" % (found[0].serial_number, len(servers)), flush=True)

    config = instance.create_default_add_device_config()
    general = opendaq.IPropertyObject.cast_from(config.get_property_value("General"))
    general.set_property_value("Username", args.user)
    general.set_property_value("Password", args.password)

    device = None
    for server in servers:
        try:
            print("connecting to %s ..." % server, flush=True)
            device = instance.add_device(server, config)
            break
        except Exception as error:
            print("  failed: %s" % error, flush=True)
    if device is None:
        raise SystemExit("could not reach the host on any advertised address")
    print("connected to %s as %s" % (device.name, args.user), flush=True)

    # this player's channel is the only one the host lets this user see
    channel = find_by_name(device.channels, "Player" + args.user.removeprefix("player")) \
        if args.user.startswith("player") else (device.channels[0] if len(device.channels) else None)
    print("visible player channels:", [c.name for c in device.channels], flush=True)

    # one local Player FB: keyboard capture + video replay in a single window.
    # It has no signals until PlayerTag is set - the tag lands in the signal
    # ids, which must be globally unique on the host (this backs up the unique
    # root device id set above). The video input port only exists once
    # EnableVideo is set (capture-only is the FB default).
    player = instance.add_function_block("Player")
    player.set_property_value("PlayerTag", args.user)
    player.set_property_value("EnableVideo", True)
    state_signal = find_by_name(player.signals, "State")
    if channel is not None:
        channel.input_ports[0].connect(state_signal)
        player.input_ports[0].connect(channel.signals[0])
        print("keyboard attached to %s, video feed connected" % channel.name, flush=True)

        # game audio -> this PC's speakers, modeled as a local AudioOutput
        # device (best effort - boxes without audio hardware just skip it)
        try:
            audio_signal = find_by_name(channel.signals, "Audio")
            if audio_signal is not None:
                audio_out = instance.add_device("daqaudioout://default")
                audio_out.channels[0].input_ports[0].connect(audio_signal)
                print("audio feed connected to the default playback device", flush=True)
        except Exception as e:
            print("no audio output: %s" % e, flush=True)

    # only player1 (and admin) can reach Start on the Settings object
    if args.user in ("player1", "admin") and not args.no_start:
        settings = opendaq.IPropertyObject.cast_from(device.get_property_value("Settings"))
        if device.get_property_value("SessionState") != "Running":
            opendaq.IProcedure.cast_from(settings.get_property_value("Start"))()
        print("game state: %s" % device.get_property_value("SessionState"), flush=True)

    print("focus the Player window to play (Enter = join/start, Space = bomb, WASD/arrows = move)", flush=True)

    deadline = time.monotonic() + args.seconds if args.seconds else None
    try:
        while deadline is None or time.monotonic() < deadline:
            time.sleep(2)
            print("state=%s decoded=%d" % (device.get_property_value("SessionState"),
                                           player.get_property_value("FramesDecoded")), flush=True)
    except KeyboardInterrupt:
        pass

    if args.user in ("player1", "admin") and not args.no_start:
        settings = opendaq.IPropertyObject.cast_from(device.get_property_value("Settings"))
        opendaq.IProcedure.cast_from(settings.get_property_value("Stop"))()
        print("game stopped")


if __name__ == "__main__":
    main()
