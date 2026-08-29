#pragma once
#include <pthread.h>
typedef pthread_t TaskHandle_t;
static inline int xTaskCreate(void(*fn)(void*), const char*n, unsigned s,
                              void*a, unsigned p, TaskHandle_t*h){
  (void)n;(void)s;(void)p; pthread_t t;
  if(pthread_create(&t,0,(void*(*)(void*))fn,a)) return 0;
  pthread_detach(t); if(h)*h=t; return 1; }
static inline void vTaskDelete(void*x){ (void)x; pthread_exit(0); }
