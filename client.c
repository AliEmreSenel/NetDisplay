#define _GNU_SOURCE
#include "common.h"
#include <glob.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/random.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <spawn.h>
#include "proto.h"
#include "video_receiver.h"

extern char **environ;
static const char *self_program;

#define BITS_PER_LONG (sizeof(unsigned long) * 8u)
#define NBITS(x) (((x) + BITS_PER_LONG - 1u) / BITS_PER_LONG)
#define TEST_BIT(a,b) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1ul)
#define MAX_INPUT_DEVS 32

struct client_cfg {
    char host[64];
    char interface[IFNAMSIZ];
    char drm_device[128];
    char vaapi_device[128];
    int port;
    int want_input;
    int grab_input;
    int reconnect_ms;
};
struct input_dev {
    int fd;
    uint8_t cls;
    char path[256];
    struct input_absinfo abs_x, abs_y, mt_x, mt_y, pressure, mt_pressure;
    int have_abs_x, have_abs_y, have_mt_x, have_mt_y, have_pressure, have_mt_pressure;
};

struct input_ctx {
    int sock;
    int grab;
    pthread_mutex_t send_mutex;
    _Atomic int running;
};

static void cfg_defaults(struct client_cfg *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "auto");
    c->port = NDC_DEFAULT_PORT;
    c->want_input = 1;
    c->grab_input = 0;
    c->reconnect_ms = 500;
    /* Empty means auto-detect. This is important for the portable receiver: the
     * KMS card number and VAAPI render node are not stable across laptops. */
    c->drm_device[0] = 0;
    c->vaapi_device[0] = 0;
}

static void load_cfg(const char *path, struct client_cfg *c, int required)
{
    cfg_defaults(c);
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (!required && errno == ENOENT) return;
        ndc_die(path);
    }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *s = ndc_trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq++ = 0;
        char *k = ndc_trim(s), *v = ndc_trim(eq);
        if (!strcmp(k, "host")) snprintf(c->host, sizeof(c->host), "%s", v);
        else if (!strcmp(k, "port")) c->port = atoi(v);
        else if (!strcmp(k, "interface")) snprintf(c->interface, sizeof(c->interface), "%s", v);
        else if (!strcmp(k, "want_input")) c->want_input = atoi(v) != 0;
        else if (!strcmp(k, "grab_input")) c->grab_input = atoi(v) != 0;
        else if (!strcmp(k, "reconnect_ms")) c->reconnect_ms = atoi(v);
        else if (!strcmp(k, "drm_device")) snprintf(c->drm_device, sizeof(c->drm_device), "%s", v);
        else if (!strcmp(k, "vaapi_device")) snprintf(c->vaapi_device, sizeof(c->vaapi_device), "%s", v);
    }
    fclose(f);
    if (!c->host[0] || c->port <= 0 || c->port > 65535) {
        fprintf(stderr, "host/port missing or invalid in %s\n", path); exit(2);
    }
}


static const char *default_config_path(char *buf, size_t bufsz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(buf, bufsz, "%s/netdisplay/client.conf", xdg);
        return buf;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, bufsz, "%s/.config/netdisplay/client.conf", home);
        return buf;
    }
    return NULL;
}

static uint32_t discovery_nonce(void)
{
    uint32_t x = 0;
    if (getrandom(&x, sizeof(x), GRND_NONBLOCK) == (ssize_t)sizeof(x) && x)
        return x;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    x = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec ^ (uint32_t)getpid();
    return x ? x : 1u;
}

#define MAX_DISCOVERY_IFACES 32

struct discovery_socket {
    int fd;
    char ifname[IFNAMSIZ];
};

static void close_discovery_sockets(struct discovery_socket *ds, int n)
{
    for (int i = 0; i < n; i++) {
        if (ds[i].fd >= 0) close(ds[i].fd);
        ds[i].fd = -1;
    }
}

static int have_discovery_iface(const struct discovery_socket *ds, int n,
                                const char *ifname)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(ds[i].ifname, ifname)) return 1;
    return 0;
}

/* Make one UDP socket per usable interface. Each socket is bound to its device
 * before first use, then sends the limited broadcast 255.255.255.255. This
 * avoids routing-table ambiguity on machines with Wi-Fi + Ethernet while still
 * requiring no knowledge of the interface's subnet or directed broadcast. */
static int make_discovery_sockets(const struct client_cfg *cfg,
                                  struct discovery_socket *ds, int cap)
{
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) < 0) return -1;

    int n = 0;
    for (struct ifaddrs *i = ifs; i && n < cap; i = i->ifa_next) {
        if (!i->ifa_name || !i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) ||
            (i->ifa_flags & IFF_LOOPBACK))
            continue;
        if (cfg->interface[0] && strcmp(cfg->interface, i->ifa_name))
            continue;
        if (have_discovery_iface(ds, n, i->ifa_name))
            continue;

        int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (s < 0) continue;

        int one = 1;
        if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0) {
            close(s);
            continue;
        }

        if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE,
                       i->ifa_name, strlen(i->ifa_name) + 1) < 0) {
            fprintf(stderr, "discovery: cannot bind UDP socket to %s: %s\n",
                    i->ifa_name, strerror(errno));
            close(s);
            continue;
        }

        ds[n].fd = s;
        snprintf(ds[n].ifname, sizeof(ds[n].ifname), "%s", i->ifa_name);
        n++;
    }
    freeifaddrs(ifs);

    if (!n) {
        errno = ENETUNREACH;
        return -1;
    }
    return n;
}

static int discover_host(const struct client_cfg *cfg, char *ip, size_t ipsz,
                         int *port, char *ifname, size_t ifnamesz)
{
    struct discovery_socket ds[MAX_DISCOVERY_IFACES];
    for (int i = 0; i < MAX_DISCOVERY_IFACES; i++) ds[i].fd = -1;

    int nds = make_discovery_sockets(cfg, ds, MAX_DISCOVERY_IFACES);
    if (nds < 0) return -1;

    uint32_t nonce = discovery_nonce();
    struct ndc_discovery d = {
        .magic = htonl(NDC_DISC_MAGIC),
        .version = htons(NDC_VERSION),
        .type = htons(NDC_DISCOVER),
        .nonce = htonl(nonce),
        .tcp_port = 0,
        .reserved = 0,
    };
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)cfg->port),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST), /* 255.255.255.255 */
    };

    int sent = 0;
    for (int i = 0; i < nds; i++) {
        ssize_t n = sendto(ds[i].fd, &d, sizeof(d), 0,
                           (struct sockaddr *)&to, sizeof(to));
        if (n == (ssize_t)sizeof(d)) {
            sent++;
        } else {
            fprintf(stderr, "discovery: send on %s: %s\n",
                    ds[i].ifname, n < 0 ? strerror(errno) : "short send");
        }
    }
    if (!sent) {
        close_discovery_sockets(ds, nds);
        errno = ENETUNREACH;
        return -1;
    }

    struct pollfd pfds[MAX_DISCOVERY_IFACES];
    for (int i = 0; i < nds; i++) {
        pfds[i].fd = ds[i].fd;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    int pr;
    do { pr = poll(pfds, (nfds_t)nds, 700); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) {
        close_discovery_sockets(ds, nds);
        if (pr == 0) errno = ETIMEDOUT;
        return -1;
    }

    for (int i = 0; i < nds; i++) {
        if (!(pfds[i].revents & POLLIN)) continue;
        for (;;) {
            struct ndc_discovery offer;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(ds[i].fd, &offer, sizeof(offer), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fl);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            if (n != (ssize_t)sizeof(offer)) continue;
            if (ntohl(offer.magic) != NDC_DISC_MAGIC ||
                ntohs(offer.version) != NDC_VERSION ||
                ntohs(offer.type) != NDC_OFFER ||
                ntohl(offer.nonce) != nonce)
                continue;
            int p = ntohs(offer.tcp_port);
            if (p <= 0) continue;
            if (!inet_ntop(AF_INET, &from.sin_addr, ip, ipsz)) continue;
            *port = p;
            snprintf(ifname, ifnamesz, "%s", ds[i].ifname);
            close_discovery_sockets(ds, nds);
            return 0;
        }
    }

    close_discovery_sockets(ds, nds);
    errno = ETIMEDOUT;
    return -1;
}

static int connect_host_ip(const char *ip, int port, const char *ifname)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (ifname && *ifname &&
        setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) < 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    ndc_set_tcp_opts(s);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); errno = EINVAL; return -1; }
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        int e = errno; close(s); errno = e; return -1;
    }
    return s;
}

static int get_bits(int fd, int ev, unsigned long *bits, size_t nbytes)
{
    memset(bits, 0, nbytes);
    return ioctl(fd, EVIOCGBIT(ev, nbytes), bits);
}

static uint8_t classify_input(int fd)
{
    unsigned long evbits[NBITS(EV_MAX + 1)];
    if (get_bits(fd, 0, evbits, sizeof(evbits)) < 0) return 0;

    if (TEST_BIT(evbits, EV_ABS)) {
        unsigned long absbits[NBITS(ABS_MAX + 1)];
        if (get_bits(fd, EV_ABS, absbits, sizeof(absbits)) >= 0 &&
            TEST_BIT(absbits, ABS_MT_POSITION_X) && TEST_BIT(absbits, ABS_MT_POSITION_Y))
            return NDC_DEV_TOUCHPAD;
    }

    if (TEST_BIT(evbits, EV_REL)) {
        unsigned long relbits[NBITS(REL_MAX + 1)];
        if (get_bits(fd, EV_REL, relbits, sizeof(relbits)) >= 0 &&
            TEST_BIT(relbits, REL_X) && TEST_BIT(relbits, REL_Y))
            return NDC_DEV_KBM;
    }

    if (TEST_BIT(evbits, EV_KEY)) {
        unsigned long keybits[NBITS(KEY_MAX + 1)];
        if (get_bits(fd, EV_KEY, keybits, sizeof(keybits)) >= 0 &&
            TEST_BIT(keybits, KEY_A) && TEST_BIT(keybits, KEY_SPACE) && TEST_BIT(keybits, KEY_ENTER))
            return NDC_DEV_KBM;
    }
    return 0;
}

static void read_abs(int fd, int code, struct input_absinfo *a, int *have)
{
    if (ioctl(fd, EVIOCGABS(code), a) == 0 && a->maximum > a->minimum) *have = 1;
}

static int scan_inputs(struct input_dev *devs, int grab)
{
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob("/dev/input/event*", 0, NULL, &g) != 0) return 0;
    int ndev = 0;
    for (size_t i = 0; i < g.gl_pathc && ndev < MAX_INPUT_DEVS; i++) {
        int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        uint8_t cls = classify_input(fd);
        if (!cls) { close(fd); continue; }

        struct input_dev *d = &devs[ndev];
        memset(d, 0, sizeof(*d));
        d->fd = fd; d->cls = cls;
        snprintf(d->path, sizeof(d->path), "%s", g.gl_pathv[i]);
        if (cls == NDC_DEV_TOUCHPAD) {
            read_abs(fd, ABS_X, &d->abs_x, &d->have_abs_x);
            read_abs(fd, ABS_Y, &d->abs_y, &d->have_abs_y);
            read_abs(fd, ABS_MT_POSITION_X, &d->mt_x, &d->have_mt_x);
            read_abs(fd, ABS_MT_POSITION_Y, &d->mt_y, &d->have_mt_y);
            read_abs(fd, ABS_PRESSURE, &d->pressure, &d->have_pressure);
            read_abs(fd, ABS_MT_PRESSURE, &d->mt_pressure, &d->have_mt_pressure);
        }
        if (grab && ioctl(fd, EVIOCGRAB, 1) < 0)
            fprintf(stderr, "warning: cannot grab %s: %s\n", d->path, strerror(errno));
        char name[128] = "?";
        (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        fprintf(stderr, "input %s: %s (%s)\n", cls == NDC_DEV_TOUCHPAD ? "touchpad" : "kbd/mouse", name, d->path);
        ndev++;
    }
    globfree(&g);
    return ndev;
}

static void close_inputs(struct input_dev *devs, int ndev, int grab)
{
    for (int i = 0; i < ndev; i++) {
        if (devs[i].fd >= 0) {
            if (grab) (void)ioctl(devs[i].fd, EVIOCGRAB, 0);
            close(devs[i].fd);
        }
    }
}

static int scale_abs(int v, const struct input_absinfo *a, int outmax)
{
    if (!a || a->maximum <= a->minimum) return v;
    if (v <= a->minimum) return 0;
    if (v >= a->maximum) return outmax;
    int64_t num = (int64_t)(v - a->minimum) * outmax;
    return (int)(num / (a->maximum - a->minimum));
}

static int touchpad_abs_value(struct input_dev *d, uint16_t code, int v, int *supported)
{
    *supported = 1;
    switch (code) {
    case ABS_X: return d->have_abs_x ? scale_abs(v, &d->abs_x, 10000) : v;
    case ABS_Y: return d->have_abs_y ? scale_abs(v, &d->abs_y, 7000) : v;
    case ABS_MT_POSITION_X: return d->have_mt_x ? scale_abs(v, &d->mt_x, 10000) : v;
    case ABS_MT_POSITION_Y: return d->have_mt_y ? scale_abs(v, &d->mt_y, 7000) : v;
    case ABS_PRESSURE: return d->have_pressure ? scale_abs(v, &d->pressure, 255) : v;
    case ABS_MT_PRESSURE: return d->have_mt_pressure ? scale_abs(v, &d->mt_pressure, 255) : v;
    case ABS_MT_SLOT:
    case ABS_MT_TRACKING_ID:
    case ABS_MT_TOUCH_MAJOR:
    case ABS_MT_TOUCH_MINOR:
    case ABS_MT_WIDTH_MAJOR:
    case ABS_MT_WIDTH_MINOR:
    case ABS_MT_ORIENTATION:
#ifdef ABS_MT_TOOL_TYPE
    case ABS_MT_TOOL_TYPE:
#endif
#ifdef ABS_MT_DISTANCE
    case ABS_MT_DISTANCE:
#endif
        return v;
    default:
        *supported = 0; return v;
    }
}

static int send_input(struct input_ctx *ctx, uint8_t dev, const struct input_event *ev, int value)
{
    struct ndc_input w = {
        .device = dev,
        .reserved0 = 0,
        .type = htons(ev->type),
        .code = htons(ev->code),
        .reserved1 = 0,
        .value = (int32_t)htonl((uint32_t)value),
    };
    pthread_mutex_lock(&ctx->send_mutex);
    int r = ndc_send_msg(ctx->sock, NDC_INPUT, &w, sizeof(w));
    pthread_mutex_unlock(&ctx->send_mutex);
    return r;
}

static void *input_main(void *opaque)
{
    struct input_ctx *ctx = opaque;
    struct input_dev devs[MAX_INPUT_DEVS];
    int ndev = scan_inputs(devs, ctx->grab);
    if (!ndev) {
        fprintf(stderr, "no readable keyboard/mouse/touchpad evdev devices\n");
        atomic_store(&ctx->running, 0);
        return NULL;
    }

    struct pollfd pfds[MAX_INPUT_DEVS];
    for (int i = 0; i < ndev; i++) { pfds[i].fd = devs[i].fd; pfds[i].events = POLLIN; }

    while (atomic_load(&ctx->running)) {
        int pr = poll(pfds, ndev, 250);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        for (int i = 0; i < ndev; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            for (;;) {
                struct input_event ev;
                ssize_t n = read(devs[i].fd, &ev, sizeof(ev));
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (n < 0 && errno == EINTR) continue;
                if (n != (ssize_t)sizeof(ev)) goto done;

                if (ev.type != EV_SYN && ev.type != EV_KEY && ev.type != EV_REL && ev.type != EV_ABS)
                    continue;
                if (devs[i].cls == NDC_DEV_KBM) {
                    if (ev.type == EV_ABS) continue;
                    if (ev.type == EV_KEY && ev.value == 2) continue; /* host handles repeat */
                    if (send_input(ctx, NDC_DEV_KBM, &ev, ev.value) < 0) goto done;
                } else {
                    if (ev.type == EV_REL) continue;
                    int value = ev.value;
                    if (ev.type == EV_ABS) {
                        int supported;
                        value = touchpad_abs_value(&devs[i], ev.code, ev.value, &supported);
                        if (!supported) continue;
                    }
                    if (send_input(ctx, NDC_DEV_TOUCHPAD, &ev, value) < 0) goto done;
                }
            }
        }
    }
done:
    close_inputs(devs, ndev, ctx->grab);
    atomic_store(&ctx->running, 0);
    shutdown(ctx->sock, SHUT_RDWR);
    return NULL;
}


static pid_t spawn_builtin_receiver(const struct client_cfg *cfg, int video_port,
                                    int width, int height, int refresh_hz,
                                    const char *ifname, int *ready_fd)
{
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) return -1;

    enum { CHILD_READY_FD = 198 };
    char portbuf[16], readybuf[16], widthbuf[16], heightbuf[16], refreshbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", video_port);
    snprintf(readybuf, sizeof(readybuf), "%d", CHILD_READY_FD);
    snprintf(widthbuf, sizeof(widthbuf), "%d", width);
    snprintf(heightbuf, sizeof(heightbuf), "%d", height);
    snprintf(refreshbuf, sizeof(refreshbuf), "%d", refresh_hz);
    char *const av[] = {
        (char *)self_program,
        (char *)"--video-receiver",
        portbuf,
        (char *)(cfg->drm_device[0] ? cfg->drm_device : ""),
        (char *)(cfg->vaapi_device[0] ? cfg->vaapi_device : ""),
        (char *)(ifname ? ifname : ""),
        readybuf,
        widthbuf,
        heightbuf,
        refreshbuf,
        NULL,
    };

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    int e = posix_spawn_file_actions_init(&fa);
    if (e != 0) goto fail_no_fa;
    e = posix_spawn_file_actions_adddup2(&fa, pfd[1], CHILD_READY_FD);
    if (e != 0) goto fail;
    e = posix_spawn_file_actions_addclose(&fa, pfd[0]);
    if (e != 0) goto fail;
    e = posix_spawn_file_actions_addclose(&fa, pfd[1]);
    if (e != 0) goto fail;

    e = posix_spawnattr_init(&attr);
    if (e != 0) goto fail;
    short flags = POSIX_SPAWN_SETPGROUP;
    (void)posix_spawnattr_setflags(&attr, flags);
    (void)posix_spawnattr_setpgroup(&attr, 0);

    pid_t p = -1;
    e = posix_spawnp(&p, self_program, &fa, &attr, av, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    if (e != 0) {
        close(pfd[0]);
        errno = e;
        return -1;
    }
    *ready_fd = pfd[0];
    return p;

fail:
    posix_spawn_file_actions_destroy(&fa);
fail_no_fa:
    close(pfd[0]); close(pfd[1]);
    errno = e;
    return -1;
}

static int wait_receiver_ready(pid_t pid, int fd, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN | POLLHUP };
    int pr;
    do { pr = poll(&p, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) { if (pr == 0) errno = ETIMEDOUT; return -1; }

    uint8_t b = 0;
    ssize_t n;
    do { n = read(fd, &b, 1); } while (n < 0 && errno == EINTR);
    if (n == 1 && b == 1) return 0;

    int st = 0;
    pid_t wr = waitpid(pid, &st, WNOHANG);
    if (wr == pid && WIFEXITED(st))
        fprintf(stderr, "built-in video receiver exited status=%d before ready\n", WEXITSTATUS(st));
    errno = EIO;
    return -1;
}

int main(int argc, char **argv)
{
    self_program = argv[0];
    if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("netdisplay-client %s protocol=%u video=runtime-negotiated\n",
               NETDISPLAY_VERSION, NDC_VERSION);
        return 0;
    }
    if (argc == 10 && !strcmp(argv[1], "--video-receiver"))
        return nd_video_receiver_run(atoi(argv[2]),
                                     argv[3][0] ? argv[3] : NULL,
                                     argv[4][0] ? argv[4] : NULL,
                                     argv[5][0] ? argv[5] : NULL,
                                     atoi(argv[6]), atoi(argv[7]),
                                     atoi(argv[8]), atoi(argv[9]));

    if (argc > 2) {
        fprintf(stderr, "usage: %s [CLIENT_CONFIG]\n", argv[0]);
        return 2;
    }
    char default_cfg[4096];
    const char *cfg_path = argc == 2 ? argv[1] :
        default_config_path(default_cfg, sizeof(default_cfg));
    struct client_cfg cfg;
    load_cfg(cfg_path, &cfg, argc == 2);

    fprintf(stderr, "netdisplay-client: discovery=%s control-port=%d input-request=%s\n",
            !strcmp(cfg.host, "auto") ? "255.255.255.255/SO_BINDTODEVICE" : cfg.host,
            cfg.port, cfg.want_input ? "yes" : "no");

    for (;;) {
        char host_ip[INET_ADDRSTRLEN];
        char discovered_if[IFNAMSIZ] = "";
        int control_port = cfg.port;

        if (!strcmp(cfg.host, "auto")) {
            if (discover_host(&cfg, host_ip, sizeof(host_ip), &control_port,
                              discovered_if, sizeof(discovered_if)) < 0) {
                usleep((useconds_t)cfg.reconnect_ms * 1000u);
                continue;
            }
            fprintf(stderr, "discovered source %s:%d via %s\n",
                    host_ip, control_port, discovered_if);
        } else {
            snprintf(host_ip, sizeof(host_ip), "%s", cfg.host);
            if (cfg.interface[0])
                snprintf(discovered_if, sizeof(discovered_if), "%s", cfg.interface);
        }

        int s = connect_host_ip(host_ip, control_port, discovered_if);
        if (s < 0) {
            fprintf(stderr, "control connect %s:%d: %s\n",
                    host_ip, control_port, strerror(errno));
            usleep((useconds_t)cfg.reconnect_ms * 1000u);
            continue;
        }

        struct ndc_hello hello = {
            .flags = htonl(cfg.want_input ? NDC_FLAG_WANT_INPUT : 0)
        };
        if (ndc_send_msg(s, NDC_HELLO, &hello, sizeof(hello)) < 0) {
            close(s);
            continue;
        }

        uint8_t payload[NDC_MAX_PAYLOAD];
        uint32_t len = sizeof(payload);
        uint16_t type = 0;
        int rr = ndc_recv_msg(s, &type, payload, &len);
        if (rr <= 0 || type != NDC_WELCOME || len != sizeof(struct ndc_welcome)) {
            fprintf(stderr, "bad WELCOME\n");
            close(s);
            continue;
        }

        struct ndc_welcome wel;
        memcpy(&wel, payload, sizeof(wel));
        int input_allowed = !!(ntohl(wel.flags) & NDC_FLAG_INPUT_ALLOWED);
        int video_port = ntohs(wel.video_port);
        int width = ntohs(wel.width);
        int height = ntohs(wel.height);
        int refresh_hz = ntohs(wel.refresh_hz);
        if (video_port <= 0 || width <= 0 || (width & 1) ||
            height <= 0 || (height & 1) || refresh_hz <= 0) {
            fprintf(stderr,
                    "invalid negotiated stream %dx%d@%d port %d\n",
                    width, height, refresh_hz, video_port);
            close(s);
            usleep((useconds_t)cfg.reconnect_ms * 1000u);
            continue;
        }

        fprintf(stderr, "source connected; video %dx%d@%d UDP %d via %s; input %s\n",
                width, height, refresh_hz, video_port,
                discovered_if[0] ? discovered_if : "route",
                input_allowed ? "allowed" : "off");

        int ready_fd = -1;
        pid_t video_pid = spawn_builtin_receiver(&cfg, video_port, width, height,
                                                 refresh_hz, discovered_if, &ready_fd);
        if (video_pid < 0) {
            perror("fork built-in video receiver");
            close(s);
            usleep((useconds_t)cfg.reconnect_ms * 1000u);
            continue;
        }
        if (wait_receiver_ready(video_pid, ready_fd, 10000) < 0) {
            fprintf(stderr, "video receiver failed to initialize: %s\n", strerror(errno));
            close(ready_fd);
            ndc_stop_child(&video_pid);
            close(s);
            usleep((useconds_t)cfg.reconnect_ms * 1000u);
            continue;
        }
        close(ready_fd);
        ready_fd = -1;

        if (ndc_send_msg(s, NDC_READY, NULL, 0) < 0) {
            ndc_stop_child(&video_pid);
            close(s);
            continue;
        }

        struct input_ctx ictx = {
            .sock = s,
            .grab = cfg.grab_input,
            .send_mutex = PTHREAD_MUTEX_INITIALIZER,
        };
        atomic_init(&ictx.running, input_allowed && cfg.want_input);
        pthread_t it;
        int have_it = 0;
        if (atomic_load(&ictx.running)) {
            int e = pthread_create(&it, NULL, input_main, &ictx);
            if (e) {
                errno = e;
                perror("pthread_create input");
                atomic_store(&ictx.running, 0);
            } else {
                have_it = 1;
            }
        }

        for (;;) {
            int st = 0;
            pid_t wr = waitpid(video_pid, &st, WNOHANG);
            if (wr == video_pid) {
                if (WIFEXITED(st))
                    fprintf(stderr, "built-in video receiver exited status=%d\n", WEXITSTATUS(st));
                else if (WIFSIGNALED(st))
                    fprintf(stderr, "built-in video receiver killed by signal %d\n", WTERMSIG(st));
                video_pid = -1;
                break;
            }

            struct pollfd pfd = { .fd = s, .events = POLLIN };
            int pr;
            do { pr = poll(&pfd, 1, 1000); } while (pr < 0 && errno == EINTR);
            if (pr < 0) break;
            if (pr == 0) {
                pthread_mutex_lock(&ictx.send_mutex);
                int sr = ndc_send_msg(s, NDC_PING, NULL, 0);
                pthread_mutex_unlock(&ictx.send_mutex);
                if (sr < 0) break;
                continue;
            }
            if (!(pfd.revents & POLLIN)) break;

            uint8_t b[NDC_MAX_PAYLOAD];
            uint32_t bl = sizeof(b);
            uint16_t bt = 0;
            int r = ndc_recv_msg(s, &bt, b, &bl);
            if (r <= 0) break;
            if (bt == NDC_PING && bl == 0) {
                pthread_mutex_lock(&ictx.send_mutex);
                int sr = ndc_send_msg(s, NDC_PONG, NULL, 0);
                pthread_mutex_unlock(&ictx.send_mutex);
                if (sr < 0) break;
            } else if (bt == NDC_PONG && bl == 0) {
                /* heartbeat reply */
            } else if (bt == NDC_STOP && bl == 0) {
                break;
            } else {
                fprintf(stderr, "protocol violation from source: message type %u\n", bt);
                break;
            }
        }

        atomic_store(&ictx.running, 0);
        shutdown(s, SHUT_RDWR);
        if (have_it) pthread_join(it, NULL);
        close(s);
        ndc_stop_child(&video_pid);
        fprintf(stderr, "source disconnected; discovering again\n");
        usleep((useconds_t)cfg.reconnect_ms * 1000u);
    }
}
