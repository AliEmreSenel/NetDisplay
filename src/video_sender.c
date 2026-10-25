// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "common.h"
#include "crypto.h"
#include "proto.h"
#include "screencopy.h"
#include "video_sender.h"
#include "video_tx.h"
#include "encoder.h"

struct output {
  struct wl_output *wl;
  char name[128];
  struct output *next;
};

static struct wl_display *display;
static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *sc_mgr;
static struct output *outputs;
static struct output *target;

static struct wl_buffer *cap_buffer;
static uint8_t *cap_map;
static size_t cap_size;
static uint32_t frame_format, frame_width, frame_height, frame_stride;
static uint32_t buf_format, buf_width, buf_height, buf_stride;
static int frame_y_invert;
static uint8_t *flip_map;

static AVCodecContext *enc;
static AVFrame *enc_frame;
static AVFrame *convert_frame;
static AVPacket *enc_pkt;
static struct nd_encoder encoder;
static int encoder_uses_hw_frames;
static enum AVPixelFormat capture_format = AV_PIX_FMT_NONE;
static enum AVPixelFormat encoder_sw_format = AV_PIX_FMT_NONE;
static struct SwsContext *sws;
static char selected_encoder[64];
static uint16_t selected_codec;
static uint64_t frame_pts;

static int udp_fd = -1;
static uint64_t session_id;
static uint32_t seq_id;
static struct nd_encoder_rate encoder_rate;
static uint32_t stream_width;
static uint32_t stream_height;
static uint32_t stream_fps;
static int frame_encrypted;
static uint8_t frame_key[ND_KEY_SIZE];
static struct nd_video_tx *tx;

static unsigned long long stat_capture;
static unsigned long long stat_encoded;
static struct timespec stat_last;

static void ff_die(const char *what, int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(err, buf, sizeof(buf));
  fprintf(stderr, "%s: %s\n", what, buf);
  exit(EXIT_FAILURE);
}

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t subpixel,
                         const char *make, const char *model,
                         int32_t transform) {
  (void)data;
  (void)o;
  (void)x;
  (void)y;
  (void)pw;
  (void)ph;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;
}
static void out_mode(void *data, struct wl_output *o, uint32_t flags,
                     int32_t width, int32_t height, int32_t refresh) {
  (void)data;
  (void)o;
  (void)flags;
  (void)width;
  (void)height;
  (void)refresh;
}
static void out_done(void *data, struct wl_output *o) {
  (void)data;
  (void)o;
}
static void out_scale(void *data, struct wl_output *o, int32_t scale) {
  (void)data;
  (void)o;
  (void)scale;
}
static void out_name(void *data, struct wl_output *o, const char *name) {
  (void)o;
  struct output *out = data;
  snprintf(out->name, sizeof(out->name), "%s", name);
}
static void out_description(void *data, struct wl_output *o,
                            const char *description) {
  (void)data;
  (void)o;
  (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_description,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
  (void)data;
  if (!strcmp(iface, wl_shm_interface.name)) {
    shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  } else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
    uint32_t v = version < 3 ? version : 3;
    if (v < 2) {
      fprintf(stderr, "zwlr_screencopy_manager_v1 >= v2 required\n");
      exit(EXIT_FAILURE);
    }
    sc_mgr =
        wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, v);
  } else if (!strcmp(iface, wl_output_interface.name)) {
    if (version < 4) {
      fprintf(stderr, "wl_output v4 required for stable output names\n");
      exit(EXIT_FAILURE);
    }
    struct output *out = calloc(1, sizeof(*out));
    if (!out)
      die("calloc output");
    out->wl = wl_registry_bind(reg, name, &wl_output_interface, 4);
    wl_output_add_listener(out->wl, &output_listener, out);
    out->next = outputs;
    outputs = out;
  }
}

static void registry_remove(void *data, struct wl_registry *reg,
                            uint32_t name) {
  (void)data;
  (void)reg;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static uint64_t make_session_id(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t x = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
  x ^= (uint64_t)(unsigned)getpid() << 17;
  return x ? x : 1;
}

static void setup_udp(const char *ip, int port) {
  udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (udp_fd < 0)
    die("socket");

  /* A few milliseconds of local TX elasticity only.  Transmission itself
   * runs on a dedicated thread, so this queue can never stall Wayland. */
  int sndbuf = 256 * 1024;
  if (setsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0)
    die("SO_SNDBUF");

  int tos = IPTOS_LOWDELAY;
  (void)setsockopt(udp_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

#ifdef IP_MTU_DISCOVER
  int pmtu = IP_PMTUDISC_DO;
  (void)setsockopt(udp_fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof(pmtu));
#endif

  struct sockaddr_in dst = {
      .sin_family = AF_INET,
      .sin_port = htons((uint16_t)port),
  };
  if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
    fprintf(stderr, "bad IPv4 address: %s\n", ip);
    exit(EXIT_FAILURE);
  }
  if (connect(udp_fd, (struct sockaddr *)&dst, sizeof(dst)) < 0)
    die("connect UDP");

  socklen_t sl = sizeof(sndbuf);
  if (getsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl) == 0)
    fprintf(stderr, "UDP SO_SNDBUF actual=%d bytes\n", sndbuf);
}

static void alloc_capture_buffer(uint32_t format, uint32_t width,
                                 uint32_t height, uint32_t stride) {
  if (width != stream_width || height != stream_height) {
    fprintf(stderr, "capture is %ux%u, expected %ux%u\n", width, height,
            stream_width, stream_height);
    fprintf(stderr, "set the Hyprland output to %ux%u@%u\n", stream_width,
            stream_height, stream_fps);
    exit(EXIT_FAILURE);
  }

  if (format != WL_SHM_FORMAT_XRGB8888 && format != WL_SHM_FORMAT_ARGB8888) {
    fprintf(stderr,
            "unsupported wl_shm format 0x%08x; need XRGB8888/ARGB8888\n",
            format);
    exit(EXIT_FAILURE);
  }

  size_t size = (size_t)stride * height;
  int fd = memfd_create("netdisplay-capture", MFD_CLOEXEC);
  if (fd < 0)
    die("memfd_create");
  if (ftruncate(fd, (off_t)size) < 0)
    die("ftruncate capture");

  void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
    die("mmap capture");

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
  if (!pool)
    die("wl_shm_create_pool");
  struct wl_buffer *buffer = wl_shm_pool_create_buffer(
      pool, 0, (int)width, (int)height, (int)stride, format);
  wl_shm_pool_destroy(pool);
  close(fd);
  if (!buffer)
    die("wl_shm_pool_create_buffer");

  cap_buffer = buffer;
  cap_map = map;
  cap_size = size;
  buf_format = format;
  buf_width = width;
  buf_height = height;
  buf_stride = stride;

  /* Wayland XRGB8888/ARGB8888 are native-endian 32-bit words. These FFmpeg
   * aliases describe exactly 0x00RRGGBB / 0xAARRGGBB native-endian memory. */
  capture_format =
      (format == WL_SHM_FORMAT_XRGB8888) ? AV_PIX_FMT_0RGB32 : AV_PIX_FMT_RGB32;
}

static void init_encoder(void) {
  if (nd_encoder_open(&encoder, selected_encoder, (int)stream_width,
                       (int)stream_height, (int)stream_fps, &encoder_rate, capture_format, 0) < 0) {
    fprintf(stderr, "cannot open encoder '%s'\n", selected_encoder);
    exit(EXIT_FAILURE);
  }
  enc = encoder.context;
  encoder_sw_format = encoder.sw_format;
  encoder_uses_hw_frames = encoder.frames != NULL;

  convert_frame = av_frame_alloc();
  enc_pkt = av_packet_alloc();
  if (!convert_frame || !enc_pkt)
    die("av_frame/packet_alloc");
  convert_frame->format = encoder_sw_format;
  convert_frame->width = enc->width;
  convert_frame->height = enc->height;
  convert_frame->color_range = enc->color_range;
  convert_frame->color_primaries = enc->color_primaries;
  convert_frame->color_trc = enc->color_trc;
  convert_frame->colorspace = enc->colorspace;
  if (av_frame_get_buffer(convert_frame, 32) < 0)
    die("av_frame_get_buffer");

  if (encoder_uses_hw_frames) {
    enc_frame = av_frame_alloc();
    if (!enc_frame || av_hwframe_get_buffer(encoder.frames, enc_frame, 0) < 0)
      die("av_hwframe_get_buffer");
  } else {
    enc_frame = convert_frame;
  }

  sws = sws_getContext(enc->width, enc->height, capture_format, enc->width,
                       enc->height, encoder_sw_format, SWS_FAST_BILINEAR, NULL,
                       NULL, NULL);
  if (!sws)
    die("sws_getContext");

  fprintf(
      stderr,
      "%s via %s: %ux%u@%u transfer=%s encoder=%s capture=%s all-intra\n",
      ndc_codec_name(selected_codec), selected_encoder, stream_width,
      stream_height, stream_fps,
      av_get_pix_fmt_name(encoder_sw_format)
          ? av_get_pix_fmt_name(encoder_sw_format)
          : "?",
      av_get_pix_fmt_name(enc->pix_fmt) ? av_get_pix_fmt_name(enc->pix_fmt)
                                        : "?",
      av_get_pix_fmt_name(capture_format) ? av_get_pix_fmt_name(capture_format)
                                          : "?");
  if (encoder_rate.mode == ND_RATE_QP)
    fprintf(stderr, "rate-control=qp QP=%d\n", encoder_rate.qp);
  else
    fprintf(stderr, "rate-control=bandwidth target=%lld peak=%lld kbit/s VBV=%d bits\n",
            (long long)enc->bit_rate / 1000,
            (long long)enc->rc_max_rate / 1000, enc->rc_buffer_size);
}

static void print_stats(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!stat_last.tv_sec && !stat_last.tv_nsec) {
    stat_last = now;
    return;
  }
  double dt = (double)(now.tv_sec - stat_last.tv_sec) +
              (double)(now.tv_nsec - stat_last.tv_nsec) / 1e9;
  if (dt < 1.0)
    return;

  static unsigned long long lc, le, lb, lt, lr, la, lg, lerr, lo;
  struct nd_video_tx_stats stats = nd_video_tx_snapshot(tx);
  unsigned long long b = stats.bytes, t = stats.complete, r = stats.replaced;
  unsigned long long a = stats.aborted, g = stats.eagain, er = stats.errors;
  unsigned long long o = stats.oversize;

  fprintf(
      stderr,
      "capture %.1f  encoded %.1f  tx %.1f Mbit/s  complete %.1f  latest-drop "
      "%llu  tx-abort %llu  eagain %llu  err %llu  oversize %llu\n",
      (stat_capture - lc) / dt, (stat_encoded - le) / dt,
      (b - lb) * 8.0 / dt / 1e6, (t - lt) / dt, r - lr, a - la, g - lg,
      er - lerr, o - lo);

  lc = stat_capture;
  le = stat_encoded;
  lb = b;
  lt = t;
  lr = r;
  la = a;
  lg = g;
  lerr = er;
  lo = o;
  stat_last = now;
}

static int encode_and_publish(int y_invert) {
  uint8_t *src = cap_map;
  int linesize = (int)buf_stride;

  if (y_invert) {
    if (!flip_map) {
      flip_map = av_malloc(cap_size);
      if (!flip_map)
        die("av_malloc flip");
    }
    for (uint32_t y = 0; y < stream_height; y++)
      memcpy(flip_map + (size_t)y * buf_stride,
             cap_map + (size_t)(stream_height - 1u - y) * buf_stride,
             buf_stride);
    src = flip_map;
  }

  if (av_frame_make_writable(convert_frame) < 0)
    return -1;
  const uint8_t *src_data[4] = {src, NULL, NULL, NULL};
  int src_linesize[4] = {linesize, 0, 0, 0};
  if (sws_scale(sws, src_data, src_linesize, 0, (int)stream_height,
                convert_frame->data, convert_frame->linesize) <= 0)
    return -1;
  convert_frame->pts = (int64_t)frame_pts++;
  convert_frame->pict_type = AV_PICTURE_TYPE_I;

  int ret;
  if (encoder_uses_hw_frames) {
    if (av_frame_make_writable(enc_frame) < 0 ||
        av_hwframe_transfer_data(enc_frame, convert_frame, 0) < 0 ||
        av_frame_copy_props(enc_frame, convert_frame) < 0)
      return -1;
  }
  ret = avcodec_send_frame(enc, enc_frame);
  if (ret < 0) {
    char b[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, b, sizeof(b));
    fprintf(stderr, "%s send failed: %s\n", selected_encoder, b);
    return -1;
  }

  int produced = 0;
  for (;;) {
    ret = avcodec_receive_packet(enc, enc_pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    if (ret < 0)
      ff_die("avcodec_receive_packet", ret);
    produced = 1;
    if (frame_encrypted && seq_id == UINT32_MAX) {
      fprintf(stderr, "encrypted frame counter exhausted; restarting stream\n");
      exit(EXIT_SUCCESS);
    }
    stat_encoded++;
    uint32_t seq = ++seq_id;
    nd_video_tx_publish(tx, enc_pkt, seq);
  }
  return produced ? 0 : 1;
}

static void request_frame(void);

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t width, uint32_t height,
                         uint32_t stride) {
  (void)data;
  (void)frame;
  frame_format = format;
  frame_width = width;
  frame_height = height;
  frame_stride = stride;
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t flags) {
  (void)data;
  (void)frame;
  frame_y_invert = !!(flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
  (void)data;
  (void)sec_hi;
  (void)sec_lo;
  (void)nsec;
  zwlr_screencopy_frame_v1_destroy(frame);
  stat_capture++;

  (void)encode_and_publish(frame_y_invert);

  /* UDP runs independently.  Request the next compositor frame as soon as
   * the encoder has accepted this one; Ethernet serialization is no longer in
   * the 60 Hz capture critical path. */
  request_frame();
  print_stats();
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
  (void)data;
  fprintf(stderr, "screencopy failed; retrying\n");
  zwlr_screencopy_frame_v1_destroy(frame);
  request_frame();
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t x, uint32_t y, uint32_t width,
                         uint32_t height) {
  (void)data;
  (void)frame;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
}

static void frame_linux_dmabuf(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t format, uint32_t width,
                               uint32_t height) {
  (void)data;
  (void)frame;
  (void)format;
  (void)width;
  (void)height;
}

static void frame_buffer_done(void *data,
                              struct zwlr_screencopy_frame_v1 *frame) {
  (void)data;
  if (!frame_width || !frame_height || !frame_stride) {
    fprintf(stderr, "compositor did not offer wl_shm screencopy\n");
    exit(EXIT_FAILURE);
  }

  if (!cap_buffer) {
    alloc_capture_buffer(frame_format, frame_width, frame_height, frame_stride);
    init_encoder();
  } else if (frame_format != buf_format || frame_width != buf_width ||
             frame_height != buf_height || frame_stride != buf_stride ||
             (size_t)buf_stride * buf_height != cap_size) {
    fprintf(stderr, "capture format/size changed mid-stream\n");
    exit(EXIT_FAILURE);
  }

  zwlr_screencopy_frame_v1_copy(frame, cap_buffer);
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .damage = frame_damage,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done,
};

static void flush_wayland(void) {
  struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLOUT};
  for (;;) {
    if (wl_display_flush(display) >= 0)
      return;
    if (errno != EAGAIN)
      die("wl_display_flush");
    while (poll(&pfd, 1, -1) < 0)
      if (errno != EINTR)
        die("poll Wayland");
  }
}

static void request_frame(void) {
  frame_y_invert = 0;
  frame_format = frame_width = frame_height = frame_stride = 0;
  struct zwlr_screencopy_frame_v1 *f =
      zwlr_screencopy_manager_v1_capture_output(sc_mgr, 0, target->wl);
  if (!f) {
    fprintf(stderr, "capture_output failed\n");
    exit(EXIT_FAILURE);
  }
  zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, NULL);
  flush_wayland();
}

int nd_video_sender_run(const char *output_name, const char *ip, int port,
                        const struct nd_encoder_rate *rate,
                        int width, int height, int refresh_hz,
                        uint64_t wire_session, int encrypted, int key_fd,
                        uint16_t video_codec, const char *encoder_name) {
  if (!output_name || !*output_name || !ip || !*ip || port <= 0 ||
      port > 65535 || !nd_encoder_rate_valid(rate) || width <= 0 ||
      width > UINT16_MAX || (width & 1) || height <= 0 || height > UINT16_MAX ||
      (height & 1) || refresh_hz <= 0 || refresh_hz > UINT16_MAX ||
      !ndc_codec_bit(video_codec) || !encoder_name || !*encoder_name) {
    fprintf(stderr, "invalid built-in video sender parameters\n");
    return 2;
  }
  encoder_rate = *rate;
  selected_codec = video_codec;
  snprintf(selected_encoder, sizeof(selected_encoder), "%s", encoder_name);
  stream_width = (uint32_t)width;
  stream_height = (uint32_t)height;
  stream_fps = (uint32_t)refresh_hz;
  frame_encrypted = encrypted;
  if (frame_encrypted) {
    if (key_fd < 0 || ndc_read_full(key_fd, frame_key, sizeof(frame_key)) <= 0)
      die("read video key");
    close(key_fd);
  }

  setup_udp(ip, port);
  session_id = wire_session ? wire_session : make_session_id();
  seq_id = 0;
  tx = nd_video_tx_open(udp_fd, session_id, frame_encrypted ? frame_key : NULL,
                        stream_fps);
  explicit_bzero(frame_key, sizeof(frame_key));
  if (!tx)
    die("video transmitter");

  display = wl_display_connect(NULL);
  if (!display)
    die("wl_display_connect");
  struct wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &registry_listener, NULL);
  if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0)
    die("wl_display_roundtrip");

  if (!shm || !sc_mgr) {
    fprintf(stderr, "missing wl_shm or wlr-screencopy global\n");
    return 1;
  }

  fprintf(stderr, "Wayland outputs:");
  for (struct output *o = outputs; o; o = o->next) {
    fprintf(stderr, " %s", o->name[0] ? o->name : "?");
    if (!strcmp(o->name, output_name))
      target = o;
  }
  fputc('\n', stderr);
  if (!target) {
    fprintf(stderr, "output '%s' not found\n", output_name);
    return 1;
  }

  fprintf(
      stderr,
      "stream %ux%u@%u codec=%s encoder=%s: output=%s -> %s:%d session=%016llx "
      "encryption=%s\n",
      stream_width, stream_height, stream_fps, ndc_codec_name(selected_codec),
      selected_encoder, output_name, ip, port, (unsigned long long)session_id,
      frame_encrypted ? "on" : "off");

  request_frame();
  while (wl_display_dispatch(display) >= 0) {
  }
  perror("wl_display_dispatch");
  nd_video_tx_close(tx);
  return 1;
}
