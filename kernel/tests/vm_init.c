#define _GNU_SOURCE
#include "../../src/common.h"
#include "../../src/power_native.h"
#include <dirent.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>

static void finish(int success) {
  puts(success ? "NETDISPLAY POWER VM PASS" : "NETDISPLAY POWER VM FAIL");
  fflush(NULL);
  reboot(RB_POWER_OFF);
  _exit(success ? 0 : 1);
}
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s: %s\n", __LINE__, #x, strerror(errno));     \
      finish(0);                                                               \
    }                                                                          \
  } while (0)
static void property(struct nd_power_native *d, const char *key,
                     const char *value) {
  unsigned i;
  for (i = 0; i < d->count; i++)
    if (!strcmp(d->properties[i].key, key))
      break;
  if (i == d->count)
    d->count++;
  CHECK(d->count <= ND_POWER_MAX_PROPERTIES);
  snprintf(d->properties[i].key, ND_POWER_KEY_SIZE, "%s", key);
  snprintf(d->properties[i].value, ND_POWER_VALUE_SIZE, "%s", value);
}
static void read_check(const char *device, const char *key, const char *value) {
  char path[512], buf[512] = {0};
  snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s", device, key);
  int fd = open(path, O_RDONLY);
  CHECK(fd >= 0);
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  CHECK(n >= 0);
  close(fd);
  if (strcmp(buf, value))
    fprintf(stderr, "%s: got %s, expected %s\n", path, buf, value);
  CHECK(!strcmp(buf, value));
}
static void absent(const char *name) {
  char path[256];
  snprintf(path, sizeof(path), "/sys/class/power_supply/%s", name);
  CHECK(access(path, F_OK) < 0 && errno == ENOENT);
}
static int device_count(void) {
  DIR *dir = opendir("/sys/class/power_supply");
  CHECK(dir);
  int n = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)))
    if (!strncmp(entry->d_name, "netdisplay-", 11))
      n++;
  closedir(dir);
  return n;
}
static void check_upower(void) {
  if (access("/usr/bin/upowerd", X_OK) < 0) {
    puts("UPower unavailable: sysfs integration tests only");
    return;
  }
  pid_t bus = fork();
  CHECK(bus >= 0);
  if (!bus) {
    execl("/usr/bin/dbus-daemon", "dbus-daemon", "--nofork", "--nopidfile",
          "--config-file=/dbus.conf", (char *)NULL);
    _exit(127);
  }
  for (int i = 0; i < 50 && access("/run/dbus/system_bus_socket", F_OK); i++)
    usleep(100000);
  CHECK(access("/run/dbus/system_bus_socket", F_OK) == 0);
  pid_t daemon = fork();
  CHECK(daemon >= 0);
  if (!daemon) {
    execl("/usr/bin/upowerd", "upowerd", (char *)NULL);
    _exit(127);
  }
  int found = 0;
  for (int attempt = 0; attempt < 30 && !found; attempt++) {
    usleep(100000);
    pid_t query = fork();
    CHECK(query >= 0);
    if (!query) {
      int fd = open("/upower-dump", O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0)
        _exit(127);
      dup2(fd, STDOUT_FILENO);
      close(fd);
      execl("/usr/bin/upower", "upower", "--dump", (char *)NULL);
      _exit(127);
    }
    int status;
    CHECK(waitpid(query, &status, 0) == query);
    FILE *f = fopen("/upower-dump", "r");
    CHECK(f);
    char buf[16384] = {0};
    CHECK(fread(buf, 1, sizeof(buf) - 1, f) < sizeof(buf));
    fclose(f);
    found = strstr(buf, "netdisplay-test-battery") && strstr(buf, "ABC123") &&
            strstr(buf, "72%");
    if (found) {
      CHECK(strstr(buf, "power supply:         no"));
      puts("UPower discovered the remote battery, identity and capacity as a "
           "peripheral");
    }
    if (attempt == 29 && !found)
      fputs(buf, stderr);
  }
  CHECK(found);
  kill(daemon, SIGTERM);
  CHECK(waitpid(daemon, NULL, 0) == daemon);
  kill(bus, SIGTERM);
  CHECK(waitpid(bus, NULL, 0) == bus);
}
int main(void) {
  /* This binary must only run as init in the disposable test VM. */
  if (getpid() != 1) {
    fputs("Run through run-vm.sh, not on the host.\n", stderr);
    return 2;
  }
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);
  CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
  CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
  CHECK(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);
  int module = open("/netdisplay_power.ko", O_RDONLY);
  CHECK(module >= 0);
  CHECK(syscall(SYS_finit_module, module, "", 0) == 0);
  close(module);
  int a = open(ND_POWER_DEVICE_PATH, O_WRONLY | O_CLOEXEC);
  CHECK(a >= 0);
  int b = open(ND_POWER_DEVICE_PATH, O_WRONLY | O_CLOEXEC);
  CHECK(b >= 0);
  struct nd_power_native *d = calloc(1, sizeof(*d));
  CHECK(d);
  d->version = ND_POWER_ABI_VERSION;
  strcpy(d->name, "netdisplay-test-battery");
  property(d, "type", "Battery");
  property(d, "capacity", "73");
  property(d, "status", "Charging");
  property(d, "present", "1");
  property(d, "voltage_now", "12100000");
  property(d, "energy_now", "42000000");
  property(d, "energy_full", "60000000");
  property(d, "energy_full_design", "65000000");
  property(d, "cycle_count", "301");
  property(d, "temp", "325");
  property(d, "manufacturer", "Test vendor");
  property(d, "serial_number", "ABC123");
  property(d, "model_name", "Remote computer");
  property(d, "health", "Good");
  property(d, "technology", "Li-ion");
  property(d, "driver_extra", "opaque detail");
  property(d, "scope", "System");
  CHECK(write(a, d, sizeof(*d) - 1) < 0 && errno == EMSGSIZE);
  d->version++;
  CHECK(write(a, d, sizeof(*d)) < 0 && errno == EINVAL);
  d->version--;
  CHECK(write(a, d, sizeof(*d)) == sizeof(*d));
  read_check(d->name, "scope", "Device\n");
  read_check(d->name, "remote/scope", "System\n");
  read_check(d->name, "capacity", "73\n");
  read_check(d->name, "status", "Charging\n");
  read_check(d->name, "voltage_now", "12100000\n");
  read_check(d->name, "energy_full", "60000000\n");
  read_check(d->name, "cycle_count", "301\n");
  read_check(d->name, "temp", "325\n");
  read_check(d->name, "serial_number", "ABC123\n");
  read_check(d->name, "health", "Good\n");
  read_check(d->name, "technology", "Li-ion\n");
  read_check(d->name, "remote/driver_extra", "opaque detail\n");
  const char *states[] = {"Discharging", "Not charging", "Full", "Unknown"};
  for (unsigned i = 0; i < 4; i++) {
    char expected[64];
    snprintf(expected, sizeof(expected), "%s\n", states[i]);
    property(d, "status", states[i]);
    CHECK(write(a, d, sizeof(*d)) == sizeof(*d));
    read_check(d->name, "status", expected);
  }
  property(d, "capacity", "72");
  CHECK(write(a, d, sizeof(*d)) == sizeof(*d));
  read_check(d->name, "capacity", "72\n");
  property(d, "serial_number", "changed");
  CHECK(write(a, d, sizeof(*d)) < 0 && errno == ESTALE);
  property(d, "serial_number", "ABC123");
  strcpy(d->properties[0].key, "../type");
  CHECK(write(a, d, sizeof(*d)) < 0 && errno == EINVAL);
  strcpy(d->properties[0].key, "type");
  memset(d->properties[1].value, 'x', ND_POWER_VALUE_SIZE);
  CHECK(write(a, d, sizeof(*d)) < 0 && errno == EINVAL);
  property(d, "capacity", "72");
  CHECK(write(a, d, sizeof(*d)) == sizeof(*d));
  memset(d, 0, sizeof(*d));
  d->version = ND_POWER_ABI_VERSION;
  strcpy(d->name, "netdisplay-test-charger");
  property(d, "type", "USB_PD");
  property(d, "online", "1");
  property(d, "input_power_limit", "65000000");
  property(d, "usb_type", "SDP DCP [PD]");
  CHECK(write(b, d, sizeof(*d)) == sizeof(*d));
  read_check(d->name, "type", "USB_PD\n");
  read_check(d->name, "online", "1\n");
  read_check(d->name, "scope", "Device\n");
  read_check(d->name, "input_power_limit", "65000000\n");
  read_check(d->name, "remote/usb_type", "SDP DCP [PD]\n");
  property(d, "online", "0");
  CHECK(write(b, d, sizeof(*d)) == sizeof(*d));
  read_check(d->name, "online", "0\n");
  CHECK(device_count() == 2);
  check_upower();
  close(a);
  absent("netdisplay-test-battery");
  CHECK(device_count() == 1);
  close(b);
  absent("netdisplay-test-charger");

  /* Exercise the actual userspace backend against the loaded module. */
  struct nd_power_snapshot *snapshot = calloc(1, sizeof(*snapshot));
  CHECK(snapshot);
  snapshot->count = 2;
  strcpy(snapshot->devices[0].name, "BAT0");
  snapshot->devices[0].count = 2;
  strcpy(snapshot->devices[0].properties[0].key, "type");
  strcpy(snapshot->devices[0].properties[0].value, "Battery");
  strcpy(snapshot->devices[0].properties[1].key, "capacity");
  strcpy(snapshot->devices[0].properties[1].value, "50");
  snapshot->devices[1] = snapshot->devices[0];
  strcpy(snapshot->devices[1].name, "BAT1");
  struct nd_power_sink sink, other;
  nd_power_sink_init(&sink);
  nd_power_sink_init(&other);
  CHECK(nd_power_sink_update(&sink, snapshot, "192.0.2.1", 1) == 0);
  CHECK(nd_power_sink_update(&other, snapshot, "192.0.2.2", 2) == 0);
  CHECK(device_count() == 4);
  snapshot->count = 1;
  CHECK(nd_power_sink_update(&sink, snapshot, "192.0.2.1", 1) == 0);
  CHECK(device_count() == 3);
  snapshot->devices[0].count++;
  strcpy(snapshot->devices[0].properties[2].key, "status");
  strcpy(snapshot->devices[0].properties[2].value, "Full");
  CHECK(nd_power_sink_update(&sink, snapshot, "192.0.2.1", 1) == 0);
  read_check("netdisplay-192.0.2.1-1-1-BAT0", "status", "Full\n");
  nd_power_sink_clear(&other);
  CHECK(device_count() == 1);
  puts("Waiting for kernel stale-device expiry...");
  sleep(17);
  CHECK(device_count() == 0);
  CHECK(nd_power_sink_update(&sink, snapshot, "192.0.2.1", 1) == 0);
  CHECK(device_count() == 1);
  nd_power_sink_expire(&sink, nd_power_now_ms() + ND_POWER_STALE_MS + 1);
  CHECK(device_count() == 0);
  nd_power_sink_clear(&sink);
  free(snapshot);
  free(d);
  CHECK(syscall(SYS_delete_module, "netdisplay_power", O_NONBLOCK) == 0);
  finish(1);
}
