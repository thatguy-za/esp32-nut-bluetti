#pragma once
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){
  pthread_mutex_t *m=malloc(sizeof *m); pthread_mutex_init(m,0); return m; }
#define xSemaphoreTake(m,t) (pthread_mutex_lock(m),1)
#define xSemaphoreGive(m)   (pthread_mutex_unlock(m),1)
