/*
 * Minimal upsd-compatible TCP server for NUT clients.
 *
 * Implements the read-only subset of the NUT network protocol that
 * `upsc`, `upsmon`, and most integrations use:
 *   LIST UPS / LIST VAR / GET VAR / GET UPSDESC / GET NUMLOGINS /
 *   GET DESC / GET TYPE / LIST RW / LIST CMD / VER / NETVER / HELP /
 *   USERNAME / PASSWORD / LOGIN / LOGOUT / STARTTLS
 *
 * Protocol reference:
 *   https://networkupstools.org/docs/developer-guide.chunked/ar01s09.html
 */

#include "nut_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "esp_log.h"

static const char *TAG = "nut_server";

#define NUT_SERVER_VERSION "esp32-nut-ecoflow 0.1.0"
#define CLIENT_RX_BUF      512
#define CLIENT_TX_BUF      1024
#define CLIENT_TASK_STACK  5120

typedef struct {
    char name[NUT_VAR_NAME_LEN];
    char value[NUT_VAR_VALUE_LEN];
    bool used;
} nut_var_t;

static struct {
    nut_server_config_t cfg;
    char                ups_name[32];
    char                ups_desc[64];
    nut_var_t           vars[NUT_MAX_VARS];
    SemaphoreHandle_t   lock;
    volatile int        client_count;
    bool                started;
} s;

/* ------------------------------------------------------------------ */
/* Variable table                                                     */
/* ------------------------------------------------------------------ */

static nut_var_t *var_find_locked(const char *name)
{
    for (int i = 0; i < NUT_MAX_VARS; i++) {
        if (s.vars[i].used && strcmp(s.vars[i].name, name) == 0) {
            return &s.vars[i];
        }
    }
    return NULL;
}

void nut_server_set_var(const char *name, const char *value)
{
    if (!name || !value || !s.lock) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    nut_var_t *v = var_find_locked(name);
    if (!v) {
        for (int i = 0; i < NUT_MAX_VARS; i++) {
            if (!s.vars[i].used) {
                v = &s.vars[i];
                v->used = true;
                strlcpy(v->name, name, sizeof(v->name));
                break;
            }
        }
    }
    if (v) {
        strlcpy(v->value, value, sizeof(v->value));
    } else {
        ESP_LOGW(TAG, "var table full, dropping '%s'", name);
    }
    xSemaphoreGive(s.lock);
}

void nut_server_set_var_int(const char *name, long value)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", value);
    nut_server_set_var(name, buf);
}

void nut_server_set_var_float(const char *name, float value, int decimals)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    nut_server_set_var(name, buf);
}

void nut_server_clear_var(const char *name)
{
    if (!name || !s.lock) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    nut_var_t *v = var_find_locked(name);
    if (v) {
        v->used = false;
    }
    xSemaphoreGive(s.lock);
}

void nut_server_set_status(const char *status)
{
    nut_server_set_var("ups.status", status ? status : "OFF");
}

/* ------------------------------------------------------------------ */
/* Small send helpers                                                  */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, len - off, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
            return -1;
        }
        off += n;
    }
    return 0;
}

static int send_str(int fd, const char *str)
{
    return send_all(fd, str, strlen(str));
}

static int send_fmt(int fd, const char *fmt, ...)
{
    char buf[CLIENT_TX_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return -1;
    }
    if (n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    return send_all(fd, buf, n);
}

/* Append a NUT-quoted string ("..." with \ and " escaped) to dst. */
static void append_quoted(char *dst, size_t dstsz, const char *src)
{
    size_t len = strlen(dst);
    if (len + 1 < dstsz) {
        dst[len++] = '"';
    }
    for (const char *p = src; *p && len + 2 < dstsz; p++) {
        if (*p == '"' || *p == '\\') {
            dst[len++] = '\\';
        }
        dst[len++] = *p;
    }
    if (len + 1 < dstsz) {
        dst[len++] = '"';
    }
    dst[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Command handling                                                    */
/* ------------------------------------------------------------------ */

#define MAX_TOKENS 8

static int tokenize(char *line, char *tok[MAX_TOKENS])
{
    int n = 0;
    char *p = line;
    while (*p && n < MAX_TOKENS) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (*p == '"') {
            p++;
            tok[n++] = p;
            while (*p && *p != '"') {
                p++;
            }
        } else {
            tok[n++] = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return n;
}

static bool ups_matches(const char *name)
{
    return name && strcmp(name, s.ups_name) == 0;
}

static int cmd_list_ups(int fd)
{
    char line[128] = "UPS ";
    strlcat(line, s.ups_name, sizeof(line));
    strlcat(line, " ", sizeof(line));
    append_quoted(line, sizeof(line), s.ups_desc);
    return send_fmt(fd, "BEGIN LIST UPS\n%s\nEND LIST UPS\n", line);
}

static int cmd_list_var(int fd, const char *ups)
{
    if (!ups_matches(ups)) {
        return send_str(fd, "ERR UNKNOWN-UPS\n");
    }
    if (send_fmt(fd, "BEGIN LIST VAR %s\n", ups) < 0) {
        return -1;
    }
    int rc = 0;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (int i = 0; i < NUT_MAX_VARS && rc == 0; i++) {
        if (!s.vars[i].used) {
            continue;
        }
        char line[64 + NUT_VAR_VALUE_LEN];
        snprintf(line, sizeof(line), "VAR %s %s ", ups, s.vars[i].name);
        append_quoted(line, sizeof(line), s.vars[i].value);
        strlcat(line, "\n", sizeof(line));
        rc = send_str(fd, line);
    }
    xSemaphoreGive(s.lock);
    if (rc < 0) {
        return -1;
    }
    return send_fmt(fd, "END LIST VAR %s\n", ups);
}

static int cmd_get_var(int fd, const char *ups, const char *var)
{
    if (!ups_matches(ups)) {
        return send_str(fd, "ERR UNKNOWN-UPS\n");
    }
    char out[64 + NUT_VAR_VALUE_LEN];
    int rc;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    nut_var_t *v = var_find_locked(var);
    if (v) {
        snprintf(out, sizeof(out), "VAR %s %s ", ups, var);
        append_quoted(out, sizeof(out), v->value);
        strlcat(out, "\n", sizeof(out));
        rc = 1;
    } else {
        rc = 0;
    }
    xSemaphoreGive(s.lock);
    if (rc) {
        return send_str(fd, out);
    }
    return send_str(fd, "ERR VAR-NOT-SUPPORTED\n");
}

static int handle_line(int fd, char *line)
{
    char *tok[MAX_TOKENS];
    int n = tokenize(line, tok);
    if (n == 0) {
        return 0;
    }

    if (strcasecmp(tok[0], "LIST") == 0 && n >= 2) {
        if (strcasecmp(tok[1], "UPS") == 0) {
            return cmd_list_ups(fd);
        }
        if (strcasecmp(tok[1], "VAR") == 0 && n >= 3) {
            return cmd_list_var(fd, tok[2]);
        }
        if ((strcasecmp(tok[1], "RW") == 0 || strcasecmp(tok[1], "CMD") == 0 ||
             strcasecmp(tok[1], "ENUM") == 0 || strcasecmp(tok[1], "RANGE") == 0 ||
             strcasecmp(tok[1], "CLIENT") == 0 || strcasecmp(tok[1], "CLIENTS") == 0) &&
            n >= 3) {
            return send_fmt(fd, "BEGIN LIST %s %s\nEND LIST %s %s\n",
                            tok[1], tok[2], tok[1], tok[2]);
        }
        return send_str(fd, "ERR INVALID-ARGUMENT\n");
    }

    if (strcasecmp(tok[0], "GET") == 0 && n >= 3) {
        if (strcasecmp(tok[1], "VAR") == 0 && n >= 4) {
            return cmd_get_var(fd, tok[2], tok[3]);
        }
        if (strcasecmp(tok[1], "UPSDESC") == 0) {
            if (!ups_matches(tok[2])) {
                return send_str(fd, "ERR UNKNOWN-UPS\n");
            }
            char out[128];
            snprintf(out, sizeof(out), "UPSDESC %s ", tok[2]);
            append_quoted(out, sizeof(out), s.ups_desc);
            strlcat(out, "\n", sizeof(out));
            return send_str(fd, out);
        }
        if (strcasecmp(tok[1], "NUMLOGINS") == 0) {
            return send_fmt(fd, "NUMLOGINS %s 0\n", tok[2]);
        }
        if (strcasecmp(tok[1], "DESC") == 0 && n >= 4) {
            return send_fmt(fd, "DESC %s %s \"%s\"\n", tok[2], tok[3], tok[3]);
        }
        if (strcasecmp(tok[1], "TYPE") == 0 && n >= 4) {
            return send_fmt(fd, "TYPE %s %s STRING:%d\n",
                            tok[2], tok[3], NUT_VAR_VALUE_LEN - 1);
        }
        return send_str(fd, "ERR INVALID-ARGUMENT\n");
    }

    if (strcasecmp(tok[0], "USERNAME") == 0 || strcasecmp(tok[0], "PASSWORD") == 0 ||
        strcasecmp(tok[0], "LOGIN") == 0 ||
        strcasecmp(tok[0], "PRIMARY") == 0 || strcasecmp(tok[0], "MASTER") == 0) {
        /* PRIMARY/MASTER: grant upsmon primary privileges (single-appliance
         * server, no real login slots to arbitrate). */
        return send_str(fd, "OK\n");
    }
    if (strcasecmp(tok[0], "LOGOUT") == 0) {
        send_str(fd, "OK Goodbye\n");
        return -1; /* close */
    }
    if (strcasecmp(tok[0], "STARTTLS") == 0) {
        return send_str(fd, "ERR FEATURE-NOT-CONFIGURED\n");
    }
    if (strcasecmp(tok[0], "VER") == 0) {
        return send_str(fd, NUT_SERVER_VERSION "\n");
    }
    if (strcasecmp(tok[0], "NETVER") == 0 || strcasecmp(tok[0], "PROTVER") == 0) {
        return send_str(fd, "1.3\n");
    }
    if (strcasecmp(tok[0], "HELP") == 0) {
        return send_str(fd, "Commands: HELP VER GET LIST LOGIN LOGOUT USERNAME "
                            "PASSWORD STARTTLS\n");
    }

    return send_str(fd, "ERR UNKNOWN-COMMAND\n");
}

/* ------------------------------------------------------------------ */
/* Per-client task                                                     */
/* ------------------------------------------------------------------ */

static void client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[CLIENT_RX_BUF];
    size_t fill = 0;

    struct timeval tv = { .tv_sec = 120, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;) {
        int n = recv(fd, buf + fill, sizeof(buf) - 1 - fill, 0);
        if (n <= 0) {
            break;
        }
        fill += n;
        buf[fill] = '\0';

        char *start = buf;
        char *nl;
        bool closed = false;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            size_t len = strlen(start);
            if (len && start[len - 1] == '\r') {
                start[len - 1] = '\0';
            }
            if (handle_line(fd, start) < 0) {
                closed = true;
                break;
            }
            start = nl + 1;
        }
        if (closed) {
            break;
        }
        /* keep any partial line */
        fill = strlen(start);
        memmove(buf, start, fill + 1);
        if (fill == sizeof(buf) - 1) {
            send_str(fd, "ERR TOO-LONG\n");
            fill = 0;
        }
    }

    close(fd);
    s.client_count--;
    ESP_LOGI(TAG, "client closed (%d active)", s.client_count);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Accept task                                                         */
/* ------------------------------------------------------------------ */

static void accept_task(void *arg)
{
    (void)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(s.cfg.tcp_port),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%u) failed: %d", s.cfg.tcp_port, errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, 4) < 0) {
        ESP_LOGE(TAG, "listen() failed: %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on :%u as UPS '%s'", s.cfg.tcp_port, s.ups_name);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            ESP_LOGW(TAG, "accept() failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (s.client_count >= s.cfg.max_clients) {
            send_str(fd, "ERR ACCESS-DENIED\n");
            close(fd);
            continue;
        }
        char ip[16];
        inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
        s.client_count++;
        ESP_LOGI(TAG, "client %s connected (%d active)", ip, s.client_count);
        if (xTaskCreate(client_task, "nut_client", CLIENT_TASK_STACK,
                        (void *)(intptr_t)fd, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "cannot spawn client task");
            close(fd);
            s.client_count--;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

int nut_server_start(const nut_server_config_t *config)
{
    if (s.started) {
        return 0;
    }
    if (!config) {
        return -1;
    }
    s.cfg = *config;
    if (s.cfg.tcp_port == 0) {
        s.cfg.tcp_port = 3493;
    }
    if (s.cfg.max_clients == 0) {
        s.cfg.max_clients = 4;
    }
    strlcpy(s.ups_name, config->ups_name ? config->ups_name : "ups",
            sizeof(s.ups_name));
    strlcpy(s.ups_desc, config->ups_desc ? config->ups_desc : "EcoFlow via ESP32",
            sizeof(s.ups_desc));

    s.lock = xSemaphoreCreateMutex();
    if (!s.lock) {
        return -1;
    }

    /* Seed the mandatory NUT variables so clients see a coherent UPS
     * even before the first EcoFlow poll completes. */
    nut_server_set_var("device.mfr", "EcoFlow");
    nut_server_set_var("device.model", s.ups_desc);
    nut_server_set_var("device.type", "ups");
    nut_server_set_var("driver.name", "esp32-nut-ecoflow");
    nut_server_set_var("driver.version", NUT_SERVER_VERSION);
    nut_server_set_var("ups.mfr", "EcoFlow");
    nut_server_set_var("ups.model", s.ups_desc);
    nut_server_set_var("ups.status", "OFF");
    nut_server_set_var("battery.charge", "0");

    if (xTaskCreate(accept_task, "nut_accept", 4096, NULL, 5, NULL) != pdPASS) {
        return -1;
    }
    s.started = true;
    return 0;
}
