#pragma once
#include <stdio.h>
/* args referenced so `static const char *TAG` doesn't warn as unused */
#define ESP_LOGE(t,...) do{ (void)(t); printf("[E] "); printf(__VA_ARGS__); printf("\n"); }while(0)
#define ESP_LOGW(t,...) do{ (void)(t); printf("[W] "); printf(__VA_ARGS__); printf("\n"); }while(0)
#define ESP_LOGI(t,...) do{ (void)(t); }while(0)
#define ESP_LOGD(t,...) do{ (void)(t); }while(0)
#define ESP_LOGV(t,...) do{ (void)(t); }while(0)
