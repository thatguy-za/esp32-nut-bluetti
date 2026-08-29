#pragma once
/*
 * One-shot EcoFlow account login to resolve the account `user_id` that
 * the BLE auth handshake needs. Runs over HTTPS; the password is used
 * only for this call and never stored.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* region: "api", "api-e", "api-a", "api-j", "api-r", "api-cn", or ""/"auto"
 * (auto -> "api"). Returns 0 on success and writes the user id to
 * user_id_out; on failure returns non-zero and writes a message to err. */
int ef_cloud_login(const char *identifier, const char *password,
                   const char *region,
                   char *user_id_out, size_t user_id_sz,
                   char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif
