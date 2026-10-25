// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_ENCODER_RATE_H
#define NETDISPLAY_ENCODER_RATE_H

#include <libavcodec/avcodec.h>

#define ND_DEFAULT_BITRATE_KBPS 20000
#define ND_MAX_BITRATE_KBPS 1000000

enum nd_rate_control { ND_RATE_QP, ND_RATE_BANDWIDTH };

struct nd_encoder_rate {
  enum nd_rate_control mode;
  int qp;
  int bitrate_kbps;
};

int nd_rate_control_parse(const char *value, enum nd_rate_control *mode);
/* Decimal kb/s (1000 bits/s); reject malformed values and overflow. */
int nd_bitrate_kbps_parse(const char *value, int *bitrate_kbps);
int nd_encoder_rate_valid(const struct nd_encoder_rate *rate);
/* Apply before avcodec_open2; return an FFmpeg error if unsupported. */
int nd_encoder_rate_apply(AVCodecContext *ctx,
                          const struct nd_encoder_rate *rate, int fps);

#endif
