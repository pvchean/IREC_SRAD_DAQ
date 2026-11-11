#pragma once
#include "freertos/FreeRTOS.h"
typedef struct host_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, unsigned prio, TaskHandle_t *out);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
