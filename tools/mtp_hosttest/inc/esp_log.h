#pragma once
#include <inttypes.h>
void host_log(int level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) host_log(0, tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(1, tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(2, tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) host_log(3, tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) host_log(4, tag, __VA_ARGS__)
