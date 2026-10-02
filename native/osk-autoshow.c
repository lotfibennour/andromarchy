// osk-autoshow: shows Omarchy's on-screen keyboard when a text field gets focus and hides it when the focus leaves,
// like Android's soft keyboard.
//
// It registers as the Wayland input method (input-method-unstable-v2). When an app focuses a text field it enables
// text-input-v3 and the compositor activates the input method; when the field loses focus it is deactivated. Each
// state change (applied on "done") runs `omarchy-shell -q shell summon|hide <plugin>`. It never grabs the keyboard,
// so physical keys keep going to the focused app, and the plugin keeps typing through wtype. Stays hidden while the
// app reports a physical keyboard ($XDG_RUNTIME_DIR/hardware-keyboard).
// Apps without text-input (terminals: foot, kitty, Alacritty) do not trigger it: the bar icon still toggles it.
//
// Build: wayland-scanner client-header/private-code input-method-unstable-v2.xml, then
//        cc -O2 -o osk-autoshow osk-autoshow.c input-method-unstable-v2-protocol.c -lwayland-client
#define _GNU_SOURCE
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <wayland-client.h>

#include "input-method-unstable-v2-client-protocol.h"

#define PLUGIN "io.github.abdxdev.onscreen-keyboard"

extern char **environ;

static struct wl_seat *seat;
static struct zwp_input_method_manager_v2 *manager;
static bool pending_active, shown, unavailable;

static void run_shell(const char *method)
{
    char *argv[] = {"omarchy-shell", "-q", "shell", (char *)method, PLUGIN, NULL};
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0)
        waitpid(pid, NULL, 0);
}

static void on_activate(void *data, struct zwp_input_method_v2 *im)
{
    pending_active = true;
}

static void on_deactivate(void *data, struct zwp_input_method_v2 *im)
{
    pending_active = false;
}

static void on_surrounding_text(void *data, struct zwp_input_method_v2 *im, const char *text, uint32_t cursor,
                                uint32_t anchor)
{
}

static void on_text_change_cause(void *data, struct zwp_input_method_v2 *im, uint32_t cause)
{
}

static void on_content_type(void *data, struct zwp_input_method_v2 *im, uint32_t hint, uint32_t purpose)
{
}

// Written by the Android app ("1" while a USB or Bluetooth keyboard is connected): Linux cannot see input devices.
static bool hardware_keyboard(void)
{
    char path[512];
    const char *run = getenv("XDG_RUNTIME_DIR");
    snprintf(path, sizeof path, "%s/hardware-keyboard", run ? run : "/tmp");
    FILE *f = fopen(path, "r");
    int c = f ? fgetc(f) : EOF;
    if (f)
        fclose(f);
    return c == '1';
}

static void on_done(void *data, struct zwp_input_method_v2 *im)
{
    // Like Android: no on-screen keyboard while a physical one is connected (the bar icon still opens it).
    bool want = pending_active && !hardware_keyboard();
    if (want == shown)
        return;
    shown = want;
    run_shell(shown ? "summon" : "hide");
}

static void on_unavailable(void *data, struct zwp_input_method_v2 *im)
{
    // Another input method (e.g. fcitx5) already holds the seat.
    unavailable = true;
}

static const struct zwp_input_method_v2_listener im_listener = {
    .activate = on_activate,
    .deactivate = on_deactivate,
    .surrounding_text = on_surrounding_text,
    .text_change_cause = on_text_change_cause,
    .content_type = on_content_type,
    .done = on_done,
    .unavailable = on_unavailable,
};

static void on_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    if (!strcmp(iface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name))
        manager = wl_registry_bind(reg, name, &zwp_input_method_manager_v2_interface, 1);
}

static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {on_global, on_global_remove};

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "osk-autoshow: no Wayland display\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!seat || !manager) {
        fprintf(stderr, "osk-autoshow: compositor lacks wl_seat or input-method-v2\n");
        return 1;
    }
    struct zwp_input_method_v2 *im = zwp_input_method_manager_v2_get_input_method(manager, seat);
    zwp_input_method_v2_add_listener(im, &im_listener, NULL);
    while (wl_display_dispatch(display) != -1 && !unavailable)
        ;
    if (unavailable)
        fprintf(stderr, "osk-autoshow: another input method is active\n");
    return unavailable ? 2 : 0;
}
