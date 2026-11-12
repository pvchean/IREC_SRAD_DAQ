#include "nor_ro.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PATH_SIZE 40U                             /* base_path (15) + "/" + name (15) + 0 */

static pfs_nor_littlefs_cfg_t s_cfg;
static bool                   s_mounted;
static StaticSemaphore_t      s_lock_buf;
static SemaphoreHandle_t      s_lock;
static volatile uint32_t      s_refused;

static bool name_ok(const char *name)
{
    size_t n;
    if (name == NULL) { return false; }
    for (n = 0U; (n <= PFS_NOR_NAME_MAX) && (name[n] != '\0'); n++) {
        char c = name[n];
        if (!(((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_'))) { return false; }
    }
    return (n >= 1U) && (n <= PFS_NOR_NAME_MAX) && (name[n] == '\0');
}

/* Read only, no format. A store that does not mount gives PFS_HAL_ERR_IO: PFS then uses Path B (a scan of the NAND). */
static pfs_hal_status_t ro_mount(void)
{
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_mounted) {
        esp_vfs_littlefs_conf_t conf;
        memset(&conf, 0, sizeof conf);
        conf.base_path = s_cfg.base_path;
        conf.partition_label = s_cfg.partition_label;
        conf.format_if_mount_failed = 0U;
        conf.read_only = 1U;
        if (esp_vfs_littlefs_register(&conf) == ESP_OK) { s_mounted = true; }
    }
    (void)xSemaphoreGive(s_lock);
    return s_mounted ? PFS_HAL_OK : PFS_HAL_ERR_IO;
}

static pfs_hal_status_t ro_read(const char *name, uint8_t *dst, size_t max_len, size_t *len_out)
{
    char path[PATH_SIZE];
    pfs_hal_status_t st = PFS_HAL_OK;
    struct stat sb;
    FILE *fp;
    int n;

    if (!name_ok(name) || (dst == NULL) || (len_out == NULL)) { return PFS_HAL_ERR_ARG; }
    n = snprintf(path, sizeof path, "%s/%s", s_cfg.base_path, name);
    if (n <= 0 || (size_t)n >= sizeof path) { return PFS_HAL_ERR_ARG; }
    if (!s_mounted) { return PFS_HAL_ERR_IO; }

    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    if (stat(path, &sb) != 0) {
        st = (errno == ENOENT) ? PFS_HAL_ERR_NOT_FOUND : PFS_HAL_ERR_IO;
    } else if ((sb.st_size < 0) || ((size_t)sb.st_size > max_len)) {
        st = PFS_HAL_ERR_NO_MEM;
    } else {
        fp = fopen(path, "rb");
        if (fp == NULL) {
            st = (errno == ENOENT) ? PFS_HAL_ERR_NOT_FOUND : PFS_HAL_ERR_IO;
        } else {
            size_t size = (size_t)sb.st_size;
            if (fread(dst, 1U, size, fp) != size) { st = PFS_HAL_ERR_IO; } else { *len_out = size; }
            if (fclose(fp) != 0) { st = PFS_HAL_ERR_IO; }
        }
    }
    (void)xSemaphoreGive(s_lock);
    return st;
}

static pfs_hal_status_t ro_write(const char *name, const uint8_t *src, size_t len)
{
    (void)name; (void)src; (void)len;
    s_refused++;
    return PFS_HAL_ERR_IO;
}

static const pfs_nor_hal_t s_hal = { ro_mount, ro_read, ro_write };

const pfs_nor_hal_t *nor_ro_littlefs_get(const pfs_nor_littlefs_cfg_t *cfg)
{
    if ((cfg == NULL) || (cfg->partition_label == NULL) || (cfg->base_path == NULL) ||
        (strlen(cfg->base_path) < 1U) || (strlen(cfg->base_path) > PFS_NOR_LITTLEFS_PATH_MAX)) {
        return NULL;
    }
    s_cfg = *cfg;
    if (s_lock == NULL) { s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf); }
    return (s_lock != NULL) ? &s_hal : NULL;
}

uint32_t nor_ro_refused_writes(void) { return s_refused; }
