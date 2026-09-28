#define _GNU_SOURCE
#include "common.h"
#include "input_receiver.h"
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>

static void finish(int ok) {
  puts(ok ? "NETDISPLAY INPUT VM PASS" : "NETDISPLAY INPUT VM FAIL");
  fflush(NULL);
  reboot(RB_POWER_OFF);
  _exit(ok ? 0 : 1);
}
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s: %s\n", __LINE__, #x, strerror(errno));     \
      finish(0);                                                               \
    }                                                                          \
  } while (0)
static void wakeup(int signal_number) { (void)signal_number; }
static int create_device(int keyboard) {
  int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
  CHECK(fd >= 0);
  CHECK(ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0);
  if (keyboard) {
    const int keys[] = {KEY_A,     KEY_C,   KEY_SPACE,
                        KEY_ENTER, KEY_ESC, KEY_LEFTCTRL};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
      CHECK(ioctl(fd, UI_SET_KEYBIT, keys[i]) == 0);
  } else {
    CHECK(ioctl(fd, UI_SET_EVBIT, EV_REL) == 0);
    CHECK(ioctl(fd, UI_SET_RELBIT, REL_X) == 0);
    CHECK(ioctl(fd, UI_SET_RELBIT, REL_Y) == 0);
    CHECK(ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) == 0);
  }
  struct uinput_setup setup = {.id = {.bustype = BUS_USB,
                                      .vendor = 0x4e44,
                                      .product = keyboard ? 10 : 11}};
  snprintf(setup.name, sizeof(setup.name), "NetDisplay test %s",
           keyboard ? "keyboard" : "mouse");
  CHECK(ioctl(fd, UI_DEV_SETUP, &setup) == 0);
  CHECK(ioctl(fd, UI_DEV_CREATE) == 0);
  return fd;
}
static void event(int fd, uint16_t type, uint16_t code, int value) {
  struct input_event events[2] = {{.type = type, .code = code, .value = value},
                                  {.type = EV_SYN, .code = SYN_REPORT}};
  CHECK(write(fd, events, sizeof(events)) == sizeof(events));
}
static int expect(int fd, uint16_t type, uint16_t code, int value,
                  int timeout) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long long end =
      (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 + timeout;
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    int remaining =
        (int)(end - ((long long)now.tv_sec * 1000 + now.tv_nsec / 1000000));
    if (remaining <= 0)
      return 0;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int ready = poll(&p, 1, remaining);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0)
      return 0;
    struct ndc_input input;
    uint32_t length = sizeof(input);
    uint16_t message;
    if (ndc_recv_msg(fd, &message, &input, &length) <= 0)
      return 0;
    CHECK(message == NDC_INPUT && length == sizeof(input));
    if (ntohs(input.type) == type && ntohs(input.code) == code &&
        (int32_t)ntohl((uint32_t)input.value) == value)
      return 1;
  }
}
int main(void) {
  if (getpid() != 1) {
    fputs("Use run-input-vm.sh; this test is VM-only.\n", stderr);
    return 2;
  }
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);
  CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
  CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
  CHECK(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);
  FILE *modules = fopen("/modules.list", "r");
  CHECK(modules);
  char path[256];
  while (fgets(path, sizeof(path), modules)) {
    path[strcspn(path, "\n")] = 0;
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(syscall(SYS_finit_module, fd, "", 0) == 0);
    close(fd);
  }
  fclose(modules);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGUSR1, wakeup);
  int sockets[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  struct timeval timeout = {.tv_sec = 2};
  CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) == 0);
  struct nd_input_context input;
  CHECK(nd_input_start(&input, 1) == 0);
  nd_input_session(&input, sockets[0], 1);
  usleep(200000);
  CHECK(atomic_load(&input.running));
  int keyboard = create_device(1);
  usleep(1200000);
  event(keyboard, EV_KEY, KEY_LEFTCTRL, 1);
  event(keyboard, EV_KEY, KEY_C, 1);
  CHECK(expect(sockets[1], EV_KEY, KEY_LEFTCTRL, 1, 2000));
  CHECK(expect(sockets[1], EV_KEY, KEY_C, 1, 2000));
  event(keyboard, EV_KEY, KEY_C, 0);
  event(keyboard, EV_KEY, KEY_LEFTCTRL, 0);
  CHECK(expect(sockets[1], EV_KEY, KEY_LEFTCTRL, 0, 2000));
  CHECK(!atomic_load(&input.quit));
  int mouse = create_device(0);
  usleep(1200000);
  event(mouse, EV_REL, REL_X, 7);
  CHECK(expect(sockets[1], EV_REL, REL_X, 7, 2000));
  event(mouse, EV_KEY, BTN_LEFT, 1);
  CHECK(expect(sockets[1], EV_KEY, BTN_LEFT, 1, 2000));
  CHECK(ioctl(mouse, UI_DEV_DESTROY) == 0);
  close(mouse);
  CHECK(expect(sockets[1], EV_KEY, BTN_LEFT, 0, 2000));
  event(keyboard, EV_KEY, KEY_A, 1);
  CHECK(expect(sockets[1], EV_KEY, KEY_A, 1, 2000));
  int second = create_device(1);
  usleep(1200000);
  event(second, EV_KEY, KEY_A, 1);
  CHECK(!expect(sockets[1], EV_KEY, KEY_A, 1, 200));
  CHECK(ioctl(keyboard, UI_DEV_DESTROY) == 0);
  close(keyboard);
  CHECK(!expect(sockets[1], EV_KEY, KEY_A, 0, 300));
  event(second, EV_KEY, KEY_A, 0);
  CHECK(expect(sockets[1], EV_KEY, KEY_A, 0, 2000));
  nd_input_prompt(&input, 1);
  CHECK(input.paused);
  event(second, EV_KEY, KEY_C, 1);
  event(second, EV_KEY, KEY_C, 0);
  CHECK(!expect(sockets[1], EV_KEY, KEY_C, 1, 200));
  nd_input_prompt(&input, 0);
  usleep(200000);
  event(second, EV_KEY, KEY_ESC, 1);
  for (int i = 0; i < 8; i++)
    event(second, EV_KEY, KEY_ESC, 2);
  event(second, EV_KEY, KEY_ESC, 0);
  CHECK(expect(sockets[1], EV_KEY, KEY_ESC, 0, 2000));
  CHECK(!atomic_load(&input.quit));
  event(second, EV_KEY, KEY_C, 1);
  event(second, EV_KEY, KEY_C, 0);
  CHECK(expect(sockets[1], EV_KEY, KEY_C, 0, 2000));
  /* The local exit shortcut must work even when forwarding is disabled. */
  nd_input_session(&input, sockets[0], 0);
  usleep(200000);
  for (int i = 0; i < 4; i++) {
    event(second, EV_KEY, KEY_ESC, 1);
    event(second, EV_KEY, KEY_ESC, 0);
  }
  usleep(200000);
  CHECK(!atomic_load(&input.quit));
  /* Reconnection must not erase four already completed local presses. */
  nd_input_session(&input, -1, 0);
  usleep(200000);
  event(second, EV_KEY, KEY_ESC, 1);
  event(second, EV_KEY, KEY_ESC, 0);
  for (int i = 0; i < 50 && atomic_load(&input.running); i++)
    usleep(100000);
  CHECK(atomic_load(&input.quit) && !atomic_load(&input.running));
  nd_input_session(&input, -1, 0);
  nd_input_stop(&input);
  close(second);
  close(sockets[0]);
  close(sockets[1]);
  finish(1);
}
