// SPDX-License-Identifier: GPL-2.0-only
#include "common.h"
#include "session.h"
#include <sodium.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static const uint8_t salt[ND_SALT_SIZE] = {9, 8, 7, 6, 5, 4, 3, 2,
                                           1, 0, 1, 2, 3, 4, 5, 6};
static const char secret_text[] = "type this secret";

static void socket_pair(int fd[2]) {
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  struct timeval timeout = {.tv_sec = 2};
  for (int i = 0; i < 2; i++) {
    CHECK(setsockopt(fd[i], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(setsockopt(fd[i], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
  }
}

/* Source side, in a child process. After a successful handshake it echoes
 * one sealed message back and reports its video key over the socket. */
static void serve(int fd, int have_key, int password, int policy, int expect_fail) {
  struct nd_channel ch;
  nd_channel_init(&ch, fd);
  uint8_t key[ND_KEY_SIZE] = {1}, video_key[ND_KEY_SIZE] = {0};
  struct ndc_hello hello;
  struct ndc_challenge challenge;
  CHECK(ndc_recv_expected(&ch, NDC_HELLO, &hello, sizeof(hello)) == 0);
  int r = nd_session_server_auth(&ch, &hello, have_key ? key : NULL,
                                 password ? salt : NULL, policy, &challenge,
                                 video_key);
  CHECK((r < 0) == expect_fail);
  if (r == 0 && have_key) {
    CHECK(ch.sealed);
    uint8_t b[NDC_MAX_PAYLOAD];
    uint32_t len = sizeof(b);
    uint16_t type;
    CHECK(ndc_recv_msg(&ch, &type, b, &len) == 1);
    CHECK(type == NDC_INPUT && len == sizeof(secret_text) &&
          !memcmp(b, secret_text, len));
    CHECK(ndc_send_msg(&ch, NDC_TEST_REPORT, b, len) == 0);
    CHECK(ndc_send_msg(&ch, NDC_TEST_PORT, video_key, sizeof(video_key)) == 0);
  }
  nd_channel_clear(&ch);
  close(fd);
  _exit(0);
}

static void exercise(int have_key, int encrypted, int policy, int wrong_key,
                     int password) {
  int fd[2];
  socket_pair(fd);
  uint8_t client_key[ND_KEY_SIZE] = {1};
  if (wrong_key) client_key[0]++;
  int rejected = (encrypted && (!have_key || policy == ND_ENCRYPTION_OFF)) ||
                 (!encrypted && policy == ND_ENCRYPTION_REQUIRED);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(fd[0]);
    serve(fd[1], have_key, password, policy, rejected || wrong_key);
  }
  close(fd[1]);
  struct nd_channel ch;
  nd_channel_init(&ch, fd[0]);
  struct ndc_hello hello = {.flags = htonl(encrypted ? NDC_FLAG_FRAME_ENCRYPT : 0),
                            .display_count = htons(1),
                            .video_codecs = htonl(NDC_CODEC_H264)};
  nd_crypto_random(hello.nonce, sizeof(hello.nonce));
  CHECK(ndc_send_msg(&ch, NDC_HELLO, &hello, sizeof(hello)) == 0);
  struct ndc_challenge challenge;
  int r = ndc_recv_expected(&ch, NDC_CHALLENGE, &challenge, sizeof(challenge));
  if (rejected) {
    CHECK(r == -1 && errno == EACCES);
  } else {
    CHECK(r == 0);
    CHECK(ntohl(challenge.flags) ==
          ((have_key ? NDC_FLAG_AUTH_REQUIRED : 0) |
           (have_key && password ? NDC_FLAG_PASSWORD : 0) |
           (encrypted ? NDC_FLAG_FRAME_ENCRYPT : 0)));
    static const uint8_t zero[NDC_SHARE_SIZE];
    CHECK(!memcmp(challenge.salt, password && have_key ? salt : zero,
                  sizeof(challenge.salt)));
    CHECK(!memcmp(challenge.share, zero, sizeof(challenge.share)) == !have_key);
    if (have_key) {
      uint8_t video_key[ND_KEY_SIZE];
      r = nd_session_client_auth(&ch, client_key, &hello, &challenge, video_key);
      CHECK((r < 0) == wrong_key);
      if (wrong_key) {
        CHECK(errno == EACCES && !ch.sealed);
      } else {
        CHECK(ch.sealed);
        CHECK(ndc_send_msg(&ch, NDC_INPUT, secret_text, sizeof(secret_text)) == 0);
        uint8_t b[NDC_MAX_PAYLOAD];
        uint32_t len = sizeof(b);
        uint16_t type;
        CHECK(ndc_recv_msg(&ch, &type, b, &len) == 1);
        CHECK(type == NDC_TEST_REPORT && len == sizeof(secret_text) &&
              !memcmp(b, secret_text, len));
        len = sizeof(b);
        CHECK(ndc_recv_msg(&ch, &type, b, &len) == 1);
        CHECK(type == NDC_TEST_PORT && len == ND_KEY_SIZE &&
              !memcmp(b, video_key, ND_KEY_SIZE));
      }
    }
  }
  nd_channel_clear(&ch);
  close(fd[0]);
  int status;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* Builds a pair of channels sealed with the same session keys, the way a
 * completed handshake leaves them. */
static void sealed_pair(int fd[2], struct nd_channel *client,
                        struct nd_channel *server) {
  socket_pair(fd);
  uint8_t key[ND_KEY_SIZE] = {3};
  struct ndc_hello hello = {0};
  struct ndc_challenge challenge = {0};
  struct ndc_auth auth;
  struct ndc_auth_reply reply;
  struct nd_pake pake;
  struct nd_session_keys server_keys, client_keys;
  nd_crypto_random(hello.nonce, sizeof(hello.nonce));
  nd_crypto_random(challenge.nonce, sizeof(challenge.nonce));
  CHECK(nd_handshake_server_start(&pake, key, &hello, &challenge) == 0);
  CHECK(nd_handshake_client(key, &hello, &challenge, &auth, &client_keys) == 0);
  CHECK(nd_handshake_server_finish(&pake, key, &hello, &challenge, &auth,
                                   &reply, &server_keys) == 0);
  CHECK(nd_crypto_verify(reply.proof, client_keys.server_proof));
  CHECK(!memcmp(&client_keys, &server_keys, sizeof(client_keys)));
  nd_channel_init(client, fd[0]);
  nd_channel_init(server, fd[1]);
  memcpy(client->tx_key, client_keys.client_to_server, ND_KEY_SIZE);
  memcpy(client->rx_key, client_keys.server_to_client, ND_KEY_SIZE);
  memcpy(server->tx_key, server_keys.server_to_client, ND_KEY_SIZE);
  memcpy(server->rx_key, server_keys.client_to_server, ND_KEY_SIZE);
  client->sealed = server->sealed = 1;
  nd_pake_clear(&pake);
}

static int read_raw(int fd, uint8_t *b, size_t cap) {
  struct ndc_hdr h;
  CHECK(ndc_read_full(fd, &h, sizeof(h)) == 1);
  uint32_t len = ntohl(h.length);
  CHECK(sizeof(h) + len <= cap);
  memcpy(b, &h, sizeof(h));
  CHECK(ndc_read_full(fd, b + sizeof(h), len) == 1);
  return (int)(sizeof(h) + len);
}

static void sealed_wire(void) {
  int fd[2];
  struct nd_channel client, server;
  uint8_t raw[sizeof(struct ndc_hdr) + NDC_MAX_WIRE];
  uint8_t b[NDC_MAX_PAYLOAD];
  uint32_t len;
  uint16_t type;

  /* Ciphertext only: neither the type nor the payload is visible. */
  sealed_pair(fd, &client, &server);
  CHECK(ndc_send_msg(&client, NDC_INPUT, secret_text, sizeof(secret_text)) == 0);
  int n = read_raw(fd[1], raw, sizeof(raw));
  struct ndc_hdr h;
  memcpy(&h, raw, sizeof(h));
  CHECK(ntohs(h.type) == NDC_SEALED &&
        ntohl(h.length) == sizeof(secret_text) + NDC_SEAL_OVERHEAD);
  CHECK(!memmem(raw, (size_t)n, secret_text, sizeof(secret_text) - 1));

  /* A tampered message is refused. */
  raw[n - 1] ^= 1;
  CHECK(ndc_write_full(fd[0], raw, (size_t)n) == 0);
  len = sizeof(b);
  CHECK(ndc_recv_msg(&server, &type, b, &len) == -1 && errno == EBADMSG);
  nd_channel_clear(&client); nd_channel_clear(&server);
  close(fd[0]); close(fd[1]);

  /* A replayed message is refused: the sequence has moved on. */
  sealed_pair(fd, &client, &server);
  CHECK(ndc_send_msg(&client, NDC_PING, NULL, 0) == 0);
  n = read_raw(fd[1], raw, sizeof(raw));
  CHECK(ndc_write_full(fd[0], raw, (size_t)n) == 0);
  CHECK(ndc_write_full(fd[0], raw, (size_t)n) == 0);
  len = sizeof(b);
  CHECK(ndc_recv_msg(&server, &type, b, &len) == 1 && type == NDC_PING && !len);
  len = sizeof(b);
  CHECK(ndc_recv_msg(&server, &type, b, &len) == -1 && errno == EBADMSG);
  nd_channel_clear(&client); nd_channel_clear(&server);
  close(fd[0]); close(fd[1]);

  /* Plaintext injected into a sealed session is refused. */
  sealed_pair(fd, &client, &server);
  struct ndc_input injected = {.device = NDC_DEV_KBM};
  CHECK(ndc_send_plain(fd[0], NDC_INPUT, &injected, sizeof(injected)) == 0);
  len = sizeof(b);
  CHECK(ndc_recv_msg(&server, &type, b, &len) == -1 && errno == EPROTO);
  nd_channel_clear(&client); nd_channel_clear(&server);
  close(fd[0]); close(fd[1]);

  /* A message sealed for one direction does not open in the other. */
  sealed_pair(fd, &client, &server);
  CHECK(ndc_send_msg(&server, NDC_PONG, NULL, 0) == 0);
  n = read_raw(fd[0], raw, sizeof(raw));
  CHECK(ndc_write_full(fd[0], raw, (size_t)n) == 0);
  len = sizeof(b);
  CHECK(ndc_recv_msg(&server, &type, b, &len) == -1 && errno == EBADMSG);
  nd_channel_clear(&client); nd_channel_clear(&server);
  close(fd[0]); close(fd[1]);
}

/* An impostor source that does not hold the key cannot answer the proof. */
static void forged_server(void) {
  int fd[2];
  socket_pair(fd);
  uint8_t key[ND_KEY_SIZE] = {1}, other[ND_KEY_SIZE] = {2}, video_key[ND_KEY_SIZE];
  struct ndc_hello hello = {0};
  struct ndc_challenge challenge = {0};
  struct nd_pake pake;
  nd_crypto_random(hello.nonce, sizeof(hello.nonce));
  nd_crypto_random(challenge.nonce, sizeof(challenge.nonce));
  CHECK(nd_handshake_server_start(&pake, other, &hello, &challenge) == 0);
  /* Preload a reply; client writes fit in the socket buffer. */
  struct nd_channel impostor, client;
  nd_channel_init(&impostor, fd[1]);
  nd_channel_init(&client, fd[0]);
  struct ndc_auth_reply bad = {{0}};
  CHECK(ndc_send_msg(&impostor, NDC_AUTH, &bad, sizeof(bad)) == 0);
  CHECK(nd_session_client_auth(&client, key, &hello, &challenge, video_key) == -1);
  CHECK(errno == EACCES && !client.sealed);
  nd_pake_clear(&pake);
  close(fd[0]); close(fd[1]);

  /* The identity element is not a valid share. */
  socket_pair(fd);
  nd_channel_init(&client, fd[0]);
  memset(challenge.share, 0, sizeof(challenge.share));
  CHECK(nd_session_client_auth(&client, key, &hello, &challenge, video_key) == -1);
  CHECK(errno == EPROTO);
  close(fd[0]); close(fd[1]);
}

/* A wrong key or a modified transcript fails key confirmation. */
static void handshake_binding(void) {
  uint8_t key[ND_KEY_SIZE] = {5}, wrong[ND_KEY_SIZE] = {6};
  struct ndc_hello hello = {.flags = htonl(NDC_FLAG_FRAME_ENCRYPT)};
  struct ndc_challenge challenge = {0};
  struct ndc_auth auth;
  struct ndc_auth_reply reply;
  struct nd_pake pake;
  struct nd_session_keys a, b;
  nd_crypto_random(hello.nonce, sizeof(hello.nonce));
  nd_crypto_random(challenge.nonce, sizeof(challenge.nonce));
  CHECK(nd_handshake_server_start(&pake, key, &hello, &challenge) == 0);
  CHECK(nd_handshake_client(wrong, &hello, &challenge, &auth, &a) == 0);
  CHECK(nd_handshake_server_finish(&pake, key, &hello, &challenge, &auth,
                                   &reply, &b) == -1 && errno == EACCES);
  /* A downgraded flag the source never sent. */
  struct ndc_hello altered = hello;
  altered.flags = 0;
  CHECK(nd_handshake_client(key, &altered, &challenge, &auth, &a) == 0);
  CHECK(nd_handshake_server_finish(&pake, key, &hello, &challenge, &auth,
                                   &reply, &b) == -1 && errno == EACCES);
  /* Fresh shares each time, so two sessions never share keys. */
  struct ndc_auth auth2;
  CHECK(nd_handshake_client(key, &hello, &challenge, &auth, &a) == 0);
  CHECK(nd_handshake_client(key, &hello, &challenge, &auth2, &b) == 0);
  CHECK(memcmp(auth.share, auth2.share, sizeof(auth.share)) &&
        memcmp(a.video, b.video, sizeof(a.video)));
  nd_pake_clear(&pake);
}

int main(void) {
  CHECK(sodium_init() >= 0);
  for (int policy = ND_ENCRYPTION_OFF; policy <= ND_ENCRYPTION_REQUIRED; policy++)
    for (int encrypted = 0; encrypted <= 1; encrypted++)
      for (int have_key = 0; have_key <= 1; have_key++)
        exercise(have_key, encrypted, policy, 0, 0);
  exercise(1, 1, ND_ENCRYPTION_REQUIRED, 0, 1);
  exercise(1, 1, ND_ENCRYPTION_REQUIRED, 1, 0);
  exercise(1, 0, ND_ENCRYPTION_OFF, 1, 1);
  sealed_wire();
  forged_server();
  handshake_binding();
  puts("PASS: session policy, PAKE mutual authentication, sealed control "
       "channel, tamper/replay/injection rejection and forged server rejection");
  return 0;
}
