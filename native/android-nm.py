#!/usr/bin/env python3
# /usr/local/bin/android-nm: org.freedesktop.NetworkManager on the session's system bus, fed by Android. Apps cannot
# see the network interfaces, so the app (SessionService) writes the active network to $XDG_RUNTIME_DIR/network:
#   "wifi <strength 0-100> <ssid...>" | "wired" | "none"
# and this serves the minimum Quickshell's Networking (Omarchy's bar and network panel) reads: one Wi-Fi device with
# its access point, saved connection and active connection, and one wired device (mobile data, USB, Ethernet: Quickshell
# knows no other kind). Read-only: connecting to another network is done in Android.
import os
import uuid
import warnings

import gi

warnings.simplefilter("ignore", DeprecationWarning)  # Gio.DBusConnection.register_object: still the simplest form
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

FILE = os.path.join(os.environ["XDG_RUNTIME_DIR"], "network")
NM = "/org/freedesktop/NetworkManager"
WIFI, WIRED = NM + "/Devices/1", NM + "/Devices/2"
P = "org.freedesktop.NetworkManager"

XML = f"""<node>
 <interface name="{P}">
  <method name="GetDevices"><arg type="ao" direction="out"/></method>
  <method name="GetAllDevices"><arg type="ao" direction="out"/></method>
  <method name="CheckConnectivity"><arg type="u" direction="out"/></method>
  <signal name="DeviceAdded"><arg type="o"/></signal>
  <signal name="DeviceRemoved"><arg type="o"/></signal>
  <signal name="StateChanged"><arg type="u"/></signal>
  <property name="WirelessEnabled" type="b" access="readwrite"/>
  <property name="WirelessHardwareEnabled" type="b" access="read"/>
  <property name="Connectivity" type="u" access="read"/>
  <property name="ConnectivityCheckAvailable" type="b" access="read"/>
  <property name="ConnectivityCheckEnabled" type="b" access="readwrite"/>
  <property name="State" type="u" access="read"/>
  <property name="PrimaryConnection" type="o" access="read"/>
  <property name="ActiveConnections" type="ao" access="read"/>
  <property name="NetworkingEnabled" type="b" access="read"/>
  <property name="Version" type="s" access="read"/>
 </interface>
 <interface name="{P}.Device">
  <method name="Disconnect"/>
  <signal name="StateChanged"><arg type="u"/><arg type="u"/><arg type="u"/></signal>
  <property name="DeviceType" type="u" access="read"/>
  <property name="Interface" type="s" access="read"/>
  <property name="HwAddress" type="s" access="read"/>
  <property name="Managed" type="b" access="read"/>
  <property name="State" type="u" access="read"/>
  <property name="Autoconnect" type="b" access="readwrite"/>
  <property name="AvailableConnections" type="ao" access="read"/>
  <property name="ActiveConnection" type="o" access="read"/>
  <property name="InterfaceFlags" type="u" access="read"/>
 </interface>
 <interface name="{P}.Device.Wireless">
  <method name="GetAccessPoints"><arg type="ao" direction="out"/></method>
  <method name="GetAllAccessPoints"><arg type="ao" direction="out"/></method>
  <method name="RequestScan"><arg type="a{{sv}}"/></method>
  <signal name="AccessPointAdded"><arg type="o"/></signal>
  <signal name="AccessPointRemoved"><arg type="o"/></signal>
  <property name="LastScan" type="x" access="read"/>
  <property name="WirelessCapabilities" type="u" access="read"/>
  <property name="ActiveAccessPoint" type="o" access="read"/>
  <property name="Mode" type="u" access="read"/>
  <property name="AccessPoints" type="ao" access="read"/>
 </interface>
 <interface name="{P}.Device.Wired">
  <property name="Speed" type="u" access="read"/>
 </interface>
 <interface name="{P}.AccessPoint">
  <property name="Ssid" type="ay" access="read"/>
  <property name="Strength" type="y" access="read"/>
  <property name="Flags" type="u" access="read"/>
  <property name="WpaFlags" type="u" access="read"/>
  <property name="RsnFlags" type="u" access="read"/>
  <property name="Mode" type="u" access="read"/>
  <property name="Frequency" type="u" access="read"/>
 </interface>
 <interface name="{P}.Settings.Connection">
  <method name="GetSettings"><arg type="a{{sa{{sv}}}}" direction="out"/></method>
  <method name="GetSecrets"><arg type="s"/><arg type="a{{sa{{sv}}}}" direction="out"/></method>
  <signal name="Updated"/>
  <signal name="Removed"/>
 </interface>
 <interface name="{P}.Connection.Active">
  <signal name="StateChanged"><arg type="u"/><arg type="u"/></signal>
  <property name="Connection" type="o" access="read"/>
  <property name="State" type="u" access="read"/>
  <property name="Type" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Devices" type="ao" access="read"/>
 </interface>
</node>"""
INFO = {i.name: i for i in Gio.DBusNodeInfo.new_for_xml(XML).interfaces}

kind, strength, ssid = "none", 0, ""
gen = 0  # bumped per Wi-Fi network: new object paths, so Quickshell registers the new SSID
conn = None
registered = []


def paths():
    return {"ap": f"{NM}/AccessPoint/{gen}", "settings": f"{NM}/Settings/{gen}", "active": f"{NM}/ActiveConnection/{gen}",
            "wired_active": f"{NM}/ActiveConnection/wired"}


def read_state():
    global kind, strength, ssid
    try:
        with open(FILE) as f:
            parts = f.read().strip().split(" ", 2)
    except OSError:
        return
    if parts[0] == "wifi" and len(parts) >= 2:
        kind, strength, ssid = "wifi", max(0, min(100, int(parts[1]))), (parts[2] if len(parts) > 2 else "") or "Wi-Fi"
    elif parts[0] in ("wired", "none"):
        kind = parts[0]


def props(path, iface):
    p = paths()
    wifi_up, wired_up = kind == "wifi", kind == "wired"
    o = lambda s: GLib.Variant("o", s)
    if iface == P:
        primary = p["active"] if wifi_up else p["wired_active"] if wired_up else "/"
        return {
            "WirelessEnabled": GLib.Variant("b", True), "WirelessHardwareEnabled": GLib.Variant("b", True),
            "Connectivity": GLib.Variant("u", 4 if kind != "none" else 1),  # full / none
            "ConnectivityCheckAvailable": GLib.Variant("b", False), "ConnectivityCheckEnabled": GLib.Variant("b", False),
            "State": GLib.Variant("u", 70 if kind != "none" else 20),  # connected global / disconnected
            "PrimaryConnection": o(primary),
            "ActiveConnections": GLib.Variant("ao", [primary] if primary != "/" else []),
            "NetworkingEnabled": GLib.Variant("b", True), "Version": GLib.Variant("s", "1.52.0-android"),
        }
    if iface == P + ".Device":
        wifi = path == WIFI
        up = wifi_up if wifi else wired_up
        return {
            "DeviceType": GLib.Variant("u", 2 if wifi else 1),  # wifi / ethernet
            "Interface": GLib.Variant("s", "wlan0" if wifi else "android0"),
            "HwAddress": GLib.Variant("s", "00:00:00:00:00:0" + ("1" if wifi else "2")),
            "Managed": GLib.Variant("b", True), "State": GLib.Variant("u", 100 if up else 30),  # activated / disconnected
            "Autoconnect": GLib.Variant("b", True),
            "AvailableConnections": GLib.Variant("ao", [p["settings"]] if wifi and wifi_up else []),
            "ActiveConnection": o((p["active"] if wifi else p["wired_active"]) if up else "/"),
            "InterfaceFlags": GLib.Variant("u", 0x10003 if up else 0x1),  # up, lower up, carrier
        }
    if iface == P + ".Device.Wireless":
        aps = [p["ap"]] if wifi_up else []
        return {
            "LastScan": GLib.Variant("x", GLib.get_monotonic_time() // 1000), "WirelessCapabilities": GLib.Variant("u", 0x1ff),
            "ActiveAccessPoint": o(p["ap"] if wifi_up else "/"), "Mode": GLib.Variant("u", 2),
            "AccessPoints": GLib.Variant("ao", aps),
        }
    if iface == P + ".Device.Wired":
        return {"Speed": GLib.Variant("u", 0)}
    if iface == P + ".AccessPoint":
        return {
            "Ssid": GLib.Variant("ay", ssid.encode()), "Strength": GLib.Variant("y", strength),
            "Flags": GLib.Variant("u", 1), "WpaFlags": GLib.Variant("u", 0), "RsnFlags": GLib.Variant("u", 0x188),
            "Mode": GLib.Variant("u", 2), "Frequency": GLib.Variant("u", 5180),
        }
    if iface == P + ".Connection.Active":
        wired = path.endswith("/wired")
        return {
            "Connection": o("/" if wired else p["settings"]), "State": GLib.Variant("u", 2),  # activated
            "Type": GLib.Variant("s", "802-3-ethernet" if wired else "802-11-wireless"),
            "Id": GLib.Variant("s", "Android" if wired else ssid), "Devices": GLib.Variant("ao", [WIRED if wired else WIFI]),
        }
    return {}


def on_call(c, sender, path, iface, method, params, inv):
    p = paths()
    if method in ("GetDevices", "GetAllDevices"):
        inv.return_value(GLib.Variant("(ao)", ([WIFI, WIRED],)))
    elif method == "CheckConnectivity":
        inv.return_value(GLib.Variant("(u)", (4 if kind != "none" else 1,)))
    elif method in ("GetAccessPoints", "GetAllAccessPoints"):
        inv.return_value(GLib.Variant("(ao)", ([p["ap"]] if kind == "wifi" else [],)))
    elif method == "GetSettings":
        inv.return_value(GLib.Variant("(a{sa{sv}})", ({
            "connection": {"id": GLib.Variant("s", ssid), "uuid": GLib.Variant("s", str(uuid.uuid5(uuid.NAMESPACE_DNS, ssid))),
                           "type": GLib.Variant("s", "802-11-wireless")},
            "802-11-wireless": {"ssid": GLib.Variant("ay", ssid.encode()), "mode": GLib.Variant("s", "infrastructure")},
        },)))
    elif method == "GetSecrets":
        inv.return_value(GLib.Variant("(a{sa{sv}})", ({},)))
    else:  # Disconnect, RequestScan: Android owns the radios
        inv.return_value(None)


def on_get(c, sender, path, iface, prop):
    return props(path, iface).get(prop)


def on_set(c, sender, path, iface, prop, value):
    return True  # Wi-Fi switch and the like: Android's


def register_network_objects():
    p = paths()
    for path, ifaces in ((p["ap"], [P + ".AccessPoint"]), (p["settings"], [P + ".Settings.Connection"]),
                         (p["active"], [P + ".Connection.Active"])):
        for i in ifaces:
            registered.append(conn.register_object(path, INFO[i], on_call, on_get, on_set))


def emit_changed(path, iface):
    conn.emit_signal(None, path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                     GLib.Variant("(sa{sv}as)", (iface, props(path, iface), [])))


def update():
    global gen
    old = (kind, strength, ssid)
    read_state()
    if (kind, strength, ssid) == old:
        return True
    if kind == "wifi" and (old[0] != "wifi" or old[2] != ssid):  # another network: fresh objects for its SSID
        if old[0] == "wifi":
            conn.emit_signal(None, WIFI, P + ".Device.Wireless", "AccessPointRemoved", GLib.Variant("(o)", (paths()["ap"],)))
        gen += 1
        register_network_objects()
        conn.emit_signal(None, WIFI, P + ".Device.Wireless", "AccessPointAdded", GLib.Variant("(o)", (paths()["ap"],)))
    elif old[0] == "wifi" and kind != "wifi":
        conn.emit_signal(None, WIFI, P + ".Device.Wireless", "AccessPointRemoved", GLib.Variant("(o)", (paths()["ap"],)))
    if kind == "wifi":
        emit_changed(paths()["ap"], P + ".AccessPoint")
    for path, iface in ((WIFI, P + ".Device"), (WIFI, P + ".Device.Wireless"), (WIRED, P + ".Device"), (NM, P)):
        emit_changed(path, iface)
    return True


def on_bus(c, name):
    global conn
    conn = c
    c.register_object(NM, INFO[P], on_call, on_get, on_set)
    for path, ifaces in ((WIFI, [P + ".Device", P + ".Device.Wireless"]), (WIRED, [P + ".Device", P + ".Device.Wired"])):
        for i in ifaces:
            c.register_object(path, INFO[i], on_call, on_get, on_set)
    c.register_object(paths()["wired_active"], INFO[P + ".Connection.Active"], on_call, on_get, on_set)
    register_network_objects()
    GLib.timeout_add_seconds(3, update)


read_state()
Gio.bus_own_name(Gio.BusType.SYSTEM, P, Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
GLib.MainLoop().run()
