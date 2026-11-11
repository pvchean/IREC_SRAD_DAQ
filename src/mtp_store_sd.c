/* SD card backend of the MTP files mode. Read only. Docs/MTP/Storage_Design.md MTP-S-5.
   It mounts the card itself (SDMMC slot 1, 1-bit, as sd_log.c does) and never formats it. The files open with fopen(..., "rb"),
   which is f_open with FA_READ only. Files mode never runs sd_log_start(), so nothing else writes to the card.
   With no card, present() is false and the other calls fail safely. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "board.h"
#include "io.h"
#include "mtp_store.h"

#define BASE        "/sdcard"
#define MAX_FILES   256U
#define RETRY_US    2000000LL         /* time between two mount tries when no card is in */
#define NAME_MAX_SD 16U               /* 8.3 name; a longer name is not shown */

static const char *TAG = "mtp_sd";

typedef struct { char name[NAME_MAX_SD]; uint32_t size; uint32_t mtime; } ent_t;

static bool         s_init;
static sdmmc_card_t *s_card;
static ent_t        s_ent[MAX_FILES];
static uint32_t     s_n;
static int64_t      s_last_try_us;
static FILE        *s_f;              /* the open file (one at a time) */
static int32_t      s_f_idx = -1;

static void close_file(void)
{
    if (s_f) { fclose(s_f); s_f = NULL; }
    s_f_idx = -1;
}

static void drop_card(void)
{
    close_file();
    if (s_card) { esp_vfs_fat_sdcard_unmount(BASE, s_card); s_card = NULL; }
    s_n = 0;
}

static bool mount_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_mount_config_t mc = VFS_FAT_MOUNT_DEFAULT_CONFIG();      /* format_if_mount_failed = false */

    slot.width = 1;
    slot.clk = SDMMC_CLK_PIN;
    slot.cmd = SDMMC_CMD_PIN;
    slot.d0  = SDMMC_DAT0_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    mc.max_files = 2;
    mc.format_if_mount_failed = false;                                   /* never format */
    return esp_vfs_fat_sdmmc_mount(BASE, &host, &slot, &mc, &s_card) == ESP_OK;
}

static bool sd_present(void)
{
    if (!s_init) { return false; }
    if (s_card) {
        if (sdmmc_get_status(s_card) == ESP_OK) { return true; }         /* CMD13: the card still answers */
        ESP_LOGW(TAG, "card gone");
        drop_card();
    }
    int64_t now = esp_timer_get_time();
    if (s_last_try_us != 0 && now - s_last_try_us < RETRY_US) { return false; }
    s_last_try_us = now;
    if (mount_card()) { ESP_LOGI(TAG, "card mounted (read only use)"); return true; }
    s_card = NULL;
    return false;
}

static int sd_refresh(void)
{
    close_file();
    s_n = 0;
    if (!sd_present()) { return -1; }
    DIR *d = opendir(BASE);
    if (!d) { drop_card(); return -1; }
    struct dirent *e;
    while (s_n < MAX_FILES && (e = readdir(d)) != NULL) {
        struct stat sb;
        char p[sizeof BASE + 1 + 300];
        if (strlen(e->d_name) >= NAME_MAX_SD) { continue; }
        snprintf(p, sizeof p, BASE "/%s", e->d_name);
        if (stat(p, &sb) != 0 || !S_ISREG(sb.st_mode)) { continue; }     /* folders such as "System Volume Information" are skipped */
        ent_t *t = &s_ent[s_n++];
        strcpy(t->name, e->d_name);
        t->size  = (uint32_t)sb.st_size;
        t->mtime = sb.st_mtime > 0 ? (uint32_t)sb.st_mtime : 0U;
    }
    closedir(d);
    return (int)s_n;
}

static uint32_t sd_count(void) { return s_n; }

static bool sd_info(uint32_t i, mtp_file_info_t *out)
{
    if (i >= s_n || out == NULL) { return false; }
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "uSD_%s", s_ent[i].name);     /* the names of tools/daq_get.py */
    out->size = s_ent[i].size;
    out->mtime = s_ent[i].mtime;
    out->size_exact = true;
    return true;
}

static int32_t sd_read(uint32_t i, uint64_t offset, uint8_t *buf, size_t len)
{
    if (!s_card || i >= s_n || buf == NULL) { return -1; }
    if (offset >= s_ent[i].size) { return 0; }
    if (len > s_ent[i].size - offset) { len = (size_t)(s_ent[i].size - offset); }
    if (len > 0x7FFFFFFFU) { len = 0x7FFFFFFFU; }
    if (s_f_idx != (int32_t)i) {
        char p[sizeof BASE + 1 + NAME_MAX_SD];
        close_file();
        snprintf(p, sizeof p, BASE "/%s", s_ent[i].name);
        s_f = fopen(p, "rb");                                           /* FA_READ only */
        if (!s_f) { drop_card(); return -1; }                            /* the card went away: mount again at the next present() */
        s_f_idx = (int32_t)i;
    }
    if (fseek(s_f, (long)offset, SEEK_SET) != 0) { return -1; }
    size_t n = fread(buf, 1, len, s_f);
    if (n < len && ferror(s_f)) { close_file(); return -1; }
    return (int32_t)n;
}

static uint64_t sd_capacity(void)
{
    return s_card ? (uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size : 0U;
}

static const mtp_store_t s_store = { 2U, "SD", sd_present, sd_refresh, sd_count, sd_info, sd_read, sd_capacity };

bool mtp_store_sd_init(void)
{
    if (!io_usb_files_mode()) { return false; }                          /* serial mode: do nothing */
    s_init = true;
    return true;
}

const mtp_store_t *mtp_store_sd_get(void) { return s_init ? &s_store : NULL; }
