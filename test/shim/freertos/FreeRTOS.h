#pragma once
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#define portMAX_DELAY 0xffffffffUL
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
typedef unsigned TickType_t;
typedef int BaseType_t;
static inline void vTaskDelay(uint32_t t){ usleep(t*1000); }
