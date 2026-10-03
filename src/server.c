// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include "common.h"
#include "crypto.h"
#include "display_state.h"
#include "proto.h"
#include "network_test.h"
#include "power_native.h"
#include "video_sender.h"
#include <dirent.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <sodium/core.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

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
  int power_devices;
  int qp;
  uint16_t video_codec; /* 0=auto */
  int max_clients;
  int max_displays_per_client;
  int max_width, max_height, max_fps;
  int frame_encryption; /* 0=off, 1=allowed, 2=required */
  char psk_file[512];
  char password_key[ND_KEY_SIZE * 2u + 1u];
  uint8_t psk[ND_KEY_SIZE];
  int have_psk;
  int password_auth;
  char connect_cmd[2048];
  char disconnect_cmd[2048];
};
static void cfg_defaults(struct host_cfg *c) {
  memset(c, 0, sizeof(*c));
  snprintf(c->listen_addr, sizeof(c->listen_addr), "0.0.0.0");
  c->port = NDC_DEFAULT_PORT;
  c->video_port = NDC_DEFAULT_VIDEO_PORT;
  c->input_enabled = 0;
  c->power_devices = 1;
  c->qp = 24;
  c->max_clients = 8;
  c->max_displays_per_client = NDC_MAX_DISPLAYS;
  c->max_width = 8192;
  c->max_height = 8192;
  c->max_fps = 240;
  c->frame_encryption = 0;
  snprintf(c->output, sizeof(c->output), "netdisplay");
  snprintf(c->brightness_device, sizeof(c->brightness_device), "auto");
}

static void load_cfg(const char *path, struct host_cfg *c) {
  cfg_defaults(c);
  FILE *f = fopen(path, "r");
  if (!f)
    die(path);
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    char *s = ndc_trim(line);
    if (!*s || *s == '#')
      continue;
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq++ = 0;
    char *k = ndc_trim(s), *v = ndc_trim(eq);
    if (!strcmp(k, "listen"))
      snprintf(c->listen_addr, sizeof(c->listen_addr), "%s", v);
    else if (!strcmp(k, "allowed_peer"))
      snprintf(c->allowed_peer, sizeof(c->allowed_peer), "%s", v);
    else if (!strcmp(k, "port"))
      c->port = atoi(v);
    else if (!strcmp(k, "video_port"))
      c->video_port = atoi(v);
    else if (!strcmp(k, "power_devices"))
      c->power_devices = atoi(v) != 0;
    else if (!strcmp(k, "input_enabled"))
      c->input_enabled = atoi(v) != 0;
    else if (!strcmp(k, "qp"))
      c->qp = atoi(v);
    else if (!strcmp(k, "video_codec")) {
      if (!strcmp(v, "auto")) c->video_codec = 0;
      else if (!strcmp(v, "av1")) c->video_codec = NDC_VIDEO_AV1;
      else if (!strcmp(v, "hevc") || !strcmp(v, "h265")) c->video_codec = NDC_VIDEO_HEVC;
      else if (!strcmp(v, "h264")) c->video_codec = NDC_VIDEO_H264;
      else { fprintf(stderr, "invalid video_codec in %s\n", path); exit(2); }
    }
    else if (!strcmp(k, "max_clients"))
      c->max_clients = atoi(v);
    else if (!strcmp(k, "max_displays_per_client"))
      c->max_displays_per_client = atoi(v);
    else if (!strcmp(k, "max_width"))
      c->max_width = atoi(v);
    else if (!strcmp(k, "max_height"))
      c->max_height = atoi(v);
    else if (!strcmp(k, "max_fps"))
      c->max_fps = atoi(v);
    else if (!strcmp(k, "psk_file"))
      snprintf(c->psk_file, sizeof(c->psk_file), "%s", v);
    else if (!strcmp(k, "password_key")) {
      if (strlen(v) >= sizeof(c->password_key)) {
        fprintf(stderr, "password_key is too long in %s\n", path);
        exit(2);
      }
      snprintf(c->password_key, sizeof(c->password_key), "%s", v);
    } else if (!strcmp(k, "password") && *v) {
      fprintf(stderr, "plaintext password is not supported; store password_key "
                      "from --derive-password-key\n");
      exit(2);
    } else if (!strcmp(k, "frame_encryption")) {
      if (!strcmp(v, "off"))
        c->frame_encryption = 0;
      else if (!strcmp(v, "allowed"))
        c->frame_encryption = 1;
      else if (!strcmp(v, "required"))
        c->frame_encryption = 2;
      else {
        fprintf(stderr, "invalid frame_encryption in %s\n", path);
        exit(2);
      }
    } else if (!strcmp(k, "output"))
      snprintf(c->output, sizeof(c->output), "%s", v);
    else if (!strcmp(k, "source_output"))
      snprintf(c->source_output, sizeof(c->source_output), "%s", v);
    else if (!strcmp(k, "brightness_device"))
      snprintf(c->brightness_device, sizeof(c->brightness_device), "%s", v);
    else if (!strcmp(k, "connect_cmd"))
      snprintf(c->connect_cmd, sizeof(c->connect_cmd), "%s", v);
    else if (!strcmp(k, "disconnect_cmd"))
      snprintf(c->disconnect_cmd, sizeof(c->disconnect_cmd), "%s", v);
  }
  fclose(f);
  if (c->port <= 0 || c->port > 65535 || c->video_port <= 0 ||
      c->video_port > 65535 || c->qp < 0 || c->qp > 51 || !c->output[0] ||
      c->max_clients <= 0 || c->max_clients > 32 ||
      c->max_displays_per_client <= 0 ||
      c->max_displays_per_client > (int)NDC_MAX_DISPLAYS || c->max_width <= 0 ||
      c->max_width > UINT16_MAX || c->max_height <= 0 ||
      c->max_height > UINT16_MAX || c->max_fps <= 0 ||
      c->max_fps > UINT16_MAX ||
      c->video_port + c->max_displays_per_client - 1 > 65535) {
    fprintf(stderr, "invalid port/output/QP/client limit in %s\n", path);
    exit(2);
  }
  if (c->psk_file[0] && c->password_key[0]) {
    fprintf(stderr, "configure password_key or psk_file, not both\n");
    exit(2);
  }
  if (c->psk_file[0]) {
    if (nd_crypto_load_psk(c->psk_file, c->psk) < 0)
      die(c->psk_file);
    c->have_psk = 1;
  } else if (c->password_key[0]) {
    if (nd_crypto_key_from_hex(c->psk, c->password_key) < 0) {
      fprintf(stderr,
              "password_key must contain exactly 64 hexadecimal characters\n");
      exit(2);
    }
    c->have_psk = 1;
    c->password_auth = 1;
  }
  if (c->frame_encryption && !c->have_psk) {
    fprintf(stderr, "frame_encryption requires password_key or psk_file\n");
    exit(2);
  }
  if (c->password_auth && c->frame_encryption == 0)
    fprintf(stderr,
            "warning: password authentication is configured but "
            "frame_encryption=off; video UDP will remain plaintext\n");
}

static int derive_password_key_cli(void) {
  char password[1024] = "", confirm[1024] = "";
  if (isatty(STDIN_FILENO)) {
    char *p = getpass("Password: ");
    if (!p || !*p || strlen(p) >= sizeof(password))
      goto invalid;
    snprintf(password, sizeof(password), "%s", p);
    explicit_bzero(p, strlen(p));
    p = getpass("Confirm password: ");
    if (!p || strlen(p) >= sizeof(confirm))
      goto invalid;
    snprintf(confirm, sizeof(confirm), "%s", p);
    explicit_bzero(p, strlen(p));
    if (strcmp(password, confirm)) {
      fprintf(stderr, "passwords do not match\n");
      goto invalid;
    }
  } else {
    if (!fgets(password, sizeof(password), stdin))
      goto invalid;
    size_t n = strcspn(password, "\r\n");
    if (!password[n] && !feof(stdin))
      goto invalid;
    password[n] = 0;
    if (!password[0])
      goto invalid;
  }
  uint8_t key[ND_KEY_SIZE];
  char hex[ND_KEY_SIZE * 2u + 1u];
  if (nd_crypto_password_key(key, password) < 0) {
    explicit_bzero(password, sizeof(password));
    explicit_bzero(confirm, sizeof(confirm));
    return 1;
  }
  nd_crypto_key_to_hex(hex, key);
  puts(hex);
  explicit_bzero(key, sizeof(key));
  explicit_bzero(hex, sizeof(hex));
  explicit_bzero(password, sizeof(password));
  explicit_bzero(confirm, sizeof(confirm));
  return 0;
invalid:
  fprintf(stderr, "a non-empty password shorter than 1024 bytes is required\n");
  explicit_bzero(password, sizeof(password));
  explicit_bzero(confirm, sizeof(confirm));
  return 2;
}

static int ui_setup_abs(int fd, unsigned code, int min, int max, int res) {
  if (ioctl(fd, UI_SET_ABSBIT, code) < 0)
    return -1;
#ifdef UI_ABS_SETUP
  struct uinput_abs_setup a;
  memset(&a, 0, sizeof(a));
  a.code = (uint16_t)code;
  a.absinfo.minimum = min;
  a.absinfo.maximum = max;
  a.absinfo.resolution = res;
  if (ioctl(fd, UI_ABS_SETUP, &a) < 0)
    return -1;
#else
  (void)min;
  (void)max;
  (void)res;
#endif
  return 0;
}

static int create_kbm_uinput(void) {
  int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(fd, UI_SET_EVBIT, EV_REL) < 0)
    goto fail;

  /* Keyboard keys, excluding the BTN_* block so this device is not mistaken
   * for a gamepad/touch device. */
  for (int code = 1; code <= KEY_MAX; code++) {
    if (code >= BTN_MISC && code <= BTN_GEAR_UP)
      continue;
    (void)ioctl(fd, UI_SET_KEYBIT, code);
  }
  int btns[] = {BTN_LEFT,  BTN_RIGHT,   BTN_MIDDLE, BTN_SIDE,
                BTN_EXTRA, BTN_FORWARD, BTN_BACK,   BTN_TASK};
  for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++)
    (void)ioctl(fd, UI_SET_KEYBIT, btns[i]);

  int rels[] = {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL};
  for (size_t i = 0; i < sizeof(rels) / sizeof(rels[0]); i++)
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
  if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
    goto fail;
  return fd;
fail:
  close(fd);
  return -1;
}

static int create_touchpad_uinput(void) {
  int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
    goto fail;
#ifdef UI_SET_PROPBIT
  (void)ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
  (void)ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);
#endif
  int keys[] = {BTN_LEFT,           BTN_TOUCH,          BTN_TOOL_FINGER,
                BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    (void)ioctl(fd, UI_SET_KEYBIT, keys[i]);
#ifdef BTN_TOOL_QUINTTAP
  (void)ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_QUINTTAP);
#endif

  /* 100 x 70 mm logical pad at 100 units/mm. Client normalizes to this. */
  if (ui_setup_abs(fd, ABS_X, 0, 10000, 100) < 0)
    goto fail;
  if (ui_setup_abs(fd, ABS_Y, 0, 7000, 100) < 0)
    goto fail;
  if (ui_setup_abs(fd, ABS_MT_POSITION_X, 0, 10000, 100) < 0)
    goto fail;
  if (ui_setup_abs(fd, ABS_MT_POSITION_Y, 0, 7000, 100) < 0)
    goto fail;
  if (ui_setup_abs(fd, ABS_MT_SLOT, 0, 15, 0) < 0)
    goto fail;
  if (ui_setup_abs(fd, ABS_MT_TRACKING_ID, -1, 65535, 0) < 0)
    goto fail;
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
  if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
    goto fail;
  return fd;
fail:
  close(fd);
  return -1;
}

static void destroy_ui(int *fd) {
  if (*fd >= 0) {
    (void)ioctl(*fd, UI_DEV_DESTROY);
    close(*fd);
    *fd = -1;
  }
}

static int inject_event(int kbm, int tp, const struct ndc_input *w) {
  int fd = w->device == NDC_DEV_TOUCHPAD ? tp : kbm;
  if (fd < 0)
    return 0;
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = ntohs(w->type);
  ev.code = ntohs(w->code);
  ev.value = (int32_t)ntohl((uint32_t)w->value);
  if (ev.type != EV_SYN && ev.type != EV_KEY && ev.type != EV_REL &&
      ev.type != EV_ABS)
    return 0;
  ssize_t n;
  do {
    n = write(fd, &ev, sizeof(ev));
  } while (n < 0 && errno == EINTR);
  return n == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int make_listener(const struct host_cfg *cfg) {
  int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0)
    die("socket");
  int one = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_port = htons((uint16_t)cfg->port)};
  if (!strcmp(cfg->listen_addr, "0.0.0.0"))
    a.sin_addr.s_addr = htonl(INADDR_ANY);
  else if (inet_pton(AF_INET, cfg->listen_addr, &a.sin_addr) != 1) {
    fprintf(stderr, "bad listen address %s\n", cfg->listen_addr);
    exit(2);
  }
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
    die("bind control");
  if (listen(s, cfg->max_clients) < 0)
    die("listen");
  return s;
}

static int peer_allowed(const struct host_cfg *cfg, const char *ip) {
  return !cfg->allowed_peer[0] || !strcmp(cfg->allowed_peer, ip);
}

static int valid_wire_name(const char *name) {
  if (!name || !*name)
    return 0;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
      return 0;
  return 1;
}

static _Atomic int session_count;

static int make_discovery_socket(const struct host_cfg *cfg) {
  int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (s < 0)
    die("socket discovery");
  int one = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a = {
      .sin_family = AF_INET,
      .sin_port = htons((uint16_t)cfg->port),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
    die("bind discovery");
  return s;
}

static void *discovery_main(void *opaque) {
  const struct host_cfg *cfg = opaque;
  int s = make_discovery_socket(cfg);
  for (;;) {
    struct ndc_discovery d;
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    ssize_t n;
    do {
      n = recvfrom(s, &d, sizeof(d), 0, (struct sockaddr *)&from, &fl);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(d))
      continue;
    if (ntohl(d.magic) != NDC_DISC_MAGIC || ntohs(d.version) != NDC_VERSION ||
        ntohs(d.type) != NDC_DISCOVER)
      continue;
    if (atomic_load_explicit(&session_count, memory_order_relaxed) >=
        cfg->max_clients)
      continue;

    char peer[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer)))
      continue;
    if (!peer_allowed(cfg, peer))
      continue;

    struct ndc_discovery offer = {
        .magic = htonl(NDC_DISC_MAGIC),
        .version = htons(NDC_VERSION),
        .type = htons(NDC_OFFER),
        .nonce = d.nonce,
        .tcp_port = htons((uint16_t)cfg->port),
        .reserved = 0,
    };
    (void)sendto(s, &offer, sizeof(offer), 0, (struct sockaddr *)&from,
                 sizeof(from));
  }
  return NULL;
}

struct server_stream {
  uint32_t display_id;
  uint64_t stream_id;
  int port, width, height, refresh_hz;
  uint16_t video_codec;
  char connector[NDC_NAME_MAX];
  char output[NDC_NAME_MAX];
  char encoder_name[64];
  uint8_t key[ND_KEY_SIZE];
  pid_t video_pid;
  int lifecycle_started;
};

struct session_ctx {
  const struct host_cfg *cfg;
  struct nd_display_state_source *display_state;
  int fd;
  char peer[INET_ADDRSTRLEN];
  unsigned serial;
};

static _Atomic unsigned next_session_serial = 1;

static pid_t spawn_builtin_sender(const struct host_cfg *cfg, const char *peer,
                                  const struct server_stream *st,
                                  int encrypted) {
  enum { CHILD_KEY_FD = 196 };
  char portbuf[16], qpbuf[16], widthbuf[16], heightbuf[16], refreshbuf[16];
  char sessionbuf[32], encryptedbuf[8], keyfdbuf[16], codecbuf[16];
  snprintf(portbuf, sizeof(portbuf), "%d", st->port);
  snprintf(qpbuf, sizeof(qpbuf), "%d", cfg->qp);
  snprintf(widthbuf, sizeof(widthbuf), "%d", st->width);
  snprintf(heightbuf, sizeof(heightbuf), "%d", st->height);
  snprintf(refreshbuf, sizeof(refreshbuf), "%d", st->refresh_hz);
  snprintf(sessionbuf, sizeof(sessionbuf), "%llu",
           (unsigned long long)st->stream_id);
  snprintf(encryptedbuf, sizeof(encryptedbuf), "%d", encrypted);
  snprintf(keyfdbuf, sizeof(keyfdbuf), "%d", encrypted ? CHILD_KEY_FD : -1);
  snprintf(codecbuf, sizeof(codecbuf), "%u", (unsigned)st->video_codec);
  char *const av[] = {
      (char *)self_program,
      (char *)"--video-sender",
      (char *)st->output,
      (char *)peer,
      portbuf,
      qpbuf,
      widthbuf,
      heightbuf,
      refreshbuf,
      sessionbuf,
      encryptedbuf,
      keyfdbuf,
      codecbuf,
      (char *)st->encoder_name,
      NULL,
  };

  int keypipe[2] = {-1, -1};
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  int e = posix_spawn_file_actions_init(&fa);
  if (e != 0) {
    errno = e;
    return -1;
  }
  if (encrypted) {
    if (pipe2(keypipe, O_CLOEXEC) < 0) {
      posix_spawn_file_actions_destroy(&fa);
      return -1;
    }
    e = posix_spawn_file_actions_adddup2(&fa, keypipe[0], CHILD_KEY_FD);
    if (!e)
      e = posix_spawn_file_actions_addclose(&fa, keypipe[1]);
    if (e)
      goto fail;
  }
  e = posix_spawnattr_init(&attr);
  if (e != 0)
    goto fail;
  short flags = POSIX_SPAWN_SETPGROUP;
  (void)posix_spawnattr_setflags(&attr, flags);
  (void)posix_spawnattr_setpgroup(&attr, 0);

  pid_t p = -1;
  e = posix_spawnp(&p, self_program, &fa, &attr, av, environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  if (keypipe[0] >= 0)
    close(keypipe[0]);
  if (e != 0) {
    if (keypipe[1] >= 0)
      close(keypipe[1]);
    errno = e;
    return -1;
  }
  if (encrypted) {
    size_t off = 0;
    int ok = 0;
    while (off < sizeof(st->key)) {
      ssize_t n = write(keypipe[1], st->key + off, sizeof(st->key) - off);
      if (n > 0) {
        off += (size_t)n;
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      ok = -1;
      break;
    }
    close(keypipe[1]);
    if (ok < 0) {
      ndc_stop_child(&p);
      return -1;
    }
  }
  return p;
fail:
  posix_spawn_file_actions_destroy(&fa);
  if (keypipe[0] >= 0)
    close(keypipe[0]);
  if (keypipe[1] >= 0)
    close(keypipe[1]);
  errno = e;
  return -1;
}

static int recv_expected(int fd, uint16_t expected, void *payload,
                         uint32_t expected_len) {
  uint32_t len = expected_len;
  uint16_t type = 0;
  int r = ndc_recv_msg(fd, &type, payload, &len);
  if (r <= 0 || type != expected || len != expected_len) {
    if (r > 0)
      errno = EPROTO;
    return -1;
  }
  return 0;
}

static int run_stream_hook(const struct host_cfg *cfg, const char *peer,
                           const struct server_stream *st, const char *cmd) {
  char vars[9][256];
  snprintf(vars[0], sizeof(vars[0]), "ND_PEER_IP=%s", peer);
  snprintf(vars[1], sizeof(vars[1]), "ND_OUTPUT=%s", st->output);
  snprintf(vars[2], sizeof(vars[2]), "ND_CONNECTOR=%s", st->connector);
  snprintf(vars[3], sizeof(vars[3]), "ND_VIDEO_PORT=%d", st->port);
  snprintf(vars[4], sizeof(vars[4]), "ND_CONTROL_PORT=%d", cfg->port);
  snprintf(vars[5], sizeof(vars[5]), "ND_WIDTH=%d", st->width);
  snprintf(vars[6], sizeof(vars[6]), "ND_HEIGHT=%d", st->height);
  snprintf(vars[7], sizeof(vars[7]), "ND_REFRESH_HZ=%d", st->refresh_hz);
  snprintf(vars[8], sizeof(vars[8]), "ND_DISPLAY_ID=%u", st->display_id);

  size_t inherited = 0;
  while (environ[inherited])
    inherited++;
  char **envp = calloc(inherited + 10u, sizeof(*envp));
  if (!envp)
    return -1;
  size_t n = 0;
  for (size_t i = 0; i < inherited; i++) {
    int replace = 0;
    for (size_t v = 0; v < 9; v++) {
      size_t key_len = (size_t)(strchr(vars[v], '=') - vars[v]);
      if (!strncmp(environ[i], vars[v], key_len) &&
          environ[i][key_len] == '=') {
        replace = 1;
        break;
      }
    }
    if (!replace)
      envp[n++] = environ[i];
  }
  for (size_t v = 0; v < 9; v++)
    envp[n++] = vars[v];
  envp[n] = NULL;

  char *const av[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
  pid_t pid = -1;
  int e = posix_spawn(&pid, "/bin/sh", NULL, NULL, av, envp);
  free(envp);
  if (e) {
    errno = e;
    return -1;
  }
  int status;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      return -1;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static void *session_main(void *opaque) {
  struct session_ctx *ctx = opaque;
  const struct host_cfg *cfg = ctx->cfg;
  int c = ctx->fd;
  struct server_stream streams[NDC_MAX_DISPLAYS];
  memset(streams, 0, sizeof(streams));
  for (unsigned i = 0; i < NDC_MAX_DISPLAYS; i++)
    streams[i].video_pid = -1;
  int kbm = -1, tp = -1, attached = 0;
  unsigned count = 0;
  struct nd_power_rx power_rx = {0};
  struct nd_power_sink power_sink;
  nd_power_sink_init(&power_sink);

  struct ndc_hello hello;
  if (recv_expected(c, NDC_HELLO, &hello, sizeof(hello)) < 0)
    goto done;
  uint32_t requested = ntohl(hello.flags);
  uint32_t client_codecs = ntohl(hello.video_codecs) & NDC_CODEC_ALL;
  unsigned advertised_count = ntohs(hello.display_count);
  if (!advertised_count || !client_codecs ||
      advertised_count > (unsigned)cfg->max_displays_per_client)
    goto done;
  count = advertised_count;

  int wants_encryption = !!(requested & NDC_FLAG_FRAME_ENCRYPT);
  if ((wants_encryption && cfg->frame_encryption == 0) ||
      (!wants_encryption && cfg->frame_encryption == 2) ||
      (wants_encryption && !cfg->have_psk)) {
    (void)ndc_send_msg(c, NDC_REJECT, NULL, 0);
    goto done;
  }
  int encrypted = wants_encryption && cfg->frame_encryption != 0;
  struct ndc_challenge challenge = {
      .flags = htonl((cfg->have_psk ? NDC_FLAG_HAVE_PSK : 0) |
                     (cfg->password_auth ? NDC_FLAG_PASSWORD : 0) |
                     (encrypted ? NDC_FLAG_FRAME_ENCRYPT : 0)),
  };
  nd_crypto_random(challenge.nonce, sizeof(challenge.nonce));
  if (ndc_send_msg(c, NDC_CHALLENGE, &challenge, sizeof(challenge)) < 0)
    goto done;
  if (cfg->have_psk) {
    struct ndc_auth got, reply;
    uint8_t expected[NDC_AUTH_SIZE];
    if (recv_expected(c, NDC_AUTH, &got, sizeof(got)) < 0)
      goto done;
    nd_crypto_proof(expected, cfg->psk, "client", hello.nonce, challenge.nonce);
    if (!nd_crypto_verify(got.proof, expected)) {
      fprintf(stderr, "receiver %s authentication failed\n", ctx->peer);
      (void)ndc_send_msg(c, NDC_REJECT, NULL, 0);
      goto done;
    }
    nd_crypto_proof(reply.proof, cfg->psk, "server", hello.nonce,
                    challenge.nonce);
    if (ndc_send_msg(c, NDC_AUTH, &reply, sizeof(reply)) < 0)
      goto done;
  }

  for (unsigned i = 0; i < count; i++) {
    struct ndc_display d;
    if (recv_expected(c, NDC_DISPLAY, &d, sizeof(d)) < 0)
      goto done;
    d.connector[NDC_NAME_MAX - 1] = 0;
    struct server_stream *st = &streams[i];
    st->display_id = ntohl(d.display_id);
    st->width = ntohs(d.width);
    st->height = ntohs(d.height);
    st->refresh_hz = ntohs(d.refresh_hz);
    st->port = cfg->video_port + (int)i;
    snprintf(st->connector, sizeof(st->connector), "%s", d.connector);
    if (!st->display_id || !valid_wire_name(st->connector) || st->width <= 0 ||
        (st->width & 1) || st->height <= 0 || (st->height & 1) ||
        st->refresh_hz <= 0 || st->width > cfg->max_width ||
        st->height > cfg->max_height || st->refresh_hz > cfg->max_fps)
      goto done;
    for (unsigned j = 0; j < i; j++)
      if (streams[j].display_id == st->display_id)
        goto done;
    nd_crypto_random(&st->stream_id, sizeof(st->stream_id));
    if (!st->stream_id)
      st->stream_id = 1;
    snprintf(st->output, sizeof(st->output), "%.40s-%u-%u", cfg->output,
             ctx->serial, st->display_id);
    if (encrypted)
      nd_crypto_stream_key(st->key, cfg->psk, st->stream_id, hello.nonce,
                           challenge.nonce);
  }

  /* Try complete codec/backend combinations once and keep the selected
   * backend names. This avoids probing every codec twice and preserves the
   * session-wide AV1 -> HEVC -> H.264 preference while allowing each display
   * mode to use the best hardware backend available for that mode. */
  uint16_t codec_order[3] = {NDC_VIDEO_AV1, NDC_VIDEO_HEVC, NDC_VIDEO_H264};
  size_t codec_count = 3;
  if (cfg->video_codec) {
    codec_order[0] = cfg->video_codec;
    codec_count = 1;
  }

  uint16_t video_codec = 0;
  char selected_encoders[NDC_MAX_DISPLAYS][64] = {{0}};
  for (size_t ci = 0; ci < codec_count && !video_codec; ci++) {
    uint16_t candidate_codec = codec_order[ci];
    uint32_t candidate_bit =
        candidate_codec == NDC_VIDEO_AV1 ? NDC_CODEC_AV1 :
        candidate_codec == NDC_VIDEO_HEVC ? NDC_CODEC_HEVC : NDC_CODEC_H264;
    if (!(client_codecs & candidate_bit))
      continue;

    int usable = 1;
    for (unsigned i = 0; i < count; i++) {
      struct server_stream *st = &streams[i];
      if (nd_video_choose_encoder(candidate_codec, st->width, st->height,
                                  st->refresh_hz, cfg->qp,
                                  selected_encoders[i],
                                  sizeof(selected_encoders[i])) < 0) {
        usable = 0;
        break;
      }
    }
    if (usable)
      video_codec = candidate_codec;
  }

  if (!video_codec) {
    fprintf(stderr, "receiver %s has no mutually usable hardware video codec%s\n",
            ctx->peer, cfg->video_codec ? " for forced video_codec" : "");
    (void)ndc_send_msg(c, NDC_REJECT, NULL, 0);
    goto done;
  }

  for (unsigned i = 0; i < count; i++) {
    struct server_stream *st = &streams[i];
    st->video_codec = video_codec;
    snprintf(st->encoder_name, sizeof(st->encoder_name), "%s",
             selected_encoders[i]);
    fprintf(stderr,
            "receiver %s display %u selected codec=%u encoder=%s %dx%d@%d\n",
            ctx->peer, st->display_id, (unsigned)video_codec, st->encoder_name,
            st->width, st->height, st->refresh_hz);
  }

  /* Lifecycle hooks run only after codec/backend negotiation succeeded. */
  for (unsigned i = 0; i < count; i++) {
    struct server_stream *st = &streams[i];
    if (cfg->connect_cmd[0]) {
      int rc = run_stream_hook(cfg, ctx->peer, st, cfg->connect_cmd);
      if (rc) {
        fprintf(stderr, "connect_cmd exited %d\n", rc);
        goto done;
      }
    }
    st->lifecycle_started = 1;
  }

  int input_allowed = cfg->input_enabled && (requested & NDC_FLAG_WANT_INPUT);
  if (input_allowed) {
    kbm = create_kbm_uinput();
    tp = create_touchpad_uinput();
    if (kbm < 0 || tp < 0) {
      fprintf(stderr, "cannot create uinput devices for %s; input disabled\n",
              ctx->peer);
      destroy_ui(&kbm);
      destroy_ui(&tp);
      input_allowed = 0;
    }
  }
  struct ndc_welcome welcome = {
      .flags = htonl((input_allowed ? NDC_FLAG_INPUT_ALLOWED : 0) |
                     (requested & NDC_FLAG_NETWORK_TEST) |
                     (cfg->power_devices ? requested & NDC_FLAG_POWER_INFO : 0) |
                     (encrypted ? NDC_FLAG_FRAME_ENCRYPT : 0)),
      .display_count = htons((uint16_t)count),
      .video_codec = htons(video_codec),
  };
  if (ndc_send_msg(c, NDC_WELCOME, &welcome, sizeof(welcome)) < 0)
    goto done;
  for (unsigned i = 0; i < count; i++) {
    struct server_stream *st = &streams[i];
    struct ndc_stream wire = {
        .display_id = htonl(st->display_id),
        .stream_id = nd_hton64(st->stream_id),
        .video_port = htons((uint16_t)st->port),
        .width = htons((uint16_t)st->width),
        .height = htons((uint16_t)st->height),
        .refresh_hz = htons((uint16_t)st->refresh_hz),
    };
    snprintf(wire.output, sizeof(wire.output), "%s", st->output);
    if (ndc_send_msg(c, NDC_STREAM, &wire, sizeof(wire)) < 0)
      goto done;
  }

  if (requested & NDC_FLAG_NETWORK_TEST) {
    for (unsigned i = 0; i < count; i++) {
      struct server_stream *st = &streams[i];
      if (nd_network_test(c, 1, ctx->peer, st->port, NULL, st->refresh_hz,
                           st->stream_id, encrypted ? st->key : NULL) < 0) {
        fprintf(stderr, "network test failed for %s display %u\n",
                ctx->peer, st->display_id);
        goto done;
      }
    }
  }

  for (unsigned ready = 0; ready < count; ready++) {
    struct ndc_stream_ready r;
    if (recv_expected(c, NDC_STREAM_READY, &r, sizeof(r)) < 0)
      goto done;
    uint32_t id = ntohl(r.display_id);
    struct server_stream *st = NULL;
    for (unsigned i = 0; i < count; i++)
      if (streams[i].display_id == id) {
        st = &streams[i];
        break;
      }
    if (!st || st->video_pid > 0)
      goto done;
    st->video_pid = spawn_builtin_sender(cfg, ctx->peer, st, encrypted);
    if (st->video_pid < 0)
      goto done;
  }

  if (nd_display_state_attach(ctx->display_state, c) < 0)
    goto done;
  attached = 1;
  fprintf(stderr, "receiver %s running %u display(s), codec=%u encryption %s\n",
          ctx->peer, count, (unsigned)video_codec, encrypted ? "on" : "off");
  for (;;) {
    uint64_t now_ms = nd_power_now_ms();
    nd_power_sink_expire(&power_sink, now_ms);
    if (power_rx.pending && now_ms - power_rx.started_ms > 5000) goto done;
    for (unsigned i = 0; i < count; i++) {
      if (streams[i].video_pid <= 0)
        continue;
      int status = 0;
      if (waitpid(streams[i].video_pid, &status, WNOHANG) ==
          streams[i].video_pid) {
        streams[i].video_pid = -1;
        (void)nd_display_state_send(ctx->display_state, c, NDC_STOP, NULL, 0);
        goto done;
      }
    }
    struct pollfd p = {.fd = c, .events = POLLIN};
    int pr;
    do {
      pr = poll(&p, 1, 500);
    } while (pr < 0 && errno == EINTR);
    if (pr < 0 || (pr > 0 && !(p.revents & POLLIN)))
      goto done;
    if (!pr)
      continue;
    uint8_t b[NDC_MAX_PAYLOAD];
    uint32_t len = sizeof(b);
    uint16_t type;
    int rr = ndc_recv_msg(c, &type, b, &len);
    if (rr <= 0)
      goto done;
    if (type == NDC_INPUT && input_allowed && len == sizeof(struct ndc_input)) {
      struct ndc_input in;
      memcpy(&in, b, sizeof(in));
      if (inject_event(kbm, tp, &in) < 0)
        goto done;
    } else if (type >= NDC_POWER_BEGIN && type <= NDC_POWER_END &&
               cfg->power_devices && (requested & NDC_FLAG_POWER_INFO)) {
      struct nd_power_snapshot *snapshot = NULL;
      int result = nd_power_receive(&power_rx, type, b, len, &snapshot);
      if (result < 0) goto done;
      if (result > 0) {
        (void)nd_power_sink_update(&power_sink, snapshot, ctx->peer, ctx->serial);
        free(snapshot);
      }
    } else if (type == NDC_PING && len == 0) {
      if (nd_display_state_send(ctx->display_state, c, NDC_PONG, NULL, 0) < 0)
        goto done;
    } else if (!(type == NDC_PONG && len == 0))
      goto done;
  }

done:
  nd_power_rx_clear(&power_rx);
  nd_power_sink_clear(&power_sink);
  fprintf(stderr, "receiver %s disconnected\n", ctx->peer);
  if (attached)
    nd_display_state_detach(ctx->display_state, c);
  destroy_ui(&kbm);
  destroy_ui(&tp);
  for (unsigned i = 0; i < count; i++) {
    ndc_stop_child(&streams[i].video_pid);
    if (streams[i].lifecycle_started && cfg->disconnect_cmd[0])
      (void)run_stream_hook(cfg, ctx->peer, &streams[i], cfg->disconnect_cmd);
  }
  close(c);
  atomic_fetch_sub_explicit(&session_count, 1, memory_order_relaxed);
  free(ctx);
  return NULL;
}

int main(int argc, char **argv) {
  self_program = argv[0];
  if (sodium_init() < 0)
    die("libsodium");
  if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
    printf("netdisplay-server %s protocol=%u multi-client/multi-display\n",
           NETDISPLAY_VERSION, NDC_VERSION);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--derive-password-key"))
    return derive_password_key_cli();
  if (argc == 14 && !strcmp(argv[1], "--video-sender"))
    return nd_video_sender_run(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                               atoi(argv[6]), atoi(argv[7]), atoi(argv[8]),
                               strtoull(argv[9], NULL, 10), atoi(argv[10]),
                               atoi(argv[11]), (uint16_t)atoi(argv[12]), argv[13]);

  if (argc != 2) {
    fprintf(stderr, "usage: %s SERVER_CONFIG\n", argv[0]);
    return 2;
  }
  struct host_cfg cfg;
  load_cfg(argv[1], &cfg);
  struct nd_display_state_source display_state;
  if (nd_display_state_start(&display_state, cfg.output, cfg.source_output,
                             cfg.brightness_device) < 0)
    die("start display-state watchers");
  int ls = make_listener(&cfg);
  pthread_t discovery_thread;
  int de = pthread_create(&discovery_thread, NULL, discovery_main, &cfg);
  if (de != 0) {
    errno = de;
    die("pthread_create discovery");
  }
  pthread_detach(discovery_thread);

  fprintf(stderr,
          "netdisplay-server: %s:%d, max-clients=%d, video ports %d-%d, "
          "encryption=%s\n",
          cfg.listen_addr, cfg.port, cfg.max_clients, cfg.video_port,
          cfg.video_port + cfg.max_displays_per_client - 1,
          cfg.frame_encryption == 2   ? "required"
          : cfg.frame_encryption == 1 ? "allowed"
                                      : "off");

  for (;;) {
    struct sockaddr_in pa;
    socklen_t plen = sizeof(pa);
    int c = accept4(ls, (struct sockaddr *)&pa, &plen, SOCK_CLOEXEC);
    if (c < 0) {
      if (errno == EINTR)
        continue;
      die("accept");
    }
    char peer[INET_ADDRSTRLEN] = "?";
    (void)inet_ntop(AF_INET, &pa.sin_addr, peer, sizeof(peer));
    int old_count =
        atomic_fetch_add_explicit(&session_count, 1, memory_order_relaxed);
    if (!peer_allowed(&cfg, peer) || old_count >= cfg.max_clients) {
      fprintf(stderr, "reject receiver %s\n", peer);
      atomic_fetch_sub_explicit(&session_count, 1, memory_order_relaxed);
      close(c);
      continue;
    }
    ndc_set_tcp_opts(c);
    struct session_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
      atomic_fetch_sub_explicit(&session_count, 1, memory_order_relaxed);
      close(c);
      continue;
    }
    ctx->cfg = &cfg;
    ctx->display_state = &display_state;
    ctx->fd = c;
    ctx->serial = atomic_fetch_add_explicit(&next_session_serial, 1,
                                            memory_order_relaxed);
    snprintf(ctx->peer, sizeof(ctx->peer), "%s", peer);
    pthread_t thread;
    int e = pthread_create(&thread, NULL, session_main, ctx);
    if (e) {
      free(ctx);
      close(c);
      atomic_fetch_sub_explicit(&session_count, 1, memory_order_relaxed);
      continue;
    }
    pthread_detach(thread);
  }
}
