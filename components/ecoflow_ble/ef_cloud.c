#include "ef_cloud.h"
#include "ecoflow_ble.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static const char *TAG = "ef_cloud";

#define RESP_MAX 4096

static const char *resolve_host(const char *region)
{
    if (!region || !region[0] || strcmp(region, "auto") == 0) {
        return "api.ecoflow.com";
    }
    static char host[32];
    snprintf(host, sizeof(host), "%s.ecoflow.com", region);
    return host;
}

int ef_cloud_login(const char *identifier, const char *password,
                   const char *region,
                   char *user_id_out, size_t user_id_sz,
                   char *err, size_t err_sz)
{
#define FAILF(...) do { snprintf(err, err_sz, __VA_ARGS__); return -1; } while (0)

    if (!identifier || !identifier[0] || !password || !password[0]) {
        FAILF("email and password required");
    }

    unsigned char pw_b64[256];
    size_t pw_b64_len = 0;
    if (mbedtls_base64_encode(pw_b64, sizeof(pw_b64), &pw_b64_len,
                              (const unsigned char *)password,
                              strlen(password)) != 0) {
        FAILF("password too long");
    }
    pw_b64[pw_b64_len] = '\0';

    bool is_phone = (identifier[0] == '+') ||
                    (strspn(identifier, "0123456789") == strlen(identifier));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "scene", "IOT_APP");
    cJSON_AddStringToObject(root, "appVersion", "1.0.0");
    cJSON_AddStringToObject(root, "password", (const char *)pw_b64);
    cJSON *oauth = cJSON_AddObjectToObject(root, "oauth");
    cJSON_AddStringToObject(oauth, "bundleId", "com.ef.EcoFlow");
    cJSON_AddStringToObject(root, "userType", "ECOFLOW");
    cJSON_AddStringToObject(root, is_phone ? "phone" : "email", identifier);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        FAILF("json build failed");
    }

    char url[96];
    snprintf(url, sizeof(url), "https://%s/auth/login", resolve_host(region));

    char *resp = malloc(RESP_MAX);
    if (!resp) {
        free(body);
        FAILF("out of memory");
    }
    resp[0] = '\0';
    size_t resp_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1536,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(body);
        free(resp);
        FAILF("http init failed");
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Accept", "application/json");

    int rc = esp_http_client_open(cli, strlen(body));
    if (rc != ESP_OK) {
        esp_http_client_cleanup(cli);
        free(body);
        free(resp);
        FAILF("connect failed: %s", esp_err_to_name(rc));
    }
    esp_http_client_write(cli, body, strlen(body));
    free(body);

    esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);

    for (;;) {
        int n = esp_http_client_read(cli, resp + resp_len, RESP_MAX - 1 - resp_len);
        if (n <= 0) {
            break;
        }
        resp_len += n;
        if (resp_len >= RESP_MAX - 1) {
            break;
        }
    }
    resp[resp_len] = '\0';
    esp_http_client_cleanup(cli);

    if (status != 200) {
        int r = snprintf(err, err_sz, "login HTTP %d", status);
        (void)r;
        free(resp);
        return -1;
    }

    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (!j) {
        FAILF("bad JSON from server");
    }

    int ret = -1;
    cJSON *code = cJSON_GetObjectItem(j, "code");
    const char *code_s = cJSON_IsString(code) ? code->valuestring : NULL;
    if (!code_s || strcmp(code_s, "0") != 0) {
        cJSON *msg = cJSON_GetObjectItem(j, "message");
        snprintf(err, err_sz, "login rejected: %s",
                 cJSON_IsString(msg) ? msg->valuestring : (code_s ? code_s : "?"));
    } else {
        cJSON *data = cJSON_GetObjectItem(j, "data");
        cJSON *user = data ? cJSON_GetObjectItem(data, "user") : NULL;
        cJSON *uid = user ? cJSON_GetObjectItem(user, "userId") : NULL;
        if (cJSON_IsString(uid) && uid->valuestring[0]) {
            strlcpy(user_id_out, uid->valuestring, user_id_sz);
            ESP_LOGI(TAG, "resolved user_id (%d chars)", (int)strlen(user_id_out));
            ret = 0;
        } else {
            snprintf(err, err_sz, "no userId in response");
        }
    }
    cJSON_Delete(j);
    return ret;
#undef FAILF
}

int ecoflow_resolve_user_id(const char *identifier, const char *password,
                            const char *region,
                            char *out, size_t out_sz,
                            char *err, size_t err_sz)
{
    return ef_cloud_login(identifier, password, region, out, out_sz, err, err_sz);
}
