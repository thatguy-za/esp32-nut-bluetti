#pragma once
#include "esp_err.h"

/* Tiny captive-portal DNS server: answers every A query with the given
 * IPv4 address (dotted-quad, e.g. "192.168.4.1"). Spawns one task. */
esp_err_t dns_server_start(const char *redirect_ip);
void      dns_server_stop(void);
