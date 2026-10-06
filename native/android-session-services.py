#!/usr/bin/env python3
# /usr/local/bin/android-session-services: session-bus services a desktop expects and nothing here provides
# (tools/dbus-probe.sh):
#   org.freedesktop.ScreenSaver  apps keeping the screen awake (video, calls): Omarchy's "stay awake" mode while any
#                                inhibitor is held (and only if it was not already on by the user's choice)
#   org.freedesktop.FileManager1 "show in folder" from browsers and editors: opens the folder with xdg-open
#   org.freedesktop.portal.Documents  Flatpak apps' file sharing, without FUSE (see below)
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


# org.freedesktop.portal.Documents without FUSE (Android refuses /dev/fuse, the real xdg-document-portal cannot
# mount): the "mount point" is a plain directory of links, <id>/<name> -> the real file, and by-app/<app> points
# back to it (Flatpak binds that as the app's /run/user/<uid>/doc). Every app sees every document: our Flatpak
# runs without isolation anyway.
import hashlib

DOCUMENTS = """<node><interface name="org.freedesktop.portal.Documents">
 <method name="GetMountPoint"><arg type="ay" direction="out"/></method>
 <method name="Add"><arg type="h" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="in"/><arg type="s" direction="out"/></method>
 <method name="AddNamed"><arg type="h" direction="in"/><arg type="ay" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="in"/><arg type="s" direction="out"/></method>
 <method name="AddFull"><arg type="ah" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/><arg type="as" direction="out"/><arg type="a{sv}" direction="out"/></method>
 <method name="AddNamedFull"><arg type="h" direction="in"/><arg type="ay" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/><arg type="s" direction="out"/><arg type="a{sv}" direction="out"/></method>
 <method name="GrantPermissions"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/></method>
 <method name="RevokePermissions"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/></method>
 <method name="Delete"><arg type="s" direction="in"/></method>
 <method name="Lookup"><arg type="ay" direction="in"/><arg type="s" direction="out"/></method>
 <method name="Info"><arg type="s" direction="in"/><arg type="ay" direction="out"/><arg type="a{sas}" direction="out"/></method>
 <method name="List"><arg type="s" direction="in"/><arg type="a{say}" direction="out"/></method>
 <method name="GetHostPaths"><arg type="as" direction="in"/><arg type="a{say}" direction="out"/></method>
 <property name="version" type="u" access="read"/>
</interface></node>"""

DOC = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "doc")


def ay(s):
    return GLib.Variant("ay", s.encode() + b"\0")


def fd_path(fd):
    return os.readlink(f"/proc/self/fd/{fd}")  # through fakechroot: the path as programs see it


def doc_add(target):
    """A document for the file `target` (it may not exist yet: a "save as"). Same file, same id."""
    doc_id = hashlib.sha1(target.encode()).hexdigest()[:8]
    d = os.path.join(DOC, doc_id)
    os.makedirs(d, exist_ok=True)
    link = os.path.join(d, os.path.basename(target))
    if not os.path.islink(link):
        os.symlink(target, link)
    return doc_id


def doc_target(doc_id):
    d = os.path.join(DOC, doc_id)
    names = os.listdir(d) if os.path.isdir(d) else []
    return os.readlink(os.path.join(d, names[0])) if names else None


def doc_app(app_id):
    if app_id:
        os.makedirs(os.path.join(DOC, "by-app"), exist_ok=True)
        link = os.path.join(DOC, "by-app", app_id)
        if not os.path.lexists(link):
            os.symlink("..", link)


def on_documents(conn, sender, path, iface, method, params, inv):
    fds = inv.get_message().get_unix_fd_list()
    get = lambda i: fds.get(i)
    a = params.unpack() if params is not None else ()
    try:
        if method == "GetMountPoint":
            inv.return_value(GLib.Variant("(ay)", (DOC.encode() + b"\0",)))
        elif method == "Add":
            inv.return_value(GLib.Variant("(s)", (doc_add(fd_path(get(a[0]))),)))
        elif method == "AddNamed":
            name = bytes(a[1]).rstrip(b"\0").decode()
            inv.return_value(GLib.Variant("(s)", (doc_add(os.path.join(fd_path(get(a[0])), name)),)))
        elif method == "AddFull":
            doc_app(a[2])
            ids = [doc_add(fd_path(get(h))) for h in a[0]]
            inv.return_value(GLib.Variant("(asa{sv})", (ids, {"mountpoint": ay(DOC)})))
        elif method == "AddNamedFull":
            doc_app(a[3])
            name = bytes(a[1]).rstrip(b"\0").decode()
            doc_id = doc_add(os.path.join(fd_path(get(a[0])), name))
            inv.return_value(GLib.Variant("(sa{sv})", (doc_id, {"mountpoint": ay(DOC)})))
        elif method == "GrantPermissions":
            doc_app(a[1])
            inv.return_value(None)
        elif method == "Lookup":
            target = bytes(a[0]).rstrip(b"\0").decode()
            found = [i for i in (os.listdir(DOC) if os.path.isdir(DOC) else []) if i != "by-app" and doc_target(i) == target]
            inv.return_value(GLib.Variant("(s)", (found[0] if found else "",)))
        elif method == "Info":
            inv.return_value(GLib.Variant("(aya{sas})", ((doc_target(a[0]) or "").encode() + b"\0", {})))
        elif method == "List":
            docs = {i: (doc_target(i) or "").encode() + b"\0" for i in (os.listdir(DOC) if os.path.isdir(DOC) else []) if i != "by-app"}
            inv.return_value(GLib.Variant("(a{say})", (docs,)))
        elif method == "GetHostPaths":
            inv.return_value(GLib.Variant("(a{say})", ({i: (doc_target(i) or "").encode() + b"\0" for i in a[0]},)))
        elif method == "Delete":
            d = os.path.join(DOC, a[0])
            for n in os.listdir(d) if os.path.isdir(d) else []:
                os.unlink(os.path.join(d, n))
            if os.path.isdir(d):
                os.rmdir(d)
            inv.return_value(None)
        else:
            inv.return_value(None)
    except Exception as e:
        inv.return_dbus_error("org.freedesktop.portal.Error.Failed", str(e))


def on_documents_bus(conn, name):
    os.makedirs(DOC, exist_ok=True)
    for app in os.listdir(os.path.expanduser("~/.local/share/flatpak/app")) if os.path.isdir(os.path.expanduser("~/.local/share/flatpak/app")) else []:
        doc_app(app)
    docs = Gio.DBusNodeInfo.new_for_xml(DOCUMENTS).interfaces[0]
    conn.register_object("/org/freedesktop/portal/documents", docs, on_documents,
                         lambda *a: GLib.Variant("u", 5), None)


for bus_name in ("org.freedesktop.ScreenSaver", "org.freedesktop.FileManager1"):
    Gio.bus_own_name(Gio.BusType.SESSION, bus_name, Gio.BusNameOwnerFlags.NONE,
                     on_bus if bus_name == "org.freedesktop.ScreenSaver" else None, None, None)
Gio.bus_own_name(Gio.BusType.SESSION, "org.freedesktop.portal.Documents", Gio.BusNameOwnerFlags.REPLACE,
                 on_documents_bus, None, None)
GLib.MainLoop().run()
