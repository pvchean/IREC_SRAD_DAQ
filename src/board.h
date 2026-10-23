/* Pins of the DAQ board (from DAQ_Test_Sketch/Global.h). */
#ifndef BOARD_H
#define BOARD_H

#define I2C0_SDA_PIN   8
#define I2C0_SCL_PIN   18
#define I2C0_RDY_PIN   17      /* ADS1115 ALERT/RDY, falling edge = new sample */

#define SERIAL1_TX_PIN 42
#define SERIAL1_RX_PIN 41
#define SERIAL2_TX_PIN 40
#define SERIAL2_RX_PIN 39

#define NAND_MISO_PIN  13
#define NAND_MOSI_PIN  11
#define NAND_CLK_PIN   12
#define NAND_CS_PIN    10
/* GPIO 14 and 21 are SDMMC pins on this board, so the driver does not hold WP and HOLD. */
#define NAND_WP_PIN    (-1)
#define NAND_HOLD_PIN  (-1)


/* Mode and address pins: input with pull-up. A switch to ground or a floating pin sets the level. */
#define MODE1_PIN      1       /* high = auto record at power-up, low = manual. The tactile button also pulls it low. */
#define MODE2_PIN      2       /* high = serial, low = files (USB mode, not used yet) */
#define ADDR0_PIN      9
#define ADDR1_PIN      48
#define ADDR2_PIN      38

/* Daisy chain (UART1 and UART2, 8N1). Needed line rate = 2 (safety) x 10 bit/byte x bytes/s of the busiest
   link. A board sends 860 samples/s of the ADS1115 (8.6 byte per sample with the packet overhead, 7364 B/s)
   and later the load cell (9.6 byte per sample, 8223 B/s). The link next to an end board carries all other
   boards in one direction:  boards N | ADS only | ADS + load cell
                             2        | 147 k    | 312 k  -> 460800 baud
                             3        | 295 k    | 623 k  -> 921600 baud
                             4        | 442 k    | 935 k  -> 1000000 baud
   115200 baud carries only 11.5 KB/s: not enough for 2 x margin with one other board. */
#define CHAIN_BAUD     460800

/* Micro SD card, SDMMC slot 1, 1-bit mode. */
#define SDMMC_CLK_PIN   21
#define SDMMC_CMD_PIN   47
#define SDMMC_DAT0_PIN  14

/* LED shift register (from led_sr_driver.cpp). Output QA..QH = LED index 0..7. */
#define SR_LATCH_PIN   5
#define SR_CLOCK_PIN   4
#define SR_DATA_PIN    6

enum { LED_WORKING = 1, LED_RECORDING, LED_AUTOREC, LED_WIFI, LED_NAND, LED_SD, LED_ERROR };

#endif
