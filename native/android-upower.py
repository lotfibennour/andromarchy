#!/usr/bin/env python3
# /usr/local/bin/android-upower: org.freedesktop.UPower on the session's system bus, fed by Android. Apps cannot read
# /sys/class/power_supply, so the app (SessionService) writes "<percent> <charging 0|1>" to $XDG_RUNTIME_DIR/battery
# on every Android battery broadcast; this serves it as one battery, the minimum Quickshell's UPower (Omarchy's bar
# and power panel) reads.
import os
import warnings

warnings.simplefilter("ignore", DeprecationWarning)  # Gio.DBusConnection.register_object: still the simplest form

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

FILE = os.path.join(os.environ["XDG_RUNTIME_DIR"], "battery")
BAT = "/org/freedesktop/UPower/devices/battery_BAT0"
DISPLAY = "/org/freedesktop/UPower/devices/DisplayDevice"

XML = """<node>
 <interface name="org.freedesktop.UPower">
  <method name="EnumerateDevices"><arg type="ao" direction="out"/></method>
  <method name="GetDisplayDevice"><arg type="o" direction="out"/></method>
  <method name="GetCriticalAction"><arg type="s" direction="out"/></method>
  <signal name="DeviceAdded"><arg type="o"/></signal>
  <signal name="DeviceRemoved"><arg type="o"/></signal>
  <property name="DaemonVersion" type="s" access="read"/>
  <property name="OnBattery" type="b" access="read"/>
  <property name="LidIsClosed" type="b" access="read"/>
  <property name="LidIsPresent" type="b" access="read"/>
 </interface>
 <interface name="org.freedesktop.UPower.Device">
  <method name="Refresh"/>
  <property name="NativePath" type="s" access="read"/>
  <property name="Vendor" type="s" access="read"/>
  <property name="Model" type="s" access="read"/>
  <property name="Type" type="u" access="read"/>
  <property name="PowerSupply" type="b" access="read"/>
  <property name="IsPresent" type="b" access="read"/>
  <property name="IsRechargeable" type="b" access="read"/>
  <property name="Online" type="b" access="read"/>
  <property name="State" type="u" access="read"/>
  <property name="Percentage" type="d" access="read"/>
  <property name="Energy" type="d" access="read"/>
  <property name="EnergyFull" type="d" access="read"/>
  <property name="EnergyRate" type="d" access="read"/>
  <property name="TimeToEmpty" type="x" access="read"/>
  <property name="TimeToFull" type="x" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="WarningLevel" type="u" access="read"/>
  <property name="BatteryLevel" type="u" access="read"/>
  <property name="UpdateTime" type="t" access="read"/>
 </interface>
</node>"""

percent, charging = 100.0, True


def read_state():
    global percent, charging
    try:
        with open(FILE) as f:
            p, c = f.read().split()[:2]
        percent, charging = float(p), c == "1"
    except (OSError, ValueError):
        pass


def device_props():
    state = 4 if charging and percent >= 100 else 1 if charging else 2  # fully charged / charging / discharging
    level = int(percent // 10 * 10)
    icon = "battery-full" if percent >= 100 and charging else \
        f"battery-level-{level}{'-charging' if charging else ''}-symbolic"
    return {
        "NativePath": GLib.Variant("s", "android"),
        "Vendor": GLib.Variant("s", "Android"),
        "Model": GLib.Variant("s", "Battery"),
        "Type": GLib.Variant("u", 2),  # battery
        "PowerSupply": GLib.Variant("b", True),
        "IsPresent": GLib.Variant("b", True),
        "IsRechargeable": GLib.Variant("b", True),
        "Online": GLib.Variant("b", charging),
        "State": GLib.Variant("u", state),
        "Percentage": GLib.Variant("d", percent),
        "Energy": GLib.Variant("d", percent),
        "EnergyFull": GLib.Variant("d", 100.0),
        "EnergyRate": GLib.Variant("d", 0.0),
        "TimeToEmpty": GLib.Variant("x", 0),
        "TimeToFull": GLib.Variant("x", 0),
        "IconName": GLib.Variant("s", icon),
        "WarningLevel": GLib.Variant("u", 3 if percent <= 5 else 2 if percent <= 10 and not charging else 1),
        "BatteryLevel": GLib.Variant("u", 1),  # none: Percentage is exact
        "UpdateTime": GLib.Variant("t", int(GLib.get_real_time() / 1e6)),
    }


def daemon_props():
    return {
        "DaemonVersion": GLib.Variant("s", "1.90-android"),
        "OnBattery": GLib.Variant("b", not charging),
        "LidIsClosed": GLib.Variant("b", False),
        "LidIsPresent": GLib.Variant("b", False),
    }


def on_call(conn, sender, path, iface, method, params, inv):
    if method == "EnumerateDevices":
        inv.return_value(GLib.Variant("(ao)", ([BAT],)))
    elif method == "GetDisplayDevice":
        inv.return_value(GLib.Variant("(o)", (DISPLAY,)))
    elif method == "GetCriticalAction":
        inv.return_value(GLib.Variant("(s)", ("PowerOff",)))
    else:  # Refresh
        inv.return_value(None)


def on_get(conn, sender, path, iface, prop):
    return (daemon_props() if iface == "org.freedesktop.UPower" else device_props())[prop]


def changed(conn):
    for path, iface, props in ((BAT, "org.freedesktop.UPower.Device", device_props()),
                               (DISPLAY, "org.freedesktop.UPower.Device", device_props()),
                               ("/org/freedesktop/UPower", "org.freedesktop.UPower", daemon_props())):
        conn.emit_signal(None, path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         GLib.Variant("(sa{sv}as)", (iface, props, [])))


def on_bus(conn, name):
    info = Gio.DBusNodeInfo.new_for_xml(XML)
    conn.register_object("/org/freedesktop/UPower", info.interfaces[0], on_call, on_get, None)
    for path in (BAT, DISPLAY):
        conn.register_object(path, info.interfaces[1], on_call, on_get, None)

    def poll():  # the file is rewritten on each Android broadcast (about once a minute or on plug/unplug)
        old = (percent, charging)
        read_state()
        if (percent, charging) != old:
            changed(conn)
        return True

    GLib.timeout_add_seconds(5, poll)


read_state()
Gio.bus_own_name(Gio.BusType.SYSTEM, "org.freedesktop.UPower", Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
GLib.MainLoop().run()
