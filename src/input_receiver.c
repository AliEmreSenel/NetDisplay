// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "input_receiver.h"
#include "common.h"
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8u)
#define NBITS(x) (((x) + BITS_PER_LONG - 1u) / BITS_PER_LONG)
#define TEST_BIT(a, b)                                                         \
  (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1ul)
#define MAX_INPUT_DEVS ND_INPUT_MAX_DEVICES

struct input_dev {
  int fd;
  int keyboard, grabbed, grab_warned, dropped;
  dev_t rdev;
  ino_t inode;
  struct nd_input_keys keys;
  uint8_t cls;
  char path[256];
  struct input_absinfo abs_x, abs_y, mt_x, mt_y, pressure, mt_pressure;
  int have_abs_x, have_abs_y, have_mt_x, have_mt_y, have_pressure,
      have_mt_pressure;
};

static int get_bits(int fd, int ev, unsigned long *bits, size_t nbytes) {
  memset(bits, 0, nbytes);
  return ioctl(fd, EVIOCGBIT(ev, nbytes), bits);
}

static uint8_t classify_input(int fd) {
  unsigned long evbits[NBITS(EV_MAX + 1)];
  if (get_bits(fd, 0, evbits, sizeof(evbits)) < 0)
    return 0;

  if (TEST_BIT(evbits, EV_ABS)) {
    unsigned long absbits[NBITS(ABS_MAX + 1)];
    if (get_bits(fd, EV_ABS, absbits, sizeof(absbits)) >= 0 &&
        TEST_BIT(absbits, ABS_MT_POSITION_X) &&
        TEST_BIT(absbits, ABS_MT_POSITION_Y))
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
        TEST_BIT(keybits, KEY_A) && TEST_BIT(keybits, KEY_SPACE) &&
        TEST_BIT(keybits, KEY_ENTER))
      return NDC_DEV_KBM;
  }
  return 0;
}

static void read_abs(int fd, int code, struct input_absinfo *a, int *have) {
  if (ioctl(fd, EVIOCGABS(code), a) == 0 && a->maximum > a->minimum)
    *have = 1;
}

static int is_keyboard(int fd) {
  unsigned long bits[NBITS(KEY_CNT)];
  return get_bits(fd, EV_KEY, bits, sizeof(bits)) >= 0 &&
         TEST_BIT(bits, KEY_A) && TEST_BIT(bits, KEY_SPACE) &&
         TEST_BIT(bits, KEY_ENTER);
}

static void set_grab(struct input_dev *d, int grab) {
  if (grab == d->grabbed)
    return;
  if (ioctl(d->fd, EVIOCGRAB, grab) == 0) {
    d->grabbed = grab;
    d->grab_warned = 0;
  } else if (grab && !d->grab_warned) {
    fprintf(stderr, "warning: cannot grab %s: %s\n", d->path, strerror(errno));
    d->grab_warned = 1;
  }
}

static void scan_inputs(struct input_dev *devs, int grab) {
  glob_t g = {0};
  if (glob("/dev/input/event*", 0, NULL, &g) != 0) {
    globfree(&g);
    return;
  }
  for (size_t i = 0; i < g.gl_pathc; i++) {
    struct stat st;
    if (stat(g.gl_pathv[i], &st) < 0)
      continue;
    int exists = 0, slot = -1;
    for (int j = 0; j < MAX_INPUT_DEVS; j++) {
      if (devs[j].fd < 0)
        slot = j;
      else if (devs[j].inode == st.st_ino && devs[j].rdev == st.st_rdev)
        exists = 1;
    }
    if (exists || slot < 0)
      continue;
    int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;
    uint8_t cls = classify_input(fd);
    if (!cls || fstat(fd, &st) < 0) {
      close(fd);
      continue;
    }
    struct input_dev *d = &devs[slot];
    memset(d, 0, sizeof(*d));
    d->fd = fd;
    d->cls = cls;
    d->rdev = st.st_rdev;
    d->inode = st.st_ino;
    d->keyboard = is_keyboard(fd);
    snprintf(d->path, sizeof(d->path), "%s", g.gl_pathv[i]);
    if (cls == NDC_DEV_TOUCHPAD) {
      read_abs(fd, ABS_X, &d->abs_x, &d->have_abs_x);
      read_abs(fd, ABS_Y, &d->abs_y, &d->have_abs_y);
      read_abs(fd, ABS_MT_POSITION_X, &d->mt_x, &d->have_mt_x);
      read_abs(fd, ABS_MT_POSITION_Y, &d->mt_y, &d->have_mt_y);
      read_abs(fd, ABS_PRESSURE, &d->pressure, &d->have_pressure);
      read_abs(fd, ABS_MT_PRESSURE, &d->mt_pressure, &d->have_mt_pressure);
    }
    set_grab(d, grab);
    char name[128] = "?";
    (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    fprintf(stderr, "input attached: %s (%s)\n", name, d->path);
  }
  globfree(&g);
}

static int scale_abs(int v, const struct input_absinfo *a, int outmax) {
  if (!a || a->maximum <= a->minimum)
    return v;
  if (v <= a->minimum)
    return 0;
  if (v >= a->maximum)
    return outmax;
  int64_t num = (int64_t)(v - a->minimum) * outmax;
  return (int)(num / (a->maximum - a->minimum));
}

static int touchpad_abs_value(struct input_dev *d, uint16_t code, int v,
                              int *supported) {
  *supported = 1;
  switch (code) {
  case ABS_X:
    return d->have_abs_x ? scale_abs(v, &d->abs_x, 10000) : v;
  case ABS_Y:
    return d->have_abs_y ? scale_abs(v, &d->abs_y, 7000) : v;
  case ABS_MT_POSITION_X:
    return d->have_mt_x ? scale_abs(v, &d->mt_x, 10000) : v;
  case ABS_MT_POSITION_Y:
    return d->have_mt_y ? scale_abs(v, &d->mt_y, 7000) : v;
  case ABS_PRESSURE:
    return d->have_pressure ? scale_abs(v, &d->pressure, 255) : v;
  case ABS_MT_PRESSURE:
    return d->have_mt_pressure ? scale_abs(v, &d->mt_pressure, 255) : v;
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
    *supported = 0;
    return v;
  }
}

int nd_input_key(struct nd_input_state *state, struct nd_input_keys *device,
                 unsigned cls, int keyboard, unsigned code, int value) {
  if (code >= KEY_CNT || (value != 0 && value != 1) || cls < 1 || cls > 2)
    return 0;
  if (device->down[code] == value)
    return 0;
  device->down[code] = (unsigned char)value;
  if (keyboard && value == 1 && (code < BTN_MISC || code >= KEY_OK)) {
    if (code == KEY_ESC) {
      if (++state->escape_count == 5)
        return 2;
    } else
      state->escape_count = 0;
  }
  unsigned short *held = &state->held[cls - 1][code];
  if (value)
    return ++*held == 1;
  if (!*held)
    return 0;
  return --*held == 0;
}

int nd_input_terminal_enter(struct nd_input_terminal *terminal, int fd) {
  if (terminal->active)
    return 0;
  if (tcgetattr(fd, &terminal->saved) < 0)
    return -1;
  struct termios raw = terminal->saved;
  cfmakeraw(&raw);
  /* Only input is raw; keep normal newline formatting for diagnostics. */
  raw.c_oflag = terminal->saved.c_oflag;
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSAFLUSH, &raw) < 0)
    return -1;
  terminal->fd = fd;
  terminal->active = 1;
  return 0;
}
void nd_input_terminal_restore(struct nd_input_terminal *terminal) {
  if (!terminal->active)
    return;
  (void)tcsetattr(terminal->fd, TCSAFLUSH, &terminal->saved);
  terminal->active = 0;
}

/* Caller holds send_mutex; session detachment cannot race fd reuse. */
static int send_input(struct nd_input_context *ctx, uint8_t dev, uint16_t type,
                      uint16_t code, int value) {
  if (!ctx->forward || ctx->sock < 0)
    return 0;
  struct ndc_input w = {.device = dev,
                        .type = htons(type),
                        .code = htons(code),
                        .value = (int32_t)htonl((uint32_t)value)};
  int r = ndc_send_msg(ctx->sock, NDC_INPUT, &w, sizeof(w));
  if (r < 0) {
    ctx->forward = 0;
    shutdown(ctx->sock, SHUT_RDWR);
  }
  return r;
}

static void release_input(struct nd_input_context *ctx,
                          struct nd_input_state *state,
                          struct input_dev *device) {
  for (unsigned code = 0; code < KEY_CNT; code++) {
    if (device->keys.down[code] &&
        nd_input_key(state, &device->keys, device->cls, 0, code, 0) == 1)
      (void)send_input(ctx, device->cls, EV_KEY, (uint16_t)code, 0);
  }
  if (device->cls == NDC_DEV_TOUCHPAD) {
    for (int slot = 0; slot < 16; slot++) {
      (void)send_input(ctx, device->cls, EV_ABS, ABS_MT_SLOT, slot);
      (void)send_input(ctx, device->cls, EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    (void)send_input(ctx, device->cls, EV_ABS, ABS_PRESSURE, 0);
  }
  (void)send_input(ctx, device->cls, EV_SYN, SYN_REPORT, 0);
}
static void remove_input(struct nd_input_context *ctx,
                         struct nd_input_state *state,
                         struct input_dev *device) {
  release_input(ctx, state, device);
  if (device->keyboard)
    state->escape_count = 0;
  set_grab(device, 0);
  close(device->fd);
  device->fd = -1;
  fprintf(stderr, "input detached: %s\n", device->path);
}
static uint64_t input_clock_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void *input_main(void *opaque) {
  struct nd_input_context *ctx = opaque;
  struct input_dev devs[MAX_INPUT_DEVS];
  struct nd_input_state state = {0};
  memset(devs, 0, sizeof(devs));
  for (int i = 0; i < MAX_INPUT_DEVS; i++)
    devs[i].fd = -1;
  unsigned generation = 0, tty_escapes = 0;
  uint64_t next_scan = 0;
  while (atomic_load(&ctx->running) && !atomic_load(&ctx->quit)) {
    pthread_mutex_lock(&ctx->send_mutex);
    if (generation != ctx->generation) {
      /* Remote sessions change independently of the local exit sequence. */
      memset(state.held, 0, sizeof(state.held));
      for (int i = 0; i < MAX_INPUT_DEVS; i++)
        memset(&devs[i].keys, 0, sizeof(devs[i].keys));
      generation = ctx->generation;
    }
    if (ctx->prompting)
      nd_input_terminal_restore(&ctx->terminal);
    else if (ctx->terminal.fd >= 0)
      (void)nd_input_terminal_enter(&ctx->terminal, ctx->terminal.fd);
    if (input_clock_ms() >= next_scan) {
      for (int i = 0; i < MAX_INPUT_DEVS; i++) {
        struct stat st;
        if (devs[i].fd >= 0 &&
            (stat(devs[i].path, &st) < 0 || st.st_ino != devs[i].inode ||
             st.st_rdev != devs[i].rdev))
          remove_input(ctx, &state, &devs[i]);
      }
      scan_inputs(devs, ctx->grab && !ctx->prompting);
      next_scan = input_clock_ms() + 1000;
    }
    int keyboards = 0;
    for (int i = 0; i < MAX_INPUT_DEVS; i++) {
      if (devs[i].fd < 0)
        continue;
      set_grab(&devs[i], ctx->grab && !ctx->prompting);
      keyboards += devs[i].keyboard;
    }
    ctx->paused = ctx->prompting;
    pthread_cond_broadcast(&ctx->changed);
    struct pollfd pfds[MAX_INPUT_DEVS + 1];
    for (int i = 0; i < MAX_INPUT_DEVS; i++)
      pfds[i] = (struct pollfd){.fd = devs[i].fd, .events = POLLIN};
    pfds[MAX_INPUT_DEVS] = (struct pollfd){
        .fd = ctx->terminal.active ? ctx->terminal.fd : -1, .events = POLLIN};
    pthread_mutex_unlock(&ctx->send_mutex);
    int pr = poll(pfds, MAX_INPUT_DEVS + 1, 100);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    pthread_mutex_lock(&ctx->send_mutex);
    /* Session/prompt state may have changed while polling. */
    if (generation != ctx->generation || ctx->prompting != ctx->paused) {
      pthread_mutex_unlock(&ctx->send_mutex);
      continue;
    }
    if (!ctx->prompting && ctx->terminal.active &&
        (pfds[MAX_INPUT_DEVS].revents & POLLIN)) {
      unsigned char bytes[128];
      ssize_t n = read(ctx->terminal.fd, bytes, sizeof(bytes));
      for (ssize_t i = 0; !keyboards && i < n; i++) {
        tty_escapes = bytes[i] == 27 ? tty_escapes + 1 : 0;
        if (tty_escapes == 5)
          atomic_store(&ctx->quit, 1);
      }
    }
    for (int i = 0; i < MAX_INPUT_DEVS && !atomic_load(&ctx->quit); i++) {
      struct input_dev *d = &devs[i];
      if (d->fd < 0)
        continue;
      if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        remove_input(ctx, &state, d);
        continue;
      }
      if (!(pfds[i].revents & POLLIN))
        continue;
      /* Bound each batch so a busy mouse cannot starve shutdown/hotplug checks.
       */
      for (int batch = 0; batch < 128 && !atomic_load(&ctx->quit); batch++) {
        struct input_event ev;
        ssize_t n = read(d->fd, &ev, sizeof(ev));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          break;
        if (n < 0 && errno == EINTR)
          continue;
        if (n != sizeof(ev)) {
          remove_input(ctx, &state, d);
          break;
        }
        if (ctx->prompting) {
          if (ev.type == EV_KEY &&
              nd_input_key(&state, &d->keys, d->cls, d->keyboard, ev.code,
                           ev.value) == 2)
            atomic_store(&ctx->quit, 1);
          continue;
        }
        if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
          release_input(ctx, &state, d);
          d->dropped = 1;
          state.escape_count = 0;
          continue;
        }
        if (d->dropped) {
          if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            unsigned long keys[NBITS(KEY_CNT)] = {0};
            if (ioctl(d->fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
              remove_input(ctx, &state, d);
              break;
            }
            for (unsigned key = 0; key < KEY_CNT; key++) {
              if (TEST_BIT(keys, key) &&
                  nd_input_key(&state, &d->keys, d->cls, 0, key, 1) == 1)
                (void)send_input(ctx, d->cls, EV_KEY, (uint16_t)key, 1);
            }
            (void)send_input(ctx, d->cls, EV_SYN, SYN_REPORT, 0);
            d->dropped = 0;
          }
          continue;
        }
        if (ev.type == EV_KEY) {
          int action = nd_input_key(&state, &d->keys, d->cls, d->keyboard,
                                    ev.code, ev.value);
          if (action == 2) {
            atomic_store(&ctx->quit, 1);
            break;
          }
          if (action != 1)
            continue;
        }
        if (ev.type != EV_SYN && ev.type != EV_KEY && ev.type != EV_REL &&
            ev.type != EV_ABS)
          continue;
        int value = ev.value;
        if (d->cls == NDC_DEV_KBM && ev.type == EV_ABS)
          continue;
        if (d->cls == NDC_DEV_TOUCHPAD) {
          if (ev.type == EV_REL)
            continue;
          if (ev.type == EV_ABS) {
            int supported;
            value = touchpad_abs_value(d, ev.code, value, &supported);
            if (!supported)
              continue;
          }
        }
        (void)send_input(ctx, d->cls, ev.type, ev.code, value);
      }
    }
    pthread_mutex_unlock(&ctx->send_mutex);
  }
  if (atomic_load(&ctx->running) && !atomic_load(&ctx->quit))
    atomic_store(&ctx->quit, 1);
  pthread_mutex_lock(&ctx->send_mutex);
  for (int i = 0; i < MAX_INPUT_DEVS; i++)
    if (devs[i].fd >= 0)
      remove_input(ctx, &state, &devs[i]);
  if (ctx->sock >= 0)
    shutdown(ctx->sock, SHUT_RDWR);
  nd_input_terminal_restore(&ctx->terminal);
  ctx->paused = 1;
  atomic_store(&ctx->running, 0);
  pthread_cond_broadcast(&ctx->changed);
  pthread_mutex_unlock(&ctx->send_mutex);
  if (atomic_load(&ctx->quit))
    pthread_kill(ctx->owner, SIGUSR1);
  return NULL;
}
int nd_input_start(struct nd_input_context *ctx, int grab) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->sock = -1;
  ctx->grab = grab;
  ctx->owner = pthread_self();
  ctx->terminal.fd =
      open("/dev/tty", O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  pthread_mutex_init(&ctx->send_mutex, NULL);
  pthread_cond_init(&ctx->changed, NULL);
  atomic_init(&ctx->running, 1);
  atomic_init(&ctx->quit, 0);
  int error = pthread_create(&ctx->thread, NULL, input_main, ctx);
  if (error) {
    if (ctx->terminal.fd >= 0)
      close(ctx->terminal.fd);
    pthread_mutex_destroy(&ctx->send_mutex);
    pthread_cond_destroy(&ctx->changed);
    errno = error;
    return -1;
  }
  ctx->started = 1;
  fprintf(stderr,
          "Press Escape five times consecutively to shut down the receiver.\n");
  return 0;
}
void nd_input_session(struct nd_input_context *ctx, int fd, int forward) {
  pthread_mutex_lock(&ctx->send_mutex);
  ctx->sock = fd;
  ctx->forward = forward;
  ctx->generation++;
  if (fd >= 0 && atomic_load(&ctx->quit))
    shutdown(fd, SHUT_RDWR);
  pthread_mutex_unlock(&ctx->send_mutex);
}
void nd_input_prompt(struct nd_input_context *ctx, int prompting) {
  pthread_mutex_lock(&ctx->send_mutex);
  ctx->prompting = prompting;
  ctx->generation++;
  while (prompting && !ctx->paused && atomic_load(&ctx->running))
    pthread_cond_wait(&ctx->changed, &ctx->send_mutex);
  pthread_mutex_unlock(&ctx->send_mutex);
}
void nd_input_stop(struct nd_input_context *ctx) {
  if (!ctx->started)
    return;
  atomic_store(&ctx->running, 0);
  pthread_join(ctx->thread, NULL);
  if (ctx->terminal.fd >= 0)
    close(ctx->terminal.fd);
  pthread_mutex_destroy(&ctx->send_mutex);
  pthread_cond_destroy(&ctx->changed);
  ctx->started = 0;
}
