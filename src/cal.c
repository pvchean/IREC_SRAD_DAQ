#include "cal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define STORE_MAGIC  0xCA11B001U
#define NVS_NS       "daq"
#define NVS_KEY      "cal"

typedef struct { uint32_t magic; cal_t cal; } store_t;

static const char *TAG = "cal";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;     /* a short lock: a copy of 20 bytes */
static cal_t s_cal = { .a = { 1.0f, 1.0f }, .b = { 0.0f, 0.0f }, .rev = 0U, .flags = 0U };
static volatile bool s_request, s_flash_ok;

bool cal_flash_ok(void) { return s_flash_ok; }

void cal_get(cal_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_cal;
    portEXIT_CRITICAL(&s_mux);
}

void cal_request(void) { s_request = true; }

bool cal_take_request(void)
{
    bool r = s_request;
    if (r) { s_request = false; }
    return r;
}

static bool store_write(const cal_t *c)
{
    nvs_handle_t h;
    store_t s = { STORE_MAGIC, *c };
    bool ok = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, NVS_KEY, &s, sizeof s) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    return ok;
}

void cal_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();                   /* the partition of the DAQ: nothing else uses it */
        e = nvs_flash_init();
    }
    s_flash_ok = (e == ESP_OK);
    if (!s_flash_ok) { ESP_LOGW(TAG, "NVS does not start (%d): the calibration is not kept over a reset", (int)e); return; }

    nvs_handle_t h;
    store_t s;
    size_t len = sizeof s;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY, &s, &len) == ESP_OK && len == sizeof s && s.magic == STORE_MAGIC) {
            portENTER_CRITICAL(&s_mux);
            s_cal = s.cal;
            portEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "calibration rev %u loaded", (unsigned)s.cal.rev);
        }
        nvs_close(h);
    }
}

bool cal_set(int sensor, float a, float b, cal_t *out)
{
    cal_t c;
    portENTER_CRITICAL(&s_mux);
    s_cal.a[sensor] = a;
    s_cal.b[sensor] = b;
    s_cal.flags |= (sensor == CAL_PT) ? CAL_FLAG_PT : CAL_FLAG_LC;
    s_cal.rev = (s_cal.rev + 1U) & 0xFFFFFFU;
    c = s_cal;
    portEXIT_CRITICAL(&s_mux);
    if (out) { *out = c; }
    s_request = true;                              /* the block goes into the data, also if the flash write fails */
    return s_flash_ok && store_write(&c);          /* the flash write can take some ms: no lock held */
}

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

void cal_make_block(record_t out[CAL_BLOCK_RECS])
{
    cal_t c;
    uint32_t pay[5];
    cal_get(&c);
    pay[0] = ((uint32_t)c.flags << 24) | (c.rev & 0xFFFFFFU);
    pay[1] = f2u(c.a[CAL_PT]); pay[2] = f2u(c.b[CAL_PT]);
    pay[3] = f2u(c.a[CAL_LC]); pay[4] = f2u(c.b[CAL_LC]);
    for (unsigned i = 0; i < 5U; i++) { out[i] = rec_make_meta(i + 1U, pay[i]); }
    out[5] = rec_make_meta(6U, esp_rom_crc32_le(0, (const uint8_t *)pay, sizeof pay));
}

bool cal_parse(cal_parser_t *p, const record_t *r, cal_t *out)
{
    uint32_t kind;
    if (!rec_is_meta(r)) { p->n = 0; return false; }
    kind = r->timestamp_us - REC_META_BASE;
    if (kind == 1U) { p->n = 0; }                                   /* a new block starts */
    if (kind != p->n + 1U) { p->n = 0; return false; }              /* out of order */
    p->r[p->n++] = *r;
    if (p->n < CAL_BLOCK_RECS) { return false; }
    p->n = 0;

    uint32_t pay[5];
    for (unsigned i = 0; i < 5U; i++) { pay[i] = rec_meta_payload(&p->r[i]); }
    if (rec_meta_payload(&p->r[5]) != esp_rom_crc32_le(0, (const uint8_t *)pay, sizeof pay)) { return false; }
    if (out) {
        out->flags = (uint8_t)(pay[0] >> 24);
        out->rev = pay[0] & 0xFFFFFFU;
        out->a[CAL_PT] = u2f(pay[1]); out->b[CAL_PT] = u2f(pay[2]);
        out->a[CAL_LC] = u2f(pay[3]); out->b[CAL_LC] = u2f(pay[4]);
    }
    return true;
}
