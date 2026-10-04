// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "capture.h"
#include "video_tx.h"
#include "common.h"
#include "crypto.h"
#include "frame_transport.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <poll.h>
#include <sodium.h>

static volatile sig_atomic_t stopping;
static void stop(int sig) {
  (void)sig;
  stopping = 1;
}
static int expected(int fd, uint16_t want, void *p, uint32_t size) {
  uint16_t type = 0;
  uint32_t n = size;
  return ndc_recv_msg(fd, &type, p, &n) == 1 && type == want && n == size ? 0
                                                                          : -1;
}
struct capture_worker {
  struct ndvr_capture *capture;
  pthread_mutex_t mutex;
  uint8_t *pixels;
  size_t size;
  int ready, bgra;
  uint64_t captured_ns, captures, replaced, readback_ns;
};
static void *capture_main(void *arg) {
  struct capture_worker *w = arg;
  while (!stopping) {
    const uint8_t *pixels;
    int bgra;
    int r = ndvr_capture_next(w->capture, &pixels, &bgra, 100);
    if (r < 0) {
      stopping = 1;
      break;
    }
    if (r == 1) {
      pthread_mutex_lock(&w->mutex);
      memcpy(w->pixels, pixels, w->size);
      w->replaced += w->ready;
      w->captures++;
      w->captured_ns = nd_now_ns();
      w->readback_ns = ndvr_capture_readback_ns(w->capture);
      w->bgra = bgra;
      w->ready = 1;
      pthread_mutex_unlock(&w->mutex);
    }
  }
  return NULL;
}
static int number(const char *s, int lo, int hi) {
  char *end;
  long n = strtol(s, &end, 10);
  if (!*s || *end || n < lo || n > hi) {
    fprintf(stderr, "Invalid number: %s\n", s);
    exit(2);
  }
  return (int)n;
}
static AVCodecContext *encoder(int w, int h, int fps, const char *name) {
  const AVCodec *codec = avcodec_find_encoder_by_name(name);
  if (!codec)
    return NULL;
  AVCodecContext *c = avcodec_alloc_context3(codec);
  if (!c)
    return NULL;
  c->width = w;
  c->height = h;
  c->time_base = (AVRational){1, fps};
  c->framerate = (AVRational){fps, 1};
  c->pix_fmt = AV_PIX_FMT_YUV420P;
  c->gop_size = 1;
  c->max_b_frames = 0;
  c->flags |= AV_CODEC_FLAG_LOW_DELAY;
  c->thread_count = 2;
  c->color_range = AVCOL_RANGE_MPEG;
  c->colorspace = AVCOL_SPC_BT709;
  c->color_primaries = AVCOL_PRI_BT709;
  c->color_trc = AVCOL_TRC_BT709;
  AVDictionary *opts = NULL;
  if (strstr(name, "nvenc")) {
    av_dict_set(&opts, "preset", "p1", 0);
    av_dict_set(&opts, "tune", "ull", 0);
    av_dict_set(&opts, "rc", "constqp", 0);
    const char *qp = getenv("NETDISPLAY_VR_QP");
    if (!qp) qp = "28";
    (void)number(qp, 0, 51);
    av_dict_set(&opts, "qp", qp, 0);
    av_dict_set(&opts, "zerolatency", "1", 0);
    // zerolatency affects NVENC reordering, not FFmpeg's output FIFO.
    av_dict_set(&opts, "delay", "0", 0);
    av_dict_set(&opts, "rc-lookahead", "0", 0);
    av_dict_set(&opts, "forced-idr", "1", 0);
    av_dict_set(&opts, "aud", "1", 0);
  } else {
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "crf", "23", 0);
  }
  int r = avcodec_open2(c, codec, &opts);
  AVDictionaryEntry *unused = NULL;
  while ((unused = av_dict_get(opts, "", unused, AV_DICT_IGNORE_SUFFIX)))
    fprintf(stderr, "VR unused encoder option %s=%s\n", unused->key, unused->value);
  av_dict_free(&opts);
  if (r >= 0 && strstr(name, "nvenc")) {
    int64_t delay = -1;
    if (av_opt_get_int(c->priv_data, "delay", 0, &delay) < 0 || delay != 0) {
      fprintf(stderr, "VR refusing buffered NVENC: delay=%lld (expected 0)\n", (long long)delay);
      r = AVERROR(EINVAL);
    } else fprintf(stderr, "VR NVENC verified delay=0, no B frames, no lookahead, all-IDR\n");
  }
  if (r < 0)
    avcodec_free_context(&c);
  return c;
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("netdisplay-vr-server BIND PEER CONTROL_PORT VIDEO_PORT WIDTH HEIGHT "
         "FPS KEY_FILE AUTH_KIND ENCODER IPC_PATH\nAUTH_KIND: key or password "
         "(KEY_FILE contains an already-derived 64-hex key).\nUse the "
         "netdisplay-vr launcher for configuration and SteamVR integration.");
    return 0;
  }
  if (argc != 12) {
    fprintf(stderr, "Use --help for arguments\n");
    return 2;
  }
  if (sodium_init() < 0)
    return 1;
  int port = number(argv[3], 1, 65535), video_port = number(argv[4], 1, 65535),
      w = number(argv[5], 2, 4096), h = number(argv[6], 2, 2160),
      fps = number(argv[7], 30, 120);
  if (w % 2 || h % 2 || (strcmp(argv[9], "key") && strcmp(argv[9], "password")))
    return 2;
  uint8_t key[32];
  if (nd_crypto_load_psk(argv[8], key) < 0) {
    perror("VR video key");
    return 1;
  }
  struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(port)},
                     peer = {.sin_family = AF_INET,
                             .sin_port = htons(video_port)};
  if (inet_pton(AF_INET, argv[1], &local.sin_addr) != 1 ||
      inet_pton(AF_INET, argv[2], &peer.sin_addr) != 1)
    return 2;
  struct sigaction sa = {.sa_handler = stop};
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
  if (listener < 0)
    die("VR listener");
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(listener, (struct sockaddr *)&local, sizeof(local)) < 0 ||
      listen(listener, 1) < 0)
    die("VR control bind");
  AVCodecContext *enc = encoder(w, h, fps, argv[10]);
  if (!enc) {
    fprintf(stderr, "VR encoder %s unavailable\n", argv[10]);
    close(listener);
    return 1;
  }
  if (enc->codec_id != AV_CODEC_ID_H264 && enc->codec_id != AV_CODEC_ID_HEVC) {
    fprintf(stderr, "VR encoder must be H.264 or HEVC\n");
    return 2;
  }
  unsigned codec =
      enc->codec_id == AV_CODEC_ID_HEVC ? NDC_VIDEO_HEVC : NDC_VIDEO_H264;
  AVFrame *frame = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  struct SwsContext *sws = NULL;
  if (!frame || !packet)
    return 1;
  frame->width = w;
  frame->height = h;
  frame->format = enc->pix_fmt;
  if (av_frame_get_buffer(frame, 32) < 0)
    return 1;
  struct ndvr_capture *capture = ndvr_capture_open(argv[11], w, h);
  if (!capture)
    return 1;
  fprintf(stderr,
          "netdisplay-vr-server listening %s:%d, peer %s, stereo %dx%d@%d, "
          "encoder %s; encrypted video UDP %d\n",
          argv[1], port, argv[2], w, h, fps, argv[10], video_port);
  struct capture_worker worker = {.capture = capture,
                                  .mutex = PTHREAD_MUTEX_INITIALIZER,
                                  .size = (size_t)w * h * 4};
  worker.pixels = malloc(worker.size);
  uint8_t *raw = malloc(worker.size);
  pthread_t thread;
  if (!worker.pixels || !raw ||
      pthread_create(&thread, NULL, capture_main, &worker))
    return 1;
  uint64_t frames = 0;
  while (!stopping) {
    struct pollfd ready = {.fd = listener, .events = POLLIN};
    if (poll(&ready, 1, 200) <= 0)
      continue;
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    int c = accept4(listener, (struct sockaddr *)&from, &len, SOCK_CLOEXEC);
    if (c < 0)
      continue;
    int udp = -1;
    struct ndvr_tx *tx = NULL;
    uint8_t stream_key[32] = {0};
    if (from.sin_addr.s_addr != peer.sin_addr.s_addr) {
      close(c);
      continue;
    }
    ndc_set_tcp_opts(c);
    struct timeval timeout = {.tv_sec = 5};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct ndc_hello hello;
    if (expected(c, NDC_HELLO, &hello, sizeof(hello)) < 0)
      goto done;
    uint32_t flags = ntohl(hello.flags),
             mask = codec == NDC_VIDEO_HEVC ? NDC_CODEC_HEVC : NDC_CODEC_H264;
    if (ntohs(hello.display_count) != 1 ||
        !(ntohl(hello.video_codecs) & mask) ||
        !(flags & NDC_FLAG_FRAME_ENCRYPT) || !(flags & NDC_FLAG_AUTH_REQUIRED))
      goto reject;
    uint32_t selected = NDC_FLAG_AUTH_REQUIRED | NDC_FLAG_FRAME_ENCRYPT |
                        (!strcmp(argv[9], "password") ? NDC_FLAG_PASSWORD : 0);
    struct ndc_challenge challenge = {.flags = htonl(selected)};
    nd_crypto_random(challenge.nonce, 16);
    if (ndc_send_msg(c, NDC_CHALLENGE, &challenge, sizeof(challenge)) < 0)
      goto done;
    struct ndc_auth auth;
    uint8_t proof[32];
    if (expected(c, NDC_AUTH, &auth, sizeof(auth)) < 0)
      goto done;
    nd_crypto_proof(proof, key, "client", hello.nonce, challenge.nonce);
    if (!nd_crypto_verify(proof, auth.proof))
      goto reject;
    nd_crypto_proof(auth.proof, key, "server", hello.nonce, challenge.nonce);
    if (ndc_send_msg(c, NDC_AUTH, &auth, sizeof(auth)) < 0)
      goto done;
    struct ndc_display display;
    if (expected(c, NDC_DISPLAY, &display, sizeof(display)) < 0)
      goto done;
    if (!ntohl(display.display_id) || ntohs(display.width) != w ||
        ntohs(display.height) != h || ntohs(display.refresh_hz) != fps) {
      fprintf(stderr, "VR phone mode must match %dx%d@%d\n", w, h, fps);
      goto reject;
    }
    uint64_t session;
    nd_crypto_random(&session, sizeof(session));
    if (!session)
      session = 1;
    nd_crypto_stream_key(stream_key, key, session, hello.nonce,
                         challenge.nonce);
    struct ndc_welcome welcome = {.flags = htonl(selected),
                                  .display_count = htons(1),
                                  .video_codec = htons(codec)};
    struct ndc_stream stream = {.display_id = display.display_id,
                                .stream_id = htobe64(session),
                                .video_port = htons(video_port),
                                .width = htons(w),
                                .height = htons(h),
                                .refresh_hz = htons(fps)};
    strcpy(stream.output, "netdisplay-vr-stereo");
    if (ndc_send_msg(c, NDC_WELCOME, &welcome, sizeof(welcome)) < 0 ||
        ndc_send_msg(c, NDC_STREAM, &stream, sizeof(stream)) < 0)
      goto done;
    struct ndc_stream_ready ack;
    if (expected(c, NDC_STREAM_READY, &ack, sizeof(ack)) < 0 ||
        ack.display_id != display.display_id)
      goto done;
    udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (udp < 0 || connect(udp, (struct sockaddr *)&peer, sizeof(peer)) < 0)
      goto done;
    int sndbuf = 128 * 1024;
    setsockopt(udp, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    socklen_t sndlen = sizeof(sndbuf);
    if (getsockopt(udp, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sndlen) == 0)
      fprintf(stderr, "VR UDP SO_SNDBUF actual=%d bytes (kernel accounting, not measured latency)\n", sndbuf);
    tx = ndvr_tx_open(udp, session, stream_key, (unsigned)fps);
    if (!tx) goto done;
    uint32_t seq = 0;
    uint64_t last_frames = frames, last_bytes = 0;
    double last_convert_ms = 0, last_encode_ms = 0, last_mailbox_ms = 0;
    uint64_t last_readback_ns = 0;
    uint64_t last_control = nd_now_ns(), last_report = last_control;
    fprintf(
        stderr,
        "VR phone authenticated and ready; waiting for compositor frames\n");
    while (!stopping) {
      struct pollfd cp = {.fd = c, .events = POLLIN};
      int n = poll(&cp, 1, 0);
      if (n < 0 && errno != EINTR)
        break;
      if (cp.revents & (POLLERR | POLLHUP | POLLNVAL))
        break;
      if (cp.revents & POLLIN) {
        uint8_t payload[NDC_MAX_PAYLOAD];
        uint16_t type;
        uint32_t bytes = sizeof(payload);
        if (ndc_recv_msg(c, &type, payload, &bytes) != 1)
          break;
        if (type == NDC_PING && !bytes) {
          if (ndc_send_msg(c, NDC_PONG, NULL, 0) < 0)
            break;
          last_control = nd_now_ns();
        } else if (type == NDC_STOP)
          break;
        else if (type != NDC_PONG || bytes)
          break;
      }
      if (nd_now_ns() - last_control > 15000000000ULL)
        break;
      int bgra = 0, have_frame = 0;
      pthread_mutex_lock(&worker.mutex);
      if (worker.ready) {
        // Swap buffers instead of copying a second full RGBA image.
        uint8_t *old = raw; raw = worker.pixels; worker.pixels = old;
        last_mailbox_ms = (nd_now_ns() - worker.captured_ns) / 1e6;
        last_readback_ns = worker.readback_ns;
        bgra = worker.bgra;
        worker.ready = 0;
        have_frame = 1;
      }
      pthread_mutex_unlock(&worker.mutex);
      if (!have_frame) {
        poll(NULL, 0, 2);
        continue;
      }
      uint64_t convert_start = nd_now_ns();
      const uint8_t *pixels = raw;
      if (av_frame_make_writable(frame) < 0) {
        stopping = 1;
        break;
      }
      sws = sws_getCachedContext(
          sws, w, h, bgra ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA, w, h,
          enc->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);
      if (!sws) {
        stopping = 1;
        break;
      }
      const uint8_t *planes[] = {pixels};
      int strides[] = {w * 4};
      sws_scale(sws, planes, strides, 0, h, frame->data, frame->linesize);
      last_convert_ms = (nd_now_ns() - convert_start) / 1e6;
      frame->pts = frames++;
      frame->pict_type = AV_PICTURE_TYPE_I;
      uint64_t encode_start = nd_now_ns();
      if (avcodec_send_frame(enc, frame) < 0) {
        stopping = 1;
        break;
      }
      while (avcodec_receive_packet(enc, packet) == 0) {
        if (seq == UINT32_MAX) {
          av_packet_unref(packet);
          goto done;
        }
        last_encode_ms = (nd_now_ns() - encode_start) / 1e6;
        ndvr_tx_publish(tx, packet, seq++);
      }
      if (nd_now_ns() - last_report > 5000000000ULL) {
        uint64_t now = nd_now_ns();
        double seconds = (now - last_report) / 1e9;
        struct ndvr_tx_stats st = ndvr_tx_snapshot(tx);
        fprintf(stderr, "NDVR video encode=%.1f fps tx=%.1f Mbit/s readback=%.2f mailbox=%.2f convert=%.2f encode=%.2f send=%.2f ms complete=%llu replaced=%llu aborted=%llu eagain=%llu errors=%llu\n",
          (frames-last_frames)/seconds, (st.bytes-last_bytes)*8.0/seconds/1e6,
          last_readback_ns/1e6, last_mailbox_ms, last_convert_ms, last_encode_ms, st.last_send_ms,
          (unsigned long long)st.complete, (unsigned long long)st.replaced,
          (unsigned long long)st.aborted, (unsigned long long)st.eagain, (unsigned long long)st.errors);
        last_report = now; last_frames = frames; last_bytes = st.bytes;
      }
    }
    goto done;
  reject:
    (void)ndc_send_msg(c, NDC_REJECT, NULL, 0);
    fprintf(stderr, "VR connection rejected: authentication, encryption, codec "
                    "or mode mismatch\n");
  done:
    ndvr_tx_close(tx);
    sodium_memzero(stream_key, sizeof(stream_key));
    if (udp >= 0)
      close(udp);
    close(c);
    fprintf(stderr, "VR phone disconnected\n");
  }
  stopping = 1;
  pthread_join(thread, NULL);
  free(worker.pixels);
  free(raw);
  pthread_mutex_destroy(&worker.mutex);
  ndvr_capture_close(capture);
  sws_freeContext(sws);
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&enc);
  sodium_memzero(key, sizeof(key));
  close(listener);
  return 0;
}
