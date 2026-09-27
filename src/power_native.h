#ifndef NETDISPLAY_POWER_NATIVE_H
#define NETDISPLAY_POWER_NATIVE_H
#include "power_state.h"

struct nd_power_sink {
  int fds[ND_POWER_MAX_DEVICES];
  char names[ND_POWER_MAX_DEVICES][NDC_NAME_MAX];
  uint64_t updated_ms;
  int warned;
};
void nd_power_sink_init(struct nd_power_sink *sink);
void nd_power_sink_clear(struct nd_power_sink *sink);
int nd_power_sink_update(struct nd_power_sink *sink,
                         const struct nd_power_snapshot *snapshot,
                         const char *peer, unsigned serial);
void nd_power_sink_expire(struct nd_power_sink *sink, uint64_t now);
#endif
