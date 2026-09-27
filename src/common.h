#ifndef NETDISPLAY_CONTROL_COMMON_H
#define NETDISPLAY_CONTROL_COMMON_H

#define _GNU_SOURCE

#ifndef NETDISPLAY_VERSION
#define NETDISPLAY_VERSION "dev"
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "control_proto.h"

static inline void die(const char *s) {
  perror(s);
  exit(EXIT_FAILURE);
}

static inline char *ndc_trim(char *s) {
  while (*s && isspace((unsigned char)*s))
    s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = 0;
  return s;
}

static inline int ndc_read_full(int fd, void *buf, size_t len) {
  uint8_t *p = buf;
  while (len) {
    ssize_t n = read(fd, p, len);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n == 0)
      return 0;
    if (errno == EINTR)
      continue;
    return -1;
  }
  return 1;
}

static inline int ndc_write_full(int fd, const void *buf, size_t len) {
  const uint8_t *p = buf;
  while (len) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    return -1;
  }
  return 0;
}

static inline int ndc_send_msg(int fd, uint16_t type, const void *payload,
                               uint32_t len) {
  if (len > NDC_MAX_PAYLOAD) {
    errno = EMSGSIZE;
    return -1;
  }
  struct ndc_hdr h = {
      .magic = htonl(NDC_MAGIC),
      .version = htons(NDC_VERSION),
      .type = htons(type),
      .length = htonl(len),
  };
  if (ndc_write_full(fd, &h, sizeof(h)) < 0)
    return -1;
  if (len && ndc_write_full(fd, payload, len) < 0)
    return -1;
  return 0;
}

static inline int ndc_recv_msg(int fd, uint16_t *type, void *payload,
                               uint32_t *len_io) {
  struct ndc_hdr h;
  int r = ndc_read_full(fd, &h, sizeof(h));
  if (r <= 0)
    return r;
  if (ntohl(h.magic) != NDC_MAGIC || ntohs(h.version) != NDC_VERSION) {
    errno = EPROTO;
    return -1;
  }
  uint32_t len = ntohl(h.length);
  if (len > NDC_MAX_PAYLOAD || len > *len_io) {
    errno = EMSGSIZE;
    return -1;
  }
  *type = ntohs(h.type);
  *len_io = len;
  if (len)
    return ndc_read_full(fd, payload, len);
  return 1;
}

static inline void ndc_set_tcp_opts(int fd) {
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
  int idle = 2, intvl = 1, cnt = 3;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

static inline int ndc_run_sync(const char *cmd) {
  if (!cmd || !*cmd)
    return 0;
  pid_t p = fork();
  if (p < 0)
    return -1;
  if (p == 0) {
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
  int st;
  while (waitpid(p, &st, 0) < 0)
    if (errno != EINTR)
      return -1;
  if (WIFEXITED(st))
    return WEXITSTATUS(st);
  return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static inline pid_t ndc_run_async(const char *cmd) {
  if (!cmd || !*cmd)
    return -1;
  pid_t p = fork();
  if (p < 0)
    return -1;
  if (p == 0) {
    setpgid(0, 0);
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
  setpgid(p, p);
  return p;
}

static inline void ndc_stop_child(pid_t *pid) {
  if (!pid || *pid <= 0)
    return;
  pid_t p = *pid;
  kill(-p, SIGTERM);
  for (int i = 0; i < 20; i++) {
    int st;
    pid_t r = waitpid(p, &st, WNOHANG);
    if (r == p || (r < 0 && errno == ECHILD)) {
      *pid = -1;
      return;
    }
    usleep(50000);
  }
  kill(-p, SIGKILL);
  while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {
  }
  *pid = -1;
}

#endif
