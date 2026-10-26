/* Disposable Weston 15 test module. Never loaded into the user's compositor. */
#include <libweston/desktop.h>
#include <libweston/libweston.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
/* Weston backend API, matched to the installed Weston 15 headers. */
extern void weston_seat_init(struct weston_seat *, struct weston_compositor *, const char *);
extern int weston_seat_init_keyboard(struct weston_seat *, struct xkb_keymap *);
extern int weston_seat_init_pointer(struct weston_seat *);
extern void weston_seat_release(struct weston_seat *);
extern void notify_key(struct weston_seat *, const struct timespec *, uint32_t,
                       enum wl_keyboard_key_state, enum weston_key_state_update);
extern void notify_button(struct weston_seat *, const struct timespec *, int32_t,
                          enum wl_pointer_button_state);
extern void notify_motion_absolute(struct weston_seat *, const struct timespec *,
                                   struct weston_coord_global);
extern void notify_pointer_frame(struct weston_seat *);
struct control {
    struct weston_compositor *c;
    struct weston_seat seat;
    int fd;
    pid_t pid;
    int index;
    struct wl_event_source *source;
    struct wl_listener destroy;
};
static struct weston_view *selected(struct control *p) {
    struct weston_view *v;
    int i = 0;
    wl_list_for_each(v, &p->c->view_list, link) {
        if (!v->surface->resource || !weston_surface_is_desktop_surface(v->surface))
            continue;
        pid_t pid;
        uid_t uid;
        gid_t gid;
        wl_client_get_credentials(wl_resource_get_client(v->surface->resource), &pid, &uid, &gid);
        const char *app =
            weston_desktop_surface_get_app_id(weston_surface_get_desktop_surface(v->surface));
        if (pid == p->pid && app && !strcmp(app, "aura-browser") && i++ == p->index)
            return v;
    }
    return NULL;
}
static int command(int fd, uint32_t mask, void *data) {
    struct control *p = data;
    char b[4096] = {0}, reply[64] = "ok";
    struct sockaddr_un peer;
    socklen_t len = sizeof(peer);
    ssize_t n = recvfrom(fd, b, sizeof(b) - 1, 0, (void *)&peer, &len);
    if (n < 1)
        return 0;
    int a = 0, c = 0;
    float x = 0, y = 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (sscanf(b, "focus %d %d", &a, &c) == 2) {
        p->pid = a;
        p->index = c;
        struct weston_view *v = selected(p);
        if (v) {
            if (v->layer_link.layer) {
                struct weston_layer *layer = v->layer_link.layer;
                wl_list_remove(&v->layer_link.link);
                wl_list_insert(&layer->view_list.link, &v->layer_link.link);
                weston_view_geometry_dirty(v);
                p->c->view_list_needs_rebuild = true;
            }
            weston_view_activate_input(v, &p->seat, WESTON_ACTIVATE_FLAG_CLICKED);
            weston_seat_set_keyboard_focus(&p->seat, v->surface);
            weston_compositor_damage_all(p->c);
        } else
            strcpy(reply, "missing");
    } else if (sscanf(b, "key %d %d", &a, &c) == 2)
        notify_key(&p->seat, &t, a, c, STATE_UPDATE_AUTOMATIC);
    else if (sscanf(b, "button %d %d", &a, &c) == 2) {
        notify_button(&p->seat, &t, a, c);
        notify_pointer_frame(&p->seat);
    } else if (sscanf(b, "move %f %f", &x, &y) == 2) {
        struct weston_view *v = selected(p);
        if (v) {
            struct weston_coord_global pos =
                weston_coord_surface_to_global(v, weston_coord_surface(x, y, v->surface));
            notify_motion_absolute(&p->seat, &t, pos);
            notify_pointer_frame(&p->seat);
        } else
            strcpy(reply, "missing");
    } else if (sscanf(b, "size %d %d", &a, &c) == 2) {
        struct weston_view *v = selected(p);
        if (v)
            weston_desktop_surface_set_size(weston_surface_get_desktop_surface(v->surface), a, c);
        else
            strcpy(reply, "missing");
    } else if (!strcmp(b, "close")) {
        struct weston_view *v = selected(p);
        if (v)
            weston_desktop_surface_close(weston_surface_get_desktop_surface(v->surface));
        else
            strcpy(reply, "missing");
    } else if (!strncmp(b, "capture ", 8)) {
        struct weston_view *v = selected(p);
        if (v) {
            int w, h;
            weston_surface_get_content_size(v->surface, &w, &h);
            if (w > 0 && h > 0 && w < 10000 && h < 10000) {
                size_t bytes = (size_t)w * h * 4;
                uint32_t *pixels = malloc(bytes);
                if (weston_surface_copy_content(v->surface, pixels, bytes, 0, 0, w, h) == 0) {
                    FILE *f = fopen(b + 8, "wb");
                    if (f) {
                        fprintf(f, "P6\n%d %d\n255\n", w, h);
                        for (int i = 0; i < w * h; i++) {
                            unsigned char rgb[3] = {pixels[i], pixels[i] >> 8, pixels[i] >> 16};
                            fwrite(rgb, 1, 3, f);
                        }
                        fclose(f);
                    } else
                        strcpy(reply, "file-error");
                } else
                    strcpy(reply, "copy-error");
                free(pixels);
            }
        } else
            strcpy(reply, "missing");
    } else
        strcpy(reply, "invalid");
    sendto(fd, reply, strlen(reply), 0, (void *)&peer, len);
    return 0;
}
static void destroyed(struct wl_listener *l, void *data) {
    struct control *p = wl_container_of(l, p, destroy);
    wl_event_source_remove(p->source);
    close(p->fd);
    wl_list_remove(&p->destroy.link);
    weston_seat_release(&p->seat);
    free(p);
}
WL_EXPORT int wet_module_init(struct weston_compositor *c, int *argc, char *argv[]) {
    const char *path = getenv("AURA_TEST_CONTROL_SOCKET"),
               *isolated = getenv("AURA_TEST_ISOLATED_DISPLAY");
    if (!path || !isolated || strcmp(isolated, "1"))
        return -1;
    struct control *p = calloc(1, sizeof(*p));
    p->c = c;
    p->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un s = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(s.sun_path))
        return -1;
    strcpy(s.sun_path, path);
    if (bind(p->fd, (void *)&s, sizeof(s)))
        return -1;
    weston_seat_init(&p->seat, c, "aura-qa");
    weston_seat_init_keyboard(&p->seat, NULL);
    weston_seat_init_pointer(&p->seat);
    p->source = wl_event_loop_add_fd(wl_display_get_event_loop(c->wl_display), p->fd,
                                     WL_EVENT_READABLE, command, p);
    p->destroy.notify = destroyed;
    wl_signal_add(&c->destroy_signal, &p->destroy);
    return 0;
}
