#define _GNU_SOURCE
#include "power_state.h"
#include "common.h"
#include <dirent.h>
#include <sys/stat.h>

struct __attribute__((packed)) power_device_wire {
  uint16_t id, count;
  char name[NDC_NAME_MAX];
};
struct __attribute__((packed)) power_property_wire {
  uint16_t id, reserved;
  struct nd_power_property property;
};
_Static_assert(sizeof(struct power_property_wire) <= NDC_MAX_PAYLOAD,
               "power properties must fit the control protocol");

uint64_t nd_power_now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int valid_name(const char *name, size_t size) {
  size_t n = strnlen(name, size);
  if (!n || n == size || !strcmp(name, ".") || !strcmp(name, ".."))
    return 0;
  for (size_t i = 0; i < n; i++)
    if (!((name[i] >= 'a' && name[i] <= 'z') ||
          (name[i] >= 'A' && name[i] <= 'Z') ||
          (name[i] >= '0' && name[i] <= '9') || name[i] == '_' ||
          name[i] == '-' || name[i] == '.'))
      return 0;
  return 1;
}

static int read_value(int dir, const char *name, char *value, size_t size) {
  int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return -1;
  }
  ssize_t n;
  do {
    n = read(fd, value, size);
  } while (n < 0 && errno == EINTR);
  close(fd);
  if (n < 0 || (size_t)n >= size)
    return -1;
  while (n && (value[n - 1] == '\n' || value[n - 1] == '\r'))
    n--;
  for (ssize_t i = 0; i < n; i++)
    if ((unsigned char)value[i] < 32 || (unsigned char)value[i] == 127)
      return -1;
  value[n] = 0;
  return 0;
}

static int property_cmp(const void *a, const void *b) {
  return strcmp(((const struct nd_power_property *)a)->key,
                ((const struct nd_power_property *)b)->key);
}
static int device_cmp(const void *a, const void *b) {
  return strcmp(((const struct nd_power_device *)a)->name,
                ((const struct nd_power_device *)b)->name);
}

int nd_power_read(const char *root, struct nd_power_snapshot *snapshot) {
  memset(snapshot, 0, sizeof(*snapshot));
  DIR *supplies = opendir(root);
  if (!supplies)
    return -1;
  struct dirent *entry;
  while ((entry = readdir(supplies))) {
    if (!valid_name(entry->d_name, NDC_NAME_MAX) ||
        !strncmp(entry->d_name, "netdisplay-", 11))
      continue;
    /* Class entries are symlinks to the kernel-owned device directory. */
    int fd = openat(dirfd(supplies), entry->d_name,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
      continue;
    DIR *attributes = fdopendir(fd);
    if (!attributes) {
      close(fd);
      continue;
    }
    char type[ND_POWER_VALUE_SIZE];
    if (read_value(fd, "type", type, sizeof(type)) < 0) {
      closedir(attributes);
      continue;
    }
    if (snapshot->count == ND_POWER_MAX_DEVICES) {
      closedir(attributes);
      closedir(supplies);
      errno = EOVERFLOW;
      return -1;
    }
    struct nd_power_device *device = &snapshot->devices[snapshot->count];
    memcpy(device->name, entry->d_name, strlen(entry->d_name) + 1);
    struct dirent *attr;
    while ((attr = readdir(attributes))) {
      if (!valid_name(attr->d_name, ND_POWER_KEY_SIZE) ||
          !strcmp(attr->d_name, "uevent"))
        continue;
      struct nd_power_property property = {0};
      if (read_value(fd, attr->d_name, property.value, sizeof(property.value)) <
          0)
        continue;
      if (device->count == ND_POWER_MAX_PROPERTIES) {
        closedir(attributes);
        closedir(supplies);
        errno = EOVERFLOW;
        return -1;
      }
      memcpy(property.key, attr->d_name, strlen(attr->d_name) + 1);
      device->properties[device->count++] = property;
    }
    closedir(attributes);
    if (!nd_power_get(device, "type")) {
      memset(device, 0, sizeof(*device));
      continue;
    }
    qsort(device->properties, device->count, sizeof(device->properties[0]),
          property_cmp);
    snapshot->count++;
  }
  closedir(supplies);
  qsort(snapshot->devices, snapshot->count, sizeof(snapshot->devices[0]),
        device_cmp);
  return 0;
}

const char *nd_power_get(const struct nd_power_device *device,
                         const char *key) {
  for (uint32_t i = 0; i < device->count; i++)
    if (!strcmp(device->properties[i].key, key))
      return device->properties[i].value;
  return NULL;
}

int nd_power_send(int fd, const struct nd_power_snapshot *snapshot) {
  uint32_t count = htonl(snapshot->count);
  if (ndc_send_msg(fd, NDC_POWER_BEGIN, &count, sizeof(count)) < 0)
    return -1;
  for (uint32_t i = 0; i < snapshot->count; i++) {
    const struct nd_power_device *d = &snapshot->devices[i];
    struct power_device_wire wire = {.id = htons((uint16_t)i),
                                     .count = htons((uint16_t)d->count)};
    memcpy(wire.name, d->name, sizeof(wire.name));
    if (ndc_send_msg(fd, NDC_POWER_DEVICE, &wire, sizeof(wire)) < 0)
      return -1;
    for (uint32_t j = 0; j < d->count; j++) {
      struct power_property_wire p = {.id = htons((uint16_t)i),
                                      .property = d->properties[j]};
      if (ndc_send_msg(fd, NDC_POWER_PROPERTY, &p, sizeof(p)) < 0)
        return -1;
    }
  }
  return ndc_send_msg(fd, NDC_POWER_END, NULL, 0);
}

void nd_power_rx_clear(struct nd_power_rx *rx) {
  free(rx->pending);
  memset(rx, 0, sizeof(*rx));
}

int nd_power_receive(struct nd_power_rx *rx, uint16_t type, const void *data,
                     uint32_t length, struct nd_power_snapshot **complete) {
  *complete = NULL;
  if (type == NDC_POWER_BEGIN) {
    uint32_t count;
    if (rx->pending || length != sizeof(count))
      goto invalid;
    memcpy(&count, data, sizeof(count));
    count = ntohl(count);
    if (count > ND_POWER_MAX_DEVICES)
      goto invalid;
    rx->pending = calloc(1, sizeof(*rx->pending));
    if (!rx->pending)
      return -1;
    rx->expected_devices = count;
    rx->started_ms = nd_power_now_ms();
    return 0;
  }
  if (!rx->pending || nd_power_now_ms() - rx->started_ms > 5000)
    goto invalid;
  struct nd_power_snapshot *s = rx->pending;
  if (type == NDC_POWER_DEVICE) {
    struct power_device_wire wire;
    if (length != sizeof(wire) || s->count >= rx->expected_devices ||
        rx->received_properties != rx->expected_properties)
      goto invalid;
    memcpy(&wire, data, sizeof(wire));
    if (ntohs(wire.id) != s->count ||
        ntohs(wire.count) > ND_POWER_MAX_PROPERTIES ||
        !valid_name(wire.name, sizeof(wire.name)))
      goto invalid;
    for (uint32_t i = 0; i < s->count; i++)
      if (!strcmp(s->devices[i].name, wire.name))
        goto invalid;
    memcpy(s->devices[s->count++].name, wire.name, sizeof(wire.name));
    rx->expected_properties = ntohs(wire.count);
    rx->received_properties = 0;
    return 0;
  }
  if (type == NDC_POWER_PROPERTY) {
    struct power_property_wire wire;
    if (length != sizeof(wire) || !s->count ||
        rx->received_properties >= rx->expected_properties)
      goto invalid;
    memcpy(&wire, data, sizeof(wire));
    if (ntohs(wire.id) != s->count - 1 || wire.reserved ||
        !valid_name(wire.property.key, sizeof(wire.property.key)))
      goto invalid;
    size_t n = strnlen(wire.property.value, sizeof(wire.property.value));
    if (n == sizeof(wire.property.value))
      goto invalid;
    for (size_t i = 0; i < n; i++)
      if ((unsigned char)wire.property.value[i] < 32 ||
          (unsigned char)wire.property.value[i] == 127)
        goto invalid;
    struct nd_power_device *d = &s->devices[s->count - 1];
    if (nd_power_get(d, wire.property.key))
      goto invalid;
    d->properties[d->count++] = wire.property;
    rx->received_properties++;
    return 0;
  }
  if (type == NDC_POWER_END && !length && s->count == rx->expected_devices &&
      rx->received_properties == rx->expected_properties) {
    for (uint32_t i = 0; i < s->count; i++)
      if (!nd_power_get(&s->devices[i], "type"))
        goto invalid;
    *complete = s;
    memset(rx, 0, sizeof(*rx));
    return 1;
  }
invalid:
  nd_power_rx_clear(rx);
  errno = EPROTO;
  return -1;
}
