// SPDX-License-Identifier: GPL-2.0-only
#define main netdisplay_client_main
#include "../src/client.c"
#undef main

int main(void) {
  struct client_cfg cfg;
  load_cfg(NULL, &cfg, 0);
  if (client_hello_flags(&cfg) & NDC_FLAG_NETWORK_TEST)
    return 1;

  char path[] = "/tmp/netdisplay-client-config-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return 1;
  FILE *f = fdopen(fd, "w");
  if (!f) {
    close(fd);
    unlink(path);
    return 1;
  }
  fputs("network_test=1\n", f);
  if (fclose(f)) {
    unlink(path);
    return 1;
  }
  load_cfg(path, &cfg, 1);
  if (!(client_hello_flags(&cfg) & NDC_FLAG_NETWORK_TEST)) {
    unlink(path);
    return 1;
  }
  f = fopen(path, "w");
  if (!f) {
    unlink(path);
    return 1;
  }
  fputs("network_test=0\n", f);
  if (fclose(f)) {
    unlink(path);
    return 1;
  }
  load_cfg(path, &cfg, 1);
  unlink(path);
  return !!(client_hello_flags(&cfg) & NDC_FLAG_NETWORK_TEST);
}
