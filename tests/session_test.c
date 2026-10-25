// SPDX-License-Identifier: GPL-2.0-only
#include "common.h"
#include "session.h"
#include <sodium.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void exercise(int have_key, int encrypted, int policy, int wrong_key,
                      int password) {
  int fd[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  struct timeval timeout = {.tv_sec = 2};
  for (int i = 0; i < 2; i++) {
    CHECK(setsockopt(fd[i], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(setsockopt(fd[i], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
  }
  uint8_t key[ND_KEY_SIZE] = {1}, client_key[ND_KEY_SIZE] = {1};
  if (wrong_key) client_key[0]++;
  int rejected = (encrypted && (!have_key || policy == ND_ENCRYPTION_OFF)) ||
                 (!encrypted && policy == ND_ENCRYPTION_REQUIRED);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(fd[0]);
    struct ndc_hello hello;
    struct ndc_challenge challenge;
    CHECK(ndc_recv_expected(fd[1], NDC_HELLO, &hello, sizeof(hello)) == 0);
    int r = nd_session_server_auth(fd[1], &hello, have_key ? key : NULL,
                                    password, policy, &challenge);
    CHECK((r < 0) == (rejected || wrong_key));
    close(fd[1]);
    _exit(0);
  }
  close(fd[1]);
  struct ndc_hello hello = {.flags = htonl(encrypted ? NDC_FLAG_FRAME_ENCRYPT : 0),
                           .display_count = htons(1),
                           .video_codecs = htonl(NDC_CODEC_H264)};
  nd_crypto_random(hello.nonce, sizeof(hello.nonce));
  CHECK(ndc_send_msg(fd[0], NDC_HELLO, &hello, sizeof(hello)) == 0);
  struct ndc_challenge challenge;
  int r = ndc_recv_expected(fd[0], NDC_CHALLENGE, &challenge, sizeof(challenge));
  if (rejected) {
    CHECK(r == -1 && errno == EACCES);
  } else {
    CHECK(r == 0);
    CHECK(ntohl(challenge.flags) ==
           ((have_key ? NDC_FLAG_AUTH_REQUIRED : 0) |
            (have_key && password ? NDC_FLAG_PASSWORD : 0) |
            (encrypted ? NDC_FLAG_FRAME_ENCRYPT : 0)));
    if (have_key) {
      r = nd_session_client_auth(fd[0], client_key, &hello, &challenge);
      CHECK((r < 0) == wrong_key);
      if (wrong_key) CHECK(errno == EACCES);
    }
  }
  close(fd[0]);
  int status;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void forged_server(void) {
  int fd[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  /* Preload a valid AUTH envelope with an invalid proof. Client writes fit
   * in the socket buffer without a reader; no thread or timer is needed. */
  struct ndc_auth bad = {0};
  CHECK(ndc_send_msg(fd[1], NDC_AUTH, &bad, sizeof(bad)) == 0);
  uint8_t key[ND_KEY_SIZE] = {1};
  struct ndc_hello hello = {0};
  struct ndc_challenge challenge = {0};
  CHECK(nd_session_client_auth(fd[0], key, &hello, &challenge) == -1);
  CHECK(errno == EACCES);
  close(fd[0]); close(fd[1]);
}

int main(void) {
  CHECK(sodium_init() >= 0);
  for (int policy = ND_ENCRYPTION_OFF; policy <= ND_ENCRYPTION_REQUIRED; policy++)
    for (int encrypted = 0; encrypted <= 1; encrypted++)
      for (int have_key = 0; have_key <= 1; have_key++)
        exercise(have_key, encrypted, policy, 0, 0);
  exercise(1, 1, ND_ENCRYPTION_REQUIRED, 0, 1);
  exercise(1, 1, ND_ENCRYPTION_REQUIRED, 1, 0);
  forged_server();
  puts("PASS: shared session policy, mutual authentication, wrong key and forged server rejection");
  return 0;
}
