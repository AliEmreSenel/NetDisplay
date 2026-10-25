// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "frame_transport.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>

int nd_latest_init(struct nd_latest *m, uint8_t *buffer) {
  memset(m, 0, sizeof(*m));
  m->slot.buffer = buffer;
  int r = pthread_mutex_init(&m->mutex, NULL);
  if (r) return r;
  r = pthread_cond_init(&m->cond, NULL);
  if (r) pthread_mutex_destroy(&m->mutex);
  return r;
}

int nd_latest_publish(struct nd_latest *m, struct nd_frame_slot *frame) {
  pthread_mutex_lock(&m->mutex);
  int replaced = m->ready;
  uint8_t *old = m->slot.buffer;
  m->slot = *frame;
  frame->buffer = old;
  m->ready = 1;
  pthread_cond_signal(&m->cond);
  pthread_mutex_unlock(&m->mutex);
  return replaced;
}

int nd_latest_take(struct nd_latest *m, struct nd_frame_slot *frame, int wait) {
  pthread_mutex_lock(&m->mutex);
  while (wait && !m->ready) pthread_cond_wait(&m->cond, &m->mutex);
  int ready = m->ready;
  if (ready) {
    uint8_t *old = frame->buffer;
    *frame = m->slot;
    m->slot.buffer = old;
    m->ready = 0;
  }
  pthread_mutex_unlock(&m->mutex);
  return ready;
}

void nd_latest_destroy(struct nd_latest *m) {
  pthread_cond_destroy(&m->cond);
  pthread_mutex_destroy(&m->mutex);
}

int nd_frame_send(int fd, uint64_t session, uint32_t seq,
                  const uint8_t *data, size_t size, const uint8_t *key,
                  uint8_t *scratch, int (*obsolete)(void *), void *context,
                  struct nd_tx_stats *stats) {
  if (!size || size > ND_MAX_FRAME || !session) {
    errno = EMSGSIZE;
    return -1;
  }
  if (key) {
    if (!scratch || nd_crypto_encrypt(scratch, &size, data, size, key,
                                      session, seq) < 0)
      return -1;
    data = scratch;
  }
  uint32_t count = (uint32_t)((size + ND_FRAG_DATA - 1) / ND_FRAG_DATA);
  for (uint32_t frag = 0; frag < count;) {
    if (obsolete && obsolete(context)) return 1;
    struct nd_hdr hdr[32];
    struct iovec iov[32][2];
    struct mmsghdr msg[32] = {0};
    uint32_t batch = count - frag;
    if (batch > 32) batch = 32;
    for (uint32_t j = 0; j < batch; j++) {
      size_t off = (size_t)(frag + j) * ND_FRAG_DATA;
      size_t n = size - off;
      if (n > ND_FRAG_DATA) n = ND_FRAG_DATA;
      hdr[j] = (struct nd_hdr){htonl(ND_MAGIC), nd_hton64(session), htonl(seq),
          htons((uint16_t)(frag + j)), htons((uint16_t)count), htonl((uint32_t)size)};
      iov[j][0] = (struct iovec){&hdr[j], sizeof(hdr[j])};
      iov[j][1] = (struct iovec){(void *)(data + off), n};
      msg[j].msg_hdr.msg_iov = iov[j];
      msg[j].msg_hdr.msg_iovlen = 2;
    }
    uint32_t done = 0;
    while (done < batch) {
      if (obsolete && obsolete(context)) return 1;
      int n = sendmmsg(fd, msg + done, batch - done, MSG_DONTWAIT);
      if (n > 0) {
        for (int j = 0; j < n; j++) stats->bytes += msg[done + j].msg_len;
        done += (uint32_t)n;
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        stats->eagain++;
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (poll(&p, 1, 1) < 0 && errno != EINTR) {
          stats->errors++;
          return -1;
        }
      } else {
        stats->errors++;
        return -1;
      }
    }
    frag += batch;
  }
  return 0;
}
