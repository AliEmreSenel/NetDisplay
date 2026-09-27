#define _GNU_SOURCE
#include "common.h"
#include "power_state.h"
#include <pthread.h>
#include <sys/stat.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #x, strerror(errno));    \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static void put(const char *root, const char *path, const char *value) {
  char name[512];
  snprintf(name, sizeof(name), "%s/%s", root, path);
  FILE *f = fopen(name, "w");
  CHECK(f);
  CHECK(fputs(value, f) >= 0);
  CHECK(fclose(f) == 0);
}
static void directory(const char *root, const char *path) {
  char name[512];
  snprintf(name, sizeof(name), "%s/%s", root, path);
  CHECK(mkdir(name, 0700) == 0);
}
struct sender {
  int fd;
  const struct nd_power_snapshot *snapshot;
};
static void *send_snapshot(void *opaque) {
  struct sender *s = opaque;
  CHECK(nd_power_send(s->fd, s->snapshot) == 0);
  return NULL;
}
static struct nd_power_snapshot *roundtrip(const struct nd_power_snapshot *s) {
  int fds[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  struct sender tx = {.fd = fds[0], .snapshot = s};
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, send_snapshot, &tx) == 0);
  struct nd_power_rx rx = {0};
  struct nd_power_snapshot *got = NULL;
  while (!got) {
    uint8_t buf[NDC_MAX_PAYLOAD];
    uint32_t len = sizeof(buf);
    uint16_t type;
    CHECK(ndc_recv_msg(fds[1], &type, buf, &len) > 0);
    CHECK(nd_power_receive(&rx, type, buf, len, &got) >= 0);
  }
  CHECK(!rx.pending);
  CHECK(pthread_join(thread, NULL) == 0);
  close(fds[0]);
  close(fds[1]);
  return got;
}
int main(void) {
  char root[] = "/tmp/netdisplay-power-test-XXXXXX";
  CHECK(mkdtemp(root));
  directory(root, "BAT0");
  directory(root, "AC");
  directory(root, "netdisplay-loop");
  put(root, "BAT0/type", "Battery\n");
  put(root, "BAT0/capacity", "73\n");
  put(root, "BAT0/status", "Not charging\n");
  put(root, "BAT0/energy_now", "42000000\n");
  put(root, "BAT0/serial_number", "Test \\\"battery\n");
  put(root, "BAT0/uevent", "SKIP\n");
  put(root, "BAT0/driver_specific", "custom value\n");
  put(root, "AC/type", "Mains\n");
  put(root, "AC/online", "1\n");
  put(root, "netdisplay-loop/type", "Battery\n");
  char symlink_path[512];
  snprintf(symlink_path, sizeof(symlink_path), "%s/BAT0/link", root);
  CHECK(symlink("/etc/passwd", symlink_path) == 0);
  struct nd_power_snapshot *s = calloc(1, sizeof(*s));
  CHECK(s);
  CHECK(nd_power_read(root, s) == 0 && s->count == 2);
  CHECK(!strcmp(s->devices[0].name, "AC") &&
        !strcmp(s->devices[1].name, "BAT0"));
  CHECK(!strcmp(nd_power_get(&s->devices[1], "status"), "Not charging"));
  CHECK(
      !strcmp(nd_power_get(&s->devices[1], "driver_specific"), "custom value"));
  CHECK(!nd_power_get(&s->devices[1], "uevent") &&
        !nd_power_get(&s->devices[1], "link"));
  struct nd_power_snapshot *got = roundtrip(s);
  CHECK(memcmp(got, s, sizeof(*s)) == 0);
  free(got);
  put(root, "BAT0/status", "Charging\n");
  CHECK(nd_power_read(root, s) == 0);
  got = roundtrip(s);
  CHECK(!strcmp(nd_power_get(&got->devices[1], "status"), "Charging"));
  free(got);
  memset(s, 0, sizeof(*s));
  got = roundtrip(s);
  CHECK(got->count == 0);
  free(got);

  struct nd_power_rx rx = {0};
  uint32_t count = htonl(ND_POWER_MAX_DEVICES + 1);
  CHECK(nd_power_receive(&rx, NDC_POWER_BEGIN, &count, sizeof(count), &got) <
        0);
  CHECK(nd_power_receive(&rx, NDC_POWER_END, NULL, 0, &got) < 0);
  count = htonl(1);
  CHECK(nd_power_receive(&rx, NDC_POWER_BEGIN, &count, sizeof(count), &got) ==
        0);
  CHECK(nd_power_receive(&rx, NDC_POWER_END, NULL, 0, &got) < 0 && !rx.pending);
  CHECK(nd_power_receive(&rx, NDC_POWER_BEGIN, &count, sizeof(count), &got) ==
        0);
  CHECK(nd_power_receive(&rx, NDC_POWER_BEGIN, &count, sizeof(count), &got) <
            0 &&
        !rx.pending);
  struct __attribute__((packed)) {
    uint16_t id, count;
    char name[NDC_NAME_MAX];
  } device = {0};
  device.count = htons(2);
  strcpy(device.name, "BAT0");
  CHECK(nd_power_receive(&rx, NDC_POWER_BEGIN, &count, sizeof(count), &got) ==
        0);
  CHECK(nd_power_receive(&rx, NDC_POWER_DEVICE, &device, sizeof(device),
                         &got) == 0);
  struct __attribute__((packed)) {
    uint16_t id, reserved;
    struct nd_power_property property;
  } prop = {0};
  strcpy(prop.property.key, "type");
  strcpy(prop.property.value, "Battery");
  CHECK(nd_power_receive(&rx, NDC_POWER_PROPERTY, &prop, sizeof(prop), &got) ==
        0);
  CHECK(nd_power_receive(&rx, NDC_POWER_PROPERTY, &prop, sizeof(prop), &got) <
            0 &&
        !rx.pending);
  CHECK(nd_power_receive(&rx, NDC_POWER_BEGIN, &count, sizeof(count), &got) ==
        0);
  strcpy(device.name, "../BAT0");
  CHECK(nd_power_receive(&rx, NDC_POWER_DEVICE, &device, sizeof(device), &got) <
        0);
  strcpy(device.name, "BAT0");
  CHECK(nd_power_receive(&rx, NDC_POWER_BEGIN, &count, sizeof(count), &got) ==
        0);
  CHECK(nd_power_receive(&rx, NDC_POWER_DEVICE, &device, sizeof(device),
                         &got) == 0);
  memset(prop.property.value, 'x', sizeof(prop.property.value));
  CHECK(nd_power_receive(&rx, NDC_POWER_PROPERTY, &prop, sizeof(prop), &got) <
        0);
  CHECK(nd_power_receive(&rx, NDC_POWER_BEGIN, &count, sizeof(count), &got) ==
        0);
  rx.started_ms -= 6000;
  CHECK(nd_power_receive(&rx, NDC_POWER_DEVICE, &device, sizeof(device), &got) <
        0);
  free(s);
  const char *files[] = {"BAT0/type",
                         "BAT0/capacity",
                         "BAT0/status",
                         "BAT0/energy_now",
                         "BAT0/serial_number",
                         "BAT0/uevent",
                         "BAT0/driver_specific",
                         "BAT0/link",
                         "AC/type",
                         "AC/online",
                         "netdisplay-loop/type"};
  for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, files[i]);
    CHECK(unlink(path) == 0);
  }
  const char *dirs[] = {"BAT0", "AC", "netdisplay-loop"};
  for (unsigned i = 0; i < 3; i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
    CHECK(rmdir(path) == 0);
  }
  CHECK(rmdir(root) == 0);
  puts("power collection, roundtrip, state changes and malformed snapshots "
       "passed");
  return 0;
}
