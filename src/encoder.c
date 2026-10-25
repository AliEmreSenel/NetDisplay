// SPDX-License-Identifier: GPL-2.0-only
#include "encoder.h"
#include "control_proto.h"
#include <stdio.h>
#include <string.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

struct encoder_candidate {
  uint16_t codec;
  const char *name;
  enum AVHWDeviceType hw_type;
};

/* Codec order is negotiated separately. Within one codec, prefer native GPU
 * backends that are common on Linux. The common pipeline supports both
 * software-frame hardware encoders (NVENC/QSV/AMF) and hardware-frame-only
 * encoders (notably VAAPI). */
static const struct encoder_candidate encoder_candidates[] = {
    {NDC_VIDEO_AV1, "av1_nvenc", AV_HWDEVICE_TYPE_CUDA},
    {NDC_VIDEO_AV1, "av1_qsv", AV_HWDEVICE_TYPE_QSV},
    {NDC_VIDEO_AV1, "av1_amf", AV_HWDEVICE_TYPE_NONE},
    {NDC_VIDEO_AV1, "av1_vaapi", AV_HWDEVICE_TYPE_VAAPI},
    {NDC_VIDEO_HEVC, "hevc_nvenc", AV_HWDEVICE_TYPE_CUDA},
    {NDC_VIDEO_HEVC, "hevc_qsv", AV_HWDEVICE_TYPE_QSV},
    {NDC_VIDEO_HEVC, "hevc_amf", AV_HWDEVICE_TYPE_NONE},
    {NDC_VIDEO_HEVC, "hevc_vaapi", AV_HWDEVICE_TYPE_VAAPI},
    {NDC_VIDEO_H264, "h264_nvenc", AV_HWDEVICE_TYPE_CUDA},
    {NDC_VIDEO_H264, "h264_qsv", AV_HWDEVICE_TYPE_QSV},
    {NDC_VIDEO_H264, "h264_amf", AV_HWDEVICE_TYPE_NONE},
    {NDC_VIDEO_H264, "h264_vaapi", AV_HWDEVICE_TYPE_VAAPI},
};

static const struct encoder_candidate *
find_encoder_candidate(const char *name) {
  for (size_t i = 0;
       i < sizeof(encoder_candidates) / sizeof(encoder_candidates[0]); i++)
    if (!strcmp(encoder_candidates[i].name, name))
      return &encoder_candidates[i];
  return NULL;
}

static const enum AVPixelFormat *codec_pix_fmts(const AVCodec *codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const void *list = NULL;
  if (!codec ||
      avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   &list, NULL) < 0)
    return NULL;
  return (const enum AVPixelFormat *)list;
#else
  /* FFmpeg before 7.1 exposes the same terminated list on AVCodec. */
  return codec ? codec->pix_fmts : NULL;
#endif
}

static int codec_accepts_pix_fmt(const AVCodec *codec, enum AVPixelFormat fmt);

static enum AVPixelFormat choose_software_pix_fmt(
    const AVCodec *codec, enum AVPixelFormat preferred) {
  if (preferred != AV_PIX_FMT_NONE &&
      codec_accepts_pix_fmt(codec, preferred)) {
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(preferred);
    if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
      return preferred;
  }
  const enum AVPixelFormat *fmts = codec_pix_fmts(codec);
  if (!fmts)
    return AV_PIX_FMT_NONE;
  for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
    if (!d || (d->flags & AV_PIX_FMT_FLAG_HWACCEL))
      continue;
    if (sws_isSupportedOutput(*p))
      return *p;
  }
  return AV_PIX_FMT_NONE;
}

static enum AVPixelFormat hw_format_for_type(enum AVHWDeviceType type) {
  switch (type) {
  case AV_HWDEVICE_TYPE_VAAPI:
    return AV_PIX_FMT_VAAPI;
  case AV_HWDEVICE_TYPE_QSV:
    return AV_PIX_FMT_QSV;
  case AV_HWDEVICE_TYPE_CUDA:
    return AV_PIX_FMT_CUDA;
  default:
    return AV_PIX_FMT_NONE;
  }
}

static int codec_accepts_pix_fmt(const AVCodec *codec, enum AVPixelFormat fmt) {
  const enum AVPixelFormat *fmts = codec_pix_fmts(codec);
  if (!fmts)
    return 1;
  for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
    if (*p == fmt)
      return 1;
  return 0;
}

static enum AVPixelFormat choose_transfer_pix_fmt(AVBufferRef *device) {
  AVHWFramesConstraints *constraints =
      av_hwdevice_get_hwframe_constraints(device, NULL);
  if (!constraints)
    return AV_PIX_FMT_NONE;
  enum AVPixelFormat chosen = AV_PIX_FMT_NONE;
  if (constraints->valid_sw_formats) {
    for (const enum AVPixelFormat *p = constraints->valid_sw_formats;
         *p != AV_PIX_FMT_NONE; p++) {
      const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
      if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL) &&
          sws_isSupportedOutput(*p)) {
        chosen = *p;
        break;
      }
    }
  }
  av_hwframe_constraints_free(&constraints);
  return chosen;
}

static void configure_encoder_context(AVCodecContext *c, enum AVPixelFormat fmt,
                                      int width, int height, int fps) {
  c->width = width;
  c->height = height;
  c->time_base = (AVRational){1, fps};
  c->framerate = (AVRational){fps, 1};
  c->pix_fmt = fmt;
  c->max_b_frames = 0;
  c->refs = 1;
  c->thread_count = 2;
  c->color_range = AVCOL_RANGE_MPEG;
  c->color_primaries = AVCOL_PRI_BT709;
  c->color_trc = AVCOL_TRC_BT709;
  c->colorspace = AVCOL_SPC_BT709;
  c->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_CLOSED_GOP;
}

static void encoder_open_options(const char *encoder_name,
                                 AVDictionary **opts) {
  if (strstr(encoder_name, "_nvenc")) {
    av_dict_set(opts, "preset", "p1", 0);
    av_dict_set(opts, "tune", "ull", 0);
    av_dict_set(opts, "zerolatency", "1", 0);
    av_dict_set(opts, "forced-idr", "1", 0);
    av_dict_set(opts, "delay", "0", 0);
    av_dict_set(opts, "rc-lookahead", "0", 0);
    av_dict_set(opts, "rgb_mode", "yuv420", 0);
    if (!strstr(encoder_name, "av1_"))
      av_dict_set(opts, "aud", "1", 0);
  } else if (strstr(encoder_name, "_qsv")) {
    av_dict_set(opts, "preset", "veryfast", 0);
    av_dict_set(opts, "async_depth", "1", 0);
    if (!strstr(encoder_name, "av1_"))
      av_dict_set(opts, "forced_idr", "1", 0);
    if (!strcmp(encoder_name, "h264_qsv"))
      av_dict_set(opts, "look_ahead", "0", 0);
  } else if (strstr(encoder_name, "_amf")) {
    if (!strstr(encoder_name, "av1_"))
      av_dict_set(opts, "forced_idr", "1", 0);
    av_dict_set(opts, "usage", "ultralowlatency", 0);
    av_dict_set(opts, "quality", "speed", 0);
  } else if (strstr(encoder_name, "_vaapi")) {
    av_dict_set(opts, "async_depth", "1", 0);
  } else if (!strcmp(encoder_name, "libx264") || !strcmp(encoder_name, "libx265")) {
    av_dict_set(opts, "preset", "ultrafast", 0);
    av_dict_set(opts, "tune", "zerolatency", 0);
    av_dict_set(opts, "forced-idr", "1", 0);
  }
}

static int setup_encoder_input(AVCodecContext *ctx, const AVCodec *codec,
                               const struct encoder_candidate *candidate,
                               int width, int height, int fps,
                               enum AVPixelFormat preferred_sw_format,
                               AVBufferRef **device_out,
                               AVBufferRef **frames_out,
                               enum AVPixelFormat *sw_format_out) {
  enum AVPixelFormat sw_format =
      choose_software_pix_fmt(codec, preferred_sw_format);
  if (sw_format != AV_PIX_FMT_NONE) {
    configure_encoder_context(ctx, sw_format, width, height, fps);
    *sw_format_out = sw_format;
    return 0;
  }

  if (!candidate || candidate->hw_type == AV_HWDEVICE_TYPE_NONE)
    return -1;
  enum AVPixelFormat hw_format = hw_format_for_type(candidate->hw_type);
  if (hw_format == AV_PIX_FMT_NONE || !codec_accepts_pix_fmt(codec, hw_format))
    return -1;

  AVBufferRef *device = NULL;
  if (av_hwdevice_ctx_create(&device, candidate->hw_type, NULL, NULL, 0) < 0)
    return -1;
  sw_format = choose_transfer_pix_fmt(device);
  if (sw_format == AV_PIX_FMT_NONE) {
    av_buffer_unref(&device);
    return -1;
  }

  AVBufferRef *frames = av_hwframe_ctx_alloc(device);
  if (!frames) {
    av_buffer_unref(&device);
    return -1;
  }
  AVHWFramesContext *frames_ctx = (AVHWFramesContext *)frames->data;
  frames_ctx->format = hw_format;
  frames_ctx->sw_format = sw_format;
  frames_ctx->width = width;
  frames_ctx->height = height;
  frames_ctx->initial_pool_size = 4;
  if (av_hwframe_ctx_init(frames) < 0) {
    av_buffer_unref(&frames);
    av_buffer_unref(&device);
    return -1;
  }

  configure_encoder_context(ctx, hw_format, width, height, fps);
  ctx->hw_frames_ctx = av_buffer_ref(frames);
  if (!ctx->hw_frames_ctx) {
    av_buffer_unref(&frames);
    av_buffer_unref(&device);
    return -1;
  }
  *device_out = device;
  *frames_out = frames;
  *sw_format_out = sw_format;
  return 0;
}

void nd_encoder_close(struct nd_encoder *e) {
  avcodec_free_context(&e->context);
  av_buffer_unref(&e->frames);
  av_buffer_unref(&e->device);
}

int nd_keyframe_interval_parse(const char *value, unsigned *interval) {
  if (!value || !*value || !interval) return -1;
  unsigned n = 0;
  for (const char *p = value; *p; p++) {
    if (*p < '0' || *p > '9') return -1;
    n = n * 10u + (unsigned)(*p - '0');
    if (n > ND_MAX_KEYFRAME_INTERVAL) return -1;
  }
  if (!n) return -1;
  *interval = n;
  return 0;
}

void nd_encoder_prepare_frame(struct nd_encoder *e, AVFrame *frame, int force) {
  if (force || e->frames_since_keyframe >= e->keyframe_interval) {
    frame->pict_type = AV_PICTURE_TYPE_I;
    e->frames_since_keyframe = 1;
  } else {
    frame->pict_type = AV_PICTURE_TYPE_P;
    e->frames_since_keyframe++;
  }
}

int nd_encoder_open(struct nd_encoder *e, const char *name, int width,
                     int height, int fps, unsigned keyframe_interval,
                     const struct nd_encoder_rate *rate,
                     enum AVPixelFormat preferred, int quiet) {
  *e = (struct nd_encoder){.sw_format = AV_PIX_FMT_NONE};
  if (!name || width <= 0 || height <= 0 || fps <= 0 || !keyframe_interval ||
      keyframe_interval > ND_MAX_KEYFRAME_INTERVAL || !nd_encoder_rate_valid(rate))
    return -1;
  e->keyframe_interval = e->frames_since_keyframe = keyframe_interval;
  const struct encoder_candidate *candidate = find_encoder_candidate(name);
  /* Software fallback is explicit; hardware negotiation never selects it. */
  if (!candidate && strcmp(name, "libx264") && strcmp(name, "libx265")) return -1;
  const AVCodec *codec = avcodec_find_encoder_by_name(name);
  if (!codec || !(e->context = avcodec_alloc_context3(codec))) return -1;
  if (quiet) e->context->log_level_offset = 128;
  int ret = setup_encoder_input(e->context, codec, candidate, width, height, fps,
                                preferred, &e->device, &e->frames, &e->sw_format);
  AVDictionary *opts = NULL;
  /* Some NVENC generations reject GOP=1. Explicit I-frame submission still
   * makes interval=1 independent with an advertised GOP size of two. */
  e->context->gop_size = keyframe_interval == 1 ? 2 : (int)keyframe_interval;
  if (ret == 0)
    ret = nd_encoder_rate_apply(e->context, rate, fps);
  if (ret == 0) {
    encoder_open_options(name, &opts);
    ret = avcodec_open2(e->context, codec, &opts);
  }
  if (ret >= 0 && strstr(name, "_nvenc")) {
    int64_t delay = -1;
    if (av_opt_get_int(e->context->priv_data, "delay", 0, &delay) < 0 || delay != 0) {
      if (!quiet) fprintf(stderr, "refusing buffered NVENC: delay=%lld\n", (long long)delay);
      ret = -1;
    }
  }
  if (!quiet) {
    AVDictionaryEntry *unused = NULL;
    while ((unused = av_dict_get(opts, "", unused, AV_DICT_IGNORE_SUFFIX)))
      fprintf(stderr, "unused %s option %s=%s\n", name, unused->key, unused->value);
  }
  av_dict_free(&opts);
  if (ret < 0) nd_encoder_close(e);
  return ret;
}

int nd_video_choose_encoder(uint16_t codec, int width, int height, int fps,
                            unsigned keyframe_interval,
                            const struct nd_encoder_rate *rate,
                            char *name, size_t name_size) {
  if (!ndc_codec_bit(codec) || !name || !name_size) return -1;
  for (size_t i = 0; i < sizeof(encoder_candidates) / sizeof(encoder_candidates[0]); i++) {
    if (encoder_candidates[i].codec != codec) continue;
    struct nd_encoder e;
    if (nd_encoder_open(&e, encoder_candidates[i].name, width, height, fps,
                         keyframe_interval, rate, AV_PIX_FMT_NONE, 1) >= 0) {
      nd_encoder_close(&e);
      snprintf(name, name_size, "%s", encoder_candidates[i].name);
      return 0;
    }
  }
  return -1;
}
