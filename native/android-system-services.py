#!/usr/bin/env python3
# /usr/local/bin/android-system-services: the system-bus services a desktop expects that cannot run here (systemd,
# polkitd, geoclue, power-profiles-daemon... need root, logind or hardware) as stand-ins answering from Android's state
# (tools/dbus-probe.sh lists what a desktop looks for):
#   login1        sessions, power actions (via android-request), sleep/idle inhibitors
#   hostname1     the machine's identity (Android's model), timedate1 (Android's zone), locale1 (the session's)
#   PolicyKit1    every action authorized: the user is already (fake) root
#   PowerProfiles one "balanced" profile (both bus names)
#   Accounts      the user (name from git, avatar ~/.face)
#   RealtimeKit1  accepts PipeWire's requests (no CAP_SYS_NICE for an app: nothing to gain, warnings gone)
#   GeoClue2      location from Android ($XDG_RUNTIME_DIR/location, written by the app)
# Settings that Android owns (time, zone, hostname...) are refused with a clear error.
import os
import platform
import subprocess
import warnings

warnings.simplefilter("ignore", DeprecationWarning)

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

RUN = os.environ["XDG_RUNTIME_DIR"]
V = GLib.Variant
NOT_HERE = "org.freedesktop.DBus.Error.NotSupported"


def model():
    return os.environ.get("ANDROMARCHY_MODEL", "Android device")


def vendor():
    return os.environ.get("ANDROMARCHY_VENDOR", "Android")


def os_name():
    try:
        for line in open("/etc/os-release"):
            if line.startswith("PRETTY_NAME="):
                return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return "Linux"


def git_name():
    try:
        return subprocess.run(["git", "config", "--global", "user.name"], capture_output=True, text=True,
                               timeout=2).stdout.strip() or "root"
    except (OSError, subprocess.TimeoutExpired):
        return "root"


class Obj:
    """One object: path, interface XML, properties (dict or callable), methods {name: fn(params, inv, sender)}."""

    def __init__(self, path, xml, props=None, methods=None, setters=None):
        self.path, self.props, self.methods, self.setters = path, props or {}, methods or {}, setters or {}
        self.iface = Gio.DBusNodeInfo.new_for_xml(f"<node>{xml}</node>").interfaces[0]

    def get(self):
        return self.props() if callable(self.props) else self.props

    def register(self, conn):
        def call(conn, sender, path, iface, method, params, inv):
            fn = self.methods.get(method)
            if fn is None:
                inv.return_value(None)
            else:
                fn(params, inv, sender)

        def get(conn, sender, path, iface, prop):
            return self.get()[prop]

        def set_(conn, sender, path, iface, prop, value):
            if prop in self.setters:
                self.setters[prop](value)
                return True
            return False

        conn.register_object(self.path, self.iface, call, get, set_)


def ret(sig, *values):
    return lambda params, inv, sender: inv.return_value(V(sig, values))


def ok(params, inv, sender):
    inv.return_value(None)


def refuse(what):
    return lambda params, inv, sender: inv.return_dbus_error(NOT_HERE, f"{what} is set in Android's settings")


def power(action):
    def fn(params, inv, sender):
        subprocess.Popen(["android-request", "power", action])
        inv.return_value(None)
    return fn


# ---- login1 ----
inhibitors = []  # (what, who, why, mode, uid, pid, read_fd)


def inhibit(params, inv, sender):
    what, who, why, mode = params.unpack()
    r, w = os.pipe()
    entry = (what, who, why, mode, 0, 0, r)
    inhibitors.append(entry)
    if "idle" in what.split(":"):
        subprocess.Popen(["omarchy-toggle-idle", "stay-awake"])

    def closed(fd, cond):  # the holder closed its end: inhibitor released
        if entry in inhibitors:
            inhibitors.remove(entry)
            if "idle" in what.split(":") and not any("idle" in e[0].split(":") for e in inhibitors):
                subprocess.Popen(["omarchy-toggle-idle", "allow-idle"])
        os.close(fd)
        return False

    GLib.unix_fd_add_full(GLib.PRIORITY_DEFAULT, r, GLib.IOCondition.HUP | GLib.IOCondition.ERR, closed)
    fds = Gio.UnixFDList.new()
    idx = fds.append(w)
    os.close(w)
    inv.return_value_with_unix_fd_list(V("(h)", (idx,)), fds)


SESSION = "/org/freedesktop/login1/session/_31"
SEAT = "/org/freedesktop/login1/seat/seat0"
USER = "/org/freedesktop/login1/user/_0"
LOGIN_MANAGER = """<interface name="org.freedesktop.login1.Manager">
 <method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
  <arg type="s" direction="in"/><arg type="h" direction="out"/></method>
 <method name="ListInhibitors"><arg type="a(ssssuu)" direction="out"/></method>
 <method name="ListSessions"><arg type="a(susso)" direction="out"/></method>
 <method name="ListUsers"><arg type="a(uso)" direction="out"/></method>
 <method name="ListSeats"><arg type="a(so)" direction="out"/></method>
 <method name="GetSession"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
 <method name="GetSessionByPID"><arg type="u" direction="in"/><arg type="o" direction="out"/></method>
 <method name="GetUser"><arg type="u" direction="in"/><arg type="o" direction="out"/></method>
 <method name="GetSeat"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
 <method name="PowerOff"><arg type="b" direction="in"/></method>
 <method name="Reboot"><arg type="b" direction="in"/></method>
 <method name="Suspend"><arg type="b" direction="in"/></method>
 <method name="Hibernate"><arg type="b" direction="in"/></method>
 <method name="HybridSleep"><arg type="b" direction="in"/></method>
 <method name="SuspendThenHibernate"><arg type="b" direction="in"/></method>
 <method name="CanPowerOff"><arg type="s" direction="out"/></method>
 <method name="CanReboot"><arg type="s" direction="out"/></method>
 <method name="CanSuspend"><arg type="s" direction="out"/></method>
 <method name="CanHibernate"><arg type="s" direction="out"/></method>
 <method name="CanHybridSleep"><arg type="s" direction="out"/></method>
 <method name="CanSuspendThenHibernate"><arg type="s" direction="out"/></method>
 <method name="LockSessions"/>
 <method name="UnlockSessions"/>
 <signal name="PrepareForSleep"><arg type="b"/></signal>
 <signal name="PrepareForShutdown"><arg type="b"/></signal>
 <signal name="SessionNew"><arg type="s"/><arg type="o"/></signal>
 <signal name="SessionRemoved"><arg type="s"/><arg type="o"/></signal>
 <property name="IdleHint" type="b" access="read"/>
 <property name="BlockInhibited" type="s" access="read"/>
 <property name="DelayInhibited" type="s" access="read"/>
 <property name="Docked" type="b" access="read"/>
 <property name="LidClosed" type="b" access="read"/>
 <property name="OnExternalPower" type="b" access="read"/>
 <property name="HandleLidSwitch" type="s" access="read"/>
 <property name="PreparingForSleep" type="b" access="read"/>
 <property name="PreparingForShutdown" type="b" access="read"/>
</interface>"""

LOGIN_SESSION = """<interface name="org.freedesktop.login1.Session">
 <method name="Activate"/><method name="Lock"/><method name="Unlock"/>
 <method name="SetIdleHint"><arg type="b" direction="in"/></method>
 <method name="SetLockedHint"><arg type="b" direction="in"/></method>
 <method name="SetType"><arg type="s" direction="in"/></method>
 <signal name="Lock"/><signal name="Unlock"/>
 <property name="Id" type="s" access="read"/>
 <property name="Name" type="s" access="read"/>
 <property name="User" type="(uo)" access="read"/>
 <property name="Seat" type="(so)" access="read"/>
 <property name="Type" type="s" access="read"/>
 <property name="Class" type="s" access="read"/>
 <property name="Desktop" type="s" access="read"/>
 <property name="State" type="s" access="read"/>
 <property name="Active" type="b" access="read"/>
 <property name="Remote" type="b" access="read"/>
 <property name="IdleHint" type="b" access="read"/>
 <property name="LockedHint" type="b" access="read"/>
 <property name="TTY" type="s" access="read"/>
 <property name="Leader" type="u" access="read"/>
</interface>"""

LOGIN_SEAT = """<interface name="org.freedesktop.login1.Seat">
 <property name="Id" type="s" access="read"/>
 <property name="ActiveSession" type="(so)" access="read"/>
 <property name="CanGraphical" type="b" access="read"/>
 <property name="CanTTY" type="b" access="read"/>
 <property name="Sessions" type="a(so)" access="read"/>
</interface>"""

LOGIN_USER = """<interface name="org.freedesktop.login1.User">
 <property name="UID" type="u" access="read"/>
 <property name="Name" type="s" access="read"/>
 <property name="State" type="s" access="read"/>
 <property name="Sessions" type="a(so)" access="read"/>
 <property name="Display" type="(so)" access="read"/>
 <property name="RuntimePath" type="s" access="read"/>
</interface>"""


def on_power():
    try:
        return open(os.path.join(RUN, "battery")).read().split()[1] == "1"
    except (OSError, IndexError):
        return True


login = [
    Obj("/org/freedesktop/login1", LOGIN_MANAGER, lambda: {
        "IdleHint": V("b", False), "BlockInhibited": V("s", ":".join(sorted({w for e in inhibitors
                                                                             for w in e[0].split(":") if e[3] == "block"}))),
        "DelayInhibited": V("s", ""), "Docked": V("b", False), "LidClosed": V("b", False),
        "OnExternalPower": V("b", on_power()), "HandleLidSwitch": V("s", "ignore"),
        "PreparingForSleep": V("b", False), "PreparingForShutdown": V("b", False)}, {
        "Inhibit": inhibit,
        "ListInhibitors": lambda p, inv, s: inv.return_value(V("(a(ssssuu))", ([e[:6] for e in inhibitors],))),
        "ListSessions": ret("(a(susso))", [("1", 0, "root", "seat0", SESSION)]),
        "ListUsers": ret("(a(uso))", [(0, "root", USER)]),
        "ListSeats": ret("(a(so))", [("seat0", SEAT)]),
        "GetSession": ret("(o)", SESSION), "GetSessionByPID": ret("(o)", SESSION),
        "GetUser": ret("(o)", USER), "GetSeat": ret("(o)", SEAT),
        "PowerOff": power("shutdown"), "Reboot": power("reboot"), "Suspend": power("suspend"),
        "Hibernate": power("suspend"), "HybridSleep": power("suspend"), "SuspendThenHibernate": power("suspend"),
        "CanPowerOff": ret("(s)", "yes"), "CanReboot": ret("(s)", "yes"), "CanSuspend": ret("(s)", "yes"),
        "CanHibernate": ret("(s)", "na"), "CanHybridSleep": ret("(s)", "na"),
        "CanSuspendThenHibernate": ret("(s)", "na"),
        "LockSessions": lambda p, inv, s: (subprocess.Popen(["omarchy-lock-screen"]), inv.return_value(None)),
    }),
    Obj(SESSION, LOGIN_SESSION, lambda: {
        "Id": V("s", "1"), "Name": V("s", "root"), "User": V("(uo)", (0, USER)), "Seat": V("(so)", ("seat0", SEAT)),
        "Type": V("s", "wayland"), "Class": V("s", "user"), "Desktop": V("s", "Hyprland"), "State": V("s", "active"),
        "Active": V("b", True), "Remote": V("b", False), "IdleHint": V("b", False), "LockedHint": V("b", False),
        "TTY": V("s", ""), "Leader": V("u", 1)}, {
        "Lock": lambda p, inv, s: (subprocess.Popen(["omarchy-lock-screen"]), inv.return_value(None))}),
    Obj(SEAT, LOGIN_SEAT, lambda: {
        "Id": V("s", "seat0"), "ActiveSession": V("(so)", ("1", SESSION)), "CanGraphical": V("b", True),
        "CanTTY": V("b", False), "Sessions": V("a(so)", [("1", SESSION)])}),
    Obj(USER, LOGIN_USER, lambda: {
        "UID": V("u", 0), "Name": V("s", "root"), "State": V("s", "active"), "Sessions": V("a(so)", [("1", SESSION)]),
        "Display": V("(so)", ("1", SESSION)), "RuntimePath": V("s", RUN)}),
]

# ---- hostname1, timedate1, locale1 ----
HOSTNAME = """<interface name="org.freedesktop.hostname1">
 <method name="SetHostname"><arg type="s" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetStaticHostname"><arg type="s" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetPrettyHostname"><arg type="s" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetIconName"><arg type="s" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetChassis"><arg type="s" direction="in"/><arg type="b" direction="in"/></method>
 <method name="GetProductUUID"><arg type="b" direction="in"/><arg type="ay" direction="out"/></method>
 <property name="Hostname" type="s" access="read"/><property name="StaticHostname" type="s" access="read"/>
 <property name="PrettyHostname" type="s" access="read"/><property name="IconName" type="s" access="read"/>
 <property name="Chassis" type="s" access="read"/><property name="Deployment" type="s" access="read"/>
 <property name="Location" type="s" access="read"/><property name="KernelName" type="s" access="read"/>
 <property name="KernelRelease" type="s" access="read"/><property name="KernelVersion" type="s" access="read"/>
 <property name="OperatingSystemPrettyName" type="s" access="read"/>
 <property name="OperatingSystemCPEName" type="s" access="read"/><property name="HomeURL" type="s" access="read"/>
 <property name="HardwareVendor" type="s" access="read"/><property name="HardwareModel" type="s" access="read"/>
</interface>"""

TIMEDATE = """<interface name="org.freedesktop.timedate1">
 <method name="SetTime"><arg type="x" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetTimezone"><arg type="s" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetLocalRTC"><arg type="b" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetNTP"><arg type="b" direction="in"/><arg type="b" direction="in"/></method>
 <method name="ListTimezones"><arg type="as" direction="out"/></method>
 <property name="Timezone" type="s" access="read"/><property name="LocalRTC" type="b" access="read"/>
 <property name="CanNTP" type="b" access="read"/><property name="NTP" type="b" access="read"/>
 <property name="NTPSynchronized" type="b" access="read"/><property name="TimeUSec" type="t" access="read"/>
 <property name="RTCTimeUSec" type="t" access="read"/>
</interface>"""

LOCALE = """<interface name="org.freedesktop.locale1">
 <method name="SetLocale"><arg type="as" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetVConsoleKeyboard"><arg type="s" direction="in"/><arg type="s" direction="in"/>
  <arg type="b" direction="in"/><arg type="b" direction="in"/></method>
 <method name="SetX11Keyboard"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
  <arg type="s" direction="in"/><arg type="b" direction="in"/><arg type="b" direction="in"/></method>
 <property name="Locale" type="as" access="read"/><property name="VConsoleKeymap" type="s" access="read"/>
 <property name="VConsoleKeymapToggle" type="s" access="read"/><property name="X11Layout" type="s" access="read"/>
 <property name="X11Model" type="s" access="read"/><property name="X11Variant" type="s" access="read"/>
 <property name="X11Options" type="s" access="read"/>
</interface>"""


def timezones():
    try:
        return sorted(line.split("\t")[2].strip() for line in open(
                                                                   "/usr/share/zoneinfo/zone1970.tab")
                      if line and not line.startswith("#"))
    except OSError:
        return []


def kb_layout():  # the last kb_layout set in input.lua wins (Omarchy's defaults come first, ours are appended)
    layout = "us"
    try:
        for line in open(os.path.expanduser("~/.config/hypr/input.lua")):
            if "kb_layout" in line and '"' in line and not line.lstrip().startswith("--"):
                layout = line.split("kb_layout", 1)[1].split('"')[1]
    except (OSError, IndexError):
        pass
    return layout


u = platform.uname()
misc = [
    Obj("/org/freedesktop/hostname1", HOSTNAME, lambda: {
        "Hostname": V("s", u.node), "StaticHostname": V("s", u.node), "PrettyHostname": V("s", model()),
        "IconName": V("s", "computer-tablet"), "Chassis": V("s", "tablet"), "Deployment": V("s", ""),
        "Location": V("s", ""), "KernelName": V("s", u.system), "KernelRelease": V("s", u.release),
        "KernelVersion": V("s", u.version), "OperatingSystemPrettyName": V("s", os_name()),
        "OperatingSystemCPEName": V("s", ""), "HomeURL": V("s", "https://omarchy.org"),
        "HardwareVendor": V("s", vendor()), "HardwareModel": V("s", model())}, {
        **{m: refuse("The device name") for m in ("SetHostname", "SetStaticHostname", "SetPrettyHostname",
                                                    "SetIconName", "SetChassis")},
        "GetProductUUID": lambda p, inv, s: inv.return_dbus_error(NOT_HERE, "no product UUID on Android")}),
    Obj("/org/freedesktop/timedate1", TIMEDATE, lambda: {
        "Timezone": V("s", os.environ.get("TZ", "UTC")), "LocalRTC": V("b", False), "CanNTP": V("b", False),
        "NTP": V("b", True), "NTPSynchronized": V("b", True), "TimeUSec": V("t", GLib.get_real_time()),
        "RTCTimeUSec": V("t", GLib.get_real_time())}, {
        "SetTime": refuse("The time"), "SetTimezone": refuse("The time zone"), "SetLocalRTC": refuse("The clock"),
        "SetNTP": refuse("Network time"),
        "ListTimezones": lambda p, inv, s: inv.return_value(V("(as)", (timezones(),)))}),
    Obj("/org/freedesktop/locale1", LOCALE, lambda: {
        "Locale": V("as", ["LANG=" + os.environ.get("LANG", "C.UTF-8")]), "VConsoleKeymap": V("s", ""),
        "VConsoleKeymapToggle": V("s", ""), "X11Layout": V("s", kb_layout()), "X11Model": V("s", "pc105"),
        "X11Variant": V("s", ""), "X11Options": V("s", "")}, {
        "SetLocale": refuse("The language"), "SetVConsoleKeyboard": refuse("The console keyboard"),
        "SetX11Keyboard": lambda p, inv, s: inv.return_dbus_error(
            NOT_HERE, "keyboard layouts are set in ~/.config/hypr/input.lua")}),
]

# ---- PolicyKit ----
POLKIT = """<interface name="org.freedesktop.PolicyKit1.Authority">
 <method name="CheckAuthorization"><arg type="(sa{sv})" direction="in"/><arg type="s" direction="in"/>
  <arg type="a{ss}" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
  <arg type="(bba{ss})" direction="out"/></method>
 <method name="CancelCheckAuthorization"><arg type="s" direction="in"/></method>
 <method name="EnumerateActions"><arg type="s" direction="in"/><arg type="a(ssssssuuua{ss})" direction="out"/></method>
 <method name="RegisterAuthenticationAgent"><arg type="(sa{sv})" direction="in"/><arg type="s" direction="in"/>
  <arg type="s" direction="in"/></method>
 <method name="RegisterAuthenticationAgentWithOptions"><arg type="(sa{sv})" direction="in"/><arg type="s" direction="in"/>
  <arg type="s" direction="in"/><arg type="a{sv}" direction="in"/></method>
 <method name="UnregisterAuthenticationAgent"><arg type="(sa{sv})" direction="in"/><arg type="s" direction="in"/></method>
 <signal name="Changed"/>
 <property name="BackendName" type="s" access="read"/><property name="BackendVersion" type="s" access="read"/>
 <property name="BackendFeatures" type="u" access="read"/>
</interface>"""
polkit = [Obj("/org/freedesktop/PolicyKit1/Authority", POLKIT, {
    "BackendName": V("s", "android"), "BackendVersion": V("s", "1"), "BackendFeatures": V("u", 0)}, {
    "CheckAuthorization": lambda p, inv, s: inv.return_value(V("((bba{ss}))", ((True, False, {}),))),
    "EnumerateActions": ret("(a(ssssssuuua{ss}))", [])})]

# ---- power profiles (power-profiles-daemon's two names) ----
active_profile = ["balanced"]


def profiles_props():
    return {"ActiveProfile": V("s", active_profile[0]), "PerformanceInhibited": V("s", ""),
            "PerformanceDegraded": V("s", ""), "Actions": V("as", []), "ActiveProfileHolds": V("aa{sv}", []),
            "Profiles": V("aa{sv}", [{"Profile": V("s", p), "Driver": V("s", "android"),
                                      "PlatformDriver": V("s", "android"), "CpuDriver": V("s", "android")}
                                     for p in ("power-saver", "balanced", "performance")])}


def set_profile(value):  # Android chooses the CPU governor; the choice is only remembered and shown
    active_profile[0] = value.unpack()


def profiles_xml(name):
    return f"""<interface name="{name}">
 <method name="HoldProfile"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
  <arg type="u" direction="out"/></method>
 <method name="ReleaseProfile"><arg type="u" direction="in"/></method>
 <signal name="ProfileReleased"><arg type="u"/></signal>
 <property name="ActiveProfile" type="s" access="readwrite"/><property name="PerformanceInhibited" type="s" access="read"/>
 <property name="PerformanceDegraded" type="s" access="read"/><property name="Profiles" type="aa{{sv}}" access="read"/>
 <property name="Actions" type="as" access="read"/><property name="ActiveProfileHolds" type="aa{{sv}}" access="read"/>
</interface>"""


profiles = {
    "net.hadess.PowerProfiles": Obj("/net/hadess/PowerProfiles", profiles_xml("net.hadess.PowerProfiles"),
                                    profiles_props, {"HoldProfile": ret("(u)", 1)}, {"ActiveProfile": set_profile}),
    "org.freedesktop.UPower.PowerProfiles": Obj(
        "/org/freedesktop/UPower/PowerProfiles", profiles_xml("org.freedesktop.UPower.PowerProfiles"),
        profiles_props, {"HoldProfile": ret("(u)", 1)}, {"ActiveProfile": set_profile}),
}

# ---- Accounts ----
ACCOUNTS = """<interface name="org.freedesktop.Accounts">
 <method name="ListCachedUsers"><arg type="ao" direction="out"/></method>
 <method name="FindUserById"><arg type="x" direction="in"/><arg type="o" direction="out"/></method>
 <method name="FindUserByName"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
 <signal name="UserAdded"><arg type="o"/></signal><signal name="UserDeleted"><arg type="o"/></signal>
 <property name="DaemonVersion" type="s" access="read"/><property name="HasNoUsers" type="b" access="read"/>
 <property name="HasMultipleUsers" type="b" access="read"/>
</interface>"""
ACCOUNT_USER = """<interface name="org.freedesktop.Accounts.User">
 <property name="Uid" type="t" access="read"/><property name="UserName" type="s" access="read"/>
 <property name="RealName" type="s" access="read"/><property name="AccountType" type="i" access="read"/>
 <property name="HomeDirectory" type="s" access="read"/><property name="Shell" type="s" access="read"/>
 <property name="Email" type="s" access="read"/><property name="Language" type="s" access="read"/>
 <property name="IconFile" type="s" access="read"/><property name="Locked" type="b" access="read"/>
 <property name="AutomaticLogin" type="b" access="read"/><property name="SystemAccount" type="b" access="read"/>
 <property name="LocalAccount" type="b" access="read"/><property name="PasswordMode" type="i" access="read"/>
</interface>"""
ACC_USER = "/org/freedesktop/Accounts/User0"
real_name = git_name()
accounts = [
    Obj("/org/freedesktop/Accounts", ACCOUNTS, {
        "DaemonVersion": V("s", "android"), "HasNoUsers": V("b", False), "HasMultipleUsers": V("b", False)}, {
        "ListCachedUsers": ret("(ao)", [ACC_USER]), "FindUserById": ret("(o)", ACC_USER),
        "FindUserByName": ret("(o)", ACC_USER)}),
    Obj(ACC_USER, ACCOUNT_USER, lambda: {
        "Uid": V("t", 0), "UserName": V("s", "root"), "RealName": V("s", real_name), "AccountType": V("i", 1),
        "HomeDirectory": V("s", os.path.expanduser("~")), "Shell": V("s", "/bin/bash"), "Email": V("s", ""),
        "Language": V("s", os.environ.get("LANG", "")), "IconFile": V("s", os.path.expanduser("~/.face")),
        "Locked": V("b", False), "AutomaticLogin": V("b", True), "SystemAccount": V("b", False),
        "LocalAccount": V("b", True), "PasswordMode": V("i", 0)}),
]

# ---- RealtimeKit ----
RTKIT = """<interface name="org.freedesktop.RealtimeKit1">
 <method name="MakeThreadRealtime"><arg type="t" direction="in"/><arg type="u" direction="in"/></method>
 <method name="MakeThreadRealtimeWithPID"><arg type="t" direction="in"/><arg type="t" direction="in"/>
  <arg type="u" direction="in"/></method>
 <method name="MakeThreadHighPriority"><arg type="t" direction="in"/><arg type="i" direction="in"/></method>
 <method name="MakeThreadHighPriorityWithPID"><arg type="t" direction="in"/><arg type="t" direction="in"/>
  <arg type="i" direction="in"/></method>
 <method name="ResetKnown"/><method name="ResetAll"/><method name="Exit"/>
 <property name="RTTimeUSecMax" type="x" access="read"/><property name="MaxRealtimePriority" type="i" access="read"/>
 <property name="MinNiceLevel" type="i" access="read"/>
</interface>"""
rtkit = [Obj("/org/freedesktop/RealtimeKit1", RTKIT, {
    "RTTimeUSecMax": V("x", 200000), "MaxRealtimePriority": V("i", 20), "MinNiceLevel": V("i", -15)})]

# ---- GeoClue2 ----
GEO_MANAGER = """<interface name="org.freedesktop.GeoClue2.Manager">
 <method name="GetClient"><arg type="o" direction="out"/></method>
 <method name="CreateClient"><arg type="o" direction="out"/></method>
 <method name="DeleteClient"><arg type="o" direction="in"/></method>
 <method name="AddAgent"><arg type="s" direction="in"/></method>
 <property name="InUse" type="b" access="read"/><property name="AvailableAccuracyLevel" type="u" access="read"/>
</interface>"""
GEO_CLIENT = """<interface name="org.freedesktop.GeoClue2.Client">
 <method name="Start"/><method name="Stop"/>
 <signal name="LocationUpdated"><arg type="o"/><arg type="o"/></signal>
 <property name="Location" type="o" access="read"/><property name="DistanceThreshold" type="u" access="readwrite"/>
 <property name="TimeThreshold" type="u" access="readwrite"/><property name="DesktopId" type="s" access="readwrite"/>
 <property name="RequestedAccuracyLevel" type="u" access="readwrite"/><property name="Active" type="b" access="read"/>
</interface>"""
GEO_LOCATION = """<interface name="org.freedesktop.GeoClue2.Location">
 <property name="Latitude" type="d" access="read"/><property name="Longitude" type="d" access="read"/>
 <property name="Accuracy" type="d" access="read"/><property name="Altitude" type="d" access="read"/>
 <property name="Speed" type="d" access="read"/><property name="Heading" type="d" access="read"/>
 <property name="Description" type="s" access="read"/><property name="Timestamp" type="(tt)" access="read"/>
</interface>"""
GEO_FILE = os.path.join(RUN, "location")  # "<lat> <lon> <accuracy m> <unix time>", from Android
geo = {"conn": None, "clients": {}, "n": 0, "loc": None, "locs": 0, "mtime": 0}


def read_location():
    try:
        st = os.stat(GEO_FILE)
        if st.st_mtime == geo["mtime"]:
            return False
        lat, lon, acc, t = open(GEO_FILE).read().split()[:4]
        geo["mtime"] = st.st_mtime
        geo["fix"] = (float(lat), float(lon), float(acc), int(float(t)))
        return True
    except (OSError, ValueError):
        return False


def publish_location():  # a new Location object per fix (GeoClue's contract), announced to active clients
    if "fix" not in geo:
        return
    lat, lon, acc, t = geo["fix"]
    geo["locs"] += 1
    path = f"/org/freedesktop/GeoClue2/Location/{geo['locs']}"
    Obj(path, GEO_LOCATION, {"Latitude": V("d", lat), "Longitude": V("d", lon), "Accuracy": V("d", acc),
                             "Altitude": V("d", -1.7976931348623157e308), "Speed": V("d", -1.0),
                             "Heading": V("d", -1.0), "Description": V("s", "Android"),
                             "Timestamp": V("(tt)", (t, 0))}).register(geo["conn"])
    old = geo["loc"] or "/"
    geo["loc"] = path
    for cpath, c in geo["clients"].items():
        if c["active"]:
            geo["conn"].emit_signal(None, cpath, "org.freedesktop.GeoClue2.Client", "LocationUpdated",
                                    V("(oo)", (old, path)))


def new_client(params, inv, sender):
    geo["n"] += 1
    path = f"/org/freedesktop/GeoClue2/Client/{geo['n']}"
    c = {"active": False, "props": {"DistanceThreshold": 0, "TimeThreshold": 0, "DesktopId": "",
                                    "RequestedAccuracyLevel": 8}}
    geo["clients"][path] = c

    def start(p, inv2, s):
        c["active"] = True
        if geo["loc"]:
            geo["conn"].emit_signal(None, path, "org.freedesktop.GeoClue2.Client", "LocationUpdated",
                                    V("(oo)", ("/", geo["loc"])))
        inv2.return_value(None)

    def stop(p, inv2, s):
        c["active"] = False
        inv2.return_value(None)

    setters = {k: (lambda key: lambda v: c["props"].__setitem__(key, v.unpack()))(k) for k in c["props"]}
    Obj(path, GEO_CLIENT, lambda: {
        "Location": V("o", geo["loc"] or "/"), "Active": V("b", c["active"]),
        "DistanceThreshold": V("u", c["props"]["DistanceThreshold"]),
        "TimeThreshold": V("u", c["props"]["TimeThreshold"]), "DesktopId": V("s", c["props"]["DesktopId"]),
        "RequestedAccuracyLevel": V("u", c["props"]["RequestedAccuracyLevel"])},
        {"Start": start, "Stop": stop}, setters).register(geo["conn"])
    inv.return_value(V("(o)", (path,)))


geoclue = [Obj("/org/freedesktop/GeoClue2/Manager", GEO_MANAGER, lambda: {
    "InUse": V("b", any(c["active"] for c in geo["clients"].values())),
    "AvailableAccuracyLevel": V("u", 6 if "fix" in geo else 0)}, {  # 6: street-level (Android network location)
    "GetClient": new_client, "CreateClient": new_client})]


# ---- serve ----
SERVICES = {
    "org.freedesktop.login1": login,
    "org.freedesktop.hostname1": misc[0:1],
    "org.freedesktop.timedate1": misc[1:2],
    "org.freedesktop.locale1": misc[2:3],
    "org.freedesktop.PolicyKit1": polkit,
    "net.hadess.PowerProfiles": [profiles["net.hadess.PowerProfiles"]],
    "org.freedesktop.UPower.PowerProfiles": [profiles["org.freedesktop.UPower.PowerProfiles"]],
    "org.freedesktop.Accounts": accounts,
    "org.freedesktop.RealtimeKit1": rtkit,
    "org.freedesktop.GeoClue2": geoclue,
}


def on_bus(conn, name):
    for obj in SERVICES[name]:
        obj.register(conn)
    if name == "org.freedesktop.GeoClue2":
        geo["conn"] = conn

        def poll():
            if read_location():
                publish_location()
            return True

        poll()
        GLib.timeout_add_seconds(10, poll)


for bus_name in SERVICES:
    Gio.bus_own_name(Gio.BusType.SYSTEM, bus_name, Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
GLib.MainLoop().run()
