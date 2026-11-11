#include "files_mode.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "io.h"

static const char *TAG = "files";
static volatile bool s_active;

bool files_mode_active(void)         { return s_active; }
void files_mode_set_active(bool on)  { s_active = on; }

__attribute__((weak)) void files_mode_usb_start(void)
{
    ESP_LOGW(TAG, "files mode: no USB stack yet");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

void files_mode_leave(void)
{
    io_request_serial_once();
    ESP_LOGW(TAG, "leaving files mode: restart in serial mode");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    for (;;) { }
}
