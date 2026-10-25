// SPDX-License-Identifier: GPL-2.0-only
// Real encoding, production TX, Linux/iOS reassembly and loss-aware decoding.
#include "common.h"
#include "encoder.h"
#include "frame_transport.h"
#include "video_tx.h"
#include "NDNative.h"
#include <libavutil/imgutils.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { WIDTH = 320, HEIGHT = 240, FPS = 60, FRAMES = 48 };

static AVCodecContext *decoder(enum AVCodecID id) {
  const AVCodec *codec = avcodec_find_decoder(id);
  CHECK(codec);
  AVCodecContext *dec = avcodec_alloc_context3(codec);
  CHECK(dec && avcodec_open2(dec, codec, NULL) >= 0);
  return dec;
}
static uint64_t decode(AVCodecContext *dec, const uint8_t *data, size_t size) {
  AVPacket packet = {0};
  packet.data = (uint8_t *)data;
  packet.size = (int)size;
  AVFrame *frame = av_frame_alloc();
  CHECK(frame && avcodec_send_packet(dec, &packet) >= 0);
  CHECK(avcodec_receive_frame(dec, frame) >= 0);
  CHECK(frame->width == WIDTH && frame->height == HEIGHT);
  CHECK(!(frame->flags & AV_FRAME_FLAG_CORRUPT) && !frame->decode_error_flags);
  uint64_t hash = 14695981039346656037ull;
  for (unsigned plane = 0; plane < 3; plane++)
    for (int y = 0; y < (plane ? HEIGHT / 2 : HEIGHT); y++)
      for (int x = 0; x < (plane ? WIDTH / 2 : WIDTH); x++)
        hash = (hash ^ frame->data[plane][y * frame->linesize[plane] + x]) * 1099511628211ull;
  av_frame_free(&frame);
  return hash;
}
static uint64_t run(const char *name, unsigned interval, int encrypted, int bandwidth, int early_key) {
  struct nd_encoder encoder;
  struct nd_encoder_rate rate = {bandwidth ? ND_RATE_BANDWIDTH : ND_RATE_QP, 23, ND_DEFAULT_BITRATE_KBPS};
  CHECK(nd_encoder_open(&encoder, name, WIDTH, HEIGHT, FPS, interval, &rate, AV_PIX_FMT_YUV420P, 0) >= 0);
  AVFrame *frame = av_frame_alloc(), *hw = NULL;
  AVPacket *encoded = av_packet_alloc();
  CHECK(frame && encoded);
  frame->width = WIDTH; frame->height = HEIGHT; frame->format = encoder.sw_format;
  CHECK(av_frame_get_buffer(frame, 32) >= 0);
  CHECK(encoder.sw_format == AV_PIX_FMT_YUV420P || encoder.sw_format == AV_PIX_FMT_NV12);
  if (encoder.frames) {
    hw = av_frame_alloc();
    CHECK(hw && av_hwframe_get_buffer(encoder.frames, hw, 0) >= 0);
  }
  uint8_t key[ND_KEY_SIZE] = {7};
  uint8_t *buffer = calloc(1, ND_MAX_WIRE_FRAME + AV_INPUT_BUFFER_PADDING_SIZE);
  CHECK(buffer);
  int fd[2];
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fd) == 0);
  struct timeval timeout = {.tv_sec = 2};
  CHECK(setsockopt(fd[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
  uint64_t session = 100 + interval * 2 + encrypted;
  const uint8_t *stream_key = encrypted ? key : NULL;
  struct nd_video_tx *tx = nd_video_tx_open(fd[0], session, stream_key, FPS, interval > 1);
  NDIOSAssembler *ios = nd_ios_assembler_create(session, stream_key);
  struct nd_assembly assembly = {.expected_session = session};
  struct nd_rx_stats stats = {0};
  struct nd_decode_chain linux_chain = {0}, ios_chain = {0};
  AVCodecContext *reference = decoder(encoder.context->codec_id), *lossy = decoder(encoder.context->codec_id);
  CHECK(tx && ios);
  uint64_t bytes = 0;
  unsigned decoded = 0, rejected = 0;
  for (uint32_t seq = 0; seq < FRAMES; seq++) {
    CHECK(av_frame_make_writable(frame) >= 0);
    /* Textured stationary background and a moving window, rather than a
     * uniformly colored image that would exaggerate inter-frame savings. */
    for (int y = 0; y < HEIGHT; y++)
      for (int x = 0; x < WIDTH; x++) {
        int window = x >= (int)seq * 3 && x < (int)seq * 3 + 40 && y >= 80 && y < 140;
        frame->data[0][y * frame->linesize[0] + x] = window ? 180 : (uint8_t)(x * 13 + y * 7 + x * y % 251);
      }
    if (encoder.sw_format == AV_PIX_FMT_NV12) {
      for (int y = 0; y < HEIGHT / 2; y++) memset(frame->data[1] + y * frame->linesize[1], 128, WIDTH);
    } else {
      for (int plane = 1; plane < 3; plane++)
        for (int y = 0; y < HEIGHT / 2; y++) memset(frame->data[plane] + y * frame->linesize[plane], 128, WIDTH / 2);
    }
    frame->pts = seq;
    nd_encoder_prepare_frame(&encoder, frame, early_key && seq == 17);
    AVFrame *input = frame;
    if (hw) {
      CHECK(av_frame_make_writable(hw) >= 0);
      CHECK(av_hwframe_transfer_data(hw, frame, 0) >= 0);
      CHECK(av_frame_copy_props(hw, frame) >= 0);
      input = hw;
    }
    CHECK(avcodec_send_frame(encoder.context, input) >= 0);
    CHECK(avcodec_receive_packet(encoder.context, encoded) >= 0);
    unsigned origin = early_key && seq >= 17 ? 17 : 0;
    CHECK(!!(encoded->flags & AV_PKT_FLAG_KEY) == ((seq - origin) % interval == 0));
    uint64_t expected = decode(reference, encoded->data, (size_t)encoded->size);
    bytes += encoded->size;
    size_t wire_size = (size_t)encoded->size + (interval > 1 ? sizeof(struct nd_video_meta) : 0) + (encrypted ? 16 : 0);
    unsigned fragments = (unsigned)((wire_size + ND_FRAG_DATA - 1) / ND_FRAG_DATA);
    nd_video_tx_publish(tx, encoded, seq);
    CHECK(!encoded->data);
    int complete = 0;
    /* Lose a P-frame, an I-frame, and one fragment of another access unit.
     * Later dependent frames must never be passed to a decoder with stale refs. */
    for (unsigned frag = 0; frag < fragments; frag++) {
      uint8_t datagram[ND_UDP_PAYLOAD_MAX];
      ssize_t n = recv(fd[1], datagram, sizeof(datagram), 0);
      CHECK(n > 0);
      if (seq == 1 || seq == 2 * interval || (seq == 4 * interval + 1 && frag == 0)) continue;
      complete = nd_frame_receive(&assembly, &stats, buffer, datagram, (size_t)n, stream_key);
      CHECK(nd_ios_assembler_feed(ios, datagram, (size_t)n) == complete);
    }
    if (complete) {
      NDIOSStats received = nd_ios_assembler_stats(ios);
      CHECK(received.sequence == seq && received.frame_bytes == assembly.size);
      CHECK(received.reference == assembly.reference && received.keyframe == (unsigned)assembly.keyframe);
      CHECK(!memcmp(buffer + assembly.offset, nd_ios_assembler_bytes(ios), assembly.size));
      int accepted = nd_decode_chain_accept(&linux_chain, seq, assembly.reference, assembly.keyframe);
      CHECK(nd_decode_chain_accept(&ios_chain, seq, received.reference, received.keyframe) == accepted);
      if (accepted) {
        memset(buffer + assembly.offset + assembly.size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        CHECK(decode(lossy, buffer + assembly.offset, assembly.size) == expected);
        decoded++;
      } else rejected++;
      if (interval > 1 && seq == 2 * interval + 1) CHECK(!accepted);
      if (seq == 3 * interval) CHECK(accepted);
    }
    CHECK(avcodec_receive_packet(encoder.context, encoded) == AVERROR(EAGAIN));
  }
  CHECK(decoded > FRAMES / 2);
  CHECK(interval == 1 || rejected > 0);
  nd_video_tx_close(tx);
  nd_ios_assembler_destroy(ios);
  close(fd[0]); close(fd[1]);
  avcodec_free_context(&reference); avcodec_free_context(&lossy);
  av_packet_free(&encoded); av_frame_free(&frame); av_frame_free(&hw);
  nd_encoder_close(&encoder); free(buffer);
  return bytes;
}
int main(int argc, char **argv) {
  CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "bandwidth")));
  CHECK(nd_ios_init() >= 0);
  const char *name = getenv("ND_TEST_ENCODER");
  if (!name) name = "libx264";
  if (!avcodec_find_encoder_by_name(name)) { puts("SKIP: requested encoder unavailable"); return 77; }
  av_log_set_level(AV_LOG_ERROR);
  for (int encrypted = 0; encrypted <= 1; encrypted++) {
    uint64_t sizes[3];
    for (unsigned interval = 1; interval <= 3; interval++) sizes[interval - 1] = run(name, interval, encrypted, argc == 2, 0);
    if (argc == 1) CHECK(sizes[1] < sizes[0] && sizes[2] < sizes[1]);
    printf("%s %s %s: all-I=%llu IP=%llu IPP=%llu bytes (IPP %.1f%% smaller)\n", name,
           argc == 2 ? "bandwidth" : "QP23", encrypted ? "encrypted" : "plaintext",
           (unsigned long long)sizes[0], (unsigned long long)sizes[1], (unsigned long long)sizes[2],
           (1.0 - (double)sizes[2] / sizes[0]) * 100.0);
  }
  (void)run(name, 3, 0, argc == 2, 1);
  puts("PASS: immediate I/IP/IPP output, Linux/iOS metadata, lost keys/P-frames/fragments, early IDR and exact recovery pixels");
  return 0;
}
