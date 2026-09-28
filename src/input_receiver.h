#ifndef NETDISPLAY_INPUT_RECEIVER_H
#define NETDISPLAY_INPUT_RECEIVER_H
#include <linux/input.h>
#include <pthread.h>
#include <stdatomic.h>
#include <termios.h>

#define ND_INPUT_MAX_DEVICES 32
struct nd_input_keys {
  unsigned char down[KEY_CNT];
};
struct nd_input_state {
  unsigned short held[2][KEY_CNT];
  unsigned escape_count;
};
/* 1 = forward transition, 0 = ignore, 2 = local quit. */
int nd_input_key(struct nd_input_state *state, struct nd_input_keys *device,
                 unsigned cls, int keyboard, unsigned code, int value);

struct nd_input_terminal {
  int fd, active;
  struct termios saved;
};
int nd_input_terminal_enter(struct nd_input_terminal *terminal, int fd);
void nd_input_terminal_restore(struct nd_input_terminal *terminal);

struct nd_input_context {
  pthread_mutex_t send_mutex;
  pthread_cond_t changed;
  pthread_t thread, owner;
  _Atomic int running, quit;
  int sock, forward, grab, prompting, paused, started;
  unsigned generation;
  struct nd_input_terminal terminal;
};
int nd_input_start(struct nd_input_context *ctx, int grab);
void nd_input_session(struct nd_input_context *ctx, int fd, int forward);
void nd_input_prompt(struct nd_input_context *ctx, int prompting);
void nd_input_stop(struct nd_input_context *ctx);
#endif
