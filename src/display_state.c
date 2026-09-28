// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "common.h"
#include "display_state.h"

#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <wayland-client.h>

#include "output-power.h"

struct watched_output {
    struct nd_display_state_source *state;
    struct wl_output *wl;
    struct zwlr_output_power_v1 *power;
    uint32_t global_name;
    uint32_t mode;
    int have_mode;
    char name[128];
    struct watched_output *next;
};

struct wayland_watch {
    struct nd_display_state_source *state;
    struct wl_display *display;
    struct wl_registry *registry;
    struct zwlr_output_power_manager_v1 *manager;
    struct watched_output *outputs;
};

static int source_matches(const struct nd_display_state_source *s,
                          const char *name)
{
    if (!name || !*name) return 0;
    size_t prefix = strlen(s->output);
    if (!strncmp(name, s->output, prefix) &&
        (name[prefix] == 0 || name[prefix] == '-')) return 0;
    return !s->source_output[0] || !strcmp(name, s->source_output);
}

static int send_state_locked(struct nd_display_state_source *s)
{
    if (!s->session_count || !s->flags) return 0;
    struct ndc_display_state state = {
        .flags = htonl(s->flags),
        .brightness = htonl(s->brightness),
    };
    int result = 0;
    for (unsigned i = 0; i < s->session_count;) {
        int fd = s->session_fds[i];
        if (ndc_send_msg(fd, NDC_DISPLAY_STATE, &state, sizeof(state)) == 0) {
            i++;
            continue;
        }
        shutdown(fd, SHUT_RDWR);
        s->session_fds[i] = s->session_fds[--s->session_count];
        result = -1;
    }
    return result;
}

static void publish_dpms(struct nd_display_state_source *s, int on)
{
    pthread_mutex_lock(&s->mutex);
    uint32_t old = s->flags;
    s->flags |= NDC_DISPLAY_HAS_DPMS;
    if (on) s->flags |= NDC_DISPLAY_DPMS_ON;
    else s->flags &= ~NDC_DISPLAY_DPMS_ON;
    if (old != s->flags) {
        fprintf(stderr, "host DPMS changed: %s\n", on ? "on" : "off");
        (void)send_state_locked(s);
    }
    pthread_mutex_unlock(&s->mutex);
}

static void publish_brightness(struct nd_display_state_source *s,
                               uint32_t brightness)
{
    if (brightness > 10000u) brightness = 10000u;
    pthread_mutex_lock(&s->mutex);
    int changed = !(s->flags & NDC_DISPLAY_HAS_BRIGHTNESS) ||
                  s->brightness != brightness;
    s->flags |= NDC_DISPLAY_HAS_BRIGHTNESS;
    s->brightness = brightness;
    if (changed) {
        fprintf(stderr, "host brightness changed: %.2f%%\n",
                (double)brightness / 100.0);
        (void)send_state_locked(s);
    }
    pthread_mutex_unlock(&s->mutex);
}

static void power_mode(void *data, struct zwlr_output_power_v1 *power,
                       uint32_t mode)
{
    (void)power;
    struct watched_output *out = data;
    out->mode = mode;
    out->have_mode = 1;
    if (source_matches(out->state, out->name))
        publish_dpms(out->state, mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

static void power_failed(void *data, struct zwlr_output_power_v1 *power)
{
    (void)power;
    struct watched_output *out = data;
    fprintf(stderr, "cannot watch DPMS for Wayland output %s\n",
            out->name[0] ? out->name : "(unnamed)");
}

static const struct zwlr_output_power_v1_listener power_listener = {
    .mode = power_mode,
    .failed = power_failed,
};

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make,
                            const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}
static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags; (void)width; (void)height; (void)refresh;
}
static void output_done(void *data, struct wl_output *output)
{ (void)data; (void)output; }
static void output_scale(void *data, struct wl_output *output, int32_t scale)
{ (void)data; (void)output; (void)scale; }
static void output_name(void *data, struct wl_output *output, const char *name)
{
    (void)output;
    struct watched_output *out = data;
    snprintf(out->name, sizeof(out->name), "%s", name);
    if (!out->state->source_output[0] &&
        strcmp(out->name, out->state->output)) {
        snprintf(out->state->source_output,
                 sizeof(out->state->source_output), "%s", out->name);
        fprintf(stderr, "host display-state source: %s\n", out->name);
    }
    if (out->have_mode && source_matches(out->state, out->name))
        publish_dpms(out->state,
                     out->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
}
static void output_description(void *data, struct wl_output *output,
                               const char *description)
{ (void)data; (void)output; (void)description; }

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void attach_power_watches(struct wayland_watch *w)
{
    if (!w->manager) return;
    for (struct watched_output *o = w->outputs; o; o = o->next) {
        if (o->power) continue;
        o->power = zwlr_output_power_manager_v1_get_output_power(w->manager,
                                                                  o->wl);
        zwlr_output_power_v1_add_listener(o->power, &power_listener, o);
    }
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct wayland_watch *w = data;
    if (!strcmp(interface, wl_output_interface.name)) {
        if (version < 4) return;
        struct watched_output *o = calloc(1, sizeof(*o));
        if (!o) return;
        o->state = w->state;
        o->global_name = name;
        o->wl = wl_registry_bind(registry, name, &wl_output_interface, 4);
        wl_output_add_listener(o->wl, &output_listener, o);
        o->next = w->outputs;
        w->outputs = o;
        attach_power_watches(w);
    } else if (!strcmp(interface, zwlr_output_power_manager_v1_interface.name)) {
        uint32_t bind_version = version < 1 ? version : 1;
        w->manager = wl_registry_bind(registry, name,
                                      &zwlr_output_power_manager_v1_interface,
                                      bind_version);
        attach_power_watches(w);
    }
}

static void registry_remove(void *data, struct wl_registry *registry,
                            uint32_t name)
{
    (void)registry;
    struct wayland_watch *w = data;
    struct watched_output **p = &w->outputs;
    while (*p) {
        struct watched_output *o = *p;
        if (o->global_name != name) { p = &o->next; continue; }
        *p = o->next;
        if (o->power) zwlr_output_power_v1_destroy(o->power);
        wl_output_destroy(o->wl);
        free(o);
        break;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void *wayland_thread(void *opaque)
{
    struct nd_display_state_source *state = opaque;
    struct wayland_watch w = { .state = state };
    w.display = wl_display_connect(NULL);
    if (!w.display) {
        fprintf(stderr, "DPMS sync unavailable: cannot connect to Wayland\n");
        return NULL;
    }
    w.registry = wl_display_get_registry(w.display);
    wl_registry_add_listener(w.registry, &registry_listener, &w);
    if (wl_display_roundtrip(w.display) < 0 ||
        wl_display_roundtrip(w.display) < 0 || !w.manager) {
        fprintf(stderr, "DPMS sync unavailable: compositor has no wlr-output-power-management-v1\n");
        wl_display_disconnect(w.display);
        return NULL;
    }
    while (wl_display_dispatch(w.display) >= 0) {}
    fprintf(stderr, "DPMS sync stopped: Wayland connection closed\n");
    wl_display_disconnect(w.display);
    return NULL;
}

static int read_uint_file(const char *path, unsigned long *value)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fscanf(f, "%lu", value) == 1 ? 0 : -1;
    fclose(f);
    return ok;
}

static int find_backlight(const char *setting, char *dir, size_t dir_size)
{
    if (!setting[0] || !strcmp(setting, "none")) return -1;
    if (strcmp(setting, "auto")) {
        if (setting[0] == '/') snprintf(dir, dir_size, "%s", setting);
        else snprintf(dir, dir_size, "/sys/class/backlight/%s", setting);
        return 0;
    }
    glob_t g;
    if (glob("/sys/class/backlight/*", 0, NULL, &g) != 0 || !g.gl_pathc)
        return -1;
    snprintf(dir, dir_size, "%s", g.gl_pathv[0]);
    globfree(&g);
    return 0;
}

static void *brightness_thread(void *opaque)
{
    struct nd_display_state_source *state = opaque;
    char dir[PATH_MAX - 32], current[PATH_MAX], actual[PATH_MAX], maximum[PATH_MAX];
    if (find_backlight(state->brightness_device, dir, sizeof(dir)) < 0) {
        fprintf(stderr, "brightness sync unavailable: no host backlight device\n");
        return NULL;
    }
    snprintf(current, sizeof(current), "%s/brightness", dir);
    snprintf(actual, sizeof(actual), "%s/actual_brightness", dir);
    snprintf(maximum, sizeof(maximum), "%s/max_brightness", dir);
    unsigned long max_value;
    if (read_uint_file(maximum, &max_value) < 0 || !max_value) {
        fprintf(stderr, "brightness sync unavailable: cannot read %s\n", maximum);
        return NULL;
    }

    int ino = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (ino >= 0) {
        (void)inotify_add_watch(ino, current, IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
        (void)inotify_add_watch(ino, actual, IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
    }
    uint32_t last = UINT32_MAX;
    for (;;) {
        unsigned long value;
        /* brightness is the requested level and changes in the same kernel
         * notification that woke us. actual_brightness can lag while a panel
         * performs a hardware fade, so do not delay the network update on it. */
        if (read_uint_file(current, &value) == 0) {
            uint64_t scaled = (uint64_t)value * 10000u / max_value;
            if (scaled != last) {
                last = (uint32_t)scaled;
                publish_brightness(state, last);
            }
        }
        if (ino < 0) { usleep(100000); continue; }
        struct pollfd p = { .fd = ino, .events = POLLIN };
        int pr;
        do { pr = poll(&p, 1, 100); } while (pr < 0 && errno == EINTR);
        if (pr > 0) {
            char events[1024];
            while (read(ino, events, sizeof(events)) > 0) {}
        }
    }
}

int nd_display_state_start(struct nd_display_state_source *s,
                           const char *output, const char *source_output,
                           const char *brightness_device)
{
    memset(s, 0, sizeof(*s));
    s->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    s->session_count = 0;
    snprintf(s->output, sizeof(s->output), "%s", output);
    snprintf(s->source_output, sizeof(s->source_output), "%s", source_output);
    snprintf(s->brightness_device, sizeof(s->brightness_device), "%s",
             brightness_device);
    pthread_t power, brightness;
    int e = pthread_create(&power, NULL, wayland_thread, s);
    if (e) { errno = e; return -1; }
    pthread_detach(power);
    e = pthread_create(&brightness, NULL, brightness_thread, s);
    if (e) { errno = e; return -1; }
    pthread_detach(brightness);
    return 0;
}

int nd_display_state_attach(struct nd_display_state_source *s, int fd)
{
    pthread_mutex_lock(&s->mutex);
    if (s->session_count >= sizeof(s->session_fds) / sizeof(s->session_fds[0])) {
        pthread_mutex_unlock(&s->mutex);
        errno = ENOSPC;
        return -1;
    }
    s->session_fds[s->session_count++] = fd;
    int rc = send_state_locked(s);
    pthread_mutex_unlock(&s->mutex);
    return rc;
}

void nd_display_state_detach(struct nd_display_state_source *s, int fd)
{
    pthread_mutex_lock(&s->mutex);
    for (unsigned i = 0; i < s->session_count; i++) {
        if (s->session_fds[i] != fd) continue;
        s->session_fds[i] = s->session_fds[--s->session_count];
        break;
    }
    pthread_mutex_unlock(&s->mutex);
}

int nd_display_state_send(struct nd_display_state_source *s, int fd,
                          uint16_t type, const void *payload, uint32_t len)
{
    pthread_mutex_lock(&s->mutex);
    int found = 0;
    for (unsigned i = 0; i < s->session_count; i++)
        if (s->session_fds[i] == fd) { found = 1; break; }
    int rc = found ? ndc_send_msg(fd, type, payload, len) : -1;
    pthread_mutex_unlock(&s->mutex);
    return rc;
}
