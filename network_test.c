#define _GNU_SOURCE
#include "common.h"
#include "frame_transport.h"
#include "network_test.h"
#include <netinet/ip.h>
#include <poll.h>
#include <sodium.h>

#define STAGES 5u
#define SAMPLES 120u
#define WINDOW_NS 500000000ull
#define GRACE_NS 150000000ull
struct __attribute__((packed)) probe_ack {
  uint64_t sent_ns, assembly_ns, service_ns;
  uint8_t hash[32];
};
struct sample {
  uint64_t sent_ns, token;
  uint8_t hash[32];
  int acknowledged;
};
struct probe {
  int fd, control, fps;
  uint64_t session;
  uint8_t keys[4][ND_KEY_SIZE];
  int encrypted;
  uint8_t *data, *scratch, *receive, *latest;
};

static int recv_message(int fd, uint16_t *type, void *payload, uint32_t *size) {
  /* Bound partial TCP messages as well as an entirely silent peer. */
  struct timeval previous, timeout = {.tv_sec = 10};
  socklen_t n = sizeof(previous);
  if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, &n) < 0) return -1;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    return -1;
  int r = ndc_recv_msg(fd, type, payload, size);
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, sizeof(previous));
  return r > 0 ? 0 : -1;
}

static int recv_control(int fd, uint16_t expected, void *payload, uint32_t size) {
  uint16_t type;
  uint32_t length = size;
  return recv_message(fd, &type, payload, &length) == 0 &&
         type == expected && length == size ? 0 : -1;
}

static int expired(void *opaque) { return nd_now_ns() >= *(uint64_t *)opaque; }
static const uint8_t *key_for(struct probe *p, unsigned index) {
  return p->encrypted ? p->keys[index] : NULL;
}
static int compare_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static int send_probe(struct probe *p, unsigned direction) {
  struct nd_assembly assembly = {.expected_session = p->session};
  struct nd_rx_stats rx = {0};
  for (unsigned stage = 0; stage < STAGES; stage++) {
    size_t size = 4096u << (stage * 2u);
    struct sample samples[SAMPLES] = {0};
    double rtt[SAMPLES], network[SAMPLES], receive[SAMPLES];
    unsigned sent = 0, verified = 0, corrupt = 0, aborted = 0, skipped = 0;
    struct nd_tx_stats tx = {0};
    nd_crypto_random(p->data, size);
    uint64_t start = nd_now_ns(), end = start + WINDOW_NS;
    uint64_t next = start, period = 1000000000ull / (unsigned)p->fps;
    uint64_t last_ack = end;
    while (nd_now_ns() < end + GRACE_NS) {
      uint64_t now = nd_now_ns();
      if (now < end && now >= next && sent < SAMPLES) {
        /* Do not queue missed production slots or retry an obsolete frame. */
        uint64_t missed = (now - next) / period;
        skipped += (unsigned)missed;
        next += (missed + 1) * period;
        struct sample *s = &samples[sent];
        uint64_t stamp = nd_hton64(now);
        memcpy(p->data, &stamp, sizeof(stamp));
        s->token = now;
        crypto_generichash(s->hash, sizeof(s->hash), p->data, size, NULL, 0);
        uint32_t seq = stage * SAMPLES + sent++;
        uint64_t deadline = next < end ? next : end;
        s->sent_ns = nd_now_ns();
        int r = nd_frame_send(p->fd, p->session, seq, p->data, size,
                              key_for(p, direction * 2), p->scratch,
                              expired, &deadline, &tx);
        if (r != 0) aborted++;
      }
      /* Bound draining so a peer cannot prevent the ramp/deadline advancing. */
      for (unsigned drain = 0; drain < 256; drain++) {
        uint8_t packet[ND_UDP_PAYLOAD_MAX];
        ssize_t n = recv(p->fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0) break;
        if (!nd_frame_receive(&assembly, &rx, p->receive, packet, (size_t)n,
                               key_for(p, direction * 2 + 1))) continue;
        if (assembly.size != sizeof(struct probe_ack) ||
            assembly.seq / SAMPLES != stage) continue;
        unsigned index = assembly.seq % SAMPLES;
        if (index >= sent || samples[index].acknowledged) continue;
        struct probe_ack ack;
        memcpy(&ack, p->receive, sizeof(ack));
        struct sample *s = &samples[index];
        uint64_t elapsed = assembly.complete_ns - s->sent_ns;
        uint64_t service = nd_ntoh64(ack.service_ns);
        if (nd_ntoh64(ack.sent_ns) != s->token || service > elapsed ||
            sodium_memcmp(s->hash, ack.hash, sizeof(ack.hash))) {
          corrupt++;
          continue;
        }
        s->acknowledged = 1;
        rtt[verified] = elapsed / 1e6;
        network[verified] = (elapsed - service) / 1e6;
        receive[verified++] = nd_ntoh64(ack.assembly_ns) / 1e6;
        if (assembly.complete_ns > last_ack) last_ack = assembly.complete_ns;
      }
      struct pollfd fds[2] = {{.fd = p->fd, .events = POLLIN},
                             {.fd = p->control, .events = POLLIN}};
      int r = poll(fds, 2, 1);
      if ((r < 0 && errno != EINTR) || fds[1].revents) return -1;
    }
    double seconds = (last_ack - start) / 1e9;
    char report[NDC_MAX_PAYLOAD];
    int used = snprintf(report, sizeof(report), "network-test %s frame=%zu offered=%.2f verified=%.2f "
            "UDP=%.2f Mbit/s complete=%u/%u lost-or-late=%u "
            "aborted=%u producer-skipped=%u corrupt=%u send-errors=%llu",
            direction ? "video-downlink" : "uplink", size,
            sent * size * 8.0 / (WINDOW_NS / 1e9) / 1e6,
            verified * size * 8.0 / seconds / 1e6,
            tx.bytes * 8.0 / seconds / 1e6,
            verified, sent, sent - verified, aborted, skipped, corrupt,
            (unsigned long long)tx.errors);
    if (verified) {
      qsort(rtt, verified, sizeof(double), compare_double);
      qsort(network, verified, sizeof(double), compare_double);
      qsort(receive, verified, sizeof(double), compare_double);
      unsigned p95 = (verified * 95u + 99u) / 100u - 1u;
      if (used < 0 || (size_t)used >= sizeof(report)) return -1;
      snprintf(report + used, sizeof(report) - (size_t)used,
              " RTT-p50/p95=%.3f/%.3f ms transport-RTT-p50/p95/max="
              "%.3f/%.3f/%.3f ms variation-p95-p50=%.3f ms "
              "receive-assembly-p95=%.3f ms\n", rtt[verified / 2], rtt[p95],
              network[verified / 2], network[p95], network[verified - 1],
              network[p95] - network[verified / 2], receive[p95]);
    } else {
      if (used < 0 || (size_t)used >= sizeof(report)) return -1;
      snprintf(report + used, sizeof(report) - (size_t)used, " latency=unavailable\n");
    }
    fputs(report, stderr);
    if (ndc_send_msg(p->control, NDC_TEST_REPORT, report,
                     (uint32_t)strlen(report) + 1) < 0) return -1;
  }
  if (ndc_send_msg(p->control, NDC_TEST_DONE, NULL, 0) < 0) return -1;
  return recv_control(p->control, NDC_TEST_DONE, NULL, 0);
}

struct probe_receiver {
  struct probe *probe;
  unsigned direction;
  struct nd_latest mailbox;
  struct nd_frame_slot assembly;
  struct nd_rx_stats stats;
  _Atomic int stop;
};

static void *probe_receive_main(void *opaque) {
  struct probe_receiver *rx = opaque;
  struct probe *p = rx->probe;
  struct nd_assembly a = {.expected_session = p->session};
  while (!atomic_load(&rx->stop)) {
    struct pollfd fd = {.fd = p->fd, .events = POLLIN};
    if (poll(&fd, 1, 10) <= 0) continue;
    uint8_t packet[ND_UDP_PAYLOAD_MAX];
    ssize_t n = recv(p->fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
    if (n < 0) continue;
    if (!nd_frame_receive(&a, &rx->stats, rx->assembly.buffer, packet, (size_t)n,
                           key_for(p, rx->direction * 2))) continue;
    rx->assembly.size = a.size;
    rx->assembly.seq = a.seq;
    rx->assembly.session = a.session;
    rx->assembly.first_ns = a.first_ns;
    rx->assembly.complete_ns = a.complete_ns;
    if (nd_latest_publish(&rx->mailbox, &rx->assembly))
      atomic_fetch_add(&rx->stats.latest_overwrite, 1);
  }
  return NULL;
}

static int hash_probe(struct probe *p, unsigned direction) {
  uint64_t started = nd_now_ns();
  struct probe_receiver rx = {.probe = p, .direction = direction,
                              .assembly = {.buffer = p->receive}};
  int error = nd_latest_init(&rx.mailbox, p->latest);
  if (error) { errno = error; return -1; }
  pthread_t thread;
  error = pthread_create(&thread, NULL, probe_receive_main, &rx);
  if (error) {
    nd_latest_destroy(&rx.mailbox);
    errno = error;
    return -1;
  }
  struct nd_frame_slot frame = {.buffer = p->data};
  uint64_t deadline = nd_now_ns() + 10000000000ull;
  int result = -1;
  while (nd_now_ns() < deadline) {
    struct pollfd control = {.fd = p->control, .events = POLLIN};
    int r = poll(&control, 1, 1);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) break;
    if (control.revents) {
      char report[NDC_MAX_PAYLOAD];
      uint32_t length = sizeof(report);
      uint16_t type;
      if (recv_message(p->control, &type, report, &length) < 0) break;
      if (type == NDC_TEST_REPORT && length && !report[length - 1]) {
        /* Reports are informational only; never use peer text as a format. */
        int valid = 1;
        for (uint32_t i = 0; i + 1 < length; i++)
          if (!isprint((unsigned char)report[i]) && report[i] != '\n') valid = 0;
        if (!valid) break;
        fprintf(stderr, "peer %s", report);
        continue;
      }
      if (type == NDC_TEST_DONE && !length) result = 0;
      break;
    }
    if (!nd_latest_take(&rx.mailbox, &frame, 0) || frame.size < sizeof(uint64_t))
      continue;
    struct probe_ack ack;
    memcpy(&ack.sent_ns, frame.buffer, sizeof(ack.sent_ns));
    ack.assembly_ns = nd_hton64(frame.complete_ns - frame.first_ns);
    crypto_generichash(ack.hash, sizeof(ack.hash), frame.buffer, frame.size, NULL, 0);
    ack.service_ns = nd_hton64(nd_now_ns() - frame.complete_ns);
    uint64_t ack_deadline = nd_now_ns() + 1000000ull;
    struct nd_tx_stats tx = {0};
    (void)nd_frame_send(p->fd, p->session, frame.seq, (uint8_t *)&ack, sizeof(ack),
                        key_for(p, direction * 2 + 1), p->scratch,
                        expired, &ack_deadline, &tx);
  }
  atomic_store(&rx.stop, 1);
  pthread_join(thread, NULL);
  /* Restore the three distinct buffer owners after all pointer swaps. */
  p->receive = rx.assembly.buffer;
  p->latest = rx.mailbox.slot.buffer;
  p->data = frame.buffer;
  nd_latest_destroy(&rx.mailbox);
  if (result == 0) {
    uint64_t bytes = atomic_load(&rx.stats.complete_bytes);
    fprintf(stderr, "network-test %s received=%llu frames partial-dropped=%llu "
            "latest-dropped=%llu invalid=%llu useful=%.2f Mbit/s\n",
            direction ? "video-downlink" : "uplink",
            (unsigned long long)atomic_load(&rx.stats.complete),
            (unsigned long long)atomic_load(&rx.stats.partial_drop),
            (unsigned long long)atomic_load(&rx.stats.latest_overwrite),
            (unsigned long long)atomic_load(&rx.stats.bad),
            bytes * 8.0 / ((nd_now_ns() - started) / 1e9) / 1e6);
    result = ndc_send_msg(p->control, NDC_TEST_DONE, NULL, 0);
  }
  return result;
}

int nd_network_test(int control, int server, const char *peer, int video_port,
                     const char *interface_name, int fps, uint64_t session,
                     const uint8_t *key) {
  struct probe p = {.fd = -1, .control = control, .session = session,
                    .fps = fps > 240 ? 240 : fps, .encrypted = key != NULL};
  int result = -1;
  if (p.fps < 1 || !session || video_port < 1 || video_port > 65535) return -1;
  /* Keep delayed probe packets out of the subsequent video sequence space,
   * including when frame encryption is disabled. */
  p.session = session ^ 0x4e4450524f424531ull;
  if (!p.session) p.session = 1;
  p.fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (p.fd < 0) return -1;
  int buffer = 256 * 1024, tos = IPTOS_LOWDELAY;
  if (setsockopt(p.fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) < 0 ||
      setsockopt(p.fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) < 0)
    goto done;
  (void)setsockopt(p.fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#ifdef IP_MTU_DISCOVER
  int pmtu = IP_PMTUDISC_DO;
  (void)setsockopt(p.fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof(pmtu));
#endif
  if (interface_name && *interface_name &&
      setsockopt(p.fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name,
                 strlen(interface_name) + 1) < 0) goto done;
  struct sockaddr_in local = {.sin_family = AF_INET,
      .sin_port = server ? 0 : htons((uint16_t)video_port),
      .sin_addr.s_addr = htonl(INADDR_ANY)};
  if (bind(p.fd, (struct sockaddr *)&local, sizeof(local)) < 0) goto done;
  socklen_t length = sizeof(local);
  if (getsockname(p.fd, (struct sockaddr *)&local, &length) < 0) goto done;
  uint16_t port = local.sin_port;
  if (server) {
    if (ndc_send_msg(control, NDC_TEST_PORT, &port, sizeof(port)) < 0) goto done;
    port = htons((uint16_t)video_port);
  } else if (recv_control(control, NDC_TEST_PORT, &port, sizeof(port)) < 0 || !port)
    goto done;
  struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = port};
  if (inet_pton(AF_INET, peer, &remote.sin_addr) != 1 ||
      connect(p.fd, (struct sockaddr *)&remote, sizeof(remote)) < 0) goto done;
  p.data = malloc(ND_MAX_WIRE_FRAME);
  p.scratch = malloc(ND_MAX_WIRE_FRAME);
  p.receive = malloc(ND_MAX_WIRE_FRAME);
  p.latest = malloc(ND_MAX_WIRE_FRAME);
  if (!p.data || !p.scratch || !p.receive || !p.latest) goto done;
  if (key) {
    /* Independent keys for each direction and ACK/data, also separate from
     * video: sequence numbers can restart without reusing an AEAD nonce. */
    for (unsigned i = 0; i < 4; i++) {
      uint8_t label[] = "netdisplay-probe-v1-0";
      label[sizeof(label) - 2] += (uint8_t)i;
      crypto_generichash(p.keys[i], ND_KEY_SIZE, label, sizeof(label), key, ND_KEY_SIZE);
    }
  }
  if (server) {
    if (recv_control(control, NDC_TEST_READY, NULL, 0) < 0 ||
        ndc_send_msg(control, NDC_TEST_READY, NULL, 0) < 0) goto done;
    if (hash_probe(&p, 0) < 0 || send_probe(&p, 1) < 0) goto done;
  } else {
    if (ndc_send_msg(control, NDC_TEST_READY, NULL, 0) < 0 ||
        recv_control(control, NDC_TEST_READY, NULL, 0) < 0) goto done;
    if (send_probe(&p, 0) < 0 || hash_probe(&p, 1) < 0) goto done;
  }
  result = 0;
done:
  sodium_memzero(p.keys, sizeof(p.keys));
  free(p.data);
  free(p.scratch);
  free(p.receive);
  free(p.latest);
  close(p.fd);
  return result;
}
