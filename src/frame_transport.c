#define _GNU_SOURCE
#include "frame_transport.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

uint64_t nd_now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

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

int nd_frame_receive(struct nd_assembly *a, struct nd_rx_stats *st,
                     uint8_t *buffer, const void *packet, size_t n,
                     const uint8_t *key) {
  struct nd_hdr h;
  if (n < sizeof(h) || n > ND_UDP_PAYLOAD_MAX) goto bad;
  memcpy(&h, packet, sizeof(h));
  uint64_t session = nd_ntoh64(h.session);
  uint32_t seq = ntohl(h.seq), size = ntohl(h.frame_size);
  uint16_t frag = ntohs(h.frag), count = ntohs(h.frag_count);
  if (ntohl(h.magic) != ND_MAGIC || !session || !size ||
      size > (key ? ND_MAX_WIRE_FRAME : ND_MAX_FRAME) || !count ||
      count != (size + ND_FRAG_DATA - 1) / ND_FRAG_DATA || frag >= count)
    goto bad;
  if (a->expected_session && session != a->expected_session) return 0;
  size_t off = (size_t)frag * ND_FRAG_DATA;
  size_t want = size - off;
  if (want > ND_FRAG_DATA) want = ND_FRAG_DATA;
  if (n != sizeof(h) + want) goto bad;
  if (session == a->previous_session) return 0;
  if (session != a->session || (int32_t)(seq - a->seq) > 0) {
    if (a->active) atomic_fetch_add(&st->partial_drop, 1);
    if (session != a->session) a->previous_session = a->session;
    a->session = session;
    a->seq = seq;
    a->size = size;
    a->fragments = count;
    a->count = 0;
    a->active = 1;
    a->first_ns = nd_now_ns();
    memset(a->got, 0, sizeof(a->got));
  } else if (!a->active || seq != a->seq) {
    return 0;
  } else if (size != a->size || count != a->fragments) goto bad;
  uint64_t mask = 1ull << (frag & 63u);
  if (a->got[frag >> 6] & mask) {
    atomic_fetch_add(&st->dup, 1);
    return 0;
  }
  memcpy(buffer + off, (const uint8_t *)packet + sizeof(h), want);
  a->got[frag >> 6] |= mask;
  if (++a->count != a->fragments) return 0;
  a->active = 0;
  a->complete_ns = nd_now_ns();
  if (key) {
    size_t plain;
    if (nd_crypto_decrypt(buffer, &plain, buffer, a->size, key, session, seq) < 0 ||
        !plain || plain > ND_MAX_FRAME) goto bad;
    a->size = (uint32_t)plain;
  }
  atomic_fetch_add(&st->complete, 1);
  atomic_fetch_add(&st->complete_bytes, a->size);
  atomic_fetch_add(&st->assembly_ns, a->complete_ns - a->first_ns);
  return 1;
bad:
  atomic_fetch_add(&st->bad, 1);
  return 0;
}
