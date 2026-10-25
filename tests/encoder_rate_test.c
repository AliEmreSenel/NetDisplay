// SPDX-License-Identifier: GPL-2.0-only
#include "encoder.h"
#include <stdio.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                  \
      return -1;                                                               \
    }                                                                          \
  } while (0)

enum { WIDTH = 320, HEIGHT = 180, FPS = 30, FRAMES = 120 };

/* Exercise real rate control, alternating cheap flat images and expensive
 * noise. Every packet must be immediately available and independently usable,
 * preserving the stream's no-backlog / all-intra contract in both modes. */
static int64_t encode(const AVCodec *codec, struct nd_encoder_rate rate) {
  struct nd_encoder encoder;
  CHECK(nd_encoder_open(&encoder, codec->name, WIDTH, HEIGHT, FPS, 1, &rate,
                        AV_PIX_FMT_YUV420P, 1) >= 0);
  AVCodecContext *ctx = encoder.context;
  AVFrame *frame = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  CHECK(frame && packet);
  frame->format = ctx->pix_fmt;
  frame->width = WIDTH;
  frame->height = HEIGHT;
  CHECK(av_frame_get_buffer(frame, 32) == 0);
  int64_t bytes = 0;
  uint32_t random = 1;
  for (int i = 0; i < FRAMES; i++) {
    CHECK(av_frame_make_writable(frame) == 0);
    for (int p = 0; p < 3; p++) {
      int width = p ? WIDTH / 2 : WIDTH;
      int height = p ? HEIGHT / 2 : HEIGHT;
      for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
          random = random * 1664525u + 1013904223u;
          frame->data[p][y * frame->linesize[p] + x] =
              i % 60 < 15 ? 128 : (uint8_t)(random >> 24);
        }
    }
    frame->pts = i;
    frame->pict_type = AV_PICTURE_TYPE_I;
    CHECK(avcodec_send_frame(ctx, frame) == 0);
    CHECK(avcodec_receive_packet(ctx, packet) == 0);
    CHECK(packet->flags & AV_PKT_FLAG_KEY);
    bytes += packet->size;
    av_packet_unref(packet);
    CHECK(avcodec_receive_packet(ctx, packet) == AVERROR(EAGAIN));
  }
  if (rate.mode == ND_RATE_BANDWIDTH) {
    int64_t allowance = ((int64_t)rate.bitrate_kbps * 1000 * FRAMES / FPS +
                         ctx->rc_buffer_size) /
                        8;
    if (bytes > allowance * 11 / 10)
      fprintf(stderr, "%d kbit/s: encoded=%lld allowance=%lld bytes\n",
              rate.bitrate_kbps, (long long)bytes, (long long)allowance);
    CHECK(bytes <= allowance * 11 / 10);
  }
  av_packet_free(&packet);
  av_frame_free(&frame);
  nd_encoder_close(&encoder);
  return bytes;
}

static int validate(void) {
  enum nd_rate_control mode;
  int bitrate;
  unsigned interval;
  CHECK(nd_keyframe_interval_parse("1", &interval) == 0 && interval == 1);
  CHECK(nd_keyframe_interval_parse("3", &interval) == 0 && interval == 3);
  CHECK(nd_keyframe_interval_parse("60", &interval) == 0 && interval == 60);
  const char *bad_interval[] = {"", "0", "-1", "61", "1.5", "3p", "9999999999999999999"};
  for (unsigned i = 0; i < sizeof(bad_interval) / sizeof(bad_interval[0]); i++)
    CHECK(nd_keyframe_interval_parse(bad_interval[i], &interval) < 0);
  CHECK(nd_rate_control_parse("qp", &mode) == 0 && mode == ND_RATE_QP);
  CHECK(nd_rate_control_parse("bandwidth", &mode) == 0 &&
        mode == ND_RATE_BANDWIDTH);
  CHECK(nd_rate_control_parse("adaptive", &mode) < 0);
  CHECK(nd_bitrate_kbps_parse("20000", &bitrate) == 0 && bitrate == 20000);
  const char *invalid[] = {"",
                           "0",
                           "-1",
                           "1.5",
                           "20mbps",
                           "1000001",
                           "99999999999999999999999999999999"};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    CHECK(nd_bitrate_kbps_parse(invalid[i], &bitrate) < 0);
  return 0;
}

int main(void) {
  if (validate() < 0)
    return 1;
  const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
  if (!codec) {
    puts("SKIP: libx264 is required for encoder rate-control integration "
         "testing");
    return 77;
  }
  av_log_set_level(AV_LOG_ERROR);
  int64_t high_quality =
      encode(codec, (struct nd_encoder_rate){ND_RATE_QP, 20, 20000});
  int64_t low_quality =
      encode(codec, (struct nd_encoder_rate){ND_RATE_QP, 40, 20000});
  int64_t constrained =
      encode(codec, (struct nd_encoder_rate){ND_RATE_BANDWIDTH, 20, 4000});
  int64_t generous =
      encode(codec, (struct nd_encoder_rate){ND_RATE_BANDWIDTH, 20, 10000});
  /* QP is inactive in bandwidth mode. Production slice threading allows small
   * rate-control variations between encodes, so compare with a 2% tolerance. */
  int64_t constrained_other_qp =
      encode(codec, (struct nd_encoder_rate){ND_RATE_BANDWIDTH, 40, 4000});
  int64_t difference = constrained > constrained_other_qp
                           ? constrained - constrained_other_qp
                           : constrained_other_qp - constrained;
  if (high_quality <= 0 || low_quality <= 0 || constrained <= 0 ||
      generous <= 0 || constrained_other_qp <= 0 ||
      high_quality <= low_quality * 3 / 2 || generous <= constrained * 3 / 2 ||
      difference > constrained / 50) {
    fprintf(stderr, "Unexpected encoded sizes: QP20=%lld QP40=%lld 4Mbps=%lld "
                    "10Mbps=%lld other-QP=%lld\n",
            (long long)high_quality, (long long)low_quality,
            (long long)constrained, (long long)generous,
            (long long)constrained_other_qp);
    return 1;
  }
  printf("PASS: real all-intra encoding, immediate output, QP size response, "
         "bitrate budget and scene changes (QP20=%lld QP40=%lld 4Mbps=%lld "
         "10Mbps=%lld bytes)\n",
         (long long)high_quality, (long long)low_quality,
         (long long)constrained, (long long)generous);
  return 0;
}
