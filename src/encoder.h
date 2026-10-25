// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_ENCODER_H
#define ND_ENCODER_H
#include <stddef.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

struct nd_encoder {
  AVCodecContext *context;
  AVBufferRef *device, *frames;
  enum AVPixelFormat sw_format;
};
/* All callers submit independent I frames. Software fallback is selected by
 * name only; automatic negotiation considers hardware encoders exclusively. */
int nd_encoder_open(struct nd_encoder *, const char *name, int width, int height,
                     int fps, int qp, enum AVPixelFormat preferred, int quiet);
void nd_encoder_close(struct nd_encoder *);
int nd_video_choose_encoder(uint16_t codec, int width, int height, int fps,
                            int qp, char *name, size_t name_size);
#endif
