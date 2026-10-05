#!/usr/bin/env python3
# /usr/local/bin/android-session-services: session-bus services a desktop expects and nothing here provides
# (tools/dbus-probe.sh):
#   org.freedesktop.ScreenSaver  apps keeping the screen awake (video, calls): Omarchy's "stay awake" mode while any
#                                inhibitor is held (and only if it was not already on by the user's choice)
#   org.freedesktop.FileManager1 "show in folder" from browsers and editors: opens the folder with xdg-open
import os
import subprocess
import warnings

warnings.simplefilter("ignore", DeprecationWarning)

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

SCREENSAVER = """<node><interface name="org.freedesktop.ScreenSaver">
 <method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="u" direction="out"/></method>
 <method name="UnInhibit"><arg type="u" direction="in"/></method>
 <method name="GetActive"><arg type="b" direction="out"/></method>
 <method name="SetActive"><arg type="b" direction="in"/><arg type="b" direction="out"/></method>
 <method name="GetActiveTime"><arg type="u" direction="out"/></method>
 <method name="GetSessionIdleTime"><arg type="u" direction="out"/></method>
 <method name="SimulateUserActivity"/>
 <signal name="ActiveChanged"><arg type="b"/></signal>
</interface></node>"""

FILEMANAGER = """<node><interface name="org.freedesktop.FileManager1">
 <method name="ShowFolders"><arg type="as" direction="in"/><arg type="s" direction="in"/></method>
 <method name="ShowItems"><arg type="as" direction="in"/><arg type="s" direction="in"/></method>
 <method name="ShowItemProperties"><arg type="as" direction="in"/><arg type="s" direction="in"/></method>
</interface></node>"""

STAY_AWAKE = os.path.expanduser("~/.local/state/omarchy/indicators/stay-awake")
inhibitors = {}  # cookie -> sender
next_cookie = [1]
we_set_it = [False]


def apply_inhibit():
    held = bool(inhibitors)
    if held and not os.path.exists(STAY_AWAKE):
        subprocess.Popen(["omarchy-toggle-idle", "stay-awake"])
        we_set_it[0] = True
    elif not held and we_set_it[0]:
        subprocess.Popen(["omarchy-toggle-idle", "allow-idle"])
        we_set_it[0] = False


def on_screensaver(conn, sender, path, iface, method, params, inv):
    if method == "Inhibit":
        cookie = next_cookie[0]
        next_cookie[0] += 1
        inhibitors[cookie] = sender
        apply_inhibit()
        inv.return_value(GLib.Variant("(u)", (cookie,)))
    elif method == "UnInhibit":
        inhibitors.pop(params.unpack()[0], None)
        apply_inhibit()
        inv.return_value(None)
    elif method in ("GetActive", "SetActive"):
        inv.return_value(GLib.Variant("(b)", (False,)))
    elif method in ("GetActiveTime", "GetSessionIdleTime"):
        inv.return_value(GLib.Variant("(u)", (0,)))
    else:
        inv.return_value(None)


def open_paths(uris, parent):
    for uri in uris:
        path = Gio.File.new_for_uri(uri).get_path() if "://" in uri else uri
        if path:
            subprocess.Popen(["xdg-open", os.path.dirname(path) if parent else path])


def on_filemanager(conn, sender, path, iface, method, params, inv):
    uris = params.unpack()[0]
    open_paths(uris, parent=(method != "ShowFolders"))
    inv.return_value(None)


def on_bus(conn, name):
    ss = Gio.DBusNodeInfo.new_for_xml(SCREENSAVER).interfaces[0]
    for path in ("/org/freedesktop/ScreenSaver", "/ScreenSaver"):  # both are in use
        conn.register_object(path, ss, on_screensaver, None, None)
    fm = Gio.DBusNodeInfo.new_for_xml(FILEMANAGER).interfaces[0]
    conn.register_object("/org/freedesktop/FileManager1", fm, on_filemanager, None, None)

    # An app that exits without UnInhibit (crash, killed tab) must not keep the screen awake forever.
    def owner_changed(conn, sender, path, iface, signal, params):
        name, old, new = params.unpack()
        if old and not new:
            for cookie in [c for c, s in inhibitors.items() if s == name]:
                del inhibitors[cookie]
            apply_inhibit()

    conn.signal_subscribe("org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                          "/org/freedesktop/DBus", None, Gio.DBusSignalFlags.NONE, owner_changed)


for bus_name in ("org.freedesktop.ScreenSaver", "org.freedesktop.FileManager1"):
    Gio.bus_own_name(Gio.BusType.SESSION, bus_name, Gio.BusNameOwnerFlags.NONE,
                     on_bus if bus_name == "org.freedesktop.ScreenSaver" else None, None, None)
GLib.MainLoop().run()
