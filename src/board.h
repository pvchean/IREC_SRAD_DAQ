/* Pins of the DAQ board (from DAQ_Test_Sketch/Global.h). */
#ifndef BOARD_H
#define BOARD_H

#define I2C0_SDA_PIN   8
#define I2C0_SCL_PIN   18
#define I2C0_RDY_PIN   17      /* ADS1115 ALERT/RDY, falling edge = new sample */

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
