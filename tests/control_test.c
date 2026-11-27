// SPDX-License-Identifier: GPL-2.0-only
#include <sodium/core.h>
#define _GNU_SOURCE
#include "common.h"
#include "crypto.h"
#include "proto.h"
#include "network_test.h"
#include "session.h"

#include <signal.h>
#include <sys/stat.h>

static int prediction_enabled = 1;

static int reserve_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(a);
  if (getsockname(fd, (struct sockaddr *)&a, &len) < 0) {
    close(fd);
    return -1;
  }
  int port = ntohs(a.sin_port);
  close(fd);
  return port;
}

static int connect_retry(int port) {
  for (int attempt = 0; attempt < 100; attempt++) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return -1;
    struct timeval tv = {.tv_sec = 2};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_port = htons((uint16_t)port),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
      return fd;
    close(fd);
    usleep(20000);
  }
  return -1;
}

static const uint8_t salt[ND_SALT_SIZE] = {0x5a, 1, 2, 3, 4, 5, 6, 7,
                                           8, 9, 10, 11, 12, 13, 14, 15};

struct session {
  int fd;
  struct nd_channel ch;
};

static int recv_type(struct nd_channel *ch, uint16_t wanted, void *payload,
                     uint32_t size) {
  uint16_t type = 0;
  uint32_t len = size;
  int r = ndc_recv_msg(ch, &type, payload, &len);
  return r > 0 && type == wanted && len == size ? 0 : -1;
}

static int open_session(struct session *session, int port, uint32_t display_id,
                        int advertises_key, const uint8_t psk[ND_KEY_SIZE]) {
  int fd = connect_retry(port);
  if (fd < 0)
    return -1;
  struct nd_channel *ch = &session->ch;
  nd_channel_init(ch, fd);
  session->fd = fd;
  struct ndc_hello hello = {
      .flags = htonl((advertises_key ? NDC_FLAG_HAVE_PSK : 0) |
                     (advertises_key ? NDC_FLAG_NETWORK_TEST | NDC_FLAG_POWER_INFO | NDC_FLAG_PREDICTED_VIDEO : 0) |
                     NDC_FLAG_FRAME_ENCRYPT),
      .display_count = htons(1),
      .video_codecs = htonl(NDC_CODEC_ALL),
  };
  nd_crypto_random(hello.nonce, sizeof(hello.nonce));
  if (ndc_send_msg(ch, NDC_HELLO, &hello, sizeof(hello)) < 0)
    goto fail;
  struct ndc_challenge challenge;
  uint8_t video_key[ND_KEY_SIZE];
  if (recv_type(ch, NDC_CHALLENGE, &challenge, sizeof(challenge)) < 0 ||
      ntohl(challenge.flags) !=
          (NDC_FLAG_HAVE_PSK | NDC_FLAG_PASSWORD | NDC_FLAG_FRAME_ENCRYPT) ||
      memcmp(challenge.salt, salt, sizeof(salt)))
    goto fail;
  if (nd_session_client_auth(ch, psk, &hello, &challenge, video_key) < 0 ||
      !ch->sealed)
    goto fail;
  struct ndc_display display = {
      .display_id = htonl(display_id),
      .width = htons(1280),
      .height = htons(720),
      .refresh_hz = htons(60),
  };
  snprintf(display.connector, sizeof(display.connector), "TEST-%u", display_id);
  if (ndc_send_msg(ch, NDC_DISPLAY, &display, sizeof(display)) < 0)
    goto fail;
  struct ndc_welcome welcome;
  uint16_t welcome_type = 0;
  uint32_t welcome_len = sizeof(welcome);
  int wr = ndc_recv_msg(ch, &welcome_type, &welcome, &welcome_len);
  if (wr > 0 && welcome_type == NDC_REJECT && welcome_len == 0) {
    nd_channel_clear(ch);
    close(fd);
    return -2; /* no usable hardware codec on this test builder */
  }
  if (wr <= 0 || welcome_type != NDC_WELCOME || welcome_len != sizeof(welcome) ||
      ntohs(welcome.display_count) != 1 || !ntohs(welcome.video_codec) ||
      !(ntohl(welcome.flags) & NDC_FLAG_FRAME_ENCRYPT))
    goto fail;
  struct ndc_stream stream;
  if (recv_type(ch, NDC_STREAM, &stream, sizeof(stream)) < 0 ||
      ntohl(stream.display_id) != display_id || !nd_ntoh64(stream.stream_id))
    goto fail;
  if (!!(ntohl(welcome.flags) & NDC_FLAG_POWER_INFO) != !!advertises_key)
    goto fail;
  if (!!(ntohl(welcome.flags) & NDC_FLAG_NETWORK_TEST) != !!advertises_key)
    goto fail;
  if (!!(ntohl(welcome.flags) & NDC_FLAG_PREDICTED_VIDEO) != (advertises_key && prediction_enabled))
    goto fail;
  if (advertises_key) {
    uint8_t key[ND_KEY_SIZE];
    nd_crypto_stream_key(key, video_key, nd_ntoh64(stream.stream_id));
    if (nd_network_test(ch, 0, "127.0.0.1", ntohs(stream.video_port), NULL,
                         ntohs(stream.refresh_hz), nd_ntoh64(stream.stream_id),
                         key) < 0) goto fail;
  }
  return 0;
fail:
  nd_channel_clear(ch);
  close(fd);
  return -1;
}

/* A receiver holding the wrong password key must be refused before any
 * session state is created. */
static int wrong_password_rejected(int port) {
  struct session session;
  int fd = connect_retry(port);
  if (fd < 0)
    return 0;
  nd_channel_init(&session.ch, fd);
  struct ndc_hello hello = {.flags = htonl(NDC_FLAG_HAVE_PSK |
                                           NDC_FLAG_FRAME_ENCRYPT),
                            .display_count = htons(1),
                            .video_codecs = htonl(NDC_CODEC_ALL)};
  nd_crypto_random(hello.nonce, sizeof(hello.nonce));
  struct ndc_challenge challenge;
  uint8_t wrong[ND_KEY_SIZE] = {1}, video_key[ND_KEY_SIZE];
  int rejected =
      ndc_send_msg(&session.ch, NDC_HELLO, &hello, sizeof(hello)) == 0 &&
      recv_type(&session.ch, NDC_CHALLENGE, &challenge, sizeof(challenge)) == 0 &&
      nd_session_client_auth(&session.ch, wrong, &hello, &challenge,
                             video_key) < 0 &&
      errno == EACCES;
  close(fd);
  return rejected;
}

int main(int argc, char **argv) {
  if (argc != 2 && !(argc == 3 && (!strcmp(argv[2], "bandwidth") || !strcmp(argv[2], "all-intra"))))
    return 2;
  prediction_enabled = argc != 3 || strcmp(argv[2], "all-intra");
  if (sodium_init() < 0)
    return 2;
  int port = -1;
  for (int i = 0; i < 100 && (port < 1 || port > 65527); i++)
    port = reserve_port();
  if (port < 1 || port > 65527)
    return 3;
  char path[] = "/tmp/netdisplay-control-test-XXXXXX";
  int config = mkstemp(path);
  if (config < 0)
    return 4;
  uint8_t psk[ND_KEY_SIZE];
  if (nd_crypto_password_key(psk, "test control password", salt) < 0) {
    close(config);
    unlink(path);
    return 5;
  }
  char password_key[ND_PASSWORD_KEY_TEXT];
  nd_crypto_format_password_key(password_key, salt, psk);
  char text[768];
  int n = snprintf(text, sizeof(text),
                   "listen=127.0.0.1\nport=%d\nvideo_port=%d\n"
                   "max_clients=4\noutput=testdisplay\n"
                   "brightness_device=none\npassword_key=%s\n"
                   "frame_encryption=allowed\n%s",
                   port, port + 1, password_key,
                   !prediction_enabled ? "keyframe_interval=1\n" :
                   argc == 3 ? "rate_control=bandwidth\nbitrate_kbps=20000\nkeyframe_interval=2\n" : "");
  if (write(config, text, (size_t)n) != n) {
    close(config);
    unlink(path);
    return 5;
  }
  close(config);

  pid_t server = fork();
  if (server < 0) {
    unlink(path);
    return 6;
  }
  if (!server) {
    /* Keep the test away from any desktop session it runs inside. */
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("WAYLAND_SOCKET");
    unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    execl(argv[1], argv[1], path, (char *)NULL);
    _exit(127);
  }
  struct session one, two;
  int first = open_session(&one, port, 1, 1, psk);
  if (first == -2) {
    kill(server, SIGTERM);
    while (waitpid(server, NULL, 0) < 0 && errno == EINTR) {}
    unlink(path);
    puts("SKIP: no usable hardware encoder on test builder");
    return 77;
  }
  int second = first >= 0 ? open_session(&two, port, 2, 0, psk) : -1;
  int refused = first >= 0 && second >= 0 && wrong_password_rejected(port);
  /* Stopping the server ends the live sessions, then it exits cleanly. */
  kill(server, SIGTERM);
  int status = 0;
  while (waitpid(server, &status, 0) < 0 && errno == EINTR) {
  }
  int ended = first >= 0 && second >= 0;
  for (int i = 0; ended && i < 2; i++) {
    struct session *s = i ? &two : &one;
    uint8_t b[NDC_MAX_PAYLOAD];
    uint32_t len = sizeof(b);
    uint16_t type;
    while (ndc_recv_msg(&s->ch, &type, b, &len) > 0)
      len = sizeof(b);
  }
  if (first >= 0) {
    nd_channel_clear(&one.ch);
    close(one.fd);
  }
  if (second >= 0) {
    nd_channel_clear(&two.ch);
    close(two.fd);
  }
  unlink(path);
  if (first < 0 || second < 0)
    return 7;
  if (!refused)
    return 8;
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    fprintf(stderr, "server did not exit cleanly on SIGTERM\n");
    return 9;
  }
  puts("two concurrent sealed control sessions negotiated, wrong password "
       "refused, clean shutdown");
  return 0;
}
