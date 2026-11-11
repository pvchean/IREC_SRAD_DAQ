#pragma once
#include "esp_log.h"
#define ESP_RETURN_ON_FALSE(a, err, tag, fmt, ...) do { if (!(a)) { ESP_LOGE(tag, fmt, ##__VA_ARGS__); return (err); } } while (0)
#define ESP_GOTO_ON_FALSE(a, err, goto_tag, tag, fmt, ...) do { if (!(a)) { ESP_LOGE(tag, fmt, ##__VA_ARGS__); ret = (err); goto goto_tag; } } while (0)
