/* Drives the real nut_server.c over a loopback socket and checks the
 * protocol responses a NUT client (upsc / upsmon) relies on. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "nut_server.h"

static int fails;
/* Evaluate the condition ONCE: it usually contains a cmd() call, and a
 * second evaluation would re-send the command — which matters now that
 * LOGIN is stateful. */
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

static int sock_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(3493) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { perror("connect"); return -1; }
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static const char *cmd(int fd, const char *c, char *buf, size_t n)
{
    char line[128];
    snprintf(line, sizeof line, "%s\n", c);
    send(fd, line, strlen(line), 0);
    usleep(80000);
    int r = recv(fd, buf, n - 1, 0);
    if (r < 0) r = 0;
    buf[r] = '\0';
    return buf;
}

/* Stand-in for main.c's nut_verify_login(). */
static bool verify(const char *user, const char *pass, void *ctx)
{
    (void)ctx;
    return user && pass && strcmp(user, "upsmon") == 0 &&
           strcmp(pass, "s3cret") == 0;
}

int main(void)
{
    nut_server_config_t cfg = {
        .ups_name = "ecoflow", .ups_desc = "EcoFlow River 3 UPS (245Wh)",
        .tcp_port = 3493, .max_clients = 4,
    };
    if (nut_server_start(&cfg) != 0) { fprintf(stderr, "start failed\n"); return 1; }

    nut_server_set_var_int("battery.charge", 75);
    nut_server_set_var_int("battery.charge.low", 20);
    nut_server_set_var_float("battery.temperature", 33.0f, 1);
    nut_server_set_var("ups.model", "River 3 UPS (245Wh)");
    nut_server_set_status("OL");
    usleep(150000);

    int fd = sock_connect();
    OKF(fd >= 0, "connect to :3493");
    if (fd < 0) return 1;

    char b[2048];
    OKF(strstr(cmd(fd, "VER", b, sizeof b), "esp32-nut-ecoflow") != NULL, "VER -> %s", b);
    OKF(strcmp(cmd(fd, "NETVER", b, sizeof b), "1.3\n") == 0, "NETVER -> %s", b);

    cmd(fd, "LIST UPS", b, sizeof b);
    OKF(strstr(b, "UPS ecoflow \"EcoFlow River 3 UPS (245Wh)\"") &&
        strstr(b, "BEGIN LIST UPS") && strstr(b, "END LIST UPS"), "LIST UPS -> %s", b);

    cmd(fd, "LIST VAR ecoflow", b, sizeof b);
    OKF(strstr(b, "VAR ecoflow battery.charge \"75\"") &&
        strstr(b, "VAR ecoflow ups.status \"OL\"") &&
        strstr(b, "VAR ecoflow ups.model \"River 3 UPS (245Wh)\"") &&
        strstr(b, "VAR ecoflow battery.temperature \"33.0\"") &&
        strstr(b, "END LIST VAR ecoflow"), "LIST VAR has expected vars");

    OKF(strcmp(cmd(fd, "GET VAR ecoflow battery.charge", b, sizeof b),
               "VAR ecoflow battery.charge \"75\"\n") == 0, "GET VAR -> %s", b);
    OKF(strcmp(cmd(fd, "GET VAR ecoflow no.such.var", b, sizeof b),
               "ERR VAR-NOT-SUPPORTED\n") == 0, "GET unknown var -> %s", b);
    OKF(strcmp(cmd(fd, "GET VAR wrongups ups.status", b, sizeof b),
               "ERR UNKNOWN-UPS\n") == 0, "GET wrong ups -> %s", b);
    OKF(strstr(cmd(fd, "GET UPSDESC ecoflow", b, sizeof b),
               "UPSDESC ecoflow \"EcoFlow River 3 UPS (245Wh)\"") != NULL, "GET UPSDESC");
    OKF(strcmp(cmd(fd, "GET NUMLOGINS ecoflow", b, sizeof b),
               "NUMLOGINS ecoflow 0\n") == 0, "GET NUMLOGINS -> %s", b);

    /* upsmon handshake with no login configured: anyone may LOGIN. */
    OKF(strcmp(cmd(fd, "USERNAME upsmon", b, sizeof b), "OK\n") == 0, "USERNAME");
    OKF(strcmp(cmd(fd, "PASSWORD x", b, sizeof b), "OK\n") == 0, "PASSWORD");
    OKF(strcmp(cmd(fd, "USERNAME again", b, sizeof b),
               "ERR ALREADY-SET-USERNAME\n") == 0, "USERNAME twice rejected");
    OKF(strcmp(cmd(fd, "PASSWORD again", b, sizeof b),
               "ERR ALREADY-SET-PASSWORD\n") == 0, "PASSWORD twice rejected");
    OKF(strcmp(cmd(fd, "LOGIN ecoflow", b, sizeof b), "OK\n") == 0, "LOGIN (no auth set)");
    OKF(strcmp(cmd(fd, "PRIMARY ecoflow", b, sizeof b), "OK\n") == 0, "PRIMARY");
    OKF(strcmp(cmd(fd, "MASTER ecoflow", b, sizeof b), "OK\n") == 0, "MASTER (legacy)");

    OKF(strstr(cmd(fd, "LIST CLIENT ecoflow", b, sizeof b), "BEGIN LIST CLIENT ecoflow") &&
        strstr(b, "END LIST CLIENT ecoflow"), "LIST CLIENT (empty)");
    OKF(strstr(cmd(fd, "LIST RW ecoflow", b, sizeof b), "END LIST RW ecoflow") != NULL, "LIST RW (empty)");

    OKF(strcmp(cmd(fd, "FROBNICATE", b, sizeof b), "ERR UNKNOWN-COMMAND\n") == 0, "unknown cmd");

    close(fd);

    /* ---- with a login configured ---- */
    nut_server_set_auth(verify, NULL);

    fd = sock_connect();
    OKF(fd >= 0, "reconnect with auth configured");
    /* Reads must stay anonymous: upsc cannot send credentials. */
    OKF(strstr(cmd(fd, "LIST UPS", b, sizeof b), "UPS ecoflow") != NULL,
        "LIST UPS still anonymous");
    OKF(strcmp(cmd(fd, "GET VAR ecoflow battery.charge", b, sizeof b),
               "VAR ecoflow battery.charge \"75\"\n") == 0,
        "GET VAR still anonymous");
    OKF(strstr(cmd(fd, "LIST VAR ecoflow", b, sizeof b), "ups.status") != NULL,
        "LIST VAR still anonymous");
    /* But LOGIN without credentials is refused. */
    OKF(strcmp(cmd(fd, "LOGIN ecoflow", b, sizeof b), "ERR ACCESS-DENIED\n") == 0,
        "LOGIN with no credentials denied");
    close(fd);

    fd = sock_connect();
    cmd(fd, "USERNAME upsmon", b, sizeof b);
    cmd(fd, "PASSWORD wrong", b, sizeof b);
    OKF(strcmp(cmd(fd, "LOGIN ecoflow", b, sizeof b), "ERR ACCESS-DENIED\n") == 0,
        "LOGIN with wrong password denied");
    close(fd);

    fd = sock_connect();
    cmd(fd, "USERNAME wronguser", b, sizeof b);
    cmd(fd, "PASSWORD s3cret", b, sizeof b);
    OKF(strcmp(cmd(fd, "LOGIN ecoflow", b, sizeof b), "ERR ACCESS-DENIED\n") == 0,
        "LOGIN with wrong username denied");
    close(fd);

    fd = sock_connect();
    cmd(fd, "USERNAME upsmon", b, sizeof b);
    cmd(fd, "PASSWORD s3cret", b, sizeof b);
    OKF(strcmp(cmd(fd, "LOGIN ecoflow", b, sizeof b), "OK\n") == 0,
        "LOGIN with correct credentials accepted");
    OKF(strcmp(cmd(fd, "PRIMARY ecoflow", b, sizeof b), "OK\n") == 0,
        "PRIMARY with correct credentials accepted");
    close(fd);

    /* Credentials must not leak between connections. */
    fd = sock_connect();
    OKF(strcmp(cmd(fd, "LOGIN ecoflow", b, sizeof b), "ERR ACCESS-DENIED\n") == 0,
        "a new connection starts unauthenticated");
    close(fd);

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
