#include "files_mode.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "io.h"

#ifdef DAQ_MTP
#include "soc/rtc_cntl_reg.h"
#ifdef DAQ_MTP_USB
#include "usb_files.h"
#endif
#endif

static const char *TAG = "files";
static volatile bool s_active;

bool files_mode_active(void)         { return s_active; }
void files_mode_set_active(bool on)  { s_active = on; }

__attribute__((weak)) void files_mode_usb_start(void)
{
    ESP_LOGW(TAG, "files mode: no USB stack yet");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

#ifdef DAQ_MTP
void files_mode_restart(files_mode_restart_t how)
{
    static volatile int s_busy;
    if (__atomic_exchange_n(&s_busy, 1, __ATOMIC_SEQ_CST)) { for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); } }   /* another task restarts the chip */
    if (how == FM_LEAVE) { io_request_serial_once(); }
    ESP_LOGW(TAG, "restart from files mode: %s", how == FM_REBOOT ? "files mode again" : how == FM_DOWNLOAD ? "ROM download mode" : "serial mode");
    vTaskDelay(pdMS_TO_TICKS(300));                         /* let the last CDC reply go out */
#ifdef DAQ_MTP_USB
    usb_files_prepare_restart();                            /* tud_disconnect + 500 ms, PHY to USB Serial/JTAG, pad repair */
#endif
    if (how == FM_DOWNLOAD) { REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT); }
    esp_restart();
    for (;;) { }
}

void files_mode_leave(void) { files_mode_restart(FM_LEAVE); }
#else
void files_mode_restart(files_mode_restart_t how)
{
    (void)how;
    esp_restart();
    for (;;) { }
}

void files_mode_leave(void)
{
    io_request_serial_once();
    ESP_LOGW(TAG, "leaving files mode: restart in serial mode");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    for (;;) { }
}
#endif
