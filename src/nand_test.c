/* Read-only test of the NAND SPI clock. It starts the NAND driver (no file system, no mount) and reads each page of
   the chip, PASSES times. It never programs or erases. For each block it makes a CRC-32 over all main bytes, the user
   OOB bytes, and the ECC status of its pages. The same chip content must give the same digests at 40 MHz and at 80 MHz.
   Commands: none. The result goes to the USB serial port. */
#ifdef DAQ_NANDREAD
#include "nand_test.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_private/spi_common_internal.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pfs_esp32s3.h"
#include "pfs_zd35q1gc.h"

#include "board.h"

#define PASSES 3

static uint8_t s_main[NAND_PAGE_SIZE], s_oob[NAND_USER_OOB_SIZE];
static uint32_t s_blk[NAND_BLOCKS_PER_DRIVE];

void nand_read_test(void)
{
    const pfs_os_hal_t *os = pfs_os_esp32s3_get();
    spi_bus_esp32s3_cfg_t cfg = {
        .mosi = NAND_MOSI_PIN, .miso = NAND_MISO_PIN, .sclk = NAND_CLK_PIN, .cs = NAND_CS_PIN,
        .wp = NAND_WP_PIN, .hold = NAND_HOLD_PIN, .clock_hz = NAND_SPI_HZ, .log = os->log
    };
    const nand_hal_t *nand = nand_zd35q1gc_bind(spi_bus_esp32s3_get(&cfg));
    uint32_t first_digest = 0;

    vTaskDelay(pdMS_TO_TICKS(3000));               /* the host opens the serial port */
    nand_status_t st = nand->init();
    const spi_bus_attr_t *a = spi_bus_get_attr(SPI2_HOST);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"     /* the device handle is inside the HAL */
    int actual = spi_get_actual_clock(80000000, (int)NAND_SPI_HZ, 128);
#pragma GCC diagnostic pop
    printf("NANDREAD: init=%d requested_hz=%u actual_hz=%d iomux_pins=%d\n", (int)st, (unsigned)NAND_SPI_HZ, actual,
           a ? ((a->flags & SPICOMMON_BUSFLAG_IOMUX_PINS) != 0) : -1);
    if (st != NAND_OK) { for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); } }

    for (int pass = 1; pass <= PASSES; pass++) {
        uint32_t errors = 0, ecc[4] = { 0, 0, 0, 0 };
        int64_t t0 = esp_timer_get_time();
        for (uint16_t b = 0; b < NAND_BLOCKS_PER_DRIVE; b++) {
            uint32_t crc = 0;
            for (uint16_t p = 0; p < NAND_PAGES_PER_BLOCK; p++) {
                nand_ecc_t e = NAND_ECC_OK;
                uint8_t eb;
                nand_status_t r = nand->read_page(b, p, s_main, s_oob, &e);
                if (r != NAND_OK) { if (errors++ < 5) { printf("NANDREAD: error %d at block %u page %u\n", (int)r, b, p); } }
                eb = (uint8_t)((r != NAND_OK) ? 0x80U | (uint8_t)r : (uint8_t)e);
                if ((unsigned)e < 4U) { ecc[e]++; }
                crc = esp_rom_crc32_le(crc, s_main, sizeof s_main);
                crc = esp_rom_crc32_le(crc, s_oob, sizeof s_oob);
                crc = esp_rom_crc32_le(crc, &eb, 1);
            }
            s_blk[b] = crc;
            if ((b % 64U) == 63U) { vTaskDelay(1); }       /* the watchdog */
        }
        int64_t us = esp_timer_get_time() - t0;
        uint32_t digest = esp_rom_crc32_le(0, (const uint8_t *)s_blk, sizeof s_blk);
        if (pass == 1) { first_digest = digest; }
        printf("NANDREAD: pass=%d pages=%u read_errors=%" PRIu32 " ecc_ok=%" PRIu32 " ecc_corrected=%" PRIu32
               " ecc_no_margin=%" PRIu32 " ecc_uncorrectable=%" PRIu32 " digest=%08" PRIX32 " same_as_pass1=%d time_ms=%" PRId64
               " us_per_page=%.1f\n", pass, (unsigned)(NAND_BLOCKS_PER_DRIVE * NAND_PAGES_PER_BLOCK), errors, ecc[0], ecc[1],
               ecc[2], ecc[3], digest, digest == first_digest, us / 1000, (double)us / (NAND_BLOCKS_PER_DRIVE * NAND_PAGES_PER_BLOCK));
        if (pass == 1) {
            for (unsigned b = 0; b < NAND_BLOCKS_PER_DRIVE; b += 16) {
                printf("BLK %04u:", b);
                for (unsigned i = 0; i < 16; i++) { printf(" %08" PRIX32, s_blk[b + i]); }
                printf("\n");
            }
        }
    }
    printf("NANDREAD: done\n");
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
#endif
