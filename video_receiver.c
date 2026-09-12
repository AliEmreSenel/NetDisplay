#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

#include "proto.h"
#include "common.h"
#include "crypto.h"
#include "video_receiver.h"

struct dumb_fb {
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    uint32_t fb_id;
    uint8_t *map;
};

struct plane_candidate {
    uint32_t id;
    uint64_t type;
    uint64_t zpos;
    uint64_t zmax;
    int have_zpos;
    int zpos_immutable;
};

static AVBufferRef *va_device;
static enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_VAAPI;

static void die(const char *s)
{
    perror(s);
    exit(EXIT_FAILURE);
}

static void ff_die(const char *what, int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "%s: %s\n", what, buf);
    exit(EXIT_FAILURE);
}

static inline void scanout_store_fence(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("sfence" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static struct dumb_fb make_dumb(int fd, uint32_t w, uint32_t h, uint32_t bpp)
{
    struct dumb_fb out = {0};
    struct drm_mode_create_dumb c = {
        .width = w,
        .height = h,
        .bpp = bpp,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0)
        die("DRM_IOCTL_MODE_CREATE_DUMB");

    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0)
        die("DRM_IOCTL_MODE_MAP_DUMB");

    void *map = mmap(NULL, c.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, m.offset);
    if (map == MAP_FAILED) die("mmap dumb");

    out.handle = c.handle;
    out.pitch = c.pitch;
    out.size = c.size;
    out.map = map;
    return out;
}

static struct dumb_fb make_nv12_fb(int fd, uint32_t w, uint32_t h)
{
    /* One linear dumb object: Y occupies pitch*h, UV occupies pitch*h/2. */
    struct dumb_fb out = make_dumb(fd, w, h + h / 2u, 8);
    uint32_t handles[4] = { out.handle, out.handle, 0, 0 };
    uint32_t pitches[4] = { out.pitch, out.pitch, 0, 0 };
    uint32_t offsets[4] = { 0, out.pitch * h, 0, 0 };

    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_NV12,
                      handles, pitches, offsets, &out.fb_id, 0) != 0)
        die("drmModeAddFB2 NV12");
    return out;
}

static int crtc_index(drmModeRes *res, uint32_t crtc_id)
{
    for (int i = 0; i < res->count_crtcs; i++)
        if (res->crtcs[i] == crtc_id) return i;
    return -1;
}

static drmModeConnector *find_connected(int fd, drmModeRes *res)
{
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
            return c;
        drmModeFreeConnector(c);
    }
    return NULL;
}

static int open_connected_drm(const char *requested, char *chosen, size_t chosen_sz)
{
    if (requested) {
        int fd = open(requested, O_RDWR | O_CLOEXEC);
        if (fd < 0) return -1;
        snprintf(chosen, chosen_sz, "%s", requested);
        return fd;
    }

    for (int i = 0; i < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) { close(fd); continue; }
        drmModeConnector *c = find_connected(fd, res);
        if (c) {
            drmModeFreeConnector(c);
            drmModeFreeResources(res);
            snprintf(chosen, chosen_sz, "%s", path);
            return fd;
        }
        drmModeFreeResources(res);
        close(fd);
    }
    errno = ENOENT;
    return -1;
}

static drmModeConnector *find_connector_id(int fd, drmModeRes *res, uint32_t id)
{
    for (int i = 0; i < res->count_connectors; i++) {
        if (res->connectors[i] != id) continue;
        return drmModeGetConnector(fd, id);
    }
    return NULL;
}

static int mode_refresh(const drmModeModeInfo *mode)
{
    if (mode->vrefresh > 0) return mode->vrefresh;
    if (!mode->htotal || !mode->vtotal) return 0;
    return (int)(((uint64_t)mode->clock * 1000u +
                  ((uint64_t)mode->htotal * mode->vtotal) / 2u) /
                 ((uint64_t)mode->htotal * mode->vtotal));
}

static drmModeModeInfo choose_mode(drmModeConnector *c, uint32_t width,
                                   uint32_t height, uint32_t refresh_hz)
{
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].hdisplay == width && c->modes[i].vdisplay == height &&
            (uint32_t)mode_refresh(&c->modes[i]) == refresh_hz)
            return c->modes[i];
    for (int i = 0; i < c->count_modes; i++)
        if ((c->modes[i].type & DRM_MODE_TYPE_PREFERRED) &&
            c->modes[i].hdisplay == width && c->modes[i].vdisplay == height)
            return c->modes[i];
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].hdisplay == width && c->modes[i].vdisplay == height)
            return c->modes[i];
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED)
            return c->modes[i];
    return c->modes[0];
}

static drmModeModeInfo preferred_mode(drmModeConnector *c)
{
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) return c->modes[i];
    return c->modes[0];
}

static uint32_t choose_unused_crtc(int fd, drmModeRes *res,
                                   drmModeConnector *c, uint32_t used)
{
    for (int ei = 0; ei < c->count_encoders; ei++) {
        drmModeEncoder *e = drmModeGetEncoder(fd, c->encoders[ei]);
        if (!e) continue;
        for (int i = 0; i < res->count_crtcs && i < 32; i++) {
            if ((e->possible_crtcs & (1u << i)) && !(used & (1u << i))) {
                uint32_t id = res->crtcs[i];
                drmModeFreeEncoder(e);
                return id;
            }
        }
        drmModeFreeEncoder(e);
    }
    return 0;
}

int nd_video_open_displays(const char *requested_drm, int *master_fd,
                           char *drm_path, size_t path_size,
                           struct nd_local_display *out, int capacity)
{
    if (!master_fd || !out || capacity <= 0) { errno = EINVAL; return -1; }
    int fd = open_connected_drm(requested_drm, drm_path, path_size);
    if (fd < 0) return -1;
    if (drmSetMaster(fd) < 0) { int e = errno; close(fd); errno = e; return -1; }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    (void)drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { int e = errno; close(fd); errno = e; return -1; }
    uint32_t used = 0;
    int count = 0;
    for (int i = 0; i < res->count_connectors && count < capacity; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection != DRM_MODE_CONNECTED || c->count_modes <= 0) {
            drmModeFreeConnector(c);
            continue;
        }
        uint32_t crtc = choose_unused_crtc(fd, res, c, used);
        int ci = crtc_index(res, crtc);
        if (!crtc || ci < 0) { drmModeFreeConnector(c); continue; }
        drmModeModeInfo mode = preferred_mode(c);
        struct nd_local_display *d = &out[count++];
        memset(d, 0, sizeof(*d));
        d->connector_id = c->connector_id;
        d->crtc_id = crtc;
        snprintf(d->connector, sizeof(d->connector), "%s-%u",
                 drmModeGetConnectorTypeName(c->connector_type),
                 c->connector_type_id);
        d->width = (uint16_t)mode.hdisplay;
        d->height = (uint16_t)mode.vdisplay;
        int hz = mode_refresh(&mode);
        d->refresh_hz = (uint16_t)(hz > 0 ? hz : 60);
        used |= 1u << ci;
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    if (!count) { close(fd); errno = ENODEV; return -1; }
    *master_fd = fd;
    return count;
}

static int plane_has_format(drmModePlane *p, uint32_t format)
{
    for (uint32_t i = 0; i < p->count_formats; i++)
        if (p->formats[i] == format) return 1;
    return 0;
}

static int plane_property(int fd, uint32_t plane_id, const char *name,
                          uint64_t *value, uint64_t *minv, uint64_t *maxv,
                          int *immutable, uint32_t *prop_id)
{
    drmModeObjectProperties *ops =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!ops) return 0;

    int found = 0;
    for (uint32_t i = 0; i < ops->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, ops->props[i]);
        if (!pr) continue;
        if (!strcmp(pr->name, name)) {
            if (prop_id) *prop_id = pr->prop_id;
            if (value) *value = ops->prop_values[i];
            if (immutable) *immutable = !!(pr->flags & DRM_MODE_PROP_IMMUTABLE);
            if ((pr->flags & DRM_MODE_PROP_RANGE) && pr->count_values >= 2) {
                if (minv) *minv = pr->values[0];
                if (maxv) *maxv = pr->values[1];
            }
            found = 1;
            drmModeFreeProperty(pr);
            break;
        }
        drmModeFreeProperty(pr);
    }
    drmModeFreeObjectProperties(ops);
    return found;
}

static const char *plane_type_name(uint64_t t)
{
    switch (t) {
    case DRM_PLANE_TYPE_PRIMARY: return "primary";
    case DRM_PLANE_TYPE_OVERLAY: return "overlay";
    case DRM_PLANE_TYPE_CURSOR: return "cursor";
    default: return "unknown";
    }
}

static int candidate_better(const struct plane_candidate *a,
                            const struct plane_candidate *b)
{
    if (a->have_zpos != b->have_zpos)
        return a->have_zpos > b->have_zpos;
    if (a->have_zpos && a->zpos != b->zpos)
        return a->zpos > b->zpos;
    if ((a->type == DRM_PLANE_TYPE_OVERLAY) !=
        (b->type == DRM_PLANE_TYPE_OVERLAY))
        return a->type == DRM_PLANE_TYPE_OVERLAY;
    return a->id > b->id;
}

static int set_plane_property(int fd, uint32_t plane_id, const char *name,
                              uint64_t value)
{
    uint32_t prop_id = 0;
    if (!plane_property(fd, plane_id, name, NULL, NULL, NULL, NULL, &prop_id))
        return 0;
    if (drmModeObjectSetProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                 prop_id, value) != 0)
        return -1;
    return 1;
}

static int connector_property_id(int fd, uint32_t connector_id,
                                 const char *name, uint32_t *prop_id)
{
    drmModeObjectProperties *ops =
        drmModeObjectGetProperties(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!ops) return 0;
    int found = 0;
    for (uint32_t i = 0; i < ops->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, ops->props[i]);
        if (!pr) continue;
        if (!strcmp(pr->name, name)) {
            *prop_id = pr->prop_id;
            found = 1;
            drmModeFreeProperty(pr);
            break;
        }
        drmModeFreeProperty(pr);
    }
    drmModeFreeObjectProperties(ops);
    return found;
}

struct drm_display_control {
    pthread_mutex_t mutex;
    int fd;
    int state_fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t black_fb;
    struct dumb_fb *video;
    drmModeModeInfo mode;
    uint32_t stream_width;
    uint32_t stream_height;
    int front;
    int dpms_on;
};

static int set_connector_dpms(struct drm_display_control *c, int on)
{
    uint32_t prop_id;
    if (!connector_property_id(c->fd, c->connector_id, "DPMS", &prop_id))
        return 0;
    uint64_t value = on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
    if (drmModeConnectorSetProperty(c->fd, c->connector_id,
                                    prop_id, value) != 0)
        return -1;
    return 1;
}

static void apply_dpms(struct drm_display_control *c, int on)
{
    pthread_mutex_lock(&c->mutex);
    if (on == c->dpms_on) {
        pthread_mutex_unlock(&c->mutex);
        return;
    }
    int property = set_connector_dpms(c, on);
    if (property < 0)
        perror("DRM connector DPMS");
    if (property <= 0) {
        if (!on) {
            if (drmModeSetCrtc(c->fd, c->crtc_id, 0, 0, 0,
                               NULL, 0, NULL) != 0)
                perror("DRM DPMS-off modeset");
        } else {
            uint32_t connector = c->connector_id;
            if (drmModeSetCrtc(c->fd, c->crtc_id, c->black_fb, 0, 0,
                               &connector, 1, &c->mode) != 0) {
                perror("DRM DPMS-on modeset");
            } else if (drmModeSetPlane(c->fd, c->plane_id, c->crtc_id,
                                       c->video[c->front].fb_id, 0,
                                       0, 0, c->mode.hdisplay,
                                       c->mode.vdisplay, 0, 0,
                                       c->stream_width << 16,
                                       c->stream_height << 16) != 0) {
                perror("DRM DPMS-on plane restore");
            }
        }
    }
    c->dpms_on = on;
    fprintf(stderr, "receiver DPMS: %s\n", on ? "on" : "off");
    pthread_mutex_unlock(&c->mutex);
}

static void *display_control_thread(void *opaque)
{
    struct drm_display_control *c = opaque;
    for (;;) {
        struct ndc_display_state state;
        int r = ndc_read_full(c->state_fd, &state, sizeof(state));
        if (r <= 0) break;
        uint32_t flags = ntohl(state.flags);
        if (flags & NDC_DISPLAY_HAS_DPMS)
            apply_dpms(c, !!(flags & NDC_DISPLAY_DPMS_ON));
    }
    return NULL;
}

static void force_plane_visible(int fd, const struct plane_candidate *p)
{
    if (p->have_zpos && !p->zpos_immutable && p->zmax > p->zpos) {
        if (set_plane_property(fd, p->id, "zpos", p->zmax) < 0)
            perror("setting overlay zpos");
        else
            fprintf(stderr, "set plane %u zpos to %llu\n", p->id,
                    (unsigned long long)p->zmax);
    }

    uint64_t alpha = 0, amin = 0, amax = 0;
    int immutable = 0;
    if (plane_property(fd, p->id, "alpha", &alpha, &amin, &amax,
                       &immutable, NULL) && !immutable && alpha != amax) {
        if (set_plane_property(fd, p->id, "alpha", amax) < 0)
            perror("setting overlay alpha");
    }
}

static uint32_t choose_nv12_plane(int fd, uint32_t crtc_id, int crtc_idx,
                                  uint32_t test_fb, const drmModeModeInfo *mode,
                                  uint32_t width, uint32_t height)
{
    drmModePlaneRes *prs = drmModeGetPlaneResources(fd);
    if (!prs) die("drmModeGetPlaneResources");

    struct plane_candidate best = {0};
    int have_best = 0;
    fprintf(stderr, "NV12 planes usable on this CRTC:\n");

    for (uint32_t i = 0; i < prs->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(fd, prs->planes[i]);
        if (!p) continue;
        if (!(p->possible_crtcs & (1u << crtc_idx)) ||
            !plane_has_format(p, DRM_FORMAT_NV12) ||
            (p->crtc_id && p->crtc_id != crtc_id)) {
            drmModeFreePlane(p);
            continue;
        }

        uint64_t type = UINT64_MAX, zpos = 0, zmin = 0, zmax = 0;
        int immutable = 0;
        int have_type = plane_property(fd, p->plane_id, "type",
                                       &type, NULL, NULL, NULL, NULL);
        int have_zpos = plane_property(fd, p->plane_id, "zpos",
                                       &zpos, &zmin, &zmax, &immutable, NULL);
        fprintf(stderr, "  plane %u: type=%s", p->plane_id,
                have_type ? plane_type_name(type) : "?");
        if (have_zpos)
            fprintf(stderr, ", zpos=%llu range=%llu..%llu%s",
                    (unsigned long long)zpos,
                    (unsigned long long)zmin,
                    (unsigned long long)zmax,
                    immutable ? " immutable" : "");
        fputc('\n', stderr);

        if (have_type && type == DRM_PLANE_TYPE_PRIMARY) {
            drmModeFreePlane(p);
            continue;
        }

        struct plane_candidate c = {
            .id = p->plane_id,
            .type = have_type ? type : UINT64_MAX,
            .zpos = zpos,
            .zmax = zmax,
            .have_zpos = have_zpos,
            .zpos_immutable = immutable,
        };
        if (!have_best || candidate_better(&c, &best)) {
            best = c;
            have_best = 1;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(prs);
    if (!have_best) return 0;

    force_plane_visible(fd, &best);
    if (drmModeSetPlane(fd, best.id, crtc_id, test_fb, 0,
                        0, 0, mode->hdisplay, mode->vdisplay,
                        0, 0, width << 16, height << 16) != 0) {
        perror("drmModeSetPlane NV12");
        return 0;
    }
    fprintf(stderr, "using NV12 overlay plane %u\n", best.id);
    return best.id;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    (void)ctx;
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == hw_pix_fmt) return *p;
    fprintf(stderr, "decoder did not offer VAAPI format\n");
    return AV_PIX_FMT_NONE;
}

static AVCodecContext *init_decoder(const char *va_path)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "H.264 decoder not found\n");
        exit(EXIT_FAILURE);
    }

    int ret = av_hwdevice_ctx_create(&va_device, AV_HWDEVICE_TYPE_VAAPI,
                                     va_path, NULL, 0);
    if (ret < 0) ff_die("av_hwdevice_ctx_create(VAAPI)", ret);

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) die("avcodec_alloc_context3 decoder");
    dec->get_format = get_hw_format;
    dec->hw_device_ctx = av_buffer_ref(va_device);
    dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec->thread_count = 1;
    dec->thread_type = 0;

    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) ff_die("avcodec_open2(H.264)", ret);
    return dec;
}

static void copy_decoded_to_nv12(const AVFrame *src, struct dumb_fb *dst,
                                 uint32_t width, uint32_t height)
{
    uint8_t *dy = dst->map;
    uint8_t *duv = dst->map + (size_t)dst->pitch * height;

    if (src->width != (int)width || src->height != (int)height) {
        fprintf(stderr, "decoded size %dx%d, expected %ux%u\n",
                src->width, src->height, width, height);
        exit(EXIT_FAILURE);
    }

    if (src->format == AV_PIX_FMT_NV12) {
        for (uint32_t y = 0; y < height; y++)
            memcpy(dy + (size_t)y * dst->pitch,
                   src->data[0] + (size_t)y * src->linesize[0], width);
        for (uint32_t y = 0; y < height / 2u; y++)
            memcpy(duv + (size_t)y * dst->pitch,
                   src->data[1] + (size_t)y * src->linesize[1], width);
    } else if (src->format == AV_PIX_FMT_YUV420P) {
        for (uint32_t y = 0; y < height; y++)
            memcpy(dy + (size_t)y * dst->pitch,
                   src->data[0] + (size_t)y * src->linesize[0], width);
        for (uint32_t y = 0; y < height / 2u; y++) {
            uint8_t *d = duv + (size_t)y * dst->pitch;
            const uint8_t *u = src->data[1] + (size_t)y * src->linesize[1];
            const uint8_t *v = src->data[2] + (size_t)y * src->linesize[2];
            for (uint32_t x = 0; x < width / 2u; x++) {
                d[2u*x] = u[x];
                d[2u*x + 1u] = v[x];
            }
        }
    } else {
        const char *n = av_get_pix_fmt_name((enum AVPixelFormat)src->format);
        fprintf(stderr, "unsupported transferred pixel format %s (%d)\n",
                n ? n : "?", src->format);
        exit(EXIT_FAILURE);
    }
    scanout_store_fence();
}

static int decode_one(AVCodecContext *dec, const uint8_t *data, uint32_t size,
                      uint32_t seq, AVFrame *hw, AVFrame *sw)
{
    AVPacket pkt = {0};
    pkt.data = (uint8_t *)data;
    pkt.size = (int)size;
    pkt.pts = pkt.dts = seq;

    int ret = avcodec_send_packet(dec, &pkt);
    if (ret < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret, b, sizeof(b));
        fprintf(stderr, "decode send seq=%u: %s\n", seq, b);
        avcodec_flush_buffers(dec);
        return -1;
    }

    av_frame_unref(hw);
    ret = avcodec_receive_frame(dec, hw);
    if (ret < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret, b, sizeof(b));
        fprintf(stderr, "decode receive seq=%u: %s\n", seq, b);
        avcodec_flush_buffers(dec);
        return -1;
    }
    if (hw->format != hw_pix_fmt) {
        fprintf(stderr, "decoder unexpectedly returned software frame format %d\n",
                hw->format);
        return -1;
    }

    av_frame_unref(sw);
    ret = av_hwframe_transfer_data(sw, hw, 0);
    if (ret < 0) {
        char b[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret, b, sizeof(b));
        fprintf(stderr, "VAAPI transfer seq=%u: %s\n", seq, b);
        return -1;
    }
    return 0;
}

static int seq_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}


struct frame_mailbox {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint8_t *latest_buf;
    uint32_t latest_size;
    uint32_t latest_seq;
    uint64_t latest_session;
    int have_latest;
};

struct rx_stats {
    _Atomic unsigned long long complete;
    _Atomic unsigned long long partial_drop;
    _Atomic unsigned long long latest_overwrite;
    _Atomic unsigned long long bad;
    _Atomic unsigned long long dup;
};

struct rx_ctx {
    int sock;
    struct frame_mailbox *mailbox;
    uint8_t *assembly_buf;
    struct rx_stats *stats;
    int encrypted;
    uint8_t key[ND_KEY_SIZE];
};

static void mailbox_publish(struct rx_ctx *rx, uint32_t size,
                            uint32_t seq, uint64_t session)
{
    struct frame_mailbox *m = rx->mailbox;

    pthread_mutex_lock(&m->mutex);

    /* There is exactly one pending completed frame. If decode has not taken
     * it yet, replace it with the newer one. No completed-frame FIFO exists. */
    if (m->have_latest)
        atomic_fetch_add_explicit(&rx->stats->latest_overwrite, 1,
                                  memory_order_relaxed);

    uint8_t *old_latest = m->latest_buf;
    m->latest_buf = rx->assembly_buf;
    rx->assembly_buf = old_latest;

    m->latest_size = size;
    m->latest_seq = seq;
    m->latest_session = session;
    m->have_latest = 1;

    pthread_cond_signal(&m->cond);
    pthread_mutex_unlock(&m->mutex);
}

static void *rx_thread_main(void *opaque)
{
    struct rx_ctx *rx = opaque;
    struct rx_stats *st = rx->stats;

    uint64_t got[(ND_MAX_FRAGS + 63u) / 64u];
    memset(got, 0, sizeof(got));

    uint64_t current_session = 0;
    uint64_t previous_session = 0;
    uint32_t current_seq = 0;
    uint32_t current_size = 0;
    uint16_t current_frag_count = 0;
    uint32_t got_count = 0;
    int assembling = 0;

    uint8_t datagram[ND_UDP_PAYLOAD_MAX];

    for (;;) {
        ssize_t n;
        do {
            n = recv(rx->sock, datagram, sizeof(datagram), MSG_TRUNC);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            perror("recv");
            return NULL;
        }
        if (n < (ssize_t)sizeof(struct nd_hdr) ||
            n > (ssize_t)sizeof(datagram)) {
            atomic_fetch_add_explicit(&st->bad, 1, memory_order_relaxed);
            continue;
        }

        struct nd_hdr h;
        memcpy(&h, datagram, sizeof(h));
        if (ntohl(h.magic) != ND_MAGIC) {
            atomic_fetch_add_explicit(&st->bad, 1, memory_order_relaxed);
            continue;
        }

        uint64_t sess = nd_ntoh64(h.session);
        uint32_t seq = ntohl(h.seq);
        uint16_t frag = ntohs(h.frag);
        uint16_t frag_count = ntohs(h.frag_count);
        uint32_t frame_size = ntohl(h.frame_size);

        if (!sess || !frag_count || frame_size == 0 ||
            frame_size > (rx->encrypted ? ND_MAX_WIRE_FRAME : ND_MAX_FRAME)) {
            atomic_fetch_add_explicit(&st->bad, 1, memory_order_relaxed);
            continue;
        }

        uint32_t expected_count =
            (frame_size + ND_FRAG_DATA - 1u) / ND_FRAG_DATA;
        if (expected_count != frag_count || frag >= frag_count) {
            atomic_fetch_add_explicit(&st->bad, 1, memory_order_relaxed);
            continue;
        }

        size_t off = (size_t)frag * ND_FRAG_DATA;
        size_t want = frame_size - off;
        if (want > ND_FRAG_DATA)
            want = ND_FRAG_DATA;
        if ((size_t)n != sizeof(h) + want) {
            atomic_fetch_add_explicit(&st->bad, 1, memory_order_relaxed);
            continue;
        }

        /* Session IDs are opaque, not ordered. Remember the immediately
         * retired session so late UDP packets from a restarted sender cannot
         * switch us back to the old stream. */
        if (current_session == 0 ||
            (sess != current_session && sess != previous_session)) {
            if (assembling && got_count != current_frag_count)
                atomic_fetch_add_explicit(&st->partial_drop, 1,
                                          memory_order_relaxed);
            previous_session = current_session;
            current_session = sess;
            current_seq = seq;
            current_size = frame_size;
            current_frag_count = frag_count;
            got_count = 0;
            memset(got, 0, sizeof(got));
            assembling = 1;
            fprintf(stderr, "new sender session %016llx seq=%u\n",
                    (unsigned long long)sess, seq);
        } else if (sess == previous_session) {
            continue;
        } else if (!assembling) {
            if (!seq_newer(seq, current_seq))
                continue;
            current_seq = seq;
            current_size = frame_size;
            current_frag_count = frag_count;
            got_count = 0;
            memset(got, 0, sizeof(got));
            assembling = 1;
        } else if (seq != current_seq) {
            if (!seq_newer(seq, current_seq))
                continue;

            /* The instant a newer IDR appears, an incomplete older IDR is
             * worthless. Drop it instead of waiting for missing fragments. */
            if (got_count != current_frag_count)
                atomic_fetch_add_explicit(&st->partial_drop, 1,
                                          memory_order_relaxed);

            current_seq = seq;
            current_size = frame_size;
            current_frag_count = frag_count;
            got_count = 0;
            memset(got, 0, sizeof(got));
        } else if (frame_size != current_size ||
                   frag_count != current_frag_count) {
            atomic_fetch_add_explicit(&st->bad, 1, memory_order_relaxed);
            continue;
        }

        uint64_t mask = 1ULL << (frag & 63u);
        uint64_t *word = &got[frag >> 6];
        if (*word & mask) {
            atomic_fetch_add_explicit(&st->dup, 1, memory_order_relaxed);
            continue;
        }

        memcpy(rx->assembly_buf + off, datagram + sizeof(h), want);
        *word |= mask;
        got_count++;

        if (got_count == current_frag_count) {
            if (rx->encrypted) {
                size_t plain_size = 0;
                if (nd_crypto_decrypt(rx->assembly_buf, &plain_size,
                                      rx->assembly_buf, current_size, rx->key,
                                      current_session, current_seq) < 0 ||
                    plain_size == 0 || plain_size > ND_MAX_FRAME) {
                    atomic_fetch_add_explicit(&st->bad, 1,
                                              memory_order_relaxed);
                    assembling = 0;
                    continue;
                }
                current_size = (uint32_t)plain_size;
            }
            memset(rx->assembly_buf + current_size, 0,
                   AV_INPUT_BUFFER_PADDING_SIZE);
            atomic_fetch_add_explicit(&st->complete, 1,
                                      memory_order_relaxed);

            mailbox_publish(rx, current_size, current_seq, current_session);
            assembling = 0;
        }
    }
}

int nd_video_receiver_run(int port, int fd, uint32_t connector_id,
                          uint32_t requested_crtc, const char *va_path,
                          const char *interface_name, int ready_fd, int width,
                          int height, int refresh_hz, int state_fd,
                          int encrypted, int key_fd)
{
    if (port <= 0 || port > 65535 ||
        width <= 0 || width > UINT16_MAX || (width & 1) ||
        height <= 0 || height > UINT16_MAX || (height & 1) ||
        refresh_hz <= 0 || refresh_hz > UINT16_MAX) {
        fprintf(stderr, "invalid built-in video receiver parameters\n");
        return 2;
    }
    uint32_t stream_width = (uint32_t)width;
    uint32_t stream_height = (uint32_t)height;
    if (va_path && !*va_path) va_path = NULL;
    if (fd < 0 || !connector_id || !requested_crtc) return 2;

    drmModeRes *res = drmModeGetResources(fd);
    if (!res)
        die("drmModeGetResources");
    drmModeConnector *conn = find_connector_id(fd, res, connector_id);
    if (!conn) {
        fprintf(stderr, "DRM connector %u is no longer available\n", connector_id);
        return 1;
    }

    drmModeModeInfo mode = choose_mode(conn, stream_width, stream_height,
                                       (uint32_t)refresh_hz);
    if (mode.hdisplay != stream_width || mode.vdisplay != stream_height) {
        fprintf(stderr,
                "warning: panel mode is %ux%u; stream is %ux%u (plane will scale)\n",
                mode.hdisplay, mode.vdisplay, stream_width, stream_height);
    }

    int crtc_idx = -1;
    uint32_t crtc_id = requested_crtc;
    crtc_idx = crtc_index(res, crtc_id);
    if (crtc_idx < 0) {
        fprintf(stderr, "no usable CRTC\n");
        return 1;
    }

    struct dumb_fb black = make_dumb(fd, mode.hdisplay, mode.vdisplay, 32);
    memset(black.map, 0, black.size);
    if (drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32,
                     black.pitch, black.handle, &black.fb_id) != 0)
        die("drmModeAddFB black");

    uint32_t conn_id = conn->connector_id;
    if (drmModeSetCrtc(fd, crtc_id, black.fb_id, 0, 0,
                       &conn_id, 1, &mode) != 0)
        die("drmModeSetCrtc");

    struct dumb_fb video[2];
    video[0] = make_nv12_fb(fd, stream_width, stream_height);
    video[1] = make_nv12_fb(fd, stream_width, stream_height);
    for (int i = 0; i < 2; i++) {
        memset(video[i].map, 16, (size_t)video[i].pitch * stream_height);
        memset(video[i].map + (size_t)video[i].pitch * stream_height,
               128, (size_t)video[i].pitch * (stream_height / 2u));
    }
    scanout_store_fence();

    uint32_t plane_id = choose_nv12_plane(fd, crtc_id, crtc_idx,
                                           video[0].fb_id, &mode,
                                           stream_width, stream_height);
    if (!plane_id) {
        fprintf(stderr, "no usable NV12 overlay plane; run modetest -p\n");
        return 1;
    }

    struct drm_display_control display_control = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .fd = fd,
        .state_fd = state_fd,
        .connector_id = conn_id,
        .crtc_id = crtc_id,
        .plane_id = plane_id,
        .black_fb = black.fb_id,
        .video = video,
        .mode = mode,
        .stream_width = stream_width,
        .stream_height = stream_height,
        .front = 0,
        .dpms_on = 1,
    };
    pthread_t display_thread;
    int display_error = pthread_create(&display_thread, NULL,
                                       display_control_thread,
                                       &display_control);
    if (display_error != 0) {
        errno = display_error;
        die("pthread_create display control");
    }
    pthread_detach(display_thread);

    AVCodecContext *dec = init_decoder(va_path);
    AVFrame *hw = av_frame_alloc();
    AVFrame *sw = av_frame_alloc();
    if (!hw || !sw)
        die("av_frame_alloc");

    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        die("socket");

    if (interface_name && *interface_name &&
        setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, interface_name,
                   strlen(interface_name) + 1) < 0)
        die("SO_BINDTODEVICE video");

    /* The receive thread drains this continuously. Keep enough burst room for
     * scheduling jitter, but not enough for seconds of stale video. Linux
     * normally reports roughly 2x this requested value. */
    int rcvbuf = 256 * 1024;
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
        die("SO_RCVBUF");

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind");

    socklen_t sl = sizeof(rcvbuf);
    if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &sl) == 0)
        fprintf(stderr, "UDP SO_RCVBUF actual=%d bytes\n", rcvbuf);

    /* Three compressed buffers, but never a queue:
     *   assembly_buf: RX is currently filling it
     *   latest_buf:   newest completed IDR waiting for decode
     *   decode_buf:   decoder currently owns it
     * Pointer swaps make publication zero-copy. */
    uint8_t *assembly_buf =
        av_malloc((size_t)ND_MAX_WIRE_FRAME + AV_INPUT_BUFFER_PADDING_SIZE);
    uint8_t *latest_buf =
        av_malloc((size_t)ND_MAX_WIRE_FRAME + AV_INPUT_BUFFER_PADDING_SIZE);
    uint8_t *decode_buf =
        av_malloc((size_t)ND_MAX_WIRE_FRAME + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!assembly_buf || !latest_buf || !decode_buf)
        die("av_malloc compressed buffers");

    struct frame_mailbox mailbox = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .latest_buf = latest_buf,
        .latest_size = 0,
        .latest_seq = 0,
        .latest_session = 0,
        .have_latest = 0,
    };

    struct rx_stats stats = {0};
    struct rx_ctx rx = {
        .sock = s,
        .mailbox = &mailbox,
        .assembly_buf = assembly_buf,
        .stats = &stats,
        .encrypted = encrypted,
    };
    if (encrypted) {
        if (key_fd < 0 || ndc_read_full(key_fd, rx.key, sizeof(rx.key)) <= 0)
            die("read video key");
        close(key_fd);
    }

    pthread_t rx_thread;
    int perr = pthread_create(&rx_thread, NULL, rx_thread_main, &rx);
    if (perr != 0) {
        errno = perr;
        die("pthread_create RX");
    }

    fprintf(stderr,
            "listening port %d for %ux%u@%d encryption=%s; RX runs continuously; only newest complete IDR is decoded\n",
            port, stream_width, stream_height, refresh_hz,
            encrypted ? "on" : "off");

    if (ready_fd >= 0) {
        const uint8_t ready = 1;
        ssize_t wr;
        do { wr = write(ready_fd, &ready, 1); } while (wr < 0 && errno == EINTR);
        close(ready_fd);
        ready_fd = -1;
    }

    unsigned long long displayed = 0;
    unsigned long long skipped_before_decode = 0;
    uint64_t last_session = 0;
    uint32_t last_seq = 0;
    int have_last_seq = 0;

    struct timespec stat0;
    clock_gettime(CLOCK_MONOTONIC, &stat0);
    unsigned long long last_complete = 0;
    unsigned long long last_partial = 0;
    unsigned long long last_overwrite = 0;
    unsigned long long last_bad = 0;
    unsigned long long last_dup = 0;
    unsigned long long last_displayed = 0;
    unsigned long long last_skipped = 0;

    for (;;) {
        uint32_t size, seq;
        uint64_t session;

        pthread_mutex_lock(&mailbox.mutex);
        while (!mailbox.have_latest)
            pthread_cond_wait(&mailbox.cond, &mailbox.mutex);

        /* Swap decoder ownership with the single latest slot. RX can
         * immediately publish/overwrite another completed frame while decode
         * works on this one. */
        uint8_t *tmp = decode_buf;
        decode_buf = mailbox.latest_buf;
        mailbox.latest_buf = tmp;

        size = mailbox.latest_size;
        seq = mailbox.latest_seq;
        session = mailbox.latest_session;
        mailbox.have_latest = 0;
        pthread_mutex_unlock(&mailbox.mutex);

        if (have_last_seq && session == last_session && seq_newer(seq, last_seq)) {
            uint32_t delta = seq - last_seq;
            if (delta > 1)
                skipped_before_decode += (unsigned long long)(delta - 1u);
        }
        last_session = session;
        last_seq = seq;
        have_last_seq = 1;

        if (decode_one(dec, decode_buf, size, seq, hw, sw) == 0) {
            pthread_mutex_lock(&display_control.mutex);
            int back = display_control.front ^ 1;
            copy_decoded_to_nv12(sw, &video[back], stream_width, stream_height);
            if (!display_control.dpms_on) {
                display_control.front = back;
            } else if (drmModeSetPlane(fd, plane_id, crtc_id, video[back].fb_id, 0,
                                0, 0, mode.hdisplay, mode.vdisplay,
                                0, 0, stream_width << 16, stream_height << 16) != 0) {
                perror("drmModeSetPlane frame");
            } else {
                display_control.front = back;
                displayed++;
            }
            pthread_mutex_unlock(&display_control.mutex);
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (double)(now.tv_sec - stat0.tv_sec) +
                    (double)(now.tv_nsec - stat0.tv_nsec) / 1e9;
        if (dt >= 1.0) {
            unsigned long long complete =
                atomic_load_explicit(&stats.complete, memory_order_relaxed);
            unsigned long long partial =
                atomic_load_explicit(&stats.partial_drop, memory_order_relaxed);
            unsigned long long overwrite =
                atomic_load_explicit(&stats.latest_overwrite, memory_order_relaxed);
            unsigned long long bad =
                atomic_load_explicit(&stats.bad, memory_order_relaxed);
            unsigned long long dup =
                atomic_load_explicit(&stats.dup, memory_order_relaxed);

            fprintf(stderr,
                    "rx %.1f complete/s  displayed %.1f/s  latest-dropped %llu  partial-dropped %llu  seq-skipped %llu  bad %llu  dup %llu\n",
                    (complete - last_complete) / dt,
                    (displayed - last_displayed) / dt,
                    overwrite - last_overwrite,
                    partial - last_partial,
                    skipped_before_decode - last_skipped,
                    bad - last_bad,
                    dup - last_dup);

            last_complete = complete;
            last_displayed = displayed;
            last_partial = partial;
            last_overwrite = overwrite;
            last_skipped = skipped_before_decode;
            last_bad = bad;
            last_dup = dup;
            stat0 = now;
        }
    }
}
