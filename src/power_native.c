#define _GNU_SOURCE
#include "power_native.h"
#include "common.h"

void nd_power_sink_init(struct nd_power_sink *sink) {
  memset(sink, 0, sizeof(*sink));
  for (unsigned i = 0; i < ND_POWER_MAX_DEVICES; i++)
    sink->fds[i] = -1;
}
void nd_power_sink_clear(struct nd_power_sink *sink) {
  for (unsigned i = 0; i < ND_POWER_MAX_DEVICES; i++) {
    if (sink->fds[i] >= 0)
      close(sink->fds[i]);
    sink->fds[i] = -1;
    sink->names[i][0] = 0;
  }
  sink->updated_ms = 0;
}
void nd_power_sink_expire(struct nd_power_sink *sink, uint64_t now) {
  if (sink->updated_ms && now - sink->updated_ms > ND_POWER_STALE_MS)
    nd_power_sink_clear(sink);
}
static int native_write(int fd, const struct nd_power_native *data) {
  ssize_t n;
  do {
    n = write(fd, data, sizeof(*data));
  } while (n < 0 && errno == EINTR);
  if (n == sizeof(*data))
    return 0;
  if (n >= 0)
    errno = EIO;
  return -1;
}
int nd_power_sink_update(struct nd_power_sink *sink,
                         const struct nd_power_snapshot *snapshot,
                         const char *peer, unsigned serial) {
  struct nd_power_native *data = calloc(1, sizeof(*data));
  if (!data)
    return -1;
  int result = 0;
  for (unsigned i = 0; i < ND_POWER_MAX_DEVICES; i++) {
    int found = 0;
    for (unsigned j = 0; j < snapshot->count; j++)
      if (!strcmp(sink->names[i], snapshot->devices[j].name))
        found = 1;
    if (!found) {
      if (sink->fds[i] >= 0)
        close(sink->fds[i]);
      sink->fds[i] = -1;
      sink->names[i][0] = 0;
    }
  }
  for (unsigned j = 0; j < snapshot->count; j++) {
    const struct nd_power_device *device = &snapshot->devices[j];
    unsigned slot;
    for (slot = 0; slot < ND_POWER_MAX_DEVICES; slot++)
      if (!strcmp(sink->names[slot], device->name))
        break;
    if (slot == ND_POWER_MAX_DEVICES)
      for (slot = 0; slot < ND_POWER_MAX_DEVICES && sink->names[slot][0];
           slot++) {
      }
    if (slot == ND_POWER_MAX_DEVICES) {
      result = -1;
      break;
    }
    snprintf(sink->names[slot], sizeof(sink->names[slot]), "%s", device->name);
    memset(data, 0, sizeof(*data));
    data->version = ND_POWER_ABI_VERSION;
    data->count = device->count;
    snprintf(data->name, sizeof(data->name), "netdisplay-%s-%ld-%u-%s", peer,
             (long)getpid(), serial, device->name);
    memcpy(data->properties, device->properties,
           device->count * sizeof(device->properties[0]));
    if (sink->fds[slot] < 0)
      sink->fds[slot] =
          open(ND_POWER_DEVICE_PATH, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    int rc = sink->fds[slot] < 0 ? -1 : native_write(sink->fds[slot], data);
    if (rc < 0 && errno == ESTALE && sink->fds[slot] >= 0) {
      close(sink->fds[slot]);
      sink->fds[slot] =
          open(ND_POWER_DEVICE_PATH, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
      rc = sink->fds[slot] < 0 ? -1 : native_write(sink->fds[slot], data);
    }
    if (rc < 0) {
      if (!sink->warned) {
        fprintf(stderr,
                "cannot mirror power devices for %s: %s; install/enable the "
                "optional netdisplay-power DKMS module and check device "
                "permissions\n",
                peer, strerror(errno));
        sink->warned = 1;
      }
      if (sink->fds[slot] >= 0)
        close(sink->fds[slot]);
      sink->fds[slot] = -1;
      result = -1;
    }
  }
  if (!result)
    sink->warned = 0;
  sink->updated_ms = nd_power_now_ms();
  free(data);
  return result;
}
