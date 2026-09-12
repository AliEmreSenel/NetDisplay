#define _GNU_SOURCE
#include "common.h"
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <spawn.h>
#include "proto.h"
#include "display_state.h"
#include "video_sender.h"

extern char **environ;
static const char *self_program;

struct host_cfg {
    char listen_addr[64];
    char allowed_peer[64];
    char output[128];
    char source_output[128];
    char brightness_device[256];
    int port;
    int video_port;
    int input_enabled;
    int width;
    int height;
    int refresh_hz;
    int qp;
    char connect_cmd[2048];
    char disconnect_cmd[2048];
};
static void cfg_defaults(struct host_cfg *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->listen_addr, sizeof(c->listen_addr), "0.0.0.0");
    c->port = NDC_DEFAULT_PORT;
    c->video_port = NDC_DEFAULT_VIDEO_PORT;
    c->input_enabled = 0;
    c->width = ND_DEFAULT_W;
    c->height = ND_DEFAULT_H;
    c->refresh_hz = ND_DEFAULT_FPS;
    c->qp = 24;
    snprintf(c->output, sizeof(c->output), "netdisplay");
    snprintf(c->brightness_device, sizeof(c->brightness_device), "auto");
}

static void load_cfg(const char *path, struct host_cfg *c)
{
    cfg_defaults(c);
    FILE *f = fopen(path, "r");
    if (!f) ndc_die(path);
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *s = ndc_trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq++ = 0;
        char *k = ndc_trim(s), *v = ndc_trim(eq);
        if (!strcmp(k, "listen")) snprintf(c->listen_addr, sizeof(c->listen_addr), "%s", v);
        else if (!strcmp(k, "allowed_peer")) snprintf(c->allowed_peer, sizeof(c->allowed_peer), "%s", v);
        else if (!strcmp(k, "port")) c->port = atoi(v);
        else if (!strcmp(k, "video_port")) c->video_port = atoi(v);
        else if (!strcmp(k, "input_enabled")) c->input_enabled = atoi(v) != 0;
        else if (!strcmp(k, "width")) c->width = atoi(v);
        else if (!strcmp(k, "height")) c->height = atoi(v);
        else if (!strcmp(k, "refresh_hz")) c->refresh_hz = atoi(v);
        else if (!strcmp(k, "qp")) c->qp = atoi(v);
        else if (!strcmp(k, "output")) snprintf(c->output, sizeof(c->output), "%s", v);
        else if (!strcmp(k, "source_output")) snprintf(c->source_output, sizeof(c->source_output), "%s", v);
        else if (!strcmp(k, "brightness_device")) snprintf(c->brightness_device, sizeof(c->brightness_device), "%s", v);
        else if (!strcmp(k, "connect_cmd")) snprintf(c->connect_cmd, sizeof(c->connect_cmd), "%s", v);
        else if (!strcmp(k, "disconnect_cmd")) snprintf(c->disconnect_cmd, sizeof(c->disconnect_cmd), "%s", v);
    }
    fclose(f);
    if (c->port <= 0 || c->port > 65535 || c->video_port <= 0 || c->video_port > 65535 ||
        c->qp < 0 || c->qp > 51 || !c->output[0] ||
        c->width <= 0 || c->width > UINT16_MAX || (c->width & 1) ||
        c->height <= 0 || c->height > UINT16_MAX || (c->height & 1) ||
        c->refresh_hz <= 0 || c->refresh_hz > UINT16_MAX) {
        fprintf(stderr, "invalid port/output/QP/video mode in %s\n", path);
        exit(2);
    }
}

static int ui_setup_abs(int fd, unsigned code, int min, int max, int res)
{
    if (ioctl(fd, UI_SET_ABSBIT, code) < 0) return -1;
#ifdef UI_ABS_SETUP
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof(a));
    a.code = (uint16_t)code;
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    a.absinfo.resolution = res;
    if (ioctl(fd, UI_ABS_SETUP, &a) < 0) return -1;
#else
    (void)min; (void)max; (void)res;
#endif
    return 0;
}

static int create_kbm_uinput(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_REL) < 0) goto fail;

    /* Keyboard keys, excluding the BTN_* block so this device is not mistaken
     * for a gamepad/touch device. */
    for (int code = 1; code <= KEY_MAX; code++) {
        if (code >= BTN_MISC && code <= BTN_GEAR_UP) continue;
        (void)ioctl(fd, UI_SET_KEYBIT, code);
    }
    int btns[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
                   BTN_FORWARD, BTN_BACK, BTN_TASK };
    for (size_t i = 0; i < sizeof(btns)/sizeof(btns[0]); i++)
        (void)ioctl(fd, UI_SET_KEYBIT, btns[i]);

    int rels[] = { REL_X, REL_Y, REL_WHEEL, REL_HWHEEL };
    for (size_t i = 0; i < sizeof(rels)/sizeof(rels[0]); i++)
        (void)ioctl(fd, UI_SET_RELBIT, rels[i]);
#ifdef REL_WHEEL_HI_RES
    (void)ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
#endif
#ifdef REL_HWHEEL_HI_RES
    (void)ioctl(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);
#endif

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x4e44;
    us.id.product = 1;
    snprintf(us.name, sizeof(us.name), "NetDisplay Keyboard Mouse");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) goto fail;
    return fd;
fail:
    close(fd); return -1;
}

static int create_touchpad_uinput(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
#ifdef UI_SET_PROPBIT
    (void)ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    (void)ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);
#endif
    int keys[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
                   BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP };
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
        (void)ioctl(fd, UI_SET_KEYBIT, keys[i]);
#ifdef BTN_TOOL_QUINTTAP
    (void)ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_QUINTTAP);
#endif

    /* 100 x 70 mm logical pad at 100 units/mm. Client normalizes to this. */
    if (ui_setup_abs(fd, ABS_X, 0, 10000, 100) < 0) goto fail;
    if (ui_setup_abs(fd, ABS_Y, 0, 7000, 100) < 0) goto fail;
    if (ui_setup_abs(fd, ABS_MT_POSITION_X, 0, 10000, 100) < 0) goto fail;
    if (ui_setup_abs(fd, ABS_MT_POSITION_Y, 0, 7000, 100) < 0) goto fail;
    if (ui_setup_abs(fd, ABS_MT_SLOT, 0, 15, 0) < 0) goto fail;
    if (ui_setup_abs(fd, ABS_MT_TRACKING_ID, -1, 65535, 0) < 0) goto fail;
    (void)ui_setup_abs(fd, ABS_PRESSURE, 0, 255, 0);
    (void)ui_setup_abs(fd, ABS_MT_PRESSURE, 0, 255, 0);
    (void)ui_setup_abs(fd, ABS_MT_TOUCH_MAJOR, 0, 255, 0);
    (void)ui_setup_abs(fd, ABS_MT_TOUCH_MINOR, 0, 255, 0);
    (void)ui_setup_abs(fd, ABS_MT_WIDTH_MAJOR, 0, 255, 0);
    (void)ui_setup_abs(fd, ABS_MT_WIDTH_MINOR, 0, 255, 0);
    (void)ui_setup_abs(fd, ABS_MT_ORIENTATION, -127, 127, 0);
#ifdef ABS_MT_DISTANCE
    (void)ui_setup_abs(fd, ABS_MT_DISTANCE, 0, 255, 0);
#endif
#ifdef ABS_MT_TOOL_TYPE
    (void)ui_setup_abs(fd, ABS_MT_TOOL_TYPE, 0, 2, 0);
#endif

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x4e44;
    us.id.product = 2;
    snprintf(us.name, sizeof(us.name), "NetDisplay Touchpad");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) goto fail;
    return fd;
fail:
    close(fd); return -1;
}

static void destroy_ui(int *fd)
{
    if (*fd >= 0) { (void)ioctl(*fd, UI_DEV_DESTROY); close(*fd); *fd = -1; }
}

static int inject_event(int kbm, int tp, const struct ndc_input *w)
{
    int fd = w->device == NDC_DEV_TOUCHPAD ? tp : kbm;
    if (fd < 0) return 0;
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ntohs(w->type);
    ev.code = ntohs(w->code);
    ev.value = (int32_t)ntohl((uint32_t)w->value);
    if (ev.type != EV_SYN && ev.type != EV_KEY && ev.type != EV_REL && ev.type != EV_ABS)
        return 0;
    ssize_t n;
    do { n = write(fd, &ev, sizeof(ev)); } while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int make_listener(const struct host_cfg *cfg)
{
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) ndc_die("socket");
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)cfg->port) };
    if (!strcmp(cfg->listen_addr, "0.0.0.0")) a.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, cfg->listen_addr, &a.sin_addr) != 1) {
        fprintf(stderr, "bad listen address %s\n", cfg->listen_addr); exit(2);
    }
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) ndc_die("bind control");
    if (listen(s, 4) < 0) ndc_die("listen");
    return s;
}

static int peer_allowed(const struct host_cfg *cfg, const char *ip)
{
    return !cfg->allowed_peer[0] || !strcmp(cfg->allowed_peer, ip);
}

static _Atomic int session_active;

static int make_discovery_socket(const struct host_cfg *cfg)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) ndc_die("socket discovery");
    int one = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)cfg->port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
        ndc_die("bind discovery");
    return s;
}

static void *discovery_main(void *opaque)
{
    const struct host_cfg *cfg = opaque;
    int s = make_discovery_socket(cfg);
    for (;;) {
        struct ndc_discovery d;
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n;
        do { n = recvfrom(s, &d, sizeof(d), 0, (struct sockaddr *)&from, &fl); }
        while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof(d)) continue;
        if (ntohl(d.magic) != NDC_DISC_MAGIC || ntohs(d.version) != NDC_VERSION ||
            ntohs(d.type) != NDC_DISCOVER) continue;
        if (atomic_load_explicit(&session_active, memory_order_relaxed)) continue;

        char peer[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer))) continue;
        if (!peer_allowed(cfg, peer)) continue;

        struct ndc_discovery offer = {
            .magic = htonl(NDC_DISC_MAGIC),
            .version = htons(NDC_VERSION),
            .type = htons(NDC_OFFER),
            .nonce = d.nonce,
            .tcp_port = htons((uint16_t)cfg->port),
            .reserved = 0,
        };
        (void)sendto(s, &offer, sizeof(offer), 0,
                     (struct sockaddr *)&from, sizeof(from));
    }
    return NULL;
}


static pid_t spawn_builtin_sender(const struct host_cfg *cfg, const char *peer)
{
    char portbuf[16], qpbuf[16], widthbuf[16], heightbuf[16], refreshbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", cfg->video_port);
    snprintf(qpbuf, sizeof(qpbuf), "%d", cfg->qp);
    snprintf(widthbuf, sizeof(widthbuf), "%d", cfg->width);
    snprintf(heightbuf, sizeof(heightbuf), "%d", cfg->height);
    snprintf(refreshbuf, sizeof(refreshbuf), "%d", cfg->refresh_hz);
    char *const av[] = {
        (char *)self_program,
        (char *)"--video-sender",
        (char *)cfg->output,
        (char *)peer,
        portbuf,
        qpbuf,
        widthbuf,
        heightbuf,
        refreshbuf,
        NULL,
    };

    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) { errno = EINVAL; return -1; }
    short flags = POSIX_SPAWN_SETPGROUP;
    (void)posix_spawnattr_setflags(&attr, flags);
    (void)posix_spawnattr_setpgroup(&attr, 0);

    pid_t p = -1;
    int e = posix_spawnp(&p, self_program, NULL, &attr, av, environ);
    posix_spawnattr_destroy(&attr);
    if (e != 0) { errno = e; return -1; }
    return p;
}

static int wait_for_ready(int fd, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int pr;
    do { pr = poll(&p, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) { if (pr == 0) errno = ETIMEDOUT; return -1; }
    uint8_t payload[NDC_MAX_PAYLOAD];
    uint32_t len = sizeof(payload);
    uint16_t type = 0;
    int r = ndc_recv_msg(fd, &type, payload, &len);
    if (r <= 0) return -1;
    if (type != NDC_READY || len != 0) { errno = EPROTO; return -1; }
    return 0;
}

static void set_session_env(const struct host_cfg *cfg, const char *peer)
{
    char b[32];
    setenv("ND_PEER_IP", peer, 1);
    setenv("ND_OUTPUT", cfg->output, 1);
    snprintf(b, sizeof(b), "%d", cfg->video_port); setenv("ND_VIDEO_PORT", b, 1);
    snprintf(b, sizeof(b), "%d", cfg->port); setenv("ND_CONTROL_PORT", b, 1);
    snprintf(b, sizeof(b), "%d", cfg->width); setenv("ND_WIDTH", b, 1);
    snprintf(b, sizeof(b), "%d", cfg->height); setenv("ND_HEIGHT", b, 1);
    snprintf(b, sizeof(b), "%d", cfg->refresh_hz); setenv("ND_REFRESH_HZ", b, 1);
}

int main(int argc, char **argv)
{
    self_program = argv[0];
    if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("netdisplay-server %s protocol=%u default-video=%ux%u@%u\n",
               NETDISPLAY_VERSION, NDC_VERSION,
               ND_DEFAULT_W, ND_DEFAULT_H, ND_DEFAULT_FPS);
        return 0;
    }
    if (argc == 9 && !strcmp(argv[1], "--video-sender"))
        return nd_video_sender_run(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                                   atoi(argv[6]), atoi(argv[7]), atoi(argv[8]));

    if (argc != 2) {
        fprintf(stderr, "usage: %s SERVER_CONFIG\n", argv[0]);
        return 2;
    }
    struct host_cfg cfg;
    load_cfg(argv[1], &cfg);
    struct nd_display_state_source display_state;
    if (nd_display_state_start(&display_state, cfg.output, cfg.source_output,
                               cfg.brightness_device) < 0)
        ndc_die("start display-state watchers");
    int ls = make_listener(&cfg);
    pthread_t discovery_thread;
    int de = pthread_create(&discovery_thread, NULL, discovery_main, &cfg);
    if (de != 0) { errno = de; ndc_die("pthread_create discovery"); }
    pthread_detach(discovery_thread);

    fprintf(stderr,
            "netdisplay-server: control/discovery %s:%d, video %dx%d@%d UDP %d, output %s, QP %d, input %s\n",
            cfg.listen_addr, cfg.port, cfg.width, cfg.height, cfg.refresh_hz,
            cfg.video_port, cfg.output, cfg.qp,
            cfg.input_enabled ? "ENABLED" : "disabled");

    for (;;) {
        struct sockaddr_in pa;
        socklen_t plen = sizeof(pa);
        int c = accept4(ls, (struct sockaddr *)&pa, &plen, SOCK_CLOEXEC);
        if (c < 0) { if (errno == EINTR) continue; ndc_die("accept"); }
        ndc_set_tcp_opts(c);

        char peer[INET_ADDRSTRLEN] = "?";
        (void)inet_ntop(AF_INET, &pa.sin_addr, peer, sizeof(peer));
        if (!peer_allowed(&cfg, peer)) {
            fprintf(stderr, "reject receiver %s\n", peer);
            close(c);
            continue;
        }

        uint8_t payload[NDC_MAX_PAYLOAD];
        uint32_t len = sizeof(payload);
        uint16_t type = 0;
        int rr = ndc_recv_msg(c, &type, payload, &len);
        if (rr <= 0 || type != NDC_HELLO || len != sizeof(struct ndc_hello)) {
            fprintf(stderr, "bad HELLO from %s\n", peer);
            close(c);
            continue;
        }
        struct ndc_hello hello;
        memcpy(&hello, payload, sizeof(hello));
        uint32_t want = ntohl(hello.flags);

        atomic_store_explicit(&session_active, 1, memory_order_relaxed);
        set_session_env(&cfg, peer);
        fprintf(stderr, "receiver %s connected\n", peer);

        int lifecycle_started = 1;
        if (cfg.connect_cmd[0]) {
            int rc = ndc_run_sync(cfg.connect_cmd);
            if (rc) fprintf(stderr, "connect_cmd exited %d\n", rc);
        }

        int kbm = -1, tp = -1;
        int input_allowed = cfg.input_enabled && (want & NDC_FLAG_WANT_INPUT);
        if (input_allowed) {
            kbm = create_kbm_uinput();
            tp = create_touchpad_uinput();
            if (kbm < 0 || tp < 0) {
                fprintf(stderr,
                        "cannot create /dev/uinput devices: %s; input disabled for this session\n",
                        strerror(errno));
                destroy_ui(&kbm); destroy_ui(&tp);
                input_allowed = 0;
            }
        }

        struct ndc_welcome wel = {
            .flags = htonl(input_allowed ? NDC_FLAG_INPUT_ALLOWED : 0),
            .video_port = htons((uint16_t)cfg.video_port),
            .width = htons((uint16_t)cfg.width),
            .height = htons((uint16_t)cfg.height),
            .refresh_hz = htons((uint16_t)cfg.refresh_hz),
        };

        pid_t video_pid = -1;
        int session_ok = 1;
        if (ndc_send_msg(c, NDC_WELCOME, &wel, sizeof(wel)) < 0) {
            session_ok = 0;
        } else if (wait_for_ready(c, 10000) < 0) {
            fprintf(stderr, "receiver %s did not become video-ready: %s\n",
                    peer, strerror(errno));
            session_ok = 0;
        } else {
            if (nd_display_state_attach(&display_state, c) < 0) {
                session_ok = 0;
            }
        }

        if (session_ok) {
            video_pid = spawn_builtin_sender(&cfg, peer);
            if (video_pid < 0) {
                perror("fork built-in video sender");
                (void)nd_display_state_send(&display_state, c, NDC_STOP, NULL, 0);
                session_ok = 0;
            } else {
                fprintf(stderr, "session %s started: built-in video pid %ld, input %s\n",
                        peer, (long)video_pid, input_allowed ? "allowed" : "off");
            }
        }

        while (session_ok) {
            int st = 0;
            pid_t wr = waitpid(video_pid, &st, WNOHANG);
            if (wr == video_pid) {
                if (WIFEXITED(st))
                    fprintf(stderr, "built-in video sender exited status=%d\n", WEXITSTATUS(st));
                else if (WIFSIGNALED(st))
                    fprintf(stderr, "built-in video sender killed by signal %d\n", WTERMSIG(st));
                else
                    fprintf(stderr, "built-in video sender exited\n");
                video_pid = -1;
                (void)nd_display_state_send(&display_state, c, NDC_STOP, NULL, 0);
                break;
            }

            struct pollfd pfd = { .fd = c, .events = POLLIN };
            int pr;
            do { pr = poll(&pfd, 1, 500); } while (pr < 0 && errno == EINTR);
            if (pr < 0) break;
            if (pr == 0) continue;
            if (!(pfd.revents & POLLIN)) break;

            uint8_t b[NDC_MAX_PAYLOAD];
            uint32_t blen = sizeof(b);
            uint16_t bt = 0;
            int r = ndc_recv_msg(c, &bt, b, &blen);
            if (r <= 0) break;
            if (bt == NDC_INPUT && input_allowed && blen == sizeof(struct ndc_input)) {
                struct ndc_input in;
                memcpy(&in, b, sizeof(in));
                if (inject_event(kbm, tp, &in) < 0) {
                    fprintf(stderr, "uinput write failed: %s\n", strerror(errno));
                    break;
                }
            } else if (bt == NDC_PING && blen == 0) {
                if (nd_display_state_send(&display_state, c, NDC_PONG, NULL, 0) < 0) break;
            } else if (bt == NDC_PONG && blen == 0) {
                /* heartbeat reply */
            } else {
                fprintf(stderr, "protocol violation from receiver: message type %u\n", bt);
                break;
            }
        }

        fprintf(stderr, "receiver %s disconnected\n", peer);
        nd_display_state_detach(&display_state, c);
        destroy_ui(&kbm); destroy_ui(&tp);
        ndc_stop_child(&video_pid);
        close(c);
        if (lifecycle_started && cfg.disconnect_cmd[0]) {
            int rc = ndc_run_sync(cfg.disconnect_cmd);
            if (rc) fprintf(stderr, "disconnect_cmd exited %d\n", rc);
        }
        atomic_store_explicit(&session_active, 0, memory_order_relaxed);
    }
}
