/*
 * Minimal DNS responder for the setup captive portal. For any standard
 * A query it returns a single answer pointing at the ESP32's SoftAP IP,
 * so a phone that joins the AP is immediately redirected to the portal.
 * Non-A / malformed queries are ignored.
 */

#include "dns_server.h"

#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dns_server";

#define DNS_PORT 53
#define DNS_MAX  512

static TaskHandle_t s_task;
static int          s_sock = -1;
static uint32_t     s_ip;   /* network byte order */

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static void dns_task(void *arg)
{
    uint8_t rx[DNS_MAX];
    uint8_t tx[DNS_MAX];

    while (s_sock >= 0) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s_sock, rx, sizeof(rx), 0,
                         (struct sockaddr *)&from, &flen);
        if (n < (int)sizeof(dns_header_t)) {
            continue;
        }

        dns_header_t *qh = (dns_header_t *)rx;
        if (ntohs(qh->qd_count) != 1) {
            continue;
        }

        /* Walk the question name to find its end (label sequence ended by 0). */
        int p = sizeof(dns_header_t);
        while (p < n && rx[p] != 0) {
            p += rx[p] + 1;
        }
        p += 1;                 /* the zero length byte */
        int qend = p + 4;       /* QTYPE(2) + QCLASS(2) */
        if (qend > n || qend + 16 > (int)sizeof(tx)) {
            continue;
        }

        /* Build response: echo header+question, append one A answer. */
        memcpy(tx, rx, qend);
        dns_header_t *rh = (dns_header_t *)tx;
        rh->flags = htons(0x8180);   /* response, recursion available */
        rh->an_count = htons(1);
        rh->ns_count = 0;
        rh->ar_count = 0;

        int o = qend;
        tx[o++] = 0xC0;  tx[o++] = 0x0C;          /* name pointer -> offset 12 */
        tx[o++] = 0x00;  tx[o++] = 0x01;          /* TYPE A   */
        tx[o++] = 0x00;  tx[o++] = 0x01;          /* CLASS IN */
        tx[o++] = 0x00;  tx[o++] = 0x00;
        tx[o++] = 0x00;  tx[o++] = 0x3C;          /* TTL 60s  */
        tx[o++] = 0x00;  tx[o++] = 0x04;          /* RDLENGTH */
        memcpy(&tx[o], &s_ip, 4);
        o += 4;

        sendto(s_sock, tx, o, 0, (struct sockaddr *)&from, flen);
    }
    ESP_LOGI(TAG, "dns task exit");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(const char *redirect_ip)
{
    if (s_sock >= 0) {
        return ESP_OK;
    }
    s_ip = inet_addr(redirect_ip);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(DNS_PORT),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind :53 failed: %d", errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (xTaskCreate(dns_task, "dns_server", 3072, NULL, 4, &s_task) != pdPASS) {
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "captive DNS answering all A queries with %s", redirect_ip);
    return ESP_OK;
}

void dns_server_stop(void)
{
    int fd = s_sock;
    s_sock = -1;
    if (fd >= 0) {
        close(fd);
    }
    /* task notices s_sock < 0 within its 1s recv timeout and exits */
}
