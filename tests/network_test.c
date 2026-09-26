#define _GNU_SOURCE
#include "common.h"
#include "network_test.h"
#include <sodium.h>

int main(void) {
  if (sodium_init() < 0) return 1;
  alarm(25);
  for (int encrypted = 0; encrypted <= 1; encrypted++) {
    int reserve = socket(AF_INET, SOCK_DGRAM, 0), control[2];
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    if (reserve < 0 || bind(reserve, (struct sockaddr *)&address, size) < 0 ||
        getsockname(reserve, (struct sockaddr *)&address, &size) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, control) < 0) return 2;
    close(reserve);
    uint8_t key[32];
    randombytes_buf(key, sizeof(key));
    FILE *log = tmpfile();
    int saved_stderr = dup(STDERR_FILENO);
    if (!log || saved_stderr < 0 || dup2(fileno(log), STDERR_FILENO) < 0) return 6;
    pid_t child = fork();
    if (child < 0) return 3;
    if (!child) {
      close(control[0]);
      int r = nd_network_test(control[1], 1, "127.0.0.1", ntohs(address.sin_port),
                              NULL, 60, 123, encrypted ? key : NULL);
      close(control[1]);
      _exit(r ? 4 : 0);
    }
    close(control[1]);
    int r = nd_network_test(control[0], 0, "127.0.0.1", ntohs(address.sin_port),
                            NULL, 60, 123, encrypted ? key : NULL);
    close(control[0]);
    int status;
    int failed = waitpid(child, &status, 0) != child || r ||
                 !WIFEXITED(status) || WEXITSTATUS(status);
    fflush(stderr);
    if (dup2(saved_stderr, STDERR_FILENO) < 0) return 6;
    close(saved_stderr);
    rewind(log);
    char line[1024];
    unsigned reports = 0, small_verified = 0;
    while (fgets(line, sizeof(line), log)) {
      if (strncmp(line, "network-test ", 13) || !strstr(line, " frame=")) continue;
      reports++;
      if (!strstr(line, "corrupt=0 ")) failed = 1;
      if (strstr(line, " frame=4096 ")) {
        unsigned verified = 0, sent = 0;
        char *count = strstr(line, "complete=");
        if (count && sscanf(count + 9, "%u/%u", &verified, &sent) == 2 &&
            verified && verified <= sent && strstr(line, "transport-RTT-p50/p95/max="))
          small_verified++;
      }
    }
    if (reports != 10 || small_verified != 2) failed = 1;
    if (failed) {
      rewind(log);
      while (fgets(line, sizeof(line), log)) fputs(line, stderr);
    }
    fclose(log);
    if (failed) return 5;
  }
  return 0;
}
