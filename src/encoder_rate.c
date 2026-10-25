// SPDX-License-Identifier: GPL-2.0-only
#include "encoder_rate.h"

#include <errno.h>
#include <libavutil/opt.h>
#include <stdlib.h>
#include <string.h>

int nd_rate_control_parse(const char *value, enum nd_rate_control *mode) {
  if (!value || !mode)
    return -1;
  if (!strcmp(value, "qp"))
    *mode = ND_RATE_QP;
  else if (!strcmp(value, "bandwidth"))
    *mode = ND_RATE_BANDWIDTH;
  else
    return -1;
  return 0;
}

int nd_bitrate_kbps_parse(const char *value, int *bitrate_kbps) {
  if (!value || !*value || !bitrate_kbps)
    return -1;
  for (const char *p = value; *p; p++)
    if (*p < '0' || *p > '9')
      return -1;
  char *end;
  errno = 0;
  long parsed = strtol(value, &end, 10);
  if (errno || *end || parsed < 1 || parsed > ND_MAX_BITRATE_KBPS)
    return -1;
  *bitrate_kbps = (int)parsed;
  return 0;
}

int nd_encoder_rate_valid(const struct nd_encoder_rate *rate) {
  return rate &&
         (rate->mode == ND_RATE_QP || rate->mode == ND_RATE_BANDWIDTH) &&
         rate->qp >= 0 && rate->qp <= 51 && rate->bitrate_kbps >= 1 &&
         rate->bitrate_kbps <= ND_MAX_BITRATE_KBPS;
}

int nd_encoder_rate_apply(AVCodecContext *ctx,
                          const struct nd_encoder_rate *rate, int fps) {
  if (!ctx || !ctx->codec || !nd_encoder_rate_valid(rate) || fps < 1)
    return AVERROR(EINVAL);
  const char *name = ctx->codec->name;
  int bandwidth = rate->mode == ND_RATE_BANDWIDTH;
  ctx->flags &= ~AV_CODEC_FLAG_QSCALE;
  ctx->global_quality = 0;
  ctx->bit_rate = ctx->rc_min_rate = ctx->rc_max_rate = 0;
  ctx->rc_buffer_size = ctx->rc_initial_buffer_occupancy = 0;
  /* All submitted frames are intra; use the requested QP without a hidden
   * I-frame quantizer multiplier. QP scales still differ between codecs. */
  ctx->i_quant_factor = ctx->b_quant_factor = 1.0f;
  ctx->i_quant_offset = ctx->b_quant_offset = 0.0f;
  if (bandwidth) {
    int64_t peak = (int64_t)rate->bitrate_kbps * 1000;
    /* Keep target below peak so QSV selects VBR, not CBR. Two frame periods
     * bound the requested VBV reservoir without adding a video FIFO. This is
     * encoder rate control, not a strict instantaneous network speed limit. */
    ctx->bit_rate = peak * 4 / 5;
    ctx->rc_max_rate = peak;
    ctx->rc_buffer_size = (int)((peak * 2 + fps - 1) / fps);
    ctx->rc_initial_buffer_occupancy = ctx->rc_buffer_size / 2;
    /* Do not let FFmpeg's default qmax=31 prevent heavy compression. */
    ctx->qmin = 0;
    ctx->qmax = ctx->codec_id == AV_CODEC_ID_AV1 ? 255 : 51;
  }

  int ret;
  if (strstr(name, "_nvenc")) {
    ret = av_opt_set(ctx->priv_data, "rc", bandwidth ? "vbr" : "constqp", 0);
    if (ret < 0)
      return ret;
    return av_opt_set_int(ctx->priv_data, "qp", bandwidth ? -1 : rate->qp, 0);
  }
  if (strstr(name, "_qsv")) {
    if (!bandwidth) {
      ctx->flags |= AV_CODEC_FLAG_QSCALE;
      ctx->global_quality = rate->qp * FF_QP2LAMBDA;
    }
    return 0;
  }
  if (strstr(name, "_amf")) {
    ret = av_opt_set(ctx->priv_data, "rc", bandwidth ? "vbr_peak" : "cqp", 0);
    if (ret < 0)
      return ret;
    if (bandwidth)
      return av_opt_set_int(ctx->priv_data, "filler_data", 0, 0);
    return av_opt_set_int(ctx->priv_data, "qp_i", rate->qp, 0);
  }
  if (strstr(name, "_vaapi")) {
    ret = av_opt_set(ctx->priv_data, "rc_mode", bandwidth ? "VBR" : "CQP", 0);
    if (ret < 0 || bandwidth)
      return ret;
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    /* VAAPI treats global_quality=0 as unset. A positive lambda smaller
     * than FF_QP2LAMBDA explicitly selects QP 0 after integer division. */
    ctx->global_quality = rate->qp * FF_QP2LAMBDA + 1;
    return 0;
  }
  if (!strcmp(name, "libx264") || !strcmp(name, "libx265"))
    return av_opt_set_int(ctx->priv_data, "qp", bandwidth ? -1 : rate->qp, 0);
  return AVERROR(ENOSYS);
}
