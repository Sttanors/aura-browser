/* Test-only text-input-v3 compositor endpoint; no system IME/desktop connection. */
#include "text-input-v3-server.h"
#include <libweston/libweston.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>
struct input {
    struct wl_list link;
    struct wl_resource *resource, *surface;
    struct wl_listener surface_destroy;
    uint32_t serial;
    int enabled, pending, x, y, w, h;
};
static struct wl_list inputs;
static struct wl_global *manager;
static struct weston_keyboard *keyboard;
static struct wl_listener focus_listener;
static void surface_destroyed(struct wl_listener *l, void *data) {
    struct input *i = wl_container_of(l, i, surface_destroy);
    wl_list_remove(&i->surface_destroy.link);
    wl_list_init(&i->surface_destroy.link);
    i->surface = NULL;
    i->enabled = i->pending = 0;
}
static void update_focus(void) {
    struct input *i;
    struct wl_resource *surface = keyboard->focus ? keyboard->focus->resource : NULL;
    wl_list_for_each(i, &inputs, link) {
        struct wl_resource *next =
            surface && wl_resource_get_client(surface) == wl_resource_get_client(i->resource)
                ? surface
                : NULL;
        if (i->surface == next)
            continue;
        if (i->surface) {
            zwp_text_input_v3_send_leave(i->resource, i->surface);
            wl_list_remove(&i->surface_destroy.link);
            wl_list_init(&i->surface_destroy.link);
        }
        i->surface = next;
        i->enabled = i->pending = 0;
        if (next) {
            i->surface_destroy.notify = surface_destroyed;
            wl_resource_add_destroy_listener(next, &i->surface_destroy);
            zwp_text_input_v3_send_enter(i->resource, next);
        }
    }
}
static void focus_changed(struct wl_listener *l, void *data) { update_focus(); }
static void destroy_request(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void enabled(struct wl_client *c, struct wl_resource *r) {
    ((struct input *)wl_resource_get_user_data(r))->pending = 1;
}
static void disabled(struct wl_client *c, struct wl_resource *r) {
    ((struct input *)wl_resource_get_user_data(r))->pending = 0;
}
static void surrounding(struct wl_client *c, struct wl_resource *r, const char *s, int32_t cursor,
                        int32_t anchor) {}
static void cause(struct wl_client *c, struct wl_resource *r, uint32_t value) {}
static void content(struct wl_client *c, struct wl_resource *r, uint32_t hints, uint32_t purpose) {}
static void rectangle(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w,
                      int32_t h) {
    struct input *i = wl_resource_get_user_data(r);
    i->x = x;
    i->y = y;
    i->w = w;
    i->h = h;
}
static void commit(struct wl_client *c, struct wl_resource *r) {
    struct input *i = wl_resource_get_user_data(r);
    ++i->serial;
    i->enabled = i->pending && i->surface;
}
static const struct zwp_text_input_v3_interface input_implementation = {
    destroy_request, enabled, disabled, surrounding, cause, content, rectangle, commit};
static void freed(struct wl_resource *r) {
    struct input *i = wl_resource_get_user_data(r);
    wl_list_remove(&i->link);
    wl_list_remove(&i->surface_destroy.link);
    free(i);
}
static void get_input(struct wl_client *c, struct wl_resource *r, uint32_t id,
                      struct wl_resource *seat) {
    struct input *i = calloc(1, sizeof(*i));
    if (!i) {
        wl_client_post_no_memory(c);
        return;
    }
    i->resource = wl_resource_create(c, &zwp_text_input_v3_interface, 1, id);
    wl_list_init(&i->surface_destroy.link);
    wl_list_insert(&inputs, &i->link);
    wl_resource_set_implementation(i->resource, &input_implementation, i, freed);
    update_focus();
}
static const struct zwp_text_input_manager_v3_interface manager_implementation = {destroy_request,
                                                                                  get_input};
static void bind_manager(struct wl_client *c, void *data, uint32_t version, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &zwp_text_input_manager_v3_interface, 1, id);
    wl_resource_set_implementation(r, &manager_implementation, NULL, NULL);
}
void aura_ime_init(struct weston_compositor *compositor, struct weston_seat *seat) {
    wl_list_init(&inputs);
    keyboard = seat->keyboard_state;
    manager = wl_global_create(compositor->wl_display, &zwp_text_input_manager_v3_interface, 1,
                               NULL, bind_manager);
    focus_listener.notify = focus_changed;
    wl_signal_add(&keyboard->focus_signal, &focus_listener);
}
void aura_ime_finish(void) {
    struct input *i, *next;
    wl_list_remove(&focus_listener.link);
    wl_global_destroy(manager);
    wl_list_for_each_safe(i, next, &inputs, link) wl_resource_destroy(i->resource);
}
int aura_ime_command(const char *s, char *reply, size_t size) {
    if (strncmp(s, "ime-", 4))
        return 0;
    struct input *i, *active = NULL;
    wl_list_for_each(i, &inputs, link) if (i->enabled && i->surface) {
        active = i;
        break;
    }
    if (!active) {
        snprintf(reply, size, "not-enabled");
        return 1;
    }
    if (!strcmp(s, "ime-status")) {
        snprintf(reply, size, "%d,%d,%d,%d", active->x, active->y, active->w, active->h);
        return 1;
    }
    if (!strncmp(s, "ime-preedit ", 12)) {
        const char *text = s + 12;
        zwp_text_input_v3_send_preedit_string(active->resource, text, strlen(text), strlen(text));
    } else if (!strncmp(s, "ime-commit ", 11)) {
        zwp_text_input_v3_send_commit_string(active->resource, s + 11);
        zwp_text_input_v3_send_preedit_string(active->resource, "", 0, 0);
    } else if (!strcmp(s, "ime-cancel")) {
        zwp_text_input_v3_send_preedit_string(active->resource, "", 0, 0);
    } else {
        snprintf(reply, size, "invalid");
        return 1;
    }
    zwp_text_input_v3_send_done(active->resource, active->serial);
    return 1;
}
