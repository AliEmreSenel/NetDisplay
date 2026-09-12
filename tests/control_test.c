#define _GNU_SOURCE
#include "common.h"
#include "crypto.h"
#include "proto.h"

#include <signal.h>
#include <sys/stat.h>

static int reserve_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    socklen_t len = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &len) < 0) { close(fd); return -1; }
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int connect_retry(int port)
{
    for (int attempt = 0; attempt < 100; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        struct timeval tv = {.tv_sec = 2};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in a = {.sin_family = AF_INET,
                                .sin_port = htons((uint16_t)port),
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
        close(fd);
        usleep(20000);
    }
    return -1;
}

static int recv_type(int fd, uint16_t wanted, void *payload, uint32_t size)
{
    uint16_t type = 0;
    uint32_t len = size;
    int r = ndc_recv_msg(fd, &type, payload, &len);
    return r > 0 && type == wanted && len == size ? 0 : -1;
}

static int open_session(int port, uint32_t display_id, int advertises_key,
                        const uint8_t psk[ND_KEY_SIZE])
{
    int fd = connect_retry(port);
    if (fd < 0) return -1;
    struct ndc_hello hello = {
        .flags = htonl((advertises_key ? NDC_FLAG_HAVE_PSK : 0) |
                       NDC_FLAG_FRAME_ENCRYPT),
        .display_count = htons(1),
    };
    hello.nonce[0] = (uint8_t)display_id;
    if (ndc_send_msg(fd, NDC_HELLO, &hello, sizeof(hello)) < 0) goto fail;
    struct ndc_challenge challenge;
    if (recv_type(fd, NDC_CHALLENGE, &challenge, sizeof(challenge)) < 0 ||
        ntohl(challenge.flags) != (NDC_FLAG_HAVE_PSK | NDC_FLAG_PASSWORD |
                                   NDC_FLAG_FRAME_ENCRYPT))
        goto fail;
    struct ndc_auth auth, server_auth;
    nd_crypto_proof(auth.proof, psk, "client", hello.nonce, challenge.nonce);
    if (ndc_send_msg(fd, NDC_AUTH, &auth, sizeof(auth)) < 0 ||
        recv_type(fd, NDC_AUTH, &server_auth, sizeof(server_auth)) < 0)
        goto fail;
    uint8_t expected[NDC_AUTH_SIZE];
    nd_crypto_proof(expected, psk, "server", hello.nonce, challenge.nonce);
    if (!nd_crypto_verify(server_auth.proof, expected)) goto fail;
    struct ndc_display display = {
        .display_id = htonl(display_id), .width = htons(1280),
        .height = htons(720), .refresh_hz = htons(60),
    };
    snprintf(display.connector, sizeof(display.connector), "TEST-%u", display_id);
    if (ndc_send_msg(fd, NDC_DISPLAY, &display, sizeof(display)) < 0) goto fail;
    struct ndc_welcome welcome;
    if (recv_type(fd, NDC_WELCOME, &welcome, sizeof(welcome)) < 0 ||
        ntohs(welcome.display_count) != 1 ||
        !(ntohl(welcome.flags) & NDC_FLAG_FRAME_ENCRYPT)) goto fail;
    struct ndc_stream stream;
    if (recv_type(fd, NDC_STREAM, &stream, sizeof(stream)) < 0 ||
        ntohl(stream.display_id) != display_id || !nd_ntoh64(stream.stream_id))
        goto fail;
    return fd;
fail:
    close(fd);
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (nd_crypto_init() < 0) return 2;
    int port = -1;
    for (int i = 0; i < 100 && (port < 1 || port > 65527); i++)
        port = reserve_port();
    if (port < 1 || port > 65527) return 3;
    char path[] = "/tmp/netdisplay-control-test-XXXXXX";
    int config = mkstemp(path);
    if (config < 0) return 4;
    uint8_t psk[ND_KEY_SIZE];
    if (nd_crypto_password_key(psk, "test control password") < 0) {
        close(config); unlink(path); return 5;
    }
    char password_key[ND_KEY_SIZE * 2u + 1u];
    nd_crypto_key_to_hex(password_key, psk);
    char text[768];
    int n = snprintf(text, sizeof(text),
                     "listen=127.0.0.1\nport=%d\nvideo_port=%d\n"
                     "max_clients=4\noutput=testdisplay\n"
                     "brightness_device=none\npassword_key=%s\n"
                     "frame_encryption=allowed\n",
                     port, port + 1, password_key);
    if (write(config, text, (size_t)n) != n) {
        close(config); unlink(path); return 5;
    }
    close(config);

    pid_t server = fork();
    if (server < 0) { unlink(path); return 6; }
    if (!server) {
        execl(argv[1], argv[1], path, (char *)NULL);
        _exit(127);
    }
    int first = open_session(port, 1, 1, psk);
    int second = first >= 0 ? open_session(port, 2, 0, psk) : -1;
    if (first >= 0) close(first);
    if (second >= 0) close(second);
    kill(server, SIGTERM);
    while (waitpid(server, NULL, 0) < 0 && errno == EINTR) {}
    unlink(path);
    if (first < 0 || second < 0) return 7;
    puts("two concurrent control sessions negotiated");
    return 0;
}
