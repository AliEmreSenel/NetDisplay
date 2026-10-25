// SPDX-License-Identifier: GPL-2.0-only
// Exercise the production encoder -> TX -> Linux/iOS RX path without a GPU.
#include "common.h"
#include "encoder.h"
#include "frame_transport.h"
#include "video_tx.h"
#include "NDNative.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void decode_independently(const uint8_t *bytes, size_t size) {
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  CHECK(codec);
  AVCodecContext *decoder = avcodec_alloc_context3(codec);
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  CHECK(decoder && packet && frame);
  CHECK(avcodec_open2(decoder, codec, NULL) >= 0);
  CHECK(av_new_packet(packet, (int)size) >= 0);
  memcpy(packet->data, bytes, size);
  CHECK(avcodec_send_packet(decoder, packet) >= 0);
  CHECK(avcodec_receive_frame(decoder, frame) >= 0);
  CHECK(frame->width == 320 && frame->height == 240);
  av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&decoder);
}

int main(void) {
  CHECK(nd_ios_init() >= 0);
  if (!avcodec_find_encoder_by_name("libx264")) {
    puts("SKIP: FFmpeg was built without libx264");
    return 77;
  }
  struct nd_encoder encoder;
  CHECK(nd_encoder_open(&encoder, "libx264", 320, 240, 60, 23,
                         AV_PIX_FMT_YUV420P, 1) >= 0);
  AVFrame *frame = av_frame_alloc();
  AVPacket *encoded = av_packet_alloc();
  CHECK(frame && encoded && encoder.sw_format == AV_PIX_FMT_YUV420P);
  frame->width = 320; frame->height = 240; frame->format = encoder.sw_format;
  CHECK(av_frame_get_buffer(frame, 32) >= 0);
  uint8_t key[ND_KEY_SIZE] = {7};
  uint8_t *buffer = malloc(ND_MAX_WIRE_FRAME);
  CHECK(buffer);
  for (int encrypted = 0; encrypted <= 1; encrypted++) {
    int fd[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fd) == 0);
    struct timeval timeout = {.tv_sec = 2};
    CHECK(setsockopt(fd[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    uint64_t session = (uint64_t)encrypted + 1;
    const uint8_t *stream_key = encrypted ? key : NULL;
    struct nd_video_tx *tx = nd_video_tx_open(fd[0], session, stream_key, 60);
    NDIOSAssembler *ios = nd_ios_assembler_create(session, stream_key);
    struct nd_assembly assembly = {.expected_session = session};
    struct nd_rx_stats stats = {0};
    CHECK(tx && ios);
    for (unsigned seq = 0; seq < 3; seq++) {
      CHECK(av_frame_make_writable(frame) >= 0);
      for (int y = 0; y < frame->height; y++)
        for (int x = 0; x < frame->width; x++)
          frame->data[0][y * frame->linesize[0] + x] = (uint8_t)(x + y + seq);
      for (int plane = 1; plane < 3; plane++)
        for (int y = 0; y < frame->height / 2; y++)
          memset(frame->data[plane] + y * frame->linesize[plane], 128, frame->width / 2);
      frame->pts = encrypted * 3 + seq;
      frame->pict_type = AV_PICTURE_TYPE_I;
      CHECK(avcodec_send_frame(encoder.context, frame) >= 0);
      // Every input must produce one independently decodable packet immediately.
      CHECK(avcodec_receive_packet(encoder.context, encoded) >= 0);
      CHECK(encoded->flags & AV_PKT_FLAG_KEY);
      nd_video_tx_publish(tx, encoded, seq);
      CHECK(!encoded->data);
      int complete = 0;
      while (!complete) {
        uint8_t datagram[ND_UDP_PAYLOAD_MAX];
        ssize_t size = recv(fd[1], datagram, sizeof(datagram), 0);
        CHECK(size > 0);
        complete = nd_frame_receive(&assembly, &stats, buffer, datagram, (size_t)size, stream_key);
        CHECK(nd_ios_assembler_feed(ios, datagram, (size_t)size) == complete);
      }
      NDIOSStats received = nd_ios_assembler_stats(ios);
      CHECK(received.sequence == seq && received.frame_bytes == assembly.size);
      CHECK(!memcmp(buffer, nd_ios_assembler_bytes(ios), assembly.size));
      decode_independently(buffer, assembly.size);
      CHECK(avcodec_receive_packet(encoder.context, encoded) == AVERROR(EAGAIN));
    }
    nd_video_tx_close(tx); nd_ios_assembler_destroy(ios);
    close(fd[0]); close(fd[1]);
  }
  free(buffer); av_frame_free(&frame); av_packet_free(&encoded);
  nd_encoder_close(&encoder);
  puts("PASS: immediate independent H.264 frames, shared TX and Linux/iOS RX, encrypted and plaintext");
  return 0;
}
