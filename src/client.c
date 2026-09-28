
#define _GNU_SOURCE

#include "common.h"
#include "crypto.h"
#include "proto.h"
#include "network_test.h"
#include "power_state.h"
#include "input_receiver.h"
#include "video_receiver.h"
#include <dirent.h>
#include <dlfcn.h>
#include <glob.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sodium/core.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/random.h>

extern char **environ;
static const char *self_program;
static struct nd_input_context input;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "shutdown flag must be signal-safe");
static void request_shutdown(int signal_number) {
  (void)signal_number;
  atomic_store(&input.quit, 1);
}
static void stop_local_input(void) { nd_input_stop(&input); }

struct client_cfg {
  char host[64];
  char interface[IFNAMSIZ];
  char drm_device[128];
  char vaapi_device[128];
  char backlight_device[256];
  int port;
  int want_input;
  int send_power;
  int grab_input;
  int reconnect_ms;
  int frame_encryption;
  char psk_file[512];
  uint8_t psk[ND_KEY_SIZE];
  int have_psk;
  struct {
    char connector[NDC_NAME_MAX];
    int width, height, refresh_hz;
  } display[NDC_MAX_DISPLAYS];
  int display_count;
};
static void cfg_defaults(struct client_cfg *c) {
  memset(c, 0, sizeof(*c));
  snprintf(c->host, sizeof(c->host), "auto");
  c->port = NDC_DEFAULT_PORT;
  c->want_input = 1;
  c->send_power = 1;
  c->grab_input = 1;
  c->reconnect_ms = 500;
  snprintf(c->backlight_device, sizeof(c->backlight_device), "auto");
  /* Empty means auto-detect. This is important for the portable receiver: the
   * KMS card number and VAAPI render node are not stable across laptops. */
  c->drm_device[0] = 0;
  c->vaapi_device[0] = 0;
}

static int parse_display(struct client_cfg *c, const char *value) {
  if (c->display_count >= (int)NDC_MAX_DISPLAYS)
    return -1;
  char b[160];
  snprintf(b, sizeof(b), "%s", value);
  char *comma = strchr(b, ',');
  if (!comma)
    return -1;
  *comma++ = 0;
  if (strlen(b) >= NDC_NAME_MAX)
    return -1;
  int w, h, f, consumed = 0;
  if (sscanf(comma, "%dx%d@%d%n", &w, &h, &f, &consumed) != 3 ||
      comma[consumed] || w <= 0 || w > UINT16_MAX || (w & 1) || h <= 0 ||
      h > UINT16_MAX || (h & 1) || f <= 0 || f > UINT16_MAX || !b[0])
    return -1;
  for (int i = 0; i < c->display_count; i++)
    if (!strcmp(c->display[i].connector, b))
      return -1;
  int n = c->display_count++;
  snprintf(c->display[n].connector, sizeof(c->display[n].connector), "%s", b);
  c->display[n].width = w;
  c->display[n].height = h;
  c->display[n].refresh_hz = f;
  return 0;
}

static void load_cfg(const char *path, struct client_cfg *c, int required) {
  cfg_defaults(c);
  if (!path || !*path)
    return;
  FILE *f = fopen(path, "r");
  if (!f) {
    if (!required && errno == ENOENT)
      return;
    die(path);
  }
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
    if (!strcmp(k, "host"))
      snprintf(c->host, sizeof(c->host), "%s", v);
    else if (!strcmp(k, "port"))
      c->port = atoi(v);
    else if (!strcmp(k, "interface"))
      snprintf(c->interface, sizeof(c->interface), "%s", v);
    else if (!strcmp(k, "send_power"))
      c->send_power = atoi(v) != 0;
    else if (!strcmp(k, "want_input"))
      c->want_input = atoi(v) != 0;
    else if (!strcmp(k, "grab_input"))
      c->grab_input = atoi(v) != 0;
    else if (!strcmp(k, "reconnect_ms"))
      c->reconnect_ms = atoi(v);
    else if (!strcmp(k, "psk_file"))
      snprintf(c->psk_file, sizeof(c->psk_file), "%s", v);
    else if (!strcmp(k, "frame_encryption"))
      c->frame_encryption = atoi(v) != 0;
    else if (!strcmp(k, "display") && parse_display(c, v) < 0) {
      fprintf(stderr, "invalid display entry in %s\n", path);
      exit(2);
    } else if (!strcmp(k, "drm_device"))
      snprintf(c->drm_device, sizeof(c->drm_device), "%s", v);
    else if (!strcmp(k, "vaapi_device"))
      snprintf(c->vaapi_device, sizeof(c->vaapi_device), "%s", v);
    else if (!strcmp(k, "backlight_device"))
      snprintf(c->backlight_device, sizeof(c->backlight_device), "%s", v);
  }
  fclose(f);
  if (!c->host[0] || c->port <= 0 || c->port > 65535) {
    fprintf(stderr, "host/port missing or invalid in %s\n", path);
    exit(2);
  }
  if (c->psk_file[0]) {
    if (nd_crypto_load_psk(c->psk_file, c->psk) < 0)
      die(c->psk_file);
    c->have_psk = 1;
  }
}

static int prompt_password(uint8_t key[ND_KEY_SIZE]) {
  if (!isatty(STDIN_FILENO) && access("/dev/tty", R_OK | W_OK) < 0) {
    fprintf(stderr, "server requires a password, but no controlling terminal "
                    "is available; use psk_file for unattended clients\n");
    errno = ENOTTY;
    return -1;
  }
  char *password = getpass("NetDisplay password: ");
  if (!password || !*password) {
    if (password)
      explicit_bzero(password, strlen(password));
    errno = EACCES;
    return -1;
  }
  int r = nd_crypto_password_key(key, password);
  explicit_bzero(password, strlen(password));
  return r;
}

static const char *default_config_path(char *buf, size_t bufsz) {
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

static uint32_t discovery_nonce(void) {
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

static void close_discovery_sockets(struct discovery_socket *ds, int n) {
  for (int i = 0; i < n; i++) {
    if (ds[i].fd >= 0)
      close(ds[i].fd);
    ds[i].fd = -1;
  }
}

static int have_discovery_iface(const struct discovery_socket *ds, int n,
                                const char *ifname) {
  for (int i = 0; i < n; i++)
    if (!strcmp(ds[i].ifname, ifname))
      return 1;
  return 0;
}

/* Make one UDP socket per usable interface. Each socket is bound to its device
 * before first use, then sends the limited broadcast 255.255.255.255. This
 * avoids routing-table ambiguity on machines with Wi-Fi + Ethernet while still
 * requiring no knowledge of the interface's subnet or directed broadcast. */
static int make_discovery_sockets(const struct client_cfg *cfg,
                                  struct discovery_socket *ds, int cap) {
  struct ifaddrs *ifs = NULL;
  if (getifaddrs(&ifs) < 0)
    return -1;

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
    if (s < 0)
      continue;

    int one = 1;
    if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0) {
      close(s);
      continue;
    }

    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, i->ifa_name,
                   strlen(i->ifa_name) + 1) < 0) {
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
                         int *port, char *ifname, size_t ifnamesz) {
  struct discovery_socket ds[MAX_DISCOVERY_IFACES];
  for (int i = 0; i < MAX_DISCOVERY_IFACES; i++)
    ds[i].fd = -1;

  int nds = make_discovery_sockets(cfg, ds, MAX_DISCOVERY_IFACES);
  if (nds < 0)
    return -1;

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
    ssize_t n =
        sendto(ds[i].fd, &d, sizeof(d), 0, (struct sockaddr *)&to, sizeof(to));
    if (n == (ssize_t)sizeof(d)) {
      sent++;
    } else {
      fprintf(stderr, "discovery: send on %s: %s\n", ds[i].ifname,
              n < 0 ? strerror(errno) : "short send");
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
  do {
    pr = poll(pfds, (nfds_t)nds, 700);
  } while (pr < 0 && errno == EINTR);
  if (pr <= 0) {
    close_discovery_sockets(ds, nds);
    if (pr == 0)
      errno = ETIMEDOUT;
    return -1;
  }

  for (int i = 0; i < nds; i++) {
    if (!(pfds[i].revents & POLLIN))
      continue;
    for (;;) {
      struct ndc_discovery offer;
      struct sockaddr_in from;
      socklen_t fl = sizeof(from);
      ssize_t n = recvfrom(ds[i].fd, &offer, sizeof(offer), MSG_DONTWAIT,
                           (struct sockaddr *)&from, &fl);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      if (n < 0 && errno == EINTR)
        continue;
      if (n != (ssize_t)sizeof(offer))
        continue;
      if (ntohl(offer.magic) != NDC_DISC_MAGIC ||
          ntohs(offer.version) != NDC_VERSION ||
          ntohs(offer.type) != NDC_OFFER || ntohl(offer.nonce) != nonce)
        continue;
      int p = ntohs(offer.tcp_port);
      if (p <= 0)
        continue;
      if (!inet_ntop(AF_INET, &from.sin_addr, ip, ipsz))
        continue;
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

static int connect_host_ip(const char *ip, int port, const char *ifname) {
  int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (s < 0)
    return -1;
  if (ifname && *ifname &&
      setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) <
          0) {
    int e = errno;
    close(s);
    errno = e;
    return -1;
  }
  ndc_set_tcp_opts(s);
  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_port = htons((uint16_t)port)};
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
    close(s);
    errno = EINVAL;
    return -1;
  }
  if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
    if (errno != EINPROGRESS) {
      int error = errno;
      close(s);
      errno = error;
      return -1;
    }
    struct pollfd pending = {.fd = s, .events = POLLOUT};
    int connected = 0, failure = ETIMEDOUT;
    for (int attempt = 0; attempt < 20 && !atomic_load(&input.quit); attempt++) {
      int ready = poll(&pending, 1, 250);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0) {
        failure = errno;
        break;
      }
      if (!ready)
        continue;
      int error = 0;
      socklen_t size = sizeof(error);
      if (getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error)
        connected = 1;
      else
        failure = error ? error : errno;
      break;
    }
    if (!connected) {
      close(s);
      errno = atomic_load(&input.quit) ? ECANCELED : failure;
      return -1;
    }
  }
  int flags = fcntl(s, F_GETFL);
  if (atomic_load(&input.quit) || flags < 0 ||
      fcntl(s, F_SETFL, flags & ~O_NONBLOCK) < 0) {
    int error = atomic_load(&input.quit) ? ECANCELED : errno;
    close(s);
    errno = error;
    return -1;
  }
  return s;
}

static int find_client_backlight(const char *setting, char *dir, size_t dirsz) {
  if (!setting[0] || !strcmp(setting, "none"))
    return -1;
  if (strcmp(setting, "auto")) {
    if (setting[0] == '/')
      snprintf(dir, dirsz, "%s", setting);
    else
      snprintf(dir, dirsz, "/sys/class/backlight/%s", setting);
    return 0;
  }
  glob_t g;
  if (glob("/sys/class/backlight/*", 0, NULL, &g) != 0 || !g.gl_pathc)
    return -1;
  snprintf(dir, dirsz, "%s", g.gl_pathv[0]);
  globfree(&g);
  return 0;
}

static int read_ulong_file(const char *path, unsigned long *value) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  int rc = fscanf(f, "%lu", value) == 1 ? 0 : -1;
  fclose(f);
  return rc;
}

/* Backlight files are deliberately not writable by ordinary users on many
 * distributions.  logind exposes this narrowly-scoped operation to the active
 * local session, which is also how desktop environments change brightness
 * without a privileged helper.  Resolve libsystemd at runtime so the portable
 * client does not gain a build-time or ELF dependency on it. */
struct sd_bus;
struct sd_bus_message;

struct logind_api {
  int initialized;
  void *library;
  int (*default_system)(struct sd_bus **bus);
  int (*call_method)(struct sd_bus *bus, const char *destination,
                     const char *path, const char *interface,
                     const char *member, void *error,
                     struct sd_bus_message **reply, const char *types, ...);
  struct sd_bus_message *(*message_unref)(struct sd_bus_message *message);
  struct sd_bus *(*bus_unref)(struct sd_bus *bus);
};

static int load_logind_symbol(void *library, const char *name, void *target,
                              size_t target_size) {
  void *symbol = dlsym(library, name);
  if (!symbol || target_size != sizeof(symbol))
    return -1;
  memcpy(target, &symbol, sizeof(symbol));
  return 0;
}

static int logind_set_backlight(const char *name, unsigned long value) {
  static struct logind_api api;
  if (!api.initialized) {
    api.initialized = 1;
    api.library = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!api.library ||
        load_logind_symbol(api.library, "sd_bus_default_system",
                           &api.default_system,
                           sizeof(api.default_system)) < 0 ||
        load_logind_symbol(api.library, "sd_bus_call_method", &api.call_method,
                           sizeof(api.call_method)) < 0 ||
        load_logind_symbol(api.library, "sd_bus_message_unref",
                           &api.message_unref, sizeof(api.message_unref)) < 0 ||
        load_logind_symbol(api.library, "sd_bus_unref", &api.bus_unref,
                           sizeof(api.bus_unref)) < 0) {
      if (api.library)
        dlclose(api.library);
      api.library = NULL;
    }
  }
  if (!api.library || value > UINT32_MAX)
    return -ENOSYS;

  struct sd_bus *bus = NULL;
  struct sd_bus_message *reply = NULL;
  int r = api.default_system(&bus);
  if (r >= 0) {
    r = api.call_method(bus, "org.freedesktop.login1",
                        "/org/freedesktop/login1/session/auto",
                        "org.freedesktop.login1.Session", "SetBrightness", NULL,
                        &reply, "ssu", "backlight", name, (uint32_t)value);
  }
  api.message_unref(reply);
  api.bus_unref(bus);
  return r;
}

static void apply_backlight(const struct client_cfg *cfg, uint32_t percent) {
  static int initialized;
  static int available;
  static unsigned long maximum;
  static char brightness[512];
  static char device[384];
  if (!initialized) {
    char dir[384], max_path[512];
    initialized = 1;
    if (find_client_backlight(cfg->backlight_device, dir, sizeof(dir)) < 0) {
      if (strcmp(cfg->backlight_device, "none"))
        fprintf(stderr,
                "receiver brightness unavailable: no backlight device\n");
      return;
    }
    const char *base = strrchr(dir, '/');
    snprintf(device, sizeof(device), "%s", base ? base + 1 : dir);
    snprintf(brightness, sizeof(brightness), "%s/brightness", dir);
    snprintf(max_path, sizeof(max_path), "%s/max_brightness", dir);
    if (read_ulong_file(max_path, &maximum) < 0 || !maximum) {
      fprintf(stderr, "cannot read receiver backlight maximum from %s\n",
              max_path);
      return;
    }
    available = 1;
  }
  if (!available)
    return;
  if (percent > 10000u)
    percent = 10000u;
  unsigned long value =
      (unsigned long)(((uint64_t)maximum * percent + 5000u) / 10000u);
  int fd = open(brightness, O_WRONLY | O_CLOEXEC);
  int direct_error = 0;
  if (fd >= 0) {
    char b[32];
    int len = snprintf(b, sizeof(b), "%lu\n", value);
    ssize_t wr;
    do {
      wr = write(fd, b, (size_t)len);
    } while (wr < 0 && errno == EINTR);
    if (wr != len)
      direct_error = wr < 0 ? errno : EIO;
    close(fd);
    if (!direct_error) {
      fprintf(stderr, "receiver brightness: %.2f%% via %s\n",
              (double)percent / 100.0, brightness);
      return;
    }
  } else {
    direct_error = errno;
  }

  int r = logind_set_backlight(device, value);
  if (r >= 0) {
    fprintf(stderr, "receiver brightness: %.2f%% via logind (%s)\n",
            (double)percent / 100.0, device);
    return;
  }
  fprintf(stderr,
          "cannot set receiver backlight %s: direct write: %s; logind: %s\n",
          brightness, strerror(direct_error), strerror(-r));
}

static pid_t spawn_builtin_receiver(const struct client_cfg *cfg, int master_fd,
                                    const struct nd_local_display *display,
                                    int video_port, int width, int height,
                                    int refresh_hz, const char *ifname,
                                    int encrypted, uint64_t wire_session,
                                    const uint8_t key[ND_KEY_SIZE],
                                    int *ready_fd, int *state_fd) {
  int pfd[2], state_pipe[2], keypipe[2] = {-1, -1};
  if (pipe2(pfd, O_CLOEXEC) < 0)
    return -1;
  if (pipe2(state_pipe, O_CLOEXEC) < 0) {
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }

  if (encrypted && pipe2(keypipe, O_CLOEXEC) < 0) {
    close(pfd[0]);
    close(pfd[1]);
    close(state_pipe[0]);
    close(state_pipe[1]);
    return -1;
  }
  enum {
    CHILD_KEY_FD = 196,
    CHILD_DRM_FD = 197,
    CHILD_READY_FD = 198,
    CHILD_STATE_FD = 199
  };
  char portbuf[16], drmfdbuf[16], connectorbuf[16], crtcbuf[16];
  char readybuf[16], statebuf[16], widthbuf[16], heightbuf[16], refreshbuf[16];
  char encryptedbuf[8], keyfdbuf[16], sessionbuf[32];
  snprintf(sessionbuf, sizeof(sessionbuf), "%llx", (unsigned long long)wire_session);
  snprintf(portbuf, sizeof(portbuf), "%d", video_port);
  snprintf(drmfdbuf, sizeof(drmfdbuf), "%d", CHILD_DRM_FD);
  snprintf(connectorbuf, sizeof(connectorbuf), "%u", display->connector_id);
  snprintf(crtcbuf, sizeof(crtcbuf), "%u", display->crtc_id);
  snprintf(readybuf, sizeof(readybuf), "%d", CHILD_READY_FD);
  snprintf(statebuf, sizeof(statebuf), "%d", CHILD_STATE_FD);
  snprintf(widthbuf, sizeof(widthbuf), "%d", width);
  snprintf(heightbuf, sizeof(heightbuf), "%d", height);
  snprintf(refreshbuf, sizeof(refreshbuf), "%d", refresh_hz);
  snprintf(encryptedbuf, sizeof(encryptedbuf), "%d", encrypted);
  snprintf(keyfdbuf, sizeof(keyfdbuf), "%d", encrypted ? CHILD_KEY_FD : -1);
  char *const av[] = {
      (char *)self_program,
      (char *)"--video-receiver",
      portbuf,
      drmfdbuf,
      connectorbuf,
      crtcbuf,
      (char *)(cfg->vaapi_device[0] ? cfg->vaapi_device : ""),
      (char *)(ifname ? ifname : ""),
      readybuf,
      widthbuf,
      heightbuf,
      refreshbuf,
      statebuf,
      encryptedbuf,
      keyfdbuf,
      sessionbuf,
      NULL,
  };

  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  int e = posix_spawn_file_actions_init(&fa);
  if (e != 0)
    goto fail_no_fa;
  e = posix_spawn_file_actions_adddup2(&fa, master_fd, CHILD_DRM_FD);
  if (e != 0)
    goto fail;
  if (encrypted) {
    e = posix_spawn_file_actions_adddup2(&fa, keypipe[0], CHILD_KEY_FD);
    if (e != 0)
      goto fail;
    e = posix_spawn_file_actions_addclose(&fa, keypipe[1]);
    if (e != 0)
      goto fail;
  }
  e = posix_spawn_file_actions_adddup2(&fa, pfd[1], CHILD_READY_FD);
  if (e != 0)
    goto fail;
  e = posix_spawn_file_actions_addclose(&fa, pfd[0]);
  if (e != 0)
    goto fail;
  e = posix_spawn_file_actions_addclose(&fa, pfd[1]);
  if (e != 0)
    goto fail;
  e = posix_spawn_file_actions_adddup2(&fa, state_pipe[0], CHILD_STATE_FD);
  if (e != 0)
    goto fail;
  e = posix_spawn_file_actions_addclose(&fa, state_pipe[0]);
  if (e != 0)
    goto fail;
  e = posix_spawn_file_actions_addclose(&fa, state_pipe[1]);
  if (e != 0)
    goto fail;

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
  close(pfd[1]);
  close(state_pipe[0]);
  if (keypipe[0] >= 0)
    close(keypipe[0]);
  if (e != 0) {
    close(pfd[0]);
    close(state_pipe[1]);
    if (keypipe[1] >= 0)
      close(keypipe[1]);
    errno = e;
    return -1;
  }
  if (encrypted) {
    size_t off = 0;
    while (off < ND_KEY_SIZE) {
      ssize_t n = write(keypipe[1], key + off, ND_KEY_SIZE - off);
      if (n > 0) {
        off += (size_t)n;
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      close(keypipe[1]);
      ndc_stop_child(&p);
      return -1;
    }
    close(keypipe[1]);
  }
  *ready_fd = pfd[0];
  *state_fd = state_pipe[1];
  return p;

fail:
  posix_spawn_file_actions_destroy(&fa);
fail_no_fa:
  close(pfd[0]);
  close(pfd[1]);
  close(state_pipe[0]);
  close(state_pipe[1]);
  if (keypipe[0] >= 0)
    close(keypipe[0]);
  if (keypipe[1] >= 0)
    close(keypipe[1]);
  errno = e;
  return -1;
}

static int wait_receiver_ready(pid_t pid, int fd, int timeout_ms) {
  struct pollfd p = {.fd = fd, .events = POLLIN | POLLHUP};
  int pr;
  do {
    if (atomic_load(&input.quit)) {
      errno = ECANCELED;
      return -1;
    }
    pr = poll(&p, 1, timeout_ms);
  } while (pr < 0 && errno == EINTR && !atomic_load(&input.quit));
  if (pr <= 0) {
    if (pr == 0)
      errno = ETIMEDOUT;
    return -1;
  }

  uint8_t b = 0;
  ssize_t n;
  do {
    n = read(fd, &b, 1);
  } while (n < 0 && errno == EINTR);
  if (n == 1 && b == 1)
    return 0;

  int st = 0;
  pid_t wr = waitpid(pid, &st, WNOHANG);
  if (wr == pid && WIFEXITED(st))
    fprintf(stderr, "built-in video receiver exited status=%d before ready\n",
            WEXITSTATUS(st));
  errno = EIO;
  return -1;
}

struct client_stream_runtime {
  uint32_t display_id;
  uint64_t stream_id;
  int port, width, height, refresh_hz;
  const struct nd_local_display *display;
  pid_t pid;
  int state_fd;
};

static int recv_control(int fd, uint16_t expected, void *payload,
                        uint32_t size) {
  uint16_t type = 0;
  uint32_t len = size;
  int r = ndc_recv_msg(fd, &type, payload, &len);
  if (r <= 0 || type != expected || len != size) {
    if (r > 0)
      errno = type == NDC_REJECT ? EACCES : EPROTO;
    return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  self_program = argv[0];
  if (sodium_init() < 0)
    die("libsodium");
  if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
    printf("netdisplay-client %s protocol=%u video=runtime-negotiated\n",
           NETDISPLAY_VERSION, NDC_VERSION);
    return 0;
  }
  if ((argc == 15 || argc == 16) && !strcmp(argv[1], "--video-receiver"))
    return main_receiver(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                         atoi(argv[5]), argv[6], argv[7], atoi(argv[8]),
                         atoi(argv[9]), atoi(argv[10]), atoi(argv[11]),
                         atoi(argv[12]), atoi(argv[13]), atoi(argv[14]),
                         argc == 16 ? strtoull(argv[15], NULL, 16) : 0);
  if (argc > 2) {
    fprintf(stderr, "usage: %s [CLIENT_CONFIG]\n", argv[0]);
    return 1;
  }
  char default_cfg[4096];
  const char *cfg_path =
      argc == 2 ? argv[1]
                : default_config_path(default_cfg, sizeof(default_cfg));
  struct client_cfg cfg;
  load_cfg(cfg_path, &cfg, argc == 2);
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  struct sigaction stop_action = {.sa_handler = request_shutdown};
  sigemptyset(&stop_action.sa_mask);
  sigaction(SIGUSR1, &stop_action, NULL);
  sigaction(SIGTERM, &stop_action, NULL);
  sigaction(SIGHUP, &stop_action, NULL);
  if (nd_input_start(&input, cfg.grab_input) < 0)
    die("start local input");
  atexit(stop_local_input);

  struct nd_local_display found[NDC_MAX_DISPLAYS], displays[NDC_MAX_DISPLAYS];
  int master_fd = -1;
  char drm_path[128];
  int found_count = nd_video_open_displays(
      cfg.drm_device[0] ? cfg.drm_device : NULL, &master_fd, drm_path,
      sizeof(drm_path), found, NDC_MAX_DISPLAYS);
  if (found_count < 0)
    die("open connected DRM displays");
  int display_count = 0;
  if (!cfg.display_count) {
    memcpy(displays, found, (size_t)found_count * sizeof(found[0]));
    display_count = found_count;
  } else {
    for (int r = 0; r < cfg.display_count; r++) {
      int matched = 0;
      for (int i = 0; i < found_count; i++) {
        if (strcmp(cfg.display[r].connector, found[i].connector))
          continue;
        displays[display_count] = found[i];
        displays[display_count].width = (uint16_t)cfg.display[r].width;
        displays[display_count].height = (uint16_t)cfg.display[r].height;
        displays[display_count].refresh_hz =
            (uint16_t)cfg.display[r].refresh_hz;
        display_count++;
        matched = 1;
        break;
      }
      if (!matched)
        fprintf(stderr, "configured display %s is not connected\n",
                cfg.display[r].connector);
    }
    if (!display_count) {
      fprintf(stderr, "none of the configured displays is connected\n");
      return 1;
    }
  }
  fprintf(
      stderr,
      "netdisplay-client: DRM %s, %d display(s), server=%s, encryption=%s\n",
      drm_path, display_count, cfg.host, cfg.frame_encryption ? "on" : "off");
  for (int i = 0; i < display_count; i++)
    fprintf(stderr, "  %s: %ux%u@%u CRTC %u\n", displays[i].connector,
            displays[i].width, displays[i].height, displays[i].refresh_hz,
            displays[i].crtc_id);

  while (!atomic_load(&input.quit)) {
    char host_ip[sizeof(cfg.host)], discovered_if[IFNAMSIZ] = "";
    int control_port = cfg.port;
    if (!strcmp(cfg.host, "auto")) {
      if (discover_host(&cfg, host_ip, sizeof(host_ip), &control_port,
                        discovered_if, sizeof(discovered_if)) < 0)
        goto retry;
    } else {
      snprintf(host_ip, sizeof(host_ip), "%s", cfg.host);
      if (cfg.interface[0])
        snprintf(discovered_if, sizeof(discovered_if), "%s", cfg.interface);
    }
    int s = connect_host_ip(host_ip, control_port, discovered_if);
    if (s < 0)
      goto retry;
    struct timeval send_timeout = {.tv_sec = 2};
    (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    nd_input_session(&input, s, 0);

    struct client_stream_runtime streams[NDC_MAX_DISPLAYS];
    memset(streams, 0, sizeof(streams));
    for (int i = 0; i < display_count; i++) {
      streams[i].pid = -1;
      streams[i].state_fd = -1;
      streams[i].display_id = (uint32_t)i + 1u;
      streams[i].display = &displays[i];
    }
    struct ndc_hello hello = {
        .flags = htonl((cfg.want_input ? NDC_FLAG_WANT_INPUT : 0) |
                       NDC_FLAG_NETWORK_TEST |
                       (cfg.send_power ? NDC_FLAG_POWER_INFO : 0) |
                       (cfg.have_psk ? NDC_FLAG_HAVE_PSK : 0) |
                       (cfg.frame_encryption ? NDC_FLAG_FRAME_ENCRYPT : 0)),
        .display_count = htons((uint16_t)display_count),
    };
    nd_crypto_random(hello.nonce, sizeof(hello.nonce));
    int session_ok = ndc_send_msg(s, NDC_HELLO, &hello, sizeof(hello)) == 0;
    struct ndc_challenge challenge;
    if (session_ok &&
        recv_control(s, NDC_CHALLENGE, &challenge, sizeof(challenge)) < 0)
      session_ok = 0;
    uint32_t server_flags = session_ok ? ntohl(challenge.flags) : 0;
    int server_requires_auth = !!(server_flags & NDC_FLAG_AUTH_REQUIRED);
    int server_uses_password = !!(server_flags & NDC_FLAG_PASSWORD);
    int prompted_password = 0, authenticated = 0;
    if (session_ok && server_requires_auth && !cfg.have_psk &&
        !server_uses_password) {
      fprintf(stderr, "server requires a PSK; configure psk_file\n");
      errno = EACCES;
      session_ok = 0;
    } else if (session_ok && server_uses_password && cfg.psk_file[0]) {
      fprintf(stderr,
              "server requires an interactive password; remove psk_file\n");
      errno = EINVAL;
      session_ok = 0;
    } else if (session_ok && server_uses_password && !cfg.have_psk) {
      nd_input_prompt(&input, 1);
      int password_result = atomic_load(&input.quit) ? -1 : prompt_password(cfg.psk);
      nd_input_prompt(&input, 0);
      if (password_result < 0) {
        fprintf(stderr, "password entry failed\n");
        session_ok = 0;
      } else {
        cfg.have_psk = 1;
        prompted_password = 1;
      }
    }
    if (session_ok &&
        (server_requires_auth != cfg.have_psk ||
         !!(server_flags & NDC_FLAG_FRAME_ENCRYPT) != cfg.frame_encryption)) {
      errno = EPROTO;
      session_ok = 0;
    }
    if (session_ok && cfg.have_psk) {
      struct ndc_auth auth, reply;
      nd_crypto_proof(auth.proof, cfg.psk, "client", hello.nonce,
                      challenge.nonce);
      if (ndc_send_msg(s, NDC_AUTH, &auth, sizeof(auth)) < 0 ||
          recv_control(s, NDC_AUTH, &reply, sizeof(reply)) < 0)
        session_ok = 0;
      if (session_ok) {
        uint8_t expected[NDC_AUTH_SIZE];
        nd_crypto_proof(expected, cfg.psk, "server", hello.nonce,
                        challenge.nonce);
        if (!nd_crypto_verify(reply.proof, expected)) {
          errno = EACCES;
          session_ok = 0;
        } else
          authenticated = 1;
      }
    }
    if (!session_ok && prompted_password && !authenticated) {
      explicit_bzero(cfg.psk, sizeof(cfg.psk));
      cfg.have_psk = 0;
      fprintf(stderr,
              "authentication failed; password will be requested again\n");
    }
    for (int i = 0; session_ok && i < display_count; i++) {
      struct ndc_display d = {
          .display_id = htonl((uint32_t)i + 1u),
          .width = htons(displays[i].width),
          .height = htons(displays[i].height),
          .refresh_hz = htons(displays[i].refresh_hz),
      };
      snprintf(d.connector, sizeof(d.connector), "%s", displays[i].connector);
      if (ndc_send_msg(s, NDC_DISPLAY, &d, sizeof(d)) < 0)
        session_ok = 0;
    }
    struct ndc_welcome welcome;
    if (session_ok &&
        recv_control(s, NDC_WELCOME, &welcome, sizeof(welcome)) < 0)
      session_ok = 0;
    int input_allowed =
        session_ok && !!(ntohl(welcome.flags) & NDC_FLAG_INPUT_ALLOWED);
    if (session_ok && ntohs(welcome.display_count) != display_count)
      session_ok = 0;
    for (int i = 0; session_ok && i < display_count; i++) {
      struct ndc_stream wire;
      if (recv_control(s, NDC_STREAM, &wire, sizeof(wire)) < 0) {
        session_ok = 0;
        break;
      }
      uint32_t id = ntohl(wire.display_id);
      if (!id || id > (uint32_t)display_count) {
        session_ok = 0;
        break;
      }
      struct client_stream_runtime *st = &streams[id - 1u];
      st->stream_id = nd_ntoh64(wire.stream_id);
      st->port = ntohs(wire.video_port);
      st->width = ntohs(wire.width);
      st->height = ntohs(wire.height);
      st->refresh_hz = ntohs(wire.refresh_hz);
      if (!st->stream_id || !st->port || !st->width || (st->width & 1) ||
          !st->height || (st->height & 1) || !st->refresh_hz)
        session_ok = 0;
    }
    if (session_ok && (ntohl(welcome.flags) & NDC_FLAG_NETWORK_TEST)) {
      for (int i = 0; session_ok && i < display_count; i++) {
        struct client_stream_runtime *st = &streams[i];
        uint8_t key[ND_KEY_SIZE] = {0};
        if (cfg.frame_encryption)
          nd_crypto_stream_key(key, cfg.psk, st->stream_id, hello.nonce,
                               challenge.nonce);
        fprintf(stderr, "testing network for display %u (%d Hz)\n",
                st->display_id, st->refresh_hz);
        if (nd_network_test(s, 0, host_ip, st->port, discovered_if,
                             st->refresh_hz, st->stream_id,
                             cfg.frame_encryption ? key : NULL) < 0)
          session_ok = 0;
        memset(key, 0, sizeof(key));
      }
    }
    for (int i = 0; session_ok && i < display_count; i++) {
      struct client_stream_runtime *st = &streams[i];
      if (atomic_load(&input.quit)) {
        session_ok = 0;
        break;
      }
      uint8_t key[ND_KEY_SIZE] = {0};
      if (cfg.frame_encryption)
        nd_crypto_stream_key(key, cfg.psk, st->stream_id, hello.nonce,
                             challenge.nonce);
      int ready_fd = -1;
      st->pid = spawn_builtin_receiver(&cfg, master_fd, st->display, st->port,
                                       st->width, st->height, st->refresh_hz,
                                       discovered_if, cfg.frame_encryption,
                                       st->stream_id, key,
                                       &ready_fd, &st->state_fd);
      if (st->pid < 0 || wait_receiver_ready(st->pid, ready_fd, 10000) < 0) {
        if (ready_fd >= 0)
          close(ready_fd);
        session_ok = 0;
        break;
      }
      close(ready_fd);
      struct ndc_stream_ready ready = {.display_id = htonl(st->display_id)};
      if (ndc_send_msg(s, NDC_STREAM_READY, &ready, sizeof(ready)) < 0)
        session_ok = 0;
    }

    nd_input_session(&input, s, session_ok && input_allowed && cfg.want_input);

    struct nd_power_snapshot *power = NULL;
    if (session_ok && cfg.send_power && (ntohl(welcome.flags) & NDC_FLAG_POWER_INFO))
      power = calloc(1, sizeof(*power));
    uint64_t next_power_ms = 0;
    int power_read_warned = 0;
    while (session_ok && !atomic_load(&input.quit)) {
      uint64_t now_ms = nd_power_now_ms();
      if (power && now_ms >= next_power_ms) {
        next_power_ms = now_ms + ND_POWER_INTERVAL_MS;
        if (nd_power_read("/sys/class/power_supply", power) == 0) {
          power_read_warned = 0;
          pthread_mutex_lock(&input.send_mutex);
          int sent = nd_power_send(s, power);
          pthread_mutex_unlock(&input.send_mutex);
          if (sent < 0) break;
        } else if (!power_read_warned) {
          fprintf(stderr, "cannot read power supplies: %s\n", strerror(errno));
          power_read_warned = 1;
        }
      }
      for (int i = 0; i < display_count; i++) {
        int status = 0;
        if (streams[i].pid > 0 &&
            waitpid(streams[i].pid, &status, WNOHANG) == streams[i].pid) {
          streams[i].pid = -1;
          session_ok = 0;
          break;
        }
      }
      if (!session_ok)
        break;
      struct pollfd p = {.fd = s, .events = POLLIN};
      int pr;
      do {
        pr = poll(&p, 1, 1000);
      } while (pr < 0 && errno == EINTR);
      if (pr < 0 || (pr > 0 && !(p.revents & POLLIN)))
        break;
      if (!pr) {
        pthread_mutex_lock(&input.send_mutex);
        int sr = ndc_send_msg(s, NDC_PING, NULL, 0);
        pthread_mutex_unlock(&input.send_mutex);
        if (sr < 0)
          break;
        continue;
      }
      uint8_t b[NDC_MAX_PAYLOAD];
      uint32_t len = sizeof(b);
      uint16_t type;
      if (ndc_recv_msg(s, &type, b, &len) <= 0)
        break;
      if (type == NDC_PING && len == 0) {
        pthread_mutex_lock(&input.send_mutex);
        int sr = ndc_send_msg(s, NDC_PONG, NULL, 0);
        pthread_mutex_unlock(&input.send_mutex);
        if (sr < 0)
          break;
      } else if (type == NDC_PONG && len == 0) {
      } else if (type == NDC_STOP && len == 0) {
        break;
      } else if (type == NDC_DISPLAY_STATE &&
                 len == sizeof(struct ndc_display_state)) {
        struct ndc_display_state state;
        memcpy(&state, b, sizeof(state));
        uint32_t flags = ntohl(state.flags);
        if (flags & NDC_DISPLAY_HAS_BRIGHTNESS)
          apply_backlight(&cfg, ntohl(state.brightness));
        if (flags & NDC_DISPLAY_HAS_DPMS) {
          for (int i = 0; i < display_count; i++) {
            ssize_t n;
            do {
              n = write(streams[i].state_fd, &state, sizeof(state));
            } while (n < 0 && errno == EINTR);
            if (n != (ssize_t)sizeof(state)) {
              session_ok = 0;
              break;
            }
          }
        }
      } else
        break;
    }
    free(power);
    nd_input_session(&input, -1, 0);
    shutdown(s, SHUT_RDWR);
    close(s);
    for (int i = 0; i < display_count; i++) {
      if (streams[i].state_fd >= 0)
        close(streams[i].state_fd);
      ndc_stop_child(&streams[i].pid);
    }
    if (!atomic_load(&input.quit))
      fprintf(stderr, "source disconnected; reconnecting\n");
  retry:
    if (atomic_load(&input.quit)) break;
    usleep((useconds_t)cfg.reconnect_ms * 1000u);
  }
  nd_input_stop(&input);
  close(master_fd);
  explicit_bzero(cfg.psk, sizeof(cfg.psk));
  fprintf(stderr, "receiver shut down\n");
  return 0;
}
