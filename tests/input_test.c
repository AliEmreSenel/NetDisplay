// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "common.h"
#include "input_receiver.h"
#include <poll.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #x, strerror(errno));    \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
int main(void) {
  struct nd_input_state state = {0};
  struct nd_input_keys keyboard = {0}, second = {0}, mouse = {0};
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_LEFTCTRL, 1) == 1);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_C, 1) == 1);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_C, 0) == 1);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_LEFTCTRL, 0) == 1);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_ESC, 1) == 1);
  for (int i = 0; i < 10; i++)
    CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_ESC, 2) == 0);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_ESC, 1) == 0);
  CHECK(state.escape_count == 1);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_ESC, 0) == 1);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_A, 1) == 1 &&
        state.escape_count == 0);
  CHECK(nd_input_key(&state, &second, NDC_DEV_KBM, 1, KEY_A, 1) == 0);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_A, 0) == 0);
  CHECK(nd_input_key(&state, &second, NDC_DEV_KBM, 1, KEY_A, 0) == 1);
  CHECK(nd_input_key(&state, &second, NDC_DEV_KBM, 1, KEY_A, 0) == 0);
  for (int i = 0; i < 4; i++) {
    CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_ESC, 1) == 1);
    CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_ESC, 0) == 1);
  }
  CHECK(nd_input_key(&state, &mouse, NDC_DEV_KBM, 0, BTN_LEFT, 1) == 1);
  CHECK(nd_input_key(&state, &mouse, NDC_DEV_KBM, 0, BTN_LEFT, 0) == 1 &&
        state.escape_count == 4);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_ESC, 1) == 2);
  CHECK(nd_input_key(&state, &keyboard, NDC_DEV_KBM, 1, KEY_CNT, 1) == 0);

  int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  CHECK(master >= 0);
  CHECK(grantpt(master) == 0 && unlockpt(master) == 0);
  int slave = open(ptsname(master), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  CHECK(slave >= 0);
  struct termios before, during, after;
  CHECK(tcgetattr(slave, &before) == 0);
  struct nd_input_terminal terminal = {.fd = slave};
  CHECK(nd_input_terminal_enter(&terminal, slave) == 0);
  CHECK(tcgetattr(slave, &during) == 0);
  CHECK(!(during.c_lflag & (ISIG | ICANON | ECHO)));
  CHECK(during.c_oflag == before.c_oflag);
  const char control[] = {3, 27};
  CHECK(write(master, control, sizeof(control)) == sizeof(control));
  struct pollfd p = {.fd = slave, .events = POLLIN};
  CHECK(poll(&p, 1, 1000) == 1);
  char got[2];
  CHECK(read(slave, got, sizeof(got)) == sizeof(got));
  CHECK(!memcmp(got, control, sizeof(got)));
  nd_input_terminal_restore(&terminal);
  CHECK(tcgetattr(slave, &after) == 0);
  CHECK(after.c_iflag == before.c_iflag && after.c_oflag == before.c_oflag &&
        after.c_lflag == before.c_lflag && after.c_cflag == before.c_cflag);
  CHECK(!memcmp(after.c_cc, before.c_cc, sizeof(before.c_cc)));
  nd_input_terminal_restore(&terminal);
  close(slave);
  close(master);
  puts("raw Ctrl+C, terminal restoration, Escape presses and shared key "
       "lifetimes passed");
  return 0;
}
